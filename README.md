# STM32F103C8 网关与 OTA 合并工程

这一个目录包含 STM32 Bootloader、原 FreeRTOS 网关应用、ESP-01S 的 MQTT/OTA 固件和电脑端服务。

**下载阶段 App 继续 Modbus 采样、更新 OLED 和保存队列；完整固件通过校验后才重启安装。** 下载期间暂停遥测上传，取消下载或安装完成后继续补传。安装与回退阶段采样暂停。

## 目录与存储

| 路径 | 用途 |
| --- | --- |
| `stm32/app/GatewayOTA.ioc` | CubeMX 初始化配置|
| `stm32/app/MDK-ARM/GatewayOTA.uvprojx` | Keil 应用工程；从 `0x08006000` 链接，最大 40 KiB |
| `stm32/bootloader/MDK-ARM/GatewayBootloader.uvprojx` | Keil Bootloader 工程；占用前 24 KiB |
| `stm32/check-mx-keil.ps1` | CubeMX 重新生成后检查用户函数、Keil 源码清单和 RTOS 向量 |
| `stm32/build-bootloader.ps1`、`stm32/export-app.ps1` | 可选的 GCC 构建与导出工具 |
| `esp01s/GatewayOTA/` | 同一份 ESP 程序处理 MQTT 遥测/ACK 与按命令执行 OTA |
| `pc/` | Modbus 模拟、MQTT 落库、固件发布服务 |
| `tests/run-host.ps1` | 协议、队列、App 下载、Bootloader 恢复及 PC 逻辑的主机测试 |

W25Q32 的 `/CS` 接 **PA4**；SPI1 仍用 PA5/PA6/PA7。

STM32 内部 Flash：`0x08000000–0x08005FFF` 为 Bootloader，`0x08006000–0x0800FFFF` 为应用。W25Q32 的 `0x000000–0x1FFFFF` 保留原网关 512 个 4 KiB 队列扇区；`0x200000`、`0x201000` 为两份 OTA 元数据；`0x210000–0x219FFF` 为候选镜像；`0x220000–0x229FFF` 为旧应用备份。升级不擦除队列。

## 首次构建与烧录

1. 按 [接线表](docs/01_接线.md) 接好 STM32、W25Q32、ESP-01S、OLED 和 RS485。确认 STM32 为 64 KiB Flash、20 KiB RAM，外部 W25Q32 为 4 MiB。
2. 在 STM32CubeMX 打开 `stm32/app/GatewayOTA.ioc`，配置初始化并 **Generate Code**。工程已设置为 MDK-ARM V5、`Keep User Code=true`、生成前备份。业务函数写在 `USER CODE BEGIN/END` 区域或 `User/` 独立文件；在 VS Code 编辑时不要把新函数写进生成区。每次生成后运行 `./stm32/check-mx-keil.ps1`；若检查失败，先恢复缺失的用户代码或 Keil 文件条目，再编译。
3. 在 Keil 打开 `stm32/bootloader/MDK-ARM/GatewayBootloader.uvprojx` 并构建，得到 `stm32/bootloader/MDK-ARM/Bootloader/bootloader.bin`。再打开 `stm32/app/MDK-ARM/GatewayOTA.uvprojx` 并构建，得到 `stm32/app/MDK-ARM/GatewayOTA/GatewayOTA.bin`。Keil 工程已设置构建后用 FromELF 导出 BIN。
4. 检查 Bootloader BIN 不超过 24576 字节，应用 BIN 不超过 40960 字节。Keil 的应用 IROM 起点为 `0x08006000`、长度 40 KiB，IRAM 长度 20 KiB；仍须在实板观察任务栈余量。
5. 用 ST-Link 将 Keil Bootloader BIN 烧至 `0x08000000`，Keil 应用 BIN 烧至 `0x08006000`，读回核对两个地址的向量表。若烧录工具执行整片擦除，之后需重新烧入两份程序。不要把应用 BIN 烧到 `0x08000000`。
6. 将 `esp01s/config.example.h` 的字段填入 `esp01s/GatewayOTA/config.h`，尤其是电脑局域网 IP 对应的 `MQTT_HOST` 和 `OTA_MANIFEST_URL`。Arduino IDE 选择 ESP8266 的 Generic ESP8266 Module，烧入唯一的 `GatewayOTA.ino`。

## 运行网关与升级

电脑端按原网关流程运行 Mosquitto、`pc/modbus_slave.py` 和 `pc/collector.py`；用 MQTTX 订阅 `gateway/<DEVICE_ID>/#`。OLED 应显示两个 Modbus 从站数值和 VET6 PB12 电平，SQLite 收到经 ACK 确认的三路记录。C8 当前 App 是 V10204；VET6 可独立烧录。旧 V2 队列记录保持可读。`gateway/<DEVICE_ID>/cmd` 的 `POLL:10000` 仍可调整轮询间隔。

以后自行发布新版 App：

1. 在 Keil 的 **Options for Target → C/C++ → Define** 把 `GW_APP_VERSION=10204` 改成更大的版本号，例如 `10205`。修改业务代码后点 **Rebuild**，确认 `0 Error(s)`；Keil 会自动导出 `GatewayOTA.bin`。不要用 Keil 下载这次 BIN，否则无法验证 OTA。
2. 双击 `pc/Start_OTA.cmd`。它从 Keil 工程读版本号、检查 BIN 比源码新，并按 ESP `config.h` 的下载地址发布镜像。窗口显示 `Serving` 后保持开启。若端口已有本项目的 OTA 服务，它会核对现有服务提供的是新清单；此时保持原服务运行。
3. 用浏览器打开窗口给出的 `manifest.txt` 地址，确认 `VERSION` 是新版本。MQTTX 连接本机 Broker，订阅 `gateway/gw-01/status`；向 `gateway/gw-01/cmd` 发送纯文本 `OTA`，关闭 Retain。
4. 收到 `ota-transfer-complete` 后，以 OLED 版本号、两路 Modbus、CAN 0/1、`FLASH:OK` 及 SQLite 新序号确认安装与采样恢复。升级时电脑、ESP、C8、W25 和 OTA 服务均保持供电/运行。

命令行发布方式仍可使用：

```powershell
python pc/release_server.py publish stm32/app/MDK-ARM/GatewayOTA/GatewayOTA.bin --version <Keil中的新版本号> --origin http://<电脑局域网IP>:8765
python pc/release_server.py serve --host 0.0.0.0 --port 8765
```

把 ESP 配置中的 `OTA_MANIFEST_URL` 设成同一个地址的 `/manifest.txt`；向 `gateway/<DEVICE_ID>/cmd` 发布文本 `OTA`。ESP 在当前应用上询问版本，下载清单和 BIN，通过 USART1 直接把固件分块交给仍在运行的 App。App 写入 W25 候选区、完整读回校验后保存 `STAGED` 并复位；Bootloader 备份旧版、安装新版；新应用完成一次轮询后写入确认。下载期间队列仍在 W25Q32 前 2 MiB，采样继续入队；ESP 暂停遥测转发，之后恢复。`ota-transfer-complete` 只代表 Bootloader 已报告安装，最终结果以重启后的 `VERSION`、OLED、MQTT 和 SQLite 验证为准。

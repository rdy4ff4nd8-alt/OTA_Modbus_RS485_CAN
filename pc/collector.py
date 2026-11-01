"""Persist telemetry in SQLite, then acknowledge its sequence through MQTT.

Run this alongside Mosquitto. MQTT delivery alone is not proof of disk commit;
the STM32 retires a W25Q32 record only after this service commits it.
"""
import argparse
import json
import sqlite3
import time

FIELDS = ("slave1_value", "slave2_value", "can_value", "status", "uptime_ms")


def validate(payload, device="gw-01"):
    # MQTT payload 是 JSON 字节串；先解析，再确认设备 ID、序号和采样字段。
    data = json.loads(payload)
    if data.get("device") != device:
        raise ValueError("unexpected device")
    # 序号需落在 STM32 使用的无符号 32 位范围内。
    if not isinstance(data.get("seq"), int) or not 0 < data["seq"] < 2**32:
        raise ValueError("invalid sequence")
    # V2 消息没有 CAN 字段，入库时用 0 填充。
    data.setdefault("can_value", 0)
    if any(not isinstance(data.get(key), int) for key in FIELDS):
        raise ValueError("invalid sample")
    return data


def save(db, data):
    # with db 在退出时提交事务，异常时回滚。
    with db:
        # (device, seq) 是主键；重发的同一条采样会被忽略，不会重复入库。
        db.execute(
            "INSERT OR IGNORE INTO samples "
            "(device, seq, received_at, slave1_value, slave2_value, can_value, status, uptime_ms) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            (data["device"], data["seq"], int(time.time()), *(data[k] for k in FIELDS)),
        )


def ensure_schema(db):
    db.execute(
        "CREATE TABLE IF NOT EXISTS samples (device TEXT NOT NULL, seq INTEGER NOT NULL, "
        "received_at INTEGER NOT NULL, slave1_value INTEGER NOT NULL, "
        "slave2_value INTEGER NOT NULL, can_value INTEGER NOT NULL DEFAULT 0, "
        "status INTEGER NOT NULL, uptime_ms INTEGER NOT NULL, "
        "PRIMARY KEY (device, seq))"
    )
    # 表缺少 CAN 列时就地加列，原有采样和序号保留。
    if "can_value" not in {row[1] for row in db.execute("PRAGMA table_info(samples)")}:
        db.execute("ALTER TABLE samples ADD COLUMN can_value INTEGER NOT NULL DEFAULT 0")


def main():
    # 只有实际运行采集器才需要 paho；导入本文件做单元测试无需连接 MQTT。
    import paho.mqtt.client as mqtt
    # 这些参数指定 Broker、SQLite 文件和本采集器负责的设备。
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=1883)
    parser.add_argument("--db", default="modbus_gateway.sqlite3")
    parser.add_argument("--device", default="gw-01")
    args = parser.parse_args()
    db = sqlite3.connect(args.db)
    # 一台设备的同一个序号在表中只能出现一次。
    ensure_schema(db)

    def on_connect(client, userdata, flags, reason_code, properties):
        # 连接成功才订阅该设备的遥测主题，QoS 1 允许消息重发。
        if reason_code == 0:
            client.subscribe(f"gateway/{args.device}/telemetry", qos=1)
            print("Connected; waiting for telemetry", flush=True)

    def on_message(client, userdata, message):
        try:
            data = validate(message.payload, args.device)
            save(db, data)
            # SQLite 事务已提交，才向 ESP 发送该序号的 ACK；STM32 随后可以出队。
            client.publish(f"gateway/{args.device}/ack", str(data["seq"]), qos=1)
            print("stored/ack", data["seq"], flush=True)
        # 坏包或数据库错误时不发 ACK，让设备保留并重试。
        except (ValueError, json.JSONDecodeError, sqlite3.Error) as exc:
            print("rejected:", exc, flush=True)

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="gw-collector")
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect_async(args.host, args.port, keepalive=30)
    try:
        # 在这里持续处理 MQTT 收发及重连，退出时关闭数据库。
        client.loop_forever(retry_first_connection=True)
    finally:
        db.close()


if __name__ == "__main__":
    # 直接运行本文件时启动采集器；作为模块导入时不自动连接 MQTT。
    main()

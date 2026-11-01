#include "modbus_master.h"
#include "gateway_protocol.h"
#include "main.h"

extern UART_HandleTypeDef huart3;

uint8_t Modbus_ReadRegister(uint8_t slave, uint16_t reg, uint16_t *value) {
    /* 功能码 03：从 reg 开始读 1 个保持寄存器，末两字节稍后填 CRC。 */
    uint8_t req[8] = {slave, 3, (uint8_t)(reg >> 8), (uint8_t)reg, 0, 1, 0, 0};
    uint8_t reply[7];
    uint16_t crc = GW_Crc16(req, 6);
    req[6] = (uint8_t)crc; req[7] = (uint8_t)(crc >> 8);
    /* RS485 模块自动控制 DE/RE 收发方向；串口参数为 19200、8E1。 */
    /* 最多请求两次；发送失败或回复超时/格式错误都进入下一次尝试。 */
    for (uint8_t attempt = 0; attempt < 2; ++attempt) {
        if (HAL_UART_Transmit(&huart3, req, sizeof req, 100) != HAL_OK) continue;
        if (HAL_UART_Receive(&huart3, reply, sizeof reply, 250) == HAL_OK &&
            reply[0] == slave && reply[1] == 3 && reply[2] == 2 &&
            GW_Crc16(reply, 5) == (uint16_t)(reply[5] | ((uint16_t)reply[6] << 8))) {
            /* Modbus 寄存器高字节在前，组合成 16 位结果并写回调用方。 */
            *value = (uint16_t)((uint16_t)reply[3] << 8) | reply[4];
            return 1;
        }
        /* 丢掉不完整回复和溢出标志，给下一次请求一个干净的接收状态。 */
        (void)HAL_UART_AbortReceive(&huart3);
        __HAL_UART_CLEAR_OREFLAG(&huart3);
        vTaskDelay(pdMS_TO_TICKS(3)); /* 重试前留出 Modbus RTU 帧间静默。 */
    }
    return 0;
}

#ifndef MODBUS_MASTER_H
#define MODBUS_MASTER_H
#include <stdint.h>
/* 用 Modbus RTU 功能码 03 读取指定从站的一个保持寄存器；成功返回 1。 */
uint8_t Modbus_ReadRegister(uint8_t slave, uint16_t reg, uint16_t *value);
#endif

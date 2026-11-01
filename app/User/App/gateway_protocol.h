#ifndef GATEWAY_PROTOCOL_H
#define GATEWAY_PROTOCOL_H

#include <stdint.h>

/* 遥测帧固定 24 字节；V3 的 18..19 是 CAN 值，20..21 是前 20 字节 CRC16。
 * 第 23 字节留给 Flash 队列：0xFF 待确认，0x00 已确认。 */
#define GW_FRAME_SIZE 24U
#define GW_MAGIC0 0x47U
#define GW_MAGIC1 0x57U
#define GW_VERSION 3U

/* 一次采样的业务字段；status 的 bit0/bit1 分别表示两个从站有效。 */
typedef struct {
    uint32_t sequence;
    uint16_t slave1_value;
    uint16_t slave2_value;
    uint16_t can_value;
    uint16_t status;
    uint32_t uptime_ms;
} GW_Sample;

/* status 的 bit0/bit1/bit2 分别表示从站 1/2/CAN 有效；旧 V2 帧仍可解码。 */
uint16_t GW_Crc16(const uint8_t *data, uint16_t length);
/* 结构体→24 字节小端帧；反向解码前先检查魔数、版本与 CRC。 */
void GW_Encode(const GW_Sample *sample, uint8_t frame[GW_FRAME_SIZE]);
uint8_t GW_Decode(const uint8_t frame[GW_FRAME_SIZE], GW_Sample *sample);

#endif

#include "gateway_protocol.h"

/* 手工按小端写/读整数：低字节在前，避免依赖编译器或指针对齐。 */
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16); }

uint16_t GW_Crc16(const uint8_t *data, uint16_t length)
{
    /* Modbus/网关帧共用 CRC16：每个字节再处理 8 个数据位。 */
    uint16_t crc = 0xffffU;
    for (uint16_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; ++j)
            crc = (crc & 1U) ? (uint16_t)((crc >> 1) ^ 0xa001U) : (uint16_t)(crc >> 1);
    }
    return crc;
}

void GW_Encode(const GW_Sample *sample, uint8_t frame[GW_FRAME_SIZE])
{
    /* 全部先填 0xFF；末字节因此天然代表“待 ACK”。 */
    for (uint8_t i = 0; i < GW_FRAME_SIZE; ++i) frame[i] = 0xffU;
    /* 0～3 字节是帧头，4～17 字节放采样字段。 */
    frame[0] = GW_MAGIC0; frame[1] = GW_MAGIC1; frame[2] = GW_VERSION; frame[3] = 1U;
    put32(frame + 4, sample->sequence);
    put16(frame + 8, sample->slave1_value);
    put16(frame + 10, sample->slave2_value);
    put16(frame + 12, sample->status);
    put32(frame + 14, sample->uptime_ms);
    put16(frame + 18, sample->can_value);
    /* V3 的 CRC 包含 CAN 值；第 23 字节的 ACK 标记不参与校验。 */
    put16(frame + 20, GW_Crc16(frame, 20));
}

uint8_t GW_Decode(const uint8_t frame[GW_FRAME_SIZE], GW_Sample *sample)
{
    /* 任一帧头或 CRC 检查失败都不把字节当有效采样。 */
    if (frame[0] != GW_MAGIC0 || frame[1] != GW_MAGIC1 || frame[3] != 1U) return 0;
    if (frame[2] == 2U) {
        if (get16(frame + 18) != GW_Crc16(frame, 18)) return 0;
    } else if (frame[2] == GW_VERSION) {
        if (get16(frame + 20) != GW_Crc16(frame, 20)) return 0;
    } else return 0;
    /* sample==0 时只验证，不需要把字段解出来。 */
    if (sample != 0) {
        sample->sequence = get32(frame + 4);
        sample->slave1_value = get16(frame + 8);
        sample->slave2_value = get16(frame + 10);
        sample->can_value = frame[2] == GW_VERSION ? get16(frame + 18) : 0U;
        sample->status = get16(frame + 12);
        sample->uptime_ms = get32(frame + 14);
    }
    return 1;
}

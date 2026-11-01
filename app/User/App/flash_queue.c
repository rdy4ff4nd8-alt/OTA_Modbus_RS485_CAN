#include "flash_queue.h"
#include "main.h"

extern SPI_HandleTypeDef hspi1;

#define SECTOR_SIZE 4096UL
/* 帧最后一个字节可单独写 0，表示电脑已提交该记录。 */
#define ACK_OFFSET (GW_FRAME_SIZE - 1U)
#define IO_TIMEOUT 100U
static uint32_t jedec_id;
volatile uint8_t flash_queue_append_error;
volatile uint8_t flash_queue_verify_index;
volatile uint8_t flash_queue_verify_expected;
volatile uint8_t flash_queue_verify_actual;

/* /CS 低有效：selected=1 时拉低，结束后必须拉高。 */
static void cs(uint8_t selected) { HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, selected ? GPIO_PIN_RESET : GPIO_PIN_SET); }
static uint8_t transfer(uint8_t value, uint8_t *received) {
    /* SPI 同时发送与接收 1 字节；received==0 表示只关心发送。 */
    uint8_t rx = 0xff;
    if (HAL_SPI_TransmitReceive(&hspi1, &value, &rx, 1, IO_TIMEOUT) != HAL_OK) return 0;
    if (received) *received = rx;
    return 1;
}
static uint8_t address(uint32_t value) {
    /* W25 的读/写/擦命令之后跟 24 位地址，高字节先发。 */
    return transfer((uint8_t)(value >> 16), 0) &&
           transfer((uint8_t)(value >> 8), 0) &&
           transfer((uint8_t)value, 0);
}
static uint8_t ready(uint32_t timeout_ms) {
    uint32_t start = HAL_GetTick();
    do {
        /* 命令 0x05 读状态寄存器；最低位为 1 表示芯片正在忙。 */
        uint8_t sr = 0xff;
        cs(1);
        uint8_t ok = transfer(0x05, 0) && transfer(0xff, &sr);
        cs(0);
        if (!ok) return 0;
        if ((sr & 1U) == 0) return 1;
    } while ((uint32_t)(HAL_GetTick() - start) < timeout_ms);
    return 0;
}
static uint8_t write_enable(void) {
    /* NOR Flash 每次编程或擦除之前都要重新发 0x06 写使能。 */
    cs(1); uint8_t ok = transfer(0x06, 0); cs(0); return ok;
}
static uint8_t read_bytes(uint32_t at, uint8_t *out, uint16_t count) {
    /* 0x03 普通读：命令+地址后逐字节把外部 Flash 拷到 RAM。 */
    if (!ready(50)) return 0;
    cs(1);
    if (!transfer(0x03, 0) || !address(at)) { cs(0); return 0; }
    for (uint16_t i = 0; i < count; ++i)
        if (!transfer(0xff, &out[i])) { cs(0); return 0; }
    cs(0);
    return 1;
}
static uint8_t program(uint32_t at, const uint8_t *data, uint16_t count) {
    /* 0x02 页编程不能越过 256 字节页边界。 */
    if (!ready(50) || count > 256U || ((at & 255U) + count) > 256U) return 0;
    if (!write_enable()) return 0;
    cs(1);
    if (!transfer(0x02, 0) || !address(at)) { cs(0); return 0; }
    for (uint16_t i = 0; i < count; ++i)
        if (!transfer(data[i], 0)) { cs(0); return 0; }
    cs(0); return ready(100);
}
static uint8_t erase(uint32_t at) {
    /* 0x20 擦除一个 4 KiB 扇区，使所有位重新变成 1。 */
    if (!ready(50) || !write_enable()) return 0;
    cs(1);
    uint8_t ok = transfer(0x20, 0) && address(at);
    cs(0);
    if (!ok) return 0;
    return ready(2000);
}
static uint32_t sequence_of(const uint8_t *frame) {
    /* 帧的第 4～7 字节按小端组成 32 位序号。 */
    return (uint32_t)frame[4] | ((uint32_t)frame[5] << 8) |
           ((uint32_t)frame[6] << 16) | ((uint32_t)frame[7] << 24);
}
static uint8_t read_slot(uint16_t slot, uint8_t frame[GW_FRAME_SIZE]) {
    /* 一条记录独占一个扇区：slot×4096 是它在 W25 中的起点。 */
    return read_bytes((uint32_t)slot * SECTOR_SIZE, frame, GW_FRAME_SIZE);
}

uint8_t FlashQueue_Init(void) {
    uint8_t id[3] = {0};
    jedec_id = 0;
    /* JEDEC ID 0x9F：确认芯片确为可用的 W25Q32 容量级。 */
    cs(0); cs(1);
    if (!transfer(0x9f, 0)) { cs(0); return 0; }
    for (uint8_t i = 0; i < 3; ++i)
        if (!transfer(0xff, &id[i])) { cs(0); return 0; }
    cs(0);
    jedec_id = ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | id[2];
    /* 实板 W25Q32 的 JEDEC ID 为 EF 40 16；拒绝其他容量和悬空总线。 */
    return id[0] == 0xefU && id[1] == 0x40U && id[2] == 0x16U;
}

uint32_t FlashQueue_JedecId(void) { return jedec_id; }

uint8_t FlashQueue_Append(GW_Sample *sample) {
    uint8_t frame[GW_FRAME_SIZE], temp[GW_FRAME_SIZE];
    flash_queue_append_error = 0;
    flash_queue_verify_index = 0xffU;
    /* 找最大旧序号，保证重启后新记录继续递增。 */
    uint32_t maximum = 0, oldest_acked = 0xffffffffUL;
    int16_t candidate_blank = -1, candidate_acked = -1;
    uint16_t maximum_slot = GW_QUEUE_SLOTS;
    /* 先找空槽；读取失败立即停止，不能贸然覆盖已有数据。 */
    for (uint16_t i = 0; i < GW_QUEUE_SLOTS; ++i) {
        if (!read_slot(i, temp)) { flash_queue_append_error = 1; return 0; }
        if (GW_Decode(temp, 0)) {
            uint32_t seq = sequence_of(temp);
            if (seq >= maximum) { maximum = seq; maximum_slot = i; }
        } else if (candidate_blank < 0) candidate_blank = (int16_t)i;
    }
    if (maximum == 0xffffffffUL) { flash_queue_append_error = 2; return 0; }
    /* 无空槽时，仅允许回收已经收到 PC ACK 的旧槽。 */
    if (candidate_blank < 0) {
        for (uint16_t i = 0; i < GW_QUEUE_SLOTS; ++i) {
            if (!read_slot(i, temp)) { flash_queue_append_error = 1; return 0; }
            if (i != maximum_slot && GW_Decode(temp, 0) && temp[ACK_OFFSET] == 0 &&
                sequence_of(temp) < oldest_acked) {
                oldest_acked = sequence_of(temp); candidate_acked = (int16_t)i;
            }
        }
    }
    int16_t target = candidate_blank >= 0 ? candidate_blank : candidate_acked;
    if (target < 0) { flash_queue_append_error = 3; return 0; } /* 队列满，不能覆盖未确认记录。 */
    /* 编码、擦扇区、写入、读回验证；任何一步失败都不报告成功。 */
    sample->sequence = maximum + 1U;
    GW_Encode(sample, frame);
    uint32_t at = (uint32_t)target * SECTOR_SIZE;
    if (!erase(at)) { flash_queue_append_error = 4; return 0; }
    if (!program(at, frame, GW_FRAME_SIZE)) { flash_queue_append_error = 5; return 0; }
    if (!read_slot((uint16_t)target, temp)) { flash_queue_append_error = 6; return 0; }
    if (!GW_Decode(temp, 0) || sequence_of(temp) != sample->sequence) {
        for (uint8_t i = 0; i < GW_FRAME_SIZE; ++i) {
            if (temp[i] != frame[i]) {
                flash_queue_verify_index = i;
                flash_queue_verify_expected = frame[i];
                flash_queue_verify_actual = temp[i];
                break;
            }
        }
        flash_queue_append_error = 7;
        return 0;
    }
    return 1;
}

uint8_t FlashQueue_Oldest(uint8_t frame[GW_FRAME_SIZE]) {
    /* 只从未 ACK 的帧里取最小序号，保证按先后顺序重发。 */
    uint8_t temp[GW_FRAME_SIZE];
    uint32_t minimum = 0xffffffffUL;
    uint8_t found = 0;
    for (uint16_t i = 0; i < GW_QUEUE_SLOTS; ++i) {
        if (!read_slot(i, temp)) return 0;
        if (GW_Decode(temp, 0) && temp[ACK_OFFSET] != 0 && sequence_of(temp) < minimum) {
            minimum = sequence_of(temp);
            for (uint8_t j = 0; j < GW_FRAME_SIZE; ++j) frame[j] = temp[j];
            found = 1;
        }
    }
    return found;
}

uint8_t FlashQueue_Ack(uint32_t sequence) {
    /* 不擦整个扇区，只把最后一字节从 0xFF 编程成 0x00。 */
    uint8_t temp[GW_FRAME_SIZE], zero = 0;
    for (uint16_t i = 0; i < GW_QUEUE_SLOTS; ++i) {
        if (!read_slot(i, temp)) return 0;
        if (GW_Decode(temp, 0) && sequence_of(temp) == sequence) {
            if (temp[ACK_OFFSET] == 0) return 1;
            return program((uint32_t)i * SECTOR_SIZE + ACK_OFFSET, &zero, 1);
        }
    }
    return 0;
}

uint16_t FlashQueue_PendingCount(void) {
    /* 用于观察队列：统计 ACK 字节仍非 0 的有效帧。 */
    uint8_t temp[GW_FRAME_SIZE]; uint16_t count = 0;
    for (uint16_t i = 0; i < GW_QUEUE_SLOTS; ++i) {
        if (!read_slot(i, temp)) return count;
        if (GW_Decode(temp, 0) && temp[ACK_OFFSET] != 0) ++count;
    }
    return count;
}

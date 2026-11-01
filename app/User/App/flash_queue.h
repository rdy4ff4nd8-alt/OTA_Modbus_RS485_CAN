#ifndef FLASH_QUEUE_H
#define FLASH_QUEUE_H
#include <stdint.h>
#include "gateway_protocol.h"

/* W25Q32 的前 512 个 4 KiB 扇区用于采样队列，每扇区一条记录；
 * 用空间换取简单的断电恢复和确认标记。 */
#define GW_QUEUE_SLOTS 512U

/* 最近一次 Append 失败步骤，供 OLED 和 Keil Watch 诊断：
 * 1=扫描读失败，2=序号耗尽，3=队列满，4=擦除失败，
 * 5=编程失败，6=回读失败，7=回读内容不符；0=成功。 */
extern volatile uint8_t flash_queue_append_error;
/* E07 时第一个不相同的字节：索引、预期值、回读值；索引 FF 表示字节完全相同。 */
extern volatile uint8_t flash_queue_verify_index;
extern volatile uint8_t flash_queue_verify_expected;
extern volatile uint8_t flash_queue_verify_actual;

/* 依次为：扫描恢复、持久化采样、读最旧未确认帧、按序号确认、统计积压数。 */
uint8_t FlashQueue_Init(void);
uint32_t FlashQueue_JedecId(void);
uint8_t FlashQueue_Append(GW_Sample *sample);
uint8_t FlashQueue_Oldest(uint8_t frame[GW_FRAME_SIZE]);
uint8_t FlashQueue_Ack(uint32_t sequence);
uint16_t FlashQueue_PendingCount(void);

#endif

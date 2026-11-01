#ifndef OTA_LAYOUT_H
#define OTA_LAYOUT_H

#include <stdint.h>

/* STM32F103C8：内部 Flash 64 KiB、每页 1 KiB，SRAM 20 KiB。 */
/* 0x080... 是 STM32 内部 Flash：前 24 KiB 是 Bootloader，后 40 KiB 是应用。 */
#define OTA_BOOT_BASE       0x08000000UL
#define OTA_BOOT_SIZE       0x00006000UL
#define OTA_APP_BASE        0x08006000UL
#define OTA_APP_SIZE        0x0000A000UL
#define OTA_FLASH_END       0x08010000UL
#define OTA_PAGE_SIZE       1024UL
#define OTA_RAM_BASE        0x20000000UL
#define OTA_RAM_END         0x20005000UL

/* W25Q32 的 0x000000..0x1FFFFF 留给网关队列，共 512 个 4 KiB 扇区。 */
/* 以下 0x00... 地址属于 W25Q32，不是 STM32 内部 Flash 地址。 */
#define OTA_META_A          0x200000UL
#define OTA_META_B          0x201000UL
/* 候选与备份各预留 40 KiB；它们互不重叠，也不碰前 2 MiB 队列。 */
#define OTA_CANDIDATE      0x210000UL
#define OTA_BACKUP         0x220000UL
#define OTA_SECTOR_SIZE     4096UL
#define OTA_BLOCK_SIZE      128U

#define OTA_META_MAGIC      0x3441544FUL /* "OTA4"：C8 App 下载格式，隔离旧 OTA2 和 VET6 OTA3 */
#define OTA_STATE_IDLE      0U
#define OTA_STATE_INSTALL   1U
#define OTA_STATE_TESTING   2U
#define OTA_STATE_STAGED    3U /* App 已校验完整候选，Bootloader 尚未备份旧版 */

/* 版本用整数表示：10000 对应 v1.0.0，10100 对应 v1.1.0。 */
typedef struct {
    /* magic/校验和用于识别完整记录；generation 用于选 A/B 较新的一份。 */
    uint32_t magic;
    uint32_t generation;
    uint32_t state;
    uint32_t version;
    /* pending_version 只有试装或安装时有效；确认后清零。 */
    uint32_t pending_version;
    uint32_t image_len;
    uint32_t backup_crc;
    uint8_t image_sha[32];
    /* CRC32 覆盖本结构体在 checksum 之前的所有字节。 */
    uint32_t checksum;
} ota_meta_t;

#endif

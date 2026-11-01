#ifndef OTA_W25_H
#define OTA_W25_H
#include <stddef.h>
#include <stdint.h>
/* W25Q32 的初始化、按地址读写和 4 KiB 扇区擦除；返回 1 表示成功。 */
int ota_w25_init(void);
int ota_w25_read(uint32_t addr, void *out, size_t n);
int ota_w25_write(uint32_t addr, const void *data, size_t n);
int ota_w25_erase4k(uint32_t addr);
#endif

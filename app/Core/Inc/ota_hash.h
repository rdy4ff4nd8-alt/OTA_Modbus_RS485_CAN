#ifndef OTA_HASH_H
#define OTA_HASH_H
#include <stddef.h>
#include <stdint.h>

/* CRC32 可传入上一块结果作 seed，以便分块累计。 */
uint32_t ota_crc32(const void *data, size_t n, uint32_t seed);
/* SHA-256 状态里保存 8 个工作字、已处理位数和未满的 64 字节块。 */
typedef struct {
    uint32_t h[8];
    uint64_t bits;
    uint8_t block[64];
    uint32_t used;
} ota_sha256_t;
/* init -> 零次或多次 update -> final 得到 32 字节摘要。 */
void ota_sha256_init(ota_sha256_t *s);
void ota_sha256_update(ota_sha256_t *s, const void *data, size_t n);
void ota_sha256_final(ota_sha256_t *s, uint8_t out[32]);
#endif

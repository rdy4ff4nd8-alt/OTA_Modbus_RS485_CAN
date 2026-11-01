#ifndef OTA_META_H
#define OTA_META_H
#include "ota_layout.h"
/* 双份元数据的读取、校验、写入；返回 1 表示成功，0 表示失败。 */
int ota_meta_load(ota_meta_t *out);
int ota_meta_save(ota_meta_t *meta);
int ota_meta_valid(const ota_meta_t *meta);
#endif

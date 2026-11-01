#ifndef OTA_DOWNLOAD_H
#define OTA_DOWNLOAD_H
#include "ota_layout.h"

/* 在 BridgeTask 中处理整行；返回 0=普通命令，1=已处理，2=提交后必须复位。 */
int ota_download_line(const char *line, ota_meta_t *meta);
int ota_download_active(void);
void ota_download_poll(void);
void ota_download_abort(void);

/* App 提供的板级接口；每次 Flash 操作单独持锁，释放后让采样任务运行。 */
void ota_download_lock(void);
void ota_download_unlock(void);
void ota_download_pause(void);
uint32_t ota_download_now(void);
void ota_download_reply(const char *line);
#endif

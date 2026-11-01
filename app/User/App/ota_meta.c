#include "ota_meta.h"
#include "ota_hash.h"
#include "ota_w25.h"
#include <stddef.h>
#include <string.h>

/* 编译期检查结构体恰好 64 字节；尺寸变了就不能按旧格式读 W25。 */
typedef char meta_must_be_64_bytes[(sizeof(ota_meta_t)==64U)?1:-1];
static uint32_t active_sector=OTA_META_B;

int ota_meta_valid(const ota_meta_t *m) {
    /* 魔数、状态、长度和自身 CRC 都正确才算一份有效元数据。 */
    return m->magic==OTA_META_MAGIC && m->state<=OTA_STATE_STAGED &&
           m->image_len<=OTA_APP_SIZE &&
           ota_crc32(m,offsetof(ota_meta_t,checksum),0)==m->checksum;
}
int ota_meta_load(ota_meta_t *out) {
    /* A/B 双副本：掉电可能只写好其中一份，分别读回验证。 */
    ota_meta_t a,b;int va=ota_w25_read(OTA_META_A,&a,sizeof a)&&ota_meta_valid(&a);
    int vb=ota_w25_read(OTA_META_B,&b,sizeof b)&&ota_meta_valid(&b);
    if (va && (!vb || (int32_t)(a.generation-b.generation)>0)) {
        /* 两份都有效时按递增代数选较新版本。 */
        *out=a;active_sector=OTA_META_A;return 1;
    }
    if (vb) { *out=b;active_sector=OTA_META_B;return 1; }
    /* 两份都无效：首次启动按空白元数据初始化。 */
    memset(out,0,sizeof *out);out->magic=OTA_META_MAGIC;
    active_sector=OTA_META_B;return 0;
}
int ota_meta_save(ota_meta_t *m) {
    /* 在副本上计算新代数和校验和，旧有效扇区先不动。 */
    ota_meta_t next=*m,verify;
    next.magic=OTA_META_MAGIC;next.generation=m->generation+1U;
    next.checksum=ota_crc32(&next,offsetof(ota_meta_t,checksum),0);
    uint32_t target=active_sector==OTA_META_A?OTA_META_B:OTA_META_A;
    /* 写到另一个扇区并读回逐字节比较；成功后才切换 active_sector。 */
    if (!ota_w25_erase4k(target) ||
        !ota_w25_write(target,&next,sizeof next) ||
        !ota_w25_read(target,&verify,sizeof verify) ||
        memcmp(&next,&verify,sizeof next)!=0) return 0;
    *m=next;active_sector=target;return 1;
}

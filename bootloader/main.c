#include "ota_board.h"
#include "ota_hash.h"
#include "ota_layout.h"
#include "ota_meta.h"
#include "ota_w25.h"
#include <string.h>

/* App 负责下载；Bootloader 只处理已提交镜像的备份、安装和断电恢复。 */
static uint8_t buffer[256];
static ota_meta_t meta;

static int image_vector(const uint8_t *p,uint32_t size) {
    if (size<8U) return 0;
    /* 镜像开头 8 字节是小端的初始栈指针 sp 和复位入口 pc。 */
    uint32_t sp=(uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
    uint32_t pc=(uint32_t)p[4]|((uint32_t)p[5]<<8)|((uint32_t)p[6]<<16)|((uint32_t)p[7]<<24);
    return sp>=OTA_RAM_BASE&&sp<=OTA_RAM_END&&!(sp&7U)&&pc>=OTA_APP_BASE&&
           /* Thumb 函数地址最低位必须为 1，实际 Flash 地址需落在镜像内。 */
           (pc&~1UL)<OTA_APP_BASE+size&&(pc&1U);
}
static int current_app_valid(void) {
    /* 只检查旧应用向量是否像一个可启动程序；完整备份还要另做 CRC。 */
    return image_vector((const uint8_t *)OTA_APP_BASE,OTA_APP_SIZE);
}
static int ext_crc(uint32_t base,uint32_t size,uint32_t expected) {
    /* 分 256 字节读 W25 并累计 CRC，不需要把整个镜像装入 RAM。 */
    uint32_t crc=0;
    for(uint32_t off=0;off<size;off+=sizeof buffer) {
        uint32_t n=size-off;if(n>sizeof buffer)n=sizeof buffer;
        if(!ota_w25_read(base+off,buffer,n))return 0;
        crc=ota_crc32(buffer,n,crc);
    }
    return crc==expected;
}
static int candidate_sha(void) {
    /* 候选镜像先验长度和向量，再核对完整 SHA-256。 */
    ota_sha256_t sha;uint8_t digest[32];ota_sha256_init(&sha);
    if(meta.image_len<8U||meta.image_len>OTA_APP_SIZE||
       !ota_w25_read(OTA_CANDIDATE,buffer,8)||!image_vector(buffer,meta.image_len))return 0;
    for(uint32_t off=0;off<meta.image_len;off+=sizeof buffer) {
        uint32_t n=meta.image_len-off;if(n>sizeof buffer)n=sizeof buffer;
        if(!ota_w25_read(OTA_CANDIDATE+off,buffer,n))return 0;
        ota_sha256_update(&sha,buffer,n);
    }
    ota_sha256_final(&sha,digest);
    return memcmp(digest,meta.image_sha,32)==0;
}
static int backup_current(void) {
    uint32_t crc=0;
    if(!current_app_valid())return 0;
    /* 先擦 W25 备份区，再逐块复制整个 40 KiB 内部应用区。 */
    for(uint32_t a=0;a<OTA_APP_SIZE;a+=OTA_SECTOR_SIZE)
        if(!ota_w25_erase4k(OTA_BACKUP+a))return 0;
    for(uint32_t off=0;off<OTA_APP_SIZE;off+=sizeof buffer) {
        const void *p=(const void *)(OTA_APP_BASE+off);
        crc=ota_crc32(p,sizeof buffer,crc);
        if(!ota_w25_write(OTA_BACKUP+off,p,sizeof buffer))return 0;
    }
    if(!ext_crc(OTA_BACKUP,OTA_APP_SIZE,crc))return 0;
    /* 读回验证备份后，才允许把 CRC 放进待提交元数据。 */
    meta.backup_crc=crc;
    return 1;
}
static int flash_image(uint32_t ext_base,uint32_t size) {
    FLASH_EraseInitTypeDef e={0};uint32_t page_error=0;
    /* C8 按 1 KiB 页擦除；整槽擦空后，只写有效长度，尾部保持 0xFF。 */
    if(HAL_FLASH_Unlock()!=HAL_OK)return 0;
    e.TypeErase=FLASH_TYPEERASE_PAGES;e.PageAddress=OTA_APP_BASE;
    e.NbPages=OTA_APP_SIZE/OTA_PAGE_SIZE;
    int ok=HAL_FLASHEx_Erase(&e,&page_error)==HAL_OK;
    for(uint32_t off=0;ok&&off<size;off+=sizeof buffer) {
        /* 候选或备份区每次读 256 字节，统一刷到内部应用槽。 */
        uint32_t n=size-off;if(n>sizeof buffer)n=sizeof buffer;
        memset(buffer,0xff,sizeof buffer);
        ok=ota_w25_read(ext_base+off,buffer,n);
        for(uint32_t i=0;ok&&i<n;i+=2U) {
            /* STM32F1 以 16 位半字编程，写后立即读回比较。 */
            uint16_t half=(uint16_t)buffer[i]|((uint16_t)buffer[i+1U]<<8);
            uint32_t addr=OTA_APP_BASE+off+i;
            ok=HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,addr,half)==HAL_OK &&
               *(volatile uint16_t *)addr==half;
        }
    }
    /* 无论成功失败都锁回内部 Flash，最后再检查应用向量。 */
    (void)HAL_FLASH_Lock();return ok&&current_app_valid();
}
static int installed_matches_candidate(void) {
    /* 刷写后直接对内部应用算 SHA，确认与收到的候选完全一致。 */
    ota_sha256_t sha;uint8_t digest[32];ota_sha256_init(&sha);
    ota_sha256_update(&sha,(const void *)OTA_APP_BASE,meta.image_len);
    ota_sha256_final(&sha,digest);
    return memcmp(digest,meta.image_sha,32)==0;
}
static int restore_backup(void) {
    /* 回退前先核对 W25 备份 CRC，回刷后再核对内部 CRC。 */
    if(!ext_crc(OTA_BACKUP,OTA_APP_SIZE,meta.backup_crc)||
       !flash_image(OTA_BACKUP,OTA_APP_SIZE))return 0;
    uint32_t crc=ota_crc32((const void *)OTA_APP_BASE,OTA_APP_SIZE,0);
    if(crc!=meta.backup_crc)return 0;
    /* 恢复备份后回到稳定状态，并保留备份对应的版本号。 */
    meta.state=OTA_STATE_IDLE;meta.pending_version=0;meta.image_len=0;
    return ota_meta_save(&meta);
}
static int install_candidate(void) {
    /* 候选验证与刷写都成功后，才写 TESTING 等待新应用确认。 */
    if(!candidate_sha())return 0;
    if(!flash_image(OTA_CANDIDATE,meta.image_len)||!installed_matches_candidate())return 0;
    meta.state=OTA_STATE_TESTING;
    return ota_meta_save(&meta);
}
/* STAGED 阶段尚未碰内部 App；候选损坏或备份失败时可安全取消。 */
static int cancel_stage(void) {
    meta.state=OTA_STATE_IDLE;meta.pending_version=0;meta.image_len=0;
    return ota_meta_save(&meta);
}
int main(void) {
    ota_board_init();
    if(!ota_w25_init()) {
        /* 外部 Flash 不可用时不能把整机锁死在 Bootloader：报错后照样启动 App，
           App 会显示 FLASH ERR 并继续采样，OTA 才需要外部 Flash。 */
        ota_board_uart_line("FLASH_ERR\n");
        ota_board_jump_app();
        for(;;)HAL_Delay(250);
    }
    (void)ota_meta_load(&meta);
    if(meta.state==OTA_STATE_STAGED) {
        /* 断电发生在备份期间，下次仍是 STAGED，可重新备份完整旧 App。 */
        if(!candidate_sha()||!backup_current()) {
            if(!cancel_stage()) {ota_board_uart_line("META_ERR\n");for(;;)HAL_Delay(250);}
            ota_board_uart_line("REJECT\n");
        } else {
            /* 只有验证过备份，才持久化 INSTALL 并允许擦写内部应用。 */
            meta.state=OTA_STATE_INSTALL;
            if(!ota_meta_save(&meta)) {ota_board_uart_line("META_ERR\n");for(;;)HAL_Delay(250);}
        }
    }
    if(meta.state==OTA_STATE_INSTALL) {
        if(install_candidate())ota_board_uart_line("INSTALLED\n");
        else if(restore_backup())ota_board_uart_line("ROLLED_BACK\n");
        else {ota_board_uart_line("RECOVERY_ERR\n");for(;;)HAL_Delay(250);}
    } else if(meta.state==OTA_STATE_TESTING) {
        /* 上次安装成功但新版尚未确认就复位：恢复旧版。 */
        if(!restore_backup()) {ota_board_uart_line("ROLLBACK_ERR\n");for(;;)HAL_Delay(250);}
        ota_board_uart_line("ROLLED_BACK\n");
    }
    /* 无待处理升级时直接启动 App。 */
    ota_board_jump_app();
    ota_board_uart_line("NO_APP\n");
    for(;;)HAL_Delay(1000);
}

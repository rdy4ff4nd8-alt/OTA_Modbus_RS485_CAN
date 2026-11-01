#include "ota_download.h"
#include "ota_hash.h"
#include "ota_meta.h"
#include "ota_w25.h"
#include <string.h>

/* 一次只接收一个候选镜像；中途断电时这些 RAM 状态自然消失，旧 App 保持有效。 */
static uint8_t active, expected_sha[32], block[OTA_BLOCK_SIZE], verify[OTA_BLOCK_SIZE];
static uint32_t image_len, version, received, next_seq, last_rx;

static int hex(char c) {
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    if(c>='A'&&c<='F')return c-'A'+10;
    return -1;
}
static int number(const char **p,uint32_t *out) {
    uint32_t v=0;unsigned count=0;
    while(**p>='0'&&**p<='9') {
        uint32_t d=(uint32_t)(*(*p)++-'0');
        if(v>(0xffffffffUL-d)/10U)return 0;
        v=v*10U+d;++count;
    }
    *out=v;return count!=0;
}
static int unhex(const char *p,uint8_t *out,size_t n) {
    for(size_t i=0;i<n;++i) {
        int hi=hex(p[2*i]),lo=hex(p[2*i+1]);
        if(hi<0||lo<0)return 0;
        out[i]=(uint8_t)((hi<<4)|lo);
    }
    return 1;
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static void reply_seq(const char *prefix,uint32_t seq) {
    char line[24],digits[10];unsigned used=0,n=0;
    while(*prefix)line[used++]=*prefix++;
    do {digits[n++]=(char)('0'+seq%10U);seq/=10U;} while(seq);
    while(n)line[used++]=digits[--n];
    line[used++]='\n';line[used]=0;ota_download_reply(line);
}
int ota_download_active(void) { return active; }
void ota_download_abort(void) {
    active=0;ota_download_reply("FAIL\n");
}
void ota_download_poll(void) {
    /* 无数据 15 秒就放弃本次下载，恢复正常遥测，且不写升级标志。 */
    if(active&&ota_download_now()-last_rx>=15000U)ota_download_abort();
}
static int start(const char *s,const ota_meta_t *meta) {
    uint32_t v,n;uint8_t sha[32];
    if(active||meta->state!=OTA_STATE_IDLE) {ota_download_reply("BUSY\n");return 1;}
    if(!number(&s,&v)||*s++!=' '||!number(&s,&n)||*s++!=' '||
       strlen(s)!=64U||!unhex(s,sha,32)||v<=meta->version||n<8U||n>OTA_APP_SIZE) {
        ota_download_reply("REJECT\n");return 1;
    }
    active=1;version=v;image_len=n;received=0;next_seq=0;
    memcpy(expected_sha,sha,32);
    /* 只擦镜像实际需要的扇区，每个扇区之间释放互斥锁，允许采样入队。 */
    for(uint32_t off=0;off<n;off+=OTA_SECTOR_SIZE) {
        ota_download_lock();int ok=ota_w25_erase4k(OTA_CANDIDATE+off);ota_download_unlock();
        ota_download_pause();
        if(!ok) {ota_download_abort();return 1;}
    }
    last_rx=ota_download_now();ota_download_reply("READY\n");return 1;
}
static int data(const char *s) {
    uint32_t seq,crc=0,off=0,wanted=0;size_t n;
    if(!active) {ota_download_reply("FAIL\n");return 1;}
    /* DATA <序号> <8 位 CRC32> <十六进制数据>，换行提供重同步边界。 */
    if(!number(&s,&seq)||*s++!=' '||strlen(s)<10U)goto nak;
    for(unsigned i=0;i<8;++i) {int h=hex(*s++);if(h<0)goto nak;crc=(crc<<4)|(uint32_t)h;}
    if(*s++!=' ')goto nak;
    n=strlen(s);
    if(!n||(n&1U)||n>2U*OTA_BLOCK_SIZE)goto nak;
    n/=2U;
    if(!unhex(s,block,n)||ota_crc32(block,n,0)!=crc)goto nak;
    if(seq>=((image_len+OTA_BLOCK_SIZE-1U)/OTA_BLOCK_SIZE))goto nak;
    off=seq*OTA_BLOCK_SIZE;wanted=image_len-off;
    if(wanted>OTA_BLOCK_SIZE)wanted=OTA_BLOCK_SIZE;
    if(n!=wanted)goto nak;
    /* ACK 丢失造成上一块重发时，读回并比较，避免重复推进进度。 */
    if(next_seq&&seq==next_seq-1U) {
        ota_download_lock();int ok=ota_w25_read(OTA_CANDIDATE+off,verify,n);ota_download_unlock();
        if(!ok) {ota_download_abort();return 1;}
        if(memcmp(block,verify,n))goto nak;
    } else {
        if(seq!=next_seq)goto nak;
        ota_download_lock();
        int ok=ota_w25_write(OTA_CANDIDATE+off,block,n)&&
               ota_w25_read(OTA_CANDIDATE+off,verify,n)&&memcmp(block,verify,n)==0;
        ota_download_unlock();
        if(!ok) {ota_download_abort();return 1;}
        received+=(uint32_t)n;++next_seq;
    }
    last_rx=ota_download_now();reply_seq("ACK ",seq);return 1;
nak:
    reply_seq("NAK ",next_seq);return 1;
}
static int finish(ota_meta_t *meta) {
    if(!active||received!=image_len) {ota_download_abort();return 1;}
    ota_sha256_t sha;uint8_t digest[32];ota_sha256_init(&sha);
    /* 从 W25 重读整包算 SHA，不依赖仅在 UART 上算出的摘要。 */
    for(uint32_t off=0;off<image_len;off+=sizeof block) {
        uint32_t n=image_len-off;if(n>sizeof block)n=sizeof block;
        ota_download_lock();int ok=ota_w25_read(OTA_CANDIDATE+off,block,n);ota_download_unlock();
        if(!ok) {ota_download_abort();return 1;}
        if(off==0) {
            uint32_t sp=le32(block),pc=le32(block+4);
            if(sp<OTA_RAM_BASE||sp>OTA_RAM_END||(sp&7U)||!(pc&1U)||
               (pc&~1UL)<OTA_APP_BASE||(pc&~1UL)>=OTA_APP_BASE+image_len) {
                ota_download_abort();return 1;
            }
        }
        ota_sha256_update(&sha,block,n);ota_download_pause();
    }
    ota_sha256_final(&sha,digest);
    if(memcmp(digest,expected_sha,32)) {ota_download_abort();return 1;}
    /* STAGED 表示候选完整但尚未备份/覆盖内部 Flash，Bootloader 再完成安装。 */
    ota_meta_t next=*meta;
    next.state=OTA_STATE_STAGED;next.pending_version=version;next.image_len=image_len;
    memcpy(next.image_sha,expected_sha,32);
    ota_download_lock();int ok=ota_meta_save(&next);ota_download_unlock();
    active=0;
    if(ok)*meta=next;
    ota_download_reply(ok?"STAGED\n":"FAIL\n");
    /* 写入后读回失败也可能已提交：必须复位让 Bootloader 判定，不能再覆盖候选区。 */
    return 2;
}
int ota_download_line(const char *line,ota_meta_t *meta) {
    if(strncmp(line,"START ",6)==0)return start(line+6,meta);
    if(strncmp(line,"DATA ",5)==0)return data(line+5);
    if(strcmp(line,"FINISH")==0)return finish(meta);
    if(strcmp(line,"ABORT")==0) {active=0;ota_download_reply("ABORTED\n");return 1;}
    return 0;
}

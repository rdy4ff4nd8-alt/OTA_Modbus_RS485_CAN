#include "ota_hash.h"
#include <string.h>

uint32_t ota_crc32(const void *data, size_t n, uint32_t seed) {
    /* seed 让整份固件可逐块累计 CRC，不必一次载入 RAM。 */
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = seed ^ 0xffffffffUL;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (unsigned b = 0; b < 8; ++b) c = (c >> 1) ^ ((c & 1U) ? 0xedb88320UL : 0U);
    }
    return c ^ 0xffffffffUL;
}

/* SHA-256 用循环右移和大端 32 位字；它与网关帧的小端字段不同。 */
static uint32_t rotr(uint32_t v, unsigned n) { return (v >> n) | (v << (32U - n)); }
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void putbe32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
/* SHA-256 标准规定的 64 个轮常量，不是密钥。 */
static const uint32_t k[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
static void transform(ota_sha256_t *s, const uint8_t p[64]) {
    /* 将 64 字节输入展开为 64 个 32 位字，再执行 64 轮压缩。 */
    uint32_t w[64];
    /* 原始块每 4 字节组成一个大端字，前 16 个字直接来自输入。 */
    for (unsigned i = 0; i < 16; ++i) w[i] = be32(p + 4U*i);
    for (unsigned i = 16; i < 64; ++i) {
        /* 后 48 个字由较早的字、循环右移和逻辑右移推导。 */
        uint32_t a = w[i-15], b = w[i-2];
        w[i] = w[i-16] + (rotr(a,7)^rotr(a,18)^(a>>3)) + w[i-7] + (rotr(b,17)^rotr(b,19)^(b>>10));
    }
    uint32_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4],f=s->h[5],g=s->h[6],h=s->h[7];
    for (unsigned i = 0; i < 64; ++i) {
        /* t1 含选择函数 (e&f)^(~e&g)；t2 含多数函数。 */
        uint32_t s1=rotr(e,6)^rotr(e,11)^rotr(e,25);
        uint32_t t1=h+s1+((e&f)^(~e&g))+k[i]+w[i];
        uint32_t s0=rotr(a,2)^rotr(a,13)^rotr(a,22);
        uint32_t t2=s0+((a&b)^(a&c)^(b&c));
        /* 八个工作字轮换并混入 t1/t2；uint32_t 溢出按模 2^32 处理。 */
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    /* 压缩结果累加到上下文，供下一块继续使用。 */
    s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;
    s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}
void ota_sha256_init(ota_sha256_t *s) {
    /* 算法标准初值；bits 记总位数，used 记当前 64 字节块已用长度。 */
    static const uint32_t iv[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                 0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(s->h,iv,sizeof iv);s->bits=0;s->used=0;
}
void ota_sha256_update(ota_sha256_t *s, const void *data, size_t n) {
    /* 流式喂数据：满 64 字节才运行一次 transform。 */
    const uint8_t *p=(const uint8_t *)data;
    while (n) {
        size_t take=64U-s->used;
        if (take>n) take=n;
        memcpy(s->block+s->used,p,take);s->used+=(uint32_t)take;
        s->bits+=(uint64_t)take*8U;p+=take;n-=take;
        if (s->used==64U) { transform(s,s->block);s->used=0; }
    }
}
void ota_sha256_final(ota_sha256_t *s, uint8_t out[32]) {
    /* SHA-256 规定先加 0x80、补零，再把原始总位数写在末 8 字节。 */
    uint64_t bits=s->bits;
    s->block[s->used++]=0x80;
    /* 长度字段要占最后 8 字节；空间不足就先处理一个填充块。 */
    if (s->used>56U) {
        memset(s->block+s->used,0,64U-s->used);transform(s,s->block);s->used=0;
    }
    memset(s->block+s->used,0,56U-s->used);
    /* 将原始位长度按大端写入最后 8 字节，再输出 8 个大端字。 */
    for (unsigned i=0;i<8;++i) s->block[56U+i]=(uint8_t)(bits>>(56U-8U*i));
    transform(s,s->block);
    for (unsigned i=0;i<8;++i) putbe32(out+4U*i,s->h[i]);
    /* 摘要写出后清除中间状态。 */
    memset(s,0,sizeof *s);
}

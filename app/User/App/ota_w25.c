#include "ota_w25.h"
#include "stm32f1xx_hal.h"

extern SPI_HandleTypeDef hspi1;
/* W25Q32 为 32 Mbit = 4 MiB，所有传入地址都先做容量边界检查。 */
#define FLASH_BYTES 0x400000UL
#define CS_LOW() HAL_GPIO_WritePin(GPIOA,GPIO_PIN_4,GPIO_PIN_RESET)
#define CS_HIGH() HAL_GPIO_WritePin(GPIOA,GPIO_PIN_4,GPIO_PIN_SET)

static int tx(const uint8_t *p, uint16_t n) { return HAL_SPI_Transmit(&hspi1,(uint8_t *)p,n,100)==HAL_OK; }
static int rx(uint8_t *p, uint16_t n) { return HAL_SPI_Receive(&hspi1,p,n,100)==HAL_OK; }
static void address(uint8_t cmd[4],uint8_t op,uint32_t a) {
    /* 一字节命令后接三字节地址，高位先传。 */
    cmd[0]=op;cmd[1]=(uint8_t)(a>>16);cmd[2]=(uint8_t)(a>>8);cmd[3]=(uint8_t)a;
}
static int bounds(uint32_t a,size_t n) { return a<FLASH_BYTES && n<=FLASH_BYTES-a; }
static int wait_ready(void) {
    /* 状态寄存器 0x05 的 bit0 为忙位；最多等待 3 秒。 */
    uint32_t start=HAL_GetTick();uint8_t cmd=5,status=1;
    do {
        CS_LOW();int ok=tx(&cmd,1)&&rx(&status,1);CS_HIGH();
        if (!ok) return 0;
        if (!(status&1U)) return 1;
        HAL_Delay(1);
    } while (HAL_GetTick()-start<3000U);
    return 0;
}
static int wren(void) {
    /* 每次擦/写前都须发送 0x06 写使能。 */
    uint8_t c=6;CS_LOW();int ok=tx(&c,1);CS_HIGH();return ok;
}
int ota_w25_init(void) {
    /* JEDEC 命令 0x9F 读取厂商、型号、容量码。 */
    uint8_t cmd=0x9f,id[3]={0};CS_LOW();int ok=tx(&cmd,1)&&rx(id,3);CS_HIGH();
    /* 只有实板确认的 EF 40 16 才允许写入；App 与 Bootloader 使用同一检查。 */
    return ok && id[0]==0xef && id[1]==0x40 && id[2]==0x16 && wait_ready();
}
int ota_w25_read(uint32_t a,void *out,size_t n) {
    /* 普通读命令 0x03，分块接收以适配 HAL 的长度参数。 */
    if (!bounds(a,n)) return 0;
    uint8_t c[4];address(c,3,a);
    CS_LOW();int ok=tx(c,4);
    while (ok&&n) { uint16_t chunk=n>256U?256U:(uint16_t)n;ok=rx((uint8_t *)out,chunk);
        out=(uint8_t *)out+chunk;n-=chunk; }
    CS_HIGH();return ok;
}
int ota_w25_erase4k(uint32_t a) {
    /* 0x20 只擦 4 KiB 对齐的一个扇区。 */
    if ((a&4095U)||!bounds(a,4096U)||!wait_ready()||!wren()) return 0;
    uint8_t c[4];address(c,0x20,a);CS_LOW();int ok=tx(c,4);CS_HIGH();
    return ok&&wait_ready();
}
int ota_w25_write(uint32_t a,const void *data,size_t n) {
    /* 页编程不能跨 256 字节边界，因此按当前页剩余空间切块。 */
    if (!bounds(a,n)) return 0;
    const uint8_t *p=(const uint8_t *)data;
    while (n) {
        size_t chunk=256U-(a&255U);if(chunk>n)chunk=n;
        if (!wait_ready()||!wren()) return 0;
        uint8_t c[4];address(c,2,a);CS_LOW();int ok=tx(c,4)&&tx(p,(uint16_t)chunk);CS_HIGH();
        if (!ok||!wait_ready()) return 0;
        a+=(uint32_t)chunk;p+=chunk;n-=chunk;
    }
    return 1;
}

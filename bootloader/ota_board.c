#include "ota_board.h"
#include "ota_layout.h"
#include <string.h>

SPI_HandleTypeDef hspi1;
UART_HandleTypeDef huart1;

/* 裸机启动文件调用 __libc_init_array 时需要这个空的 _init 符号。 */
void _init(void) {}

void ota_board_init(void) {
    /* Bootloader 自己初始化 HAL；应用稍后会重新配置自己的时钟和外设。 */
    HAL_Init();
    /* 使用芯片内部 HSI 时钟，不依赖板上外部晶振规格。 */
    RCC_OscInitTypeDef osc={0};RCC_ClkInitTypeDef clk={0};
    osc.OscillatorType=RCC_OSCILLATORTYPE_HSI;osc.HSIState=RCC_HSI_ON;
    osc.HSICalibrationValue=RCC_HSICALIBRATION_DEFAULT;
    if (HAL_RCC_OscConfig(&osc)!=HAL_OK) for(;;){}
    clk.ClockType=RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK|RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource=RCC_SYSCLKSOURCE_HSI;clk.AHBCLKDivider=RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider=RCC_HCLK_DIV1;clk.APB2CLKDivider=RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clk,FLASH_LATENCY_0)!=HAL_OK) for(;;){}
    /* 先把 W25 /CS 拉高，避免初始化 SPI 时误发擦写命令。 */
    __HAL_RCC_GPIOA_CLK_ENABLE();__HAL_RCC_GPIOB_CLK_ENABLE();
    HAL_GPIO_WritePin(GPIOA,GPIO_PIN_4,GPIO_PIN_SET);
    GPIO_InitTypeDef p={0};p.Pin=GPIO_PIN_4;
    p.Mode=GPIO_MODE_OUTPUT_PP;p.Speed=GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA,&p);
    p.Pin=GPIO_PIN_13;HAL_GPIO_Init(GPIOB,&p);
    HAL_GPIO_WritePin(GPIOB,GPIO_PIN_13,GPIO_PIN_SET);
    /* SPI1 模式 0 接 W25Q32；片选由 PA4 GPIO 手动控制。 */
    hspi1.Instance=SPI1;hspi1.Init.Mode=SPI_MODE_MASTER;
    hspi1.Init.Direction=SPI_DIRECTION_2LINES;hspi1.Init.DataSize=SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity=SPI_POLARITY_LOW;hspi1.Init.CLKPhase=SPI_PHASE_1EDGE;
    hspi1.Init.NSS=SPI_NSS_SOFT;hspi1.Init.BaudRatePrescaler=SPI_BAUDRATEPRESCALER_4;
    hspi1.Init.FirstBit=SPI_FIRSTBIT_MSB;hspi1.Init.TIMode=SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation=SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.CRCPolynomial=7;
    if (HAL_SPI_Init(&hspi1)!=HAL_OK) for(;;){}
    /* Bootloader 与 ESP 用 USART1 的 115200、8N1 传升级命令和固件块。 */
    huart1.Instance=USART1;huart1.Init.BaudRate=115200;
    huart1.Init.WordLength=UART_WORDLENGTH_8B;huart1.Init.StopBits=UART_STOPBITS_1;
    huart1.Init.Parity=UART_PARITY_NONE;huart1.Init.Mode=UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl=UART_HWCONTROL_NONE;
    if (HAL_UART_Init(&huart1)!=HAL_OK) for(;;){}
}
void ota_board_uart_line(const char *s) {
    /* s 是以 '\0' 结束的 C 字符串，strlen 不把结尾字节发出去。 */
    (void)HAL_UART_Transmit(&huart1,(uint8_t *)s,(uint16_t)strlen(s),1000);
}
void HAL_MspInit(void) {
    /* 开启底层复用时钟，并保留 SWD 调试接口。 */
    __HAL_RCC_AFIO_CLK_ENABLE();__HAL_RCC_PWR_CLK_ENABLE();
    __HAL_AFIO_REMAP_SWJ_NOJTAG();
}
void HAL_SPI_MspInit(SPI_HandleTypeDef *h) {
    if (h->Instance!=SPI1)return;
    /* PA5=SCK、PA7=MOSI 为复用输出；PA6=MISO 为输入。 */
    __HAL_RCC_SPI1_CLK_ENABLE();__HAL_RCC_GPIOA_CLK_ENABLE();
    GPIO_InitTypeDef p={0};p.Pin=GPIO_PIN_5|GPIO_PIN_7;
    p.Mode=GPIO_MODE_AF_PP;p.Speed=GPIO_SPEED_FREQ_HIGH;HAL_GPIO_Init(GPIOA,&p);
    p.Pin=GPIO_PIN_6;p.Mode=GPIO_MODE_INPUT;HAL_GPIO_Init(GPIOA,&p);
}
void HAL_UART_MspInit(UART_HandleTypeDef *h) {
    if (h->Instance!=USART1)return;
    /* PA9 发往 ESP RX，PA10 接收 ESP TX。 */
    __HAL_RCC_USART1_CLK_ENABLE();__HAL_RCC_GPIOA_CLK_ENABLE();
    GPIO_InitTypeDef p={0};p.Pin=GPIO_PIN_9;p.Mode=GPIO_MODE_AF_PP;
    p.Speed=GPIO_SPEED_FREQ_HIGH;HAL_GPIO_Init(GPIOA,&p);
    p.Pin=GPIO_PIN_10;p.Mode=GPIO_MODE_INPUT;HAL_GPIO_Init(GPIOA,&p);
}
/* Bootloader 不运行 RTOS，SysTick 直接驱动 HAL 的毫秒计数。 */
void SysTick_Handler(void) { HAL_IncTick(); }
#if defined(__CC_ARM)
/* Keil ARMCC 5：r0/r1 分别是 sp/pc，嵌入汇编不额外生成函数序言。 */
__asm static void start_app(uint32_t sp,uint32_t pc) {
    MSR MSP, r0
    CPSIE i
    BX r1
}
#elif defined(__GNUC__)
__attribute__((naked,noreturn)) static void start_app(
    uint32_t sp __attribute__((unused)),uint32_t pc __attribute__((unused))) {
    /* r0/r1 是 ARM 调用约定下的 sp/pc；设置主栈后跳到应用复位入口。 */
    __asm volatile("msr msp, r0\n"
                   "cpsie i\n"
                   "bx r1\n");
}
#else
#error "start_app requires Keil ARMCC or GCC"
#endif
void ota_board_jump_app(void) {
    /* Cortex-M 镜像开头两个字是初始栈指针和 Reset_Handler 地址。 */
    uint32_t sp=*(volatile uint32_t *)OTA_APP_BASE;
    uint32_t pc=*(volatile uint32_t *)(OTA_APP_BASE+4U);
    if (sp<0x20000000UL||sp>0x20005000UL||
        pc<OTA_APP_BASE||pc>=OTA_FLASH_END||!(pc&1U)) return;
    /* 跳转前停止 Bootloader 的中断/外设，避免它们在应用里继续触发。 */
    __disable_irq();
    HAL_UART_DeInit(&huart1);HAL_SPI_DeInit(&hspi1);HAL_DeInit();
    SysTick->CTRL=0;SysTick->LOAD=0;SysTick->VAL=0;
    for (unsigned i=0;i<8;++i) NVIC->ICER[i]=0xffffffffUL;
    for (unsigned i=0;i<8;++i) NVIC->ICPR[i]=0xffffffffUL;
    /* 应用从 0x08006000 链接，它的中断向量表也必须从此处读取。 */
    SCB->VTOR=OTA_APP_BASE;
    __DSB();__ISB();
    start_app(sp,pc);
}

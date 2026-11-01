#ifndef OTA_BOARD_H
#define OTA_BOARD_H
#include "stm32f1xx_hal.h"
/* Bootloader 使用 SPI1 连 W25、USART1 连 ESP；句柄供底层驱动使用。 */
extern SPI_HandleTypeDef hspi1;
extern UART_HandleTypeDef huart1;
/* 板级初始化、发送文字行、设置向量/栈并跳转到应用。 */
void ota_board_init(void);
void ota_board_uart_line(const char *line);
void ota_board_jump_app(void);
#endif

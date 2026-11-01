/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "OLED.h"
#include "gateway_protocol.h"
#include "flash_queue.h"
#include "modbus_master.h"
#include "ota_layout.h"
#include "ota_meta.h"
#include "ota_download.h"
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#ifndef GW_APP_VERSION
/* 首次烧录版本；发布新版时编译宏必须与发布清单的 VERSION 一致。 */
#define GW_APP_VERSION 10000UL
#endif
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
CAN_HandleTypeDef hcan;

SPI_HandleTypeDef hspi1;

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart3;

/* USER CODE BEGIN PV */
/* 两个任务共用同一颗 W25Q32；持锁期间才能访问外部 Flash。 */
static SemaphoreHandle_t flash_lock;
static uint8_t flash_ok;
/* volatile 让任务每次重新读取共享标志；Flash 的互斥仍由 flash_lock 保证。 */
static volatile uint32_t poll_interval_ms = 5000U;
static volatile uint8_t update_requested;
static volatile uint8_t poll_completed;
static uint8_t ota_testing;
static ota_meta_t ota_meta;
/* USART1 中断只入队；512 字节可容纳一条最长约 280 字节的 OTA 数据行。 */
static uint8_t esp_rx_byte;
static volatile uint8_t esp_rx_ring[512];
static volatile uint16_t esp_rx_head;
static volatile uint16_t esp_rx_tail;
static volatile uint8_t esp_rx_fault;
/* CAN 联调阶段只接收 ID 0x301；Keil Watch 可查看最近值和收帧次数。 */
volatile uint16_t can_last_value;
volatile uint16_t can_last_sequence;
volatile uint32_t can_rx_count;
volatile uint32_t can_last_tick;
volatile uint32_t can_init_ok;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_CAN_Init(void);
static void MX_SPI1_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART3_UART_Init(void);
/* USER CODE BEGIN PFP */
static void BootBlink(uint8_t times);
static uint8_t OTA_StartupCheck(void);
static void SendVersion(void);
static uint32_t parse_ack(const char *s);
static uint32_t parse_poll(const char *s);
static void PollTask(void *argument);
static void BridgeTask(void *argument);
static uint8_t CAN_GatewayStart(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* CAN 外设本体已由生成区 MX_CAN_Init 配好（PA11/PA12、250 kbit/s）。
   这里只做 CubeMX 不生成的三件事：过滤器、启动、开接收中断。
   不加过滤器：FIFO 会收到总线上所有帧；不 Start：收发不了；
   不开通知：收不到中断，回调永远不会被调用。 */
static uint8_t CAN_GatewayStart(void) {
    CAN_FilterTypeDef filter = {0};
    /* 32 位掩码模式：只放行标准数据帧 0x301。
       FilterIdHigh = 0x301<<5：标准 ID 占过滤寄存器 31:21 位；
       FilterMaskIdLow = 6：IDE、RTR 两位也参与匹配（必须是标准帧 + 数据帧）。 */
    filter.FilterBank = 0;
    filter.FilterMode = CAN_FILTERMODE_IDMASK;
    filter.FilterScale = CAN_FILTERSCALE_32BIT;
    filter.FilterIdHigh = (uint16_t)(0x301U << 5);
    filter.FilterIdLow = 0x0000U;
    filter.FilterMaskIdHigh = (uint16_t)(0x7FFU << 5);
    filter.FilterMaskIdLow = 0x0006U;
    filter.FilterFIFOAssignment = CAN_RX_FIFO0;
    filter.FilterActivation = ENABLE;
    filter.SlaveStartFilterBank = 14;
    if (HAL_CAN_ConfigFilter(&hcan, &filter) != HAL_OK) return 0U;
    if (HAL_CAN_Start(&hcan) != HAL_OK) return 0U;
    /* 打开 FIFO0 收到消息中断，收到帧后进 HAL_CAN_RxFifo0MsgPendingCallback。 */
    if (HAL_CAN_ActivateNotification(&hcan, CAN_IT_RX_FIFO0_MSG_PENDING) != HAL_OK) return 0U;
    return 1U;
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
  /* 应用从 0x08006000 启动，中断向量也必须切到这个地址。 */
  SCB->VTOR = OTA_APP_BASE;
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_CAN_Init();
  MX_SPI1_Init();
  MX_USART1_UART_Init();
  MX_USART3_UART_Init();
  /* USER CODE BEGIN 2 */
  can_init_ok = CAN_GatewayStart();
  if (!can_init_ok) Error_Handler();
  /* 三灯快闪：不依赖屏幕和任何时间基准，先证明 App 真的跳进来了。 */
  BootBlink(1);
  /* 屏幕放在串口之前初始化，避免串口出错时连屏都点不亮。 */
  OLED_Init();
  /* 常显正在运行的 App 编译版本，升级后可直接从屏幕核对。 */
  OLED_ShowString(1, 1, "V");
  OLED_ShowNum(1, 2, GW_APP_VERSION, 5);
  OLED_ShowString(1, 7, " C:ERR    ");
  OLED_ShowString(2, 1, "ID1:----- WAIT  ");
  OLED_ShowString(3, 1, "ID2:----- WAIT  ");
  OLED_ShowString(4, 1, "INIT...         ");
  /* 屏没有应答时再快闪三下，用来区分“接线/供电问题”和“程序没跑”。 */
  if (OLED_I2C_Nack) BootBlink(3);
  if (HAL_UART_Receive_IT(&huart1, &esp_rx_byte, 1) != HAL_OK)
  {
    OLED_ShowString(4, 1, "UART1 ERR       ");
    Error_Handler();
  }
  flash_ok = FlashQueue_Init();
  if (!flash_ok)
  {
    OLED_ShowString(4, 1, "FLASH ERR       ");
    HAL_GPIO_WritePin(LED3_GPIO_Port, LED3_Pin, GPIO_PIN_SET);
  }
  if (flash_ok && !OTA_StartupCheck()) NVIC_SystemReset();

  /* 栈单位为 32 位字；桥接任务用 768 字（3 KiB）容纳下载行、SHA 和 HAL 调用。 */
  flash_lock = xSemaphoreCreateMutex();
  if (!flash_lock ||
      xTaskCreate(PollTask, "poll", 256, 0, 2, 0) != pdPASS ||
      xTaskCreate(BridgeTask, "bridge", 768, 0, 1, 0) != pdPASS) Error_Handler();
  vTaskStartScheduler();
  Error_Handler();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 19200;
  huart3.Init.WordLength = UART_WORDLENGTH_9B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_EVEN;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, LED1_Pin|LED2_Pin|LED3_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, FLASH_CS_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOB, OLEDSCL_Pin|OLEDSDA_Pin, GPIO_PIN_SET);

  /*Configure GPIO pins : LED1_Pin LED2_Pin LED3_Pin */
  GPIO_InitStruct.Pin = LED1_Pin|LED2_Pin|LED3_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : FLASH_CS_Pin */
  GPIO_InitStruct.Pin = FLASH_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : OLEDSCL_Pin OLEDSDA_Pin */
  GPIO_InitStruct.Pin = OLEDSCL_Pin|OLEDSDA_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/**
  * @brief CAN Initialization Function
  * @param None
  * @retval None
  */
static void MX_CAN_Init(void)
{

  /* USER CODE BEGIN CAN_Init 0 */

  /* USER CODE END CAN_Init 0 */

  /* USER CODE BEGIN CAN_Init 1 */

  /* USER CODE END CAN_Init 1 */
  hcan.Instance = CAN1;
  hcan.Init.Prescaler = 9;
  hcan.Init.Mode = CAN_MODE_NORMAL;
  hcan.Init.SyncJumpWidth = CAN_SJW_1TQ;
  hcan.Init.TimeSeg1 = CAN_BS1_13TQ;
  hcan.Init.TimeSeg2 = CAN_BS2_2TQ;
  hcan.Init.TimeTriggeredMode = DISABLE;
  hcan.Init.AutoBusOff = ENABLE;
  hcan.Init.AutoWakeUp = DISABLE;
  hcan.Init.AutoRetransmission = ENABLE;
  hcan.Init.ReceiveFifoLocked = DISABLE;
  hcan.Init.TransmitFifoPriority = DISABLE;
  if (HAL_CAN_Init(&hcan) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN CAN_Init 2 */

  /* USER CODE END CAN_Init 2 */
}

/* USER CODE BEGIN 4 */
/* 收帧回调：HAL_CAN_IRQHandler 在 FIFO0 有消息时调用这里。
   只接受标准数据帧 0x301，且数据长度必须为 4 字节。 */
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *phcan)
{
    CAN_RxHeaderTypeDef header;
    uint8_t data[8];
    /* FIFO 里可能堆着多帧，因此循环取完。 */
    while (HAL_CAN_GetRxFifoFillLevel(phcan, CAN_RX_FIFO0) > 0U) {
        if (HAL_CAN_GetRxMessage(phcan, CAN_RX_FIFO0, &header, data) != HAL_OK)
            break;
        if (header.IDE == CAN_ID_STD && header.StdId == 0x301U &&
            header.RTR == CAN_RTR_DATA && header.DLC == 4U) {
            /* 前两个字节组成数值，后两个字节组成序号，均为小端序。 */
            uint32_t d = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                         ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
            can_last_value = (uint16_t)d;
            can_last_sequence = (uint16_t)(d >> 16);
            can_last_tick = HAL_GetTick();
            can_rx_count++;
        }
    }
}

/* 上电自检：三灯同闪，不依赖 OLED、HAL_Delay 和 RTOS，纯 NOP 延时。 */
static void BootBlink(uint8_t times) {
    uint8_t n;
    uint32_t i;
    for (n = 0; n < times; ++n) {
        HAL_GPIO_WritePin(GPIOB, LED1_Pin | LED2_Pin | LED3_Pin, GPIO_PIN_SET);
        for (i = 0; i < 400000U; ++i) __NOP();
        HAL_GPIO_WritePin(GPIOB, LED1_Pin | LED2_Pin | LED3_Pin, GPIO_PIN_RESET);
        for (i = 0; i < 400000U; ++i) __NOP();
    }
}

static uint8_t OTA_StartupCheck(void) {
    /* W25 尚无有效元数据，按首次烧录初始化为当前稳定版本。 */
    if (!ota_meta_load(&ota_meta)) {
        ota_meta.version = GW_APP_VERSION;
        return ota_meta_save(&ota_meta);
    }
    if (ota_meta.state == OTA_STATE_IDLE) {
        /* Keil 直接下载新版 App 时，同步稳定版本，供下次 OTA 回退使用。 */
        if (ota_meta.version != GW_APP_VERSION) {
            ota_meta.version = GW_APP_VERSION;
            return ota_meta_save(&ota_meta);
        }
        return 1;
    }
    /* 只有待确认版本与当前固件相同，才允许新版试运行。 */
    if (ota_meta.state != OTA_STATE_TESTING ||
        ota_meta.pending_version != GW_APP_VERSION) return 0;
    ota_testing = 1;
    return 1;
}

static void SendVersion(void) {
    char line[24], digits[10];
    uint32_t version = GW_APP_VERSION;
    uint8_t used = 8, count = 0;
    /* 先写固定前缀；下面用除 10 取余把整数拆成逆序数字。 */
    memcpy(line, "VERSION ", 8);
    do { digits[count++] = (char)('0' + version % 10U); version /= 10U; }
    while (version && count < sizeof(digits));
    /* 倒着取出数字，得到正常顺序的十进制字符串。 */
    while (count) line[used++] = digits[--count];
    line[used++] = '\n';
    (void)HAL_UART_Transmit(&huart1, (uint8_t *)line, used, 100);
}

static uint32_t parse_ack(const char *s) {
    /* 仅接受 ACK:<十进制序号>；返回 0 表示格式错误。 */
    if (s[0] != 'A' || s[1] != 'C' || s[2] != 'K' || s[3] != ':') return 0;
    uint32_t value = 0; size_t digits = 0;
    /* OTA 复用了更长的行缓冲，索引不能在第 256 个字符处回绕。 */
    for (size_t i = 4; s[i] != '\0'; ++i) {
        /* 先检查字符和乘 10 前的上限，再检查相加后的溢出。 */
        if (s[i] < '0' || s[i] > '9' || value > 429496729U) return 0;
        uint32_t next = value * 10U + (uint32_t)(s[i] - '0');
        if (next < value) return 0;
        value = next; ++digits;
    }
    return digits ? value : 0;
}

static uint32_t parse_poll(const char *s) {
    const char prefix[] = "SET:POLL:";
    /* sizeof 包括末尾的 '\0'，因此前缀长度要减 1。 */
    for (uint8_t i = 0; i < sizeof(prefix) - 1U; ++i)
        if (s[i] != prefix[i]) return 0;
    uint32_t value = 0; size_t digits = 0;
    for (size_t i = sizeof(prefix) - 1U; s[i] != '\0'; ++i) {
        /* 只允许 2000～60000 ms，避免无意义的过快或过慢采样。 */
        if (s[i] < '0' || s[i] > '9' || value > 60000U) return 0;
        value = value * 10U + (uint32_t)(s[i] - '0'); ++digits;
    }
    return digits && value >= 2000U && value <= 60000U ? value : 0;
}

/* USART3 和 OLED 只由轮询任务使用，避免两个任务同时操作。 */
static void PollTask(void *argument) {
    (void)argument;
    /* 用上次计划唤醒时间维持固定轮询周期。 */
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        /* 镜像完整并尝试提交后才暂停采样，等 BridgeTask 复位进 Bootloader。 */
        if (update_requested) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        /* 清零使读取失败的从站保持无效状态，不沿用上一轮数值。 */
        GW_Sample sample = {0};
        /* 读取地址 1/2、寄存器 0；status 的 bit0/bit1 表示读数有效。 */
        if (Modbus_ReadRegister(1, 0, &sample.slave1_value)) sample.status |= 1U;
        if (Modbus_ReadRegister(2, 0, &sample.slave2_value)) sample.status |= 2U;
        sample.uptime_ms = HAL_GetTick();
        /* 超过 3 秒没有 CAN 新帧时不沿用旧值；bit2 表示 CAN 有效。 */
        if (can_rx_count && (uint32_t)(sample.uptime_ms - can_last_tick) <= 3000U) {
            sample.can_value = can_last_value;
            sample.status |= 4U;
        }
        uint16_t displayed_can = (sample.status & 4U) ? sample.can_value : 0xffffU;
        OLED_ShowString(1, 7, " C:ERR    ");
        if (sample.status & 4U) {
            OLED_ShowNum(1, 10, sample.can_value, 5);
            OLED_ShowString(1, 15, "  ");
        }
        HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin,
            ((sample.status & 3U) == 3U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        /* 先写 ERR 占位，再用成功读到的值覆盖，防止屏幕留旧数据。 */
        OLED_ShowString(2, 1, "ID1:----- ERR   ");
        if (sample.status & 1U) {
            OLED_ShowNum(2, 5, sample.slave1_value, 5);
            OLED_ShowString(2, 11, "OK  ");
        }
        OLED_ShowString(3, 1, "ID2:----- ERR   ");
        if (sample.status & 2U) {
            OLED_ShowNum(3, 5, sample.slave2_value, 5);
            OLED_ShowString(3, 11, "OK  ");
        }
        if (!flash_ok) {
            /* SPI 传输失败时 ID 保持 000000；读到全 0 的芯片也会显示相同结果。 */
            OLED_ShowString(4, 1, "F:      ERR    ");
            OLED_ShowHexNum(4, 3, FlashQueue_JedecId(), 6);
        } else {
            OLED_ShowString(4, 1, "P:");
            OLED_ShowNum(4, 3, poll_interval_ms / 1000U, 2);
            OLED_ShowString(4, 5, "S FLASH:... ");
        }
        /* 锁住 SPI/片选；等锁期间再次检查 OTA 标志。 */
        if (flash_ok && !update_requested &&
            xSemaphoreTake(flash_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
            uint8_t stored = update_requested ? 0 : FlashQueue_Append(&sample);
            HAL_GPIO_WritePin(LED3_GPIO_Port, LED3_Pin,
                stored ? GPIO_PIN_RESET : GPIO_PIN_SET);
            if (stored) OLED_ShowString(4, 13, "OK  ");
            else {
                if (flash_queue_append_error == 7U) {
                    /* I=字节索引，W=预期写入值，R=回读值，均为十六进制。 */
                    OLED_ShowString(4, 1, "E7 I00 W00 R00  ");
                    OLED_ShowHexNum(4, 5, flash_queue_verify_index, 2);
                    OLED_ShowHexNum(4, 9, flash_queue_verify_expected, 2);
                    OLED_ShowHexNum(4, 13, flash_queue_verify_actual, 2);
                } else {
                    OLED_ShowString(4, 1, "FLASH WRITE E:  ");
                    OLED_ShowNum(4, 15, flash_queue_append_error, 2);
                }
            }
            xSemaphoreGive(flash_lock);
        }
        /* 已完成一轮轮询，新安装应用可据此确认试运行。 */
        poll_completed = 1;
        /* 等待下轮 Modbus 时只刷新 CAN 显示，不增加采样或 Flash 写入频率。 */
        TickType_t next_poll = wake + pdMS_TO_TICKS(poll_interval_ms);
        while (!update_requested && (int32_t)(next_poll - xTaskGetTickCount()) > 0) {
            uint32_t now = HAL_GetTick();
            uint16_t current_can = (can_rx_count &&
                (uint32_t)(now - can_last_tick) <= 3000U) ? can_last_value : 0xffffU;
            if (current_can != displayed_can) {
                if (current_can == 0xffffU) OLED_ShowString(1, 7, " C:ERR    ");
                else {
                    OLED_ShowNum(1, 10, current_can, 5);
                    OLED_ShowString(1, 15, "  ");
                }
                displayed_can = current_can;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        wake = update_requested ? xTaskGetTickCount() : next_poll;
    }
}

/* 下载器在桥接任务里运行；每次只锁住一个扇区或块，不暂停 PollTask。 */
void ota_download_lock(void) { (void)xSemaphoreTake(flash_lock,portMAX_DELAY); }
void ota_download_unlock(void) { xSemaphoreGive(flash_lock); }
void ota_download_pause(void) { vTaskDelay(pdMS_TO_TICKS(1)); }
uint32_t ota_download_now(void) { return HAL_GetTick(); }
void ota_download_reply(const char *line) {
    (void)HAL_UART_Transmit(&huart1,(uint8_t *)line,(uint16_t)strlen(line),100);
}

/* 上行兼有 GW 遥测帧和 OTA 文本回复；下行命令统一按换行符分帧。 */
static void BridgeTask(void *argument) {
    (void)argument;
    uint8_t frame[GW_FRAME_SIZE],byte,dropping=0;
    char line[304];uint16_t used=0;
    uint32_t last_tx=0,last_sent_seq=0,line_tick=0;
    for(;;) {
        if(esp_rx_fault) {
            /* 溢出/硬件串口错误时整条命令作废，不能把残包当成新命令。 */
            esp_rx_fault=0;esp_rx_tail=esp_rx_head;used=0;dropping=1;
            line_tick=HAL_GetTick();
            if(ota_download_active())ota_download_abort();
        }
        if((used||dropping)&&HAL_GetTick()-line_tick>2000U) {used=0;dropping=0;}
        while(esp_rx_tail!=esp_rx_head) {
            byte=esp_rx_ring[esp_rx_tail];
            esp_rx_tail=(uint16_t)((esp_rx_tail+1U)&511U);
            line_tick=HAL_GetTick();
            if(byte=='\n') {
                if(!dropping) {
                    line[used]=0;
                    int handled=0;
                    if(flash_ok)handled=ota_download_line(line,&ota_meta);
                    else if(strncmp(line,"START ",6)==0) {ota_download_reply("REJECT\n");handled=1;}
                    /* 只有完整镜像已经校验并尝试提交后，才进入复位分支。 */
                    if(handled==2) {update_requested=1;break;}
                    if(!handled&&!ota_download_active()) {
                        uint32_t ack=parse_ack(line),interval=parse_poll(line);
                        if(strcmp(line,"VERSION")==0)SendVersion();
                        if(strcmp(line,"UPDATE")==0)ota_download_reply("REJECT\n");
                        if(interval)poll_interval_ms=interval;
                        if(ack&&ack==last_sent_seq&&flash_ok&&
                           xSemaphoreTake(flash_lock,pdMS_TO_TICKS(1000))==pdTRUE) {
                            (void)FlashQueue_Ack(ack);xSemaphoreGive(flash_lock);last_tx=0;
                        }
                    }
                }
                used=0;dropping=0;
            } else if(byte!='\r'&&!dropping) {
                if(byte>=32&&byte<=126&&used<sizeof(line)-1U)line[used++]=(char)byte;
                else {used=0;dropping=1;} /* 丢弃到下一个换行，保证重传能重新同步。 */
            }
        }
        ota_download_poll();
        if(update_requested) {
            /* 下载期间采样持续运行；此时才等最后一次队列写结束并重启。 */
            (void)xSemaphoreTake(flash_lock,portMAX_DELAY);
            ota_download_reply("REBOOT\n");vTaskDelay(pdMS_TO_TICKS(100));NVIC_SystemReset();
        }
        if(ota_testing&&poll_completed&&
           xSemaphoreTake(flash_lock,pdMS_TO_TICKS(100))==pdTRUE) {
            ota_meta.version=GW_APP_VERSION;ota_meta.pending_version=0;ota_meta.state=OTA_STATE_IDLE;
            uint8_t confirmed=ota_meta_save(&ota_meta);xSemaphoreGive(flash_lock);
            if(!confirmed)NVIC_SystemReset();
            ota_testing=0;
        }
        /* 下载暂占 ESP 串口，停止遥测重发；PollTask 仍将新采样存入原队列。 */
        uint32_t now=HAL_GetTick();
        if(!ota_download_active()&&flash_ok&&(last_tx==0||now-last_tx>=3000U)&&
           xSemaphoreTake(flash_lock,pdMS_TO_TICKS(100))==pdTRUE) {
            if(FlashQueue_Oldest(frame)) {
                last_sent_seq=(uint32_t)frame[4]|((uint32_t)frame[5]<<8)|
                              ((uint32_t)frame[6]<<16)|((uint32_t)frame[7]<<24);
                (void)HAL_UART_Transmit(&huart1,frame,GW_FRAME_SIZE,100);
                HAL_GPIO_TogglePin(LED2_GPIO_Port,LED2_Pin);
            }
            xSemaphoreGive(flash_lock);last_tx=now;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *uart) {
    if (uart->Instance != USART1) return;
    /* 单生产者（中断）/单消费者（任务）环形缓冲，满时舍弃新字节。 */
    uint16_t next = (uint16_t)((esp_rx_head + 1U) & 511U);
    if (next != esp_rx_tail) {
        esp_rx_ring[esp_rx_head] = esp_rx_byte;
        esp_rx_head = next;
    } else esp_rx_fault = 1;
    (void)HAL_UART_Receive_IT(&huart1, &esp_rx_byte, 1);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart) {
    if (uart->Instance != USART1) return;
    /* 清溢出并重新开启 1 字节接收，避免通信一次错误后永久停收。 */
    esp_rx_fault = 1;
    __HAL_UART_CLEAR_OREFLAG(&huart1);
    (void)HAL_UART_Receive_IT(&huart1, &esp_rx_byte, 1);
}
/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM4 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM4)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

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

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
CAN_HandleTypeDef hcan;

/* USER CODE BEGIN PV */
/* Keil Watch 可查看这几个量：成功发送次数、错误次数、CAN 错误寄存器。 */
volatile uint32_t can_tx_ok_count;
volatile uint32_t can_tx_error_count;
volatile uint32_t can_last_esr;
volatile uint32_t can_init_ok;
/* 上电内部回环自检结果：1 表示芯片内部发送成功，不代表外部总线已通。 */
volatile uint32_t can_loopback_ok;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_CAN_Init(void);
static void MX_GPIO_Init(void);
/* USER CODE BEGIN PFP */
static uint8_t CAN_NodeSelfTest(void);
static void CAN_NodeSend(uint16_t value, uint16_t sequence);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* 回环自检：把 CAN 切到“静默回环”模式发一帧，检查芯片内部收发通路是否正常。
   为什么需要：外部总线没接好时，先把“芯片自身没问题”和“外面没 ACK”两件事分开。
   不驱动外部 CAN 线，也不需要另一个节点应答，所以单板就能自检。
   测完必须切回正常模式，否则后面 0x301 发不到外部总线上。 */
static uint8_t CAN_NodeSelfTest(void)
{
  uint32_t start;

  /* 切静默回环模式再初始化一次。
     hcan 是生成区的全局句柄，这里只改 Mode 字段重新 HAL_CAN_Init，
     其余时序参数（Prescaler=9、BS1=13、BS2=2）沿用 MX_CAN_Init 的配置。
     不改的话 CAN 一直工作在正常模式，单板自检永远等不到 ACK。 */
  hcan.Init.Mode = CAN_MODE_SILENT_LOOPBACK;
  if (HAL_CAN_Init(&hcan) != HAL_OK)
    return 0U;

  /* HAL_CAN_Start 让 CAN 离开初始化模式、真正开始收发。
     不调用它，AddTxMessage 只会把帧放进邮箱但永远发不出去。 */
  if (HAL_CAN_Start(&hcan) != HAL_OK)
    return 0U;

  /* 发一帧测试报文，然后等邮箱 0 的“请求完成”标志 RQCP0。
     回环模式下 ACK 由芯片内部自己应答，100 ms 内必出结果。 */
  CAN_NodeSend(0x1234U, 0x5678U);
  start = HAL_GetTick();
  while (__HAL_CAN_GET_FLAG(&hcan, CAN_FLAG_RQCP0) == 0U)
    if ((HAL_GetTick() - start) > 100U)
      break;

  /* TXOK0=1 表示内部发送成功；失败也要把邮箱释放掉再继续。 */
  can_loopback_ok = (__HAL_CAN_GET_FLAG(&hcan, CAN_FLAG_TXOK0) != 0U) ? 1U : 0U;
  if (can_loopback_ok == 0U)
    HAL_CAN_AbortTxRequest(&hcan, CAN_TX_MAILBOX0); /* 超时也要释放发送邮箱。 */
  __HAL_CAN_CLEAR_FLAG(&hcan, CAN_FLAG_RQCP0);      /* 清完成标志，避免下次循环误判。 */

  /* 自检后切回正常模式，使后续 0x301 帧发到外部总线。 */
  hcan.Init.Mode = CAN_MODE_NORMAL;
  if (HAL_CAN_Init(&hcan) != HAL_OK)
    return 0U;
  return (HAL_CAN_Start(&hcan) == HAL_OK) ? 1U : 0U;
}

/* 发送标准帧 0x301：4 字节依次为数值低/高字节、序号低/高字节。
   HAL_CAN_AddTxMessage 会自动选择空闲发送邮箱。 */
static void CAN_NodeSend(uint16_t value, uint16_t sequence)
{
  CAN_TxHeaderTypeDef header = {0};
  uint32_t mailbox;
  uint8_t data[4];

  header.StdId = 0x301U;       /* 标准帧 ID，和 C8 网关过滤器约定一致。 */
  header.IDE = CAN_ID_STD;     /* 标准帧（11 位 ID），不是扩展帧。 */
  header.RTR = CAN_RTR_DATA;   /* 数据帧，不是远程帧。 */
  header.DLC = 4U;             /* 4 字节数据。 */
  header.TransmitGlobalTime = DISABLE;

  data[0] = (uint8_t)(value & 0xFFU);
  data[1] = (uint8_t)(value >> 8);
  data[2] = (uint8_t)(sequence & 0xFFU);
  data[3] = (uint8_t)(sequence >> 8);

  HAL_CAN_AddTxMessage(&hcan, &header, data, &mailbox);
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
  uint16_t sequence = 0U;
  uint32_t last_send;

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
  /* USER CODE BEGIN 2 */
  /* 先自检（静默回环），通过后 CAN_NodeSelfTest 内部已切回正常模式并 Start。 */
  can_init_ok = CAN_NodeSelfTest();
  if (can_init_ok == 0U)
    Error_Handler();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  last_send = HAL_GetTick();
  while (1)
  {
    /* ESR 是 CAN 错误寄存器（TEC/REC/LEC），Watch 里观察总线错误类型用。 */
    can_last_esr = hcan.Instance->ESR;

    /* 只在三个邮箱全空时发送：HAL_CAN_AddTxMessage 自动挑编号最小的空邮箱，
       三空则必选邮箱 0，所以主循环固定查邮箱 0 的 RQCP0/TXOK0 就不会漏。
       RQCP0=邮箱 0 请求完成（成功、出错、仲裁失败都会置位）。 */
    if (__HAL_CAN_GET_FLAG(&hcan, CAN_FLAG_RQCP0) != 0U)
    {
      if (__HAL_CAN_GET_FLAG(&hcan, CAN_FLAG_TXOK0) != 0U)
      {
        /* 发送成功只更新计数；灯的状态由定时翻转决定。 */
        can_tx_ok_count++;
        sequence++;
      }
      else
      {
        /* 发送失败（无 ACK、位错误等）：只计错误数，数据不变下次重发。 */
        can_tx_error_count++;
      }
      __HAL_CAN_CLEAR_FLAG(&hcan, CAN_FLAG_RQCP0); /* 写 1 清零，不清会一直进这个分支。 */
    }
    /* PB12 每秒翻转一次；CAN 的数值 0/1 就是此刻的引脚电平。 */
    if ((HAL_GetTick() - last_send) >= 1000U)
    {
      last_send = HAL_GetTick();
      HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_12);
      if (HAL_CAN_GetTxMailboxesFreeLevel(&hcan) == 3U)
        CAN_NodeSend((uint16_t)HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_12), sequence);
    }
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
  * @brief CAN Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  __HAL_RCC_GPIOB_CLK_ENABLE();
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_RESET);
  GPIO_InitStruct.Pin = GPIO_PIN_12;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
}

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

/* USER CODE END 4 */

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

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
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef struct {
    uint32_t magic;    // tờ chứng nhận
    uint32_t size;
    uint32_t crc;      // CRC16 app
} BootInfo;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define FLAG_ADDR    0x0800FC00UL   // page cuoi cung
#define FLAG_MAGIC   0xB007CAFEUL   // cờ nhận dạng tờ chứng nhận
#define APP_ADDR     0x08004000UL
#define APP_MAX      0x4000UL
#define PAGE_SIZE    1024UL

#define PKT_START    0xAA
#define CMD_START    0x01
#define CMD_DATA     0x02
#define CMD_END      0x03
#define BL_ACK       0x06
#define BL_NACK      0x15
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef huart1;

/* USER CODE BEGIN PV */
static uint8_t  pkt_data[256];
static uint32_t fw_size    = 0;
static uint32_t write_addr = APP_ADDR;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART1_UART_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static void uart_print(const char *s)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)s, strlen(s), 100);
}

static void send_reply(uint8_t r)
{
    HAL_UART_Transmit(&huart1, &r, 1, 100);
}

static uint16_t crc16(const uint8_t *d, uint32_t len, uint16_t crc)
{
    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= d[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
    return crc;
}

static HAL_StatusTypeDef flash_erase(uint32_t addr, uint32_t pages)
{
    FLASH_EraseInitTypeDef e = {0};
    uint32_t err;

    e.TypeErase   = FLASH_TYPEERASE_PAGES;
    e.PageAddress = addr;
    e.NbPages     = pages;

    HAL_FLASH_Unlock();
    HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&e, &err);
    HAL_FLASH_Lock();
    return st;
}

static HAL_StatusTypeDef flash_write(uint32_t addr, const uint8_t *data, uint16_t len)
{
    HAL_FLASH_Unlock();
    for (uint16_t i = 0; i < len; i += 2)
    {
        uint16_t hw = data[i] | ((i + 1 < len ? data[i + 1] : 0xFF) << 8);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr + i, hw) != HAL_OK)
        {
            HAL_FLASH_Lock();
            return HAL_ERROR;
        }
    }
    HAL_FLASH_Lock();
    return HAL_OK;
}

static int app_is_valid(void)
{
    const BootInfo *info = (const BootInfo *)FLAG_ADDR;

    if (info->magic != FLAG_MAGIC) return 0;                      // chưa có chứng nhận
    if (info->size == 0 || info->size > APP_MAX) return 0;

    // tinh va so crc
    return crc16((const uint8_t *)APP_ADDR, info->size, 0xFFFF) == (uint16_t)info->crc;
}

// Ghi tờ chứng nhận sau khi update thành công
static HAL_StatusTypeDef mark_app_valid(uint32_t size, uint16_t crc)
{
    BootInfo info = { FLAG_MAGIC, size, crc };
    return flash_write(FLAG_ADDR, (const uint8_t *)&info, sizeof(info));
}

static void jump_to_app(uint32_t addr)
{
    uint32_t sp  = *(volatile uint32_t *)addr;
    uint32_t rst = *(volatile uint32_t *)(addr + 4);

    if ((sp & 0x2FFE0000) != 0x20000000) return;

    __disable_irq();
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;
    SCB->VTOR = addr;
    __set_MSP(sp);
    __enable_irq();

    ((void (*)(void))rst)();
}

// 0 nếu nhận đủ gói và CRC đúng, -1 nếu lỗi
static int receive_packet(uint8_t *cmd, uint16_t *len)
{
    uint8_t b, hdr[3], c[2];

    // Đợi start byte
    do {
        if (HAL_UART_Receive(&huart1, &b, 1, HAL_MAX_DELAY) != HAL_OK) return -1;
    } while (b != PKT_START);

    if (HAL_UART_Receive(&huart1, hdr, 3, 500) != HAL_OK) return -1;  // CMD, LEN_L, LEN_H
    *cmd = hdr[0];
    *len = hdr[1] | (hdr[2] << 8);
    if (*len > sizeof(pkt_data)) return -1;

    if (*len && HAL_UART_Receive(&huart1, pkt_data, *len, 1000) != HAL_OK) return -1;
    if (HAL_UART_Receive(&huart1, c, 2, 500) != HAL_OK) return -1;

    uint16_t rx   = c[0] | (c[1] << 8);
    uint16_t calc = crc16(pkt_data, *len, crc16(hdr, 3, 0xFFFF));
    return (rx == calc) ? 0 : -1;
}

static void update_mode(void)
{
    uint8_t cmd;
    uint16_t len;

    uart_print("BOOT\r\n");

    while (1)
    {
        if (receive_packet(&cmd, &len) != 0) { send_reply(BL_NACK); continue; }

        switch (cmd)
        {
        case CMD_START:
            if (len != 4) { send_reply(BL_NACK); break; }
            fw_size = pkt_data[0] | (pkt_data[1] << 8) | (pkt_data[2] << 16)
                    | ((uint32_t)pkt_data[3] << 24);
            if (fw_size == 0 || fw_size > APP_MAX) { fw_size = 0; send_reply(BL_NACK); break; }

            write_addr = APP_ADDR;

            // Xóa tờ chứng nhận TRƯỚC khi xóa app. Mất điện giữa chừng thì không còn chứng nhận
            if (flash_erase(FLAG_ADDR, 1) != HAL_OK) { fw_size = 0; send_reply(BL_NACK); break; }

            send_reply(flash_erase(APP_ADDR, (fw_size + PAGE_SIZE - 1) / PAGE_SIZE) == HAL_OK
                       ? BL_ACK : BL_NACK);
            break;

        case CMD_DATA:
            if (fw_size == 0 || write_addr + len > APP_ADDR + fw_size ||
                flash_write(write_addr, pkt_data, len) != HAL_OK)
            {
                send_reply(BL_NACK);
                break;
            }
            write_addr += len;
            send_reply(BL_ACK);
            break;

        case CMD_END:
            if (len == 2 && fw_size != 0 && write_addr == APP_ADDR + fw_size)
            {
                uint16_t expect = pkt_data[0] | (pkt_data[1] << 8);
                if (crc16((uint8_t *)APP_ADDR, fw_size, 0xFFFF) == expect)
                {
                    // CRC đúng: xóa trang cờ (phòng khi gửi END hai lần) rồi ghi chứng nhận
                    if (flash_erase(FLAG_ADDR, 1) == HAL_OK &&
                        mark_app_valid(fw_size, expect) == HAL_OK)
                    {
                        send_reply(BL_ACK);
                        break;
                    }
                }
            }
            send_reply(BL_NACK);
            break;

        default:
            send_reply(BL_NACK);
            break;
        }
    }
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

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
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
  HAL_Delay(50);
  if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_0) == GPIO_PIN_SET && app_is_valid())
  {
      jump_to_app(APP_ADDR);   // chỉ nhảy khi không nhấn nút VÀ app có chứng nhận hợp lệ
  }
  update_mode();

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
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
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
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, GPIO_PIN_RESET);

  /*Configure GPIO pin : PA0 */
  GPIO_InitStruct.Pin = GPIO_PIN_0;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : PA1 */
  GPIO_InitStruct.Pin = GPIO_PIN_1;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
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

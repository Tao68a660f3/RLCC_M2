/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f1xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define LED_EN_Pin GPIO_PIN_1
#define LED_EN_GPIO_Port GPIOA
#define LED_LAT_Pin GPIO_PIN_4
#define LED_LAT_GPIO_Port GPIOA
#define LED_SCK_Pin GPIO_PIN_5
#define LED_SCK_GPIO_Port GPIOA
#define LED_DATA_Pin GPIO_PIN_7
#define LED_DATA_GPIO_Port GPIOA
#define LED_LA_Pin GPIO_PIN_0
#define LED_LA_GPIO_Port GPIOB
#define LED_LB_Pin GPIO_PIN_1
#define LED_LB_GPIO_Port GPIOB
#define LED_LC_Pin GPIO_PIN_10
#define LED_LC_GPIO_Port GPIOB
#define LED_LD_Pin GPIO_PIN_11
#define LED_LD_GPIO_Port GPIOB
#define W25QXX_CS_Pin GPIO_PIN_12
#define W25QXX_CS_GPIO_Port GPIOB
#define LCD_SCK_Pin GPIO_PIN_3
#define LCD_SCK_GPIO_Port GPIOB
#define LCD_SID_Pin GPIO_PIN_4
#define LCD_SID_GPIO_Port GPIOB
#define LCD_CS_Pin GPIO_PIN_5
#define LCD_CS_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */

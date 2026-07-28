/**
 * @file    app.c
 * @brief   应用主控模块，负责初始化所有驱动和主循环调度
 */
#include "app.h"
#include "bsp_rtc.h"
#include "lcd_driver.h"
#include "stm32f1xx_hal.h"
#include "w25q64.h"
#include <stdio.h>

extern RTC_HandleTypeDef hrtc;
extern SPI_HandleTypeDef hspi2;

void App_Init(void) {
  LCD_Init();
  LCD_WaitReady();
  LCD_ShowString(1, 1, "System Ready");

  W25Q64_Init(&hspi2);
  uint32_t id = W25Q64_ReadID();
  char buf[17];
  snprintf(buf, sizeof(buf), "W25Q ID:0x%06X", id);
  LCD_ShowString(2, 1, buf);

  RTC_App_Init(&hrtc);
}

void App_Loop(void) {
  static uint32_t last_tick = 0;
  LCD_Task_Handler();

  if (HAL_GetTick() - last_tick >= 1000) {
    RTC_DateTimeTypeDef dt;
    if (RTC_GetDateTime(&hrtc, &dt)) {
      char buf[18];
      snprintf(buf, sizeof(buf), "RTC: %02d:%02d:%02d", dt.hours, dt.minutes,
               dt.seconds);
      LCD_ShowString(1, 1, buf);
    }
    last_tick = HAL_GetTick();
  }
}
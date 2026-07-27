/**
 * @file    app.c
 * @brief   应用主控模块，负责初始化所有驱动和主循环调度
 */
#include "app.h"
#include "lcd_driver.h"
#include "lcd_test.h"
#include "stm32f103xb.h"
#include "stm32f1xx_hal.h"
#include "stm32f1xx_hal_gpio.h"

void App_Init(void) { LCD_TestAll(); }

void App_Loop(void) {
  LCD_Task_Handler();
  LCD_TestTask();
}

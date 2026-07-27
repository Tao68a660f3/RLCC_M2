/**
 * @file    lcd_test.c
 * @brief   LCD 显示功能测试（在 App_Loop 中运行，确保异步队列正常处理）
 */
#include "lcd_driver.h"
#include <string.h>

static uint8_t test_stage = 0;
static uint32_t test_tick = 0;

const char a[] = {0xCF, 0xD4, 0xCA, 0xBE, 0xD6, 0xD0, 0xCE,
                  0xC4, 0xD7, 0xD6, 0xB7, 0xFB, 0x00};
const char b[] = {0x4C, 0x43, 0x44, 0xB2, 0xE2, 0xCA, 0xD4,
                  0xCD, 0xEA, 0xB3, 0xC9, 0xA3, 0xA1, 0x00};
const char c[] = {0xCF, 0xB5, 0xCD, 0xB3, 0xD5, 0xFD, 0xB3,
                  0xA3, 0xD4, 0xCB, 0xD0, 0xD0, 0x00};

void LCD_TestAll(void) {
  /* 仅做初始化，显示内容在 LCD_TestTask 中分步执行 */
  LCD_Init();
  test_stage = 1;
  test_tick = HAL_GetTick();
}

void LCD_TestTask(void) {
  switch (test_stage) {

  case 1: /* 显示英文和中文字符串 */
    LCD_ShowString(1, 1, "Hello World!");
    LCD_ShowString(2, 1, a);
    test_stage = 2;
    test_tick = HAL_GetTick();
    break;

  case 2: /* 等待 2 秒（队列在主循环中自动被 LCD_Task_Handler 消化） */
    if (HAL_GetTick() - test_tick >= 2000) {
      LCD_Clear();
      test_stage = 3;
      test_tick = HAL_GetTick();
    }
    break;

  case 3: /* 等待清屏完成 */
    if (LCD_IsIdle() && (HAL_GetTick() - test_tick >= 100)) {
      LCD_ShowString(1, 1, "Num:");
      LCD_ShowNum(1, 5, 12345, 5);
      LCD_ShowString(2, 1, "Hex:");
      LCD_ShowHexNum(2, 5, 0xABCD, 4);
      test_stage = 4;
      test_tick = HAL_GetTick();
    }
    break;

  case 4: /* 等待 2 秒 */
    if (HAL_GetTick() - test_tick >= 2000) {
      LCD_Clear();
      test_stage = 5;
      test_tick = HAL_GetTick();
    }
    break;

  case 5: /* 等待清屏完成 */
    if (LCD_IsIdle() && (HAL_GetTick() - test_tick >= 100)) {
      LCD_ShowString(1, 1, "Float:");
      LCD_ShowFloatNum(1, 7, 3.14159f, 2);
      LCD_ShowString(2, 1, "Neg:");
      LCD_ShowSignedNum(2, 5, -42, 3);
      test_stage = 6;
      test_tick = HAL_GetTick();
    }
    break;

  case 6: /* 等待 2 秒 */
    if (HAL_GetTick() - test_tick >= 2000) {
      LCD_Clear();
      test_stage = 7;
      test_tick = HAL_GetTick();
    }
    break;

  case 7: /* 等待清屏完成后显示最终信息 */
    if (LCD_IsIdle() && (HAL_GetTick() - test_tick >= 100)) {
      LCD_ShowString(1, 1, b);
      LCD_ShowString(2, 1, c);
      test_stage = 0; /* 结束 */
    }
    break;

  default:
    break;
  }
}
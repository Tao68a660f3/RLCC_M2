/**
 * @file    app.c
 * @brief   应用主控模块，负责初始化所有驱动和主循环调度
 */
#include "app.h"
#include "bsp_rtc.h"
#include "canvas_renderer.h"
#include "font_config.h"
#include "lcd_driver.h"
#include "led_display.h"
#include "led_driver.h"
#include "stm32f1xx_hal.h"
#include "w25q64.h"
#include "window_manager.h"
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

  /* ── LED 屏幕初始化 ── */
  LED_Init();

  /* ── 初始化字库适配器（若 Flash 无字库则回退到内部 5×8） ── */
  Font_Config_Init();

  Font_Select_ASC(FONT_ASC_1608);
  Font_Select_GBK(FONT_GBK_1616S);

  WindowManager_Init();

  /* ── 示例 1: 用 Display_ShowString 简单显示文字 ── */
  // LED_Clear();
  // Display_ShowString(4, 0, "Hello", COLOR_ON);
  // Display_ShowString(4, 8, "RLCC!", COLOR_ON);
  // LED_Commit();
  // HAL_Delay(500);

  // /* ── 示例 2: 用窗口管理器显示中文 ── */
  // Window_Config(0, 0, 0, 160, 16);                                 //
  // 全屏窗口 Window_FillText(0, "STM32F103C8T6", 1, CANVAS_R, VALIGN_MIDDLE);
  // // 显示文字 Window_SetAlignment(0, ALIGN_CENTER);

  /* ── 示例 3: 第二个窗口滚动文字 ── */
  const char a[] = {0x48, 0x65, 0x6C, 0x6C, 0x6F, 0x20, 0x57, 0x6F, 0x72,
                    0x6C, 0x64, 0x21, 0x20, 0xD6, 0xD0, 0xCE, 0xC4, 0xCF,
                    0xD4, 0xCA, 0xBE, 0xB2, 0xE2, 0xCA, 0xD4, 0x00};
  Window_Config(0, 0, 0, 160, 16);
  Window_FillText(0, a, 1, CANVAS_R, VALIGN_MIDDLE);
  Window_SetAlignment(0, ALIGN_CENTER);
  window_list[0].scroll_divider = 1; // 每 3 帧滚动一次
  // window_list[0].scroll_step = 1;
  window_list[0].scroll_step = -1; // 向左滚动
}

void App_Loop(void) {
  static uint32_t last_tick = 0;
  LCD_Task_Handler();

  /* ── 窗口管理器处理（~60fps） ── */
  WindowManager_Process();

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

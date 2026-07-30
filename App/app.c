/**
 * @file    app.c
 * @brief   应用主控模块，负责初始化所有驱动和主循环调度
 */
#include "app.h"
#include "bsp_rtc.h"
#include "canvas_renderer.h"
#include "com_manager.h"
#include "font_config.h"
#include "lcd_driver.h"
#include "led_display.h"
#include "led_driver.h"
#include "lyric_service.h"
#include "protocol_parser.h"
#include "stm32f1xx_hal.h"
#include "ui_manager.h"
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

  /* ── 应用层模块初始化 ── */
  LyricService_Init();
  UI_Manager_Init();
  Protocol_Init();

  /* ── 串口初始化（DMA接收 + 空闲中断） ── */
  extern UART_HandleTypeDef huart1;
  COM_Init(&huart1);

  /* ── 清屏显示初始文本 ── */
  LED_Clear();
  Font_Select_ASC(FONT_ASC_1608);
  Font_Select_GBK(FONT_GBK_1616S);
  Window_Config(0, 0, 0, 160, 16);
  Window_FillText(0, "RLCC Ready", 1, CANVAS_R, VALIGN_MIDDLE);
  Window_SetAlignment(0, ALIGN_CENTER);

  LED_SetBrightness(100);
}

void App_Loop(void) {
  static uint32_t last_tick = 0;
  LCD_Task_Handler();

  /* ── 串口数据处理（根据当前系统模式分发） ── */
  if (g_curr_sys_mode == SYS_MODE_TXT_MODE) {
    COM_Process_TextMode();
  } else {
    COM_Process_ProtocolMode();
  }

  /* ── UI 管理器 Tick（歌词查找 + 窗口刷新） ── */
  UI_Manager_Tick();

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
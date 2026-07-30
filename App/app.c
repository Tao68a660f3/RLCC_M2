/**
 * @file    app.c
 * @brief   应用主控模块，负责初始化所有驱动和主循环调度
 */
#include "app.h"
#include "bsp_rtc.h"
#include "canvas_renderer.h"
#include "com_manager.h"
#include "env_manager.h"
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
extern UART_HandleTypeDef huart1;

const char start_info_txt[] = {
    0xCF, 0xB5, 0xCD, 0xB3, 0xC6, 0xF4, 0xB6,
    0xAF, 0xD6, 0xD0, 0x2E, 0x2E, 0x2E, 0x00}; // 系统启动中...

void App_Init(void) {
  /* ── 串口初始化（DMA接收 + 空闲中断） ── */
  COM_Init(&huart1);

  W25Q64_Init(&hspi2);
  RTC_App_Init(&hrtc);
  Env_Manager_Init();
  Env_Manager_Tick();

  LCD_Init();
  LCD_WaitReady();
  LCD_ShowString(1, 1, start_info_txt);
  LCD_WaitReady(); /* 强制刷新队列，确保启动信息在进入 Loop 前显示到 LCD */

  /* ── LED 屏幕初始化 ── */
  LED_Init();

  /* ── 初始化字库适配器（若 Flash 无字库则回退到内部 5×8） ── */
  Font_Config_Init();

  Font_Select_ASC(FONT_ASC_1608);
  Font_Select_GBK(FONT_GBK_1616S);

  WindowManager_Init();

  /* ── 应用层模块初始化 ── */
  LyricService_Init();
  Protocol_Init();
  UI_Manager_Init();

  /* LED 同步显示启动信息（不依赖 App_Loop 中的 Tick 循环） */
  UI_Manager_ShowTextSync(start_info_txt);

  HAL_Delay(2500);
}

void App_Loop(void) {
  LCD_Task_Handler();

  /* ── 串口数据处理：统一走协议解析（文本透传由 cmd=0x16 分发处理） ── */
  COM_Process_ProtocolMode();

  /* ── 环境管理器 Tick（RTC 同步 + 温湿度超时） ── */
  Env_Manager_Tick();

  /* ── UI 管理器 Tick（歌词查找 + 窗口刷新） ── */
  UI_Manager_Tick();

  /* ── 刷新日期/时间到 LCD ── */
  Env_Manager_ShowDateTime();
}

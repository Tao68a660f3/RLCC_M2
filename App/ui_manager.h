#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include "main.h"
#include "mem_pool.h"

typedef enum {
  UI_MODE_COMMON, // 模式1；160*16单行窗口（两种sys_mode均可使用）
  UI_MODE_ENV,    // 模式2：128*16窗口 + 时间温度小窗口（两种sys_mode均可使用）
} UI_Mode_t;

typedef enum {
  SYS_MODE_TXT_MODE = 0,
  SYS_MODE_PROTOCOL_MODE,
} Sys_Mode_t;

extern UI_Mode_t g_curr_ui_mode;
extern Sys_Mode_t g_curr_sys_mode;

void UI_Manager_Init(void);
void UI_Manager_Tick(void);

// ====== 协议命令预处理（在 Protocol_Dispatcher 之前调用）======
void UI_Manager_OnProtocolCmd(uint8_t cmd, uint8_t *payload, uint8_t len);

// ====== 模式切换接口 ======
void UI_Manager_SetMode_COMMON(void);
void UI_Manager_SetMode_ENV(void);
void UI_Manager_NextUIMode(void);

void UI_Manager_SetSysMode(Sys_Mode_t mode);
void UI_Manager_ToggleSysMode(void);

/** 在 App_Init 阶段同步显示文本到 LED 屏幕（不依赖 Tick 循环） */
void UI_Manager_ShowTextSync(const char *text);

// 文本模式：COM 层拆出完整行后回调此函数，填充共享缓冲区
void UI_Manager_OnTextLineReceived(const char *text);

// ====== 息屏控制 ======
void UI_Manager_ToggleScreen(void);
uint8_t UI_Manager_IsScreenOff(void);
void UI_Manager_WakeScreen(void);

#endif
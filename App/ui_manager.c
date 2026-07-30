#include "ui_manager.h"
#include "font_config.h"
#include "led_driver.h"
#include "lyric_service.h"
#include "mem_pool.h"
#include "window_manager.h"
#include <stdint.h>
#include <string.h>

// ============================= 全局状态 =============================
UI_Mode_t g_curr_ui_mode = UI_MODE_COMMON;
Sys_Mode_t g_curr_sys_mode = SYS_MODE_PROTOCOL_MODE;

/** 息屏标志：1=熄灭，0=亮屏 */
static uint8_t s_screen_off = 0;

// ============================= 共享文本缓冲区 =============================
#define TXT_BUF_SIZE 512
static char s_shared_text[TXT_BUF_SIZE] = "";
static uint8_t s_text_updated = 0; // 新文本到达标记（触发窗口刷新）

/**
 * @brief 用当前文本刷新窗口 0 的显示
 */
static void _FlushTextToWindow(void) {
  Font_Select_ASC(FONT_ASC_1608);
  Font_Select_GBK(FONT_GBK_1616S);

  Window_FillText(0, s_shared_text, C_RED, CANVAS_R, VALIGN_MIDDLE);

  // 超出宽度则开启滚动
  LED_Window *win = &window_list[0];
  if (win->canvas.width > win->w) {
    win->scroll_divider = 1;
    win->scroll_step = -1;
    Window_SetAlignment(0, ALIGN_LEFT);
  } else {
    win->scroll_divider = 0;
    Window_SetAlignment(0, ALIGN_CENTER);
  }
}

// ============================= EWMA 平滑播放时间（歌词模式）
// =============================
uint32_t g_play_time_ms = 0;

static void _UpdateSmoothTime(void) {
  static uint32_t s_smooth = 0;

  if (g_sys.local_record_tick == 0) {
    g_play_time_ms = 0;
    return;
  }

  if (!g_sys.is_playing) {
    s_smooth = g_sys.remote_time_ms;
    g_play_time_ms = g_sys.remote_time_ms;
    return;
  }

  uint32_t raw =
      g_sys.remote_time_ms + (HAL_GetTick() - g_sys.local_record_tick);

  if (s_smooth == 0) {
    s_smooth = raw;
    g_play_time_ms = raw;
    return;
  }

#define SEEK_THRESHOLD_MS 300

  if (raw > s_smooth) {
    uint32_t delta = raw - s_smooth;
    if (delta > SEEK_THRESHOLD_MS) {
      s_smooth = raw;
    } else {
      uint32_t step = (delta + 4) / 8;
      if (step < 1)
        step = 1;
      if (step > delta)
        step = delta;
      s_smooth += step;
    }
  } else {
    uint32_t delta = s_smooth - raw;
    if (delta > SEEK_THRESHOLD_MS) {
      s_smooth = raw;
    } else {
      s_smooth += 1;
    }
  }

#undef SEEK_THRESHOLD_MS

  g_play_time_ms = s_smooth;
}

/**
 * @brief 在 sorted_lyrics 中查找当前时间对应的歌词行
 * @return 指向 LyricArea 的指针，未找到返回 NULL
 */
static LyricArea *_FindCurrentLyric(uint32_t now) {
  for (uint32_t i = 0; i < g_sys.active_count; i++) {
    LyricArea *a = g_sys.sorted_lyrics[i];
    if (now >= a->start_time_ms && now < (a->start_time_ms + a->duration))
      return a;
  }
  return NULL;
}

//
// ============================= 公开 API =============================

void UI_Manager_Init(void) {
  WindowManager_Init();
  g_curr_ui_mode = UI_MODE_COMMON;
  g_curr_sys_mode = SYS_MODE_PROTOCOL_MODE;
  s_screen_off = 0;
  s_text_updated = 0;
  s_shared_text[0] = '\0';

  Window_Config(0, 0, 0, 160, 16);
}

/**
 * @brief 协议命令预处理：根据 cmd 自动切换系统模式
 *        在 Protocol_Dispatcher 被调用，在所有路由之前执行
 */
void UI_Manager_OnProtocolCmd(uint8_t cmd, uint8_t *payload, uint8_t len) {
  (void)payload;

  if (cmd == 0x16) {
    // cmd=0x16: 切到文本透传模式
    if (g_curr_sys_mode != SYS_MODE_TXT_MODE) {
      UI_Manager_SetSysMode(SYS_MODE_TXT_MODE);
    }

    uint32_t copy_len = (len < TXT_BUF_SIZE - 1) ? len : (TXT_BUF_SIZE - 1);
    memcpy(s_shared_text, payload, copy_len);
    s_shared_text[copy_len] = '\0';
    s_text_updated = 1;
    return;
  }

  if (cmd == 0x20) {
    // cmd=0x20: RTC 时间同步，不切换模式
    return;
  }

  // cmd=0x10~0x15: 切到歌词模式
  if (g_curr_sys_mode != SYS_MODE_PROTOCOL_MODE) {
    UI_Manager_SetSysMode(SYS_MODE_PROTOCOL_MODE);
  }
  // 歌词数据由 Lyric_OnXXXReceived 处理，UI 侧在 Tick 中按时间查找
}

void UI_Manager_SetMode_COMMON(void) {
  g_curr_ui_mode = UI_MODE_COMMON;
  WindowManager_Init();
  LED_Clear();
  s_text_updated = 0;
  Font_Select_ASC(FONT_ASC_1608);
  Font_Select_GBK(FONT_GBK_1616S);

  Window_Config(0, 0, 0, 160, 16);
  _FlushTextToWindow();
}

void UI_Manager_SetMode_ENV(void) {
  g_curr_ui_mode = UI_MODE_ENV;
  WindowManager_Init();
  LED_Clear();
  s_text_updated = 0;
  Font_Select_ASC(FONT_ASC_1608);
  Font_Select_GBK(FONT_GBK_1616S);

  // 128x16 主内容区 + 右侧 32x16 时间/温度
  Window_Config(0, 0, 0, 128, 16);
  Window_Config(1, 128, 0, 32, 8);
  Window_Config(2, 128, 8, 32, 8);
  _FlushTextToWindow();
}

void UI_Manager_NextUIMode(void) {
  if (g_curr_ui_mode == UI_MODE_COMMON) {
    UI_Manager_SetMode_ENV();
  } else {
    UI_Manager_SetMode_COMMON();
  }
}

void UI_Manager_SetSysMode(Sys_Mode_t mode) {
  if (mode == g_curr_sys_mode)
    return;

  g_curr_sys_mode = mode;
  s_text_updated = 0;

  // 重新配置窗口布局（行数 / 字体适应新模式）
  if (g_curr_ui_mode == UI_MODE_COMMON) {
    UI_Manager_SetMode_COMMON();
  }
}

void UI_Manager_ToggleSysMode(void) {
  Sys_Mode_t next = (g_curr_sys_mode == SYS_MODE_TXT_MODE)
                        ? SYS_MODE_PROTOCOL_MODE
                        : SYS_MODE_TXT_MODE;
  UI_Manager_SetSysMode(next);
}

void UI_Manager_OnTextLineReceived(const char *text) {
  // 外部手动喂文本
  uint32_t copy_len = strlen(text);
  if (copy_len >= TXT_BUF_SIZE)
    copy_len = TXT_BUF_SIZE - 1;
  memcpy(s_shared_text, text, copy_len);
  s_shared_text[copy_len] = '\0';
  s_text_updated = 1;
}

void UI_Manager_ToggleScreen(void) {
  s_screen_off = !s_screen_off;
  LED_SetScreenEnable(!s_screen_off);
}

uint8_t UI_Manager_IsScreenOff(void) { return s_screen_off; }

void UI_Manager_WakeScreen(void) {
  if (s_screen_off) {
    s_screen_off = 0;
    LED_SetScreenEnable(1);
  }
}

// ============================= Tick：主循环唯一入口
// =============================
void UI_Manager_Tick(void) {
  extern volatile uint8_t need_commit;

  // ---- 歌词模式：更新平滑时间 + 查找当前歌词 ----
  if (g_curr_sys_mode == SYS_MODE_PROTOCOL_MODE) {
    _UpdateSmoothTime();

    // 按时间查找歌词
    LyricArea *curr = _FindCurrentLyric(g_play_time_ms);
    if (curr) {
      // 找到当前行，拷贝到共享缓冲区（仅当文本变化时）
      if (strcmp(s_shared_text, curr->text) != 0) {
        uint32_t copy_len = strlen(curr->text);
        if (copy_len >= TXT_BUF_SIZE)
          copy_len = TXT_BUF_SIZE - 1;
        memcpy(s_shared_text, curr->text, copy_len);
        s_shared_text[copy_len] = '\0';
        s_text_updated = 1;
      }
    }
  }

  // 息屏：跳过渲染
  if (s_screen_off)
    return;
  if (need_commit)
    return;

  // ---- 有文本更新则刷新窗口 ----
  if (s_text_updated) {
    s_text_updated = 0;
    _FlushTextToWindow();
  }

  // ---- 驱动窗口渲染 ----
  /* ── 窗口管理器处理 ── */
  WindowManager_Process();
}
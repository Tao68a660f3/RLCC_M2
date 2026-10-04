#include "ui_manager.h"
#include "app.h"
#include "font_config.h"
#include "led_driver.h"
#include "lyric_service.h"
#include "mem_pool.h"
#include "sync_algorithm.h"
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
static uint8_t s_text_updated = 0;

/** 标题保护标志：切歌设标题后，第一条非空歌词到来前保护标题不被空歌词覆盖 */
static uint8_t s_title_guarded = 0;

/** 安全拷贝字符串到共享缓冲区并标记更新 */
static void _SetText(const char *text) {
  uint32_t copy_len = strlen(text);
  if (copy_len >= TXT_BUF_SIZE)
    copy_len = TXT_BUF_SIZE - 1;
  memcpy(s_shared_text, text, copy_len);
  s_shared_text[copy_len] = '\0';
  s_text_updated = 1;
}

/** 清空共享缓冲区并标记更新 */
static void _ClearText(void) {
  if (s_shared_text[0] == '\0')
    return; // 已为空，无需操作
  s_shared_text[0] = '\0';
  s_text_updated = 1;
}

/** 清除窗口 0 物理区域两个缓冲 */
static void _ClearWindow0(void) {
  LED_Window *win = &window_list[0];
  if (win->is_active)
    LED_ClearAreaAllBuffers(win->x, win->y, win->w, win->h);
}

/**
 * @brief 用当前文本刷新窗口 0 的显示
 */
static void _FlushTextToWindow(void) {
  Font_Select_ASC(FONT_ASC_1608);
  Font_Select_GBK(FONT_GBK_1616S);

  Window_FillText(0, s_shared_text, VALIGN_MIDDLE);

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

// =============== 播放时间（歌词模式） ===============
// 时间轴由 sync_algorithm 统一提供（0x11 → Sync_OnPacketAt），此处只采样
uint32_t g_play_time_ms = 0;

// ============ 配置 ============
// 1 = 允许该位置的占位行作为显示起点（显示空）
#define ALLOW_LEADING_PLACEHOLDER 1  // 开头占位
#define ALLOW_TRAILING_PLACEHOLDER 0 // 末尾占位（0 = 保持最后一句）

/**
 * @brief 在 sorted_lyrics 中查找当前时间对应的歌词行
 *
 * 单次扫描定位：
 *   a = 最后一个 start <= now 的有效行（组首）
 *   b = 第一个 start > now 的有效行
 * 显示策略：
 *   - a 仍在播放区间内        → 显示 a
 *   - a 已过期、b 距 now≤1.5s → 提前切到 b
 *   - a 已过期、已无下一行     → 末尾处理（见 ALLOW_TRAILING_PLACEHOLDER）
 *   - now 在第一句之前         → 开头处理（见 ALLOW_LEADING_PLACEHOLDER）
 * 同组（start 相同）只锚组首：_Lyric_UpdateSortedArray 的排序保证
 * 组首为 0x15/0x12 原文行，不会锚到翻译行。
 *
 * @return 指向 LyricArea 的指针；池空返回 NULL
 */
static LyricArea *_FindCurrentLyric(uint32_t now) {
  if (g_sys.active_count == 0)
    return NULL;

  int a_idx = -1;
  int b_idx = -1;
  uint32_t last_start = 0;
  uint8_t has_last = 0;

  // 定位 a（最后一个 start <= now 的有效行）与 b（第一个 start > now 的有效行）；
  // 占位行跳过，同组只锚组首（排序保证组首是 0x15/0x12 原文行）
  for (int i = 0; i < (int)g_sys.active_count; i++) {
    LyricArea *cur = g_sys.sorted_lyrics[i];

    if (cur->start_time_ms == 0 && cur->duration == 0)
      continue; // 占位行

    if (cur->start_time_ms <= now) {
      if (has_last && cur->start_time_ms == last_start)
        continue; // 同组，只锚组首
      a_idx = i;
      last_start = cur->start_time_ms;
      has_last = 1;
    } else {
      b_idx = i;
      break;
    }
  }

  int anchor;
  if (a_idx >= 0) {
    LyricArea *a = g_sys.sorted_lyrics[a_idx];
    // 减法判据：防 start_time_ms + duration 回绕
    if ((now - a->start_time_ms) < a->duration) {
      anchor = a_idx; // a 仍在有效区间
    } else if (b_idx >= 0) {
      // a 已过期：b 快到了提前切，否则保留 a 过渡
      uint32_t dt = g_sys.sorted_lyrics[b_idx]->start_time_ms - now;
      anchor = (dt <= 1500u) ? b_idx : a_idx;
    } else {
      // 全过期：末尾处理
      anchor = a_idx; // 保持最后一句
#if ALLOW_TRAILING_PLACEHOLDER
      int nxt = a_idx + 1;
      if (nxt < (int)g_sys.active_count && g_sys.sorted_lyrics[nxt]->cmd == 0x13)
        nxt++; // 跳过紧跟的翻译行
      if (nxt < (int)g_sys.active_count)
        anchor = nxt; // 末尾占位（显示空）
#endif
    }
  } else {
    // a 不存在：now 在第一句之前 → 开头处理
#if ALLOW_LEADING_PLACEHOLDER
    anchor = (b_idx > 0) ? (b_idx - 1) : 0; // 最后一个开头占位 / 第一句
#else
    anchor = (b_idx >= 0) ? b_idx : 0; // 第一句按"下一行"
#endif
  }

  return g_sys.sorted_lyrics[anchor];
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
    if (g_curr_sys_mode != SYS_MODE_TXT_MODE)
      UI_Manager_SetSysMode(SYS_MODE_TXT_MODE);

    // 安全截取 payload 到共享缓冲区
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
  if (g_curr_sys_mode != SYS_MODE_PROTOCOL_MODE)
    UI_Manager_SetSysMode(SYS_MODE_PROTOCOL_MODE);
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
  if (g_curr_ui_mode == UI_MODE_COMMON)
    UI_Manager_SetMode_ENV();
  else
    UI_Manager_SetMode_COMMON();
}

void UI_Manager_SetSysMode(Sys_Mode_t mode) {
  if (mode == g_curr_sys_mode)
    return;

  g_curr_sys_mode = mode;
  s_text_updated = 0;

  // 重新配置窗口布局（行数 / 字体适应新模式）
  if (g_curr_ui_mode == UI_MODE_COMMON)
    UI_Manager_SetMode_COMMON();
}

void UI_Manager_ToggleSysMode(void) {
  Sys_Mode_t next = (g_curr_sys_mode == SYS_MODE_TXT_MODE)
                        ? SYS_MODE_PROTOCOL_MODE
                        : SYS_MODE_TXT_MODE;
  UI_Manager_SetSysMode(next);
}

void UI_Manager_OnTextLineReceived(const char *text) { _SetText(text); }

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

// ============================= 初始化时 LED 同步显示
// =============================
/**
 * @brief 在 App_Init 阶段同步显示文本到 LED 屏幕
 *        直接设置文本、刷新窗口、提交显存，不依赖 Tick 循环
 */
void UI_Manager_ShowTextSync(const char *text) {
  _SetText(text);
  _ClearWindow0();
  _FlushTextToWindow();

  // 同步渲染到显存并提交
  Window_BlitToScreen(&window_list[0], 0, 0);
  LED_Commit();
  Window_BlitToScreen(&window_list[0], 0, 0);
  LED_Commit();
}

// ============================= Tick：主循环唯一入口
// =============================
void UI_Manager_Tick(void) {
  extern volatile uint8_t need_commit;

  // 切歌标志：清除旧歌词残留，显示标题
  if (media_updated) {
    media_updated = 0;

    if (g_sys.title[0] != '\0') {
      _SetText(g_sys.title);
      s_title_guarded = 1; // 设标题保护，防止后续空歌词覆盖
    } else {
      _ClearText();
      s_title_guarded = 0;
    }

    _ClearWindow0();
  }

  // ---- 歌词模式：更新平滑时间 + 查找当前歌词 ----
  if (g_curr_sys_mode == SYS_MODE_PROTOCOL_MODE) {
    g_play_time_ms = Sync_GetTime();

    LyricArea *curr = _FindCurrentLyric(g_play_time_ms);
    if (curr) {
      // 保护中 && 空文本 → 跳过，保留标题
      if (s_title_guarded && curr->text[0] == '\0') {
        // 什么都不做，标题继续显示
      } else if (strcmp(s_shared_text, curr->text) != 0) {
        _SetText(curr->text);
        s_title_guarded = 0; // 有真实内容了，解除保护
      }
    } else {
      // 未匹配到歌词（切歌间隙 / 播放结束），仅在非保护状态时清空
      if (!s_title_guarded)
        _ClearText();
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
  WindowManager_Process();
}
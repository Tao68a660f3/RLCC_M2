#include "ui_manager.h"
#include "app.h"
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

  Window_FillText(0, s_shared_text, COLOR_ON, CANVAS_R, VALIGN_MIDDLE);

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

// =============== 平滑播放时间（歌词模式） ===============
uint32_t g_play_time_ms = 0;

/**
 * @brief 纯整数微秒级 PID 速度微调 Smoothing
 */
static void _UpdateSmoothTime(void) {
  static uint32_t s_smooth_ms = 0;
  static uint32_t s_smooth_sub_ms = 0; // 毫秒的小数部分 (范围 0~999，即微秒)
  static uint32_t s_last_tick = 0;

  uint32_t now_tick = HAL_GetTick();
  if (s_last_tick == 0) {
    s_last_tick = now_tick;
  }
  uint32_t dt = now_tick - s_last_tick; // 130FPS 下 dt 通常为 7ms 或 8ms
  s_last_tick = now_tick;

  // 1. 边界拦截
  if (g_sys.local_record_tick == 0) {
    g_play_time_ms = 0;
    return;
  }
  if (!g_sys.is_playing) {
    s_smooth_ms = g_sys.remote_time_ms;
    s_smooth_sub_ms = 0;
    g_play_time_ms = g_sys.remote_time_ms;
    return;
  }

  // 2. 计算原始推算时间
  uint32_t raw = g_sys.remote_time_ms + (now_tick - g_sys.local_record_tick);

  if (s_smooth_ms == 0) {
    s_smooth_ms = raw;
    s_smooth_sub_ms = 0;
    g_play_time_ms = raw;
    return;
  }

#define SEEK_THRESHOLD_MS 300

  // 3. 计算时间差 error
  int32_t error = (int32_t)raw - (int32_t)s_smooth_ms;

  // 4. 大跨步拖动进度条 (Seek)
  if (error > SEEK_THRESHOLD_MS || error < -SEEK_THRESHOLD_MS) {
    s_smooth_ms = raw;
    s_smooth_sub_ms = 0;
  } else {
    // 5. 纯整数 PID 速度微调
    // 基准速度 ratio = 1000 (代表 1.000x)
    // Kp = 2，即每落后 1ms，速度提升 2/1000 = 0.2%
    int32_t speed_ratio = 1000 + (error * 2);

    // 限制微调上限为 0.900x ~ 1.100x (900 ~ 1100)
    // 130FPS 下肉眼对 ±10% 的速度微调绝对无法感知，但 1 秒内能轻松抹平 100ms
    // 的抖动！
    if (speed_ratio > 1100)
      speed_ratio = 1100;
    if (speed_ratio < 900)
      speed_ratio = 900;

    // 6. 高精度整数累加
    // 实际增加的微毫秒 = dt * speed_ratio
    // 比如 dt=8ms, speed_ratio=1020, 增加 8160 (即 8.16ms)
    uint32_t total_sub = s_smooth_sub_ms + (dt * (uint32_t)speed_ratio);

    s_smooth_ms += total_sub / 1000;    // 进位到整数毫秒
    s_smooth_sub_ms = total_sub % 1000; // 留存小数微秒
  }

#undef SEEK_THRESHOLD_MS

  g_play_time_ms = s_smooth_ms;
}

/**
 * @brief 在 sorted_lyrics 中查找当前时间对应的歌词行
 * @return 指向 LyricArea 的指针，未找到返回 NULL
 */
static LyricArea *_FindCurrentLyric(uint32_t now) {
  for (uint32_t i = 0; i < g_sys.active_count; i++) {
    LyricArea *a = g_sys.sorted_lyrics[i];
    uint32_t a_end = a->start_time_ms + a->duration;

    // 情形A：当前时间正在本行区间内
    if (now >= a->start_time_ms && now <= a_end)
      return a;

    // 情形B：尚未到达本行 → 退 1 行保持显示
    if (now < a->start_time_ms)
      return (i > 0) ? g_sys.sorted_lyrics[i - 1] : g_sys.sorted_lyrics[0];

    // 情形C：本行已过期 → 检查下一行是否需要提前切入
    if ((i + 1) < g_sys.active_count) {
      LyricArea *next = g_sys.sorted_lyrics[i + 1];
      // 跳过 cmd=0x13（间奏行）
      if (next->cmd == 0x13 && (i + 2) < g_sys.active_count)
        next = g_sys.sorted_lyrics[i + 2];
      // 距下一行开始不到 1.5s → 提前切换
      if ((now + 1500) >= next->start_time_ms &&
          next->start_time_ms > (a->start_time_ms + a->duration))
        return next;
    }
  }
  return g_sys.sorted_lyrics[0];
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
  Window_BlitToScreen(&window_list[0], 0xFFFF, 0, 0, 0, 0);
  LED_Commit();
  Window_BlitToScreen(&window_list[0], 0xFFFF, 0, 0, 0, 0);
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
    _UpdateSmoothTime();

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
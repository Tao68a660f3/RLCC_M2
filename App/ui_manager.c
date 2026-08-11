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

/** PI 平滑后的播放时间 (ms) —— 全局时间轴，UI 切换不清零 */
static uint32_t s_pi_time = 0;
/** 上次 PI 计算的墙钟 (ms) */
static uint32_t s_pi_last_tick = 0;

/**
 * @brief 130FPS 时间轴算法：speed 感知外推 + PI 平滑控推进速率
 * 每帧在 LyricWM_RenderMgr 入口调用一次。
 *
 * 原理：
 *   0x11 同步包在 Lyric_OnSyncReceived 中已建立锚点
 *   (base_remote_time / local_send_tick / playback_speed)，此后推算
 *   阶段完全不依赖 UDP 接收时刻 → 免疫网络延迟抖动。
 *
 *   目标值 target = base_remote_time + 墙钟流逝 × speed
 *   (含变速感知，与 C# 播放器速率同频)。
 *
 *   渲染端用 PI 平滑跟踪 target，分三种行为：
 *   - 正常播放：s_pi_time 以 speed 速率推进，误差用 P 项低通吸收
 *   - 变速确认：speed 更新 + 锚点重建 → target 拉开差距，
 *     P 项逐帧收敛（歌词平滑追赶，不瞬移）
 *   - Seek/暂停恢复：硬跳重建锚点 + playback_speed_changed 事件
 *     → s_pi_time 直接对齐 base_remote_time（瞬移接管）
 *
 * 边界：
 *   - 未完成首次同步 → 返回 0
 *   - 暂停 → 冻结在最近已知进度
 *   - 卡帧/异常间隔 → elapsed 钳制，防瞬移
 */
// ===== 渲染端 PI 平滑调参 =====
// P 项系数 (Q16, ≈2.3%)：调大→变速追赶快(输出偏抖)；调小→稳(追赶滞后)
#define PI_KP_Q16 3200
// 单帧 P 项补偿上限 (ms)：防异常误差(如一次大 Seek)导致歌词瞬移
#define PI_COMP_MAX_MS 40
// 单帧墙钟上限 (ms)：防长时间卡帧后恢复时的瞬移
#define PI_FRAME_MAX_MS 100

static void _UpdateSmoothTime(void) {
  uint32_t now_tick = HAL_GetTick();

  // 1. 边界拦截：未同步（切歌后 ClearPool 清 clock_synced，自动回到此路径）
  if (!g_sys.clock_synced) {
    g_play_time_ms = 0;
    s_pi_time = 0;
    s_pi_last_tick = now_tick;
    return;
  }

  // 2. 暂停：冻结在最近已知进度
  if (!g_sys.is_playing) {
    g_play_time_ms = g_sys.base_remote_time;
    s_pi_time = g_sys.base_remote_time;
    s_pi_last_tick = now_tick;
    return;
  }

  // 3. 变速/Seek 事件：直接对齐新锚点（硬跳接管），重置 PI
  if (g_sys.playback_speed_changed) {
    g_sys.playback_speed_changed = 0;
    s_pi_time = g_sys.base_remote_time;
    s_pi_last_tick = now_tick;
  }

  // 4. 目标 = 锚点 + 墙钟流逝 × speed（Q8，含变速感知）
  uint32_t elapsed = now_tick - s_pi_last_tick;
  if (elapsed > PI_FRAME_MAX_MS) {
    elapsed = PI_FRAME_MAX_MS;
    s_pi_last_tick = now_tick - elapsed; // 平移，避免累积误差
  }
  uint32_t target =
      (uint32_t)((int64_t)g_sys.base_remote_time +
                 (int64_t)((int32_t)now_tick - (int32_t)g_sys.local_send_tick) *
                     (int32_t)g_sys.playback_speed / (int32_t)256);

  // 5. 推进速率 = elapsed × speed（speed 感知，与锚点同步增长）
  int32_t frame_step =
      (int32_t)((int64_t)elapsed * g_sys.playback_speed / (int32_t)256);
  s_pi_time = (uint32_t)((int32_t)s_pi_time + frame_step);

  // 6. P 项误差补偿：低通吸收 target 与 s_pi_time 的偏差
  int32_t err = (int32_t)target - (int32_t)s_pi_time;
  int32_t comp = (int32_t)((int64_t)err * PI_KP_Q16 / 65536);
  if (comp > PI_COMP_MAX_MS)
    comp = PI_COMP_MAX_MS;
  if (comp < -PI_COMP_MAX_MS)
    comp = -PI_COMP_MAX_MS;
  s_pi_time = (uint32_t)((int32_t)s_pi_time + comp);
  s_pi_last_tick = now_tick;

  // 7. 封顶总时长
  if (g_sys.total_ms > 0 && s_pi_time > g_sys.total_ms)
    g_play_time_ms = g_sys.total_ms;
  else
    g_play_time_ms = s_pi_time;
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
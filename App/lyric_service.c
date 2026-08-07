#include "lyric_service.h"
#include "stdbool.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SYNC_SEEK_THRESHOLD_MS 500
#define CLOCK_FILTER_DIV 16
#define JITTER_MAX_MS 200
#define RETUNE_MARGIN_MS 3

#define SEEK_RATIO_MIN_Q8 64   // 0.25x
#define SEEK_RATIO_MAX_Q8 1024 // 4.0x
#define SPEED_WINDOW_PACKETS 16
#define SPEED_CHANGE_PERCENT 15

#define SPEED_NORMAL_Q8 256 // 1.0x
#define SPEED_MIN_Q8 128    // 0.5x
#define SPEED_MAX_Q8 768    // 3.0x

// ===== 变速检测滑窗状态 (包对增量，发包间隔免疫) =====
static uint32_t s_win_start_remote = 0; // 窗口首包播放进度
static uint32_t s_win_start_local = 0;  // 窗口首包 STM32 墙钟
static uint8_t s_win_pkt = 0;           // 窗口当前包数
static uint8_t s_speed_confirm = 0;     // 变速连续确认计数
static uint8_t s_has_prev = 0;          // 是否有上一包参考
static uint32_t s_last_remote_ms = 0;   // 上一包播放进度
static uint32_t s_last_local_rx = 0;    // 上一包 STM32 墙钟

SystemStatus g_sys;
uint8_t media_updated = 0;

void LyricService_Init(void) {
  memset(&g_sys, 0, sizeof(g_sys));
  g_sys.playback_speed = SPEED_NORMAL_Q8; // 默认 1.0x
}

void LyricService_ClearPool(void) {
  // 清空歌词池
  memset(g_sys.lyric_pool, 0, sizeof(g_sys.lyric_pool));
  memset(g_sys.sorted_lyrics, 0, sizeof(g_sys.sorted_lyrics));
  g_sys.active_count = 0;

  // 重置时间轴 (时钟同步标志清除，等待新歌首个同步包建立新基准)
  g_sys.remote_time_ms = 0;
  g_sys.total_ms = 0;
  g_sys.upstream_tick_ms = 0;
  g_sys.clock_offset_ms = 0;
  g_sys.base_remote_time = 0;
  g_sys.base_remote_tick = 0;
  g_sys.local_send_tick = 0;
  g_sys.min_obs_latency_ms = 0;
  g_sys.playback_speed = SPEED_NORMAL_Q8; // 新歌按 1.0x 起步
  g_sys.playback_speed_changed = 0;
  g_sys.clock_synced = 0;

  // 重置变速滑窗与增量跟踪
  s_win_pkt = 0;
  s_speed_confirm = 0;
  s_has_prev = 0;

  media_updated = 1;
}

// 排序优先级：同 Index 时 0x15(增强原文) > 0x12(原文) > 0x13(翻译)
// 数值越小优先级越高（排越前）
static uint8_t _Lyric_CmdPriority(uint8_t cmd) {
  switch (cmd) {
  case 0x15:
    return 0;
  case 0x12:
    return 1;
  case 0x13:
    return 2;
  default:
    return 3;
  }
}

// 查找某个 cmd 类型的下一行（用于时长预测）
static LyricArea *_Lyric_FindNextSameCmd(uint8_t cmd, int from) {
  for (int j = from; j < g_sys.active_count; j++) {
    if (g_sys.sorted_lyrics[j]->cmd == cmd) {
      return g_sys.sorted_lyrics[j];
    }
  }
  return NULL;
}

// 查找同 Index 的 0x15 增强行（用于精准时长继承）
static LyricArea *_Lyric_FindEnhancedByIndex(uint16_t index) {
  for (int i = 0; i < g_sys.active_count; i++) {
    if (g_sys.sorted_lyrics[i]->cmd == 0x15 &&
        g_sys.sorted_lyrics[i]->line_index == index) {
      return g_sys.sorted_lyrics[i];
    }
  }
  return NULL;
}

static void _Lyric_UpdateSortedArray(void) {
  g_sys.active_count = 0;

  // 1. 收集所有有效指针
  for (int i = 0; i < LYRIC_POOL_SIZE; i++) {
    if (g_sys.lyric_pool[i].is_valid) {
      g_sys.sorted_lyrics[g_sys.active_count++] = &g_sys.lyric_pool[i];
    }
  }

  // 2. 基础排序：Index 升序，同 Index 按优先级 0x15/0x12 > 0x13
  for (int i = 0; i < g_sys.active_count; i++) {
    for (int j = i + 1; j < g_sys.active_count; j++) {
      LyricArea *a = g_sys.sorted_lyrics[i];
      LyricArea *b = g_sys.sorted_lyrics[j];
      bool swap = false;
      if (a->line_index > b->line_index) {
        swap = true;
      } else if (a->line_index == b->line_index) {
        if (_Lyric_CmdPriority(a->cmd) > _Lyric_CmdPriority(b->cmd)) {
          swap = true;
        }
      }

      if (swap) {
        LyricArea *tmp = g_sys.sorted_lyrics[i];
        g_sys.sorted_lyrics[i] = g_sys.sorted_lyrics[j];
        g_sys.sorted_lyrics[j] = tmp;
      }
    }
  }

  // 3. --- 动态时长预测 (核心增强) ---
  for (int i = 0; i < g_sys.active_count; i++) {
    LyricArea *curr = g_sys.sorted_lyrics[i];

    // 0x15 自带精准 EndTime，跳过预测
    if (curr->cmd == 0x15) {
      continue;
    }

    // 0x12/13 需要动态预测
    if (curr->cmd == 0x12 || curr->cmd == 0x13) {
      bool found_next = false;

      // 3.1 优先继承同 Index 的 0x15 增强行精准时长
      LyricArea *enh = _Lyric_FindEnhancedByIndex(curr->line_index);
      if (enh && enh->duration > 0) {
        curr->duration = enh->duration;
        continue;
      }

      // 3.2 寻找同一个 cmd 类型的下一行
      LyricArea *next = _Lyric_FindNextSameCmd(curr->cmd, i + 1);
      if (next && next->start_time_ms > curr->start_time_ms) {
        // 时长 = 下一行开始时间 - 本行开始时间
        curr->duration = next->start_time_ms - curr->start_time_ms;
        found_next = true;
      }

      // 3.3 如果后面没数据了，根据歌曲总时长 total_ms 预测
      if (!found_next && g_sys.total_ms > curr->start_time_ms) {
        uint32_t remaining = g_sys.total_ms - curr->start_time_ms;
        // 最后一两句通常不会持续到死，限制在 5s 内
        curr->duration = (remaining > 5000) ? 5000 : remaining;
      }
    }
  }
}

// 0x10: 元数据解析 (TLV格式: Len + String)
void Lyric_OnMetadataReceived(uint8_t *payload, uint16_t len) {
  uint16_t p = 0;

  // Title
  uint8_t t_len = payload[p++];
  if (t_len >= MAX_METADATA_STR_LEN)
    t_len = MAX_METADATA_STR_LEN - 1;
  memcpy(g_sys.title, &payload[p], t_len);
  g_sys.title[t_len] = '\0';
  p += t_len;

  // Artist
  uint8_t r_len = payload[p++];
  if (r_len >= MAX_METADATA_STR_LEN)
    r_len = MAX_METADATA_STR_LEN - 1;
  memcpy(g_sys.artist, &payload[p], r_len);
  g_sys.artist[r_len] = '\0';
  p += r_len;

  // Album
  if (p < len) {
    uint8_t a_len = payload[p++];
    if (a_len >= MAX_METADATA_STR_LEN)
      a_len = MAX_METADATA_STR_LEN - 1;
    memcpy(g_sys.album, &payload[p], a_len);
    g_sys.album[a_len] = '\0';
  }

  LyricService_ClearPool();
}

// =====================================================================
// 0x11 时间同步：内部辅助函数（_Sync_* 均为纯搬移，行为不变）
// =====================================================================

// 重建时间轴锚点 + 重置最小观察延迟 (4 处公共操作)
static void _Sync_Rebase(int32_t obs, uint32_t remote_time_ms,
                         uint32_t upstream_tick_ms, uint32_t local_rx) {
  g_sys.clock_offset_ms = obs;
  g_sys.base_remote_time = remote_time_ms;
  g_sys.base_remote_tick = upstream_tick_ms;
  g_sys.local_send_tick = local_rx;
  g_sys.min_obs_latency_ms = obs;
}

// 重置变速滑窗计数（不含 s_has_prev / s_last_*）
static void _Sync_ResetTracking(void) {
  s_win_pkt = 0;
  s_speed_confirm = 0;
}

// 首次同步：建立锚点 + 通知渲染端硬跳对齐 + 丢弃旧参考
static void _Sync_HandleFirstSync(int32_t obs, uint32_t remote_time_ms,
                                  uint32_t upstream_tick_ms,
                                  uint32_t local_rx) {
  _Sync_Rebase(obs, remote_time_ms, upstream_tick_ms, local_rx);
  g_sys.clock_synced = 1;
  // 通知渲染端：下帧 s_pi_time 直接硬跳对齐新锚点，
  // 否则 s_pi_time 从 0 起步仅靠 P 项(≤40ms/帧)追赶，
  // 播放中途开机时进度会长时间落后表现为"从 0 追赶"。
  g_sys.playback_speed_changed = 1;
  // 本包即新参考，重置增量跟踪
  s_has_prev = 0;
  _Sync_ResetTracking();
}

// 暂停：用暂停包自带的权威暂停点重建锚点，使渲染端冻结值精确定位到
// 暂停时刻（而非可能因长时间无重同步事件而老化的旧锚点），
// 修复暂停后时间轴滞后数秒的问题。
// (恢复包由增量或硬跳兜底捕获)
static void _Sync_HandlePause(int32_t obs, uint32_t remote_time_ms,
                              uint32_t upstream_tick_ms, uint32_t local_rx) {
  _Sync_Rebase(obs, remote_time_ms, upstream_tick_ms, local_rx);
  s_has_prev = 0; // 丢弃暂停前参考，防恢复包用旧参考误判
  _Sync_ResetTracking();
}

// 变速测速滑窗 (SPEED_WINDOW_PACKETS 包首尾增量)
static void _Speed_UpdateWindow(int32_t obs, uint32_t remote_time_ms,
                                uint32_t upstream_tick_ms, uint32_t local_rx) {
  if (s_win_pkt == 0) {
    s_win_start_remote = remote_time_ms;
    s_win_start_local = local_rx;
    s_win_pkt = 1;
  } else {
    s_win_pkt++;
  }

  if (s_win_pkt >= SPEED_WINDOW_PACKETS) {
    int32_t w_remote = (int32_t)remote_time_ms - (int32_t)s_win_start_remote;
    int32_t w_local = (int32_t)local_rx - (int32_t)s_win_start_local;
    if (w_local >= 8 && w_remote > 0) {
      int32_t measured_q8 =
          (int32_t)((int64_t)w_remote * 256 / (int64_t)w_local);
      // 防御：playback_speed 若越界（含未初始化的 0），
      // 按 1.0x 处理，避免除零 HardFault 与误判变速。
      int32_t cur_q8 = (int32_t)g_sys.playback_speed;
      if (cur_q8 < (int32_t)SPEED_MIN_Q8 || cur_q8 > (int32_t)SPEED_MAX_Q8) {
        cur_q8 = (int32_t)SPEED_NORMAL_Q8;
      }
      int32_t diff_pct = abs(measured_q8 - cur_q8) * 100 / cur_q8;

      // 连续两窗命中才确认，防单窗抖动误判
      if (diff_pct > SPEED_CHANGE_PERCENT) {
        s_speed_confirm++;
      } else {
        s_speed_confirm = 0;
      }

      if (s_speed_confirm >= 2) {
        // 变速确认：clamp + 更新 speed + 重建锚点 + 置事件
        if (measured_q8 < (int32_t)SPEED_MIN_Q8)
          measured_q8 = (int32_t)SPEED_MIN_Q8;
        if (measured_q8 > (int32_t)SPEED_MAX_Q8)
          measured_q8 = (int32_t)SPEED_MAX_Q8;
        if (measured_q8 != (int32_t)g_sys.playback_speed)
          g_sys.playback_speed_changed = 1;
        g_sys.playback_speed = (uint16_t)measured_q8;
        _Sync_Rebase(obs, remote_time_ms, upstream_tick_ms, local_rx);
        _Sync_ResetTracking(); // 重新开窗，新锚点下继续自学习
      } else {
        // 未确认：滑动窗口 (以上一包为新区间起点，保留尾增量)
        s_win_start_remote = s_last_remote_ms;
        s_win_start_local = s_last_local_rx;
        s_win_pkt = 1;
      }
    } else {
      // 窗口内增量异常 (重连/序乱)：整体重新开窗
      _Sync_ResetTracking();
    }
  }
}

// 增量事件检测：单包倍率出界 → Seek/切歌/恢复，硬跳重建 + 重置 1.0x
// 返回 true 表示已命中 Seek，主流程应直接返回
static bool _Sync_DetectEvent(int32_t obs, uint32_t remote_time_ms,
                              uint32_t upstream_tick_ms, uint32_t local_rx) {
  int32_t d_remote = (int32_t)remote_time_ms - (int32_t)s_last_remote_ms;
  int32_t d_local = (int32_t)local_rx - (int32_t)s_last_local_rx;

  // 边界保护：间隔过小(快连包)或进度零增量跳过
  if (d_local >= 8 && d_remote != 0) {
    int32_t ratio_q8 = (int32_t)((int64_t)d_remote * 256 / (int64_t)d_local);

    // ---- 2a. Seek 判定：单包倍率出界 → 硬跳 + 重置 1.0x ----
    // (回退时 d_remote<0 → ratio_q8<0 → 天然命中下限)
    if (ratio_q8 < (int32_t)SEEK_RATIO_MIN_Q8 ||
        ratio_q8 > (int32_t)SEEK_RATIO_MAX_Q8) {
      _Sync_Rebase(obs, remote_time_ms, upstream_tick_ms, local_rx);
      g_sys.playback_speed = SPEED_NORMAL_Q8;
      g_sys.playback_speed_changed = 1; // 渲染端感知回 1.0x
      _Sync_ResetTracking();
      s_last_remote_ms = remote_time_ms;
      s_last_local_rx = local_rx;
      return true;
    }

    // ---- 2b. 变速测速滑窗 ----
    _Speed_UpdateWindow(obs, remote_time_ms, upstream_tick_ms, local_rx);
  }
  return false;
}

// 外推硬跳兜底 (慢发包 / 暂停恢复 / 增量检测未捕获的硬切)
// 返回 true 表示已硬跳，主流程应直接返回；不重置 speed，
// 若实为变速，后续滑窗会自动测出并纠正
static bool _Sync_CheckExtrapolation(int32_t obs, uint32_t remote_time_ms,
                                     uint32_t upstream_tick_ms,
                                     uint32_t local_rx) {
  int32_t estimated =
      (int32_t)((int64_t)g_sys.base_remote_time +
                (int64_t)((int32_t)local_rx - (int32_t)g_sys.local_send_tick) *
                    (int32_t)g_sys.playback_speed / (int32_t)256);
  if (abs((int32_t)remote_time_ms - estimated) > SYNC_SEEK_THRESHOLD_MS) {
    _Sync_Rebase(obs, remote_time_ms, upstream_tick_ms, local_rx);
    // 通知渲染端硬跳对齐。此路径是暂停恢复 / 慢发包硬切的主入口：
    // 暂停期间 s_pi_time 冻结在旧值，若不置事件，恢复后只能靠
    // P 项(≤40ms/帧)追赶，暂停越久进度落后越久（表现为"从 0 追赶"）。
    g_sys.playback_speed_changed = 1;
    return true;
  }
  return false;
}

// 微调链：jitter 过滤 / min_obs 锚点追踪 / 晶振漂移低通
static void _Sync_FineTune(int32_t obs, uint32_t remote_time_ms,
                           uint32_t upstream_tick_ms, uint32_t local_rx) {
  int32_t cs_delta =
      (int32_t)upstream_tick_ms - (int32_t)g_sys.base_remote_tick; // C# 端增量
  int32_t st_delta =
      (int32_t)local_rx - (int32_t)g_sys.local_send_tick; // STM32 端增量
  int32_t jitter = st_delta - cs_delta;

  // 网络延迟突变包：直接忽略，锚点纹丝不动
  if (jitter > JITTER_MAX_MS || jitter < -JITTER_MAX_MS)
    return;

  // 最小延迟锚点追踪：观测到更干净路径时前移锚点
  if (obs < g_sys.min_obs_latency_ms - RETUNE_MARGIN_MS) {
    // new_offset = -(offset_true + d_new)，只可能更准，绝不回跳
    g_sys.clock_offset_ms += (g_sys.min_obs_latency_ms - obs);
    g_sys.min_obs_latency_ms = obs;
    g_sys.base_remote_time = remote_time_ms;
    g_sys.base_remote_tick = upstream_tick_ms;
    g_sys.local_send_tick = local_rx;
    return;
  }

  // 普通漂移微调：低通吸收晶振漂移 (每包 ≤ ±2.5ms)
  g_sys.clock_offset_ms += jitter / CLOCK_FILTER_DIV;
  g_sys.local_send_tick =
      (uint32_t)((int32_t)g_sys.base_remote_tick - g_sys.clock_offset_ms);
}

/**
 * @brief 0x11: 时间同步（锚点 + 最小延迟追踪 + 抖动过滤 + 变速/Seek 检测）
 *
 * 包格式: [0]=is_playing [1..4]=remote_time_ms [5..8]=total_ms
 *         [9..12]=upstream_tick_ms (C# Environment.TickCount32)
 *
 * 事件检测基于"包对增量倍率"（Δremote/Δlocal），与发包间隔无关：
 *  - 单包倍率 <0.25x 或 >4x → Seek/切歌/恢复，硬跳重建 + 重置 1.0x
 *  - 滑窗测速连续两窗偏差 >15% → 变速确认，更新 speed + 重建锚点
 */
void Lyric_OnSyncReceived(uint8_t *payload, uint16_t len) {
  if (len < 13)
    return;

  uint8_t is_playing = payload[0];
  uint32_t remote_time_ms;
  uint32_t total_ms;
  uint32_t upstream_tick_ms;
  // 非对齐安全解包：payload 为 DMA 字节流，偏移 1/5/9 非 4 对齐，
  // 直接强转 *(uint32_t*) 会触发 M4 UNALIGNED HardFault，必须 memcpy。
  memcpy(&remote_time_ms, &payload[1], 4);
  memcpy(&total_ms, &payload[5], 4);
  memcpy(&upstream_tick_ms, &payload[9], 4); // C# TickCount32
  uint32_t local_rx = HAL_GetTick();

  g_sys.is_playing = is_playing;
  g_sys.total_ms = total_ms;
  g_sys.remote_time_ms = remote_time_ms;
  g_sys.upstream_tick_ms = upstream_tick_ms;

  // 可观测延迟 = C#时钟 - STM32时钟 + 单向网络延迟
  // (offset_true 为常数，取最小值等价于追踪真实延迟下界)
  int32_t obs = (int32_t)upstream_tick_ms - (int32_t)local_rx;

  // ===== 0. 首次同步：建立锚点，不做任何增量检测 =====
  if (!g_sys.clock_synced) {
    _Sync_HandleFirstSync(obs, remote_time_ms, upstream_tick_ms, local_rx);
    return;
  }

  // ===== 1. 暂停：不参与任何增量/变速/微调 =====
  // (外推值由 _UpdateSmoothTime 冻结；恢复包由下方增量或硬跳兜底捕获)
  if (!is_playing) {
    _Sync_HandlePause(obs, remote_time_ms, upstream_tick_ms, local_rx);
    return;
  }

  // ===== 2. 增量事件检测 (Seek / 变速)，仅在上一包参考存在时进行 =====
  if (s_has_prev &&
      _Sync_DetectEvent(obs, remote_time_ms, upstream_tick_ms, local_rx))
    return;

  // 更新增量参考 (供下一包使用)
  s_has_prev = 1;
  s_last_remote_ms = remote_time_ms;
  s_last_local_rx = local_rx;

  // ===== 3. 外推硬跳兜底 (慢发包 / 暂停恢复 / 增量检测未捕获的硬切) =====
  if (_Sync_CheckExtrapolation(obs, remote_time_ms, upstream_tick_ms, local_rx))
    return;

  // ===== 4. 微调链：jitter 过滤 / min_obs / 漂移低通 =====
  _Sync_FineTune(obs, remote_time_ms, upstream_tick_ms, local_rx);
}

/**
 * @brief 0x12/13/14: 歌词内容处理 (核心池管理)
 * 修正点：
 * 1. 偏移量对齐：Index(2B)@0, Time(4B)@2, WordCount/Text@6
 * 2. 查重逻辑：Index + Cmd 联合判定，防止原文翻译互踢
 * 3. 排序逻辑：Index升序，同Index时 原文(12/14) > 翻译(13)
 */
void Lyric_OnContentReceived(uint8_t cmd, uint8_t *payload, uint16_t len) {
  // --- 1. 偏移量解析 (基于 payload[0] 是 Index) ---
  // 注意：payload 地址不一定 4 字节对齐，必须用 memcpy 避免 HardFault
  uint16_t index;
  memcpy(&index, &payload[0], sizeof(uint16_t));
  uint32_t start_time;
  memcpy(&start_time, &payload[2], sizeof(uint32_t));
  LyricArea *slot = NULL;

  // --- 2. 查找 Slot (查重) ---
  // 必须 line_index 和 cmd 均匹配才视为同一条，进行覆盖
  for (int i = 0; i < LYRIC_POOL_SIZE; i++) {
    if (g_sys.lyric_pool[i].is_valid &&
        g_sys.lyric_pool[i].line_index == index &&
        g_sys.lyric_pool[i].cmd == cmd) {
      slot = &g_sys.lyric_pool[i];
      break;
    }
  }

  // --- 3. 找空位或 LRU 替换 ---
  if (!slot) {
    uint32_t min_tick = 0xFFFFFFFF;
    int oldest_idx = 0;
    for (int i = 0; i < LYRIC_POOL_SIZE; i++) {
      if (!g_sys.lyric_pool[i].is_valid) {
        slot = &g_sys.lyric_pool[i];
        break;
      }
      if (g_sys.lyric_pool[i].update_tick < min_tick) {
        min_tick = g_sys.lyric_pool[i].update_tick;
        oldest_idx = i;
      }
    }
    if (!slot)
      slot = &g_sys.lyric_pool[oldest_idx];
  }

  // --- 4. 填充基础数据 ---
  slot->is_valid = 0; // 写入时锁定
  slot->cmd = cmd;
  slot->line_index = index;
  slot->start_time_ms = start_time;
  slot->update_tick = HAL_GetTick();
  uint32_t txt_pos = 0;

  // --- 5. 内容解析 (修正字符偏移) ---
  if (cmd == 0x14) { // 逐字模式（单色屏：只拼文本、记时长，丢弃逐字细节）
    uint8_t word_count = payload[6];
    uint8_t *p_data = &payload[7];

    for (int i = 0; i < word_count; i++) {
      // 注意：p_data 地址不一定对齐，用 memcpy 避免 HardFault
      uint16_t word_offset;
      memcpy(&word_offset, p_data, sizeof(uint16_t));
      slot->duration = word_offset; // 最后一个字的时间偏移就是整行时长
      uint8_t w_len = p_data[2];
      p_data += 3;

      if (txt_pos + w_len < LYRIC_TEXT_SIZE) {
        memcpy(&slot->text[txt_pos], p_data, w_len);
        txt_pos += w_len;
      }
      p_data += w_len;
    }

    if (txt_pos < LYRIC_TEXT_SIZE)
      slot->text[txt_pos] = '\0';

  } else if (cmd == 0x15) { // 增强原文行:
                            // [Index:2B][StartTime:4B][EndTime:4B][Text...]
    uint32_t end_time;
    memcpy(&end_time, &payload[6], sizeof(uint32_t));
    if (end_time > start_time)
      slot->duration = end_time - start_time;
    else
      slot->duration = 4000; // 兜底

    uint8_t txt_offset = 10;
    uint32_t txt_len = (len > txt_offset) ? (len - txt_offset) : 0;

    if (txt_len >= LYRIC_TEXT_SIZE)
      txt_len = LYRIC_TEXT_SIZE - 1;

    memcpy(slot->text, &payload[txt_offset], txt_len);
    slot->text[txt_len] = '\0';

  } else if (cmd == 0x16) { // 纯文本: 填默认值
    slot->line_index = 0;
    slot->start_time_ms = g_sys.remote_time_ms;
    slot->duration = 5000;

    uint32_t txt_len = (len > 0) ? len : 0;
    if (txt_len >= LYRIC_TEXT_SIZE)
      txt_len = LYRIC_TEXT_SIZE - 1;

    memcpy(slot->text, &payload[0], txt_len);
    slot->text[txt_len] = '\0';

  } else { // 0x12 (原文) 或 0x13 (翻译)
    // 修正：C# data[9] 开始是文本，对应 payload[6]
    uint8_t txt_offset = 6;
    uint32_t txt_len = (len > txt_offset) ? (len - txt_offset) : 0;

    if (txt_len >= LYRIC_TEXT_SIZE)
      txt_len = LYRIC_TEXT_SIZE - 1;

    memcpy(slot->text, &payload[txt_offset], txt_len);
    slot->text[txt_len] = '\0';

    // 普通行暂定 4s 持续时间
    slot->duration = 4000;
  }

  // --- 6. 测量宽度并激活 ---
  // _Lyric_MeasureVerbatim(slot);
  slot->is_valid = 1;

  // --- 7. 更新有序指针数组 ---
  _Lyric_UpdateSortedArray();
}
#include "window_manager.h"
#include "flash_font.h"
#include "led_driver.h"
#include "mem_pool.h"
#include <stdint.h>
#include <string.h>

extern volatile uint8_t need_commit;

// ============================================================================
// 辅助函数：字符串遍历测量与绘制 (使用 Flash 字库适配器)
// ============================================================================

/**
 * @brief 测量一段字符串在画布上所需的像素尺寸
 * @param str   待测量的字符串 (UTF-8? 实际上为单字节 ASC + 双字节 GBK 混合)
 * @return Size2D 尺寸
 *
 * 使用 Font_Flash_ASC_Adapter / Font_Flash_GBK_Adapter 进行测量 (render_now=0)
 * 跳过 \r\n 换行符
 */
static Size2D _Window_MeasureText(const char *str) {
  DrawContext ctx = {0};
  Size2D size = {0};
  const char *p = str;
  while (*p) {
    if (*p == '\n' || *p == '\r') {
      if (*p == '\r' && *(p + 1) == '\n')
        p++;
      p++;
      continue;
    }
    GlyphInfo g;
    if ((uint8_t)*p < 0x80) {
      if (Font_Flash_ASC_Adapter((uint8_t)*p, &g, 0))
        Canvas_MeasureStep(&ctx, &g, &size);
      p++;
    } else {
      uint16_t gbk = ((uint16_t)(uint8_t)*p << 8) | (uint8_t)*(p + 1);
      if (Font_Flash_GBK_Adapter(gbk, &g, 0))
        Canvas_MeasureStep(&ctx, &g, &size);
      p += 2;
    }
  }
  return size;
}

/**
 * @brief 在已分配的 Canvas 上绘制字符串（支持行内竖直对齐）
 * @param ctx    绘图上下文 (需初始化 target, cur_x, cur_y 等)
 * @param str    待绘制字符串
 * @param color  颜色 (单色驱动中颜色被忽略，或可做亮度/反转)
 * @param valign 行内竖直对齐方式
 * @param row_h  当前行总高度 (px)
 *
 * 每个字符绘制前计算 y_offset = (row_h - glyph->height) * factor，
 * 临时偏移 ctx->cur_y 绘制后恢复，实现不同高度字符行内对齐。
 */
static void _Window_RenderText(DrawContext *ctx, const char *str,
                               CanvasColor color, Valign valign,
                               uint16_t row_h) {
  /* y_factor: 0=顶部, 128=居中, 255=底部，避免浮点运算 */
  uint8_t y_factor;
  switch (valign) {
  case VALIGN_TOP:
    y_factor = 0;
    break;
  case VALIGN_MIDDLE:
    y_factor = 128;
    break;
  case VALIGN_BOTTOM:
    y_factor = 255;
    break;
  default:
    y_factor = 0;
    break;
  }
  const char *p = str;
  while (*p) {
    if (*p == '\n' || *p == '\r') {
      if (*p == '\r' && *(p + 1) == '\n')
        p++;
      p++;
      continue;
    }
    GlyphInfo g;
    if ((uint8_t)*p < 0x80) {
      if (Font_Flash_ASC_Adapter((uint8_t)*p, &g, 1)) {
        uint16_t y_off =
            (row_h > g.height)
                ? (uint16_t)((uint32_t)(row_h - g.height) * y_factor / 255)
                : 0;
        ctx->cur_y += y_off;
        Canvas_DrawBitmap(ctx, &g, color);
        ctx->cur_y -= y_off;
      }
      p++;
    } else {
      uint16_t gbk = ((uint16_t)(uint8_t)*p << 8) | (uint8_t)*(p + 1);
      if (Font_Flash_GBK_Adapter(gbk, &g, 1)) {
        uint16_t y_off =
            (row_h > g.height)
                ? (uint16_t)((uint32_t)(row_h - g.height) * y_factor / 255)
                : 0;
        ctx->cur_y += y_off;
        Canvas_DrawBitmap(ctx, &g, color);
        ctx->cur_y -= y_off;
      }
      p += 2;
    }
  }
}

// 1. 窗口实例表（静态分配）
LED_Window window_list[MAX_WINDOWS];

/**
 * @brief 初始化窗口管理器
 */
void WindowManager_Init(void) {
  Pool_Init(); // 初始化画布内存池
  memset(window_list, 0, sizeof(window_list));
  for (int i = 0; i < MAX_WINDOWS; i++) {
    window_list[i].canvas.handle = -1; // 标记画布无效
  }
}

/**
 * @brief 配置窗口基础物理属性
 */
void Window_Config(uint8_t idx, int16_t x, int16_t y, uint16_t w, uint16_t h) {
  if (idx >= MAX_WINDOWS)
    return;
  window_list[idx].x = x;
  window_list[idx].y = y;
  window_list[idx].w = w;
  window_list[idx].h = h;
  window_list[idx].is_active = 1;
}

/**
 * @brief 设置窗口对齐逻辑 (计算 x_offset)
 */
void Window_SetAlignment(uint8_t idx, WinAlign align) {
  if (idx >= MAX_WINDOWS || window_list[idx].canvas.handle == -1)
    return;

  LED_Window *win = &window_list[idx];
  uint16_t content_w = win->canvas.width;

  switch (align) {
  case ALIGN_LEFT:
    win->x_offset = 0;
    break;
  case ALIGN_CENTER:
    win->x_offset = (win->w - content_w) / 2;
    break;
  case ALIGN_RIGHT:
    win->x_offset = win->w - content_w;
    break;
  }
}

// ============================================================================
// 窗口填充文本（Flash 字库适配器版）
// ============================================================================

/**
 * @brief 使用 Flash 字库适配器填充文本到指定窗口的画布
 * @param win_idx 窗口索引
 * @param str     待渲染的字符串 (混合 ASC + GBK)
 * @param color   前景色 (单色驱动中忽略)
 * @param mode    画布内存模式 (CANVAS_R 单色)
 * @param valign  竖直对齐方式 (顶部/居中/底部)
 *
 * 流程：测量字符串尺寸 → Pool_AllocCanvas (带复用) → 绘制 → 原子切换窗口引用
 * 绘制完后根据 valign 和窗口高度计算 y_offset 实现竖直对齐
 */
void Window_FillText(uint8_t win_idx, const char *str, CanvasColor color,
                     CanvasMode mode, Valign valign) {
  if (win_idx >= MAX_WINDOWS || str == NULL)
    return;

  LED_Window *win = &window_list[win_idx];

  // --- 1. 测量阶段 ---
  Size2D size = _Window_MeasureText(str);

  // --- 2. 分配阶段（带原位复用检测） ---
  int16_t old_h = win->canvas.handle;
  CanvasHandle new_canvas = Pool_AllocCanvas(size.w, size.h, old_h, mode);

  if (new_canvas.handle != -1) {
    // --- 3. 绘制阶段 ---
    DrawContext ctx = {0};
    ctx.target = &new_canvas;
    ctx.cur_x = 0;
    ctx.cur_y = 0;
    _Window_RenderText(&ctx, str, color, valign, size.h);

    // --- 4. 原子切换 ---
    if (new_canvas.handle != old_h && old_h != -1) {
      Pool_FreeCanvas(old_h);
    }
    win->canvas = new_canvas;
  }
  win->y_offset = (win->h - win->canvas.height) / 2;
}

// ============================================================================
// 核心渲染函数：单色块搬运
// ============================================================================

/**
 * @brief 将窗口的 Canvas 以单色块搬运方式写入 frame_buffer
 * @param win        目标窗口指针
 * @param h_px       歌词高亮分界像素 (0xFFFF=静态文本, 单色驱动中歌词颜色简化)
 * @param color_high 歌词亮色 (单色驱动中忽略)
 * @param color_base 歌词底色 (单色驱动中忽略)
 * @param off_x      额外 X 偏移
 * @param off_y      额外 Y 偏移
 *
 * 单色简化：
 *   - Canvas 格式为水平扫描 MSB-first (1 位/像素, 8 像素/字节)
 *   - frame_buffer 格式相同：每像素 1 位，1=亮，0=灭
 *   - Canvas 的 bit=1 对应 frame_buffer 对应位写 1
 *   - 无颜色通道处理，无区号
 */
void Window_BlitToScreen(LED_Window *win, uint16_t h_px, uint8_t color_high,
                         uint8_t color_base, int16_t off_x, int16_t off_y) {
  (void)h_px; // 歌词模式暂未实现，保留参数接口
  (void)color_high;
  (void)color_base;
  if (!win->is_active || win->canvas.handle == -1)
    return;

  CanvasHandle *cv = &win->canvas;
  uint16_t cv_bw = (cv->width + 7) / 8;               // Canvas 每行字节数
  uint8_t *shape = cv->r_ptr ? cv->r_ptr : cv->g_ptr; // 单色画布数据
  if (!shape)
    return;

  uint8_t wb = write_idx; // 快照当前写缓冲区索引

  int16_t base_x = win->x + off_x;
  int16_t base_y = win->y + off_y;
  uint16_t w = win->w;
  uint16_t h = win->h;

  int16_t canvas_origin_x = base_x + win->x_offset;
  /* canvas_origin_y 供将来扩展，当前未使用 */
  // int16_t canvas_origin_y = base_y + win->y_offset;

  for (uint16_t vy = 0; vy < h; vy++) {
    int16_t sy = base_y + (int16_t)vy;
    if (sy < 0 || sy >= LED_HEIGHT)
      continue;

    uint8_t sr = (uint8_t)(sy & (LED_SCAN_ROWS - 1));
    uint8_t *fb_row = frame_buffer[wb][sr]; // [BYTES_PER_ROW]

    // 水平范围钳位
    int16_t start_x = base_x;
    int16_t end_x = base_x + (int16_t)w;
    if (start_x < 0)
      start_x = 0;
    if (end_x > LED_WIDTH)
      end_x = LED_WIDTH;
    if (start_x >= end_x)
      continue;

    // 画布纵向坐标
    int16_t cy = (int16_t)vy - win->y_offset;
    if (cy < 0 || cy >= (int16_t)cv->height)
      continue;

    uint8_t *c_line = shape + (uint16_t)cy * cv_bw;

    /* ── 按屏幕字节（8 像素）批量搬运 ────────────────────
     * Canvas 和 frame_buffer 均为 packed bits MSB-first。
     * 每次处理一个屏幕字节（8 像素），从 Canvas 中读 2 字节
     * 拼成 16 位，右移位对齐到屏幕字节边界，OR 写入。
     * 一个屏幕字节最多跨越 2 个 Canvas 字节，移位即可解。 */
    for (int16_t sx = start_x; sx < end_x; sx += 8) {
      int16_t cx = sx - canvas_origin_x; // 屏幕字节首像素 → Canvas 坐标

      /* 整字节在 Canvas 左侧，跳过 */
      if (cx + 7 < 0)
        continue;
      /* 已完全超出 Canvas 右侧，本行结束 */
      if (cx >= (int16_t)cv->width)
        break;

      uint16_t c_idx = (uint16_t)(cx >> 3); // Canvas 字节索引
      uint8_t c_off = (uint8_t)(cx & 7);    // 位偏移 0~7

      /* 跨 2 字节读取，高位补 0 */
      uint16_t pair = (uint16_t)c_line[c_idx] << 8;
      if (c_idx + 1 < cv_bw)
        pair |= c_line[c_idx + 1];

      /* 右移使画布像素 0 对齐到屏幕 MSB 位置 */
      uint8_t bits = (uint8_t)(pair >> (8 - c_off));

      /* 掩码裁切：去除 Canvas 边界外的不存在像素 */
      uint8_t mask = 0xFF;
      if (cx < 0)
        mask &= (uint8_t)(0xFF << (-cx)); // 裁掉左端
      if (cx + 7 >= (int16_t)cv->width) {
        int16_t tail = cx + 7 - (int16_t)(cv->width - 1);
        mask &= (uint8_t)(0xFF >> tail); // 裁掉右端
      }

      fb_row[(uint16_t)(sx >> 3)] |= (bits & mask);
    }
  }
}

/**
 * @brief 单窗口处理：滚动逻辑 + 渲染
 */
void WindowManager_Single_Process(uint8_t i) {
  LED_Window *win = &window_list[i];
  if (!win->is_active)
    return;

  // --- 1. 处理分频滚动逻辑 ---
  if (win->scroll_divider > 0) {
    win->tick_counter++;
    if (win->tick_counter >= win->scroll_divider) {
      win->tick_counter = 0;
      win->x_offset += win->scroll_step;

      // 简单的循环滚动保护：当内容完全滚出左侧，从右侧回来
      if (win->scroll_step < 0 && win->x_offset < -(int16_t)win->canvas.width) {
        win->x_offset = win->w;
        win->scroll_counter++;
      } else if (win->scroll_step > 0 && win->x_offset > win->w) {
        win->x_offset = -(int16_t)win->canvas.width;
        win->scroll_counter++;
      }
    }
  }
  // --- 2. 执行渲染搬运 (单色块搬运) ---
  Window_BlitToScreen(win, 0xFFFF, 0, 0, 0, 0);
}

/**
 * @brief 窗口大总管：逻辑更新 + 渲染执行
 * 在 While(1) 中调用
 */
void WindowManager_Process(void) {
  if (need_commit)
    return;

  for (int i = 0; i < MAX_WINDOWS; i++) {
    WindowManager_Single_Process(i);
  }
  need_commit = 1;
}
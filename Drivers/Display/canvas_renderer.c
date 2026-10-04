#include "canvas_renderer.h"
#include <string.h>

/**
 * @brief 核心点写入函数 (位平面处理)
 */
void Canvas_WritePixel(CanvasHandle *h, uint16_t x, uint16_t y,
                       CanvasColor color) {
  // 严格检查：handle有效性、坐标范围、空指针
  if (h->handle == -1 || x >= h->width || y >= h->height) {
    return;
  }

  uint16_t byte_width = (h->width + 7) / 8;
  uint32_t offset = (uint32_t)y * byte_width + (x / 8);
  uint8_t bit_mask = 0x80 >> (x % 8);

  // 红色平面 (存在时才写)
  if (h->r_ptr) {
    if (color & C_RED)
      h->r_ptr[offset] |= bit_mask;
    else
      h->r_ptr[offset] &= ~bit_mask;
  }

  // 绿色平面 (存在且不与红色共用时才写)
  if (h->g_ptr && h->g_ptr != h->r_ptr) {
    if (color & C_GREEN)
      h->g_ptr[offset] |= bit_mask;
    else
      h->g_ptr[offset] &= ~bit_mask;
  }
}

/**
 * @brief 获取某个点的颜色 (用于混合或判断)
 */
CanvasColor Canvas_GetPixel(CanvasHandle *h, uint16_t x, uint16_t y) {
  if (h->handle == -1 || x >= h->width || y >= h->height)
    return C_BLACK;

  uint16_t byte_width = (h->width + 7) / 8;
  uint32_t offset = (uint32_t)y * byte_width + (x / 8);
  uint8_t bit_mask = 0x80 >> (x % 8);

  uint8_t color = 0;
  if (h->r_ptr && (h->r_ptr[offset] & bit_mask))
    color |= C_RED;
  if (h->g_ptr && (h->g_ptr[offset] & bit_mask))
    color |= C_GREEN;
  // CANVAS_Y: g_ptr == r_ptr，两if都取同一位置，
  // C_RED|C_GREEN = C_YELLOW，语义正确

  return (CanvasColor)color;
}

/**
 * @brief 清空画布 (物理擦除)
 */
void Canvas_ClearCanvas(CanvasHandle *h) {
  if (h->handle == -1)
    return;
  uint32_t size = ((h->width + 7) / 8) * h->height;
  if (h->r_ptr)
    memset(h->r_ptr, 0, size);
  if (h->g_ptr && h->g_ptr != h->r_ptr)
    memset(h->g_ptr, 0, size);
}

/**
 * @brief 整个画布按位取反（反白/反色）
 *
 * 平面选择：
 *   - 单平面 (CANVAS_R / CANVAS_G)：只处理 r_ptr ?: g_ptr（单色平台常态）；
 *   - CANVAS_RG 双平面：两平面各自独立取反；
 *   - CANVAS_Y：r_ptr == g_ptr，同一平面只取反一次。
 *
 * 行尾 padding 必须清回 0：画布宽 w 时每行占 ceil(w/8) 字节，最后一个字节里
 * 超出 w 的低位是 padding。而 Window_BlitToScreen 的块搬运是"按屏幕字节对齐"
 * 整字节读取画布（见 window_manager.c 中 pair/bits 的拼装），padding 位一旦
 * 被置 1 就会被当作有效像素点亮，表现为每行末尾多出亮点。
 * Pool_AllocCanvas 的初值即"padding = 0"，此处保持一致。
 */
void Canvas_InvertCanvas(CanvasHandle *h) {
  if (!h || h->handle == -1)
    return;
  if (h->width == 0 || h->height == 0)
    return;

  uint16_t bw = (uint16_t)((h->width + 7) / 8); // 每行字节数
  uint32_t bytes = (uint32_t)bw * h->height;
  uint8_t rem = (uint8_t)(h->width & 7);
  /* 有效位掩码：rem==0 整字节有效；否则只保留高 rem 位（MSB-first） */
  uint8_t tail_keep = (rem == 0) ? 0xFFu : (uint8_t)(0xFFu << (8 - rem));

  uint8_t *planes[2] = {h->r_ptr, h->g_ptr};
  for (int p = 0; p < 2; p++) {
    uint8_t *buf = planes[p];
    if (buf == NULL)
      continue;
    if (p == 1 && h->g_ptr == h->r_ptr)
      continue; // CANVAS_Y：同一平面只处理一次

    for (uint32_t i = 0; i < bytes; i++)
      buf[i] = (uint8_t)~buf[i];

    if (tail_keep != 0xFFu) {
      for (uint16_t y = 0; y < h->height; y++)
        buf[(uint32_t)y * bw + (bw - 1)] &= tail_keep;
    }
  }
}

/**
 * @brief 内部函数：处理笔尖换行逻辑 (供测量和绘制通用)
 */
static void _Canvas_HandleWrap(DrawContext *ctx, uint16_t next_w) {
  if (ctx->auto_wrap && ctx->wrap_width > 0) {
    if (ctx->cur_x + next_w > ctx->wrap_width && ctx->cur_x > 0) {
      ctx->cur_x = 0;
      ctx->cur_y += (ctx->line_h + ctx->line_space);
      ctx->line_h = 0;
    }
  }
}

/**
 * @brief 测量步骤 (不写内存)
 */
void Canvas_MeasureStep(DrawContext *ctx, const GlyphInfo *glyph,
                        Size2D *limit) {
  if (!glyph)
    return;

  _Canvas_HandleWrap(ctx, glyph->width);

  // 步进笔尖
  ctx->cur_x += (glyph->width + ctx->padding);
  if (glyph->height > ctx->line_h)
    ctx->line_h = glyph->height;

  // 更新边界矩形
  if (ctx->cur_x > limit->w)
    limit->w = ctx->cur_x;
  if (ctx->cur_y + ctx->line_h > limit->h)
    limit->h = ctx->cur_y + ctx->line_h;
}

/**
 * @brief 绘制位图 (实际搬运点阵)
 */
void Canvas_DrawBitmap(DrawContext *ctx, const GlyphInfo *glyph,
                       CanvasColor color) {
  if (!ctx->target || !glyph || !glyph->p_bits)
    return;

  _Canvas_HandleWrap(ctx, glyph->width);

  uint16_t byte_w = (glyph->width + 7) / 8;

  for (uint16_t y = 0; y < glyph->height; y++) {
    for (uint16_t x = 0; x < glyph->width; x++) {
      // 检查内容是否会超出画布物理边界
      if (ctx->cur_x + x < ctx->target->width &&
          ctx->cur_y + y < ctx->target->height) {
        // 取得原始点阵中的位 (假设横向扫描)
        if (glyph->p_bits[y * byte_w + (x / 8)] & (0x80 >> (x % 8))) {
          Canvas_WritePixel(ctx->target, ctx->cur_x + x, ctx->cur_y + y, color);
        }
      }
    }
  }

  // 步进笔尖坐标
  ctx->cur_x += (glyph->width + ctx->padding);
  if (glyph->height > ctx->line_h)
    ctx->line_h = glyph->height;
}

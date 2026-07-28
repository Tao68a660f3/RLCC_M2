#include "led_display.h"
#include "inner_font.h"

// 1. 简单的画点封装
void Display_DrawPixel(int16_t x, int16_t y, LED_Color color) {
  LED_SetPixel(x, y, color);
}

void Display_Clear(void) { LED_Clear(); }

// 2. 解析你的 5x7/5x8 字库
void Display_DrawChar(int16_t x, int16_t y, char c, LED_Color color) {
  if (c < char_offset || c > char_offset + char_numsum)
    c = ' ';

  uint16_t font_ptr = (c - char_offset) * 5; // 你的字库每个字符 5 字节

  for (int col = 0; col < 5; col++) { // 遍历 5 列
    uint8_t columnData = Chars[font_ptr + col];

    for (uint8_t row = 0; row < 8; row++) { // 遍历 8 行
      if (columnData & (0x80 >> row)) {
        Display_DrawPixel(x + col, y + row, color);
      }
    }
  }
}

// 3. 静态字符串显示
void Display_ShowString(int16_t x, int16_t y, char *str, LED_Color color) {
  while (*str) {
    // 如果字符超出了右边界，直接停掉，节省 CPU
    if (x > LED_WIDTH)
      break;

    // 只有在屏幕范围内的字符才画
    if (x + 5 > 0) {
      Display_DrawChar(x, y, *str, color);
    }

    x += 6; // 5像素宽 + 1像素间距
    str++;
  }
}

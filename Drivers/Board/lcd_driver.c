#include "lcd_driver.h"
#include <string.h>

/* ==================== 软件 SPI 宏（引脚定义来自 main.h）====================
 */
#define LCD_SCK_HI()                                                           \
  HAL_GPIO_WritePin(LCD_SCK_GPIO_Port, LCD_SCK_Pin, GPIO_PIN_SET)
#define LCD_SCK_LO()                                                           \
  HAL_GPIO_WritePin(LCD_SCK_GPIO_Port, LCD_SCK_Pin, GPIO_PIN_RESET)
#define LCD_SID_HI()                                                           \
  HAL_GPIO_WritePin(LCD_SID_GPIO_Port, LCD_SID_Pin, GPIO_PIN_SET)
#define LCD_SID_LO()                                                           \
  HAL_GPIO_WritePin(LCD_SID_GPIO_Port, LCD_SID_Pin, GPIO_PIN_RESET)
#define LCD_CS_HI()                                                            \
  HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET)
#define LCD_CS_LO()                                                            \
  HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_RESET)

/* ==================== 延时 ==================== */
/* 72MHz, 约 0.42us 半周期 ~ 1.2MHz SPI（ST7920 串行最高 2.5MHz，安全余量） */
#define LCD_DELAY_CYCLES 30

static void LCD_Delay(void) {
  volatile uint32_t i = LCD_DELAY_CYCLES;
  while (i--)
    ;
}

/* ==================== 异步队列 ==================== */
static LCD_Queue_t lcdQueue;

/* ==================== 底层：软件 SPI 发送 1 字节（匹配 51 版时序）==========
 */
static void LCD_SendByte(uint8_t byte) {
  for (int8_t i = 0; i < 8; i++) {
    // 先送数据，再产生时钟（上升沿锁存）
    if (byte & (0x80 >> i))
      LCD_SID_HI();
    else
      LCD_SID_LO();
    LCD_Delay();
    LCD_SCK_LO();
    LCD_Delay();
    LCD_SCK_HI();
    LCD_Delay();
  }
}

/**
 * @brief 硬件层：发送 3 字节串行包（软件 SPI，匹配 51 版 CS 时序）
 */
static void LCD_HardwareSend(uint8_t RS, uint8_t Data) {
  uint8_t packet[3];
  // 起始字节：11111 + RW(0) + RS + 0
  packet[0] = RS ? 0xFA : 0xF8;
  packet[1] = Data & 0xF0;
  packet[2] = (Data << 4) & 0xF0;

  // 精确匹配 51 版 CS 时序：
  // CS=0 → SCLK=0 → CS=1（准备传输）
  LCD_CS_LO();
  LCD_SCK_LO();
  LCD_Delay();
  LCD_CS_HI();
  LCD_Delay();

  LCD_SendByte(packet[0]);
  LCD_SendByte(packet[1]);
  LCD_SendByte(packet[2]);

  // 结束：CS=0, SCLK=0
  LCD_CS_LO();
  LCD_Delay();
  LCD_SCK_LO();
  LCD_Delay();
}

/**
 * @brief 基础异步写入接口（供内部显示函数调用）
 */
static void LCD_Write_Async(uint8_t RS, uint8_t cmd_or_data) {
  uint16_t next_tail = (lcdQueue.tail + 1) % LCD_BUF_SIZE;

  // 缓冲区满：阻塞等待直到腾出空位
  while (next_tail == lcdQueue.head) {
    LCD_Task_Handler();
  }

  lcdQueue.data[lcdQueue.tail] = cmd_or_data;
  lcdQueue.is_rs[lcdQueue.tail] = RS;
  lcdQueue.tail = next_tail;
}

/* ==================== 初始化 ==================== */
void LCD_Init(void) {
  // CS、SCLK 初始化为低
  LCD_CS_LO();
  LCD_SCK_LO();
  LCD_SID_LO();
  HAL_Delay(50); // 等待 LCD 上电稳定（>40ms）

  // ST7920 标准串行初始化序列（参考 51 版 + 数据手册）
  LCD_HardwareSend(0, 0x30); // 1. 基本指令集
  HAL_Delay(5);              // 等待 >4.1ms

  LCD_HardwareSend(0, 0x30); // 2. 重复设置
  HAL_Delay(1);              // 等待 >100μs

  LCD_HardwareSend(0, 0x30); // 3. 再重复一次确保稳定
  HAL_Delay(1);

  LCD_HardwareSend(0, 0x06); // 4. 进入模式：光标右移，整体不移位
  HAL_Delay(1);

  LCD_HardwareSend(0, 0x01); // 5. 清屏
  HAL_Delay(12);             // 清屏需要 >10ms

  LCD_HardwareSend(0, 0x0C); // 6. 显示开，光标关，闪烁关
  HAL_Delay(1);

  LCD_HardwareSend(0, 0x02); // 7. 地址归零（DDRAM 地址=0）
  HAL_Delay(1);

  // 初始化最后，更新一下时间戳，防止后续异步任务立即触发
  lcdQueue.last_tick = HAL_GetTick();
}

/**
 * @brief 异步任务处理器，放在 main 的 while(1) 中轮询
 */
void LCD_Task_Handler(void) {
  if (lcdQueue.head != lcdQueue.tail) {
    uint8_t rs = lcdQueue.is_rs[lcdQueue.head];
    uint8_t data = lcdQueue.data[lcdQueue.head];
    uint32_t wait_time;

    if (rs == 0) {
      if (data == 0x01)
        wait_time = 2; // 清屏仅等 2ms（ST7920 需要 >1.2ms）
      else
        wait_time = 0; // 其他指令无需延迟
    } else {
      wait_time = 0; // 数据无需等待
    }

    if (HAL_GetTick() - lcdQueue.last_tick >= wait_time) {
      LCD_HardwareSend(rs, data);
      lcdQueue.last_tick = HAL_GetTick();
      lcdQueue.head = (lcdQueue.head + 1) % LCD_BUF_SIZE;
    }
  }
}

void LCD_WaitReady(void) {
  while (lcdQueue.head != lcdQueue.tail) {
    LCD_Task_Handler();
  }
}

uint8_t LCD_IsIdle(void) { return (lcdQueue.head == lcdQueue.tail); }

/* ==================== 基础显示 API ==================== */

void LCD_Clear(void) { LCD_Write_Async(0, 0x01); }

/**
 * @brief 计算 x 的 y 次幂（整数运算）
 */
static uint16_t LCD_Pow(uint16_t x, uint16_t y) {
  uint8_t i;
  uint16_t res = 1;
  for (i = 0; i < y; i++)
    res *= x;
  return res;
}

/**
 * @brief 计算 DDRAM 地址
 */
void LCD_SetCursorPos(uint8_t line, uint8_t col) {
  uint8_t addr;
  if (line == 1)
    addr = 0x80;
  else
    addr = 0x90;
  addr = addr + col % 10 - 1;
  LCD_Write_Async(0, addr);
}

/**
 * @brief 自动处理中西文混排的字符串显示（自动对齐双字节字符）
 */
uint8_t LCD_ShowStringWithAutoSpace(const char *str, uint8_t HAlign) {
  uint8_t i = 0, j = 0;
  uint16_t asciiCount = 0;

  while (str[i] != '\0' && i < 72) {
    if ((uint8_t)str[i] < 0x80) {
      // ASCII 字符（单字节）
      LCD_Write_Async(1, str[i]);
      asciiCount++;
      i++;
      j++;
      if (j % 18 == 0) {
        uint16_t n = j / 18;
        LCD_Write_Async(0, 0x80 | ((n & 0x0F) << 4));
      }
    } else {
      // 汉字（双字节）
      if (asciiCount % 2 != 0) {
        LCD_Write_Async(1, ' ');
        j++;
        if (j % 18 == 0) {
          uint16_t n = j / 18;
          LCD_Write_Async(0, 0x80 | ((n & 0x0F) << 4));
        }
      }
      asciiCount = 0;
      LCD_Write_Async(1, str[i]);
      LCD_Write_Async(1, str[i + 1]);
      i += 2;
      j += 2;
      if (j % 18 == 0) {
        uint16_t n = j / 18;
        LCD_Write_Async(0, 0x80 | ((n & 0x0F) << 4));
      }
    }
  }

  if (asciiCount % 2 != 0) {
    LCD_Write_Async(1, ' ');
    j++;
  }

  return j > 36;
}

void LCD_ShowString(uint8_t line, uint8_t col, const char *str) {
  LCD_SetCursorPos(line, col);
  LCD_ShowStringWithAutoSpace(str, 0);
}

void LCD_ShowNum(uint8_t line, uint8_t col, uint32_t num, uint8_t len) {
  uint8_t i;
  LCD_SetCursorPos(line, col);
  for (i = len; i > 0; i--) {
    LCD_Write_Async(1, num / LCD_Pow(10, i - 1) % 10 + '0');
  }
}

void LCD_ShowSignedNum(uint8_t line, uint8_t col, int32_t num, uint8_t len) {
  uint8_t i;
  uint16_t N;
  LCD_SetCursorPos(line, col);
  if (num >= 0) {
    LCD_Write_Async(1, '+');
    N = num;
  } else {
    LCD_Write_Async(1, '-');
    N = -num;
  }
  for (i = len; i > 0; i--) {
    LCD_Write_Async(1, N / LCD_Pow(10, i - 1) % 10 + '0');
  }
}

void LCD_ShowFloatNum(uint8_t line, uint8_t col, float num, uint8_t dotLen) {
  uint8_t i, j, digit_count;
  uint32_t integer_part, decimal_part;
  uint32_t multiplier = 1, temp, divisor;
  LCD_SetCursorPos(line, col);

  if (dotLen > 5)
    dotLen = 5;
  for (i = 0; i < dotLen; i++)
    multiplier *= 10;

  if (num < 0) {
    LCD_Write_Async(1, '-');
    num = -num;
  }

  integer_part = (uint32_t)num;
  decimal_part = (uint32_t)((num - (float)integer_part) * multiplier + 0.5f);

  if (decimal_part >= multiplier) {
    integer_part += 1;
    decimal_part = 0;
  }

  // 整数部分
  if (integer_part == 0) {
    LCD_Write_Async(1, '0');
  } else {
    digit_count = 0;
    for (temp = integer_part; temp > 0; temp /= 10)
      digit_count++;
    for (i = digit_count; i > 0; i--) {
      divisor = 1;
      for (j = 1; j < i; j++)
        divisor *= 10;
      LCD_Write_Async(1, (integer_part / divisor) % 10 + '0');
    }
  }

  // 小数部分
  if (dotLen > 0) {
    LCD_Write_Async(1, '.');
    temp = decimal_part;
    digit_count = 0;
    if (temp > 0) {
      while (temp > 0) {
        temp /= 10;
        digit_count++;
      }
    } else {
      digit_count = 1;
    }
    for (i = dotLen; i > digit_count; i--) {
      LCD_Write_Async(1, '0');
    }
    if (decimal_part > 0) {
      for (i = digit_count; i > 0; i--) {
        divisor = 1;
        for (j = 1; j < i; j++)
          divisor *= 10;
        LCD_Write_Async(1, (decimal_part / divisor) % 10 + '0');
      }
    } else {
      LCD_Write_Async(1, '0');
    }
  }
}

void LCD_ShowHexNum(uint8_t line, uint8_t col, uint32_t num, uint8_t len) {
  uint8_t i;
  uint8_t N;
  LCD_SetCursorPos(line, col);
  for (i = len; i > 0; i--) {
    N = num / LCD_Pow(16, i - 1) % 16;
    if (N < 10)
      LCD_Write_Async(1, N + '0');
    else
      LCD_Write_Async(1, N + 'A' - 10);
  }
}

void LCD_WriteChar(char ch) { LCD_Write_Async(1, (uint8_t)ch); }

void LCD_WriteString(const char *str) { LCD_ShowStringWithAutoSpace(str, 0); }
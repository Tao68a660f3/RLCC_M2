#ifndef __LCD_DRIVER_H
#define __LCD_DRIVER_H

#include "main.h"

/* --- 配置参数 --- */
#define LCD_BUF_SIZE 128

/* --- 异步管理结构体 --- */
typedef struct {
  uint8_t data[LCD_BUF_SIZE];
  uint8_t is_rs[LCD_BUF_SIZE]; // 0:命令, 1:数据
  uint16_t head;
  uint16_t tail;
  uint32_t last_tick; // 上次发送的时间戳
} LCD_Queue_t;

/* --- 核心驱动接口 --- */
void LCD_Task_Handler(void);
void LCD_Init(void);
void LCD_WaitReady(void);
uint8_t LCD_IsIdle(void);

/* 基础显示 */
void LCD_Clear(void);
void LCD_SetCursorPos(uint8_t line, uint8_t col);
uint8_t LCD_ShowStringWithAutoSpace(const char *str, uint8_t HAlign);
void LCD_ShowString(uint8_t line, uint8_t col, const char *str);

/* 数值显示 */
void LCD_ShowNum(uint8_t line, uint8_t col, uint32_t num, uint8_t len);
void LCD_ShowSignedNum(uint8_t line, uint8_t col, int32_t num, uint8_t len);
void LCD_ShowFloatNum(uint8_t line, uint8_t col, float num, uint8_t dotLen);
void LCD_ShowHexNum(uint8_t line, uint8_t col, uint32_t num, uint8_t len);

/* 辅助函数 */
void LCD_WriteChar(char ch);
void LCD_WriteString(const char *str);

#endif /* __LCD_DRIVER_H */
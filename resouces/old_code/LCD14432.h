#ifndef __LCD_14432_ASYNC_H
#define __LCD_14432_ASYNC_H

#include "main.h" // 包含 HAL 库和引脚定义

/* --- 配置参数 --- */
#define  LCD_CS_GPIO_Port		GPIOB
#define  LCD_CS_Pin				GPIO_PIN_0
#define  LCD_BUF_SIZE			128			// 足够容纳两次全屏刷新内容

#define SCREEN_ROLL_HEIGHT		64			// 可以滚动的区域高度

/* --- 异步管理结构体 --- */
typedef struct
{
	uint8_t data[LCD_BUF_SIZE];
	uint8_t is_rs[LCD_BUF_SIZE]; // 0:命令, 1:数据
	uint16_t head;
	uint16_t tail;
	uint32_t last_tick;          // 上次发送的时间戳
} LCD_Queue_t;

typedef struct
{
	uint8_t scroll_en;       // 滚动使能标志
	uint16_t scroll_interval; // 滚动间隔时间(ms)
	uint32_t last_scroll_tick; // 上次滚动的时间戳
	uint8_t scroll_offset;   // 当前偏移地址 (0-63)
} LCD_Scroll_t;

/* --- 核心驱动接口 --- */

void LCD_Task_Handler(void);
void LCD_Init(void);

// 基础显示
void LCD_Clear(void);
void LCD_SetCursorPos(uint8_t line, uint8_t col);
uint8_t LCD_ShowStringWithAutoSpace(const char *str, uint8_t HAlign);
void LCD_ShowString(uint8_t line, uint8_t col, uint8_t HAlign, const char *str);

//// 数值显示
void LCD_ShowNum(uint8_t line, uint8_t col, uint32_t num, uint8_t len);
void LCD_ShowSignedNum(uint8_t line, uint8_t col, int32_t num, uint8_t len);
void LCD_ShowFloatNum(uint8_t line, uint8_t col, float num, uint8_t dotLen);
void LCD_ShowHexNum(uint8_t line, uint8_t col, uint32_t num, uint8_t len);

////// 滚动控制
void LCD_Scroll_Start(uint16_t interval_ms);
void LCD_Scroll_Stop(void);
void LCD_Scroll_Reset(uint8_t start_pos);

#endif

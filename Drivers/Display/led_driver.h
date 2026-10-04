#ifndef __LED_DRIVER_H
#define __LED_DRIVER_H

#include "main.h"
#include <stdint.h>

/* ── FLUSH 分频：完整扫 N 帧后才真正 swap（防撕裂） ── */
#define FLUSH_DIV 4

/* ── 屏幕规格 ────────────────────────────────────────── */
#define LED_WIDTH 160
#define LED_HEIGHT 16
#define LED_SCAN_ROWS 16
#define BYTES_PER_ROW (LED_WIDTH / 8) // 20

/* ── 双缓冲显存 ────────────────────────────────────── */
#define LED_BUF_COUNT 2
extern uint8_t frame_buffer[LED_BUF_COUNT][LED_SCAN_ROWS][BYTES_PER_ROW];
extern volatile uint8_t write_idx;

/* ── ISR 回调：由 stm32f1xx_it.c 调用 ──────────────── */
void LED_TIM2_IRQHandler(void);

/* ── API ───────────────────────────────────────────── */
void LED_Init(void);
void LED_DeInit(void);

void LED_SetPixel(int16_t x, int16_t y, uint8_t on);
void LED_Clear(void);
void LED_ClearAreaAllBuffers(int16_t x, int16_t y, uint16_t w, uint16_t h);
void LED_Fill(void);

void LED_Commit(void);

void LED_SetBrightness(uint8_t percent); // 0~100
void LED_SetScreenEnable(uint8_t enable);
void LED_SetHorizontalMirror(uint8_t enable);

#endif
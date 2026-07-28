#include "led_driver.h"
#include <string.h>

/* ── 外部句柄 ──────────────────────────────────────── */
extern TIM_HandleTypeDef htim1;
extern SPI_HandleTypeDef hspi1;

/* ── 显存：双缓冲 ──────────────────────────────────── */
uint8_t frame_buffer[LED_BUF_COUNT][LED_SCAN_ROWS][BYTES_PER_ROW]
    __attribute__((aligned(4)));
volatile uint8_t write_idx = 0;
static volatile uint8_t read_idx = 1;

/* ── 运行时状态 ────────────────────────────────────── */
static uint8_t s_current_row = 0;
static uint8_t s_led_enabled = 1;  // 1=亮, 0=灭
static uint8_t s_brightness = 100; // 0~100
static volatile uint8_t s_need_commit = 0;
static volatile uint8_t s_flush_counter = 0;
static uint8_t s_horizontal_mirror = 0;
static uint8_t s_mirror_row[BYTES_PER_ROW]; // 镜像临时行缓冲

/* ════════════════════════════════════════════════════
 * 字节内位反转（3 次分治法，约 6~8 指令）
 * ════════════════════════════════════════════════════ */
static inline uint8_t reverse_byte(uint8_t b) {
  b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
  b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
  b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
  return b;
}

/* ════════════════════════════════════════════════════
 * 行地址选择：A=PB0, B=PB1, C=PB10, D=PB11
 * ════════════════════════════════════════════════════ */
static void LED_UpdateRow(uint8_t row) {
  uint32_t set_mask = 0;
  uint32_t reset_mask = 0;

  if (row & 0x01)
    set_mask |= LED_LA_Pin;
  else
    reset_mask |= LED_LA_Pin;
  if (row & 0x02)
    set_mask |= LED_LB_Pin;
  else
    reset_mask |= LED_LB_Pin;
  if (row & 0x04)
    set_mask |= LED_LC_Pin;
  else
    reset_mask |= LED_LC_Pin;
  if (row & 0x08)
    set_mask |= LED_LD_Pin;
  else
    reset_mask |= LED_LD_Pin;

  LED_LA_GPIO_Port->BSRR = (reset_mask << 16) | set_mask;
}

/* ════════════════════════════════════════════════════
 * 绘图接口：写入 write_idx 缓冲区
 * ════════════════════════════════════════════════════ */
void LED_SetPixel(int16_t x, int16_t y, uint8_t on) {
  if (x < 0 || x >= LED_WIDTH || y < 0 || y >= LED_HEIGHT)
    return;

  uint8_t byte_idx = (uint8_t)(x / 8);
  uint8_t bit_pos = (uint8_t)(x % 8);
  uint8_t bit_mask = (uint8_t)(0x80 >> bit_pos);

  if (on)
    frame_buffer[write_idx][y][byte_idx] &= ~bit_mask; // 0 = 亮
  else
    frame_buffer[write_idx][y][byte_idx] |= bit_mask; // 1 = 灭
}

void LED_Clear(void) {
  memset(frame_buffer, 0xFF, sizeof(frame_buffer)); // 全 1 = 全灭
}

void LED_Fill(void) {
  memset(frame_buffer, 0x00, sizeof(frame_buffer)); // 全 0 = 全亮
}

/* ════════════════════════════════════════════════════
 * 内部：实际 swap 缓冲区（仅在 ISR 中调用）
 * ════════════════════════════════════════════════════ */
static inline void LED_SwapBuffers(void) {
  uint8_t old_read = read_idx;
  read_idx = write_idx;
  write_idx = old_read;
}

/* ════════════════════════════════════════════════════
 * 提交：仅标记请求，等 FLUSH_DIV 帧后才真正 swap
 * ════════════════════════════════════════════════════ */
void LED_Commit(void) { s_need_commit = 1; }

/* ════════════════════════════════════════════════════
 * TIM1 更新中断：行刷新入口
 *   1. OE=OFF
 *   2. 更新行地址
 *   3. LAT 脉冲（锁存上一轮移入的数据）
 *   4. SPI DMA 发送下一行数据
 * ════════════════════════════════════════════════════ */
void LED_TIM1_IRQHandler(void) {
  /* ── 清除中断标志 ── */
  __HAL_TIM_CLEAR_FLAG(&htim1, TIM_FLAG_UPDATE);

  /* ── 1. OE=OFF（消隐） ── */
  LED_EN_GPIO_Port->BSRR = LED_EN_Pin;

  /* ── 2. 更新行地址 ── */
  LED_UpdateRow(s_current_row);

  /* ── 3. LAT 脉冲：锁存上一轮移入的数据 ── */
  LED_LAT_GPIO_Port->BSRR = LED_LAT_Pin;
  __NOP();
  __NOP();
  LED_LAT_GPIO_Port->BSRR = (uint32_t)LED_LAT_Pin << 16;

  /* ── 4. FLUSH 计数：完整扫完一帧（row 绕回 0）才判断 ── */
  if (s_current_row == LED_SCAN_ROWS - 1) {
    s_flush_counter++;
    if (s_need_commit) {
      if (s_flush_counter >= FLUSH_DIV) {
        LED_SwapBuffers();
        s_flush_counter = 0;
        s_need_commit = 0;
      }
    }
  }

  /* ── 5. SPI DMA 发送下一行数据 ── */
  uint8_t next_row = (s_current_row + 1) & (LED_SCAN_ROWS - 1);
  s_current_row = next_row;

  if (s_horizontal_mirror) {
    /* 镜像模式：字节翻转 + 位反转 */
    for (int i = 0; i < BYTES_PER_ROW; i++) {
      s_mirror_row[i] =
          reverse_byte(frame_buffer[read_idx][next_row][BYTES_PER_ROW - 1 - i]);
    }
    HAL_SPI_Transmit_DMA(&hspi1, s_mirror_row, BYTES_PER_ROW);
  } else {
    HAL_SPI_Transmit_DMA(&hspi1, &frame_buffer[read_idx][next_row][0],
                         BYTES_PER_ROW);
  }
}

/* ════════════════════════════════════════════════════
 * SPI1 TX 完成回调：开 OE + 启动 TIM2 调光
 * ════════════════════════════════════════════════════ */
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi) {
  if (hspi->Instance != SPI1)
    return;
  if (!s_led_enabled || s_brightness == 0)
    return;

  /* OE=ON（显示当前行） */
  LED_EN_GPIO_Port->BSRR = (uint32_t)LED_EN_Pin << 16;

  if (s_brightness < 100) {
    /* 启动 TIM2 单脉冲：到期关 OE */
    TIM2->CR1 &= ~TIM_CR1_CEN;
    TIM2->SR = 0;
    TIM2->ARR = (uint32_t)(625UL * s_brightness / 100);
    TIM2->CNT = 0;
    TIM2->CR1 |= TIM_CR1_CEN;
  }
  /* brightness == 100: OE 持续亮到下一行中断 */
}

/* ════════════════════════════════════════════════════
 * TIM2 更新中断：关 OE（亮度控制截止）
 * ════════════════════════════════════════════════════ */
void LED_TIM2_IRQHandler(void) {
  if (TIM2->SR & TIM_SR_UIF) {
    TIM2->SR = ~TIM_SR_UIF;
    LED_EN_GPIO_Port->BSRR = LED_EN_Pin; // OE=OFF
  }
}

/* ════════════════════════════════════════════════════
 * 初始化
 * ════════════════════════════════════════════════════ */
void LED_Init(void) {
  /* 清显存：全灭 */
  memset(frame_buffer, 0xFF, sizeof(frame_buffer));

  s_current_row = 0;
  LED_UpdateRow(s_current_row);

  /* ── 初始引脚状态 ── */
  HAL_GPIO_WritePin(LED_EN_GPIO_Port, LED_EN_Pin, GPIO_PIN_SET);     // OE=OFF
  HAL_GPIO_WritePin(LED_LAT_GPIO_Port, LED_LAT_Pin, GPIO_PIN_RESET); // LAT=LOW

  /* ── TIM2 重配置：亮度 PWM ──
   *  72MHz / (71+1) = 1MHz → 1 tick = 1μs
   *  OPM 模式：计到 ARR 自动停
   */
  TIM2->PSC = 71;
  TIM2->EGR |= TIM_EGR_UG; // 装入 PSC
  TIM2->SR = 0;
  TIM2->ARR = 625; // 最大一行周期
  TIM2->CNT = 0;
  TIM2->CR1 &= ~TIM_CR1_CEN;  // 先停
  TIM2->CR1 &= ~TIM_CR1_ARPE; // ARR 立即生效
  TIM2->CR1 |= TIM_CR1_OPM;   // 单脉冲
  TIM2->DIER |= TIM_DIER_UIE; // 使能更新中断
  // TIM2 NVIC 已在 CubeMX 中使能

  /* ── 启动 TIM1 行刷新 ── */
  HAL_TIM_Base_Start_IT(&htim1);
}

/* ════════════════════════════════════════════════════
 * 反初始化
 * ════════════════════════════════════════════════════ */
void LED_DeInit(void) {
  HAL_TIM_Base_Stop_IT(&htim1);
  TIM2->CR1 &= ~TIM_CR1_CEN;
  HAL_SPI_DMAStop(&hspi1);
  LED_EN_GPIO_Port->BSRR = LED_EN_Pin; // OE=OFF
}

/* ════════════════════════════════════════════════════
 * 控制接口
 * ════════════════════════════════════════════════════ */
void LED_SetScreenEnable(uint8_t enable) {
  s_led_enabled = enable;
  if (!enable) {
    LED_EN_GPIO_Port->BSRR = LED_EN_Pin; // OE=OFF
  }
}

void LED_SetBrightness(uint8_t percent) {
  if (percent > 100)
    percent = 100;
  s_brightness = percent;
}

void LED_SetHorizontalMirror(uint8_t enable) { s_horizontal_mirror = enable; }
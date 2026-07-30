#include "led_driver.h"
#include "system_utils.h"
#include <string.h>

/* ── 外部句柄 ──────────────────────────────────────── */
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
volatile uint8_t need_commit = 0;  // 非 static，供 window_manager 外部访问
static volatile uint8_t s_flush_counter = 0;
static uint8_t s_horizontal_mirror = 0;
static uint8_t s_mirror_row[BYTES_PER_ROW]; // 镜像临时行缓冲

/* ── DWT 动态测量 ──────────────────────────────────── */
static volatile uint32_t s_dwt_start = 0;
static volatile uint16_t s_spi_line_ticks = 0; // 单行 SPI 传输实测值(μs)
static volatile uint8_t s_line_measured = 0;

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
 * 内部：启动指定行的 SPI DMA 发送
 * 返回 HAL_OK 成功，否则失败
 * ════════════════════════════════════════════════════ */
static HAL_StatusTypeDef LED_StartRowSPI(uint8_t row) {
  if (s_horizontal_mirror) {
    /* 镜像模式：字节翻转 + 位反转 */
    for (int i = 0; i < BYTES_PER_ROW; i++) {
      s_mirror_row[i] =
          reverse_byte(frame_buffer[read_idx][row][BYTES_PER_ROW - 1 - i]);
    }
    return HAL_SPI_Transmit_DMA(&hspi1, s_mirror_row, BYTES_PER_ROW);
  } else {
    return HAL_SPI_Transmit_DMA(&hspi1, &frame_buffer[read_idx][row][0],
                                BYTES_PER_ROW);
  }
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

/** 清除指定物理区域在两个双缓冲中的像素 */
void LED_ClearAreaAllBuffers(int16_t x, int16_t y, uint16_t w, uint16_t h) {
  for (int buf = 0; buf < LED_BUF_COUNT; buf++) {
    for (uint16_t row = 0; row < h; row++) {
      int16_t sy = y + (int16_t)row;
      if (sy < 0 || sy >= LED_HEIGHT)
        continue;
      uint8_t sr = (uint8_t)(sy & (LED_SCAN_ROWS - 1));
      uint8_t *fb_row = frame_buffer[buf][sr];
      for (int16_t col = 0; col < (int16_t)w; col += 8) {
        int16_t sx = x + col;
        if (sx >= LED_WIDTH)
          break;
        if (sx < 0)
          continue;
        fb_row[(uint16_t)(sx >> 3)] = 0xFF; // 全灭
      }
    }
  }
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
void LED_Commit(void) { need_commit = 1; }

/* ════════════════════════════════════════════════════
 * SPI1 TX 完成回调：【自我闭环】行刷新入口
 *
 *  注意：该屏幕 OE 为高电平有效！
 *      OE = 高 → 亮屏显示
 *      OE = 低 → 灭屏消隐
 *
 *   ① 进中断先消隐(拉低OE)，保证换行锁存期间不产生鬼影
 *   ② DWT 首次动态测量单行 SPI 传输耗时
 *   ③ LAT 脉冲锁存上一轮数据
 *   ④ 更新行地址 + FLUSH 帧切换
 *   ⑤ 立即发起下一行 SPI DMA
 *   ⑥ 开启显示(拉高OE)
 *   ⑦ 若亮度<100%，启动 TIM2 单脉冲到期关 OE
 *
 *   完全替代了原来的 TIM1 中断驱动方式。
 * ════════════════════════════════════════════════════ */
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi) {
  if (hspi->Instance != SPI1)
    return;

  /* ── 1. 消隐：拉低 OE (低电平 = 灭屏) ──
   *     保证换行和锁存期间不产生鬼影
   */
  LED_EN_GPIO_Port->BSRR = (uint32_t)LED_EN_Pin << 16;

  /* ── 2. DWT 动态测量（仅首次） ──
   *     CPU = 72MHz → 1 DWT tick = 1/72 μs
   *     s_spi_line_ticks = cycles / 72  → 换算为微秒
   */
  if (!s_line_measured) {
    uint32_t cycles = DWT->CYCCNT - s_dwt_start;
    s_spi_line_ticks = (uint16_t)(cycles / 72);
    if (s_spi_line_ticks < 5)
      s_spi_line_ticks = 10;
    if (s_spi_line_ticks > 500)
      s_spi_line_ticks = 100;
    s_line_measured = 1;
  }

  /* ── 3. LAT 脉冲：锁存上一轮 SPI 移入的数据 ──
   *     此时锁存器中的数据 = 刚完成传输的行（s_current_row）的数据
   */
  LED_LAT_GPIO_Port->BSRR = LED_LAT_Pin;
  __NOP();
  __NOP();
  LED_LAT_GPIO_Port->BSRR = (uint32_t)LED_LAT_Pin << 16;

  /* ═══════════════════════════════════════════════════════
   * 【核心修复】只切一次行——切到刚刚发完的那一行
   *
   *   LAT 锁存了 s_current_row 的数据，将行地址设置到
   *   s_current_row，然后开启 OE 显示。保持行地址不变，
   *   同时 SPI 传输下一行数据（进入移位寄存器，不影响显示）。
   *
   *   ❌ 错误做法：先在 s_current_row 开 OE，再切到 next_row
   *      并发起 SPI → 新行地址会错误地显示锁存器中的旧数据
   *      （例如：row[0] 显示 row[15]）
   *
   *   正确时序：
   *   ┌─────────────────────────────────────────────┐
   *   │ ① OE=OFF 消隐                               │
   *   │ ② LAT 锁存 row[N] 数据                      │
   *   │ ③ 行地址 = row[N]（刚完成传输的行）         │
   *   │ ④ OE=ON → 显示 row[N]                      │
   *   │ ⑤ s_current_row = N+1（逻辑更新，不切GPIO）│
   *   │ ⑥ SPI DMA 传输 row[N+1]（移入移位寄存器）  │
   *   │    → row[N] 继续显示，SPI 在后台搬运        │
   *   └─────────────────────────────────────────────┘
   * ═══════════════════════════════════════════════════ */

  /* ── 4. 设置行地址 = 刚完成传输的行，并开启 OE 显示 ── */
  LED_UpdateRow(s_current_row);

  if (s_led_enabled && s_brightness > 0) {
    LED_EN_GPIO_Port->BSRR = LED_EN_Pin; // OE = ON (高电平亮屏)

    /* ── 亮度 PWM ── */
    if (s_brightness < 100) {
      TIM2->CR1 &= ~TIM_CR1_CEN;
      TIM2->SR = 0;

      uint32_t arr = (uint32_t)s_spi_line_ticks * s_brightness / 100;
      if (arr < 5)
        arr = 5;

      TIM2->ARR = arr;
      TIM2->CNT = 0;
      TIM2->CR1 |= TIM_CR1_CEN;
    }
    /* brightness == 100: OE 持续亮到下一行回调再次拉低 */
  }

  /* ── 5. FLUSH 帧切换检测 ── */
  if (s_current_row >= LED_SCAN_ROWS - 1) {
    s_flush_counter++;
    if (need_commit) {
      if (s_flush_counter >= FLUSH_DIV) {
        LED_SwapBuffers();
        s_flush_counter = 0;
        need_commit = 0;
      }
    }
  }

  /* ── 6. 逻辑更新行号指向下一行（不切换 GPIO 行地址） ──
   *     行地址保持当前行不变，显示当前行数据，
   *     同时 SPI DMA 传输下一行数据进入移位寄存器。
   *     下次回调时再切行地址。
   */
  s_current_row = (s_current_row + 1) & (LED_SCAN_ROWS - 1);

  /* ── 7. 发起下一行 SPI DMA ──
   *     行地址保持当前行不动，SPI 将下行数据移入移位寄存器
   */
  LED_StartRowSPI(s_current_row);
}

/* ════════════════════════════════════════════════════
 * TIM2 更新中断：关 OE（亮度控制截止）
 *
 *  屏幕 OE 为高电平有效，到期需拉低 OE 以关屏消隐。
 *
 * 【关键修复】清除 UIF 标志后添加 Dummy Read，
 * 防止 APB 总线延迟导致刚刚退出 ISR 又立马重入，
 * 从而避免主程序和串口彻底卡死。
 * ════════════════════════════════════════════════════ */
void LED_TIM2_IRQHandler(void) {
  if (TIM2->SR & TIM_SR_UIF) {
    TIM2->SR = ~TIM_SR_UIF;
    (void)TIM2->SR; // 【关键】Dummy Read，防止总线延迟导致重复进中断
    LED_EN_GPIO_Port->BSRR = (uint32_t)LED_EN_Pin << 16; // OE=OFF (拉低关屏)
  }
}

/* ════════════════════════════════════════════════════
 * 初始化
 *
 * 重构要点：
 *   1. 完全移除 TIM1 依赖
 *   2. 加入 DWT 测量起点，用于后续动态计算行耗时
 *   3. 增加安全重试循环确保 SPI DMA 启动成功
 *   4. OE 初始为低电平（灭屏消隐），适配高有效屏幕
 * ════════════════════════════════════════════════════ */
void LED_Init(void) {
  /* 清显存：全灭 */
  memset(frame_buffer, 0xFF, sizeof(frame_buffer));

  s_current_row = 0;
  LED_UpdateRow(s_current_row);

  /* ── 初始引脚状态 ──
   *   OE 初始为低电平 = 灭屏消隐（屏幕高电平有效）
   */
  HAL_GPIO_WritePin(LED_EN_GPIO_Port, LED_EN_Pin, GPIO_PIN_RESET); // OE=0(灭屏)
  HAL_GPIO_WritePin(LED_LAT_GPIO_Port, LED_LAT_Pin, GPIO_PIN_RESET); // LAT=LOW

  /* ── TIM2 重配置：亮度 PWM ──
   *  72MHz / (71+1) = 1MHz → 1 tick = 1μs
   *  OPM 模式：计到 ARR 自动停
   */
  TIM2->PSC = 71;
  TIM2->EGR |= TIM_EGR_UG; // 装入 PSC
  TIM2->SR = 0;
  TIM2->ARR = 625; // 初始占位值，实测后会动态覆盖
  TIM2->CNT = 0;
  TIM2->CR1 &= ~TIM_CR1_CEN;  // 先停
  TIM2->CR1 &= ~TIM_CR1_ARPE; // ARR 立即生效
  TIM2->CR1 |= TIM_CR1_OPM;   // 单脉冲
  TIM2->DIER |= TIM_DIER_UIE; // 使能更新中断
  // TIM2 NVIC 已在 CubeMX 中使能

  /* ── 默认开启水平镜像（硬件布线镜像补偿） ── */
  s_horizontal_mirror = 1;

  /* ── 记录 DWT 测量起点 ──
   *      在启动第 0 行 DMA 发送前记录周期数
   *      首次 HAL_SPI_TxCpltCallback 中计算耗时
   */
  s_dwt_start = DWT->CYCCNT;

  /* ── 启动第一轮传输（第 0 行），安全重试 ──
   *      防止在 SPI 或 DMA 尚未完全就绪时启动失败
   */
  while (LED_StartRowSPI(0) != HAL_OK) {
    /* 等待 SPI 和 DMA 就绪 */
  }
}

/* ════════════════════════════════════════════════════
 * 反初始化
 *
 * 移除了 TIM1 的停止操作（已完全解耦）
 *  OE 拉低 = 关屏消隐（适配高有效屏幕）
 * ════════════════════════════════════════════════════ */
void LED_DeInit(void) {
  TIM2->CR1 &= ~TIM_CR1_CEN;
  HAL_SPI_DMAStop(&hspi1);
  LED_EN_GPIO_Port->BSRR = (uint32_t)LED_EN_Pin << 16; // OE=OFF (拉低关屏)
}

/* ════════════════════════════════════════════════════
 * 控制接口
 *
 * 注意：屏幕 OE 为高电平有效：
 *   OE=高 → 亮屏
 *   OE=低 → 灭屏消隐
 * ════════════════════════════════════════════════════ */
void LED_SetScreenEnable(uint8_t enable) {
  s_led_enabled = enable;
  if (!enable) {
    LED_EN_GPIO_Port->BSRR = (uint32_t)LED_EN_Pin << 16; // OE=OFF (拉低关屏)
  }
}

void LED_SetBrightness(uint8_t percent) {
  if (percent > 100)
    percent = 100;
  s_brightness = percent;
}

void LED_SetHorizontalMirror(uint8_t enable) { s_horizontal_mirror = enable; }
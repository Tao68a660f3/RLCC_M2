#include "system_utils.h"

/* --- 初始化 DWT (兼容 F103) --- */
void DWT_Init(void) {
  /* 使能 Trace 调试模块 */
  // if (!(CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk)) {
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  // }

  /* 重置 CYCCNT 计数器 */
  DWT->CYCCNT = 0;

  /* 开启 CYCCNT 计数器 (bit 0) */
  // #ifdef DWT_CTRL_CYCCNTENA_Msk
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  // #else
  //   DWT->CTRL |= (1UL << 0); // 兼容某些 CMSIS 头文件未定义该宏的情况
  // #endif
}

/* --- 精准微秒延迟 --- */
void delay_us(uint32_t us) {
  uint32_t start = DWT->CYCCNT;
  /* F103 主频为 72MHz 时，ticks = us * 72 */
  uint32_t ticks = us * (SystemCoreClock / 1000000UL);

  /* 无符号减法利用溢出特性，即便 CYCCNT 翻转也能正确计算耗时 */
  while ((DWT->CYCCNT - start) < ticks)
    ;
}
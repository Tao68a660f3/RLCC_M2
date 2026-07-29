#ifndef __SYSTEM_UTILS_H
#define __SYSTEM_UTILS_H

#include "stm32f1xx_hal.h" // 修改为 F1 的 HAL 库头文件

void DWT_Init(void);
void delay_us(uint32_t us);

#endif /* __SYSTEM_UTILS_H */
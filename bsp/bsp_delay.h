#ifndef __BSP_DELAY_H
#define __BSP_DELAY_H

#include "ht32.h"

/* ==================== BSP 通用延时服务（P1 集中，2026-09-03） ====================
 * Delay() 原在 spi_lcd.c 实现/声明，归为板级服务后迁到本模块（spi_lcd.h 仍
 * include 本头，旧调用点无需改）。基于 BFTM0 1ms 节拍计数器 bftm0_ct。
 */

void Delay(u32 ms);   /* 通用毫秒阻塞延时（基于 BFTM0 1ms 节拍；休眠唤醒后 BFTM0 恢复前不可用） */

#endif /* __BSP_DELAY_H */

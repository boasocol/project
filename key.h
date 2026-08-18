/************************************************************************************************************
 * @file    key.h
 * @brief   按键功能模块：管理 PB12 物理按键（短按切换 关屏 / 开屏）。
 *
 * 关屏流程（key_screen_off）：
 *   1) ST7789 DISPOFF（关显示，等 ~10 帧）
 *   2) 背光 PB4 拉高（高电平关背光）
 *   3) ST7789 SLPIN（睡眠模式：驱动 IC 电流数 mA -> μA 级，屏幕功耗大头）
 *   4) 蓝牙 HD9803 断电（Hd9803_SetPower(0)，PA15 拉高）
 *   5) Flash 深度掉电（0xB9：待机 ~15μA -> 掉电 ~1μA）
 *   6) PWRCU_DeepSleep1(WFI) 进入深度休眠1（LSI on/RTC 走时，电流 ~1.15μA）
 *
 * 唤醒：深度休眠1（库 PWRCU_DeepSleep1）下 PB12 通过 EXTI 低电平唤醒事件经 EVWUP
 *       中断线直接唤醒 WFI。同时保留 RTC CSEC(每 0.5s)作兜底心跳。
 *       唤醒后主循环读 PB12 判断按键：按下则开屏，否则继续 Deep-Sleep。
 *
 * 开屏流程（key_screen_on）：
 *   1) 背光 PB4 拉低（亮）
 *   2) Flash 退出掉电（0xAB，之后才可读字库/背景）
 *   3) ST7789 SLPOUT（退睡眠，等 120ms）-> DISPON（开显示）
 *   4) Calendar_SetStaticNext() 强制下一整秒全量重绘（不做动画）
 *   5) 蓝牙重新上电（状态机自行恢复）
 *
 * 注意：
 *   - 主循环“后半秒静态刷新”在关屏状态不执行（Display_Update 里 Key_IsScreenOff 判断）
 *   - 唤醒后程序继续执行：无需重新初始化，RAM 缓存保留
 *   - 进/出低功耗一律走库函数，不直接操作 PWRCU/CKCU/SCB 寄存器
 ************************************************************************************************************/
#ifndef __KEY_H
#define __KEY_H

#include "ht32.h"

void Key_Init(void);          /* 配置 PB12 输入 + EXTI12 下降沿 + 唤醒事件（上电一次） */
void Key_Process(void);       /* 主循环调用：检测按键 -> 关屏/开屏 + 深度休眠1 */
int  Key_IsScreenOff(void);   /* 查询是否处于关屏状态：1=关屏，0=正常 */
void Key_HandleIrq(void);     /* EXTI4_15 中断入口（PB12 按键/唤醒，由中断文件调用） */
void Key_ClearWakeup(void);   /* EVWUP 中断入口调用：清唤醒/边沿标志并暂关 PB12 低电平唤醒事件 */

/* 深度休眠测试标志（供 main 调试打印）：1=已进入深度休眠，0=已退出开屏 */
extern volatile u8 g_key_deep_sleep_flag;

#endif

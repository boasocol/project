/************************************************************************************************************
 * @file    key.h
 * @brief   按键模块（Driver 层）：PB12/PB13/PB14 三物理按键，产出统一事件流。
 *
 * 职责边界（2026-09-05 重构：按键事件流化）：
 *   - 本模块只做【物理采集 + 事件判定 + 系统低功耗】：EXTI 下降沿 -> 三键统一长/短按判定 ->
 *     KeyEvent{键号, 事件类型} 入队；Key_GetEvent 出队供应用层消费。
 *   - 本模块【不包含任何业务语义】（“哪个键长按/短按在当前状态干什么”由应用层
 *     Screen_HandleKey 按系统状态集中分发，改功能只动应用层、不看驱动）。
 *   - 关屏/开屏/深休眠是系统级动作（WFI/外设时钟/LCD 电源/Flash 掉电），仍在本模块；
 *     但【何时关屏】的决策在应用层——应用层收到 KEY2 长按事件后调 Key_ScreenOff()。
 *
 * 事件模型：
 *   KeyEvent.id  = KEY_ID_1/2/3（对应 bsp_pins.h KEY1..KEY3 = PB12/PB13/PB14）
 *   KeyEvent.evt = KEY_EV_SHORT（按下后 KEY_LONG_MS(1000ms) 内松开）
 *                / KEY_EV_LONG （按住 ≥KEY_LONG_MS(1000ms)）
 *   三键统一判定（key.c key_scan），应用层决定各键短/长按在当下状态做什么。
 *   消抖（2026-09-05）：硬件 EXTI 消抖（DebounceCnt=40）+ 软件稳定窗口
 *   （电平连续稳定 ≥20ms 才判定，key.c KEY_DEBOUNCE_MS），一次物理按压只产出一个事件。
 *
 * 按键资源（bsp_pins.h KEY*）：
 *   KEY1 = PB12（EXTI12，兼低功耗唤醒）—— 应用层：设置模式【减】；唤醒仍走 KEY1
 *   KEY2 = PB13（EXTI13）—— 应用层：短按【选择/下一项】；长按【关屏/深休眠1】
 *   KEY3 = PB14（EXTI14）—— 应用层：设置模式【加】
 *   低功耗唤醒：深休眠1 下 KEY1(PB12) 经 EXTI 低电平唤醒事件 + EVWUP 唤醒 WFI，
 *   唤醒后自动开屏并短暂丢弃按键防电平残留误触（KEY2/KEY3 暂不参与唤醒）。
 *
 * 关屏流程（Key_ScreenOff，应用层 KEY2 长按触发）：
 *   1) ST7789 DISPOFF（关显示，等 ~10 帧）  2) 背光 PB4 拉高（灭）
 *   3) ST7789 SLPIN（睡眠模式，μA 级）      4) 关屏回调（Key_SetScreenOffCb：蓝牙断电等）
 *   5) Flash 深度掉电（0xB9）              6) PWRCU_DeepSleep1(WFI) 深度休眠1（~1.15μA）
 *   开屏流程（Key_Process 唤醒后自动）：恢复时钟 -> 背光亮 -> Flash 退掉电 -> SLPOUT/DISPON -> 开屏回调
 *
 * P2-3（2026-09-03）分层反转：本模块不 include 应用层头，关屏/开屏副作用经回调上抛。
 ************************************************************************************************************/
#ifndef __KEY_H
#define __KEY_H

#include "ht32.h"

/* ---- 键号（对应 bsp_pins.h KEY1..KEY3 = PB12/PB13/PB14；与 key.c g_keys[] 下标一一对应） ---- */
typedef enum {
  KEY_ID_1 = 0,   /* PB12（EXTI12，兼低功耗唤醒） */
  KEY_ID_2 = 1,   /* PB13（EXTI13） */
  KEY_ID_3 = 2,   /* PB14（EXTI14） */
} key_id_t;

/* ---- 事件类型 ---- */
typedef enum {
  KEY_EV_SHORT = 0,   /* 短按：按下后 KEY_LONG_MS(1000ms) 内松开 */
  KEY_EV_LONG  = 1,   /* 长按：按住 ≥KEY_LONG_MS(1000ms) */
  KEY_EV_REPEAT = 2,  /* 连发：长按触发后按住期间每 KEY_REPEAT_MS(180ms) 持续发（快加快减用，2026-09-05） */
} key_evt_t;

/* ---- 统一按键事件（Key_GetEvent 出队；应用层按 id+evt+当前状态分发功能） ---- */
typedef struct {
  key_id_t  id;     /* 哪个键（KEY_ID_1/2/3） */
  key_evt_t evt;    /* 短按 / 长按 */
} KeyEvent;

/* ---- 应用层副作用回调（P2-3 反转：key.c 不 include 应用层头） ---- */
typedef void (*KeyEventCb)(void);
void Key_SetScreenOffCb(KeyEventCb cb);  /* 进入关屏前调用（应用层可做蓝牙断电等；未注册则跳过） */
void Key_SetScreenOnCb(KeyEventCb cb);   /* 开屏完成后调用（应用层强制重绘 + 蓝牙上电；未注册则跳过） */

void Key_Init(void);          /* 配置 PB12/PB13/PB14 输入 + EXTI 下降沿 + 唤醒事件（上电一次） */
int  Key_BootHoldDetect(void); /* 上电「左+右同按」检测：1=正在外部烧录 Flash（main 据此跳过全部初始化）。
                                  只配置 PB12/PB14，不碰 SPI1 的 PA0..PA3（2026-11-23） */
void Key_Process(void);       /* 主循环调用：三键长/短按判定入队 + 关屏态兜底深休眠 + 唤醒开屏 */
int  Key_IsScreenOff(void);   /* 查询是否处于关屏状态：1=关屏，0=正常 */
int  Key_GetEvent(KeyEvent *out); /* 出队一个按键事件；返回 1=有事件(*out 有效)，0=空 */
void Key_ScreenOff(void);     /* 应用层调用（KEY2 长按）：执行关屏/深休眠1 全流程；阻塞至按键唤醒开屏 */
void Key_HandleIrq(void);     /* EXTI4_15 中断入口（三键边沿置待判定标志，由中断文件调用） */
void Key_ClearWakeup(void);   /* EVWUP 中断入口：清唤醒标志并暂关 KEY1 低电平唤醒事件 */

/* 深度休眠测试标志（供 main 调试打印）：1=已进入深度休眠，0=已退出开屏 */
extern volatile u8 g_key_deep_sleep_flag;

#endif

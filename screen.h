/************************************************************************************************************
 * @file    screen.h
 * @brief   多屏状态机：主界面 / 倒计时（屏1）/ 闹钟（屏2）+ 倒计时&闹钟业务状态机
 *
 * 背景多屏（2026-08-27）：Flash UI 区每屏 150KB（UI_OFF + 屏号×153600），
 * 切屏 = UI_SetScreen（ui.c）上传对应背景 + 本模块按当前屏分发动态绘制。
 *
 * 倒计时（屏1，MM:SS 大数字 + 状态chip + 进度条 + 开始/暂停/重置）：
 *   - 0x84 分钟倒计时(d0=1-60)/0x88 番茄(25min) 启动 -> 切屏1
 *   - 0x85 暂停 / 0x86 继续 / 0x87 结束 / 0x89 关番茄（结束停 3s 回主屏）
 *   - 到 0：播 VOICE_TX_TIMER_BEEP 提示音 + 停 3s 回主屏
 *
 * 闹钟（屏2，HH:MM 大数字 + 开关chip + 星期chip + 强调线）：
 *   - 0x8A 设闹钟 -> UI_SetAlarm + 切屏2 显示
 *   - 0x8B 开/关、0x8C 循环模式(0单次/1一~五/2一~六/3一~日)、0x8D 铃音
 *   - 到点响铃：切屏2 + VOICE_TX_RING + 强调线红（0xB0 贪睡 +10min / 0xB1 别响 / 60s 自动停）
 ************************************************************************************************************/
#ifndef __SCREEN_H
#define __SCREEN_H

#include "ht32.h"
#include "time_module.h"
#include "voice.h"   /* VoiceFrame / Voice_Send */

/* ---- 倒计时状态 ---- */
typedef enum {
  CNT_STOP  = 0,   /* 空闲（未启动） */
  CNT_RUN   = 1,   /* 运行中（绿） */
  CNT_PAUSE = 2,   /* 暂停（黄） */
  CNT_DONE  = 3,   /* 结束/停止（红，停 3s 后回主屏） */
} cnt_state_t;

/* ---- API ---- */
void Screen_Init(void);                /* 上电：回主屏（UI_SetScreen(MAIN)，由 Calendar_Init 后调用） */
void Screen_Draw(TimeStruct *t);       /* 每帧：按当前屏增量绘制（主屏委托 calendar，倒计时/闹钟本模块） */
void Screen_OnSecond(TimeStruct *t);   /* 每秒：倒计时推进 + 闹钟到点判定（任何屏都跑） */
int  Screen_HandleVoice(const VoiceFrame *f); /* 语音命令：倒计时/闹钟/切屏；返回 1=已处理 */
void Screen_ReturnMain(void);          /* 15s 无命令等超时回主屏（倒计时运行/暂停中不切走） */

/* ---- 倒计时控制（voice 调用） ---- */
void Screen_CntStart(u16 total_s);     /* 启动倒计时并切屏1（total_s>0） */
void Screen_CntPause(void);            /* 0x85 */
void Screen_CntResume(void);           /* 0x86 */
void Screen_CntStop(void);             /* 0x87/0x89：停 3s 回主屏 */
int  Screen_CntActive(void);           /* 1=运行或暂停（Screen_ReturnMain 守卫） */

/* ---- 闹钟控制（voice 调用） ---- */
void Screen_AlarmSet(u8 hh, u8 mm);    /* 0x8A 设闹钟：写配置 + 切屏2 */
void Screen_AlarmSwitch(u8 on);        /* 0x8B 开/关 */
void Screen_AlarmLoop(u8 mode);        /* 0x8C 循环模式 0/1/2/3 */
void Screen_AlarmRingStop(void);       /* 0xB1 别响了 / 0x8B 关 / 60s 自动停 */

#endif /* __SCREEN_H */

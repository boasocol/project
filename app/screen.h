/************************************************************************************************************
 * @file    screen.h
 * @brief   多屏状态机：主界面 / 倒计时（屏1）/ 闹钟（屏2）+ 倒计时&闹钟业务状态机
 *
 * 背景多屏（2026-08-27）：Flash UI 区每屏 150KB（UI_OFF + 屏号×153600），
 * 切屏 = UI_SetScreen（ui.c）上传对应背景 + 本模块按当前屏分发动态绘制。
 *
 * 倒计时（屏1，MMM:SS 大数字 + 状态chip + 进度条 + 开始/暂停/重置；坑#93 分钟 3 位支持 120 分钟）：
 *   - 0x84 分钟倒计时(d0=1-120，坑#93)/0x88 番茄(25min) 启动 -> 切屏1
 *   - 0x85 暂停 / 0x86 继续 / 0x87 结束 / 0x89 关番茄（结束停 3s 回主屏）
 *   - 到 0：播 VOICE_TX_TIMER_BEEP 提示音 + 停 3s 回主屏
 *
 * 正计时（2026-08-28，复用屏1 MM:SS 从 0 往上走，与倒计时互斥）：
 *   - 0xBA 打开正计时 -> 切屏1 从 00:00 递增；0x85/0x86 暂停/继续，0x87 结束停 3s 回主屏
 *
 * 闹钟（屏2，HH:MM 大数字 + 槽号chip + 开关chip + 星期chip + 强调线）：
 *   - 3 个闹钟槽（2026-09-05，运行时内存态，槽1 初始取 Flash 配置）
 *   - 0x8A 设闹钟(n=1-3,时,分) -> 写槽内存 + 切屏2；0x8B 开/关、0x8C 循环模式(0/1/2/3)、0x8D 铃音
 *   - 0xB9 查看闹钟 -> 切屏2 显示当前槽配置（仅切屏+回 OK，不播报）
 *   - 到点响铃：切屏2 + VOICE_TX_RING + 强调线红（0xB0 贪睡 +10min / 0xB1 别响 / 60s 自动停）
 *
 * 按键交互（2026-09-05，Screen_HandleKey 按屏分流）：
 *   主屏=时间设置模式（KEY2 进设置/切项/哨兵退出，KEY1 减/KEY3 加）；
 *   倒计时屏=选择光标常驻（分/秒/暂停/重置，左/右切换，确定：暂停/重置直接生效、
 *     分/秒进出调整态自动暂停、光标黄↔蓝）；闹钟屏=空闲确定进设置（闹钟选择/开关/循环，
 *     黄线三项+哨兵退出），响铃中确定=停铃；长按 KEY2 关屏。
 *
 * 其他（2026-08-28）：0xB4/0xB5 显示开关（DISPOFF/ON + 背光）、0xB8 整点报时开关（内存开关，整点播报 0x07）
 ************************************************************************************************************/
#ifndef __SCREEN_H
#define __SCREEN_H

#include "ht32.h"
#include "time_module.h"
#include "voice.h"   /* VoiceFrame / Voice_Send */
#include "key.h"     /* KeyEvent{键号,短/长按}：Screen_HandleKey 事件分发（2026-09-05） */

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
void Screen_Poll(void);                 /* 每轮处理计时结束期限，各页面均生效 */
void Screen_OnSecond(TimeStruct *t);   /* 每秒：倒计时推进 + 闹钟到点判定（任何屏都跑） */
int  Screen_HandleVoice(const VoiceFrame *f); /* 语音命令：倒计时/闹钟/切屏；返回 1=已处理(回OK) 0=未处理(继续基础域/回FAIL) 2=响闹中静默拦截(无回应，坑#76) */
void Screen_ReturnMain(void);          /* 15s 无命令(0xEF)等超时回主屏（倒计时运行/暂停不回；倒计时屏进屏 15s 内不回，坑#80） */
void Screen_GoMain(void);              /* 语音设时/设日成功回主屏（坑#77 #1；不受倒计时屏 15s 空闲限制，仍保护倒计时活动/设置态/响铃） */

/* ---- 倒计时/正计时控制（voice 调用） ---- */
void Screen_CntStart(u16 total_s);     /* 启动倒计时并切屏1（total_s>0） */
void Screen_CntPause(void);            /* 0x85 */
void Screen_CntResume(void);           /* 0x86 */
void Screen_CntStop(void);             /* 0x87/0x89：停 3s 回主屏 */
int  Screen_CntActive(void);           /* 1=运行或暂停（Screen_ReturnMain 守卫） */
void Screen_UpStart(void);             /* 0xBA 打开正计时：从 0 递增并切屏1（2026-08-28） */
void Screen_CntBlink(u32 now_ms);      /* 主屏后台计时沙漏闪烁（App_Render 主屏分支每帧调，2026-09-05） */

/* ---- 闹钟控制（voice 调用） ---- */
void Screen_AlarmSet(u8 n, u8 hh, u8 mm); /* 0x8A 设闹钟：n=闹钟号(1-3) + 时/分；写槽内存 + 设当前槽 + 切屏2（2026-09-05 3 槽） */
/* 0x8B 打开/关闭、0x8C 循环模式：2026-09-05 起 Screen_HandleVoice 内联按 d0/d1/d2 三槽解析（协议表），无独立函数 */
void Screen_AlarmRingStop(void);       /* 0xB1 别响了 / 0x8B 关 / 60s 自动停 */
int  Screen_Ringing(void);             /* 1=闹钟或倒计时正在响铃（语音层判「别看回待机」用，2026-11-20） */
void Screen_AlarmRungReset(void);      /* 改时间/日期后清"本分钟已响"标志（坑#82：语音/按键/蓝牙授时改时间后调，防回拨后闹钟不响） */

/* ---- 按键事件分发（2026-09-05：App 层集中分发 Driver 的 KeyEvent{键号,短/长按} 事件流） ---- */
void Screen_HandleKey(const KeyEvent *ke); /* 主循环：按「事件类型->键号->当前状态」映射功能（KEY2 长按关屏在内） */
int  Screen_SetupActive(void);         /* 1=设置模式（时间冻结显示，main 用冻结值传渲染） */
const TimeStruct *Screen_SetupValue(void); /* 设置模式下的冻结工作区时间（退出时写回 RTC） */
void Screen_SetupLine(void);           /* 主屏设置模式黄线绘制（App_Render 主屏分支调用，无设置时不动） */
void Screen_BlinkTick(void);           /* 设置项 1Hz 闪灭（2026-11-19 替代光标；App_Render 末尾每帧调） */
u8   Screen_BlinkHiddenId(void);       /* 灭相要隐藏的项 id（0xFF=无），绘制函数据此不再画它 */
void Screen_SetupVoiceSync(const TimeStruct *nt); /* 语音设时/设日成功时同步手动设置工作区（坑#71 #10 2026-09-05） */
void Screen_StatusIcons(void);         /* 主屏状态角标：整点报时符号/贪睡符号 常驻绘制（App_Render 主屏分支调用，2026-09-05） */
void debug_test(TimeStruct *t);

/* ==================== 气泡文本状态机（2026-11-17 新 UI） ====================
 * 三屏各有默认文案；语音播报时 Screen_Bubble 临时覆盖，8s 后 Screen_BubbleTick
 * 自动恢复该屏默认文案。绘制在 screen.c 的 Px_Draw* 末尾（按屏各自坐标/字号，
 * 闹钟屏窄区按宽度手动折行）。 */

/* 设置气泡当前文本（UTF-8，最多 63 字节，超出按完整字符截断）。
 *   s = NULL 或 ""  -> 立即恢复该屏默认文案
 * 说明：只置状态，真正上屏由下一次 Px_Draw* 完成（≤1s，随秒刷新）。 */
void Screen_Bubble(const char *s);

/* 每秒调用一次（Screen_OnSecond 内已调）：临时文案超时（8s）后恢复默认文案。 */
void Screen_BubbleTick(void);

/* 气泡显示「现在是XX点XX分」（0x07 整点报时/报时命令用，hh/mm 十进制两位）。 */
void Screen_BubbleTime(u8 hh, u8 mm);

/* ==================== 像素游戏机 UI（08 号设计，2026-11-17） ====================
 * 三屏绘制层：只画动态元素（背景由 UI_SetScreen 上传），坐标均为设计坐标。
 * 已接线：Calendar_Refresh -> Px_DrawMain；Screen_Draw/draw_cnt -> CountdownView_Draw；
 *         draw_alarm -> Px_DrawAlm（旧绘制 #if 0 保留）。 */
void Px_DrawMain(const TimeStruct *t);
void Px_DrawAlm(void);
void Px_RobotTick(void);   /* 机器人单独按 250ms 重绘（配合 UI_CharAnim_Tick，App_Render 每轮调用） */

#endif /* __SCREEN_H */

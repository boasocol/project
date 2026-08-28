#ifndef __CALENDAR_H
#define __CALENDAR_H

#include "ht32.h"
#include "Display.h"
#include "time_module.h"
#include "gfx.h"

/* ============================================================
 * 日历界面（竖屏 240x320，原生）
 *   顶部：日期 YYYY/MM/DD  +  星期（靠右）
 *   中部：大时钟 HH:MM（居中）
 *   底部：温度 TEMP xxC   /   湿度 HUMI xx%
 * ============================================================ */

void Calendar_Init(void);
void Calendar_Refresh(TimeStruct *t, int temp, int humi);   /* 时钟位：检测变化启动翻页动画（force_static 时含静态区块） */
void Calendar_RefreshStatic(TimeStruct *t, int temp, int humi);  /* 静态区块：日期/温湿度（后半秒时隙调用） */
void Calendar_ColonBlink(void);   /* 每调用一次翻转冒号显示/隐藏（配合半秒中断）；当前需求停用，保留声明 */
void Calendar_SetStaticNext(void);  /* 蓝牙授时等跳变：下一次 Refresh 强制静态刷新（不翻页） */
void Calendar_ForceRedraw(void);    /* 切屏回主屏：重置全部增量缓存 + 强制静态刷新，下一次 Refresh 全量重绘（时钟/日期/温湿度/蓝牙） */
void Calendar_SetBtState(int connected);  /* 设置蓝牙显示状态：1=已连接，0=未连接（内部去重） */
int  Calendar_AnimBusy(void);     /* 是否有翻页动画进行中（后半秒调度判断） */
int  Calendar_StaticDeferred(void);  /* 小时进位顺延标志是否置位 */
void Calendar_ClearStaticDeferred(void);  /* 清除小时进位顺延标志（下一秒后半秒补刷前调用） */
/* 翻页动画推进：每帧 7.5ms，由 main 主循环按 BFTM0 1ms 计数器 (bftm0_ct) 调用。
   仅当某位数字正在翻页时实际画帧；其余时间 no-op。 */
void Calendar_FlipTick(u32 now_ms);

/* 由年月日计算星期（0=周一 ... 6=周日） */
int CalcWeekday(int y, int m, int d);

#endif

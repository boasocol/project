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
void Calendar_Refresh(TimeStruct *t, int temp, int humi);
void Calendar_ColonBlink(void);   /* 每调用一次翻转冒号显示/隐藏（配合半秒中断） */
void Calendar_SetBtState(int connected);  /* 设置蓝牙显示状态：1=已连接，0=未连接（内部去重） */

/* 由年月日计算星期（0=周一 ... 6=周日） */
int CalcWeekday(int y, int m, int d);

#endif

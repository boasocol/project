#ifndef __CALENDAR_H
#define __CALENDAR_H

#include "ht32.h"
#include "Display.h"
#include "time_module.h"
/* gfx.h（遗留 5x7 点阵/七段数码管图形库）无调用者，P1 已移除（2026-09-03） */

/* ============================================================
 * 日历界面（竖屏 240x320，原生）
 *   顶部：日期 YYYY/MM/DD  +  星期（靠右）
 *   中部：大时钟 HH:MM（居中）
 *   底部：温度 TEMP xxC   /   湿度 HUMI xx%
 * ============================================================ */

void Calendar_Init(void);
void Calendar_Refresh(TimeStruct *t, int temp, int humi);   /* 每轮增量刷新：时钟位 + 静态区块（日期/温湿度，内部 c_* 缓存去重） */
void Calendar_RefreshStatic(TimeStruct *t, int temp, int humi);  /* 静态区块：日期/温湿度（每轮随 Refresh 调用；内部去重，无变化仅比较） */
void Calendar_ColonBlink(void);   /* 每调用一次翻转冒号显示/隐藏（配合半秒中断）；当前需求停用，保留声明 */
void Calendar_ColonForceOn(void); /* 强制冒号亮起（进时间设置模式用，防落在闪烁灭相位设置期一直灭，坑#71 2026-09-05） */
void Calendar_SetStaticNext(void);  /* 蓝牙授时等跳变：下一次 Refresh 强制静态刷新（不翻页） */
void Calendar_ForceRedraw(void);    /* 切屏回主屏：重置全部增量缓存 + 强制静态刷新，下一次 Refresh 全量重绘（时钟/日期/温湿度/蓝牙） */
void Calendar_SetBtState(int connected);  /* 设置蓝牙显示状态：1=已连接，0=未连接（内部去重） */
/* 翻页动画推进：每帧 7.5ms，由 main 主循环按 BFTM0 1ms 计数器 (bftm0_ct) 调用。
   仅当某位数字正在翻页时实际画帧；其余时间 no-op。 */
void Calendar_FlipTick(u32 now_ms);

/* 由年月日计算星期（0=周一 ... 6=周日） */
int CalcWeekday(int y, int m, int d);

/* 公历某月天数（万年历，坑#77 #4 2026-09-05）：month 1..12、year 公历年份；
   month 非法返回 0（调用方先验）；闰年 2 月 29 天。 */
u8 Calendar_DaysInMonth(u8 month, u16 year);

#endif

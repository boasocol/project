#ifndef __GFX_H
#define __GFX_H

#include "ht32.h"
#include "Display.h"

/* ============================================================
 * 图形库：提供文字（5x7点阵，仅含日历所需字符）与
 * 七段数码管风格数字（矢量绘制，无需字库）。
 * 所有坐标均为“逻辑坐标”（竖屏 240x320，原生）。
 * ============================================================ */

/* 绘制单个字符（5x7字库，scale 为放大倍数） */
void GFX_DrawChar(char c, int x, int y, int scale, u16 color);

/* 绘制字符串（字符间距 = 6*scale） */
void GFX_DrawString(const char *s, int x, int y, int scale, u16 color);

/* 七段数码管：绘制单个数字 0-9 */
void GFX_Draw7SegDigit(int x, int y, int w, int h, int t, int digit, u16 color);

/* 七段数码管：绘制多位数（自动清除每格背景） */
void GFX_DrawNumber7Seg(int x, int y, int cw, int ch, int t,
                        u16 val, int ndigits, u16 color, u16 bg);

/* 七段数码管：绘制冒号 */
void GFX_DrawColon(int x, int y, int cw, int ch, int t, u16 color);

/* 七段数码管：绘制 HH:MM 时钟（自动清除整块背景） */
void GFX_DrawClock(int x, int y, int cw, int ch, int t,
                   int hh, int mm, u16 color, u16 bg);

#endif

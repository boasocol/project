#ifndef __FRAMEBUF_H
#define __FRAMEBUF_H

#include "ht32.h"
#include "bigfont.h"

/* ===========================================================================
 * 帧缓冲(framebuffer) + 块传送(Blit)
 * 思路：先把“真字体”等图形画进一块内存缓冲，再把缓冲整体刷到屏幕。
 * 这样字体/图案与屏幕刷新解耦，且比逐像素 FillRect 快得多（一次开窗口连续写）。
 *
 * 帧缓冲直接存 RGB565(u16)，不再用调色板索引：
 *   - 每像素 2 字节，FB_Blit 时直接逐像素发送，无需索引->颜色查表，
 *     传输路径最简洁、CPU 开销最小（Cortex-M0+ 无 DMA，少一次内存读很划算）。
 *   - 代价：RAM = FB_W * FB_H * 2 字节。当前 48*63*2 = 6048 字节(约 5.9KB)，
 *     在 8KB RAM（栈 1KB、堆 0）内可行；再大请确认不超 8KB。
 *   - 想换尺寸：在包含本头文件前 #define FB_W / FB_H 即可。
 * =========================================================================== */
#ifndef FB_W
#define FB_W  BIGFONT_MAX_W   /* 需 >= 大字模最宽字符宽 */
#define FB_H  BIGFONT_H       /* 需 >= 大字模墨迹总高(max yoff+h) */
#endif

void FB_Clear(u16 color);                 /* 整块清成某 RGB565 颜色 */
void FB_SetPixel(u16 x, u16 y, u16 color);/* 在缓冲内画一个像素(RGB565) */
void FB_DrawGlyph(char ch, int x, int y, u16 color, int scale); /* 画一个真字体字符 */
void FB_DrawBigGlyph(u8 gi, int x, int y, u16 color); /* 画大字模字符(0-9=数字,10=冒号)，原生尺寸不放大 */
void FB_DrawString(const char *s, int x, int y, u16 color, int scale, int spacing);
void FB_DrawNumber(int val, int ndigits, int x, int y, u16 color, int scale); /* 画多位数 */
int  FB_TextWidth(const char *s, int scale, int spacing); /* 计算字符串像素宽(用于居中) */
void FB_Blit(u16 x, u16 y, u16 w, u16 h); /* 把缓冲(左上角 w*h)原样刷到屏(x,y) */

#endif

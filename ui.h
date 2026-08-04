#ifndef __UI_H
#define __UI_H

#include "ht32.h"
#include "Display.h"

/* ============================================================
 * Flash UI 层：从 C25VQ16E 读取 ui_320x240.bin 背景图并上传到 ST7789，
 * 并用 Flash 内的变尺寸 1bpp 字库（font.bin）按 flash_interface.md
 * 坐标叠加动态文字（时钟/日期/温湿度）。字模按各显示尺寸单独渲染，绘制时 1:1 不缩放。
 *
 * 本文件随 Display.c 的 #include 聚合一起编译，复用已验证的
 * spi_lcd / Display / flash_read 底层，不引入新的硬件配置。
 * ============================================================ */

/* RGB565 颜色宏（与 ui_spec.md 一致） */
#define UI_RGB(r,g,b)  ( (u16)( ( ((u16)(r)>>3) << 11 ) | ( ((u16)(g)>>2) << 5 ) | ( (u16)(b)>>3 ) ) )

#define UI_C_DIGIT   UI_RGB(0x1a,0x1a,0x1a)   /* 时钟数字 #1a1a1a */
#define UI_C_COLON   UI_RGB(0xe0,0xe0,0xe0)   /* 冒号 #e0e0e0   */
#define UI_C_WHITE   0xFFFF                    /* 白字           */
#define UI_C_BLACK   0x0000                    /* 黑字           */

/* ==================== API ==================== */
int  UI_LoadHeader(void);   /* 读分区头并校验 magic+crc32，定位 ui/font 基址。0=成功 */
void UI_UploadBackground(void); /* 整屏上传 UI 背景图到 ST7789 GRAM（小端->高字节先，软件转置横屏） */
int  UI_CacheFont(void);    /* 缓存字库索引表（按需二分查找） */
void UI_DrawGlyphRect(int rx, int ry, int rw, int rh, u32 code, u16 color); /* 在逻辑矩形内画单字（还原背景+叠墨点） */
void UI_DrawText(const char *str, int x, int y, int cell_w, int cell_h, u16 color); /* UTF-8 字符串（每字 cell_w×cell_h） */
void UI_DrawGlyphs(const u32 *codes, int n, int x, int y, int cell_w, int cell_h, u16 color); /* 直接传 Unicode 码点数组（避免源文件中文字面量） */
void UI_DrawVarDigits(const char *s, int x, int y, u16 color, u32 base); /* 数字串按变体 code 段绘制(1:1，base 区分尺寸) */

#endif

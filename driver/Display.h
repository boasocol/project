#ifndef __DISPLAY_H
#define __DISPLAY_H

#include "ht32.h"
#include "spi_lcd.h"
/* 渲染调度 Display_Update 已迁到 app_render.c（P2，2026-09-03），本头不再需要 time_module.h */

/* ==================== 屏幕参数 ==================== */
/* 物理 GRAM 尺寸（控制器内部固定 240 列 x 320 行，原生竖屏） */
#define ST7789_WIDTH   240
#define ST7789_HEIGHT  320

/* 逻辑显示尺寸：通过 MADCTL 的 MV 位做 90° 硬件旋转，逻辑坐标 = 横屏 320x240。
 * 应用层排版统一使用“逻辑坐标”(SCREEN_W x SCREEN_H)，由 lcd_map/lcd_unmap 换算到 GRAM。 */
#define SCREEN_W  320
#define SCREEN_H  240

/* ==================== 预定义颜色（RGB565） ==================== */
#define WHITE   0xFFFF
#define BLACK   0x0000
#define BLUE    0x001F
#define RED     0xF800
#define GREEN   0x07E0
#define CYAN    0x07FF
#define MAGENTA 0xF81F
#define YELLOW  0xFFE0
#define GRAY    0x8430

/* 日历主题配色（浅黄底 + 深棕字） */
/* RGB565 淡黄的关键：R、G 拉满，B 决定黄的深浅（B 越低越黄）。
 * 旧值 0xFFDB=RGB(255,251,223) B 太高，屏上看接近白色。
 * 现值 0xFF94=RGB(255,242,160)，明显淡黄。
 * 想更黄可再降 B：0xFF71=RGB(255,236,136)；想更淡回调：0xFFB6=RGB(255,244,176) */
#define CAL_BG        0xFF94    /* 淡黄背景 */
#define CAL_TEXT      0x5A06    /* 深棕文字 */
#define CAL_ACCENT_T  0x07FF    /* 温度用青色 */
#define CAL_ACCENT_H  0x07E0    /* 湿度用绿色 */

/* ==================== 字节拆分宏 ==================== */
/* 把 16-bit 值(如 RGB565 颜色、坐标)拆成高/低字节，用于分两字节 SPI 发送。
 * 例：SPI_LCD_WriteDataByte(HI8(color)); SPI_LCD_WriteDataByte(LO8(color)); */
#define HI8(v)  ((u8)(((u16)(v) >> 8) & 0xFF))   /* 取高字节 */
#define LO8(v)  ((u8)((u16)(v) & 0xFF))          /* 取低字节 */

/* ==================== ST7789 指令集（驱动用） ==================== */
#define ST7789_SWRESET   0x01   /* 软件复位 */
#define ST7789_SLPIN     0x10   /* 进入睡眠（省电，RAM 保留；key.c 关屏用，2026-09-03 由 st7789.h 合并） */
#define ST7789_SLPOUT    0x11   /* 退出睡眠 */
#define ST7789_NORON     0x13   /* 正常显示模式 */
#define ST7789_DISPON    0x29   /* 开启显示 */
#define ST7789_DISPOFF   0x28   /* 关闭显示 */
#define ST7789_CASET     0x2A   /* 列地址设置 */
#define ST7789_RASET     0x2B   /* 行地址设置 */
#define ST7789_RAMWR     0x2C   /* 写 RAM */
#define ST7789_COLMOD    0x3A   /* 颜色模式 */
#define ST7789_MADCTL    0x36   /* 内存访问控制 */
#define ST7789_DFUNCTR   0xB6   /* 显示功能控制 */

#define ST7789_COLOR_MODE_16bit  0x55   /* RGB565 */

/* ==================== MADCTL 位定义 ==================== */
#define ST7789_MADCTL_MY  0x80
#define ST7789_MADCTL_MX  0x40
#define ST7789_MADCTL_MV  0x20
#define ST7789_MADCTL_RGB 0x00
#define ST7789_MADCTL_BGR 0x08

/* 方向配置：硬件 MADCTL 旋转在本模块实测不生效，故保持原生 0x00（不写旋转位），
 * 由软件层 lcd_map 做坐标转置实现 90° 横屏。逻辑坐标 = 320x240。 */
#define ST7789_MADCTL_DEFAULT  0x00

/* ==================== 显示驱动 API（纯 ST7789 驱动，P2 拆分 2026-09-03） ==================== */
void Display_Init(void);                       /* 初始化屏幕（通信层 + 命令序列） */
void Display_Fill_Color(u16 color);           /* 全屏填充 */
void Display_FillRect(u16 x, u16 y, u16 w, u16 h, u16 color);  /* 填充矩形（逻辑坐标） */
void Display_DrawPixel(u16 x, u16 y, u16 color);              /* 画单点 */
void Display_on(void);
void Display_off(void);

/* 坐标换算（供需要直接写 GRAM 的模块使用） */
void lcd_map(u16 x, u16 y, u16 *col, u16 *row);          /* 逻辑坐标 -> GRAM 列/行 */
void lcd_unmap(u16 col, u16 row, u16 *x, u16 *y);        /* GRAM 列/行 -> 逻辑坐标 */
#endif

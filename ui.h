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

/* 翻页动画区（位于 Reserved 0x110000，详见 build_flash.py build_anim） */
#define UI_ANIM_OFF      0x110000u
/* 动画卡片背景切片（build_flash.py 预提取的连续背景区，一次读 3360B） */
#define UI_BGSLICE_OFF   0x130000u

/* 用户配置容器（CONF @config_offset，对应 build_flash.py build_config 的 16B payload） */
typedef struct {
  u8 brightness;   /* 亮度 %（默认 80） */
  u8 timezone;     /* 时区 UTC+n（默认 8） */
  u8 alarm_on;     /* 闹钟开关（默认 0） */
  u8 alarm_hour;   /* 闹钟时（默认 7） */
  u8 alarm_min;    /* 闹钟分（默认 0） */
  u8 bt_paired;    /* 蓝牙已配对（默认 1） */
  u8 wifi_on;      /* WiFi 开关（默认 0） */
  u8 reserved[9];  /* 保留 */
} ui_cfg_t;

/* ==================== API ==================== */
int  UI_LoadHeader(void);   /* 读分区头并校验 magic+crc32，定位 ui/font/config 基址。0=成功 */
int  UI_LoadConfig(void);   /* 读 CONF 配置容器并校验 CRC。0=成功；-1=magic；-2=CRC */
const ui_cfg_t *UI_GetConfig(void); /* 访问已加载配置（未加载/失败为全 0） */
void UI_UploadBackground(void); /* 整屏上传 UI 背景图到 ST7789 GRAM（小端->高字节先，软件转置横屏） */
int  UI_CacheFont(void);    /* 缓存字库索引表（按需二分查找） */
void UI_DrawGlyphRect(int rx, int ry, int rw, int rh, u32 code, u16 color); /* 在逻辑矩形内画单字（还原背景+叠墨点） */
void UI_DrawText(const char *str, int x, int y, int cell_w, int cell_h, u16 color); /* UTF-8 字符串（每字 cell_w×cell_h） */
void UI_DrawGlyphs(const u32 *codes, int n, int x, int y, int cell_w, int cell_h, u16 color); /* 直接传 Unicode 码点数组（避免源文件中文字面量） */
void UI_DrawVarDigits(const char *s, int x, int y, u16 color, u32 base); /* 数字串按变体 code 段绘制(1:1，base 区分尺寸) */

/* 翻页动画 API（ANI2 双位面折叠模型）：
   half_idx: 0=物理下半（阶段 B 新数字展开），1=物理上半（阶段 A 旧数字折叠）。
   digit   : 动画层的数字（阶段 A 为旧数字，阶段 B 为新数字）。
   bg_digit: 背景层的数字（阶段 A 为新数字，阶段 B 为旧数字）。
   frame_idx: 0..AnimFrames-1。 */
int  UI_Anim_Init(void);                                          /* 读 ANIM 头校验 + 缓存参数；0=成功 */
int  UI_Anim_Ready(void);                                         /* 动画区可用？1=可播放（w/h>0 且已初始化） */
int  UI_Anim_GetFrames(void);                                      /* 每半翻页帧数（20） */
int  UI_Anim_GetHalfW(void);                                      /* 半幅宽（40） */
int  UI_Anim_GetHalfH(void);                                      /* 半幅高（42） */
void UI_Anim_DrawFrame(int card_x, int half_y0, int digit, int half_idx, int frame_idx,
                       int bg_digit, u16 color);

#endif

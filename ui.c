/************************************************************************************************************
 * @file    ui.c
 * @brief   Flash UI 层：整屏上传 UI 背景图 + 用 Flash 字库叠加动态文字 + 翻页动画帧合成
 *
 * @note    独立编译单元（project 源组里单独勾选）。不再用 #include 聚合，依赖通过对应 .h 暴露。
 *          依赖关系：
 *            - spi_lcd.c  : 3 线 SPI 把像素发到屏幕（9bit，D8=D/C）
 *            - Display.c  : ST7789 命令/窗口（CASET/RASET/RAMWR）
 *            - flash_read.c: SPI1 读外部 Flash（背景图、字库、动画帧）
 *
 *          坐标体系与 Display.c 一致：逻辑 320x240，软件转置
 *          （lcd_map: GRAM col=逻辑y, row=逻辑x）实现横屏。
 *          背景图 bin 为小端 RGB565；ST7789 收数高字节先，故发送前逐像素交换两字节。
 *
 *          数据对应（build_flash.py 生成）：
 *            - UI 背景图  @g_ui_base   （=0x001000，逻辑行连续存储）
 *            - 字库容器   @g_font_base （=0x030000，索引表+1bpp 位图）
 *            - 翻页动画   @UI_ANIM_OFF （=0x110000，ANI2 帧表）
 ************************************************************************************************************/
#include "ui.h"
#include "BFTM0.h"   /* bftm0_ct（1ms 计数，调试分段计时用） */
#include "flash_read.h"
#include "spi_lcd.h"
#include <string.h>
#include <stdio.h>

/* ============ 全局状态 ============ */
static u32 g_ui_base   = 0;     /* UI 背景图在 Flash 中的偏移（来自分区头） */
static u32 g_font_base = 0;     /* 字库区在 Flash 中的偏移 */
static u32 g_config_base = 0;   /* 配置区在 Flash 中的偏移（CONF 容器） */
static u8  g_line[320 * 2];     /* 一行像素临时缓冲（640B，小端数据 + 字节交换后） */

/* 字库索引缓存：build_flash.py 生成 {u32 code, u32 off, u16 w, u16 h} 表，按 code 升序 */
typedef struct { u32 code; u32 off; u16 w; u16 h; } font_idx_t;
/* build_flash.py 实际仅生成 50 个字模，缓存 64 个已留足余量（原 200 浪费 1800B RAM） */
#define MAX_FONT_IDX 64
static font_idx_t g_idx[MAX_FONT_IDX];   /* 字库索引缓存（二分查找用） */
static u32 g_font_count = 0;             /* 字库字符个数 */
static u8  g_glyph[640];                 /* 单个字形位图缓冲（最大 44x84 1bpp = 504B） */
static const u8 g_blank[640] = {0};      /* 全空字模（找不到字形时仅还原背景） */
static u16 g_cur_w = 0, g_cur_h = 0;     /* 最近一次查到的字形尺寸（绘制时 1:1 用） */

/* ============ CRC32（IEEE 802.3 / zlib, poly 0xEDB88320） ============ */
/* 参数: data=待校验数据，len=长度。返回: CRC32 值。用于分区头/字库头/动画头校验。 */
static u32 ui_crc32(const u8 *data, u32 len)
{
  u32 crc = 0xFFFFFFFF;
  for (u32 i = 0; i < len; i++)
  {
    crc ^= data[i];
    for (int k = 0; k < 8; k++)
      crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320 : (crc >> 1);
  }
  return crc ^ 0xFFFFFFFF;
}

/* ============ 读分区头并校验 ============ */
/* ------------------------------------------------------------------
 * UI_LoadHeader：从 Flash 0x000000 读 HTUI 分区头并校验，记录各分区偏移。
 * 返回: 0=成功；-1=magic 错误；-2=CRC 错误。
 * 头布局（build_flash.py build_header）：[0..32) 数据（含各分区偏移），
 * [32..36) CRC32。
 * ------------------------------------------------------------------ */
int UI_LoadHeader(void)
{
  u8 buf[36];
  Flash_Read(0, buf, 36);
  if (!(buf[0] == 'H' && buf[1] == 'T' && buf[2] == 'U' && buf[3] == 'I'))
    return -1;
  u32 crc    = ui_crc32(buf, 32);
  u32 stored = (u32)buf[32] | ((u32)buf[33] << 8) | ((u32)buf[34] << 16) | ((u32)buf[35] << 24);
  if (crc != stored)
    return -2;
  /* UI 背景偏移 @8、字库偏移 @16、配置偏移 @24（与 build_header '<4sIIIIIIII' 字段对应） */
  g_ui_base     = (u32)buf[8]  | ((u32)buf[9]  << 8) | ((u32)buf[10] << 16) | ((u32)buf[11] << 24);
  g_font_base   = (u32)buf[16] | ((u32)buf[17] << 8) | ((u32)buf[18] << 16) | ((u32)buf[19] << 24);
  g_config_base = (u32)buf[24] | ((u32)buf[25] << 8) | ((u32)buf[26] << 16) | ((u32)buf[27] << 24);
  return 0;
}

/* ============ 缓存字库索引表 ============ */
/* ------------------------------------------------------------------
 * UI_CacheFont：读取 FONT 容器头并缓存全部索引条目到 g_idx[]。
 * 返回: 0=成功；-1=magic 错误；-2=CRC 错误。
 * FONT 头布局（build_flash.py '<4sBBBBB HIIII'）：
 *   count@11(H)、index_off@15(I)、crc@23(I)。
 * ------------------------------------------------------------------ */
int UI_CacheFont(void)
{
  if (g_font_base == 0) return -1;
  u8 h[28];
  Flash_Read(g_font_base, h, 28);
  if (!(h[0] == 'F' && h[1] == 'O' && h[2] == 'N' && h[3] == 'T'))
    return -1;
  u32 crc    = ui_crc32(h, 23);
  u32 stored = (u32)h[23] | ((u32)h[24] << 8) | ((u32)h[25] << 16) | ((u32)h[26] << 24);
  if (crc != stored)
    return -2;
  g_font_count = (u32)h[11] | ((u32)h[12] << 8) | ((u32)h[13] << 16) | ((u32)h[14] << 24);
  if (g_font_count > MAX_FONT_IDX) g_font_count = MAX_FONT_IDX;
  u32 index_off = (u32)h[15] | ((u32)h[16] << 8) | ((u32)h[17] << 16) | ((u32)h[18] << 24);
  /* 逐条读索引 {code u32, off u32, w u16, h u16}，共 12B/条 */
  for (u32 i = 0; i < g_font_count; i++)
  {
    u8 e[12];
    Flash_Read(g_font_base + index_off + i * 12, e, 12);
    g_idx[i].code = (u32)e[0] | ((u32)e[1] << 8) | ((u32)e[2] << 16) | ((u32)e[3] << 24);
    g_idx[i].off  = (u32)e[4] | ((u32)e[5] << 8) | ((u32)e[6] << 16) | ((u32)e[7] << 24);
    g_idx[i].w    = (u16)e[8] | ((u16)e[9] << 8);
    g_idx[i].h    = (u16)e[10] | ((u16)e[11] << 8);
  }
  return 0;
}

/* ============ 读用户配置（CONF 容器，见 requirements.md 2.6） ============ */
/* ------------------------------------------------------------------
 * UI_LoadConfig：从 Flash 读 CONF 配置容器并校验。
 * 头布局（build_flash.py build_config '<4sIII' + payload）：
 *   [0..4) magic 'CONF'、[4..8) version、[8..12) payload_len、
 *   [12..16) crc32(payload)、[16..) payload=16B 应用默认值。
 * 返回: 0=成功；-1=magic 错误；-2=CRC 错误（失败则 g_cfg 保持全 0）。
 * ------------------------------------------------------------------ */
static ui_cfg_t g_cfg;

int UI_LoadConfig(void)
{
  u8 hdr[16];
  if (g_config_base == 0) return -1;
  Flash_Read(g_config_base, hdr, 16);
  if (!(hdr[0]=='C' && hdr[1]=='O' && hdr[2]=='N' && hdr[3]=='F'))
  {
    printf("[CONF] bad magic -> default\r\n");
    return -1;
  }
  u32 plen = (u32)hdr[8] | ((u32)hdr[9] << 8) | ((u32)hdr[10] << 16) | ((u32)hdr[11] << 24);
  u32 pcrc = (u32)hdr[12] | ((u32)hdr[13] << 8) | ((u32)hdr[14] << 16) | ((u32)hdr[15] << 24);
  if (plen > sizeof(ui_cfg_t)) plen = sizeof(ui_cfg_t);
  u8 pl[sizeof(ui_cfg_t)];
  Flash_Read(g_config_base + 16, pl, plen);
  if (ui_crc32(pl, plen) != pcrc)
  {
    printf("[CONF] crc fail: stored=%08X calc=%08X -> default\r\n", pcrc, ui_crc32(pl, plen));
    return -2;
  }
  memcpy(&g_cfg, pl, plen);
  /* 注意：newlib-nano printf 不支持 "+" 标志（%+d 会挂起），tz 用 %d */
  printf("[CONF] ok: bright=%u%% tz=%d alarm=%u@%02u:%02u bt=%u wifi=%u\r\n",
         g_cfg.brightness, (int)g_cfg.timezone, g_cfg.alarm_on,
         g_cfg.alarm_hour, g_cfg.alarm_min, g_cfg.bt_paired, g_cfg.wifi_on);
  return 0;
}

const ui_cfg_t *UI_GetConfig(void) { return &g_cfg; }

/* ============ 查字形位图（二分，code 升序） ============ */
/* ------------------------------------------------------------------
 * ui_get_glyph：按 code 在索引中二分查找字形，并读取其位图到 g_glyph。
 * 参数: code=字符 Unicode/字库 code 段（如 '0'+d、0xE000+d、汉字码点）。
 * 返回: 指向 g_glyph 的 1bpp 位图指针；找不到返回 NULL 并把 g_cur_w/h 清零
 *       （调用方据此仅还原背景）。
 * 副作用: 更新 g_cur_w/g_cur_h 为查到字形的尺寸。
 * ------------------------------------------------------------------ */
static const u8 *ui_get_glyph(u32 code)
{
  int lo = 0, hi = (int)g_font_count - 1;
  while (lo <= hi)
  {
    int mid = (lo + hi) / 2;
    if (g_idx[mid].code == code)
    {
      g_cur_w = g_idx[mid].w; g_cur_h = g_idx[mid].h;
      Flash_Read(g_font_base + g_idx[mid].off, g_glyph, (u32)g_cur_h * ((g_cur_w + 7u) / 8u));
      return g_glyph;
    }
    if (g_idx[mid].code < code) lo = mid + 1;
    else hi = mid - 1;
  }
  g_cur_w = 0; g_cur_h = 0;   /* 找不到：尺寸归零，调用方仅还原背景 */
  return NULL;
}

/* ============ 整屏上传 UI 背景（小端 RGB565 -> 高字节先 + 软件转置横屏） ============ */
/* ------------------------------------------------------------------
 * UI_UploadBackground：把 Flash 中的 UI 背景图整屏上传到屏幕（上电一次）。
 * 逐行处理：Flash 读一行(640B) -> 字节交换(小端转高字节先) ->
 * CASET=逻辑行y、RASET=0..319 -> RAMWR 写一行。
 * 说明: 逻辑行 y 在 Flash 中连续存储，偏移 = (y*320 + x)*2。
 * ------------------------------------------------------------------ */
void UI_UploadBackground(void)
{
  for (int y = 0; y < 240; y++)
  {
    Flash_Read(g_ui_base + (u32)y * 320u * 2u, g_line, 320u * 2u);
    /* 小端 -> 高字节先（ST7789 收数顺序） */
    for (int i = 0; i < 320 * 2; i += 2)
    {
      u8 t = g_line[i]; g_line[i] = g_line[i + 1]; g_line[i + 1] = t;
    }
    /* 逻辑行 y 对应 GRAM 列=y、行=0..319（lcd_map: col=y,row=x） */
    SPI_LCD_Select();
    SPI_LCD_WriteCmd(ST7789_CASET);
    { u8 d[4] = {0, (u8)y, 0, (u8)y}; SPI_LCD_WriteData(d, 4); }
    SPI_LCD_WriteCmd(ST7789_RASET);
    { u8 d[4] = {0, 0, HI8(319), LO8(319)}; SPI_LCD_WriteData(d, 4); }
    SPI_LCD_WriteCmd(ST7789_RAMWR);
    SPI_LCD_WriteData(g_line, 320u * 2u);
    SPI_LCD_Deselect();
  }
}

/* ============ 在逻辑矩形内画单字（先还原该区域背景，再叠加墨点） ============ */
/* ------------------------------------------------------------------
 * UI_DrawGlyphRect：在逻辑矩形 (rx,ry)-(rx+rw-1,ry+rh-1) 内画一个字模。
 * 参数:
 *   rx,ry : 矩形左上角逻辑坐标
 *   rw,rh : 矩形宽高（一般等于字模实际尺寸，1:1 不缩放）
 *   code  : 字库 code（决定字形与尺寸）
 *   color : 墨点颜色（RGB565）；背景保留原 UI 背景图
 * 流程: 逐行——Flash 读该行背景段 -> 字节交换 -> 按字模墨点叠色 -> RAMWR 写该行。
 *      字形尺寸取自 ui_get_glyph 的 g_cur_w/g_cur_h；找不到字形则仅还原背景。
 * ------------------------------------------------------------------ */
void UI_DrawGlyphRect(int rx, int ry, int rw, int rh, u32 code, u16 color)
{
  if (rw <= 0 || rh <= 0) return;
  const u8 *g = ui_get_glyph(code);
  if (!g) g = g_blank;   /* 找不到：仅还原背景 */
  int row_bytes = (g_cur_w + 7) / 8;   /* 字模每行字节数（变尺寸） */

  for (int yy = 0; yy < rh; yy++)
  {
    int ly = ry + yy;
    /* 读该逻辑行的背景段（连续） */
    Flash_Read(g_ui_base + (u32)ly * 320u * 2u + (u32)rx * 2u, g_line, (u32)rw * 2u);
    for (int i = 0; i < rw * 2; i += 2)
    {
      u8 t = g_line[i]; g_line[i] = g_line[i + 1]; g_line[i + 1] = t;
    }
    /* 叠加字墨点：字模行/列 1:1 映射到矩形内（rw/rh 等于字模尺寸时不缩放） */
    for (int xx = 0; xx < rw; xx++)
    {
      int gx = (g_cur_w > 1) ? xx * g_cur_w / rw : 0;
      int gy = (g_cur_h > 1) ? yy * g_cur_h / rh : 0;
      int bit = (g[gy * row_bytes + (gx >> 3)] >> (7 - (gx & 7))) & 1;
      if (bit) { g_line[xx * 2] = HI8(color); g_line[xx * 2 + 1] = LO8(color); }
    }
    /* 逻辑行 ly -> GRAM 列=ly，行=rx..rx+rw-1 */
    SPI_LCD_Select();
    SPI_LCD_WriteCmd(ST7789_CASET);
    { u8 d[4] = {0, (u8)ly, 0, (u8)ly}; SPI_LCD_WriteData(d, 4); }
    SPI_LCD_WriteCmd(ST7789_RASET);
    { u8 d[4] = {HI8((u16)rx), LO8((u16)rx), HI8((u16)(rx + rw - 1)), LO8((u16)(rx + rw - 1))};
      SPI_LCD_WriteData(d, 4); }
    SPI_LCD_WriteCmd(ST7789_RAMWR);
    SPI_LCD_WriteData(g_line, (u32)rw * 2u);
    SPI_LCD_Deselect();
  }
}

/* ============ UTF-8 解析（取下一码点，*pp 前移） ============ */
/* ------------------------------------------------------------------
 * utf8_next：从字符串当前位置解析出一个 Unicode 码点。
 * 参数: pp=指向字符串指针的指针；调用后 *pp 前移到下一字符。
 * 返回: 解析出的码点（1-4 字节 UTF-8；非法序列按单字节原样返回）。
 * ------------------------------------------------------------------ */
static u32 utf8_next(const char **pp)
{
  const u8 *p = (const u8 *)*pp;
  u32 c; int n;
  if (p[0] < 0x80)              { c = p[0]; n = 1; }
  else if ((p[0] & 0xE0) == 0xC0) { c = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); n = 2; }
  else if ((p[0] & 0xF0) == 0xE0) { c = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); n = 3; }
  else if ((p[0] & 0xF8) == 0xF0) { c = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); n = 4; }
  else                          { c = p[0]; n = 1; }
  *pp = (const char *)(p + n);
  return c;
}

/* ============ 画 UTF-8 字符串（每字占 cell_w×cell_h，从左上 (x,y) 起水平排列） ============ */
/* 参数: str=UTF-8 字符串；x,y=起点；cell_w/h=每字格宽高；color=墨色。 */
void UI_DrawText(const char *str, int x, int y, int cell_w, int cell_h, u16 color)
{
  const char *p = str;
  int i = 0;
  while (*p)
  {
    u32 code = utf8_next(&p);
    UI_DrawGlyphRect(x + i * cell_w, y, cell_w, cell_h, code, color);
    i++;
  }
}

/* ============ 直接传 Unicode 码点数组（无需源文件中文字面量） ============ */
/* 参数: codes=码点数组；n=个数；x,y=起点；cell_w/h=每字格；color=墨色。 */
void UI_DrawGlyphs(const u32 *codes, int n, int x, int y, int cell_w, int cell_h, u16 color)
{
  for (int i = 0; i < n; i++)
    UI_DrawGlyphRect(x + i * cell_w, y, cell_w, cell_h, codes[i], color);
}

/* ============ 按变体 code 段画数字串（1:1 清晰） ============ */
/* 每个 '0'-'9' 映射到 base+d，其余字符用原 Unicode；横向按各字模实际宽排列
   （同尺寸数字等宽）。用于日期(0xE000)/温湿度(0xE100)的小号数字，
   避免误查到时钟大号字模(0x30)被缩放糊掉。
   参数: s=数字串（可含 '-'、'.'）；x,y=起点；color=墨色；base=数字 code 段基址。 */
void UI_DrawVarDigits(const char *s, int x, int y, u16 color, u32 base)
{
  int cx = x;
  for (const char *p = s; *p; p++)
  {
    u32 c = (u32)(unsigned char)*p;
    u32 code;
    if (c >= '0' && c <= '9') code = base + (c - '0');
    else if (c == '.')        code = 0xE200u;   /* 小数点（温湿度显示用） */
    else                      code = c;
    const u8 *g = ui_get_glyph(code);
    u16 w = g_cur_w, h = g_cur_h;
    if (!g) { w = 0; h = 0; }
    UI_DrawGlyphRect(cx, y, w, h, code, color);
    cx += w;
  }
}

/* ===========================================================================================================
 * 翻页动画（ANI2 区 @ Flash 0x110000）
 *
 * 数据布局（与 build_flash.py build_anim 对齐）：
 *   +0    magic[4]  "ANI2"
 *   +4    version   u32 (=2)
 *   +8    num       u8  (10)
 *   +9    halves    u8  (2)
 *   +10   frames    u16 (12)
 *   +12   w         u16 (40)
 *   +14   h         u16 (42)
 *   +16   crc32     u32（覆盖 [0..16)）
 *   +20   planes    u8  (2：ink + mask)
 *   +24..43  shade[0][20]  u8：half0（阶段B 展开）每帧明暗系数
 *   +44..63  shade[1][20]  u8：half1（阶段A 折叠）每帧明暗系数
 *   +64..255  reserved(0)
 *   +256.. 帧数据：digits(10) x halves(2) x frames(12) x (ink 210B + mask 210B)
 *
 * 双位面模型：
 *   mask=1 区域被折叠/展开的卡片纸覆盖：ink=1 画数字色，ink=0 画背景色变暗(shade)
 *   mask=0 区域是卡片纸后面的背景数字：bg_digit 该半幅 + 原 UI 背景
 * =========================================================================================================== */

/* 动画元数据（UI_Anim_Init 从数据头解析缓存） */
static u32 g_anim_frames = 0;    /* 每半幅帧数 */
static u32 g_anim_w      = 0;    /* 半幅宽（40） */
static u32 g_anim_h      = 0;    /* 半幅高（42） */
static u32 g_anim_num    = 0;    /* 数字个数（10） */
static u32 g_anim_half   = 0;    /* 半幅数（2） */
static u32 g_anim_planes = 0;    /* 位面数（2：ink+mask） */
static u8  g_anim_shade[2][20];  /* [half_idx][frame] 明暗系数（0..255） */
static u8  g_bgs_ok = 0;   /* 背景切片可用（UI_Anim_Init 校验 BGSC magic） */

/* 卡片左边界 x -> 背景切片索引（与 build_flash.py BG_CARD_X 一致） */
static int bg_slice_card(int card_x)
{
  static const int bx[6] = {13, 59, 117, 163, 221, 267};
  for (int k = 0; k < 6; k++)
    if (bx[k] == card_x) return k;
  return 0;
}

/* ------------------------------------------------------------------
 * anim_frame_off：计算某帧在帧数据区内的字节偏移（不含 256B 头）。
 * 参数: digit=数字 0..9；half_idx=0(下半)/1(上半)；frame_idx=帧号。
 * 返回: 帧数据偏移 = ((digit*halves + half)*frames + frame) * 每帧字节数。
 *       每帧字节数 per = h * ceil(w/8) * planes（ink+mask 双位面）。
 * ------------------------------------------------------------------ */
static u32 anim_frame_off(int digit, int half_idx, int frame_idx)
{
  u32 row = (g_anim_w + 7u) / 8u;
  u32 per = g_anim_h * row * g_anim_planes;   /* 双位面：ink + mask */
  return ((u32)digit * g_anim_half * g_anim_frames +
          (u32)half_idx * g_anim_frames +
          (u32)frame_idx) * per;
}

/* ------------------------------------------------------------------
 * UI_Anim_Init：读取 ANI2 数据头并校验，缓存动画元数据与亮度表。
 * 返回: 0=成功；-1=magic 错误；-2=CRC 错误（失败则界面退化为纯静态）。
 * ------------------------------------------------------------------ */
int UI_Anim_Init(void)
{
  u8 hdr[256];
  Flash_Read(UI_ANIM_OFF, hdr, 256);

  /* 背景切片区自检（旧镜像无此区则回退逐行读背景） */
  {
    u8 bh[16];
    Flash_Read(UI_BGSLICE_OFF, bh, 16);
    g_bgs_ok = (bh[0]=='B' && bh[1]=='G' && bh[2]=='S' && bh[3]=='C') ? 1u : 0u;
    printf("[ANIM] bg slice: %s\r\n", g_bgs_ok ? "OK (3360B x12)" : "NONE -> per-row");
  }
  printf("[ANIM] flash@%X: %c%c%c%c\n", UI_ANIM_OFF,
         hdr[0], hdr[1], hdr[2], hdr[3]);
  if (!(hdr[0]=='A' && hdr[1]=='N' && hdr[2]=='I' && hdr[3]=='2'))
  {
    printf("[ANIM] bad magic -> static\n");
    return -1;
  }
  u32 crc      = (u32)hdr[16] | ((u32)hdr[17] << 8) | ((u32)hdr[18] << 16) | ((u32)hdr[19] << 24);
  u32 crc_calc = ui_crc32(hdr, 16);
  if (crc != crc_calc)
  {
    printf("[ANIM] crc fail: stored=%08X calc=%08X -> static\n", crc, crc_calc);
    return -2;
  }
  g_anim_num    = hdr[8];
  g_anim_half   = hdr[9];
  g_anim_frames = (u32)hdr[10] | ((u32)hdr[11] << 8);
  g_anim_w      = (u32)hdr[12] | ((u32)hdr[13] << 8);
  g_anim_h      = (u32)hdr[14] | ((u32)hdr[15] << 8);
  g_anim_planes = hdr[20];                       /* 应为 2 */
  if (g_anim_planes == 0) g_anim_planes = 2;
  memcpy(g_anim_shade[0], hdr + 24, 20);         /* half0 亮度表 @24 */
  memcpy(g_anim_shade[1], hdr + 44, 20);         /* half1 亮度表 @44 */
  printf("[ANIM] ok: digits=%u halves=%u frames=%u w=%u h=%u planes=%u -> flip\n",
         g_anim_num, g_anim_half, g_anim_frames, g_anim_w, g_anim_h, g_anim_planes);
  return 0;
}

/* 动画元数据 getter（供 calendar.c 查询） */
int  UI_Anim_GetFrames(void) { return (int)g_anim_frames; }
int  UI_Anim_GetHalfW(void)   { return (int)g_anim_w; }
int  UI_Anim_GetHalfH(void)   { return (int)g_anim_h; }
int  UI_Anim_Ready(void)      { return (g_anim_w > 0u && g_anim_h > 0u && g_anim_frames > 0u) ? 1 : 0; }

/* 帧数据缓冲：ink 210B + mask 210B = 420B（一次性读两个平面） */
static u8  g_anim_buf[520];
/* 整 half 合成缓冲（40x42 RGB565 = 3360B），整块一次 RAMWR 写屏，
   取代原来逐行 42 次 SPI 事务 + 逐像素 Flash_Read，翻页阻塞大幅降低。 */
static u8 g_blit[40 * 42 * 2];

/* ------------------------------------------------------------------
 * ui_shade_color：按明暗系数缩放一个 RGB565 颜色。
 * 参数: c=原色（RGB565）；shade=0..256（256=不变）。
 * 返回: 缩放后的 RGB565。各通道先乘 shade 再 >>8（用 256 代替 /255）。
 * ------------------------------------------------------------------ */
static u16 ui_shade_color(u16 c, u16 shade)
{
  u32 r = ((c >> 11) & 0x1F) * shade >> 8;
  u32 g = ((c >> 5)  & 0x3F) * shade >> 8;
  u32 b = (c        & 0x1F) * shade >> 8;
  return (u16)((r << 11) | (g << 5) | b);
}

/* ==================================================================
 * UI_Anim_DrawFrame：绘制翻页动画的【一个半幅帧】（显示驱动最核心的合成函数）。
 *
 * 参数:
 *   card_x   : 卡片左边界逻辑 x（决定 RASET 窗口）。
 *   half_y0  : 该半幅的 GRAM 起始行（上半 183 / 下半 141）。
 *   digit    : 本帧使用的数字（阶段A=旧数字折叠，阶段B=新数字展开）。
 *   half_idx : 0=屏幕下半、1=屏幕上半（决定背景字模取哪 42 行、亮度表用哪个）。
 *   frame_idx: 帧号（0..帧数-1，决定读哪帧数据 + 亮度表取值）。
 *   bg_digit : 背景数字（阶段A=新数字，阶段B=旧数字；露出卡片后面的数字）。
 *   color    : 数字墨色（RGB565）。
 *
 * 流程:
 *   1) 一次性读该帧 ink+mask 两个位面（420B）。
 *   2) 读背景数字字模（bg_digit 的大号字模 40x84），按 half_idx 取相应 42 行。
 *   3) 取本帧亮度 shade（0..256）。
 *   4) 逐行读 UI 背景（40px 段），逐像素合成进 g_blit：
 *        mask=1 : 卡片纸——ink=1 画数字色(按 shade 变暗)，
 *                 ink=0 画背景像素(按 shade 变暗)；
 *        mask=0 : 背景——bg 字模墨点=1 画数字色，否则画背景像素。
 *   5) 整半幅一次 RAMWR 写屏（CASET=half_y0 起，RASET=card_x 起）。
 * ------------------------------------------------------------------ */
void UI_Anim_DrawFrame(int card_x, int half_y0, int digit, int half_idx, int frame_idx,
                       int bg_digit, u16 color)
{
  if (digit < 0 || digit >= (int)g_anim_num) return;
  if (bg_digit < 0 || bg_digit >= (int)g_anim_num) bg_digit = digit;
  if (half_idx < 0 || half_idx >= (int)g_anim_half) return;
  if (frame_idx < 0 || frame_idx >= (int)g_anim_frames) return;
  if (g_anim_w == 0 || g_anim_h == 0) return;

  u32 w = g_anim_w, h = g_anim_h;
  u32 row_bytes = (w + 7u) / 8u;
  u32 per       = h * row_bytes;                 /* 单平面大小（210B） */
  u32 off       = UI_ANIM_OFF + 256u + anim_frame_off(digit, half_idx, frame_idx);

  /* 1) 一次性读 ink + mask 两个平面（420B），取代原来逐像素 1680 次 Flash_Read */
  vu32 t_flash = bftm0_ct;
  Flash_Read(off, g_anim_buf, per * g_anim_planes);
  const u8 *ink  = g_anim_buf;                   /* 墨点位面 */
  const u8 *mask = g_anim_buf + per;             /* 卡片纸覆盖位面 */
  vu32 dt_flash = (u32)bftm0_ct - (u32)t_flash;

  /* 2) 读背景数字该 half 的 1bpp 位图（40x42）一次读出 */
  const u8 *bg = ui_get_glyph((u32)'0' + (u32)bg_digit);
  if (!bg) bg = g_blank;
  u32 bg_rb = (g_cur_w + 7u) / 8u;
  u32 bg_y0 = (half_idx == 1) ? 42u : 0u;        /* half1(屏幕上半)取 42..83 行，half0(屏幕下半)取 0..41 行 */

  /* 3) 本帧亮度（0..255 -> 1..256；shade=256 表示不调暗） */
  u16 shade = 256u;
  if (half_idx < 2 && frame_idx < 20)
    shade = (u16)g_anim_shade[half_idx][frame_idx] + 1u;

  /* 4) 读背景并合成。
     优先用【背景切片】：build_flash.py 已把 6 卡 x 2 half 的背景连续提取到
     UI_BGSLICE_OFF，一次 Flash_Read(3360B) 取代原 42 次逐行事务（bgread 8ms->1.5ms）。
     切片区带 BGSC magic，UI_Anim_Init 校验通过才启用，失败回退逐行读（安全）。
     注意：切片块内为【列优先】布局（x 外、yy 内），与 g_blit 写屏索引 pos 一致，
     故读背景用 g_blit[pos] 而非行优先索引，避免读写同缓冲冲突产生黑线。 */
  vu32 t_synth = bftm0_ct;
  vu32 t_bgread = bftm0_ct;                      /* [计时] 背景读取 */

  if (g_bgs_ok)
  {
    /* 一次性读整块背景切片（行优先小端 RGB565）到 g_blit，原地交换后合成写回 */
    u32 bgoff = UI_BGSLICE_OFF + 256u + ((u32)bg_slice_card(card_x) * 2u + (u32)half_idx) * w * h * 2u;
    Flash_Read(bgoff, g_blit, w * h * 2u);
    for (u32 i = 0; i < w * h * 2u; i += 2)      /* 小端->大端 原地 */
    {
      u8 t = g_blit[i]; g_blit[i] = g_blit[i + 1]; g_blit[i + 1] = t;
    }
    for (u32 yy = 0; yy < h; yy++)
    {
      u32 src_row = yy * row_bytes;              /* 当前帧位图的行偏移 */
      u32 bg_row  = (bg_y0 + yy) * bg_rb;        /* 背景字模对应行偏移 */
      u32 pos = yy * 2u;                         /* GRAM 顺序索引：列x外、行yy内 */
      for (u32 x = 0; x < w; x++)
      {
        u32 idx = src_row + (x >> 3);
        u8  mb  = (mask[idx] >> (7 - (x & 7))) & 1;
        /* 背景切片为列优先，索引 = pos 同式（先读旧背景再写合成，安全） */
        u16 bpx = (u16)((u16)g_blit[pos] << 8) | g_blit[pos + 1];
        u16 pix;
        if (mb)                                  /* 卡片纸内 */
        {
          u8 ib = (ink[idx] >> (7 - (x & 7))) & 1;
          pix = (ib) ? ((shade == 256u) ? color : ui_shade_color(color, shade))
                      : ((shade == 256u) ? bpx : ui_shade_color(bpx, shade));
        }
        else                                     /* 卡片外：露出背景数字或背景 */
        {
          u8 bb = (bg[bg_row + (x >> 3)] >> (7 - (x & 7))) & 1;
          pix = bb ? color : bpx;
        }
        g_blit[pos]     = HI8(pix);
        g_blit[pos + 1] = LO8(pix);
        pos += h * 2u;                           /* 下一列：GRAM 索引 +h 行 */
      }
    }
  }
  else
  {
    /* 回退：逐行读背景（无切片或旧镜像），逻辑同历史实现 */
    for (u32 yy = 0; yy < h; yy++)
    {
      u16 line16[40];
      u32 ly = (u32)(half_y0 + (int)yy);
      if (Flash_DualOK())
        Flash_ReadDual(g_ui_base + ly * 320u * 2u + (u32)card_x * 2u, g_line, w * 2u);
      else
        Flash_Read(g_ui_base + ly * 320u * 2u + (u32)card_x * 2u, g_line, w * 2u);
      for (u32 i = 0; i < w; i++)               /* 小端->大端 直接转 u16 */
      {
        u8 t = g_line[i * 2];
        g_line[i * 2] = g_line[i * 2 + 1];
        g_line[i * 2 + 1] = t;
        line16[i] = (u16)((u16)g_line[i * 2] << 8) | g_line[i * 2 + 1];
      }
      u32 src_row = yy * row_bytes;
      u32 bg_row  = (bg_y0 + yy) * bg_rb;
      u32 pos = yy * 2u;
      for (u32 x = 0; x < w; x++)
      {
        u32 idx = src_row + (x >> 3);
        u8  mb  = (mask[idx] >> (7 - (x & 7))) & 1;
        u16 pix;
        if (mb)                                 /* 卡片纸内 */
        {
          u8 ib = (ink[idx] >> (7 - (x & 7))) & 1;
          if (ib)                               /* 墨点：数字色 */
            pix = (shade == 256u) ? color : ui_shade_color(color, shade);
          else                                  /* 纸底：背景像素变暗 */
            pix = (shade == 256u) ? line16[x] : ui_shade_color(line16[x], shade);
        }
        else                                    /* 卡片外 */
        {
          u8 bb = (bg[bg_row + (x >> 3)] >> (7 - (x & 7))) & 1;
          pix = bb ? color : line16[x];
        }
        g_blit[pos]     = HI8(pix);
        g_blit[pos + 1] = LO8(pix);
        pos += h * 2u;
      }
    }
  }

  vu32 dt_bgread = (u32)bftm0_ct - (u32)t_bgread;  /* 背景读取耗时 */
  vu32 dt_synth = (u32)bftm0_ct - (u32)t_synth;    /* 含 bgread + 合成 */

  /* 5) 整 half 一次 RAMWR：窗口与 UI_DrawGlyphRect 同构（CASET=逻辑y, RASET=逻辑x） */
  vu32 t_spi = bftm0_ct;
  SPI_LCD_Select();
  SPI_LCD_WriteCmd(ST7789_CASET);
  { u8 d[4] = {0, (u8)half_y0, 0, (u8)(half_y0 + (int)h - 1)}; SPI_LCD_WriteData(d, 4); }
  SPI_LCD_WriteCmd(ST7789_RASET);
  { u8 d[4] = {HI8((u16)card_x), LO8((u16)card_x),
               HI8((u16)(card_x + (int)w - 1)), LO8((u16)(card_x + (int)w - 1))};
    SPI_LCD_WriteData(d, 4); }
  SPI_LCD_WriteCmd(ST7789_RAMWR);
  SPI_LCD_WriteData(g_blit, (u32)w * h * 2u);
  SPI_LCD_Deselect();
  vu32 dt_spi = (u32)bftm0_ct - (u32)t_spi;

  /* [调试] 分段计时已注释（语音开发期），保留 dt_* 计算避免改结构 */
  (void)dt_flash; (void)dt_bgread; (void)dt_synth; (void)dt_spi;
  /* {
    static u32 n = 0;
    if ((n++ & 0x1F) == 0)
      printf("[T] flash=%u bgread=%u synth=%u spi=%u\r\n",
             (unsigned)dt_flash, (unsigned)dt_bgread,
             (unsigned)(dt_synth - dt_bgread), (unsigned)dt_spi);
  } */
}

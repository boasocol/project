/************************************************************************************************************
 * @file    ui.c
 * @brief   Flash UI 层：整屏上传 UI 背景图 + 用 Flash 字库叠加动态文字
 * @note    随 Display.c 的 #include 聚合编译。复用 spi_lcd（3线SPI发屏）、Display（ST7789命令）、
 *          flash_read（SPI1 读 Flash）。坐标体系与 Display.c 一致：逻辑 320x240，
 *          软件转置（lcd_map: GRAM col=逻辑y, row=逻辑x）实现横屏。
 *          背景图 bin 为小端 RGB565，ST7789 收数高字节先，故发送前逐像素交换两字节。
 ************************************************************************************************************/
#include "ui.h"
#include "flash_read.h"
#include "spi_lcd.h"
#include <string.h>
#include <stdio.h>

/* ============ 全局状态 ============ */
static u32 g_ui_base   = 0;     /* UI 背景图在 Flash 中的偏移（来自分区头） */
static u32 g_font_base = 0;     /* 字库区在 Flash 中的偏移 */
static u8  g_line[320 * 2];     /* 一行像素临时缓冲（640B，含字节交换后数据） */

/* 字库索引缓存：build_flash.py 生成 {u32 code, u32 off, u16 w, u16 h} 表，按 code 升序 */
typedef struct { u32 code; u32 off; u16 w; u16 h; } font_idx_t;
/* build_flash.py 实际仅生成 50 个字模，缓存 64 个已留足余量（原 200 浪费 1800B RAM） */
#define MAX_FONT_IDX 64
static font_idx_t g_idx[MAX_FONT_IDX];
static u32 g_font_count = 0;
static u8  g_glyph[640];        /* 单个字形位图（最大 44x84 1bpp = 504B） */
static const u8 g_blank[640] = {0};
static u16 g_cur_w = 0, g_cur_h = 0;  /* 当前字形尺寸（绘制时 1:1 用） */

/* ============ CRC32（IEEE 802.3 / zlib, poly 0xEDB88320） ============ */
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
  g_ui_base   = (u32)buf[8]  | ((u32)buf[9]  << 8) | ((u32)buf[10] << 16) | ((u32)buf[11] << 24);
  g_font_base = (u32)buf[16] | ((u32)buf[17] << 8) | ((u32)buf[18] << 16) | ((u32)buf[19] << 24);
  return 0;
}

/* ============ 缓存字库索引表 ============ */
int UI_CacheFont(void)
{
  if (g_font_base == 0) return -1;
  u8 h[28];
  Flash_Read(g_font_base, h, 28);
  if (!(h[0] == 'F' && h[1] == 'O' && h[2] == 'N' && h[3] == 'T'))
    return -1;
  u32 crc    = ui_crc32(h, 23);
  /* bin 真实布局(build_flash.py '<4sBBBBB HIIII'): count@11 index@15 crc@23，需严格对应 */
  u32 stored = (u32)h[23] | ((u32)h[24] << 8) | ((u32)h[25] << 16) | ((u32)h[26] << 24);
  if (crc != stored)
    return -2;
  g_font_count = (u32)h[11] | ((u32)h[12] << 8) | ((u32)h[13] << 16) | ((u32)h[14] << 24);
  if (g_font_count > MAX_FONT_IDX) g_font_count = MAX_FONT_IDX;
  u32 index_off = (u32)h[15] | ((u32)h[16] << 8) | ((u32)h[17] << 16) | ((u32)h[18] << 24);
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

/* ============ 查字形位图（二分，code 升序） ============ */
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
void UI_UploadBackground(void)
{
  for (int y = 0; y < 240; y++)
  {
    /* bin 中逻辑行 y 连续存储：偏移 = (y*320 + x)*2，x=0..319 */
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
void UI_DrawGlyphRect(int rx, int ry, int rw, int rh, u32 code, u16 color)
{
  if (rw <= 0 || rh <= 0) return;
  const u8 *g = ui_get_glyph(code);
  if (!g) g = g_blank;   /* 找不到：仅还原背景 */
  int row_bytes = (g_cur_w + 7) / 8;   /* 字模每行字节数（变尺寸，不再固定 4） */

  for (int yy = 0; yy < rh; yy++)
  {
    int ly = ry + yy;
    /* 读该逻辑行的背景段（连续） */
    Flash_Read(g_ui_base + (u32)ly * 320u * 2u + (u32)rx * 2u, g_line, (u32)rw * 2u);
    for (int i = 0; i < rw * 2; i += 2)
    {
      u8 t = g_line[i]; g_line[i] = g_line[i + 1]; g_line[i + 1] = t;
    }
    /* 叠加字墨点：32x32 字模最近邻缩放到 rw×rh */
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
void UI_DrawGlyphs(const u32 *codes, int n, int x, int y, int cell_w, int cell_h, u16 color)
{
  for (int i = 0; i < n; i++)
    UI_DrawGlyphRect(x + i * cell_w, y, cell_w, cell_h, codes[i], color);
}

/* ============ 按变体 code 段画数字串（1:1 清晰） ============ */
/* 每个 '0'-'9' 映射到 base+d，其余字符用原 Unicode；横向按各字模实际宽排列
   （同尺寸数字等宽）。用于日期(0xE000)/温湿度(0xE100)的小号数字，
   避免误查到时钟大号字模(0x30)被缩放糊掉。 */
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
 *   +0   magic[4]  "ANI2"
 *   +4   version   u32 (=2)
 *   +8   num       u8  (10)
 *   +9   halves    u8  (2)
 *   +10  frames    u16 (20)
 *   +12  w         u16 (40)
 *   +14  h         u16 (42)
 *   +16  crc32     u32（覆盖 [0..16)）
 *   +20  planes    u8  (2：ink + mask)
 *   +24..63  shade[2][20]  u8：阶段 A/B 的每帧明暗系数(0..255)
 *   +64..255 reserved(0)
 *   +256.. 帧数据：digits(10) x halves(2) x frames(20) x (ink 210B + mask 210B)
 *
 * 双位面模型：
 *   mask=1 区域被折叠/展开的卡片纸覆盖：ink=1 画数字色，ink=0 画背景色变暗(shade)
 *   mask=0 区域是卡片纸后面的背景数字：bg_digit 该半幅 + 原 UI 背景
 * =========================================================================================================== */

static u32 g_anim_frames = 0;
static u32 g_anim_w      = 0;
static u32 g_anim_h      = 0;
static u32 g_anim_num    = 0;
static u32 g_anim_half   = 0;
static u32 g_anim_planes = 0;
static u8  g_anim_shade[2][20];       /* [half_idx][frame] */

static u32 anim_frame_off(int digit, int half_idx, int frame_idx)
{
  u32 row = (g_anim_w + 7u) / 8u;
  u32 per = g_anim_h * row * g_anim_planes;   /* 双位面：ink + mask */
  return ((u32)digit * g_anim_half * g_anim_frames +
          (u32)half_idx * g_anim_frames +
          (u32)frame_idx) * per;
}

int UI_Anim_Init(void)
{
  u8 hdr[256];
  Flash_Read(UI_ANIM_OFF, hdr, 256);
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
  memcpy(g_anim_shade[0], hdr + 24, 20);
  memcpy(g_anim_shade[1], hdr + 44, 20);
  printf("[ANIM] ok: digits=%u halves=%u frames=%u w=%u h=%u planes=%u -> flip\n",
         g_anim_num, g_anim_half, g_anim_frames, g_anim_w, g_anim_h, g_anim_planes);
  return 0;
}

int  UI_Anim_GetFrames(void) { return (int)g_anim_frames; }
int  UI_Anim_GetHalfW(void)   { return (int)g_anim_w; }
int  UI_Anim_GetHalfH(void)   { return (int)g_anim_h; }
int  UI_Anim_Ready(void)      { return (g_anim_w > 0u && g_anim_h > 0u && g_anim_frames > 0u) ? 1 : 0; }

/* 帧数据缓冲：ink 210B + mask 210B = 420B（一次性读两个平面） */
static u8  g_anim_buf[520];
/* 整 half 合成缓冲（40x42 RGB565 = 3360B），整块一次 RAMWR 写屏，
   取代原来逐行 42 次 SPI 事务 + 逐像素 Flash_Read，翻页阻塞大幅降低。 */
static u8 g_blit[40 * 42 * 2];

/* 把 565 颜色按 shade(0..256) 缩放（shade=256 时不变）。shade 以 0..256 传入以用 >>8 代替 /255 */
static u16 ui_shade_color(u16 c, u16 shade)
{
  u32 r = ((c >> 11) & 0x1F) * shade >> 8;
  u32 g = ((c >> 5)  & 0x3F) * shade >> 8;
  u32 b = (c        & 0x1F) * shade >> 8;
  return (u16)((r << 11) | (g << 5) | b);
}

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
  u32 per       = h * row_bytes;                 /* 单平面大小 */
  u32 off       = UI_ANIM_OFF + 256u + anim_frame_off(digit, half_idx, frame_idx);

  /* 一次性读 ink + mask 两个平面（420B），取代原来逐像素 1680 次 Flash_Read */
  Flash_Read(off, g_anim_buf, per * g_anim_planes);
  const u8 *ink  = g_anim_buf;
  const u8 *mask = g_anim_buf + per;

  /* 读背景数字该 half 的 1bpp 位图（40x42）一次读出 */
  const u8 *bg = ui_get_glyph((u32)'0' + (u32)bg_digit);
  if (!bg) bg = g_blank;
  u32 bg_rb = (g_cur_w + 7u) / 8u;
  u32 bg_y0 = (half_idx == 1) ? 42u : 0u;        /* half1(屏幕上半)取 42..83 行，half0(屏幕下半)取 0..41 行 */

  u16 shade = 256u;
  if (half_idx < 2 && frame_idx < 20)
    shade = (u16)g_anim_shade[half_idx][frame_idx] + 1u;   /* 0..255 -> 1..256 */

  /* 逐行读 UI 背景（小端->高字节先），直接合成进 g_blit。
     （合并背景读无收益：跳地址必须每行重发命令，FIFO 化后每字节轮询已消失，
     42 次事务只多 ~40µs CS 切换；且 g_blit 按 GRAM 顺序 x*h+yy 写回会与背景行重叠，
     RAM 仅 8K 无法加独立大缓冲，故保持逐行读。）
     GRAM 自动递增顺序：行(逻辑x)外、列(逻辑y)内 -> 像素索引 = x*h + yy */
  for (u32 yy = 0; yy < h; yy++)
  {
    u32 ly = (u32)(half_y0 + (int)yy);
    Flash_Read(g_ui_base + ly * 320u * 2u + (u32)card_x * 2u, g_line, w * 2u);
    for (u32 i = 0; i < w * 2u; i += 2)
    { u8 t = g_line[i]; g_line[i] = g_line[i + 1]; g_line[i + 1] = t; }

    u32 src_row = yy * row_bytes;
    u32 bg_row  = (bg_y0 + yy) * bg_rb;
    for (u32 x = 0; x < w; x++)
    {
      u32 idx = src_row + (x >> 3);
      u8  mb  = (mask[idx] >> (7 - (x & 7))) & 1;
      u16 pix;
      if (mb)
      {
        u8 ib = (ink[idx] >> (7 - (x & 7))) & 1;
        if (ib)
          pix = (shade == 256u) ? color : ui_shade_color(color, shade);
        else
        {
          u16 bg565 = (u16)((u16)g_line[x * 2] << 8) | g_line[x * 2 + 1];
          pix = (shade == 256u) ? bg565 : ui_shade_color(bg565, shade);
        }
      }
      else
      {
        u8 bb = (bg[bg_row + (x >> 3)] >> (7 - (x & 7))) & 1;
        u16 bg565 = (u16)((u16)g_line[x * 2] << 8) | g_line[x * 2 + 1];
        pix = bb ? color : bg565;
      }
      u32 pos = (x * h + yy) * 2u;
      g_blit[pos]     = HI8(pix);
      g_blit[pos + 1] = LO8(pix);
    }
  }

  /* 整 half 一次 RAMWR：窗口与 UI_DrawGlyphRect 同构（CASET=逻辑y, RASET=逻辑x） */
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
}

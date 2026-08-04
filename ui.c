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

/* ============ 全局状态 ============ */
static u32 g_ui_base   = 0;     /* UI 背景图在 Flash 中的偏移（来自分区头） */
static u32 g_font_base = 0;     /* 字库区在 Flash 中的偏移 */
static u8  g_line[320 * 2];     /* 一行像素临时缓冲（640B，含字节交换后数据） */

/* 字库索引缓存：build_flash.py 生成 {u32 code, u32 off, u16 w, u16 h} 表，按 code 升序 */
typedef struct { u32 code; u32 off; u16 w; u16 h; } font_idx_t;
static font_idx_t g_idx[200];
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
  if (g_font_count > 200) g_font_count = 200;
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

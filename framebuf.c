/************************************************************************************************************
 * @file    framebuf.c
 * @brief   帧缓冲 + 块传送(Blit)：把图形(真字体等)先画进内存缓冲，再整体刷到屏
 * @note    - 缓冲每像素 2 字节 = RGB565 原色，FB_Blit 直接逐像素发送，
 *            无需索引->颜色查表，传输路径最简洁（MCU 无 DMA，省一次内存读很划算）。
 *          - 代价：RAM = FB_W*FB_H*2 字节（当前 48*63*2 = 6048B ≈5.9KB，8KB 内可行）。
 *          - 绘制坐标使用“缓冲内局部坐标”，Blit 时再指定屏幕 (x,y) 与长宽。
 ************************************************************************************************************/
#include "framebuf.h"
#include "Display.h"
#include "font.h"

/* 帧缓冲：每像素 2 字节 = RGB565 原色（直接发送，无需调色板查表）。
 * 编译期断言：占用不能超过 SRAM 总量，避免无声溢出。 */
u16 g_fb[FB_W * FB_H];
_Static_assert(sizeof(g_fb) <= (8u * 1024u), "framebuffer exceeds SRAM");

//-----------------------------------------------------------------------------
/**
 * @brief  把整个帧缓冲清成同一 RGB565 颜色
 * @param  color : RGB565 颜色值（通常 CAL_BG 背景）
 * @retval 无
 */
void FB_Clear(u16 color)
{
  u32 i;
  for (i = 0; i < (u32)FB_W * FB_H; i++) g_fb[i] = color;
}

//-----------------------------------------------------------------------------
/**
 * @brief  在帧缓冲内写一个像素（缓冲局部坐标，自动越界保护）
 * @param  x,y  : 缓冲内坐标（0 ~ FB_W-1 / 0 ~ FB_H-1，越界则忽略）
 * @param  color: RGB565 颜色值
 * @retval 无
 */
void FB_SetPixel(u16 x, u16 y, u16 color)
{
  if (x >= FB_W || y >= FB_H) return;
  g_fb[(u32)y * FB_W + x] = color;
}

//-----------------------------------------------------------------------------
/**
 * @brief  在缓冲内画一个真字体字符（1bpp 位图，按 scale 放大成方块）
 * @param  x,y  : 缓冲内左上角（局部坐标）
 * @param  color: RGB565 颜色值
 * @param  scale: 放大倍数（1=原大，2=每个字模像素变 scale×scale 方块）
 */
void FB_DrawGlyph(char ch, int x, int y, u16 color, int scale)
{
  if (ch < FONT_FIRST || ch > FONT_LAST) return;
  const FONT_GLYPH *g = &g_font_glyphs[ch - FONT_FIRST];
  if (g->w == 0 || g->h == 0) return;

  const u8 *data = g_font_bits + g->off;
  int by = y + (int)g->yoff * scale;   /* 字符在 em 框内的顶部偏移(按 scale 缩放) */
  int gy, gx;

  for (gy = 0; gy < g->h; gy++)
  {
    int row = gy * g->pitch;
    for (gx = 0; gx < g->w; gx++)
    {
      int bit = (data[row + gx / 8] >> (7 - (gx % 8))) & 1;
      if (bit)
      {
        int sy, sx;
        for (sy = 0; sy < scale; sy++)
          for (sx = 0; sx < scale; sx++)
            FB_SetPixel(x + gx * scale + sx, by + gy * scale + sy, color);
      }
    }
  }
}

//-----------------------------------------------------------------------------
/**
 * @brief  在缓冲内画一个大字模字符（原生像素尺寸，scale=1，边缘平滑不发糊）
 * @param  gi   : 大字模索引：0-9 = 数字'0'-'9'，BIGFONT_COLON(10) = ':'
 * @param  x,y  : 缓冲内左上角（局部坐标，y 为 em 顶，内部自动加 yoff）
 * @param  color: RGB565 颜色值
 */
void FB_DrawBigGlyph(u8 gi, int x, int y, u16 color)
{
  if (gi > BIGFONT_COLON) return;
  const BIG_GLYPH *g = &g_big_glyphs[gi];
  const u8 *data = g_big_bits + g->off;
  int by = y + g->yoff;
  int gy, gx;

  for (gy = 0; gy < g->h; gy++)
  {
    int row = gy * g->pitch;
    for (gx = 0; gx < g->w; gx++)
    {
      if ((data[row + gx / 8] >> (7 - (gx % 8))) & 1)
        FB_SetPixel(x + gx, by + gy, color);
    }
  }
}

//-----------------------------------------------------------------------------
/**
 * @brief  在缓冲内画一个字符串（普通真字体，逐字符调用 FB_DrawGlyph）
 * @param  s       : ASCII 字符串（'\0' 结尾）
 * @param  x,y     : 缓冲内起点（左上角，局部坐标）
 * @param  color   : RGB565 颜色值
 * @param  scale   : 放大倍数
 * @param  spacing : 字符间距（像素）
 * @retval 无
 */
void FB_DrawString(const char *s, int x, int y, u16 color, int scale, int spacing)
{
  int cx = x;
  while (*s)
  {
    FB_DrawGlyph(*s, cx, y, color, scale);
    char ch = *s;
    int gw = 0;
    if (ch >= FONT_FIRST && ch <= FONT_LAST) gw = g_font_glyphs[ch - FONT_FIRST].w;
    cx += gw * scale + spacing;
    s++;
  }
}

//-----------------------------------------------------------------------------
/**
 * @brief  在缓冲内画一个固定位数的十进制数（高位补 0，无字符间距）
 * @param  val     : 要显示的非负整数
 * @param  ndigits : 显示位数（如 val=7,ndigits=2 -> "07"）
 * @param  x,y     : 缓冲内起点（左上角，局部坐标）
 * @param  color   : RGB565 颜色值
 * @param  scale   : 放大倍数
 * @retval 无
 */
void FB_DrawNumber(int val, int ndigits, int x, int y, u16 color, int scale)
{
  int cx = x, i;
  for (i = 0; i < ndigits; i++)
  {
    int p = 1, k;
    for (k = 0; k < ndigits - 1 - i; k++) p *= 10;
    int d = (val / p) % 10;
    char ch = (char)('0' + d);
    FB_DrawGlyph(ch, cx, y, color, scale);
    cx += (int)g_font_glyphs[ch - FONT_FIRST].w * scale;
  }
}

//-----------------------------------------------------------------------------
/**
 * @brief  计算字符串渲染后的像素总宽（用于居中/右对齐布局）
 * @param  s       : ASCII 字符串
 * @param  scale   : 放大倍数
 * @param  spacing : 字符间距（像素）
 * @retval 像素总宽（不含末字符后的间距；空串返回 0）
 */
int FB_TextWidth(const char *s, int scale, int spacing)
{
  int total = 0;
  while (*s)
  {
    int gw = 0;
    if (*s >= FONT_FIRST && *s <= FONT_LAST) gw = g_font_glyphs[*s - FONT_FIRST].w;
    total += gw * scale + spacing;
    s++;
  }
  if (total > 0) total -= spacing;
  return total;
}

//-----------------------------------------------------------------------------
/**
 * @brief  把缓冲(左上角 w*h)原样刷到屏幕 (x,y)（块传送）
 * @note   一次开 CASET/RASET 窗口，缓冲里直接是 RGB565，逐像素连续写入 GRAM，
 *         无调色板查表，效率高于逐像素 FillRect。绘制内容须落在 [0..w-1,0..h-1]。
 * @param  x,y : 屏幕目标位置（逻辑坐标，左上角）
 * @param  w,h : 传送区域宽高（超出屏幕或缓冲尺寸时自动裁剪）
 * @retval 无
 */
void FB_Blit(u16 x, u16 y, u16 w, u16 h)
{
  if (x >= SCREEN_W || y >= SCREEN_H) return;
  if (x + w > SCREEN_W) w = SCREEN_W - x;
  if (y + h > SCREEN_H) h = SCREEN_H - y;
  if (w > FB_W) w = FB_W;   /* 钳制到缓冲尺寸，防止越界读取 */
  if (h > FB_H) h = FB_H;
  if (w == 0 || h == 0) return;

  /* ---- 计算 GRAM 窗口：对逻辑矩形 4 角做 lcd_map 后取 min/max ----
   * MV=0：窗口 == 逻辑矩形（直接 col=x,row=y）。
   * MV=1：窗口为旋转后的矩形，GRAM 列=逻辑y、行=逻辑x。 */
  u16 c[4], r[4];
  lcd_map(x,         y,         &c[0], &r[0]);
  lcd_map(x,         y + h - 1, &c[1], &r[1]);
  lcd_map(x + w - 1, y,         &c[2], &r[2]);
  lcd_map(x + w - 1, y + h - 1, &c[3], &r[3]);
  u16 col0 = c[0], col1 = c[0], row0 = r[0], row1 = r[0];
  u8 k;
  for (k = 1; k < 4; k++)
  {
    if (c[k] < col0) col0 = c[k];
    if (c[k] > col1) col1 = c[k];
    if (r[k] < row0) row0 = r[k];
    if (r[k] > row1) row1 = r[k];
  }

  u16 ry, rx;
  SPI_LCD_Select();
  SPI_LCD_WriteCmd(ST7789_CASET);
  {
    u8 d[4] = {HI8(col0), LO8(col0), HI8(col1), LO8(col1)};
    SPI_LCD_WriteData(d, 4);
  }
  SPI_LCD_WriteCmd(ST7789_RASET);
  {
    u8 d[4] = {HI8(row0), LO8(row0), HI8(row1), LO8(row1)};
    SPI_LCD_WriteData(d, 4);
  }
  SPI_LCD_WriteCmd(ST7789_RAMWR);
  /* 按 GRAM 扫描顺序(行优先)写：每个 GRAM 单元反查回逻辑像素，再从帧缓冲取对应像素发送。
   * 这样 MV=1 旋转后，水平方向的整块图也能正确地落到对应的物理像素上。 */
  for (ry = row0; ry <= row1; ry++)
  {
    for (rx = col0; rx <= col1; rx++)
    {
      u16 xL, yL;
      lcd_unmap((u16)rx, (u16)ry, &xL, &yL);
      u16 fx = (u16)(xL - x);
      u16 fy = (u16)(yL - y);
      u16 pix = (fx < w && fy < h) ? g_fb[(u32)fy * FB_W + fx] : CAL_BG;
      SPI_LCD_WriteDataByte(HI8(pix));
      SPI_LCD_WriteDataByte(LO8(pix));
    }
  }
  SPI_LCD_Deselect();
}

/* ===========================================================================
 * 用法示例：
 *
 *   // 在屏幕 (sx,sy) 用真字体显示 2 位数值 val(00-99)，scale 放大倍数
 *   int w = (int)g_font_glyphs['0'-FONT_FIRST].w * 2 * scale;
 *   int h = FONT_EM_H * scale;
 *   FB_Clear(CAL_BG);                               // 背景色
 *   FB_DrawNumber(val, 2, 0, 0, CAL_TEXT, scale);  // 用主文字色画数字
 *   FB_Blit(sx, sy, w, h);                          // 整体刷到屏
 * =========================================================================== */

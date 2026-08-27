/************************************************************************************************************
 * @file    gfx.c
 * @brief   图形库：5x7 文字 + 七段数码管数字（矢量绘制，无需字库）
 * @note   所有绘制使用“逻辑坐标”（横屏 320x240），底层 Display_FillRect 自动做 MV=1 坐标交换
 ************************************************************************************************************/
#include "gfx.h"

/* ------------------------------------------------------------------
 * 5x7 点阵字库（仅含日历所需字符）
 * 每个字形 7 字节，行优先；bit4..bit0 对应从左到右 5 列（bit4 为最左）
 * ------------------------------------------------------------------ */
typedef struct
{
  char     c;
  uint8_t  b[7];
} GLYPH;

static const GLYPH FONT[] =
{
  {' ', {0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
  {'/', {0x02,0x02,0x04,0x04,0x08,0x10,0x10}},
  {'%', {0x18,0x14,0x04,0x08,0x12,0x03,0x03}},
  {'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
  {'C', {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
  {'D', {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
  {'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
  {'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
  {'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
  {'I', {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}},
  {'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
  {'N', {0x11,0x19,0x15,0x15,0x13,0x11,0x11}},
  {'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
  {'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
  {'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
  {'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
  {'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
  {'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
  {'W', {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}},
};

static const uint8_t* get_glyph(char c)
{
  for (u16 i = 0; i < sizeof(FONT) / sizeof(FONT[0]); i++)
  {
    if (FONT[i].c == c) return FONT[i].b;
  }
  return 0;  /* 未收录字符不绘制 */
}

/* ------------------------------------------------------------------
 * 文字绘制
 * ------------------------------------------------------------------ */
void GFX_DrawChar(char c, int x, int y, int scale, u16 color)
{
  const uint8_t *g = get_glyph(c);
  if (!g) return;

  for (int r = 0; r < 7; r++)
  {
    uint8_t row = g[r];
    for (int col = 0; col < 5; col++)
    {
      if (row & (1 << (4 - col)))
      {
        Display_FillRect(x + col * scale, y + r * scale, scale, scale, color);
      }
    }
  }
}

void GFX_DrawString(const char *s, int x, int y, int scale, u16 color)
{
  int cx = x;
  while (*s)
  {
    GFX_DrawChar(*s, cx, y, scale, color);
    cx += 6 * scale;   /* 5 列 + 1 列间距 */
    s++;
  }
}

/* ------------------------------------------------------------------
 * 七段数码管（矢量绘制，无需字库）
 * 段位定义： a=1,b=2,c=4,d=8,e=16,f=32,g=64
 *    aaa
 *   f   b
 *    ggg
 *   e   c
 *    ddd
 * ------------------------------------------------------------------ */
static const uint8_t SEG_MASK[10] =
{
  0x3F, /* 0 a b c d e f     */
  0x06, /* 1 b c             */
  0x5B, /* 2 a b g e d       */
  0x4F, /* 3 a b g c d       */
  0x66, /* 4 f g b c         */
  0x6D, /* 5 a f g c d       */
  0x7D, /* 6 a f g e c d     */
  0x07, /* 7 a b c           */
  0x7F, /* 8 all             */
  0x6F  /* 9 a b c d f g     */
};

void GFX_Draw7SegDigit(int x, int y, int w, int h, int t, int digit, u16 color)
{
  if (digit < 0 || digit > 9) digit = 8;
  uint8_t m = SEG_MASK[digit];
  int h2 = h / 2;

  if (m & 0x01) Display_FillRect(x + t,       y,       w - 2*t, t,       color);  /* a */
  if (m & 0x02) Display_FillRect(x + w - t,   y + t,   t,       h2 - t,  color);  /* b */
  if (m & 0x04) Display_FillRect(x + w - t,   y + h2,  t,       h2 - t,  color);  /* c */
  if (m & 0x08) Display_FillRect(x + t,       y + h - t, w - 2*t, t,     color);  /* d */
  if (m & 0x10) Display_FillRect(x,           y + h2,  t,       h2 - t,  color);  /* e */
  if (m & 0x20) Display_FillRect(x,           y + t,   t,       h2 - t,  color);  /* f */
  if (m & 0x40) Display_FillRect(x + t,       y + h2 - t/2, w - 2*t, t, color);   /* g */
}

void GFX_DrawNumber7Seg(int x, int y, int cw, int ch, int t,
                        u16 val, int ndigits, u16 color, u16 bg)
{
  for (int i = 0; i < ndigits; i++)
  {
    int cx = x + i * cw;
    Display_FillRect(cx, y, cw, ch, bg);   /* 清除本格背景 */

    int p = 1;
    for (int k = 0; k < ndigits - 1 - i; k++) p *= 10;
    int d = (val / p) % 10;
    GFX_Draw7SegDigit(cx, y, cw, ch, t, d, color);
  }
}

void GFX_DrawColon(int x, int y, int cw, int ch, int t, u16 color)
{
  int s = t;                       /* 冒号点大小 */
  int cx = x + cw / 2 - s / 2;
  Display_FillRect(cx, y + ch * 0.25, s, s, color);
  Display_FillRect(cx, y + ch * 0.65, s, s, color);
}

void GFX_DrawClock(int x, int y, int cw, int ch, int t,
                   int hh, int mm, u16 color, u16 bg)
{
  int total_w = 4 * cw + cw / 2;   /* 2位数 + 冒号 + 2位数 */
  Display_FillRect(x - 2, y - 2, total_w + 4, ch + 4, bg);   /* 清除整块 */

  GFX_DrawNumber7Seg(x, y, cw, ch, t, hh, 2, color, bg);
  GFX_DrawColon(x + 2 * cw, y, cw, ch, t, color);
  GFX_DrawNumber7Seg(x + 2 * cw + cw / 2, y, cw, ch, t, mm, 2, color, bg);
}

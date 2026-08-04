/************************************************************************************************************
 * @file    calendar.c
 * @brief   日历界面布局与刷新（横屏 320x240）
 * @note    本文件不再程序化绘制日历，而是在 Flash 中的 UI 背景图上，用 Flash 字库
 *          （ui.c）按 flash_interface.md / ui_spec.md 的坐标叠加动态文字：
 *            - 时钟数字：卡片 (x,14,30,56)，色 #1a1a1a
 *            - 冒号：    (x,14,14,56)，色 #e0e0e0
 *            - 日期徽章：y=82, h=22，白/黑字
 *            - 温湿度：  y=150, h=24，白字（图标已绘于背景图）
 *          增量刷新：仅重绘发生变化的区块，避免整屏重绘闪烁。
 ************************************************************************************************************/
#include "calendar.h"
#include "ui.h"
#include "time_module.h"

/* 设备对 GRAM 做垂直镜像：设计坐标 y 需换算为 GRAM 行 ry = 239 - y - h（x 不变）。
   背景 bin 与字库字形均已 FV 预补偿，二者一致才能使文字落在卡片正确位置。 */
#define YF(y, h) (239 - (y) - (h))

/* 数字尺寸变体 code 段（与 build_flash.py 对应）：避免日期/温湿度误查时钟大号字模被缩放。
   0x30+d=时钟大号(30x56)；0xE000+d=日期小号(10x12)；0xE100+d=温湿度(12x13) */
#define DG_SMALL 0xE000
#define DG_TH    0xE100

/* Zeller 公式算星期（0=周一 .. 6=周日） */
int CalcWeekday(int y, int m, int d)
{
  if (m < 3) { m += 12; y--; }
  int K = y % 100, J = y / 100;
  int h = (d + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
  return (h + 5) % 7;
}
/* 中文星期（用 Unicode 码点，避免源文件中文字面量被 gcc 误解析）
   星=0x661F 期=0x671F 一=0x4E00 二=0x4E8C 三=0x4E09 四=0x56DB 五=0x4E94 六=0x516D 日=0x65E5 */
static const u32 WD_CN[7][3] = {
  {0x661F, 0x671F, 0x4E00},
  {0x661F, 0x671F, 0x4E8C},
  {0x661F, 0x671F, 0x4E09},
  {0x661F, 0x671F, 0x56DB},
  {0x661F, 0x671F, 0x4E94},
  {0x661F, 0x671F, 0x516D},
  {0x661F, 0x671F, 0x65E5},
};

/* 增量刷新缓存 */
static u8  c_hh = 0xFF, c_mm = 0xFF, c_ss = 0xFF;
static u16 c_yy = 0xFFFF;
static u8  c_mo = 0xFF, c_dd = 0xFF, c_wd = 0xFF;
static int c_temp = 0x7FFF, c_humi = 0x7FFF;

/* ---- 时钟数字：40x84 卡片内（字库 1:1） ---- */
static void draw_clock_digit(int x, int d)
{
  UI_DrawGlyphRect(x, YF(14, 84), 40, 84, (u32)('0' + d), UI_C_DIGIT);
}
/* ---- 冒号：14x84 区域（组间 18 间隙中居中，左右各 2px 呼吸）；on=0 时用空格字模 ---- */
static void draw_colon(int x, int on)
{
  UI_DrawGlyphRect(x, YF(14, 84), 14, 84, on ? (u32)':' : (u32)' ', UI_C_COLON);
}

/* ============ 蓝牙状态动态绘制 ============
   背景图药丸：(x=196, y=156, w=108, h=24)；图标已占左侧，右缘 x=217。
   状态文字"已连接/未连接"共 3 个 18x22 汉字，在图标右缘与药丸右缘(303)
   之间居中绘制。字体颜色沿用图标蓝。 */
/* 已=0x5DF2 连=0x8FDE 接=0x63A5 未=0x672A */
static void draw_bt_state(int connected)
{
  static const u32 BT_CONN[3] = {0x5DF2, 0x8FDE, 0x63A5};   /* 已连接 */
  static const u32 BT_DISC[3] = {0x672A, 0x8FDE, 0x63A5};   /* 未连接 */
  int nw = 3 * 18;                          /* 3 字 x 18px */
  int x  = 217 + (303 - 217 - nw) / 2;      /* 图标右缘 217 到药丸右缘 303 居中 */
  UI_DrawGlyphs(connected ? BT_CONN : BT_DISC, 3, x, YF(156, 22), 18, 22, UI_RGB(66,165,245));
}

static u8 s_bt_connected = 0xFF;            /* 上次绘制状态（0xFF=未画过） */
void Calendar_SetBtState(int connected)
{
  u8 c = connected ? 1 : 0;
  if (c == s_bt_connected) return;          /* 状态没变不重绘 */
  s_bt_connected = c;
  draw_bt_state(c);
}

/* ============ 初始化：上传背景 + 画初始静态元素 ============ */
void Calendar_Init(void)
{
  if (UI_LoadHeader() != 0)
  {
    Display_Fill_Color(0xFF94);   /* 分区头损坏：回退淡黄底，至少屏有显示 */
    return;
  }
  UI_UploadBackground();          /* 整屏上传 Flash 中的新 UI 背景图 */
  UI_CacheFont();                 /* 缓存字库索引表 */

  /* 初始冒号（之后由 BFTM1 半秒中断驱动 Calendar_ColonBlink 闪烁） */
  draw_colon(101, 1);
  draw_colon(205, 1);

  /* 强制首帧重绘所有动态块 */
  c_yy = 0xFFFF; c_mo = 0xFF; c_dd = 0xFF; c_wd = 0xFF;
  c_hh = c_mm = c_ss = 0xFF;
  c_temp = c_humi = 0x7FFF;

  /* 蓝牙状态默认"未连接"（等状态机首次判定后更新） */
  s_bt_connected = 0xFF;
  draw_bt_state(0);
}

/* ============ 增量刷新：仅重绘变化区块 ============ */
void Calendar_Refresh(TimeStruct *t, int temp, int humi)
{
  int wd = CalcWeekday(t->year, t->month, t->day);

  /* ---- 时钟数字（4 位，分/时/秒各自变化检测） ---- */
  if (t->hour != c_hh)
  {
    draw_clock_digit(13, t->hour / 10);
    draw_clock_digit(59, t->hour % 10);
    c_hh = t->hour;
  }
  if (t->minute != c_mm)
  {
    draw_clock_digit(117, t->minute / 10);
    draw_clock_digit(163, t->minute % 10);
    c_mm = t->minute;
  }
  if (t->second != c_ss)
  {
    draw_clock_digit(221, t->second / 10);
    draw_clock_digit(267, t->second % 10);
    c_ss = t->second;
  }

  /* ---- 日期徽章（y=82, h=22，字高 12，整串在徽章内居中） ---- */
  if (t->year != c_yy || t->month != c_mo || t->day != c_dd || wd != c_wd)
  {
    char yb[8], mb[4], db[4];
    int n = 0; int y = t->year;
    yb[n++] = (char)('0' + y / 1000); yb[n++] = (char)('0' + (y / 100) % 10);
    yb[n++] = (char)('0' + (y / 10) % 10); yb[n++] = (char)('0' + y % 10); yb[n] = 0;
    mb[0] = (char)('0' + t->month / 10); mb[1] = (char)('0' + t->month % 10); mb[2] = 0;
    db[0] = (char)('0' + t->day / 10);   db[1] = (char)('0' + t->day % 10);   db[2] = 0;

    UI_DrawVarDigits(yb, 19, YF(116, 22), UI_C_WHITE, DG_SMALL);   /* 年 小号数字 */
    UI_DrawVarDigits(mb, 95, YF(116, 22), UI_C_WHITE, DG_SMALL);   /* 月 小号数字 */
    UI_DrawVarDigits(db, 139, YF(116, 22), UI_C_BLACK, DG_SMALL);  /* 日 小号数字(黑) */
    UI_DrawGlyphs(WD_CN[wd], 3, 180, YF(116, 22), 18, 22, UI_C_WHITE);  /* 星期(3*18=54 居中) */
    { static const u32 lunar[4] = {0x516D, 0x6708, 0x5341, 0x4E03}; /* 六月十七 占位 */
      UI_DrawGlyphs(lunar, 4, 240, YF(116, 22), 18, 22, UI_C_WHITE); }  /* 农历(4*18=72 居中) */
    c_yy = t->year; c_mo = t->month; c_dd = t->day; c_wd = (u8)wd;
  }

  /* ---- 温湿度数值（一位小数；值单位 0.1，如 temp=255 -> "25.5"） ----
     数值在传感器格内动态居中：温度格 x[190..246]、湿度格 x[250..306]，
     数字 8 宽 + 小数点 3 宽，按实际串宽居中，任何位数都不超界。 */
  if (temp != c_temp)
  {
    char tb[8]; int n = 0;
    int ti = temp / 10, tf = temp % 10;
    if (tf < 0) tf = -tf;
    if (ti < 0) { tb[n++] = '-'; ti = -ti; }
    if (ti >= 100) tb[n++] = (char)('0' + ti / 100);
    if (ti >= 10 || n) tb[n++] = (char)('0' + (ti / 10) % 10);
    tb[n++] = (char)('0' + ti % 10);
    tb[n++] = '.';
    tb[n++] = (char)('0' + tf); tb[n] = 0;
    /* 温度格 x[190..246]，图标占左端 9px，数值居中于图标右侧剩余 47px */
    int nw = 0; for (char *q = tb; *q; q++) nw += (*q == '.') ? 3 : 8;
    int tx = 199 + (47 - nw) / 2;            /* 图标右缘 202 后居中 */
    UI_DrawVarDigits(tb, tx, YF(187, 22), UI_C_WHITE, DG_TH);
    c_temp = temp;
  }
  if (humi != c_humi)
  {
    char hb[8]; int n = 0;
    int hi = humi / 10, hf = humi % 10; if (hf < 0) hf = -hf;
    if (hi >= 100) hb[n++] = (char)('0' + hi / 100);
    if (hi >= 10 || n) hb[n++] = (char)('0' + (hi / 10) % 10);
    hb[n++] = (char)('0' + hi % 10);
    hb[n++] = '.';
    hb[n++] = (char)('0' + hf); hb[n] = 0;
    /* 湿度格 x[250..306]，图标占左端 12px，数值居中于剩余 44px */
    int nw = 0; for (char *q = hb; *q; q++) nw += (*q == '.') ? 3 : 8;
    int hx = 262 + (44 - nw) / 2;            /* 图标右缘 265 后居中 */
    UI_DrawVarDigits(hb, hx, YF(187, 22), UI_C_WHITE, DG_TH);
    c_humi = humi;
  }
}

/* ============ 冒号闪烁（0.5s 中断驱动） ============ */
void Calendar_ColonBlink(void)
{
  static u8 colon_on = 1;
  colon_on ^= 1;
  draw_colon(101, colon_on);
  draw_colon(205, colon_on);
}



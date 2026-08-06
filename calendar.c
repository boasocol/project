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

/* ============ 翻页动画状态（6 张卡片对应 6 个时/分/秒位） ============
   卡片 x 顺序（与 clock_xs 一致）：H1=13, H2=59, M1=117, M2=163, S1=221, S2=267

   ANI2 双位面模型：
     阶段 A(state=1)：旧数字的【物理上半】绕中缝向下倒，背景露出新数字上半
     阶段 B(state=2)：新数字的【物理下半】从中缝向下展开，背景是旧数字下半
     两半各由数据头决定帧数（UI_Anim_GetFrames），整段翻页约 2*N*间隔 后 IDLE。
   half_idx 语义（与 ui.c 一致）：0=物理下半，1=物理上半。 */
/* 每半翻页帧数由动画数据头决定（UI_Anim_GetFrames），不再硬编码 */
typedef struct { u8 state; u8 new_digit; u8 old_digit; u8 frame; } FlipState;
static FlipState g_flip[6];
static const int g_card_x[6] = {13, 59, 117, 163, 221, 267};
/* 卡片上下半的 y：整卡已验证 GRAM 起始 = YF(14,84)=141（141=数字顶部）。
   上半 = 141 起始，下半 = 141+42 = 183 起始。 */
#define FLIP_CARD_Y0     (YF(14, 84))     /* 整卡 GRAM 起始列（=141） */
#define FLIP_HALF_H      42

/* 串行翻页：同一时刻只翻一位，按 秒个位→秒十位→分个位→分十位→时个位→时十位 顺序 */
static int g_active = -1;                 /* 当前正在翻页的槽，-1 表示无 */
static u8  g_pending[6];                  /* 待翻页队列标记 */
static u8  g_pend_old[6], g_pend_new[6];  /* 队列中的 old/new 目标数字 */
static u8  g_disp[6];                     /* 各槽当前已显示数字（翻完或静态绘制时更新） */

static void flip_start(int slot, u8 old_digit, u8 new_digit)
{
  if (old_digit > 9) old_digit = new_digit;   /* 首次无旧值：退化为静态刷新 */
  /* 一律记录目标并排队。即使该位正在翻页/已排队，也更新目标，
     避免“翻到一半来了新变化”时把这次翻转弄丢（旧逻辑不置 g_pending 导致跳数字）。 */
  g_pend_old[slot] = old_digit;
  g_pend_new[slot] = new_digit;
  g_pending[slot]  = 1;
}

/* 翻页动画是否可用（Calendar_Init 时记录 UI_Anim_Ready() 结果） */
static u8 g_anim_ok = 0;

/* 每 6ms 推进一帧（main 主循环按 BFTM0 1ms 计数调用）。
   翻页期间会调用 UI_Anim_DrawFrame 画当前帧；翻完恢复 IDLE（下次 Refresh 画静态）。 */
void Calendar_FlipTick(u32 now_ms)
{
  static u32 last_ms = 0;
  if (!g_anim_ok) return;              /* 动画区不可用：纯静态显示 */
  if (now_ms - last_ms < 6u) return;   /* 6ms 间隔（原 8ms，提速）；BFTM 1ms 节拍做容差 */
  last_ms = now_ms;

  /* 无活动槽时，按 低->高 顺序（秒个位…时十位）取下一个待翻页 */
  if (g_active < 0)
  {
    static const int order[6] = {5, 4, 3, 2, 1, 0};  /* 秒个位,秒十位,分个位,分十位,时个位,时十位 */
    for (int k = 0; k < 6; k++)
    {
      int s = order[k];
      if (g_pending[s])
      {
        u8 new_d = g_pend_new[s];
        u8 old_d = (g_disp[s] <= 9) ? g_disp[s] : g_pend_old[s];
        if (old_d == new_d) { g_pending[s] = 0; continue; }   /* 已是目标：无需翻 */
        g_flip[s].old_digit = old_d;
        g_flip[s].new_digit = new_d;
        g_flip[s].state     = 1;
        g_flip[s].frame     = 0;
        g_pending[s]        = 0;
        g_active            = s;
        break;
      }
    }
  }

  if (g_active < 0) return;            /* 没有待翻页 */

  int i   = g_active;
  u8  st  = g_flip[i].state;
  if (st == 0) { g_active = -1; return; }

  int hy;
  int digit, bg_digit, half_idx;
  if (st == 1)
  {   /* 阶段 A：屏幕上半(half_idx=1, GRAM183-224)折叠旧数字，背景是新数字上半 */
      half_idx = 1;
      digit    = g_flip[i].old_digit;
      bg_digit = g_flip[i].new_digit;
      hy       = FLIP_CARD_Y0 + FLIP_HALF_H;   /* GRAM 起始 = 183 */
  }
  else
  {   /* 阶段 B：屏幕下半(half_idx=0, GRAM141-182)展开新数字，背景是旧数字下半 */
      half_idx = 0;
      digit    = g_flip[i].new_digit;
      bg_digit = g_flip[i].old_digit;
      hy       = FLIP_CARD_Y0;                 /* GRAM 起始 = 141 */
  }

  UI_Anim_DrawFrame(g_card_x[i], hy, digit, half_idx, g_flip[i].frame,
                    bg_digit, UI_C_DIGIT);

  g_flip[i].frame++;
  if (g_flip[i].frame >= (u8)UI_Anim_GetFrames())
  {
    g_flip[i].frame = 0;
    if (st == 1)
      g_flip[i].state = 2;         /* 上半翻完，切阶段 B（仍为本槽） */
    else
    {   /* 下半翻完：整卡 = 新数字，更新已显示值并释放活动槽 */
        g_flip[i].state = 0;
        g_disp[i]       = g_flip[i].new_digit;
        g_active        = -1;
    }
  }
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
  g_anim_ok = (UI_Anim_Init() == 0 && UI_Anim_Ready()) ? 1 : 0;

  /* 翻页状态清零（首次 Refresh 若动画可用则按串行队列启动翻页） */
  for (int i = 0; i < 6; i++) { g_flip[i].state = 0; g_pending[i] = 0; g_disp[i] = 0; }
  g_active = -1;

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

  /* ---- 时钟数字（6 位独立变化检测，只翻真正变化的那一位） ----
     动画可用：变化位启动翻页（Calendar_FlipTick 推进帧）；
     动画不可用（如 Flash 未烧新 ANIM 区）：直接画静态字模，保证数字必现。 */
  if ((t->hour / 10) != (c_hh / 10))
  {
    if (g_anim_ok) flip_start(0, c_hh / 10, t->hour / 10);
    else           draw_clock_digit(13, t->hour / 10);
  }
  if ((t->hour % 10) != (c_hh % 10))
  {
    if (g_anim_ok) flip_start(1, c_hh % 10, t->hour % 10);
    else           draw_clock_digit(59, t->hour % 10);
  }
  c_hh = t->hour;

  if ((t->minute / 10) != (c_mm / 10))
  {
    if (g_anim_ok) flip_start(2, c_mm / 10, t->minute / 10);
    else           draw_clock_digit(117, t->minute / 10);
  }
  if ((t->minute % 10) != (c_mm % 10))
  {
    if (g_anim_ok) flip_start(3, c_mm % 10, t->minute % 10);
    else           draw_clock_digit(163, t->minute % 10);
  }
  c_mm = t->minute;

  if ((t->second / 10) != (c_ss / 10))
  {
    if (g_anim_ok) flip_start(4, c_ss / 10, t->second / 10);
    else           draw_clock_digit(221, t->second / 10);
  }
  if ((t->second % 10) != (c_ss % 10))
  {
    if (g_anim_ok) flip_start(5, c_ss % 10, t->second % 10);
    else           draw_clock_digit(267, t->second % 10);
  }
  c_ss = t->second;

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



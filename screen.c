/************************************************************************************************************
 * @file    screen.c
 * @brief   多屏状态机（2026-08-27）：主界面 / 倒计时(屏1) / 闹钟(屏2) + 倒计时&闹钟业务状态机
 *
 * 切屏模型：UI_SetScreen（ui.c）上传对应屏背景 + 设背景基址/人物区；本模块 Screen_Draw
 * 每帧按当前屏增量绘制动态元素（倒计时/闹钟屏），主屏仍由 calendar.c 负责。
 *
 * 倒计时屏1（MM:SS 大数字 40x84 白 + 状态chip + 进度条 + 开始/暂停/重置）：
 *   槽中心 x=93/133/187/227、冒号 160、块居中 x[73..247]、y=56..140（UI_Show 设计坐标 y=56）。
 *   进度条 [24,142]-[296,148]：状态色按已用时比例填充。
 *   状态chip [238,17]-[300,35]：运行绿/暂停黄/结束红 + 深字（UI_DrawTextInk）。
 *   按钮文字：开始/暂停 [16,166]-[152,218]、重置 [160,166]-[216,218]。
 * 闹钟屏2（HH:MM 大数字 y=78..162 垂直居中 + 开关chip + 星期chip + 强调线）：
 *   槽同上；开关chip [132,17]-[196,35]（已开启绿/已关闭灰）；星期chips y=206..228、x=74+i*30、
 *   选中绿深字/未选面板灰；强调线 [16,170]-[304,171]（响铃红）。
 *
 * 业务状态机：
 *   倒计时：0x84 分钟(d0=1-60)/0x88 番茄(25min) 启动切屏1；0x85 暂停/0x86 继续/0x87 结束
 *           （结束停 3s 回主屏）；到 0 → VOICE_TX_TIMER_BEEP + 停 3s 回主屏。
 *   闹钟：0x8A 设(写配置+切屏2)/0x8B 开关/0x8C 循环(0单次 1一~五 2一~六 3一~日)/0x8D 铃音；
 *          到点切屏2 + VOICE_TX_RING + 强调线红；0xB1 别响/0x8B 关/60s 自动停回主屏；
 *          0xB0 贪睡 = 停响 + 闹钟时间 +10min。
 ************************************************************************************************************/
#include "screen.h"
#include "ui.h"
#include "Display.h"
#include "calendar.h"   /* CalcWeekday */
#include "BFTM0.h"      /* bftm0_ct（1ms 计数，回主屏延时用） */

/* ==================== 颜色（概念B 色板 RGB565，与 ui-design-system 令牌一致） ==================== */
#define SC_C_GREEN   UI_RGB(34, 197, 94)    /* 状态绿 #22C55E（运行/选中） */
#define SC_C_YELLOW  UI_RGB(255, 213, 79)   /* 状态黄 #FFD54F（暂停） */
#define SC_C_RED     UI_RGB(239, 68, 68)    /* 危险红 #EF4444（结束/响铃） */
#define SC_C_BLUE    UI_RGB(37, 99, 235)    /* 主蓝 #2563EB（强调线正常） */
#define SC_C_GRAY    UI_RGB(148, 163, 184)  /* 次文字灰 #94A3B8（未选） */
#define SC_C_DARK    UI_RGB(10, 15, 30)     /* chip 深字（绿/黄底） */
#define SC_C_EDGE    UI_RGB(37, 47, 68)     /* 面板高亮底 #252F44（未选chip底） */
#define SC_C_WHITE   UI_C_DIGIT             /* 大数字白 #F8FAFC */

/* ==================== 大数字/冒号槽（屏1/屏2 共用，块居中 x[73..247]） ==================== */
#define SC_DIG_LEFT  73    /* 槽中心 93/133/187/227 -> 左 73/113/167/207 */
#define SC_DIG_STEP  40
#define SC_COL_LEFT  153   /* 冒号中心 160 -> 左 153（14 宽） */
#define SC_CNT_Y     56    /* 倒计时大数字顶 y（84 高 -> 56..140） */
#define SC_ALM_Y     78    /* 闹钟大数字顶 y（84 高 -> 78..162，垂直居中） */

/* ==================== 倒计时状态 ==================== */
static cnt_state_t g_cnt = CNT_STOP;   /* 当前状态 */
static u16 g_cnt_total = 0;            /* 总秒数（进度条分母） */
static u16 g_cnt_remain = 0;           /* 剩余秒数 */
static u32 g_cnt_ret_ms = 0;           /* 结束/停止后回主屏的 bftm0_ct 时刻（0=未定时） */

/* ==================== 闹钟运行时状态（配置在 ui_cfg_t，此处补 loop/ringing 等） ==================== */
static struct {
  u8  loop;         /* 0单次(按日响) 1一~五 2一~六 3一~日 */
  u8  ring_sel;     /* 铃音 0/1 */
  u8  ringing;      /* 1=正在响铃 */
  u8  rung;         /* 本分钟已响过（防同分重复触发） */
  u32 ring_start;   /* 响铃起始 bftm0_ct（60s 自动停） */
  u8  snooze;       /* 贪睡计数 */
} g_alm;

/* ==================== 增量刷新缓存（0xFF/非法 -> 首帧强制全绘） ==================== */
static u8  c_drawn_screen = 0xFF;   /* 已绘制的屏号（切屏触发静态元素重绘） */
static u8  c_cnt_m1 = 0xFF, c_cnt_m2 = 0xFF, c_cnt_s1 = 0xFF, c_cnt_s2 = 0xFF;
static u8  c_cnt_state = 0xFF;
static int c_cnt_prog = -1;         /* 已绘制的进度条填充宽度 */
static u8  c_alm_h1 = 0xFF, c_alm_h2 = 0xFF, c_alm_m1 = 0xFF, c_alm_m2 = 0xFF;
static u8  c_alm_on = 0xFF, c_alm_loop = 0xFF, c_alm_ring = 0xFF;

/* ==================== 小工具 ==================== */
static void draw_digit_at(int x, int y, u8 d, u16 color)
{
  char b[2] = { (char)('0' + d), 0 };
  UI_Show(x, y, b, UI_SLOT_CLOCK, color);
}

/* 冒号（14x84，屏1/2 共用；静态不闪） */
static void draw_colon_at(int x, int y)
{
  UI_DrawGlyphRect(x, 239 - (y + 84) + 1, 14, 84, (u32)':', UI_C_COLON);
}

/* ==================== 倒计时 ==================== */
static int weekday_match(void)
{
  /* 用当前 RTC 的星期匹配循环模式；0=单次(按日响)。 */
  TimeStruct t;
  TimeModule_GetTime(&t);
  int wd = CalcWeekday(t.year, t.month, t.day);   /* 0=周一..6=周日 */
  switch (g_alm.loop)
  {
    case 0:  return 1;                    /* 单次：任意一天（每日响） */
    case 1:  return (wd <= 4) ? 1 : 0;    /* 周一~五 */
    case 2:  return (wd <= 5) ? 1 : 0;    /* 周一~六 */
    default: return 1;                    /* 3=周一~日（每天） */
  }
}

void Screen_CntStart(u16 total_s)
{
  if (total_s == 0) return;
  g_cnt = CNT_RUN;
  g_cnt_total  = total_s;
  g_cnt_remain = total_s;
  g_cnt_ret_ms = 0;
  UI_SetScreen(UI_SCREEN_CNT);
}

void Screen_CntPause(void)
{
  if (g_cnt == CNT_RUN) g_cnt = CNT_PAUSE;
}

void Screen_CntResume(void)
{
  if (g_cnt == CNT_PAUSE) g_cnt = CNT_RUN;
}

void Screen_CntStop(void)
{
  if (g_cnt == CNT_STOP) return;
  g_cnt = CNT_DONE;
  g_cnt_ret_ms = (u32)bftm0_ct + 3000u;   /* 停 3s 回主屏 */
}

int Screen_CntActive(void)
{
  return (g_cnt == CNT_RUN || g_cnt == CNT_PAUSE) ? 1 : 0;
}

/* 倒计时每秒推进（Screen_OnSecond 调） */
static void cnt_tick(void)
{
  if (g_cnt != CNT_RUN) return;
  if (g_cnt_remain > 0)
  {
    g_cnt_remain--;
    if (g_cnt_remain == 0)
    {
      g_cnt = CNT_DONE;
      g_cnt_ret_ms = (u32)bftm0_ct + 3000u;
      Voice_Send(VOICE_TX_TIMER_BEEP, 0, 0, 0, 0);   /* 倒计时提示音 */
    }
  }
}

/* 倒计时屏每帧增量绘制 */
static void draw_cnt(TimeStruct *t)
{
  (void)t;
  u16 rem = g_cnt_remain;

  /* 大数字 MM:SS（分钟 2 位，秒 2 位） */
  u8 m1 = (u8)(rem / 600), m2 = (u8)((rem / 60) % 10);
  u8 s1 = (u8)((rem % 60) / 10), s2 = (u8)(rem % 10);
  if (m1 != c_cnt_m1) { draw_digit_at(SC_DIG_LEFT,             SC_CNT_Y, m1, SC_C_WHITE); c_cnt_m1 = m1; }
  if (m2 != c_cnt_m2) { draw_digit_at(SC_DIG_LEFT + SC_DIG_STEP, SC_CNT_Y, m2, SC_C_WHITE); c_cnt_m2 = m2; }
  if (s1 != c_cnt_s1) { draw_digit_at(SC_DIG_LEFT + 94,         SC_CNT_Y, s1, SC_C_WHITE); c_cnt_s1 = s1; }
  if (s2 != c_cnt_s2) { draw_digit_at(SC_DIG_LEFT + 94 + 40,    SC_CNT_Y, s2, SC_C_WHITE); c_cnt_s2 = s2; }

  /* 状态chip + 按钮1文字（状态相关）：运行绿/暂停黄/结束红 + 深字；按钮 开始/暂停 */
  if (g_cnt != c_cnt_state)
  {
    const char *tx; u16 fill;
    switch (g_cnt)
    {
      case CNT_RUN:   tx = "运行中"; fill = SC_C_GREEN;  break;
      case CNT_PAUSE: tx = "暂停中"; fill = SC_C_YELLOW; break;
      default:        tx = "已结束"; fill = SC_C_RED;    break;   /* STOP/DONE 都按结束红显示 */
    }
    /* 填整 chip + 深字墨点；chip 文字用 UI_SLOT_WEEK（18 格）居中。
       chip 背景 18 高(17..35)，字模 22 高 -> 填 22 高(15..36) 覆盖文字溢出行，防旧墨残留 */
    Display_FillRect(238, 15, 62, 22, fill);
    UI_DrawTextInk(242, 15, tx, UI_SLOT_WEEK, SC_C_DARK);   /* 3 字 54 宽，中心 269 */
    /* 按钮1 文字（运行/暂停=暂停，否则=开始）：只清文字区(36x22)保留背景圆角按钮 */
    {
      const char *b1 = (g_cnt == CNT_RUN || g_cnt == CNT_PAUSE) ? "暂停" : "开始";
      Display_FillRect(66, 181, 36, 22, SC_C_EDGE);
      UI_DrawTextInk(66, 181, b1, UI_SLOT_WEEK, SC_C_WHITE);
    }
    c_cnt_state = (u8)g_cnt;
  }

  /* 进度条 [24,142]-[296,148]：按已用比例填充状态色 */
  int filled = (g_cnt_total > 0) ? (int)((u32)(g_cnt_total - g_cnt_remain) * 272u / g_cnt_total) : 0;
  if (filled != c_cnt_prog)
  {
    u16 bar = (g_cnt == CNT_PAUSE) ? SC_C_YELLOW : (g_cnt == CNT_DONE ? SC_C_RED : SC_C_GREEN);
    Display_FillRect(24, 142, 272, 7, SC_C_EDGE);        /* 先清轨道 */
    if (filled > 0) Display_FillRect(24, 142, filled, 7, bar);
    c_cnt_prog = filled;
  }
}

/* ==================== 闹钟 ==================== */
void Screen_AlarmSet(u8 hh, u8 mm)
{
  UI_SetAlarm(1, hh, mm);
  g_alm.rung = 0;
  if (g_alm.ringing) { g_alm.ringing = 0; Voice_Send(VOICE_TX_RING, g_alm.ring_sel, 0, 0, 0); }
  UI_SetScreen(UI_SCREEN_ALARM);
}

void Screen_AlarmSwitch(u8 on)
{
  const ui_cfg_t *cfg = UI_GetConfig();
  UI_SetAlarm(on ? 1 : 0, cfg->alarm_hour, cfg->alarm_min);   /* 仅切开关，保留原时间 */
  if (!on && g_alm.ringing) Screen_AlarmRingStop();
  UI_SetScreen(UI_SCREEN_ALARM);
}

void Screen_AlarmLoop(u8 mode)
{
  g_alm.loop = (mode > 3) ? 0 : mode;
  UI_SetScreen(UI_SCREEN_ALARM);
}

void Screen_AlarmRingStop(void)
{
  if (g_alm.ringing)
  {
    g_alm.ringing = 0;
    Voice_Send(VOICE_TX_RING, g_alm.ring_sel, 0, 0, 0);   /* 停播铃声 */
  }
  UI_SetScreen(UI_SCREEN_MAIN);
}

/* 闹钟每秒判定（Screen_OnSecond 调） */
static void alarm_tick(TimeStruct *t)
{
  const ui_cfg_t *cfg = UI_GetConfig();
  /* 响铃中：60s 自动停回主屏 */
  if (g_alm.ringing)
  {
    if ((u32)(bftm0_ct - g_alm.ring_start) >= 60000u)
      Screen_AlarmRingStop();
    return;
  }
  if (!cfg->alarm_on) return;
  if (t->hour != cfg->alarm_hour || t->minute != cfg->alarm_min) { g_alm.rung = 0; return; }
  if (g_alm.rung) return;                  /* 本分钟已响 */
  if (!weekday_match()) return;

  /* 到点响铃：切屏2 + 播铃声 + 强调线红 */
  g_alm.ringing   = 1;
  g_alm.ring_start = (u32)bftm0_ct;
  g_alm.rung       = 1;
  UI_SetScreen(UI_SCREEN_ALARM);
  Voice_Send(VOICE_TX_RING, g_alm.ring_sel, 1, 0, 0);   /* 播放铃声 */
}

/* 闹钟屏每帧增量绘制 */
static void draw_alarm(TimeStruct *t)
{
  (void)t;
  const ui_cfg_t *cfg = UI_GetConfig();
  u8 hh = cfg->alarm_hour, mm = cfg->alarm_min;

  /* 大数字 HH:MM（闹钟时间） */
  u8 h1 = hh / 10, h2 = hh % 10, m1 = mm / 10, m2 = mm % 10;
  if (h1 != c_alm_h1) { draw_digit_at(SC_DIG_LEFT,             SC_ALM_Y, h1, SC_C_WHITE); c_alm_h1 = h1; }
  if (h2 != c_alm_h2) { draw_digit_at(SC_DIG_LEFT + SC_DIG_STEP, SC_ALM_Y, h2, SC_C_WHITE); c_alm_h2 = h2; }
  if (m1 != c_alm_m1) { draw_digit_at(SC_DIG_LEFT + 94,         SC_ALM_Y, m1, SC_C_WHITE); c_alm_m1 = m1; }
  if (m2 != c_alm_m2) { draw_digit_at(SC_DIG_LEFT + 94 + 40,    SC_ALM_Y, m2, SC_C_WHITE); c_alm_m2 = m2; }

  /* 开关chip：已开启绿 / 已关闭灰（填 22 高覆盖字模溢出行，防旧墨残留） */
  u8 on = cfg->alarm_on ? 1 : 0;
  if (on != c_alm_on)
  {
    if (on) { Display_FillRect(132, 15, 64, 22, SC_C_GREEN); UI_DrawTextInk(137, 15, "已开启", UI_SLOT_WEEK, SC_C_DARK); }
    else    { Display_FillRect(132, 15, 64, 22, SC_C_EDGE);  UI_DrawTextInk(137, 15, "已关闭", UI_SLOT_WEEK, SC_C_GRAY); }
    c_alm_on = on;
  }

  /* 星期chips：按循环模式选中（绿深字）/未选（灰灰字） */
  if (g_alm.loop != c_alm_loop)
  {
    static const char *DAY = "一二三四五六日";
    for (int i = 0; i < 7; i++)
    {
      int sel = 0;
      if (g_alm.loop == 0)      sel = 0;                       /* 单次：不标星期 */
      else if (g_alm.loop == 1) sel = (i <= 4);
      else if (g_alm.loop == 2) sel = (i <= 5);
      else                      sel = 1;
      int cx = 74 + i * 30;
      if (sel) { Display_FillRect(cx, 206, 24, 22, SC_C_GREEN); }
      else     { Display_FillRect(cx, 206, 24, 22, SC_C_EDGE); }
      char wb[4] = { DAY[i*3], DAY[i*3+1], DAY[i*3+2], 0 };    /* UTF-8 汉字 3 字节 */
      UI_DrawTextInk(cx + 3, 206, wb, UI_SLOT_CHAR, sel ? SC_C_DARK : SC_C_GRAY);
    }
    c_alm_loop = g_alm.loop;
  }

  /* 强调线：正常蓝 / 响铃红 */
  if (g_alm.ringing != c_alm_ring)
  {
    Display_FillRect(16, 170, 289, 2, g_alm.ringing ? SC_C_RED : SC_C_BLUE);
    c_alm_ring = g_alm.ringing;
  }
}

/* ==================== 屏静态元素（进入屏时画一次） ==================== */
static void draw_screen_static(int scr)
{
  if (scr == UI_SCREEN_CNT)
  {
    draw_colon_at(SC_COL_LEFT, SC_CNT_Y);
    /* 重置按钮文字（静态，画一次）；进度条轨道由背景自带 */
    UI_DrawTextInk(170, 181, "重置", UI_SLOT_WEEK, SC_C_WHITE);
    c_cnt_m1 = c_cnt_m2 = c_cnt_s1 = c_cnt_s2 = 0xFF;
    c_cnt_state = 0xFF; c_cnt_prog = -1;
  }
  else if (scr == UI_SCREEN_ALARM)
  {
    draw_colon_at(SC_COL_LEFT, SC_ALM_Y);
    c_alm_h1 = c_alm_h2 = c_alm_m1 = c_alm_m2 = 0xFF;
    c_alm_on = 0xFF; c_alm_loop = 0xFF; c_alm_ring = 0xFF;
  }
}

/* ==================== 对外 API ==================== */

void Screen_Init(void)
{
  const ui_cfg_t *cfg = UI_GetConfig();
  g_alm.loop     = 0;
  g_alm.ring_sel = 0;
  g_alm.ringing  = 0;
  g_alm.rung     = 0;
  g_alm.snooze   = 0;
  (void)cfg;
  c_drawn_screen = 0xFF;
  UI_SetScreen(UI_SCREEN_MAIN);
}

/* 每帧：按当前屏增量绘制 */
void Screen_Draw(TimeStruct *t)
{
  int scr = UI_GetScreen();
  if (scr != c_drawn_screen)
  {
    c_drawn_screen = (u8)scr;
    draw_screen_static(scr);
  }
  if (scr == UI_SCREEN_CNT)   draw_cnt(t);
  else if (scr == UI_SCREEN_ALARM) draw_alarm(t);
  /* 主屏（MAIN）由 calendar.c 负责，不在此绘制 */

  /* 倒计时 DONE 回主屏延时到点 */
  if (g_cnt == CNT_DONE && g_cnt_ret_ms && (u32)(bftm0_ct - g_cnt_ret_ms) < 0x80000000u)
  {
    g_cnt = CNT_STOP; g_cnt_ret_ms = 0;
    UI_SetScreen(UI_SCREEN_MAIN);
  }
}

/* 每秒：倒计时推进 + 闹钟判定 */
void Screen_OnSecond(TimeStruct *t)
{
  cnt_tick();
  alarm_tick(t);
}

/* 15s 无命令等超时回主屏（倒计时运行/暂停中不切走） */
void Screen_ReturnMain(void)
{
  if (Screen_CntActive()) return;
  UI_SetScreen(UI_SCREEN_MAIN);
}

/* ==================== 语音命令分发（voice.c 调；返回 1=已处理） ==================== */
int Screen_HandleVoice(const VoiceFrame *f)
{
  switch (f->cmd)
  {
    case VOICE_CMD_CNT_MIN:    /* 0x84 分钟倒计时 d0=1-60 */
      if (f->d0 >= 1 && f->d0 <= 60) { Screen_CntStart((u16)f->d0 * 60u); return 1; }
      return 0;
    case VOICE_CMD_TOMATO_ON:  /* 0x88 番茄：自动 25 分钟 */
      Screen_CntStart(25u * 60u); return 1;
    case VOICE_CMD_PAUSE:      /* 0x85 */
      Screen_CntPause(); return 1;
    case VOICE_CMD_RESUME:     /* 0x86 */
      Screen_CntResume(); return 1;
    case VOICE_CMD_STOP:       /* 0x87 */
    case VOICE_CMD_TOMATO_OFF: /* 0x89 */
      Screen_CntStop(); return 1;
    case VOICE_CMD_ALARM_SW:   /* 0x8B 开关：d0=闹钟1 状态（0/1；0xFF=默认不变仅查看） */
      if (f->d0 != 0xFF) Screen_AlarmSwitch(f->d0 == 1 ? 1 : 0);
      else UI_SetScreen(UI_SCREEN_ALARM);
      return 1;
    case VOICE_CMD_ALARM_LOOP: /* 0x8C 循环模式：d0=闹钟1 模式 */
      Screen_AlarmLoop(f->d0); return 1;
    case VOICE_CMD_ALARM_RING: /* 0x8D 铃音选择 */
      g_alm.ring_sel = f->d0; return 1;
    case VOICE_CMD_SNOOZE:     /* 0xB0 贪睡：停响 + 闹钟 +10min */
    {
      const ui_cfg_t *cfg = UI_GetConfig();
      u8 nh = cfg->alarm_hour, nm = cfg->alarm_min;
      if (g_alm.ringing) { g_alm.ringing = 0; Voice_Send(VOICE_TX_RING, g_alm.ring_sel, 0, 0, 0); }
      nm += 10;
      if (nm >= 60) { nm -= 60; nh = (nh + 1) % 24; }
      UI_SetAlarm(1, nh, nm);
      UI_SetScreen(UI_SCREEN_MAIN);
      return 1;
    }
    case VOICE_CMD_ALARM_OFF:  /* 0xB1 别响了 */
      Screen_AlarmRingStop(); return 1;
    default:
      return 0;
  }
}

#include "ui_skin.h"
#include "Display.h"
#include "ui.h"
#include <stdio.h>
#include <string.h>

#define SKIN_OFFSET 0x200u
#define STATUS_ICONS_OFFSET 0x500u
#define STATUS_ICON_COUNT 3u
#define FONT_SLOTS 5u
#define SCREEN_WIDTH 320u
#define SCREEN_HEIGHT 240u

typedef struct {
  SkinRect box;
  u16 font, color, centered;
} TextSlot;

typedef struct {
  u16 x[5], y, base, width, height, colon, colon_width;
} DigitRow;

typedef struct {
  SkinRect box;
  u16 off, on;
} Toggle;

typedef struct {
  DigitRow clock;
  u16 seconds_x[2], seconds_y, seconds_base, seconds_w, seconds_h;
  TextSlot date, weekday, lunar, temp, humidity, temp_name, humi_name, assistant;
  SkinRect assistant_rule;
  u16 date_separator, lunar_prefix, temperature_unit;
  Toggle hourly;
} ClockPage;

typedef struct {
  DigitRow clock;
  TextSlot number;
  SkinRect number_box;
  u16 number_base;
  Toggle enabled;
  Toggle weekdays[7];
  SkinRect switch_cover_box;
  u16 switch_cover;
} AlarmPage;

typedef struct {
  SkinRect box;
  u16 color, elliptical;
} Segment;

typedef struct {
  u16 digit_base, colon;
  u16 button[3];
  u16 progress_kind;               /* 0=continuous bar (CNTL), 1=segments, 2=hidden */
  u16 segment_count;
  Segment segment[SKIN_MAX_SEGMENTS]; /* top-to-bottom or end-to-start */
} CountdownPage;

typedef struct {
  u16 first, count;
} Frames;

/* SKIN v1 payload. All numbers are little-endian u16 and strings are UTF-8.
   Wire layout is fixed by ui_layout.py. All three screens activate together. */
typedef struct {
  ClockPage main;
  AlarmPage alarm;
  CountdownPage countdown;
  SkinBubble bubble[3];
  Frames robot[3][4];
} SkinData;

_Static_assert(sizeof(SkinData) == 728u, "SKIN v1 payload size");

/* SICO v1 holds the timer, hourly and alarm icon rectangles and sprite IDs. */
typedef struct {
  SkinRect box;
  u16 sprite;
} StatusIcon;

_Static_assert(sizeof(StatusIcon) * STATUS_ICON_COUNT == 30u, "SICO v1 payload size");

static SkinData skin;
static StatusIcon status_icons[STATUS_ICON_COUNT];
static u16 status_icons_bg;
static u8 status_icons_visible, status_icons_ready;
static u8 active;

/* ============ 内容缓存：字段内容没变就不清屏重画（2026-11-23） ============
   缘起（用户实测「每秒刷新时全屏各种参量闪一下」）：Px_DrawMain 每秒跑一次，
   而本文件的 draw_text / draw_date 一律「先 Skin_Clear(box) 从 Flash 还原背景、
   再重画文字」。于是每秒都把 日期 / 星期 / 农历 / 温度标签+数值 / 湿度标签+数值 /
   AI 标题 这 8 个字段逐个擦白再画回来（仿真实测：t+24ms 起 35ms 的滚动擦除，
   肉眼就是整屏闪一下，每秒一次）。
   现在每个字段记住「上次画的内容 hash + 背景版本」，内容没变直接返回；
   数字精灵也按位缓存 —— 每秒只有真正变化的「秒个位」动一下（约 190 像素）。
   需要「内容没变也强制重画」的场合（闪灭亮相 / 背景重传 / 闹钟屏重绘）调
   Skin_Invalidate()，只重画某一项调 Skin_InvalidateField()。 */
enum {
  SLOT_WEEKDAY, SLOT_LUNAR, SLOT_TEMP, SLOT_HUMI,
  SLOT_TEMP_NAME, SLOT_HUMI_NAME, SLOT_ASSISTANT, SLOT_ALARM_NUMBER, SLOT_COUNT
};
typedef struct { u32 hash; u16 bg; } SlotCache;
static SlotCache slot_cache[SLOT_COUNT];

/* 日期「年 / 月 / 日」三段各自缓存（2026-11-25 用户「设置中闪烁，每次闪烁都会带动
   其他未被设置的数字抖一下」）：以前只把整个日期框当一个字段，闪「年」时整框
   （年月日 10 个字符）都会被清掉重画，月/日跟着闪一下。现在按三段分开记，
   只有真的变的那一段才清+画；分隔符只在整框首画/换背景时画一次。 */
static u32 c_date_p[3] = { 0u, 0u, 0u };
static u16 c_date_bg = 0xFFFFu;

/* 时钟四位 + 冒号（主屏/闹钟屏各一套，靠 base 区分） */
static u16 c_ck_base = 0xFFFFu, c_ck_bg = 0xFFFFu, c_ck_colon_bg = 0xFFFFu;
static u8  c_ck_h1 = 0xFFu, c_ck_h2 = 0xFFu, c_ck_m1 = 0xFFu, c_ck_m2 = 0xFFu;
/* 主屏秒两位 */
static u16 c_sc_bg = 0xFFFFu;
static u8  c_sc_1 = 0xFFu, c_sc_2 = 0xFFu;
/* 闹钟槽号精灵 */
static u16 c_alm_slot_bg = 0xFFFFu;
static u8  c_alm_slot = 0xFFu;

static u32 text_hash(const char *s)
{
  u32 h = 2166136261u;                 /* FNV-1a 32 */
  while (*s) { h ^= (u8)*s++; h *= 16777619u; }
  return h ^ 0xA5A5A5A5u;              /* 空串也不与静态零初始化相撞 */
}

/* 定长 hash（日期分段用：年 4 字符 / 月 2 / 日 2） */
static u32 text_hash_len(const char *s, unsigned n)
{
  u32 h = 2166136261u;
  for (unsigned i = 0; i < n; ++i) { h ^= (u8)s[i]; h *= 16777619u; }
  return h ^ 0xA5A5A5A5u;
}

/* 返回 1 = 内容与上次一致（调用方直接跳过清屏+重画）；否则登记新内容并返回 0 */
static int slot_unchanged(unsigned slot, u32 hash)
{
  SlotCache *c = &slot_cache[slot];
  if (c->bg == g_ui_bg_ver && c->hash == hash) return 1;
  c->bg = g_ui_bg_ver;
  c->hash = hash;
  return 0;
}

/* 作废全部内容缓存：下一次绘制会把该屏动态层整层重画 */
void Skin_Invalidate(void)
{
  /* 这里**不要**动 status_icons_visible：背景重传已经由 status_icons_bg 判出来了，
     清零会让每次 Skin_Invalidate（闪灭还原 / 闹钟屏重绘）都把 3 个按钮图标擦一遍重画一遍
     —— 就是「切设置项时别的元素跟着抖」那类毛病的翻版。 */
  for (unsigned i = 0; i < SLOT_COUNT; ++i) slot_cache[i].bg = 0xFFFFu;
  c_ck_bg = c_ck_colon_bg = c_ck_base = 0xFFFFu;
  c_ck_h1 = c_ck_h2 = c_ck_m1 = c_ck_m2 = 0xFFu;
  c_sc_bg = 0xFFFFu; c_sc_1 = c_sc_2 = 0xFFu;
  c_alm_slot_bg = 0xFFFFu; c_alm_slot = 0xFFu;
  c_date_bg = 0xFFFFu; c_date_p[0] = c_date_p[1] = c_date_p[2] = 0u;
}

/* 只作废某一项：闪灭的「亮相」只重画被闪的那一项，不整屏再擦一遍 */
void Skin_InvalidateField(SkinClockField field)
{
  switch (field)
  {
    case SKIN_CLOCK_HOUR:   c_ck_h1 = c_ck_h2 = 0xFFu; break;
    case SKIN_CLOCK_MINUTE: c_ck_m1 = c_ck_m2 = 0xFFu; break;
    case SKIN_CLOCK_SECOND: c_sc_1 = c_sc_2 = 0xFFu; break;
    /* 日期按段作废：闪「年」只重画年那 4 个字符，月/日不动（2026-11-25） */
    case SKIN_CLOCK_YEAR:   c_date_p[0] = 0u; break;
    case SKIN_CLOCK_MONTH:  c_date_p[1] = 0u; break;
    case SKIN_CLOCK_DAY:    c_date_p[2] = 0u; break;
    default: break;
  }
}

static int fits(SkinRect r)
{
  return (r.w == 0u && r.h == 0u) ||
         (r.w && r.h && r.x + r.w <= SCREEN_WIDTH && r.y + r.h <= SCREEN_HEIGHT);
}

static int text_fits(const TextSlot *text)
{
  return fits(text->box) && text->font < FONT_SLOTS && text->centered <= 1u;
}

static int sprites_fit(u16 first, unsigned count)
{
  return first < UI_SpriteCount() && count <= UI_SpriteCount() - first;
}

static int toggle_fits(const Toggle *toggle)
{
  return fits(toggle->box) &&
         (toggle->off == SKIN_NO_SPRITE || (toggle->box.w && sprites_fit(toggle->off, 1u))) &&
         (toggle->on == SKIN_NO_SPRITE || (toggle->box.w && sprites_fit(toggle->on, 1u)));
}

static int row_fits(const DigitRow *row)
{
  if (!row->width || !row->height || !row->colon_width ||
      !sprites_fit(row->base, 10u) || !sprites_fit(row->colon, 1u)) return 0;
  for (unsigned i = 0; i < 5u; ++i)
  {
    u16 width = (i == 2u) ? row->colon_width : row->width;
    if (!fits((SkinRect){row->x[i], row->y, width, row->height}) ||
        (i && row->x[i] <= row->x[i-1u])) return 0;
  }
  return 1;
}

static int valid_skin(void)
{
  const ClockPage *clock = &skin.main;
  const AlarmPage *alarm = &skin.alarm;
  const CountdownPage *countdown = &skin.countdown;
  if (!row_fits(&clock->clock) || !row_fits(&alarm->clock) ||
      !fits(clock->assistant_rule) || !toggle_fits(&clock->hourly) ||
      !text_fits(&clock->date) || !text_fits(&clock->weekday) || !text_fits(&clock->lunar) ||
      !text_fits(&clock->temp) || !text_fits(&clock->humidity) || !text_fits(&clock->assistant) ||
      !text_fits(&clock->temp_name) || !text_fits(&clock->humi_name) || clock->lunar_prefix > 1u || clock->temperature_unit > 2u ||
      (clock->date_separator != '-' && clock->date_separator != '.' && clock->date_separator != '/'))
    return 0;
  if (clock->seconds_base != SKIN_NO_SPRITE &&
      (!clock->seconds_w || !clock->seconds_h || clock->seconds_x[1] <= clock->seconds_x[0] ||
       !sprites_fit(clock->seconds_base, 10u) ||
       !fits((SkinRect){clock->seconds_x[0], clock->seconds_y, clock->seconds_w, clock->seconds_h}) ||
       !fits((SkinRect){clock->seconds_x[1], clock->seconds_y, clock->seconds_w, clock->seconds_h})))
    return 0;
  if (!text_fits(&alarm->number) || !fits(alarm->number_box) || !toggle_fits(&alarm->enabled) ||
      (alarm->number_base != SKIN_NO_SPRITE && (!alarm->number_box.w || !sprites_fit(alarm->number_base, 3u))) ||
      !fits(alarm->switch_cover_box) ||
      (alarm->switch_cover != SKIN_NO_SPRITE && (!alarm->switch_cover_box.w || !sprites_fit(alarm->switch_cover, 1u))))
    return 0;
  for (unsigned i = 0; i < 7u; ++i) if (!toggle_fits(&alarm->weekdays[i])) return 0;
  if (!sprites_fit(countdown->digit_base, 10u) || !sprites_fit(countdown->colon, 1u) ||
      countdown->progress_kind > 2u || countdown->segment_count > SKIN_MAX_SEGMENTS ||
      (countdown->progress_kind == 1u && countdown->segment_count == 0u)) return 0;
  u16 seen = 0u;
  for (unsigned i = 0; i < 3u; ++i)
  {
    u16 action = countdown->button[i];
    if (action > SKIN_COUNT_PAUSE || (seen & (1u << action))) return 0;
    seen |= (1u << action);
  }
  for (unsigned i = 0; i < countdown->segment_count; ++i)
  {
    const Segment *seg = &countdown->segment[i];
    if (!fits(seg->box) || !seg->box.w || seg->elliptical > 1u ||
        (seg->elliptical && (seg->box.w > 64u || seg->box.h > 64u))) return 0;
  }
  for (unsigned page = 0; page < 3u; ++page)
  {
    const SkinBubble *entry = &skin.bubble[page];
    const CountdownBubbleLayout *b = &entry->layout;
    if (entry->text[sizeof(entry->text)-1u] || b->lines > 4u ||
        b->font >= FONT_SLOTS || b->small_font >= FONT_SLOTS || b->centered > 1u ||
        (b->lines && (!b->width || !b->line_height || b->line_height > SCREEN_HEIGHT ||
                      b->x + b->width > SCREEN_WIDTH ||
                      b->y + b->lines * (u32)b->line_height > SCREEN_HEIGHT))) return 0;
    for (unsigned i = 0; i < 4u; ++i)
    {
      const Frames *f = &skin.robot[page][i];
      if (f->count > 16u || (f->count && !sprites_fit(f->first, f->count))) return 0;
    }
  }
  return 1;
}

static int status_icons_fit(void)
{
  for (unsigned i = 0; i < STATUS_ICON_COUNT; ++i)
  {
    const StatusIcon *icon = &status_icons[i];
    SkinRect r = icon->box;
    if (!r.w || !r.h || !fits(r) || !sprites_fit(icon->sprite, 1u)) return 0;
    for (unsigned j = 0; j < i; ++j)
    {
      SkinRect p = status_icons[j].box;
      if (r.x < p.x + p.w && p.x < r.x + r.w && r.y < p.y + p.h && p.y < r.y + r.h) return 0;
    }
  }
  return 1;
}

int Skin_Load(void)
{
  active = status_icons_ready = 0;
  int result = UI_ReadLayout(SKIN_OFFSET, "SKIN", &skin, sizeof(skin));
  if (result != 0) return result;
  if (!valid_skin()) return -1;
  result = UI_ReadLayout(STATUS_ICONS_OFFSET, "SICO", status_icons, sizeof(status_icons));
  status_icons_ready = result == 0 && status_icons_fit();
  if (!status_icons_ready) printf("[UI] SICO invalid\r\n");   /* 旧镜像没有 SICO：退回固件角标 */
  return 0;
}

void Skin_Activate(int ready)
{
  active = !!ready;
  Skin_Invalidate();          /* 刚激活：内容缓存全部作废，第一次绘制要整层画全 */
  if (active) printf("[UI] SKIN flash (3 pages)\r\n");
  if (active && status_icons_ready) printf("[UI] SICO flash (3 icons)\r\n");
}

int Skin_Active(void) { return active; }

const SkinBubble *Skin_GetBubble(unsigned page)
{
  return active && page < 3u ? &skin.bubble[page] : NULL;
}

void Skin_Clear(SkinRect r)
{
  if (r.w && r.h) UI_DrawGlyphRect(r.x, SCREEN_HEIGHT - r.y - r.h, r.w, r.h, 0xFFFFu, 0);
}

static u32 font_base(const TextSlot *slot) { return (u32)slot->font << 16; }

static void draw_text(unsigned slot, const TextSlot *text, const char *text_str)
{
  if (!text->box.w) return;
  if (slot_unchanged(slot, text_hash(text_str))) return;
  Skin_Clear(text->box);
  int x = text->box.x;
  if (text->centered)
  {
    int width = UI_StrPxWidth(text_str, font_base(text));
    if (width < text->box.w) x += (text->box.w - width) / 2;
  }
  UI_DrawStrPxBounded(x, text->box.y, text_str, font_base(text), text->color, text->box.x + text->box.w);
}

static void draw_toggle(const Toggle *toggle, int on)
{
  u16 id = on ? toggle->on : toggle->off;
  if (id == SKIN_NO_SPRITE) Skin_Clear(toggle->box);
  else UI_DrawSprite(id, toggle->box.x, toggle->box.y);
}

/* 时钟：四位逐位缓存（只有变化的那一位才重贴精灵）+ 冒号每背景版本贴一次。
   show_hour/show_minute=0 是闪灭的灭相：不画、并把该两位缓存作废，亮相时补画。 */
static void draw_clock(const DigitRow *row, unsigned hour, unsigned minute,
                       int show_hour, int show_minute)
{
  if (c_ck_base != row->base || c_ck_bg != g_ui_bg_ver)
  {
    c_ck_base = row->base; c_ck_bg = g_ui_bg_ver;
    c_ck_h1 = c_ck_h2 = c_ck_m1 = c_ck_m2 = 0xFFu;
    c_ck_colon_bg = 0xFFFFu;
  }
  if (show_hour)
  {
    u8 h1 = (u8)(hour / 10u), h2 = (u8)(hour % 10u);
    if (c_ck_h1 != h1) { c_ck_h1 = h1; UI_DrawSprite(row->base + h1, row->x[0], row->y); }
    if (c_ck_h2 != h2) { c_ck_h2 = h2; UI_DrawSprite(row->base + h2, row->x[1], row->y); }
  }
  else c_ck_h1 = c_ck_h2 = 0xFFu;
  if (c_ck_colon_bg != g_ui_bg_ver)
  {
    c_ck_colon_bg = g_ui_bg_ver;
    UI_DrawSprite(row->colon, row->x[2], row->y);
  }
  if (show_minute)
  {
    u8 m1 = (u8)(minute / 10u), m2 = (u8)(minute % 10u);
    if (c_ck_m1 != m1) { c_ck_m1 = m1; UI_DrawSprite(row->base + m1, row->x[3], row->y); }
    if (c_ck_m2 != m2) { c_ck_m2 = m2; UI_DrawSprite(row->base + m2, row->x[4], row->y); }
  }
  else c_ck_m1 = c_ck_m2 = 0xFFu;
}

static void make_date(const TimeStruct *t, char result[11])
{
  unsigned year = t->year;
  result[0] = '0' + (year / 1000u) % 10u;
  result[1] = '0' + (year / 100u) % 10u;
  result[2] = '0' + (year / 10u) % 10u;
  result[3] = '0' + year % 10u;
  result[4] = (char)skin.main.date_separator;
  result[5] = '0' + t->month / 10u;
  result[6] = '0' + t->month % 10u;
  result[7] = result[4];
  result[8] = '0' + t->day / 10u;
  result[9] = '0' + t->day % 10u;
  result[10] = 0;
}

static int date_origin(const TextSlot *slot, const char *text)
{
  int x = slot->box.x;
  if (slot->centered)
  {
    int width = UI_StrPxWidth(text, font_base(slot));
    if (width < slot->box.w) x += (slot->box.w - width) / 2;
  }
  return x;
}

/* 日期：年 / 月 / 日 三段分别缓存，只有变的那一段才「清 + 画」。
   2026-11-25 用户「设置中闪烁，每次闪烁都会带动其他未被设置的数字抖一下」：
   整框当一格时，闪「年」会把整框清掉再重画一遍（5ms 采样实测：t+165ms 有
   321 像素跨 x74..137 变化 = 年月日全被擦了一遍），月/日就跟着闪。
   分隔符（'-'）不参与变化，只在整框首画 / 换背景时画一次。 */
static void draw_date(const TimeStruct *t, int hidden)
{
  const TextSlot *slot = &skin.main.date;
  if (!slot->box.w) return;
  char text[11];
  make_date(t, text);
  const u32 base = font_base(slot);
  const int limit = slot->box.x + slot->box.w;
  int x = date_origin(slot, text);

  /* 三段的起点/宽度（与 Skin_ClockItemRect 同一套算法，保证擦的框和画的框一致） */
  static const u8 part_start[3] = { 0u, 5u, 8u };
  static const u8 part_len[3]   = { 4u, 2u, 2u };
  int px[3], pw[3];
  int cx = x;
  unsigned p = 0;
  for (unsigned i = 0; i < 10u; ++i)
  {
    char digit[2] = { text[i], 0 };
    int w = UI_StrPxWidth(digit, base);
    if (p < 3u && i == part_start[p])
    {
      int pwv = 0;
      for (unsigned k = 0; k < part_len[p]; ++k)
      {
        char dd[2] = { text[i + k], 0 };
        pwv += UI_StrPxWidth(dd, base);
      }
      px[p] = cx; pw[p] = pwv; p++;
    }
    cx += w;
  }

  /* 背景重传 / 首帧：整框重画一遍（含两个分隔符），并把三段 hash 记下来 */
  if (c_date_bg != g_ui_bg_ver)
  {
    c_date_bg = g_ui_bg_ver;
    Skin_Clear(slot->box);
    int dx = x;
    for (unsigned i = 0; i < 10u; ++i)
    {
      char digit[2] = { text[i], 0 };
      int part = i < 4u ? 0 : (i < 7u ? 1 : 2);
      if (hidden != (int)(SKIN_CLOCK_YEAR + part))
        UI_DrawStrPxBounded(dx, slot->box.y, digit, base, slot->color, limit);
      dx += UI_StrPxWidth(digit, base);
    }
    for (p = 0; p < 3u; ++p)
      c_date_p[p] = text_hash_len(text + part_start[p], part_len[p]);
    return;
  }

  /* 之后逐段：内容没变的段不动 */
  for (p = 0; p < 3u; ++p)
  {
    u32 h = text_hash_len(text + part_start[p], part_len[p]);
    if (hidden == (int)(SKIN_CLOCK_YEAR + p)) { c_date_p[p] = 0u; continue; }  /* 灭相：这段不画 */
    if (c_date_p[p] == h) continue;
    c_date_p[p] = h;
    Skin_Clear((SkinRect){ (u16)px[p], slot->box.y, (u16)pw[p], slot->box.h });
    int dx = px[p];
    for (unsigned k = 0; k < part_len[p]; ++k)
    {
      char digit[2] = { text[part_start[p] + k], 0 };
      UI_DrawStrPxBounded(dx, slot->box.y, digit, base, slot->color, limit);
      dx += UI_StrPxWidth(digit, base);
    }
  }
}

void Skin_Clock(const TimeStruct *t, int weekday, const char *lunar, const char *temp,
                const char *humi, int hidden)
{
  static const char *const names[7] = {"星期一","星期二","星期三","星期四","星期五","星期六","星期日"};
  const ClockPage *p = &skin.main;
  draw_clock(&p->clock, t->hour, t->minute, hidden != SKIN_CLOCK_HOUR, hidden != SKIN_CLOCK_MINUTE);
  if (Skin_ClockHasSeconds())
  {
    if (c_sc_bg != g_ui_bg_ver) { c_sc_bg = g_ui_bg_ver; c_sc_1 = c_sc_2 = 0xFFu; }
    if (hidden != SKIN_CLOCK_SECOND)
    {
      u8 sec = (t->second > 59u) ? 0u : (u8)t->second;
      u8 s1 = (u8)(sec / 10u), s2 = (u8)(sec % 10u);
      if (c_sc_1 != s1) { c_sc_1 = s1; UI_DrawSprite(p->seconds_base + s1, p->seconds_x[0], p->seconds_y); }
      if (c_sc_2 != s2) { c_sc_2 = s2; UI_DrawSprite(p->seconds_base + s2, p->seconds_x[1], p->seconds_y); }
    }
    else c_sc_1 = c_sc_2 = 0xFFu;
  }
  draw_date(t, hidden);
  if (weekday >= 0 && weekday < 7) draw_text(SLOT_WEEKDAY, &p->weekday, names[weekday]);
  char text[32];
  if (lunar[0])
  {
    unsigned prefix = 0;
    if (p->lunar_prefix) { memcpy(text, "农历", 6u); prefix = 6u; }
    unsigned n = 0;
    while (prefix + n + 1u < sizeof(text) && lunar[n]) { text[prefix+n] = lunar[n]; ++n; }
    text[prefix+n] = 0;
    draw_text(SLOT_LUNAR, &p->lunar, text);
  }
  if (temp && humi)
  {
    draw_text(SLOT_TEMP_NAME, &p->temp_name, "温度");
    draw_text(SLOT_HUMI_NAME, &p->humi_name, "湿度");
    unsigned i = 0;
    while (i < 6u && temp[i] && temp[i] != '.') { text[i] = temp[i]; ++i; }
    static const char units[3][4] = {"\xC2\xB0" "C", "℃", ""};
    memcpy(text + i, units[p->temperature_unit], 4u);
    draw_text(SLOT_TEMP, &p->temp, text);
    i = 0;
    while (i < 6u && humi[i] && humi[i] != '.') { text[i] = humi[i]; ++i; }
    text[i++] = '%'; text[i] = 0;
    draw_text(SLOT_HUMI, &p->humidity, text);
  }
  draw_text(SLOT_ASSISTANT, &p->assistant, "AI语音助手");
  if (p->assistant_rule.w)
    Display_FillRect(p->assistant_rule.x, p->assistant_rule.y, p->assistant_rule.w,
                     p->assistant_rule.h, p->assistant.color);
}

static SkinRect digit_rect(const DigitRow *row, int minute)
{
  unsigned i = minute ? 3u : 0u;
  u16 right = row->x[i+1u] + row->width;
  if (!minute && right > row->x[2]) right = row->x[2];
  return (SkinRect){row->x[i], row->y, right - row->x[i], row->height};
}

int Skin_ClockHasSeconds(void) { return skin.main.seconds_base != SKIN_NO_SPRITE; }

int Skin_ClockItemRect(const TimeStruct *t, SkinClockField item, SkinRect *rect)
{
  const ClockPage *p = &skin.main;
  if (item <= SKIN_CLOCK_MINUTE)
    *rect = digit_rect(&p->clock, item == SKIN_CLOCK_MINUTE);
  else if (item == SKIN_CLOCK_SECOND && Skin_ClockHasSeconds())
    *rect = (SkinRect){p->seconds_x[0],p->seconds_y,
                       p->seconds_x[1]+p->seconds_w-p->seconds_x[0],p->seconds_h};
  else if (item >= SKIN_CLOCK_YEAR && item <= SKIN_CLOCK_DAY && p->date.box.w)
  {
    static const u8 start[3] = {0,5,8}, end[3] = {4,7,10};
    unsigned part = item - SKIN_CLOCK_YEAR;
    char text[11];
    make_date(t, text);
    int x = date_origin(&p->date, text);
    text[end[part]] = 0;
    int width = UI_StrPxWidth(text + start[part], font_base(&p->date));
    text[start[part]] = 0;
    x += UI_StrPxWidth(text, font_base(&p->date));
    const int limit = p->date.box.x + p->date.box.w;
    if (x + width > limit) width = limit - x;
    if (width <= 0) return 0;
    *rect = (SkinRect){x, p->date.box.y, width, p->date.box.h};
  }
  else return 0;
  return rect->w != 0u;
}

int Skin_DrawStatusIcons(unsigned visible)
{
  if (!active || !status_icons_ready) return 0;
  unsigned changed = status_icons_bg == g_ui_bg_ver ? visible ^ status_icons_visible : SKIN_STATUS_ALL;
  for (unsigned i = 0; i < STATUS_ICON_COUNT; ++i)
  {
    unsigned bit = 1u << i;
    if (!(changed & bit)) continue;
    const StatusIcon *icon = &status_icons[i];
    if (visible & bit) UI_DrawSprite(icon->sprite, icon->box.x, icon->box.y);
    else Skin_Clear(icon->box);
  }
  status_icons_bg = g_ui_bg_ver;
  status_icons_visible = (u8)visible;
  return 1;
}

void Skin_Hourly(int enabled)
{
  static u16 version = 0xFFFFu;
  static int previous = -1;
  if (version == g_ui_bg_ver && previous == enabled) return;
  version = g_ui_bg_ver;
  previous = enabled;
  draw_toggle(&skin.main.hourly, enabled);
}

void Skin_Alarm(const SkinAlarmState *s, int hidden)
{
  const AlarmPage *p = &skin.alarm;
  draw_clock(&p->clock, s->hour, s->minute, hidden != SKIN_ALARM_HOUR, hidden != SKIN_ALARM_MINUTE);
  if (hidden != SKIN_ALARM_SLOT)
  {
    if (p->number_base != SKIN_NO_SPRITE)
    {
      if (c_alm_slot_bg != g_ui_bg_ver) { c_alm_slot_bg = g_ui_bg_ver; c_alm_slot = 0xFFu; }
      if (c_alm_slot != s->slot)
      {
        c_alm_slot = s->slot;
        UI_DrawSprite(p->number_base + s->slot, p->number_box.x, p->number_box.y);
      }
    }
    else
    {
      char text[2] = {'1' + s->slot, 0};
      draw_text(SLOT_ALARM_NUMBER, &p->number, text);
    }
  }
  else slot_cache[SLOT_ALARM_NUMBER].bg = 0xFFFFu;   /* 灭相：缓存作废，亮相补画 */
  if (hidden != SKIN_ALARM_SWITCH || p->switch_cover != SKIN_NO_SPRITE)
    draw_toggle(&p->enabled, s->enabled);
  for (unsigned i = 0; i < 7u; ++i)
  {
    if (hidden == SKIN_ALARM_DAYS && i + 1u == s->days) continue;
    draw_toggle(&p->weekdays[i], (s->weekdays >> i) & 1u);
  }
}

int Skin_AlarmItemRect(const SkinAlarmState *s, SkinAlarmField item, SkinRect *rect)
{
  const AlarmPage *p = &skin.alarm;
  switch (item)
  {
    case SKIN_ALARM_SLOT: *rect = p->number_base == SKIN_NO_SPRITE ? p->number.box : p->number_box; break;
    case SKIN_ALARM_SWITCH: *rect = p->switch_cover == SKIN_NO_SPRITE ? p->enabled.box : p->switch_cover_box; break;
    case SKIN_ALARM_HOUR: case SKIN_ALARM_MINUTE: *rect = digit_rect(&p->clock, item == SKIN_ALARM_MINUTE); break;
    case SKIN_ALARM_DAYS: *rect = p->weekdays[s->days - 1u].box; break;
    default: return 0;
  }
  return rect->w != 0u;
}

int Skin_AlarmSwitchCover(void)
{
  if (skin.alarm.switch_cover == SKIN_NO_SPRITE) return 0;
  UI_DrawSprite(skin.alarm.switch_cover, skin.alarm.switch_cover_box.x, skin.alarm.switch_cover_box.y);
  return 1;
}

void Skin_Robot(unsigned page, unsigned state, unsigned frame)
{
  if (page >= 3u) return;
  const Frames *group = &skin.robot[page][state < 4u ? state : 0u];
  if (!group->count) group = &skin.robot[page][0];
  if (group->count) UI_DrawSpriteAuto(group->first + frame % group->count);
}

u16 Skin_CountdownDigit(unsigned value)
{
  return active ? skin.countdown.digit_base + value : UI_SPR_DIGIT(2u, value);
}

u16 Skin_CountdownColon(void)
{
  return active ? skin.countdown.colon : UI_SPR_COLON(2u);
}

SkinCountAction Skin_CountdownAction(unsigned button)
{
  return active ? (SkinCountAction)skin.countdown.button[button] : (SkinCountAction)button;
}

int Skin_CountdownHasExit(void)
{
  for (unsigned i = 0; i < 3u; ++i)
    if (Skin_CountdownAction(i) == SKIN_COUNT_EXIT) return 1;
  return 0;
}

int Skin_CountdownUsesSegments(void)
{
  return active && skin.countdown.progress_kind != 0u;
}

void Skin_CountdownProgress(const CountdownViewState *s)
{
  static u16 painted[SKIN_MAX_SEGMENTS];
  static u16 background = 0xFFFFu;
  if (skin.countdown.progress_kind != 1u) return;
  unsigned count = skin.countdown.segment_count;
  u32 ratio = 1024u;
  if (s->total)
  {
    u32 left = s->seconds < s->total ? s->seconds : s->total;
    if (s->counting_up) left = s->total - left;
    ratio = left * 1024u / s->total;
  }
  for (unsigned i = 0; i < count; ++i)
  {
    const Segment *seg = &skin.countdown.segment[i];
    SkinRect box = seg->box;
    int fill = (int)(ratio * count) - (int)((count-1u-i) * 1024u);
    if (fill < 0) fill = 0;
    if (fill > 1024) fill = 1024;
    unsigned h = (fill * box.h + 1023u) / 1024u;
    if (background == g_ui_bg_ver && painted[i] == h) continue;
    painted[i] = (u16)h;
    Skin_Clear(box);
    if (!h) continue;
    if (!seg->elliptical)
    {
      Display_FillRect(box.x, box.y + box.h - h, box.w, h, seg->color);
      continue;
    }
    /* Pixel-centre ellipse test. Fill each circle from the bottom up. */
    int a2 = box.w * box.w, b2 = box.h * box.h;
    for (int y = box.h - h; y < box.h; ++y)
    {
      int dy = 2*y + 1 - box.h;
      int x = 0;
      while (x < box.w / 2)
      {
        int dx = 2*x + 1 - box.w;
        if ((u32)(dx*dx)*b2 + (u32)(dy*dy)*a2 <= (u32)a2*b2) break;
        ++x;
      }
      if (2*x < box.w) Display_FillRect(box.x+x, box.y+y, box.w-2*x, 1, seg->color);
    }
  }
  background = g_ui_bg_ver;
}

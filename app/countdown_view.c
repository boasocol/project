#include "countdown_view.h"
#include "Display.h"
#include "ui.h"
#include <stdio.h>

#define COUNTDOWN_LAYOUT_OFFSET 0x100u
#include "ui_skin.h"

typedef struct {
  u16 x, y, w, h;
} Rect;

typedef struct {
  u16 center, y, ink_y, ink_height, font, color;
} CenteredLabel;

/* CNTL v1 payload. All fields are little-endian u16, in declaration order.
   Keep the packer (countdown_layout.py) and this structure in sync. */
typedef struct {
  u16 y, width, height;
  u16 short_x[5];                /* minute digits, colon, second digits */
  u16 long_x[6];                 /* same order, with a minute hundreds digit */
  Rect digit_region;
  Rect progress;
  u16 progress_color;
  CenteredLabel title;
  Rect title_rules[2];
  u16 button_centers[3];
  CenteredLabel button;          /* center supplied by button_centers */
  CountdownBubbleLayout bubble;
} CountdownLayout;

_Static_assert(sizeof(CountdownLayout) == 110u, "CNTL v1 payload size");

static const CountdownLayout default_layout = {
  .y = 55, .width = 34, .height = 46,
  .short_x = {73, 102, 141, 156, 193},
  .long_x = {64, 94, 124, 154, 168, 198},
  .digit_region = {56, 50, 184, 52},
  .progress = {249, 58, 9, 64},
  .progress_color = UI_RGB(120, 138, 62),
  .title = {148, 100, 0, 13, 0, UI_RGB(72, 34, 10)},
  .title_rules = {{97, 108, 21, 2}, {181, 108, 21, 2}},
  .button_centers = {89, 162, 232},
  .button = {0, 143, 3, 13, 3, UI_RGB(250, 250, 249)},
  .bubble = {99, 192, 146, 0, 0, 15, 2, UI_RGB(250, 246, 238), 1}
};

static CountdownLayout flash_layout;
static const CountdownLayout *layout = &default_layout;

static u32 font_base(u16 slot)
{
  return (u32)slot << 16;
}

static int rect_fits(Rect r)
{
  return r.w != 0u && r.h != 0u && r.x + r.w <= 320u && r.y + r.h <= 240u;
}

static int label_fits(const CenteredLabel *label, u16 center, const char *text)
{
  int width;
  if (center >= 320u || label->font > 4u || label->ink_height == 0u ||
      label->y + label->ink_y + label->ink_height > 240u)
    return 0;
  width = UI_StrPxWidth(text, font_base(label->font));
  return width > 0 && center >= width / 2 && center - width / 2 + width <= 320;
}

static int digits_fit(const CountdownLayout *skin, const u16 *x, unsigned count)
{
  const Rect *region = &skin->digit_region;
  if (skin->y < region->y || skin->y + skin->height > region->y + region->h)
    return 0;
  for (unsigned i = 0; i < count; ++i)
  {
    if (x[i] < region->x || x[i] + skin->width > region->x + region->w ||
        (i > 0u && x[i] <= x[i - 1u]))
      return 0;
  }
  return 1;
}

static int layout_fits(const CountdownLayout *skin, int full_skin)
{
  const CountdownBubbleLayout *bubble = &skin->bubble;
  if (!rect_fits(skin->digit_region) || skin->width == 0u || skin->height == 0u ||
      !digits_fit(skin, skin->short_x, 5u) || !digits_fit(skin, skin->long_x, 6u))
    return 0;
  if (!(full_skin && !skin->progress.w && !skin->progress.h) &&
      (!rect_fits(skin->progress) || skin->progress.w < 7u)) return 0;
  for (unsigned i = 0; i < 2u; ++i)
  {
    Rect r = skin->title_rules[i];
    if (!(full_skin && !r.w && !r.h) && !rect_fits(r)) return 0;
  }
  if (!label_fits(&skin->title, skin->title.center, "倒计时中") ||
      !label_fits(&skin->title, skin->title.center, "正计时中") ||
      !label_fits(&skin->button, skin->button_centers[0], "开始") ||
      !label_fits(&skin->button, skin->button_centers[0], "暂停") ||
      !label_fits(&skin->button, skin->button_centers[1], "重置") ||
      !label_fits(&skin->button, skin->button_centers[2], "退出"))
    return 0;
  return bubble->font <= 4u && bubble->small_font <= 4u && bubble->centered <= 1u &&
         bubble->width != 0u && bubble->x + bubble->width <= 320u &&
         bubble->lines != 0u && bubble->lines <= 4u && bubble->line_height != 0u &&
         bubble->y + bubble->lines * (u32)bubble->line_height <= 240u;
}

int CountdownView_Init(int skin_status)
{
  layout = &default_layout;
  if (skin_status < 0) return 0;
  int status = UI_ReadLayout(COUNTDOWN_LAYOUT_OFFSET, "CNTL", &flash_layout, sizeof(flash_layout));
  if (status == 0 && layout_fits(&flash_layout, skin_status == 0))
  {
    layout = &flash_layout;
    printf("[UI] CNTL flash\r\n");
    return 1;
  }
  if (status != 1) printf("[UI] CNTL invalid, using built-in layout\r\n");
  return 0;
}

const CountdownBubbleLayout *CountdownView_Bubble(void)
{
  return &layout->bubble;
}

static const char *title_text(const CountdownViewState *state)
{
  return state->counting_up ? "正计时中" : "倒计时中";
}

static const char *button_text(const CountdownViewState *state, CountdownItem item)
{
  switch (Skin_CountdownAction(item - CNT_SEL_PAUSE))
  {
    case SKIN_COUNT_TOGGLE: return state->running ? "暂停" : "开始";
    case SKIN_COUNT_START: return "开始";
    case SKIN_COUNT_PAUSE: return "暂停";
    case SKIN_COUNT_RESET: return "重置";
    case SKIN_COUNT_EXIT:  return "退出";
  }
  return "";
}

static void draw_label(const CenteredLabel *label, int center, const char *text)
{
  const u32 base = font_base(label->font);
  UI_DrawStrPx(center - UI_StrPxWidth(text, base) / 2, label->y, text, base, label->color);
}

static void label_rect(const CenteredLabel *label, int center, const char *text, CountdownRect *rect)
{
  int width = UI_StrPxWidth(text, font_base(label->font));
  *rect = (CountdownRect){center - width / 2, label->y + label->ink_y, width, label->ink_height};
}

static void clear_rect(const Rect *rect)
{
  UI_DrawGlyphRect(rect->x, 240 - rect->y - rect->h, rect->w, rect->h, 0xFFFFu, 0);
}

static unsigned minute_digits(u32 seconds)
{
  return seconds >= 6000u ? 3u : 2u;
}

static const u16 *digit_positions(unsigned minutes)
{
  return minutes == 3u ? layout->long_x : layout->short_x;
}

static void draw_digit(unsigned digit, u16 x)
{
  UI_DrawSprite(Skin_CountdownDigit(digit), x, layout->y);
}

static void draw_minutes(u32 seconds, const u16 *x, unsigned count)
{
  u32 minutes = seconds / 60u;
  if (minutes > 999u) minutes = 999u;
  if (count == 3u) draw_digit(minutes / 100u, *x++);
  draw_digit(minutes / 10u % 10u, x[0]);
  draw_digit(minutes % 10u, x[1]);
}

static void draw_seconds(u32 seconds, const u16 *x)
{
  seconds %= 60u;
  draw_digit(seconds / 10u, x[0]);
  draw_digit(seconds % 10u, x[1]);
}

void CountdownView_DrawItem(const CountdownViewState *state, CountdownItem item)
{
  const unsigned count = minute_digits(state->seconds);
  const u16 *x = digit_positions(count);
  switch (item)
  {
    case CNT_SEL_MODE:
      draw_label(&layout->title, layout->title.center, title_text(state));
      break;
    case CNT_SEL_MIN:
      draw_minutes(state->seconds, x, count);
      UI_DrawSprite(Skin_CountdownColon(), x[count], layout->y);
      break;
    case CNT_SEL_SEC:
      draw_seconds(state->seconds, x + count + 1u);
      break;
    case CNT_SEL_PAUSE:
    case CNT_SEL_RESET:
    case CNT_SEL_EXIT:
      draw_label(&layout->button, layout->button_centers[item - CNT_SEL_PAUSE], button_text(state, item));
      break;
    default:
      break;
  }
}

int CountdownView_ItemRect(const CountdownViewState *state, CountdownItem item, CountdownRect *rect)
{
  const unsigned count = minute_digits(state->seconds);
  const u16 *x = digit_positions(count);
  switch (item)
  {
    case CNT_SEL_MODE:
      label_rect(&layout->title, layout->title.center, title_text(state), rect);
      break;
    case CNT_SEL_MIN:
    {
      int right = x[count - 1u] + layout->width;
      if (right > x[count]) right = x[count];   /* Colon is drawn above the last minute digit. */
      *rect = (CountdownRect){x[0], layout->y, right - x[0], layout->height};
      break;
    }
    case CNT_SEL_SEC:
      *rect = (CountdownRect){x[count + 1u], layout->y,
                             x[count + 2u] + layout->width - x[count + 1u], layout->height};
      break;
    case CNT_SEL_PAUSE:
    case CNT_SEL_RESET:
    case CNT_SEL_EXIT:
      label_rect(&layout->button, layout->button_centers[item - CNT_SEL_PAUSE], button_text(state, item), rect);
      break;
    default:
      return 0;
  }
  return rect->w > 0;
}

static void draw_progress(const CountdownViewState *state)
{
  static const u8 inset[4] = {3, 1, 1, 3};
  static int last_height = -1;
  static u16 last_background;
  const Rect *bar = &layout->progress;
  u32 remaining = state->seconds;
  int height = bar->h;

  if (state->total != 0u)
  {
    if (remaining > state->total) remaining = state->total;
    if (state->counting_up) remaining = state->total - remaining;
    height = (int)(bar->h * remaining / state->total);
  }
  if (height == last_height && last_background == g_ui_bg_ver) return;
  last_height = height;
  last_background = g_ui_bg_ver;

  clear_rect(bar);
  for (int row = 0; row < height; ++row)
  {
    int top = height - 1 - row;
    int margin = row < 4 ? inset[row] : 0;
    if (top < 4) margin = inset[top];
    Display_FillRect(bar->x + margin, bar->y + bar->h - 1 - row, bar->w - 2 * margin, 1,
                     layout->progress_color);
  }
}

void CountdownView_Draw(const CountdownViewState *state, CountdownItem hidden)
{
  static unsigned last_digits;
  const unsigned count = minute_digits(state->seconds);
  const u16 *x = digit_positions(count);

  if (count != last_digits)
    clear_rect(&layout->digit_region);
  last_digits = count;
  if (hidden != CNT_SEL_MIN) draw_minutes(state->seconds, x, count);
  UI_DrawSprite(Skin_CountdownColon(), x[count], layout->y);
  if (hidden != CNT_SEL_SEC) draw_seconds(state->seconds, x + count + 1u);

  if (Skin_CountdownUsesSegments()) Skin_CountdownProgress(state);
  else if (layout->progress.w) draw_progress(state);
  if (hidden != CNT_SEL_MODE) CountdownView_DrawItem(state, CNT_SEL_MODE);
  for (unsigned i = 0; i < 2u; ++i)
  {
    const Rect *rule = &layout->title_rules[i];
    if (rule->w) Display_FillRect(rule->x, rule->y, rule->w, rule->h, layout->title.color);
  }
  for (CountdownItem item = CNT_SEL_PAUSE; item < CNT_SEL_COUNT; ++item)
  {
    if (item != hidden) CountdownView_DrawItem(state, item);
  }
}

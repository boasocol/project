#ifndef COUNTDOWN_VIEW_H
#define COUNTDOWN_VIEW_H

#include "ht32.h"

typedef enum {
  CNT_SEL_MODE,
  CNT_SEL_MIN,
  CNT_SEL_SEC,
  CNT_SEL_PAUSE,
  CNT_SEL_RESET,
  CNT_SEL_EXIT,
  CNT_SEL_COUNT
} CountdownItem;

typedef struct {
  s16 x, y, w, h;
} CountdownRect;

typedef struct {
  u16 x, y, width, font, small_font, line_height, lines, color, centered;
} CountdownBubbleLayout;

typedef struct {
  u32 seconds;
  u32 total;
  u8 counting_up;
  u8 running;
} CountdownViewState;

int CountdownView_Init(int skin_status);
const CountdownBubbleLayout *CountdownView_Bubble(void);
void CountdownView_Draw(const CountdownViewState *state, CountdownItem hidden);
void CountdownView_DrawItem(const CountdownViewState *state, CountdownItem item);
int CountdownView_ItemRect(const CountdownViewState *state, CountdownItem item, CountdownRect *rect);

#endif

#ifndef UI_SKIN_H
#define UI_SKIN_H

#include "ht32.h"
#include "time_module.h"
#include "countdown_view.h"

#define SKIN_NO_SPRITE 0xFFFFu
#define SKIN_MAX_SEGMENTS 8u

typedef struct {
  u16 x, y, w, h;
} SkinRect;

typedef struct {
  CountdownBubbleLayout layout;
  char text[64];
} SkinBubble;

typedef enum {
  SKIN_COUNT_TOGGLE, SKIN_COUNT_RESET, SKIN_COUNT_EXIT, SKIN_COUNT_START, SKIN_COUNT_PAUSE
} SkinCountAction;

typedef enum {
  SKIN_CLOCK_HOUR, SKIN_CLOCK_MINUTE, SKIN_CLOCK_SECOND,
  SKIN_CLOCK_YEAR, SKIN_CLOCK_MONTH, SKIN_CLOCK_DAY
} SkinClockField;

typedef enum {
  SKIN_ALARM_SLOT, SKIN_ALARM_SWITCH, SKIN_ALARM_HOUR, SKIN_ALARM_MINUTE, SKIN_ALARM_DAYS
} SkinAlarmField;

typedef struct {
  u8 slot, hour, minute, enabled, weekdays, days;
} SkinAlarmState;

int Skin_Load(void);
void Skin_Activate(int ready);
int Skin_Active(void);
/* 内容缓存（2026-11-23）：字段内容没变就不清屏重画（修「每秒全屏闪一下」）。
   需要「内容没变也强制重画」时调 Skin_Invalidate()（闪灭亮相 / 背景重传 / 闹钟屏重绘），
   只重画某一项调 Skin_InvalidateField()。 */
void Skin_Invalidate(void);
void Skin_InvalidateField(SkinClockField field);
const SkinBubble *Skin_GetBubble(unsigned page);
void Skin_Clock(const TimeStruct *t, int weekday, const char *lunar, const char *temp,
                const char *humi, int hidden);
int Skin_ClockItemRect(const TimeStruct *t, SkinClockField item, SkinRect *rect);
int Skin_ClockHasSeconds(void);
enum {
  SKIN_STATUS_TIMER = 1u << 0,
  SKIN_STATUS_HOURLY = 1u << 1,
  SKIN_STATUS_ALARM = 1u << 2,
  SKIN_STATUS_ALL = SKIN_STATUS_TIMER | SKIN_STATUS_HOURLY | SKIN_STATUS_ALARM
};
int Skin_DrawStatusIcons(unsigned visible);
void Skin_Hourly(int active);
void Skin_Alarm(const SkinAlarmState *state, int hidden);
int Skin_AlarmItemRect(const SkinAlarmState *state, SkinAlarmField item, SkinRect *rect);
int Skin_AlarmSwitchCover(void);
void Skin_Robot(unsigned page, unsigned state, unsigned frame);
u16 Skin_CountdownDigit(unsigned value);
u16 Skin_CountdownColon(void);
SkinCountAction Skin_CountdownAction(unsigned button);
int Skin_CountdownHasExit(void);
int Skin_CountdownUsesSegments(void);
void Skin_CountdownProgress(const CountdownViewState *state);
void Skin_Clear(SkinRect rect);

#endif

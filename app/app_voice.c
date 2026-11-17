/************************************************************************************************************
 * @file    app_voice.c
 * @brief   语音命令业务层（P2 反转 2026-09-03，自 voice.c 迁出）
 * @note    voice.c（协议驱动）只解析帧，收到完整固定帧后回调本模块 App_VoiceCmd 做业务：
 *            - 打印帧
 *            - Screen_HandleVoice（倒计时/番茄/暂停继续结束/闹钟开关循环铃音/贪睡/别响/
 *              显示开关/整点报时/查看闹钟/正计时，命中统一回 OK + ok 动画）
 *            - 语音基础域（唤醒/设时/设日/设闹钟/报时/状态/音量，各 vcmd_* 回对应响应）
 *            - 未接线命令（天数倒计时 0x83/0xB7）回 VOICE_TX_FAIL
 * 依赖方向：本模块（app）→ voice.c/ui.c/screen.c/time_module.c（驱动/图形/应用），
 *           不再反向（voice.c 已不含任何应用层头）。
 ************************************************************************************************************/
#include "app_voice.h"
#include "voice.h"
#include "time_module.h" /* TimeModule_SetTime/GetTime：设时钟/日期/报时 */
#include "ui.h"          /* UI_CharAnim_* 响应动画 / UI_SetScreen / UI_GetScreen */
#include "screen.h"      /* Screen_AlarmSet / Screen_HandleVoice / Screen_ReturnMain */
#include "calendar.h"    /* Calendar_DaysInMonth：设日期按万年历校验（坑#77 #4 2026-09-05） */
#include <stdio.h>

/* 2026-09-05：语音( UART0)与蓝牙(USART0)外设独立可共存，启用只受 VOICE_ENABLE 控制 */
#if VOICE_ENABLE

/* 唤醒词 0x80：模块被"小保小保"唤醒 -> 回 VOICE_TX_WAKE_BEEP 播放唤醒提示音"嘀"，
   并切动画到唤醒状态（UI_CharAnim_SetState(UI_CANI_STATE_WAKE)，走过渡；
   wake 帧未加时返回 -1 保持原状态）。模块 15s 无指令自动休眠（发 0xEF d0=01），
   MCU 收到后切回待机（见 vcmd_state）。 */
static void vcmd_wake(const VoiceFrame *f)
{
  printf("[VOICE] 唤醒词 d0=%u -> 唤醒提示音 + 动画切唤醒状态\r\n", (unsigned)f->d0);
  Voice_Send(VOICE_TX_WAKE_BEEP, 0, 0, 0, 0);
  (void)UI_CharAnim_SetState(UI_CANI_STATE_WAKE);
}

/* 设置时钟 0x81：d0=时(1-24→0-23) d1=分(0-59)；d0=FF&&d1=FF=模块未识别出时间 */
static void vcmd_set_time(const VoiceFrame *f)
{
  u8 hh = f->d0, mm = f->d1;
  TimeStruct t;
  if (f->d0 == VOICE_PARM_UNRECOG && f->d1 == VOICE_PARM_UNRECOG)
  {
    printf("[VOICE] 时间识别失败\r\n");
    Voice_Send(VOICE_TX_FAIL, 0, 0, 0, 0);
    return;
  }
  if (hh == 24) hh = 0;                       /* 24:00 -> 00:00 */
  if (hh > 23 || mm > 59)
  {
    printf("[VOICE] 时间非法 %u:%u\r\n", (unsigned)hh, (unsigned)mm);
    Voice_Send(VOICE_TX_FAIL, 0, 0, 0, 0);
    return;
  }
  TimeModule_GetTime(&t);
  t.hour = hh; t.minute = mm; t.second = 0;
  TimeModule_SetTime(&t);
  Screen_AlarmRungReset();   /* 坑#82：语音改时间后清"本分钟已响"（回拨后闹钟重新判定） */
  Screen_SetupVoiceSync(&t);   /* 坑#71 #10：手动设置模式中语音改时间，同步冻结显示为新值（即时刷新，退出不被旧值覆盖） */
  printf("[VOICE] 设置时钟 -> %02u:%02u\r\n", (unsigned)hh, (unsigned)mm);
  Screen_GoMain();         /* 坑#77 #1：语音设时间成功 → 跳回时间显示（闹钟屏下不再停留；倒计时运行/手动设置/响铃中等例外守卫内不切走，坑#80 拆 GoMain） */
  Voice_Send(VOICE_TX_OK, 0, 0, 0, 0);        /* "好的，设置成功" */
  (void)UI_CharAnim_PlayOnce(UI_CANI_STATE_OK); /* 播放"修改成功"响应动画（播完自动回原状态） */
}

/* 设置日期 0x82：d0=年十位(固定20) d1=年个位(0-99) d2=月 d3=日 */
static void vcmd_set_date(const VoiceFrame *f)
{
  u16 yr = 2000u + f->d1;
  u8  mo = f->d2, da = f->d3;
  u8  dim = Calendar_DaysInMonth(mo, (u16)yr);   /* 坑#77 #4：按正常万年历校验（防 2/31 等非法日期被设进 RTC） */
  TimeStruct t;
  if (f->d1 > 99 || dim == 0 || da < 1 || da > dim)
  {
    printf("[VOICE] 日期非法 %04u-%02u-%02u\r\n", (unsigned)yr, (unsigned)mo, (unsigned)da);
    Voice_Send(VOICE_TX_FAIL, 0, 0, 0, 0);
    return;
  }
  TimeModule_GetTime(&t);
  t.year = yr; t.month = mo; t.day = da;
  TimeModule_SetTime(&t);
  Screen_AlarmRungReset();   /* 坑#82：语音改日期后清"本分钟已响"（改日期也可能跨过闹钟时刻） */
  Screen_SetupVoiceSync(&t);   /* 坑#71 #10：手动设置模式中语音改日期，同步冻结显示为新值 */
  printf("[VOICE] 设置日期 -> %04u-%02u-%02u\r\n", (unsigned)yr, (unsigned)mo, (unsigned)da);
  Screen_GoMain();         /* 坑#77 #1：语音设日期成功 → 跳回时间显示（同设时间，坑#80 拆 GoMain） */
  Voice_Send(VOICE_TX_OK, 0, 0, 0, 0);
  (void)UI_CharAnim_PlayOnce(UI_CANI_STATE_OK); /* 播放"修改成功"响应动画 */
}

/* 报时 0xB6：读 RTC，让模块播"现在是XX点XX分"（MCU→CI 0x07） */
static void vcmd_tell_time(const VoiceFrame *f)
{
  TimeStruct t;
  (void)f;
  TimeModule_GetTime(&t);
  printf("[VOICE] 报时 -> %02u:%02u\r\n", (unsigned)t.hour, (unsigned)t.minute);
  Voice_Send(VOICE_TX_TELL_TIME, t.hour, t.minute, 0, 0);
}

/* 设置闹钟 0x8A：d0=闹钟号(1-3) d1=时 d2=分；设备支持 3 个闹钟槽（2026-09-05）。
   2026-08-28 改为【只要收到 0x8A 就切闹钟屏】：
     - 时间合法（d1<=23 d2<=59）→ 更新闹钟号 n 的时间并切屏
     - 时间未识别/非法（d1/d2=0xFF 或越界）→ 保留当前闹钟配置，仅切闹钟屏查看当前配置
   这样"设置闹钟1"即使模块没接上具体时间也能看到闹钟屏。 */
static void vcmd_set_alarm(const VoiceFrame *f)
{
  u8 n = f->d0, hh = f->d1, mm = f->d2;
  printf("[VOICE] 设闹钟原始 d0=%u d1=%u d2=%u\r\n", (unsigned)n, (unsigned)hh, (unsigned)mm);
  if (hh == 24) hh = 0;
  if (n >= 1 && n <= 3 && hh <= 23 && mm <= 59)
  {
    Screen_AlarmSet(n, hh, mm);   /* 写闹钟 n 配置 + 切到闹钟屏显示新时间（2026-09-05 3 槽） */
    printf("[VOICE] 设置闹钟%u -> %02u:%02u\r\n", (unsigned)n, (unsigned)hh, (unsigned)mm);
    Voice_Send(VOICE_TX_OK, 0, 0, 0, 0);
  }
  else
  {
    /* 时间没识别/非法：保留当前配置，仍切到闹钟屏查看（方便二次确认）；已在闹钟屏不重传背景 */
    printf("[VOICE] 设闹钟参数不完整/非法（n=%u hh=%u mm=%u）-> 仅切闹钟屏查看当前配置\r\n",
           (unsigned)n, (unsigned)hh, (unsigned)mm);
    if (UI_GetScreen() != UI_SCREEN_ALARM) UI_SetScreen(UI_SCREEN_ALARM);
    Voice_Send(VOICE_TX_FAIL, 0, 0, 0, 0);   /* 模块播"设置失败"（时间未识别属正常，需再说一次时间） */
  }
  (void)UI_CharAnim_PlayOnce(UI_CANI_STATE_OK); /* 播放"修改成功"响应动画 */
}

/* 模块状态通知 0xEF：d0=00 上电完成 / 01 15s无语音命令。
   注意：开机播报不挂这里——debug 仿真时模块全程不断电、MCU 复位不重发 0xEF，
   由 main 每次复位无条件发 0x00（见 main.c）。
   d0=01（15s 无语音命令）时动画从唤醒切回待机（睡眠）。 */
static void vcmd_state(const VoiceFrame *f)
{
  printf("[VOICE] 模块状态 d0=%u (%s)\r\n", (unsigned)f->d0,
         f->d0 == VOICE_ST_D0_PWRON   ? "上电完成" :
         f->d0 == VOICE_ST_D0_TIMEOUT ? "15s无命令" : "未知");
  if (f->d0 == VOICE_ST_D0_TIMEOUT)
  {
    printf("[VOICE] 15s无命令 -> 动画回待机 + 回主屏（倒计时屏运行中除外，2026-09-05 收窄守卫）\r\n");
    (void)UI_CharAnim_SetState(UI_CANI_STATE_IDLE);   /* 动画：唤醒状态 -> 睡眠（待机）状态 */
    Voice_Send(VOICE_TX_SLEEP_BEEP, 0, 0, 0, 0);      /* 坑#93：动画从唤醒进睡眠时发"休眠提示音"（FA F1 00 0F 00 00 00 CS FB） */
    Screen_ReturnMain();                              /* 2026-08-27：15s 无命令回主屏 */
  }
}

/* 音量 0xEE（模块侧音量）：仅记录 */
static void vcmd_volume(const VoiceFrame *f)
{
  printf("[VOICE] 音量 d0=%u (%s)\r\n", (unsigned)f->d0,
         f->d0 == 0 ? "最高声" : (f->d0 == 1 ? "最低声" : "静音"));
}

/* 语音基础命令统一入口（与 Screen_HandleVoice 同签名、同语义；返回 1=已处理）
   处理：唤醒/设时/设日/设闹钟/报时/状态/音量 */
static int voice_handle_base(const VoiceFrame *f)
{
  switch (f->cmd)
  {
    case VOICE_CMD_WAKE:      vcmd_wake(f);      return 1;
    case VOICE_CMD_SET_TIME:  vcmd_set_time(f);  return 1;
    case VOICE_CMD_SET_DATE:  vcmd_set_date(f);  return 1;
    case VOICE_CMD_SET_ALARM: vcmd_set_alarm(f); return 1;
    case VOICE_CMD_TELL_TIME: vcmd_tell_time(f); return 1;
    case VOICE_CMD_STATE:     vcmd_state(f);     return 1;
    case VOICE_CMD_VOLUME:    vcmd_volume(f);    return 1;
    default: return 0;
  }
}

/* 统一业务入口（协议驱动 voice.c 注册的回调，Voice_SetCmdHandler(App_VoiceCmd)） */
int App_VoiceCmd(const VoiceFrame *f)
{
  int rv;
  printf("[VOICE] FAF1 cmd=0x%02X d0=%02X d1=%02X d2=%02X d3=%02X\r\n",
         f->cmd, f->d0, f->d1, f->d2, f->d3);

  /* 屏幕业务域（倒计时/番茄/闹钟/正计时/显示开关/整点报时/查看闹钟，2026-08-27/28 多屏）：
     screen.c 处理，命中统一回 OK + ok 动画 */
  rv = Screen_HandleVoice(f);
  if (rv == 2)                                 /* 坑#76：响闹中静默拦截——不回应不播动画（保持响闹） */
  {
    printf("[VOICE] screen 静默拦截 cmd=0x%02X\r\n", f->cmd);
    return 1;
  }
  if (rv)
  {
    printf("[VOICE] screen cmd 0x%02X handled\r\n", f->cmd);
    /* 坑#93（2026-11-04）：倒计时启动命令（0x84 分钟 / 0x88 番茄 / 0xBA 正计时）——
       回复改为"计时开始"（0x0D，Screen_CntStart/UpStart 内已发，作唯一回复），
       不再回 0x02"设置成功"（用户实测：说"倒计时一分钟"助手答"设置成功"，应答"计时开始"）。
       其余 screen 命令照旧回 OK。 */
    if (f->cmd != VOICE_CMD_CNT_MIN && f->cmd != VOICE_CMD_TOMATO_ON && f->cmd != VOICE_CMD_CNT_UP)
      Voice_Send(VOICE_TX_OK, 0, 0, 0, 0);
    (void)UI_CharAnim_PlayOnce(UI_CANI_STATE_OK);
    return 1;
  }

  /* 语音基础域：vcmd_* 各自回响应 */
  if (voice_handle_base(f)) return 1;

  /* 两层都不认识：未接线命令（天数倒计时 0x83/0xB7，待后续） */
  printf("[VOICE] 未接线 cmd=0x%02X（天数倒计时 0x83/0xB7，待后续）\r\n", f->cmd);
  Voice_Send(VOICE_TX_FAIL, 0, 0, 0, 0);   /* 模块播"设置失败，请重试" */
  return 1;
}

#endif /* VOICE_ENABLE（2026-09-05 解除与蓝牙互斥） */

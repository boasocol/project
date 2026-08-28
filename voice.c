/************************************************************************************************************
 * @file    voice.c
 * @brief   CI-D0XGS02S 语音识别模块驱动（UART0：PB7=TX, PB8=RX，波特率 9600）
 * @note    - 占用 UART0（PB7/PB8，原 printf 调试口）；printf 改走 USART0(PC4/PC5)
 *          - 帧格式（双协议自适应）：
 *              A) 语音时钟 V10（固定 10 字节）：[H0][H1][ID][CMD][d0..d3][CS][FB]，H0H1=A5FA|FAF1
 *              B) 启英泰伦 CI130X（变长）：[A5][FC][L0][L1][type][cmd][seq][data x L][CS0][CS1][FB]
 *          - 接收中断把字节入环形缓冲，Voice_Process 在主循环做状态机解析
 ************************************************************************************************************/
#include "voice.h"
#include "hd9803.h"   /* 取 HD9803_ENABLE，与蓝牙互斥占 USART0 */
#include "BFTM0.h"    /* bftm0_ct：1ms 计数器，诊断打印用 */
#include "time_module.h"  /* TimeModule_SetTime/GetTime：语音设时钟/日期/报时 */
#include "ui.h"           /* UI_SetAlarm：语音设闹钟 */
#include "screen.h"       /* Screen_AlarmSet：设闹钟切闹钟屏（2026-08-27 多屏） */
#include <stdio.h>

#if VOICE_ENABLE && !HD9803_ENABLE

/* ---- 环形接收缓冲（ISR 写入，主循环读取） ---- */
static volatile u8  g_vrx_buf[VOICE_RX_BUF_SIZE];
static volatile u16 g_vrx_head = 0;   /* ISR 写入指针 */
static volatile u16 g_vrx_tail = 0;   /* poll 读取指针 */

/* ---- 解析状态机（双协议：V10 固定 10 字节 / CI130X 变长） ---- */
typedef enum
{
  VS_HEAD0 = 0,   /* 等帧头第 1 字节（A5 或 FA） */
  VS_HEAD1,       /* 等帧头第 2 字节（FA/F1=固定帧，FC=变长帧） */
  VS_ID,          /* [固定] 等 ID */
  VS_CMD,         /* [固定] 等 CMD */
  VS_D0, VS_D1, VS_D2, VS_D3,  /* [固定] 指令内容 4 字节 */
  VS_LEN0,        /* [变长] 等长度低字节 */
  VS_LEN1,        /* [变长] 等长度高字节 */
  VS_VDATA,       /* [变长] 收集有效数据（type/cmd/seq/data...） */
  VS_CS,          /* [固定] 等校验和 / [变长] 等校验第 1 字节 */
  VS_CS2,         /* [变长] 等校验第 2 字节 */
  VS_TAIL,        /* 等帧尾 0xFB */
} VoiceState;

static u8    g_vstate = VS_HEAD0;
static u8    g_vh0, g_vh1;         /* 帧头（A5FA / FAF1 / A5FC） */
static u8    g_vvar = 0;           /* 1=当前为 CI130X 变长帧 */
static u8    g_vfrm[VOICE_FRM_MAX];/* 帧数据缓冲：固定帧 [ID CMD D0 D1 D2 D3]，
                                      变长帧 [type cmd seq data...] */
static u16   g_vlen = 0;           /* [变长] 声明长度 L0|L1<<8（截断保护） */
static u16   g_vpos = 0;           /* 已收数据字节数 */
static u8    g_vcs = 0;            /* 本地累加和 */

/* ---- 解析结果 ---- */
static VoiceFrame g_vresult;
static u8 g_vnew = 0;

/* ------------------------------------------------------------------
 * 接收中断：一个字节入环形缓冲
 * ------------------------------------------------------------------ */
void VOICE_UART_IRQHandler(void)
{
  if (USART_GetFlagStatus(VOICE_UART, USART_FLAG_RXDR))
  {
    u8 d = USART_ReceiveData(VOICE_UART);
    u16 n = (u16)((g_vrx_head + 1) % VOICE_RX_BUF_SIZE);
    if (n != g_vrx_tail)                 /* 非满才写入 */
    {
      g_vrx_buf[g_vrx_head] = d;
      g_vrx_head = n;
    }
  }
}

/* ------------------------------------------------------------------
 * 状态机：喂一个字节，返回 1=完整帧解析完成
 * 帧 A（V10 固定 10 字节）：[H0][H1][ID][CMD][d0][d1][d2][d3][CS][FB]
 *   校验：CS = (u8)(H0+H1+ID+CMD+d0+d1+d2+d3)，H0H1 = A5FA | FAF1
 * 帧 B（CI130X 变长）：[A5][FC][L0][L1][type][cmd][seq][data x L][CS0][CS1][FB]
 *   L = L0 + L1*256（有效数据长度）；CS 算法暂不实现，先按结构锁定帧（帧尾 FB 必有）
 * ------------------------------------------------------------------ */
static u8 voice_feed(u8 d)
{
  switch (g_vstate)
  {
    case VS_HEAD0:
      if (d == VOICE_FRM_HEAD_A0 || d == VOICE_FRM_HEAD_B0)
      {
        g_vh0 = d;
        g_vstate = VS_HEAD1;
      }
      break;

    case VS_HEAD1:
      if (g_vh0 == VOICE_FRM_HEAD_A0)
      {
        if (d == VOICE_FRM_HEAD_A1)          /* A5 FA -> V10 固定帧 */
        {
          g_vh1 = d; g_vvar = 0; g_vpos = 0;
          g_vcs  = (u8)(g_vh0 + g_vh1);
          g_vstate = VS_ID;
        }
        else if (d == VOICE_FRM_HEAD_C1)     /* A5 FC -> CI130X 变长帧 */
        {
          g_vh1 = d; g_vvar = 1;
          g_vstate = VS_LEN0;
        }
        else if (d == VOICE_FRM_HEAD_A0)     /* 连续 A5 */
        {
          /* 保持等第 2 字节 */
        }
        else
          g_vstate = VS_HEAD0;
      }
      else /* g_vh0 == FA */
      {
        if (d == VOICE_FRM_HEAD_B1)          /* FA F1 -> V10 固定帧 */
        {
          g_vh1 = d; g_vvar = 0; g_vpos = 0;
          g_vcs  = (u8)(g_vh0 + g_vh1);
          g_vstate = VS_ID;
        }
        else if (d == VOICE_FRM_HEAD_A0)     /* FA A5... 可能新头 */
          g_vh0 = d;
        else if (d != VOICE_FRM_HEAD_B0)
          g_vstate = VS_HEAD0;               /* FA FA... 保持等 F1 */
      }
      break;

    /* ---- V10 固定帧：ID CMD D0 D1 D2 D3 ---- */
    case VS_ID:
      g_vfrm[0] = d; g_vcs += d; g_vstate = VS_CMD;
      break;
    case VS_CMD:
      g_vfrm[1] = d; g_vcs += d; g_vstate = VS_D0;
      break;
    case VS_D0:
      g_vfrm[2] = d; g_vcs += d; g_vstate = VS_D1;
      break;
    case VS_D1:
      g_vfrm[3] = d; g_vcs += d; g_vstate = VS_D2;
      break;
    case VS_D2:
      g_vfrm[4] = d; g_vcs += d; g_vstate = VS_D3;
      break;
    case VS_D3:
      g_vfrm[5] = d; g_vcs += d; g_vstate = VS_CS;
      break;

    /* ---- CI130X 变长帧：长度 + 有效数据 ---- */
    case VS_LEN0:
      g_vlen = d;
      g_vcs  = (u8)(VOICE_FRM_HEAD_A0 + VOICE_FRM_HEAD_C1) + d;
      g_vstate = VS_LEN1;
      break;
    case VS_LEN1:
      g_vlen |= (u16)(d << 8);
      g_vcs += d;
      /* L 为纯数据长度；type/cmd/seq 固定 3 字节在 data 之前，一并收集 */
      if (g_vlen == 0 || g_vlen + 3u > VOICE_FRM_MAX)  /* 长度异常：丢帧防溢出 */
      {
        g_vstate = VS_HEAD0;
        break;
      }
      g_vlen += 3u;                     /* 实际收集 = type+cmd+seq+data(L) */
      g_vpos = 0;
      g_vstate = VS_VDATA;
      break;
    case VS_VDATA:
      if (g_vpos < (u16)sizeof(g_vfrm))
        g_vfrm[g_vpos] = d;
      g_vcs += d;
      if (++g_vpos >= g_vlen)
        g_vstate = VS_CS;
      break;

    /* ---- 校验 ---- */
    case VS_CS:                     /* [固定]CS=累加和；[变长]CS0 暂不校验 */
      g_vstate = g_vvar ? VS_CS2 : VS_TAIL;
      if (!g_vvar && d != g_vcs)    /* 固定帧校验错：等新头 */
        g_vstate = VS_HEAD0;
      break;

    case VS_CS2:                    /* [变长]CS1 暂不校验 */
      g_vstate = VS_TAIL;
      break;

    case VS_TAIL:
      g_vstate = VS_HEAD0;
      if (d == VOICE_FRM_TAIL)
      {
        g_vresult.head[0] = g_vh0;
        g_vresult.head[1] = g_vh1;
        if (g_vvar)
        {
          /* CI130X 变长帧：vfrm[0]=type vfrm[1]=cmd vfrm[2]=seq vfrm[3..]=data */
          g_vresult.id  = g_vfrm[0];
          g_vresult.cmd = g_vfrm[1];
          g_vresult.d0  = g_vfrm[2];
          g_vresult.d1  = (g_vpos > 3) ? g_vfrm[3] : 0;
          g_vresult.d2  = (g_vpos > 4) ? g_vfrm[4] : 0;
          g_vresult.d3  = (g_vpos > 5) ? g_vfrm[5] : 0;
        }
        else
        {
          g_vresult.id  = g_vfrm[0];
          g_vresult.cmd = g_vfrm[1];
          g_vresult.d0  = g_vfrm[2];
          g_vresult.d1  = g_vfrm[3];
          g_vresult.d2  = g_vfrm[4];
          g_vresult.d3  = g_vfrm[5];
        }
        g_vnew = 1;
        return 1;
      }
      break;
  }
  return 0;
}

/************************************************************************************************************
 * MCU→CI 发送：深合芯 V10 固定帧（FA F1 00 cmd d0 d1 d2 d3 CS FB，CS=累加和低字节）
 ************************************************************************************************************/
void Voice_Send(u8 cmd, u8 d0, u8 d1, u8 d2, u8 d3)
{
  u8 b[10] = { 0xFA, 0xF1, 0x00, cmd, d0, d1, d2, d3,
               (u8)(0xFA + 0xF1 + 0x00 + cmd + d0 + d1 + d2 + d3), 0xFB };
  for (u8 i = 0; i < 10; i++)
  {
    while (!USART_GetFlagStatus(VOICE_UART, USART_FLAG_TXDE));  /* 等发送缓冲空 */
    USART_SendData(VOICE_UART, b[i]);
  }
}

/************************************************************************************************************
 * 语音命令分发（FAF1/A5FA 固定帧，CI→MCU 0x80~0xEF）
 * 核心批已实现：唤醒/设时钟/设日期/报时/设闹钟(存配置)/状态通知/音量。
 * 未实现（倒计时/番茄/闹钟开关/显示开关等）：回复 0x09"设置失败"并打印，下一批补。
 ************************************************************************************************************/
static void vcmd_wake(const VoiceFrame *f)
{
  /* 模块被"小保小保"唤醒（VOICE_CMD_WAKE）-> MCU 回 VOICE_TX_WAKE_BEEP 播放唤醒提示音"嘀"，
     并切动画到唤醒状态（UI_CharAnim_SetState(UI_CANI_STATE_WAKE)，走过渡；wake 帧未加时返回 -1 保持原状态）。
     模块 15s 无指令自动休眠（发 VOICE_CMD_STATE, VOICE_ST_D0_TIMEOUT），MCU 收到后切回待机（vcmd_state）。 */
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
  printf("[VOICE] 设置时钟 -> %02u:%02u\r\n", (unsigned)hh, (unsigned)mm);
  Voice_Send(VOICE_TX_OK, 0, 0, 0, 0);        /* "好的，设置成功" */
  (void)UI_CharAnim_PlayOnce(UI_CANI_STATE_OK); /* 播放"修改成功"响应动画（播完自动回原状态） */
}

/* 设置日期 0x82：d0=年十位(固定20) d1=年个位(0-99) d2=月 d3=日 */
static void vcmd_set_date(const VoiceFrame *f)
{
  u16 yr = 2000u + f->d1;
  u8  mo = f->d2, da = f->d3;
  TimeStruct t;
  if (f->d1 > 99 || mo < 1 || mo > 12 || da < 1 || da > 31)
  {
    printf("[VOICE] 日期非法 %04u-%02u-%02u\r\n", (unsigned)yr, (unsigned)mo, (unsigned)da);
    Voice_Send(VOICE_TX_FAIL, 0, 0, 0, 0);
    return;
  }
  TimeModule_GetTime(&t);
  t.year = yr; t.month = mo; t.day = da;
  TimeModule_SetTime(&t);
  printf("[VOICE] 设置日期 -> %04u-%02u-%02u\r\n", (unsigned)yr, (unsigned)mo, (unsigned)da);
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

/* 设置闹钟 0x8A：d0=闹钟号(1-3) d1=时 d2=分；设备仅单闹钟槽。
   2026-08-28 改为【只要收到 0x8A 就切闹钟屏】：
     - 时间合法（d1<=23 d2<=59）→ 更新闹钟时间并切屏
     - 时间未识别/非法（d1/d2=0xFF 或越界）→ 保留当前闹钟时间，仅切闹钟屏查看当前配置
   这样"设置闹钟1"即使模块没接上具体时间也能看到闹钟屏。 */
static void vcmd_set_alarm(const VoiceFrame *f)
{
  u8 n = f->d0, hh = f->d1, mm = f->d2;
  printf("[VOICE] 设闹钟原始 d0=%u d1=%u d2=%u\r\n", (unsigned)n, (unsigned)hh, (unsigned)mm);
  if (hh == 24) hh = 0;
  if (n >= 1 && n <= 3 && hh <= 23 && mm <= 59)
  {
    Screen_AlarmSet(hh, mm);   /* 写闹钟配置 + 切到闹钟屏显示新时间 */
    printf("[VOICE] 设置闹钟%u -> %02u:%02u\r\n", (unsigned)n, (unsigned)hh, (unsigned)mm);
    Voice_Send(VOICE_TX_OK, 0, 0, 0, 0);
  }
  else
  {
    /* 时间没识别/非法：保留当前配置，仍切到闹钟屏查看（方便二次确认） */
    printf("[VOICE] 设闹钟参数不完整/非法（n=%u hh=%u mm=%u）-> 仅切闹钟屏查看当前配置\r\n",
           (unsigned)n, (unsigned)hh, (unsigned)mm);
    UI_SetScreen(UI_SCREEN_ALARM);
    Voice_Send(VOICE_TX_FAIL, 0, 0, 0, 0);   /* 模块播"设置失败"（时间未识别属正常，需再说一次时间） */
  }
  (void)UI_CharAnim_PlayOnce(UI_CANI_STATE_OK); /* 播放"修改成功"响应动画 */
}

/* 模块状态通知 0xEF：d0=00 上电完成 / 01 15s无语音命令。
   注意：开机播报不挂这里——debug 仿真时模块全程不断电、MCU 复位不重发 0xEF，
   由 main 每次复位无条件发 0x00（见 main.c）。
   d0=01（15s 无语音命令）时动画从唤醒切回待机。 */
static void vcmd_state(const VoiceFrame *f)
{
  printf("[VOICE] 模块状态 d0=%u (%s)\r\n", (unsigned)f->d0,
         f->d0 == VOICE_ST_D0_PWRON   ? "上电完成" :
         f->d0 == VOICE_ST_D0_TIMEOUT ? "15s无命令" : "未知");
  if (f->d0 == VOICE_ST_D0_TIMEOUT)
  {
    printf("[VOICE] 15s无命令 -> 动画回待机 + 回主屏（倒计时运行中除外）\r\n");
    (void)UI_CharAnim_SetState(UI_CANI_STATE_IDLE);
    Screen_ReturnMain();   /* 2026-08-27：15s 无命令回主屏 */
  }
}

/* 音量 0xEE（模块侧音量）：仅记录 */
static void vcmd_volume(const VoiceFrame *f)
{
  printf("[VOICE] 音量 d0=%u (%s)\r\n", (unsigned)f->d0,
         f->d0 == 0 ? "最高声" : (f->d0 == 1 ? "最低声" : "静音"));
}

static void voice_handle_frame(const VoiceFrame *f)
{
  printf("[VOICE] FAF1 cmd=0x%02X d0=%02X d1=%02X d2=%02X d3=%02X\r\n",
         f->cmd, f->d0, f->d1, f->d2, f->d3);

  /* 倒计时/闹钟/切屏命令（2026-08-27 多屏）：screen.c 处理，成功回 OK + ok 动画 */
  if (Screen_HandleVoice(f))
  {
    printf("[VOICE] screen cmd 0x%02X handled\r\n", f->cmd);
    Voice_Send(VOICE_TX_OK, 0, 0, 0, 0);
    (void)UI_CharAnim_PlayOnce(UI_CANI_STATE_OK);
    return;
  }

  switch (f->cmd)
  {
    case VOICE_CMD_WAKE:      vcmd_wake(f);      break;
    case VOICE_CMD_SET_TIME:  vcmd_set_time(f);  break;
    case VOICE_CMD_SET_DATE:  vcmd_set_date(f);  break;
    case VOICE_CMD_SET_ALARM: vcmd_set_alarm(f); break;
    case VOICE_CMD_TELL_TIME: vcmd_tell_time(f); break;
    case VOICE_CMD_STATE:     vcmd_state(f);     break;
    case VOICE_CMD_VOLUME:    vcmd_volume(f);    break;
    default:
      printf("[VOICE] 未实现 cmd=0x%02X（天数倒计时/显示开关等，待下一批）\r\n", f->cmd);
      Voice_Send(VOICE_TX_FAIL, 0, 0, 0, 0);   /* 模块播"设置失败，请重试" */
      break;
  }
}

/************************************************************************************************************
 * UART0 初始化（PB7=TX, PB8=RX, 9600）
 ************************************************************************************************************/
void Voice_Init(void)
{
  /* 1) 使能时钟：AFIO、USART、GPIO_C */
  {
    CKCU_PeripClockConfig_TypeDef clk = {{0}};
    clk.Bit.AFIO   = 1;
    clk.Bit.UART0  = 1;
    clk.Bit.PB     = 1;
    CKCU_PeripClockConfig(clk, ENABLE);
  }

  /* 2) RX 脚内部上拉，避免悬空 */
  GPIO_PullResistorConfig(VOICE_GPIO_PORT, VOICE_RX_AFIO_PIN, GPIO_PR_UP);

  /* 3) 引脚复用：PB7=UART0_TX, PB8=UART0_RX */
  AFIO_GPxConfig(VOICE_GPIO_ID, VOICE_TX_AFIO_PIN, AFIO_FUN_USART_UART);
  AFIO_GPxConfig(VOICE_GPIO_ID, VOICE_RX_AFIO_PIN, AFIO_FUN_USART_UART);

  /* 4) USART 9600, 8N1 */
  {
    USART_InitTypeDef us = {0};
    us.USART_BaudRate   = VOICE_BAUDRATE;
    us.USART_WordLength = USART_WORDLENGTH_8B;
    us.USART_StopBits   = USART_STOPBITS_1;
    us.USART_Parity     = USART_PARITY_NO;
    us.USART_Mode       = USART_MODE_NORMAL;
    USART_Init(VOICE_UART, &us);
  }

  /* 5) NVIC + 接收中断 + 收发使能 */
  NVIC_EnableIRQ(VOICE_UART_IRQn);
  USART_IntConfig(VOICE_UART, USART_INT_RXDR, ENABLE);
  USART_TxCmd(VOICE_UART, ENABLE);
  USART_RxCmd(VOICE_UART, ENABLE);

  g_vrx_head = g_vrx_tail = 0;
  g_vstate = VS_HEAD0;
  g_vnew = 0;

  printf("[VOICE] init OK: PB7=TX PB8=RX @%lu\r\n", (unsigned long)VOICE_BAUDRATE);
}

/************************************************************************************************************
 * 主循环调用：把环形缓冲中的字节喂给状态机
 ************************************************************************************************************/
void Voice_Process(void)
{
  while (g_vrx_tail != g_vrx_head)
  {
    u8 d = g_vrx_buf[g_vrx_tail];
    g_vrx_tail = (u16)((g_vrx_tail + 1) % VOICE_RX_BUF_SIZE);
    voice_feed(d);
  }

  /* 收到完整帧：A5FC（CI130X 变长，核心批不处理）仅打印；FAF1/A5FA 走命令分发 */
  {
    VoiceFrame f;
    if (Voice_GetFrame(&f))
    {
      if (f.head[0] == VOICE_FRM_HEAD_A0 && f.head[1] == VOICE_FRM_HEAD_C1)
      {
        /* A5FC 变长帧：打印声明长度与实际全部字节 */
        printf("[VOICE] A5FC L=%u:", (unsigned)g_vlen);
        for (u16 k = 0; k < g_vlen; k++)
          printf(" %02X", g_vfrm[k]);
        printf("\r\n");
      }
      else
      {
        voice_handle_frame(&f);   /* 固定帧：打印 + 命令分发 */
      }
    }
  }
}

/************************************************************************************************************
 * 取最新完整帧：1=有新帧（取走即清），0=无
 ************************************************************************************************************/
u8 Voice_GetFrame(VoiceFrame *f)
{
  if (!g_vnew) return 0;
  *f = g_vresult;
  g_vnew = 0;
  return 1;
}

#endif /* VOICE_ENABLE */

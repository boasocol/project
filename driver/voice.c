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
#include <stdio.h>
/* P2（2026-09-03）依赖反转：本驱动不再 include 应用层头（ui.h/screen.h/time_module.h），
   命令业务由应用层 app_voice.c 注册的回调处理。
   2026-09-05：语音模块用 UART0(PB7/PB8)、蓝牙用 USART0(PC4/PC5)，外设独立可共存——
   启用条件只受 VOICE_ENABLE 控制（原 && !HD9803_ENABLE 互斥已解除）；蓝牙模式只屏蔽
   printf（USART0 归蓝牙，见 ht32_serial.c SERIAL_PutChar stub），语音模块照常工作。 */

#if VOICE_ENABLE

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

/* ---- 业务回调（P2 反转 2026-09-03）：协议驱动不碰业务，业务在 app_voice.c ---- */
static VoiceCmdHandler g_vcmd = 0;

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
 * 帧分发（P2 反转 2026-09-03）：协议驱动只解析帧，业务由应用层注册的回调处理。
 * 命令业务（设时/设日/设闹钟/报时/唤醒/状态/音量/倒计时/闹钟…）已迁到 app_voice.c 的
 * App_VoiceCmd（含 Screen_HandleVoice 分发 + 各 vcmd_* 处理 + 未接线回 FAIL）。
 ************************************************************************************************************/
void Voice_HandleFrame(const VoiceFrame *f)
{
  if (g_vcmd)
    (void)g_vcmd(f);
  else
  {
    printf("[VOICE] 无业务处理器，回 FAIL\r\n");
    Voice_Send(VOICE_TX_FAIL, 0, 0, 0, 0);
  }
}

void Voice_SetCmdHandler(VoiceCmdHandler h)
{
  g_vcmd = h;
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

  {
    CKCU_PeripClockConfig_TypeDef clk = {{0}};
    clk.Bit.VOICE_PWR_GPIO_CLK     = 1;
    CKCU_PeripClockConfig(clk, ENABLE);
  }
  GPIO_DirectionConfig(VOICE_PWR_GPIO, VOICE_PWR_PIN, GPIO_DIR_OUT);
  GPIO_SetOutBits(VOICE_PWR_GPIO,VOICE_PWR_PIN);



  printf("[VOICE] init OK: PB7=TX PB8=RX @%u\r\n", (unsigned)VOICE_BAUDRATE);
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
        Voice_HandleFrame(&f);   /* 固定帧：路由到业务回调（app_voice.c，P2 反转 2026-09-03） */
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

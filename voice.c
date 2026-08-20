/************************************************************************************************************
 * @file    voice.c
 * @brief   CI-D0XGS02S 语音识别模块驱动（UART0：PB7=TX, PB8=RX，波特率 9600）
 * @note    - 占用 UART0（PB7/PB8，原 printf 调试口）；printf 改走 USART0(PC4/PC5)
 *          - 帧格式（语音时钟 V10）：[H0][H1][ID][CMD][d0..d3][CS][FB]，H0H1=A5FA|FAF1
 *          - 接收中断把字节入环形缓冲，Voice_Process 在主循环做状态机解析
 ************************************************************************************************************/
#include "voice.h"
#include "hd9803.h"   /* 取 HD9803_ENABLE，与蓝牙互斥占 USART0 */
#include "BFTM0.h"    /* bftm0_ct：1ms 计数器，诊断打印用 */
#include <stdio.h>

#if VOICE_ENABLE && !HD9803_ENABLE

/* ---- 环形接收缓冲（ISR 写入，主循环读取） ---- */
static volatile u8  g_vrx_buf[VOICE_RX_BUF_SIZE];
static volatile u16 g_vrx_head = 0;   /* ISR 写入指针 */
static volatile u16 g_vrx_tail = 0;   /* poll 读取指针 */

/* ---- 解析状态机（语音时钟 V10：固定 10 字节帧） ---- */
typedef enum
{
  VS_HEAD0 = 0,   /* 等帧头第 1 字节（A5 或 FA） */
  VS_HEAD1,       /* 等帧头第 2 字节（FA 或 F1） */
  VS_ID,          /* 等 ID */
  VS_CMD,         /* 等 CMD */
  VS_D0, VS_D1, VS_D2, VS_D3,  /* 指令内容 4 字节 */
  VS_CS,          /* 等校验和 */
  VS_TAIL,        /* 等帧尾 0xFB */
} VoiceState;

static u8    g_vstate = VS_HEAD0;
static u8    g_vh0, g_vh1;         /* 帧头（A5FA 或 FAF1） */
static u8    g_vfrm[8];            /* ID CMD D0 D1 D2 D3 CS（校验前 6 字节累加） */
static u8    g_vpos = 0;           /* 已收 [ID..D3] 字节数 */
static u8    g_vcs = 0;            /* 本地累加和 */

/* ---- 解析结果 ---- */
static VoiceFrame g_vresult;
static u8 g_vnew = 0;

/* ---- 诊断：统计收字节/成功帧，并保留最近 16 个原始字节 ---- */
static u32 g_rx_total = 0;
static u8  g_rx_last[16];
static u8  g_rx_last_n = 0;
static u32 g_frames_ok = 0;

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
    g_rx_total++;
    g_rx_last[g_rx_last_n] = d;
    g_rx_last_n = (u8)((g_rx_last_n + 1) % 16);
  }
}

/* ------------------------------------------------------------------
 * 状态机：喂一个字节，返回 1=完整帧解析完成
 * 帧：[H0][H1][ID][CMD][d0][d1][d2][d3][CS][FB]
 * 校验：CS = (u8)(H0+H1+ID+CMD+d0+d1+d2+d3)
 * 帧头兼容：A5 FA 与 FA F1 两种变体
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
      if ((g_vh0 == VOICE_FRM_HEAD_A0 && d == VOICE_FRM_HEAD_A1) ||
          (g_vh0 == VOICE_FRM_HEAD_B0 && d == VOICE_FRM_HEAD_B1))
      {
        g_vh1 = d;
        g_vpos = 0;
        g_vcs  = (u8)(g_vh0 + g_vh1);
        g_vstate = VS_ID;
      }
      else if (d == VOICE_FRM_HEAD_A0 || d == VOICE_FRM_HEAD_B0)
      {
        g_vh0 = d;                     /* 连续帧头 */
      }
      else
        g_vstate = VS_HEAD0;
      break;

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

    case VS_CS:
      g_vstate = VS_TAIL;
      if (d != g_vcs)                 /* 校验错：等新头 */
        g_vstate = VS_HEAD0;
      break;

    case VS_TAIL:
      g_vstate = VS_HEAD0;
      if (d == VOICE_FRM_TAIL)
      {
        g_vresult.head[0] = g_vh0;
        g_vresult.head[1] = g_vh1;
        g_vresult.id  = g_vfrm[0];
        g_vresult.cmd = g_vfrm[1];
        g_vresult.d0  = g_vfrm[2];
        g_vresult.d1  = g_vfrm[3];
        g_vresult.d2  = g_vfrm[4];
        g_vresult.d3  = g_vfrm[5];
        g_vnew = 1;
        return 1;
      }
      break;
  }
  return 0;
}

/************************************************************************************************************
 * USART 初始化（PC4/PC5 -> USART, 9600）
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

  printf("[VOICE] init OK: PB7=TX PB8=RX @9600\r\n");
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

  /* [调试] 打印收到的完整语音帧（验证通讯用，后续按需替换为动作映射） */
  {
    VoiceFrame f;
    if (Voice_GetFrame(&f))
    {
      g_frames_ok++;
      printf("[VOICE] cmd=0x%02X d0=0x%02X d1=0x%02X d2=0x%02X d3=0x%02X\r\n",
             f.cmd, f.d0, f.d1, f.d2, f.d3);
    }
  }

  /* [诊断] 每 2 秒打印接收统计：rx=收字节总数 frames=成功帧数 last=最近16字节hex
     帮助区分：rx=0 -> 硬件无信号（接线/供电/波特率）；rx>0 frames=0 -> 协议/校验；
     frames>0 -> 通讯正常。 */
  {
    static u32 t_diag = 0;
    u32 now = (u32)bftm0_ct;
    if (now - t_diag >= 2000u)
    {
      t_diag = now;
      printf("[VOICE] diag: rx=%u frames=%u last=", (unsigned)g_rx_total, (unsigned)g_frames_ok);
      if (g_rx_total)
      {
        u8 i = g_rx_last_n;                    /* 最早字节在 last_n */
        for (u8 k = 0; k < 16; k++)
        {
          printf("%02X ", g_rx_last[i]);
          i = (u8)((i + 1) % 16);
        }
      }
      printf("\r\n");
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

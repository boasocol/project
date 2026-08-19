/************************************************************************************************************
 * @file    voice.c
 * @brief   CI-D0XGS02S 语音识别模块驱动（USART：PC4=TX, PC5=RX，波特率 9600）
 * @note    - 占用 UART0（PB7/PB8，原 printf 调试口）；printf 改走 USART0(PC4/PC5)
 *          - 帧格式：A5 FC | LEN(2B LE) | TYPE | CMD | SEQ | DATA[LEN] | SUM(2B LE) | FB
 *            SUM = TYPE..DATA 逐字节 16 位累加（LE 输出），不含头/长度/尾
 *          - 接收中断把字节入环形缓冲，Voice_Process 在主循环做状态机解析
 ************************************************************************************************************/
#include "voice.h"
#include "hd9803.h"   /* 取 HD9803_ENABLE，与蓝牙互斥占 USART0 */
#include <stdio.h>

#if VOICE_ENABLE && !HD9803_ENABLE

/* ---- 环形接收缓冲（ISR 写入，主循环读取） ---- */
static volatile u8  g_vrx_buf[VOICE_RX_BUF_SIZE];
static volatile u16 g_vrx_head = 0;   /* ISR 写入指针 */
static volatile u16 g_vrx_tail = 0;   /* poll 读取指针 */

/* ---- 解析状态机 ---- */
typedef enum
{
  VS_HEAD0 = 0,   /* 等 0xA5 */
  VS_HEAD1,       /* 等 0xFC */
  VS_LEN_LO,      /* 等长度低字节 */
  VS_LEN_HI,      /* 等长度高字节 */
  VS_DATA,        /* 收 TYPE/CMD/SEQ/DATA */
  VS_SUM_LO,      /* 等校验低字节 */
  VS_SUM_HI,      /* 等校验高字节 */
  VS_TAIL,        /* 等帧尾 0xFB */
} VoiceState;

static u8    g_vstate   = VS_HEAD0;
static u16   g_vlen     = 0;        /* 本次帧 DATA 长度 */
static u16   g_vpos     = 0;        /* 已收 (TYPE+CMD+SEQ+DATA) 字节数 */
static u8    g_vframe[3 + VOICE_MAX_DATA];  /* TYPE CMD SEQ DATA */
static u16   g_vsum     = 0;        /* 累加和（校验段） */
static u16   g_vsum_rx  = 0;        /* 收到的校验值 */

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
 * ------------------------------------------------------------------ */
static u8 voice_feed(u8 d)
{
  switch (g_vstate)
  {
    case VS_HEAD0:
      if (d == VOICE_FRM_HEAD0) g_vstate = VS_HEAD1;
      break;

    case VS_HEAD1:
      if (d == VOICE_FRM_HEAD1) g_vstate = VS_LEN_LO;
      else if (d == VOICE_FRM_HEAD0) g_vstate = VS_HEAD1;   /* 连续头 */
      else g_vstate = VS_HEAD0;
      break;

    case VS_LEN_LO:
      g_vlen = d;
      g_vstate = VS_LEN_HI;
      break;

    case VS_LEN_HI:
      g_vlen |= (u16)(d << 8);
      if (g_vlen > VOICE_MAX_DATA)
      {
        /* 超长帧：丢弃，等新头 */
        g_vstate = VS_HEAD0;
        break;
      }
      g_vpos = 0;
      g_vsum = 0;
      g_vstate = VS_DATA;
      break;

    case VS_DATA:
      if (g_vpos < sizeof(g_vframe))
        g_vframe[g_vpos] = d;
      g_vsum += d;                       /* 校验段累加：TYPE..DATA */
      g_vpos++;
      if (g_vpos >= 3u + g_vlen)         /* TYPE+CMD+SEQ+DATA 收满 */
        g_vstate = VS_SUM_LO;
      break;

    case VS_SUM_LO:
      g_vsum_rx = d;
      g_vstate = VS_SUM_HI;
      break;

    case VS_SUM_HI:
      g_vsum_rx |= (u16)(d << 8);
      g_vstate = VS_TAIL;
      break;

    case VS_TAIL:
      g_vstate = VS_HEAD0;
      if (d == VOICE_FRM_TAIL && g_vsum == g_vsum_rx && g_vpos >= 3u)
      {
        g_vresult.type = g_vframe[0];
        g_vresult.cmd  = g_vframe[1];
        g_vresult.seq  = g_vframe[2];
        g_vresult.len  = (u8)(g_vpos - 3u);
        for (u8 i = 0; i < g_vresult.len; i++)
          g_vresult.data[i] = g_vframe[3u + i];
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

  /* 3) 引脚复用：PC4=USART_TX, PC5=USART_RX */
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

  printf("[VOICE] USART init OK: PC4=TX PC5=RX @9600\r\n");
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

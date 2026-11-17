/************************************************************************************************************
 * @file    hd9803.c
 * @brief   HD9803 蓝牙授时模块驱动（USART：PC4=TX, PC5=RX，波特率 9600）
 *
 * 功能：
 *   - USART 初始化（PC4/PC5 复用为 USART，9600, 8N1）
 *   - 接收中断（USART_IRQHandler）将字节存入环形缓冲
 *   - Hd9803_Poll() 解析 HD9803 协议帧，提取授时时间
 *   - 通过 UART0 printf 打印模块工作状态（初始化/型号/授时时间/错误）
 *
 * 注意：UART0（PB7/PB8, printf）用于调试输出，与蓝牙 USART 互不冲突。
 ************************************************************************************************************/
#include "hd9803.h"
#include <stdio.h>
#include <string.h>

#if HD9803_ENABLE   /* ======== 蓝牙模块启用 ======== */

/* P2-3（2026-09-03）分层反转：本模块不再 include time_module.h / calendar.h，
   收到新授时时间的应用层副作用（校准 RTC + 界面静态刷新）经 Hd9803_SetTimeSink
   注册的 sink 上抛，由应用层（main.c）执行。TimeStruct 类型经 hd9803.h 间接获得。 */
static Hd9803TimeSink g_time_sink = 0;   /* 授时时间到达回调（应用层注册，可空） */

void Hd9803_SetTimeSink(Hd9803TimeSink sink) { g_time_sink = sink; }

/* ---- 环形接收缓冲 ---- */
#define HD9803_RX_BUF_SIZE      64u
static volatile u8  g_rx_buf[HD9803_RX_BUF_SIZE];
static volatile u16 g_rx_head = 0;   /* ISR 写入指针 */
static volatile u16 g_rx_tail = 0;   /* poll 读取指针 */

/* ---- 解析结果 ---- */
static u8            g_time_valid = 0;
static u8            g_time_updated = 0;
static Hd9803Time    g_time;

/* ---- 状态机 ---- */
static Hd9803State   g_state = HD9803_ST_OFF;   /* 当前状态 */
static u32           g_window_ms = 0;           /* 窗口剩余时间(ms) */
static u8            g_req_scan = 0;            /* 请求上电发现窗口 */
static u8            g_req_link = 0;            /* 请求回连窗口 */

/* 环形缓冲是否非空 */
static u8 rx_avail(void)
{
  return (u8)(g_rx_head != g_rx_tail);
}

static u8 rx_get(void)
{
  u8 d = g_rx_buf[g_rx_tail];
  g_rx_tail = (u16)((g_rx_tail + 1) % HD9803_RX_BUF_SIZE);
  return d;
}

/* 状态机内部：窗口内收到授时时间 -> 标记已连接并立即断电 */
static void sm_mark_connected(void)
{
  g_state = HD9803_ST_CONNECTED;
  g_window_ms = 0;
  Hd9803_SetPower(0);
  printf("[HD9803] state: CONNECTED (power off)\r\n");
}

/* ---- 简单忙等延时（基于 SystemCoreClock，48MHz 下约 3 周期/轮） ---- */
static void hd9803_delay_ms(volatile u32 ms)
{
  volatile u32 n = ms * (SystemCoreClock / 1000u) / 3u;
  while (n--);
}

/************************************************************************************************************
 * 模块电源控制（高有效，经外部三极管/MOS 开关给模块 VCC 供电）
 ************************************************************************************************************/
void Hd9803_SetPower(u8 on)
{
  /* 使能电源控制脚所在端口时钟（HD9803_PWR_GPIO_CLK：PA） */
  {
    CKCU_PeripClockConfig_TypeDef clk = {{0}};
    clk.Bit.HD9803_PWR_GPIO_CLK = 1;
    CKCU_PeripClockConfig(clk, ENABLE);
  }

  /* 配置为推挽输出，保持输出值 */
  GPIO_DirectionConfig(HD9803_PWR_GPIO, HD9803_PWR_PIN, GPIO_DIR_OUT);

#if (HD9803_PWR_ON_LEVEL == 1)
  if (on) GPIO_SetOutBits(HD9803_PWR_GPIO, HD9803_PWR_PIN);   /* 高=上电 */
  else    GPIO_ClearOutBits(HD9803_PWR_GPIO, HD9803_PWR_PIN); /* 低=断电 */
#else
  if (on) GPIO_ClearOutBits(HD9803_PWR_GPIO, HD9803_PWR_PIN);
  else    GPIO_SetOutBits(HD9803_PWR_GPIO, HD9803_PWR_PIN);
#endif

  /* ---- 通信脚 PC4(TX)/PC5(RX) 空闲低电平，杜绝断电后反灌漏电 ----
     模块断电后，若 TX/RX 仍保持 USART 复用或高电平，会经模块内部 ESD/保护
     二极管向已断电的 VCC 反灌电流，造成"假断电"漏电。
     故断电(on=0)时把 PC4/PC5 切回 GPIO 推挽输出低；上电(on=1)时恢复
     USART 复用（此时 Hd9803_Init 已配好串口；若尚未配置，则保持 GPIO 低，
     等 Init 后续的 AFIO_GPxConfig 完成复用切换，此处幂等无副作用）。 */
  {
    CKCU_PeripClockConfig_TypeDef clk = {{0}};
    clk.Bit.PC = 1;
    CKCU_PeripClockConfig(clk, ENABLE);

    if (!on)
    {
      /* 断电：切回 GPIO 推挽输出低 */
      AFIO_GPxConfig(HD9803_GPIO_ID, HD9803_TX_AFIO_PIN, AFIO_FUN_GPIO);
      AFIO_GPxConfig(HD9803_GPIO_ID, HD9803_RX_AFIO_PIN, AFIO_FUN_GPIO);
      GPIO_DirectionConfig(HD9803_GPIO_PORT,
                           (u16)(HD9803_TX_AFIO_PIN | HD9803_RX_AFIO_PIN), GPIO_DIR_OUT);
      GPIO_ClearOutBits(HD9803_GPIO_PORT,
                        (u16)(HD9803_TX_AFIO_PIN | HD9803_RX_AFIO_PIN));
    }
    else
    {
      /* 上电：恢复 USART 复用（若 Init 已配置则幂等；未配置也不影响后续 Init） */
      AFIO_GPxConfig(HD9803_GPIO_ID, HD9803_TX_AFIO_PIN, AFIO_FUN_USART_UART);
      AFIO_GPxConfig(HD9803_GPIO_ID, HD9803_RX_AFIO_PIN, AFIO_FUN_USART_UART);
    }
  }
}

/************************************************************************************************************
 * USART 初始化（PC4/PC5 -> USART, 9600）
 ************************************************************************************************************/
void Hd9803_Init(void)
{
  /* 0) 先给模块上电，等待就绪后再配串口（窗口结束后 Tick 会自动断电） */
  Hd9803_SetPower(1);
  hd9803_delay_ms(HD9803_PWR_ON_DELAY_MS);

  /* 1) 使能时钟：AFIO、USART、GPIO_C */
  {
    CKCU_PeripClockConfig_TypeDef clk = {{0}};
    clk.Bit.AFIO   = 1;
    clk.Bit.USART0 = 1;
    clk.Bit.PC     = 1;
    CKCU_PeripClockConfig(clk, ENABLE);
  }

  /* 2) RX 脚内部上拉，避免悬空 */
  GPIO_PullResistorConfig(HD9803_GPIO_PORT, HD9803_RX_AFIO_PIN, GPIO_PR_UP);

  /* 3) 引脚复用：PC4=USART_TX, PC5=USART_RX（第一参数是端口编号 GPIO_PC） */
  AFIO_GPxConfig(HD9803_GPIO_ID, HD9803_TX_AFIO_PIN, AFIO_FUN_USART_UART);
  AFIO_GPxConfig(HD9803_GPIO_ID, HD9803_RX_AFIO_PIN, AFIO_FUN_USART_UART);

  /* 4) USART 9600, 8N1 */
  {
    USART_InitTypeDef us = {0};
    us.USART_BaudRate   = HD9803_BAUDRATE;
    us.USART_WordLength = USART_WORDLENGTH_8B;
    us.USART_StopBits   = USART_STOPBITS_1;
    us.USART_Parity     = USART_PARITY_NO;
    us.USART_Mode       = USART_MODE_NORMAL;
    USART_Init(HD9803_UART, &us);
  }

  /* 5) NVIC + 接收中断 + 收发使能 */
  NVIC_EnableIRQ(HD9803_UART_IRQn);
  USART_IntConfig(HD9803_UART, USART_INT_RXDR, ENABLE);
  USART_TxCmd(HD9803_UART, ENABLE);
  USART_RxCmd(HD9803_UART, ENABLE);

  g_time_valid   = 0;
  g_time_updated = 0;
  g_rx_head = g_rx_tail = 0;
  g_state    = HD9803_ST_OFF;
  g_window_ms = 0;
  g_req_scan = g_req_link = 0;

  printf("[HD9803] USART init OK: PC4=TX PC5=RX @9600\r\n");
}

/************************************************************************************************************
 * 状态机：请求上电发现窗口（上电时由 main 调用一次）
 *   上电 -> 发"回连+可发现"(0x08 0x10) -> 进入 SCAN，窗口 20s
 ************************************************************************************************************/
void Hd9803_RequestScan(void)
{
  if (!HD9803_ENABLE) return;
  g_req_scan = 1;
}

/************************************************************************************************************
 * 状态机：请求回连窗口（每日 8/18 点由 main 调用）
 *   仅回连(0x08 0x02)，不发可发现；已连接失败则降级未连接
 ************************************************************************************************************/
void Hd9803_RequestRelink(void)
{
  if (!HD9803_ENABLE) return;
  g_req_link = 1;
}

/************************************************************************************************************
 * 每秒心跳：驱动窗口计时 / 电源开关 / 状态转换
 *   one_sec = 1 表示这一秒到点（由 main 的 1s 周期调用）
 ************************************************************************************************************/
void Hd9803_Tick(u8 one_sec)
{
  /* 处理待处理请求：先上电、发指令、进入对应窗口 */
  if (g_req_scan)
  {
    g_req_scan = 0;
    Hd9803_SetPower(1);
    hd9803_delay_ms(HD9803_PWR_ON_DELAY_MS);
    Hd9803_EnterPairRelinkMode();          /* 0x08 0x10: 回连+可发现 */
    g_state = HD9803_ST_SCAN;
    g_window_ms = HD9803_WINDOW_SCAN_MS;
    printf("[HD9803] state: SCAN (20s discover+relink)\r\n");
  }
  else if (g_req_link)
  {
    g_req_link = 0;
    Hd9803_SetPower(1);
    hd9803_delay_ms(HD9803_PWR_ON_DELAY_MS);
    Hd9803_EnterRelinkMode();              /* 0x08 0x02: 仅回连 */
    g_state = HD9803_ST_LINK;
    g_window_ms = HD9803_WINDOW_LINK_MS;
    printf("[HD9803] state: LINK (20s relink)\r\n");
  }

  /* 窗口计时（SCAN / LINK 时每秒扣减） */
  if (!one_sec) return;
  if (g_state == HD9803_ST_SCAN || g_state == HD9803_ST_LINK)
  {
    if (g_window_ms > 1000u) g_window_ms -= 1000u;
    else g_window_ms = 0;

    if (g_window_ms == 0)
    {
      /* 窗口超时未收到授时 -> 未连接，断电 */
      Hd9803_SetPower(0);
      g_state = HD9803_ST_DISCONNECTED;
      printf("[HD9803] state: DISCONNECTED (timeout, power off)\r\n");
    }
  }
}

/************************************************************************************************************
 * USART 接收中断：一个字节入环形缓冲
 ************************************************************************************************************/
void HD9803_UART_IRQHandler(void)
{
  if (USART_GetFlagStatus(HD9803_UART, USART_FLAG_RXDR))
  {
    u8 d = USART_ReceiveData(HD9803_UART);
    u16 n = (u16)((g_rx_head + 1) % HD9803_RX_BUF_SIZE);
    if (n != g_rx_tail)                 /* 非满才写入 */
    {
      g_rx_buf[g_rx_head] = d;
      g_rx_head = n;
    }
  }
}

/************************************************************************************************************
 * 帧解析：验证头/尾/校验，提取授时时间
 ************************************************************************************************************/
static void hd9803_parse_frame(const u8 *f, u8 len)
{
  u8 sum, i, plen;
  u8 cmd;
  const u8 *pay;

  if (len < 5) return;
  if (f[0] != HD9803_FRM_HEAD)  { printf("[HD9803] bad head 0x%02X\r\n", f[0]); return; }
  if (f[len-1] != HD9803_FRM_TAIL){ printf("[HD9803] bad tail\r\n"); return; }

  plen = f[1];
  if ((u16)plen + 3u != len) { printf("[HD9803] bad len plen=%u frame=%u\r\n", plen, len); return; }

  sum = 0;
  for (i = 1; i + 1 < len; i++) sum ^= f[i];
  if (sum != 0) { printf("[HD9803] bad checksum\r\n"); return; }

  cmd = f[2];
  pay = &f[3];

  switch (cmd)
  {
    case HD9803_CMD_MODEL:                /* 0x11 模块型号（模块->MCU） */
      if (plen >= 2)
      {
        /* 打印型号字符串（可能非 0 结尾，限长打印） */
        u8 j, n = plen - 1;               /* 去掉命令字后的型号长度 */
        if (n > 15) n = 15;
        printf("[HD9803] model: ");
        for (j = 0; j < n; j++) printf("%c", pay[j]);
        printf("\r\n");
      }
      break;

    case HD9803_CMD_TIME:                 /* 0x15 下发时间（模块->MCU） */
      if (plen == 8)                      /* 命令字+6时间+校验 */
      {
        /* 注意：HD9803 时间字段为【普通二进制】（非 BCD）！
           实测 year=0x1A(26)、second=0x0F(15) 均为非法 BCD 但作为
           普通十进制完全合理，故直接按十进制值使用。 */
        g_time.year   = pay[0];           /* 年（后两位，如 26 = 2026） */
        g_time.month  = pay[1];
        g_time.day    = pay[2];
        g_time.hour   = pay[3];
        g_time.minute = pay[4];
        g_time.second = pay[5];
        g_time_valid   = 1;
        g_time_updated = 1;
        printf("[HD9803] time: 20%02u-%02u-%02u %02u:%02u:%02u\r\n",
               g_time.year, g_time.month, g_time.day,
               g_time.hour, g_time.minute, g_time.second);

        /* 收到授时 -> 判定已连接（SCAN 或 LINK 窗口内都算） */
        if (g_state == HD9803_ST_SCAN || g_state == HD9803_ST_LINK)
          sm_mark_connected();
      }
      else
      {
        printf("[HD9803] time frame plen=%u\r\n", plen);
      }
      break;

    case HD9803_CMD_PAIR:                 /* 0x08 配对/回连模式（模块原样返回确认） */
      if (plen >= 2)
      {
        u8 mode = pay[0];
        printf("[HD9803] pair ack, mode=0x%02X\r\n", mode);
        if (mode == 0x01)      printf("[HD9803]   -> pairing mode (discoverable)\r\n");
        else if (mode == 0x02) printf("[HD9803]   -> relink last device\r\n");
        else if (mode == 0x10) printf("[HD9803]   -> relink + discoverable\r\n");
      }
      break;

    default:
      printf("[HD9803] cmd 0x%02X plen=%u\r\n", cmd, plen);
      break;
  }
}

/************************************************************************************************************
 * 主循环调用：状态机解析
 ************************************************************************************************************/
void Hd9803_Poll(void)
{
  static u8 f[64];
  static u8 flen = 0;
  static u8 state = 0;

  while (rx_avail())
  {
    u8 d = rx_get();

    switch (state)
    {
      case 0:   /* 等帧头 */
        if (d == HD9803_FRM_HEAD) { f[0] = d; flen = 1; state = 1; }
        break;
      case 1:   /* 读数据长度 */
        f[1] = d; flen = 2; state = 2;
        break;
      case 2:   /* 收内容直到帧尾 */
        if (flen < sizeof(f)) f[flen++] = d;
        if (d == HD9803_FRM_TAIL)
        {
          hd9803_parse_frame(f, flen);
          state = 0; flen = 0;
        }
        break;
      default:
        state = 0; flen = 0;
        break;
    }
  }
}

/************************************************************************************************************
 * 对外 API
 ************************************************************************************************************/
u8 Hd9803_GetTime(Hd9803Time *t)
{
  if (!t) return 0;
  *t = g_time;
  if (g_time_updated) { g_time_updated = 0; return 1; }  /* 返回 1 表示是新时间 */
  return (u8)(g_time_valid ? 2 : 0);                      /* 2 = 已有旧时间，0 = 无 */
}

Hd9803State Hd9803_GetState(void) { return g_state; }

/************************************************************************************************************
 * 主循环调用：解析串口帧 + 若收到新授时时间则上抛给应用层 sink（校准 RTC + 静态刷新由应用层做）
 ************************************************************************************************************/
void Hd9803_Process(void)
{
  Hd9803_Poll();
  Hd9803Time bt;
  if (Hd9803_GetTime(&bt) != 1) return;   /* 1 = 收到新授时时间 */

  /* 上抛给应用层：校准 RTC + 触发静态刷新（P2-3 反转，原 TimeModule_SetTime + Calendar_SetStaticNext） */
  if (g_time_sink) g_time_sink(&bt);
}

/************************************************************************************************************
 * 每秒心跳（main 在 g_time_update_flag 时调用一次）：驱动状态机 + 每日 8:00/18:00 整点回连校准
 *   原本这段逻辑散落在 main.c 每秒分支中，现收敛进本模块。
 ************************************************************************************************************/
void Hd9803_OnSecondTick(const TimeStruct *now)
{
  Hd9803_Tick(1);   /* 状态机推进：窗口计时 / 电源开关 */

  /* 已连接状态：每日 8:00 / 18:00 整点自动回连一次校准时间
     （回连失败由状态机内部降级为未连接） */
  if (Hd9803_GetState() == HD9803_ST_CONNECTED &&
      now->minute == 0 && (now->hour == 8 || now->hour == 18))
  {
    static u8 last_trig_h = 0xFF;
    if (last_trig_h != now->hour)
    {
      last_trig_h = now->hour;
      printf("[HD9803] %02u:00 trigger daily relink\r\n", now->hour);
      Hd9803_RequestRelink();
    }
  }
}

/* 发送单帧：0x24 + len + payload + 校验 + 0x42 */
static void hd9803_send_frame(const u8 *payload, u8 plen)
{
  u8 i, sum = 0;

  USART_SendData(HD9803_UART, HD9803_FRM_HEAD);
  while (USART_GetFlagStatus(HD9803_UART, USART_FLAG_TXDE) == RESET);

  USART_SendData(HD9803_UART, (u8)(plen + 1));            /* 数据长度 = plen + 校验位 */
  while (USART_GetFlagStatus(HD9803_UART, USART_FLAG_TXDE) == RESET);
  sum ^= (u8)(plen + 1);

  for (i = 0; i < plen; i++)
  {
    USART_SendData(HD9803_UART, payload[i]);
    while (USART_GetFlagStatus(HD9803_UART, USART_FLAG_TXDE) == RESET);
    sum ^= payload[i];
  }

  USART_SendData(HD9803_UART, sum);
  while (USART_GetFlagStatus(HD9803_UART, USART_FLAG_TXDE) == RESET);

  USART_SendData(HD9803_UART, HD9803_FRM_TAIL);
  while (USART_GetFlagStatus(HD9803_UART, USART_FLAG_TXDE) == RESET);
}

void Hd9803_SetName(const u8 *name, u8 len)
{
  u8 pay[16];
  if (len > 15) len = 15;
  pay[0] = HD9803_CMD_SET_NAME;
  memcpy(&pay[1], name, len);
  printf("[HD9803] send set_name len=%u\r\n", len);
  hd9803_send_frame(pay, (u8)(1 + len));
}

void Hd9803_EnterPairMode(void)
{
  u8 pay[2] = { HD9803_CMD_PAIR, 0x01 };
  printf("[HD9803] enter pair mode\r\n");
  hd9803_send_frame(pay, 2);
}

void Hd9803_EnterRelinkMode(void)
{
  u8 pay[2] = { HD9803_CMD_PAIR, 0x02 };
  printf("[HD9803] enter relink mode\r\n");
  hd9803_send_frame(pay, 2);
}

void Hd9803_EnterPairRelinkMode(void)
{
  u8 pay[2] = { HD9803_CMD_PAIR, 0x10 };
  printf("[HD9803] enter pair+relink mode\r\n");
  hd9803_send_frame(pay, 2);
}

#endif /* HD9803_ENABLE */

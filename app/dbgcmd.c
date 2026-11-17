/************************************************************************************************************
 * @file    dbgcmd.c
 * @brief   串口调试指令模拟语音输入（printf 口 USART0 PC4/PC5 @115200）
 *
 * 用法（串口助手 115200 8N1，三种输入方式）：
 *   A. 简化格式（文本）：<cmd> [d0] [d1] [d2] [d3] + 回车
 *      例：8A 1 07 30   -> 设闹钟1 07:30（等价语音"设置闹钟1七点三十"）
 *          81 07 30     -> 设时钟 07:30       84 25   -> 25 分钟倒计时
 *          85/86/87     -> 暂停/继续/结束     88      -> 番茄
 *          8B 1 / 8B 0  -> 闹钟开/关          8C 1    -> 循环一~五
 *          B0/B1        -> 贪睡/别响          B6      -> 报时
 *   B. 完整帧文本（ASCII）：FA F1 id cmd d0 d1 d2 d3 cs FB + 回车，自动识别；校验不符警告仍执行
 *   C. HEX 模式直发（推荐模拟语音模块）：串口助手切 HEX，发 FA F1 id cmd d0 d1 d2 d3 cs FB
 *      凑齐 10 字节立即执行、无需回车（帧中 0x00 不受字符串截断影响）
 *   help / ?        -> 打印命令表
 *
 * 实现：USART0 RX 中断收字节入环形缓冲；DbgCmd_Process 主循环解析：
 *       - 回显做了格式转换（2026-08-28）：HEX 帧内字节一律转两位 hex 文本（如 0x0A → "0A"、
 *         0x75 → "75"，不混成字符）、文本输入可打印 ASCII 原样，HEX 直发帧与文本混显不再乱码；
 *       - HEX 帧内控制字节不当编辑键（2026-08-28）：0x0D/0x0A 不当换行、0x08/0x7F 不当退格
 *         （设时钟 81 0E 0D 的 d1=0x0D、设闹钟 17:08 的 d2=0x08 都踩中，帧被截断/计数错乱
 *         导致不执行、后续命令全失效）；
 *       - HEX 二进制完整帧（FA F1 id cmd d0..d3 cs FB，或 0xB7 例外的 A5 FA ... FB）凑齐
 *         10 字节立即执行、无需回车，字节级判断避开帧中 0x00 被当字符串终止符截断；
 *       - 文本命令行（cmd d0 d1 d2 d3，或完整帧文本）回车触发解析。
 *       两者都构造 VoiceFrame 调 Voice_HandleFrame。与语音模块（UART0 PB7/PB8）互不干扰。
 * 启用条件 = VOICE_ENABLE && !HD9803_ENABLE（HD9803_ENABLE=1 时 USART0 归蓝牙，不能作调试口）。
 ************************************************************************************************************/
#include "dbgcmd.h"
#include "voice.h"
#include "hd9803.h"   /* HD9803_ENABLE：蓝牙启用时 USART0 归蓝牙，本模块不编译（防 ISR 冲突） */
#include <stdio.h>
#include <string.h>

#if VOICE_ENABLE && !HD9803_ENABLE

#define DBGCMD_UART       HT_USART0
#define DBGCMD_IRQn       USART0_IRQn
#define DBGCMD_IRQHandler USART0_IRQHandler

#define DBG_RING_SIZE  128u   /* 接收环形缓冲 */
#define DBG_LINE_MAX   64u    /* 单行命令最长 */

/* ---- 环形接收缓冲（ISR 写，主循环读） ---- */
static volatile u8  g_dbg_ring[DBG_RING_SIZE];
static volatile u16 g_dbg_head = 0;
static volatile u16 g_dbg_tail = 0;
static u8  g_dbg_line[DBG_LINE_MAX];   /* 当前行缓冲 */
static u8  g_dbg_n = 0;

/* ------------------------------------------------------------------
 * USART0 接收中断：只收字节入环形缓冲（回显移主循环 DbgCmd_Process，
 * 需做"控制字节→hex 文本"格式转换，ISR 里不便忙等发送）
 * ------------------------------------------------------------------ */
void DBGCMD_IRQHandler(void)
{
  if (USART_GetFlagStatus(DBGCMD_UART, USART_FLAG_RXDR) != RESET)
  {
    u8 d = (u8)USART_ReceiveData(DBGCMD_UART);
    u16 n = (u16)((g_dbg_head + 1) % DBG_RING_SIZE);
    if (n != g_dbg_tail)
    {
      g_dbg_ring[g_dbg_head] = d;
      g_dbg_head = n;
    }
  }
}

/* ------------------------------------------------------------------
 * 初始化：使能 USART0 RX 中断（须在 UART0_Configuration 之后调用）
 * ------------------------------------------------------------------ */
void DbgCmd_Init(void)
{
  g_dbg_head = g_dbg_tail = 0;
  g_dbg_n = 0;
  NVIC_EnableIRQ(DBGCMD_IRQn);
  USART_IntConfig(DBGCMD_UART, USART_INT_RXDR, ENABLE);
}

/* ---- hex 字符值；非法返回 -1 ---- */
static int dbg_hexval(char c)
{
  if (c >= '0' && c <= '9') return (int)(c - '0');
  if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
  if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
  return -1;
}

/* 解析 1-2 位 hex token（可带 0x 前缀）；成功返回 1 */
static int dbg_parse_hex(const char *s, u8 *out)
{
  int v = 0, n = 0;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
  for (; *s; s++)
  {
    int h = dbg_hexval(*s);
    if (h < 0 || n >= 2) return 0;
    v = v * 16 + h;
    n++;
  }
  if (n == 0) return 0;
  *out = (u8)v;
  return 1;
}

static void dbg_help(void)
{
  printf("\r\n[DBGCMD] 串口模拟语音指令，两种格式（hex，空格分隔，回车执行）\r\n");
  printf("  A 简化：<cmd> [d0] [d1] [d2] [d3]\r\n");
  printf("          80 唤醒 / 81 hh mm 设时钟 / 82 y m d 设日期 / 84 min 倒计时\r\n");
  printf("          85 86 87 暂停/继续/结束 / 88 番茄 / 8A n hh mm 设闹钟\r\n");
  printf("          8B 0/1 闹钟开关 / 8C mode 循环 / 8D sel 铃音 / B0 B1 贪睡/别响\r\n");
  printf("          B6 报时 / EE vol 音量\r\n");
  printf("  B 完整帧文本：FA F1 id cmd d0 d1 d2 d3 cs FB + 回车（0xB7 用 A5 FA）\r\n");
  printf("  C HEX 直发：串口助手切 HEX，发 FA F1 id cmd d0 d1 d2 d3 cs FB，凑齐 10 字节立即执行\r\n");
  printf("  例：8A 1 07 30   或   FA F1 00 8A 01 07 1E 00 9B FB\r\n");
}

/* 执行一行命令 */
static void dbg_exec(const char *line)
{
  const char *p = line;
  u8 vals[11];   /* 简化最多 5；完整帧 10（FA F1 id cmd d0..d3 cs FB） */
  int nv = 0;

  /* 跳过行首空白 */
  while (*p && (*p == ' ' || *p == '\t')) p++;

  /* help / ? */
  if (*p == '?') { dbg_help(); return; }
  if ((p[0] == 'h' || p[0] == 'H') && p[1] == 'e' && p[2] == 'l' && p[3] == 'p')
  {
    dbg_help();
    return;
  }

  /* 逐个 token 解析（空格/逗号/分号/制表符分隔），上限 11（完整帧 10 字节） */
  while (*p)
  {
    while (*p && (*p == ' ' || *p == '\t' || *p == ',' || *p == ';')) p++;
    if (!*p) break;
    const char *tok = p;
    while (*p && !(*p == ' ' || *p == '\t' || *p == ',' || *p == ';')) p++;
    char buf[16];
    int len = (int)(p - tok);
    if (len > 15) len = 15;
    memcpy(buf, tok, (size_t)len);
    buf[len] = 0;
    if (nv >= 11)
    {
      printf("[DBGCMD] token 过多（简化最多 5 个，完整帧最多 10 个）\r\n");
      return;
    }
    if (!dbg_parse_hex(buf, &vals[nv]))
    {
      printf("[DBGCMD] 非法 hex 参数: %s（输入 help 查看用法）\r\n", buf);
      return;
    }
    nv++;
  }

  if (nv == 0) return;   /* 空行 */

  /* ---- 完整帧模式：FA F1 id cmd d0 d1 d2 d3 cs FB，或 0xB7 例外的 A5 FA ... FB ----
   * 直接粘贴帧表里的完整帧即可模拟语音模块；校验不符仅警告、照常执行。 */
  if (nv >= 4 && ((vals[0] == 0xFA && vals[1] == 0xF1) || (vals[0] == 0xA5 && vals[1] == 0xFA)))
  {
    VoiceFrame f;
    f.head[0] = vals[0];
    f.head[1] = vals[1];
    f.id  = (nv > 2) ? vals[2] : 0;
    f.cmd = (nv > 3) ? vals[3] : 0;
    f.d0  = (nv > 4) ? vals[4] : 0;
    f.d1  = (nv > 5) ? vals[5] : 0;
    f.d2  = (nv > 6) ? vals[6] : 0;
    f.d3  = (nv > 7) ? vals[7] : 0;
    {
      u8 calc = (u8)((u32)f.head[0] + f.head[1] + f.id + f.cmd + f.d0 + f.d1 + f.d2 + f.d3);
      if (nv > 8 && calc != vals[8])
        printf("[DBGCMD] 校验警告: 计算 %02X != 帧内 %02X（仍执行）\r\n",
               (unsigned)calc, (unsigned)vals[8]);
    }
    printf("[DBGCMD] full-frame -> cmd=0x%02X d0=%02X d1=%02X d2=%02X d3=%02X\r\n",
           (unsigned)f.cmd, (unsigned)f.d0, (unsigned)f.d1, (unsigned)f.d2, (unsigned)f.d3);
    Voice_HandleFrame(&f);
    return;
  }

  /* ---- 简化模式：cmd d0 d1 d2 d3 ---- */
  if (nv > 5)
  {
    printf("[DBGCMD] 参数过多（最多 cmd d0 d1 d2 d3；完整帧格式：FA F1 id cmd d0..d3 cs FB）\r\n");
    return;
  }

  /* 构造语音固定帧（FA F1）并分发——等价模块识别出的命令词 */
  {
    VoiceFrame f;
    f.head[0] = 0xFA;
    f.head[1] = 0xF1;
    f.id      = 0;
    f.cmd     = vals[0];
    f.d0      = (nv > 1) ? vals[1] : 0;
    f.d1      = (nv > 2) ? vals[2] : 0;
    f.d2      = (nv > 3) ? vals[3] : 0;
    f.d3      = (nv > 4) ? vals[4] : 0;
    printf("[DBGCMD] -> cmd=0x%02X d0=%02X d1=%02X d2=%02X d3=%02X\r\n",
           (unsigned)f.cmd, (unsigned)f.d0, (unsigned)f.d1, (unsigned)f.d2, (unsigned)f.d3);
    Voice_HandleFrame(&f);
  }
}

/* ------------------------------------------------------------------
 * 回显一个字节（格式转换，2026-08-28）：
 *   文本输入（bin_ongoing=0）：可打印 ASCII（0x20-0x7E）原样回显；
 *   HEX 帧内（bin_ongoing=1）或控制/二进制字节：一律转两位 hex 文本
 *   （如 0x0A → 回显 "0A"、0x75 → 回显 "75"），避免帧内可打印字节
 *   混成字符（0x75='u'）破坏 HEX 回显的连续性，也避免 0x00/0x0D/0x0A
 *   在串口助手"HEX+字符"同时显示时乱码/换行。忙等 TXDE 确保不丢。
 * ------------------------------------------------------------------ */
static void dbg_echo(u8 d, int as_hex)
{
  static const char H[] = "0123456789ABCDEF";
  if (!as_hex && d >= 0x20 && d <= 0x7E)   /* 文本输入：可打印 ASCII 原样 */
  {
    while (USART_GetFlagStatus(DBGCMD_UART, USART_FLAG_TXDE) == RESET) {}
    USART_SendData(DBGCMD_UART, (u16)d);
  }
  else                                     /* HEX 帧内 或 控制/二进制字节：转两位 hex 文本 */
  {
    while (USART_GetFlagStatus(DBGCMD_UART, USART_FLAG_TXDE) == RESET) {}
    USART_SendData(DBGCMD_UART, (u16)H[(d >> 4) & 0x0F]);
    while (USART_GetFlagStatus(DBGCMD_UART, USART_FLAG_TXDE) == RESET) {}
    USART_SendData(DBGCMD_UART, (u16)H[d & 0x0F]);
  }
}

/* ------------------------------------------------------------------
 * 主循环调用：收环形缓冲字节攒行，回车/换行触发执行
 * ------------------------------------------------------------------ */
void DbgCmd_Process(void)
{
  while (g_dbg_tail != g_dbg_head)
  {
    u8 d = g_dbg_ring[g_dbg_tail];
    g_dbg_tail = (u16)((g_dbg_tail + 1) % DBG_RING_SIZE);

    /* HEX 帧收集进行中（已见帧头、未凑满 10 字节）：
       a) 回显统一转 hex 文本（帧内 0x75='u' 等可打印字节也转 "75"，不混成字符）；
       b) 0x0D/0x0A 是帧内数据字节，不能当行结束截断（如设时钟 81 0E 0D，d1=0x0D，
          2026-08-28 修复）。 */
    int bin_ongoing = (g_dbg_n >= 2 &&
                       ((g_dbg_line[0] == 0xFA && g_dbg_line[1] == 0xF1) ||
                        (g_dbg_line[0] == 0xA5 && g_dbg_line[1] == 0xFA)) &&
                       g_dbg_n < 10);

    dbg_echo(d, bin_ongoing);   /* 回显（HEX 帧内全转 hex 文本；文本输入可打印原样） */

    if ((d == '\r' || d == '\n') && !bin_ongoing)
    {
      if (g_dbg_n > 0)
      {
        g_dbg_line[g_dbg_n] = 0;
        printf("\r\n");
        dbg_exec((const char *)g_dbg_line);
        g_dbg_n = 0;
      }
    }
    else if ((d == 0x08 || d == 0x7F) && !bin_ongoing)   /* 退格：仅文本输入有效；HEX 帧内是数据字节（设闹钟 17:08 的 d2=0x08 踩中，2026-08-28） */
    {
      if (g_dbg_n > 0) g_dbg_n--;
    }
    else if (g_dbg_n < (u8)(DBG_LINE_MAX - 1))
    {
      g_dbg_line[g_dbg_n++] = d;

      /* ---- HEX 二进制完整帧直通（模拟语音模块，无需回车）----
       * 凑齐 FA F1 id cmd d0 d1 d2 d3 cs FB（或 0xB7 例外的 A5 FA ... FB）10 字节立即执行；
       * 字节级判断，避开帧中 0x00 被当字符串终止符截断的问题。 */
      if (g_dbg_n == 10 &&
          ((g_dbg_line[0] == 0xFA && g_dbg_line[1] == 0xF1) ||
           (g_dbg_line[0] == 0xA5 && g_dbg_line[1] == 0xFA)) &&
          g_dbg_line[9] == 0xFB)
      {
        VoiceFrame f;
        f.head[0] = g_dbg_line[0];
        f.head[1] = g_dbg_line[1];
        f.id  = g_dbg_line[2];
        f.cmd = g_dbg_line[3];
        f.d0  = g_dbg_line[4];
        f.d1  = g_dbg_line[5];
        f.d2  = g_dbg_line[6];
        f.d3  = g_dbg_line[7];
        {
          u8 calc = (u8)((u32)f.head[0] + f.head[1] + f.id + f.cmd + f.d0 + f.d1 + f.d2 + f.d3);
          if (calc != g_dbg_line[8])
            printf("[DBGCMD] 校验警告: 计算 %02X != 帧内 %02X（仍执行）\r\n",
                   (unsigned)calc, (unsigned)g_dbg_line[8]);
        }
        printf("[DBGCMD] binary frame -> cmd=0x%02X d0=%02X d1=%02X d2=%02X d3=%02X\r\n",
               (unsigned)f.cmd, (unsigned)f.d0, (unsigned)f.d1, (unsigned)f.d2, (unsigned)f.d3);
        Voice_HandleFrame(&f);
        g_dbg_n = 0;
      }
    }
  }
}

#endif /* VOICE_ENABLE && !HD9803_ENABLE */

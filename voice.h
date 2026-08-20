/************************************************************************************************************
 * @file    voice.h
 * @brief   CI-D0XGS02S 语音识别模块驱动（UART0：PB7=TX, PB8=RX，波特率 9600）
 * @note    占用 UART0（原 printf 调试口）；printf 调试改走 USART0(PC4/PC5)（原蓝牙口）。
 *          帧格式（深合芯 语音时钟 V10 协议）：
 *            [H0][H1] [ID] [CMD] [d0][d1][d2][d3] [CS] [FB]   （10 字节）
 *          H0H1 = A5 FA 或 FA F1（自适应两种帧头）
 *          CS = (u8)(H0+H1+ID+CMD+d0+d1+d2+d3) 累加和，FB 帧尾。
 ************************************************************************************************************/
#ifndef __VOICE_H
#define __VOICE_H

#include "ht32.h"

/* ---- 语音模块总开关：1=启用（PB7/PB8 UART0 占用），0=禁用 ---- */
#ifndef VOICE_ENABLE
#define VOICE_ENABLE 1
#endif

/* ---- 引脚与串口实例：UART0(PB7/PB8)，原 printf 调试口，现归语音 ---- */
#define VOICE_GPIO_ID         GPIO_PB         /* AFIO 端口编号（u32，用于 AFIO_GPxConfig） */
#define VOICE_GPIO_PORT       HT_GPIOB        /* GPIO 端口基地址指针 */
#define VOICE_TX_AFIO_PIN     AFIO_PIN_7      /* PB7 = UART0_TX（原 printf 调试口） */
#define VOICE_RX_AFIO_PIN     AFIO_PIN_8      /* PB8 = UART0_RX */
#define VOICE_UART            HT_UART0        /* UART0 @ 0x40001000 */
#define VOICE_UART_IRQn       UART0_IRQn      /* 中断号 25 */
#define VOICE_UART_IRQHandler UART0_IRQHandler
#define VOICE_BAUDRATE        19200u          /* 实测疑似 19200：9600 收 5B=10B 帧 2:1 错位（确定性乱码） */

/* ---- 帧协议常量（语音时钟 V10） ---- */
#define VOICE_FRM_HEAD_A0     0xA5u           /* 帧头变体 A：A5 FA */
#define VOICE_FRM_HEAD_A1     0xFAu
#define VOICE_FRM_HEAD_B0     0xFAu           /* 帧头变体 B：FA F1 */
#define VOICE_FRM_HEAD_B1     0xF1u
#define VOICE_FRM_TAIL        0xFBu           /* 帧尾 */
#define VOICE_RX_BUF_SIZE     128u            /* 环形接收缓冲 */

/* ---- 解析结果（语音时钟 V10 帧） ---- */
typedef struct
{
  u8 head[2];   /* 实际帧头（A5 FA 或 FA F1） */
  u8 id;        /* 产品 ID（默认 0） */
  u8 cmd;       /* 指令号码 */
  u8 d0, d1, d2, d3;  /* 指令内容 4 字节 */
} VoiceFrame;

/* ---- API ---- */
void Voice_Init(void);            /* USART 9600 + 接收中断，占 PC4/PC5 */
void Voice_Process(void);         /* 主循环调用：从环形缓冲收帧、校验、解析 */
u8   Voice_GetFrame(VoiceFrame *f); /* 取最新完整帧：1=有新帧（取走即清） */

#endif /* __VOICE_H */

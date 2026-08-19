/************************************************************************************************************
 * @file    voice.h
 * @brief   CI-D0XGS02S 语音识别模块驱动（UART0：PB7=TX, PB8=RX，波特率 9600）
 * @note    占用 UART0（原 printf 调试口）；printf 调试改走 USART0(PC4/PC5)（原蓝牙口）。
 *          帧格式：A5 FC | LEN(2B LE) | TYPE | CMD | SEQ | DATA[LEN] | SUM(2B LE) | FB
 *          校验 SUM = TYPE..DATA 逐字节 16 位累加（LE 输出），不含头/长度/尾。
 *          TYPE 0xA0=命令词信息、0xA3=通知（如唤醒状态变化）。
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
#define VOICE_BAUDRATE        9600u           /* 模块默认 9600 */

/* ---- 帧协议常量 ---- */
#define VOICE_FRM_HEAD0       0xA5u           /* 帧头第 1 字节 */
#define VOICE_FRM_HEAD1       0xFCu           /* 帧头第 2 字节 */
#define VOICE_FRM_TAIL        0xFBu           /* 帧尾 */
#define VOICE_TYPE_CMDINFO    0xA0u           /* 命令词信息 */
#define VOICE_TYPE_NOTIFY     0xA3u           /* 通知（如状态变化） */
#define VOICE_CMD_CMDWORD     0x91u           /* 命令词数据（TYPE=CMDINFO 时） */
#define VOICE_CMD_VOICE_STATE 0x9Au           /* 语音状态变化（TYPE=NOTIFY 时） */
#define VOICE_MAX_DATA        16u             /* 数据区上限（实际命令词 ~6B，足够） */
#define VOICE_RX_BUF_SIZE     128u            /* 环形接收缓冲 */

/* ---- 解析结果 ---- */
typedef struct
{
  u8 type;              /* 0xA0 命令词信息 / 0xA3 通知 */
  u8 cmd;               /* 命令号（命令词信息 0x91） */
  u8 seq;               /* 包序号（模块外发计数） */
  u8 data[VOICE_MAX_DATA]; /* 数据区（命令词唯一数据 / 通知内容） */
  u8 len;               /* data 有效字节数 */
} VoiceFrame;

/* ---- API ---- */
void Voice_Init(void);            /* USART 9600 + 接收中断，占 PC4/PC5 */
void Voice_Process(void);         /* 主循环调用：从环形缓冲收帧、校验、解析 */
u8   Voice_GetFrame(VoiceFrame *f); /* 取最新完整帧：1=有新帧（取走即清） */

#endif /* __VOICE_H */

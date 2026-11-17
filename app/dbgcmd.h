/************************************************************************************************************
 * @file    dbgcmd.h
 * @brief   串口调试指令模拟语音输入（printf 口 USART0 PC4/PC5 @115200）
 * @note    在串口助手输入 <cmd> [d0] [d1] [d2] [d3]（hex，空格分隔，回车执行），
 *          固件把它当作一条语音固定帧喂给 Voice_HandleFrame 分发——免去对着语音模块说话即可
 *          调试倒计时/闹钟/切屏等全部语音命令。
 *          启用条件 = VOICE_ENABLE && !HD9803_ENABLE（printf 口存在且语音分发可用）。
 *          编译源组需勾选 dbgcmd.c（与 screen.c 一样手动加入 HT32-IDE 源组）。
 ************************************************************************************************************/
#ifndef __DBGCMD_H
#define __DBGCMD_H

#include "ht32.h"

/* DbgCmd_Init 必须在 UART0_Configuration() 之后调用（使能 USART0 RX 中断）；main 主循环调 DbgCmd_Process() */
void DbgCmd_Init(void);
void DbgCmd_Process(void);

#endif /* __DBGCMD_H */

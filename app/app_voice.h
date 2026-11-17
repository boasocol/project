#ifndef __APP_VOICE_H
#define __APP_VOICE_H

#include "voice.h"   /* VoiceFrame */

/* ==================== 语音命令业务层（P2 反转，2026-09-03） ====================
 * 协议驱动 voice.c 只解析帧；业务分发（设时/设日/设闹钟/报时/唤醒/状态/音量 +
 * screen 域倒计时/闹钟/显示开关…）统一在应用层本模块。
 * main 初始化时注册：Voice_SetCmdHandler(App_VoiceCmd)。
 * 注意：模块体在 app_voice.c 内受 #if VOICE_ENABLE 保护（2026-09-05 解除与蓝牙互斥：
 * 语音 UART0 与蓝牙 USART0 外设独立可共存）。
 */

int App_VoiceCmd(const VoiceFrame *f);   /* 语音固定帧业务处理；返回 1=已处理（含未接线回 FAIL） */

#endif /* __APP_VOICE_H */

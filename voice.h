/************************************************************************************************************
 * @file    voice.h
 * @brief   CI-D0XGS02S 语音识别模块驱动（UART0：PB7=TX, PB8=RX，波特率 9600）
 * @note    占用 UART0（原 printf 调试口）；printf 调试改走 USART0(PC4/PC5)（原蓝牙口）。
 *          帧格式（两套协议自适应，A/B 帧头自动识别）：
 *            A) 深合芯 语音时钟 V10（固定 10 字节）：
 *                 [H0][H1] [ID] [CMD] [d0][d1][d2][d3] [CS] [FB]
 *               H0H1 = A5 FA 或 FA F1；CS = (u8)(H0+H1+ID+CMD+d0+d1+d2+d3) 累加和。
 *            B) 启英泰伦 CI130X 标准串口协议（变长）：
 *                 [A5][FC] [L0][L1] [type][cmd][seq] [data x L] [CS0][CS1] [FB]
 *               L = L0 + L1*256 为有效数据长度；type=A0 命令词信息 / A3 通知等。
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
/* 权威协议（深合芯-语音时钟V10 Excel）：波特率=9600bps、8bit、1stop、None 校验。 */
#define VOICE_BAUDRATE        9600u

/* ---- 帧协议常量 ---- */
#define VOICE_FRM_HEAD_A0     0xA5u           /* 帧头变体 A：A5 FA（V10 固定帧） */
#define VOICE_FRM_HEAD_A1     0xFAu
#define VOICE_FRM_HEAD_B0     0xFAu           /* 帧头变体 B：FA F1（V10 固定帧） */
#define VOICE_FRM_HEAD_B1     0xF1u
#define VOICE_FRM_HEAD_C1     0xFCu           /* 帧头变体 C：A5 FC（CI130X 变长帧） */
#define VOICE_FRM_TAIL        0xFBu           /* 帧尾 */
#define VOICE_FRM_MAX         32u             /* 变长帧最大有效数据长度（超长丢帧防溢出） */
#define VOICE_RX_BUF_SIZE     128u            /* 环形接收缓冲 */

/* ---- 命令宏（与 notes/voice-clock-protocol.md 命令表一一对应） ----
   方向：CI→MCU = 模块识别命令词后上报（VOICE_CMD_*）；MCU→CI = MCU 让模块播报/控制（VOICE_TX_*）。
   已接线命令在 voice.c voice_handle_frame 分发；未接线命令仍定义（保持协议全貌），
   收到未接线命令回 VOICE_TX_FAIL"设置失败"。 */

/* CI→MCU（0x80~0xEF，识别命令词后模块上报） */
#define VOICE_CMD_WAKE        0x80u  /* 唤醒词"小保小保"——已接线：回 VOICE_TX_WAKE_BEEP + 动画切 WAKE */
#define VOICE_CMD_SET_TIME    0x81u  /* 设置时钟 d0=时(1-24) d1=分——已接线 → TimeModule_SetTime */
#define VOICE_CMD_SET_DATE    0x82u  /* 设置日期 d0=年十位 d1=年个位 d2=月 d3=日——已接线 */
#define VOICE_CMD_CNT_DAY     0x83u  /* 天数倒计时（未接线） */
#define VOICE_CMD_CNT_MIN     0x84u  /* 分钟倒计时（未接线） */
#define VOICE_CMD_PAUSE       0x85u  /* 暂停计时（未接线） */
#define VOICE_CMD_RESUME      0x86u  /* 继续计时（未接线） */
#define VOICE_CMD_STOP        0x87u  /* 结束计时（未接线） */
#define VOICE_CMD_TOMATO_ON   0x88u  /* 开始番茄计时（未接线） */
#define VOICE_CMD_TOMATO_OFF  0x89u  /* 关闭番茄计时（未接线） */
#define VOICE_CMD_SET_ALARM   0x8Au  /* 设置闹钟 d0=闹钟号(1-3) d1=时 d2=分——已接线 → UI_SetAlarm(仅存配置) */
#define VOICE_CMD_ALARM_SW    0x8Bu  /* 打开/关闭闹钟（未接线） */
#define VOICE_CMD_ALARM_LOOP  0x8Cu  /* 闹钟循环模式（未接线） */
#define VOICE_CMD_ALARM_RING  0x8Du  /* 闹铃选择（未接线） */
#define VOICE_CMD_SNOOZE      0xB0u  /* 贪睡（未接线） */
#define VOICE_CMD_ALARM_OFF   0xB1u  /* 关闭本次闹钟/别响了（未接线） */
#define VOICE_CMD_DISP_OFF    0xB4u  /* 关闭显示（未接线） */
#define VOICE_CMD_DISP_ON     0xB5u  /* 打开显示（未接线） */
#define VOICE_CMD_TELL_TIME   0xB6u  /* 报时——已接线：回 VOICE_TX_TELL_TIME */
#define VOICE_CMD_CNT_DAY_END 0xB7u  /* 结束天数倒计时（帧头 A5 FA 特殊；未接线） */
#define VOICE_CMD_HOURLY      0xB8u  /* 整点报时开关（未接线） */
#define VOICE_CMD_QUERY_ALARM 0xB9u  /* 查看闹钟（未接线） */
#define VOICE_CMD_CNT_UP      0xBAu  /* 打开正计时（未接线） */
#define VOICE_CMD_VOLUME      0xEEu  /* 打开声音/音量 d0=0最高/1最低/2静音——已接线(仅记录) */
#define VOICE_CMD_STATE       0xEFu  /* 模块状态 d0=00上电完成/01 15s无命令——已接线(01→动画回 IDLE) */

/* MCU→CI（0x00~0x09，MCU 主动让模块播报/控制） */
#define VOICE_TX_BOOT        0x00u  /* 开机播报"欢迎使用xx语音闹钟"（main 每次复位无条件发） */
#define VOICE_TX_LOWBAT      0x01u  /* 低电提示 */
#define VOICE_TX_OK          0x02u  /* "好的，设置成功" */
#define VOICE_TX_WAKE_BEEP   0x03u  /* 唤醒提示音"嘀" */
#define VOICE_TX_RING        0x04u  /* 播放铃声 d0=0/1 铃声 d1=0不播/1播 */
#define VOICE_TX_TIMER_BEEP  0x05u  /* 倒计时提示音 */
#define VOICE_TX_SET_VOL     0x06u  /* 音量设置 d0=0最高/1最低/2静音 */
#define VOICE_TX_TELL_TIME   0x07u  /* 播"现在是XX点XX分" d0=时 d1=分 */
#define VOICE_TX_HOURLY      0x08u  /* 整点报时开关 d0=0/1 */
#define VOICE_TX_FAIL        0x09u  /* "设置失败，请重试" */

/* 0xEF 状态通知的 D0 取值 */
#define VOICE_ST_D0_PWRON    0x00u  /* 上电完成 */
#define VOICE_ST_D0_TIMEOUT  0x01u  /* 15s 无语音命令（超时回待机触发点） */

/* 参数错误标记：模块未识别出时间/闹钟（0x81/0x8A 回传 ff ff） */
#define VOICE_PARM_UNRECOG   0xFFu

/* ---- 解析结果（统一为 head/id/cmd/d0..d3；变长帧 id=type cmd=命令号） ---- */
typedef struct
{
  u8 head[2];   /* 实际帧头（A5 FA 或 FA F1） */
  u8 id;        /* 产品 ID（默认 0） */
  u8 cmd;       /* 指令号码 */
  u8 d0, d1, d2, d3;  /* 指令内容 4 字节 */
} VoiceFrame;

/* ---- API ---- */
void Voice_Init(void);            /* UART0(PB7/PB8) 9600 + 接收中断 */
void Voice_Process(void);         /* 主循环调用：从环形缓冲收帧、校验、解析 + 命令分发 */
u8   Voice_GetFrame(VoiceFrame *f); /* 取最新完整帧：1=有新帧（取走即清） */
void Voice_Send(u8 cmd, u8 d0, u8 d1, u8 d2, u8 d3); /* MCU→CI：FA F1 固定帧（语音回复/控制） */

#endif /* __VOICE_H */

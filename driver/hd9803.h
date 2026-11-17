/************************************************************************************************************
 * @file    hd9803.h
 * @brief   HD9803 蓝牙授时模块驱动（USART：PC4=TX, PC5=RX，波特率 9600）
 * @note    通过 UART0（PB7/PB8）printf 打印模块工作状态，便于调试。
 *          帧格式：0x24 + 数据长度 + 命令字 + 命令内容 + 校验位 + 0x42
 *          数据长度 = 命令字 + 命令内容 + 校验位；校验位 = 从长度开始逐字节异或
 ************************************************************************************************************/
#ifndef __HD9803_H
#define __HD9803_H

#include "ht32.h"
#include "time_module.h"   /* TimeStruct（授时校准 RTC 用） */
#include "bsp_pins.h"      /* HD9803_* 引脚/串口实例/电源脚映射（P1 集中，2026-09-03） */

/* ---- 蓝牙模块总开关：1=启用（PC4/PC5 USART0 占用），0=禁用（不占引脚/不进中断） ---- */
#ifndef HD9803_ENABLE
#define HD9803_ENABLE 0    /* BT disabled: USART0(PC4/PC5) -> voice, re-enable later */
#endif

/* 引脚/串口实例/电源控制脚映射见 bsp_pins.h（HD9803_GPIO_ID/PORT、TX/RX_AFIO_PIN、
   UART/IRQ、PWR_*）。HD9803_PWR_GPIO_CLK=PA 是 CKCU 结构体位域名（clk.Bit.PA）。 */
#define HD9803_BAUDRATE        9600u           /* HD9803 固定 9600 */

/* ---- 帧协议常量 ---- */
#define HD9803_FRM_HEAD        0x24u
#define HD9803_FRM_TAIL        0x42u
#define HD9803_CMD_MODEL       0x11u           /* 模块型号（模块->MCU） */
#define HD9803_CMD_TIME        0x15u           /* 下发时间（模块->MCU） */
#define HD9803_CMD_SET_NAME    0x07u           /* 设置名称（MCU->模块） */
#define HD9803_CMD_PAIR        0x08u           /* 配对/回连模式（MCU->模块） */

/* ---- 授时时间结构（普通十进制，非 BCD！year 为年份后两位） ---- */
typedef struct
{
  u8 year;    /* 年后两位（如 26 = 2026） */
  u8 month;   /* 月 1-12 */
  u8 day;     /* 日 1-31 */
  u8 hour;    /* 时 0-23 */
  u8 minute;  /* 分 0-59 */
  u8 second;  /* 秒 0-59 */
} Hd9803Time;

/* ---- 蓝牙工作状态（供 UI 显示） ---- */
typedef enum
{
  HD9803_ST_OFF = 0,          /* 电源关闭（空闲省电） */
  HD9803_ST_SCAN,             /* 上电发现窗口：20s 内可发现+回连，等待手机授时 */
  HD9803_ST_LINK,             /* 每日 8/18 点回连窗口：仅回连，等待授时 */
  HD9803_ST_CONNECTED,        /* 已连接：窗口内收到时间，手机已连过 */
  HD9803_ST_DISCONNECTED,     /* 未连接：窗口超时未收到时间 */
} Hd9803State;

/* ---- 窗口时长（秒） ---- */
#define HD9803_WINDOW_SCAN_MS    20000u          /* 上电发现窗口 20s */
#define HD9803_WINDOW_LINK_MS    20000u          /* 每日回连窗口 20s */

/* ---- API ---- */
/* 授时时间到达回调（P2-3 反转：应用层注册，收到新授时时间时被调用一次，
   应用层在此校准 RTC + 触发界面静态刷新；未注册则丢弃。本驱动不再 include 应用层头）。 */
typedef void (*Hd9803TimeSink)(const Hd9803Time *t);
void Hd9803_SetTimeSink(Hd9803TimeSink sink);

void Hd9803_SetPower(u8 on);                     /* 控制模块电源：1=上电，0=断电（高有效经开关管） */
void Hd9803_Init(void);                          /* 状态机初始化（先断电，首次 SCAN 由 main 触发） */
void Hd9803_Tick(u8 one_sec);                    /* 每秒心跳：驱动窗口计时/电源/状态机（one_sec=1 每秒一次） */
void Hd9803_RequestScan(void);                   /* 请求上电发现窗口（上电时调用） */
void Hd9803_RequestRelink(void);                 /* 请求回连窗口（每日 8/18 点由 main 调用） */
void Hd9803_Poll(void);                          /* 主循环调用：解析已接收帧（内部自动喂给状态机） */
u8   Hd9803_GetTime(Hd9803Time *t);              /* 返回最近授时时间；1=有新时间，0=无 */
Hd9803State Hd9803_GetState(void);               /* 返回当前蓝牙状态（供 UI 显示） */
void Hd9803_Process(void);                        /* 主循环调用：解析串口 + 收到新授时则校准 RTC */
void Hd9803_OnSecondTick(const TimeStruct *now);  /* 每秒调用：状态机推进 + 每日 8/18 点回连校准 */
void Hd9803_SetName(const u8 *name, u8 len);     /* 设置模块名称（len<=15，ASCII） */
void Hd9803_EnterPairMode(void);                 /* 0x08 0x01：配对模式（可被发现） */
void Hd9803_EnterRelinkMode(void);               /* 0x08 0x02：回连上次手机 */
void Hd9803_EnterPairRelinkMode(void);           /* 0x08 0x10：回连+可被发现 */

#endif /* __HD9803_H */

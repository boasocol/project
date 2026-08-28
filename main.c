/************************************************************************************************************
 * @brief  主函数：横屏日历(320x240) + 浅黄背景
 *          流程：初始化外设 -> 初始化屏幕 -> 铺浅黄底 -> 画日历 -> 每秒刷新
 * @note   主循环只做“事件分发”：每个功能都收敛到对应模块名接口，
 *         具体逻辑（蓝牙授时 / 温湿度读取 / 翻页动画 / 界面刷新 / 按键休眠）藏在模块内部。
 ************************************************************************************************************/
#include "ht32.h"
#include "BFTM0.h"
#include "BFTM1.h"
#include "time_module.h"
#include "uart_init.h"
#include "Display.h"
#include "flash_read.h"
#include "calendar.h"
#include "yx8320b.h"
#include "hd9803.h"   /* 无条件包含；HD9803_ENABLE 宏在 hd9803.h 内部定义 */
#include "voice.h"   /* 语音模块（USART0，HD9803_ENABLE=0 时占用） */
#include "key.h"      /* PB12 按键：关屏/低功耗/唤醒 */
#include "screen.h"   /* 多屏状态机：倒计时/闹钟（2026-08-27） */
#include "ui.h"       /* UI_GetScreen：冒号闪烁仅主屏 */

int main(void)
{
  UART0_Configuration();
#if VOICE_ENABLE && !HD9803_ENABLE
  /* 语音模块 UART0 RX 尽早使能：抢在模块"上电完成"帧(0xEF)之前接收。
     显示/Flash 初始化（整屏上传 153KB）较慢，若 Voice_Init 排在最后会漏掉这帧
     （模块上电约几百 ms 后发 0xEF，调试助手能抓到、MCU 抓不到就是这个原因）。 */
  Voice_Init();
#endif
  BFTM0_Configuration();
  TimeModule_Init();   /* RTC 现同时提供 1s 时间节拍与 0.5s 冒号闪烁节拍 */

  /* SPI1 外部 Flash：Calendar_Init 内读字库/背景依赖 Flash_Read，
     未初始化会因 SPI1 时钟未使能而 HardFault */
  Flash_Init();

  Display_Init();

  Calendar_Init();     /* 画静态标签(TEMP/HUMI) + 从 Flash 整屏上传背景 */
  Screen_Init();       /* 多屏状态机初始化：回主屏（2026-08-27） */
  Display_on();
  /* 温湿度传感器（软件 I2C：PB0=SCL, PB1=SDA，读数单位 0.1） */
  YX8320B_Init();
  {
    u8 aw = YX8320B_GetAddr();
    printf("[YX8320B] probe: %s\r\n",
           aw == 0 ? "addr 0x8A ACK (0x45)" : (aw == 1 ? "addr 0x88 ACK (0x44)" : "NO DEVICE (check wiring/pullup!)"));
  }

  /* PB12 按键：关屏/深度休眠1/唤醒（EXTI12 + 唤醒事件，唤醒后程序继续执行） */
  Key_Init();

#if HD9803_ENABLE
  Hd9803_Init();
  Hd9803_RequestScan();   /* 启动上电发现窗口 */
#endif

#if VOICE_ENABLE && !HD9803_ENABLE
  /* 开机播报：MCU 每次复位无条件让模块播"欢迎使用xx语音闹钟"。
     不依赖模块 0xEF——debug 仿真时模块全程不断电、MCU 复位不会重发 0xEF。
     若冷启动时模块尚未 boot 完（~几百 ms）此帧可能丢失，属可接受场景。 */
  Voice_Send(VOICE_TX_BOOT, 0, 0, 0, 0);
  printf("[VOICE] 开机播报已发送\r\n");
#endif

  TimeStruct t;
  printf("Calendar demo (landscape) started\r\n");

  while (1)
  {
    if (HT_CKCU->APBCCR1 & (1 << 4))
      WDT_Restart();

    /* 深度休眠期间：每 1s 节流打印一次 RTC 时间，验证休眠走时 / 排查残留唤醒源 */
    {
      static vu32 last_ds_print = 0;
      if (g_key_deep_sleep_flag && ((u32)bftm0_ct - last_ds_print) >= 1000u)
      {
        last_ds_print = (u32)bftm0_ct;
        TimeModule_GetTime(&t);
        printf("[DSLEEP] %04u-%02u-%02u %02u:%02u:%02u\r\n",
               t.year, t.month, t.day, t.hour, t.minute, t.second);
      }
    }

    Key_Process();            /* 关屏/开屏 + 深度休眠1（阻塞至唤醒） */

#if HD9803_ENABLE
    Hd9803_Process();         /* 蓝牙授时：解析串口 + 收到新时间则校准 RTC */
#elif VOICE_ENABLE
    Voice_Process();          /* 语音模块：收帧解析（命令词/通知） */
#endif

    /* 每轮取最新时间快照：
       Display_Update 的 Calendar_Refresh 每轮用它对比秒变化。若只在 flag 置位时
       读一次，采样分支(温湿度读取等)阻塞后 t 是过期值，且 g_time_update_flag 是
       单 bit 无法累积——阻塞期间置位的整秒会被合并掉，造成“5 秒等两秒才翻 7 秒”。 */
    TimeModule_GetTime(&t);

    /* 每秒数据采样（温湿度 / 蓝牙状态机），仅时间节拍到来时执行一次。
       此处消费 g_time_update_flag（清零）：Display_Update 里的 Calendar_Refresh
       已改为每轮增量刷新、不再消费该标志，故清零归本分支管，保证温湿度每秒只读一次。 */
    if (g_time_update_flag)
    {
      g_time_update_flag = 0;
      YX8320B_Task();             /* 每秒读一次温湿度（失败保留上次值） */
      Screen_OnSecond(&t);        /* 多屏：倒计时推进 + 闹钟到点判定（2026-08-27） */
#if HD9803_ENABLE
      Hd9803_OnSecondTick(&t);    /* 蓝牙状态机推进 + 每日 8/18 点回连校准 */
#endif
    }

    Display_Update(&t);       /* 翻页动画 + 时钟/静态区块刷新（集中界面渲染） */

    /* 冒号闪烁：消费 RTC 0.5s 中断置位的 g_colon_blink_flag，每半秒翻转冒号（仅主屏） */
    if (g_colon_blink_flag && UI_GetScreen() == UI_SCREEN_MAIN)
    {
      g_colon_blink_flag = 0;
      Calendar_ColonBlink();
    }
  }
}

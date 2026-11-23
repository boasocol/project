/************************************************************************************************************
 * @brief  主函数：横屏日历(320x240) + 浅黄背景
 *          流程：初始化外设 -> 初始化屏幕 -> 铺浅黄底 -> 画日历 -> 每秒刷新
 * @note   主循环只做“事件分发”：每个功能都收敛到对应模块名接口，
 *         具体逻辑（蓝牙授时 / 温湿度读取 / 翻页动画 / 界面刷新 / 按键休眠）藏在模块内部。
 ************************************************************************************************************/
#include "ht32.h"
#include "BFTM0.h"
#include "time_module.h"
#include "uart_init.h"
#include "Display.h"
#include "flash_read.h"
#include "calendar.h"
#include "yx8320b.h"
#include "hd9803.h"   /* 无条件包含；HD9803_ENABLE 宏在 hd9803.h 内部定义 */
#include "voice.h"   /* 语音模块（USART0，HD9803_ENABLE=0 时占用） */
#include "app_voice.h" /* App_VoiceCmd：语音命令业务回调（P2 反转，2026-09-03） */
#include "dbgcmd.h"  /* 串口调试指令模拟语音输入（printf 口 USART0 PC4/PC5，2026-08-28） */
#include "key.h"      /* PB12 按键：关屏/低功耗/唤醒 */
#include "screen.h"   /* 多屏状态机：倒计时/闹钟（2026-08-27） */
#include "ui.h"       /* UI_GetScreen：冒号闪烁仅主屏 */
#include "app_render.h" /* App_Render：按当前屏分发渲染（P2 拆 Display.c，2026-09-03） */

/* ================= P2-3（2026-09-03）应用层副作用回调 =================
   key.c / hd9803.c 已分层反转（驱动/系统层不再 include 应用层头），
   关屏/开屏的蓝牙断电上电、日历强制刷新、授时校准 RTC 等副作用
   统一收敛到 main.c（app 顶层）注册的回调中执行。 */

/* 关屏副作用：蓝牙断电（PA15 拉高）。HD9803_ENABLE 由应用层自行判断。 */
static void app_key_screen_off(void)
{
#if HD9803_ENABLE
  Hd9803_SetPower(0);
#endif
}

/* 开屏副作用：日历强制全量重绘（不翻动画）+ 蓝牙重新上电。 */
static void app_key_screen_on(void)
{
  Calendar_SetStaticNext();
#if HD9803_ENABLE
  Hd9803_SetPower(1);
#endif
}

/* 授时时间到达：校准 RTC + 静态刷新（避免多位串行翻页）。仅蓝牙启用时定义并注册。 */
#if HD9803_ENABLE
static void app_hd9803_time(const Hd9803Time *bt)
{
  /* HD9803 时间字段为普通十进制（非 BCD），year 为年份后两位 */
  TimeStruct bt_t;
  bt_t.year   = 2000 + bt->year;
  bt_t.month  = bt->month;
  bt_t.day    = bt->day;
  bt_t.hour   = bt->hour;
  bt_t.minute = bt->minute;
  bt_t.second = bt->second;
  TimeModule_SetTime(&bt_t);
  Screen_AlarmRungReset();   /* 坑#82：蓝牙授时改时间后清"本分钟已响"（授时回拨后闹钟重新判定） */

  /* 授时跳变：直接静态刷新（无动画），避免多位串行依次翻页 */
  Calendar_SetStaticNext();
  printf("[HD9803] RTC calibrated to 20%02u-%02u-%02u %02u:%02u:%02u\r\n",
         bt->year, bt->month, bt->day, bt->hour, bt->minute, bt->second);
}
#endif

/* ================= 外部烧录存储 IC 保护（2026-11-23）=================
   烧录器给板上的 SPI Flash 烧录时，会把电反灌到板子，主控因此被"半上电"启动；
   如果主控这时再初始化 SPI1 去读 Flash，就会和烧录器抢同一组总线（SCK/MOSI/CS），
   表现为烧录失败 / 校验错。
   约定：**上电时同时按住 左键(KEY1/PB12) + 右键(KEY3/PB14)** 就当作"正在烧录"：
     · 不初始化 SPI1/Flash（PA0..PA3 保持复位后的高阻输入，不驱动总线）
     · 不初始化屏幕 / 串口 / 语音，直接停在原地等断电
   一旦识别就锁住（烧录中途松手也不会突然启动去抢总线）；误触了松手断电重上电即可。
   置 0 = 关闭该保护（正常按键行为不受影响，只是没了这个后门）。 */
#define FLASH_ISP_HOLD   1

int main(void)
{
#if FLASH_ISP_HOLD
  /* 必须在任何外设初始化之前：尤其是 Flash_Init()/UI_LoadHeader() 之前的 SPI1 配置。
     本函数只碰 PB12/PB14（GPIO 输入+上拉），不碰 PA0..PA3。 */
  if (Key_BootHoldDetect())
  {
    for (;;)
    {
      /* 看门狗若已使能就喂一下，避免复位反复打断静默；未使能时这条不执行 */
      if (HT_CKCU->APBCCR1 & (1 << 4)) WDT_Restart();
    }
  }
#endif

  UART0_Configuration();
#if VOICE_ENABLE
  /* 语音模块 UART0 RX 尽早使能：抢在模块"上电完成"帧(0xEF)之前接收。
     显示/Flash 初始化（整屏上传 153KB）较慢，若 Voice_Init 排在最后会漏掉这帧
     （模块上电约几百 Ms 后发 0xEF，调试助手能抓到、MCU 抓不到就是这个原因）。
     2026-09-05：语音(UART0)与蓝牙(USART0)共存，不再受 HD9803_ENABLE 互斥。 */
  Voice_Init();
  Voice_SetCmdHandler(App_VoiceCmd);   /* 注册语音命令业务回调（协议驱动只解析帧，P2 反转 2026-09-03） */
#if !HD9803_ENABLE
  DbgCmd_Init();    /* printf 口(USART0 PC4/PC5) RX 中断：串口输入 hex 指令模拟语音命令；蓝牙模式 USART0 归蓝牙不启 */
#endif
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
  /* 注册关屏/开屏副作用回调（P2-3 反转：日历强制刷新、蓝牙断电上电收敛到 app 层） */
  Key_SetScreenOffCb(app_key_screen_off);
  Key_SetScreenOnCb(app_key_screen_on);

#if HD9803_ENABLE
  Hd9803_Init();
  Hd9803_RequestScan();   /* 启动上电发现窗口 */
  Hd9803_SetTimeSink(app_hd9803_time);  /* 注册授时时间 sink（P2-3 反转：校准 RTC 收敛到 app 层） */
#endif

#if VOICE_ENABLE
  /* 开机播报：MCU 每次复位无条件让模块播"欢迎使用xx语音闹钟"。
     不依赖模块 0xEF——debug 仿真时模块全程不断电、MCU 复位不会重发 0xEF。
     若冷启动时模块尚未 boot 完（~几百 ms）此帧可能丢失，属可接受场景。
     2026-09-05：蓝牙模式同样播报（语音 UART0 与蓝牙 USART0 共存）。 */
//  Voice_Send(0x06,1,0,0,1);
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

    Key_Process();            /* 三键长/短按判定入队 + 关屏/唤醒深休眠（关屏决策在下方 Screen_HandleKey） */

    /* 按键事件流 -> 应用层集中分发（2026-09-05 重构：
       Driver(key.c) 出 KeyEvent{键号,短/长按} 队列，Screen_HandleKey 按当前状态映射功能，
       含 KEY2 长按关屏（Screen_HandleKey 内调 Key_ScreenOff）） */
    {
      KeyEvent ke;
      while (Key_GetEvent(&ke))
        Screen_HandleKey(&ke);
    }

    /* 2026-09-05：蓝牙(USART0)与语音(UART0)外设独立，主循环各自轮询、互不干扰 */
#if HD9803_ENABLE
    Hd9803_Process();         /* 蓝牙授时：解析串口 + 收到新时间则校准 RTC */
#endif
#if VOICE_ENABLE
    Voice_Process();          /* 语音模块：收帧解析（命令词/通知） */
#if !HD9803_ENABLE
    DbgCmd_Process();         /* 串口调试指令：hex 命令模拟语音输入（蓝牙模式 USART0 归蓝牙不启） */
#endif
#endif

 //   debug_test(&t);

    /* 每轮取最新时间快照：
       App_Render 的 Calendar_Refresh 每轮用它对比秒变化。若只在 flag 置位时
       读一次，采样分支(温湿度读取等)阻塞后 t 是过期值，且 g_time_update_flag 是
       单 bit 无法累积——阻塞期间置位的整秒会被合并掉，造成“5 秒等两秒才翻 7 秒”。 */
    TimeModule_GetTime(&t);

    /* 每秒数据采样（温湿度 / 蓝牙状态机），仅时间节拍到来时执行一次。
       此处消费 g_time_update_flag（清零）：App_Render 里的 Calendar_Refresh
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

    Screen_Poll();

    /* 按键设置模式：显示冻结（传 g_set_ws 快照给渲染，Calendar_Refresh 增量缓存自动重绘调整段） */
    {
      TimeStruct td = t;
      if (Screen_SetupActive()) td = *Screen_SetupValue();
      App_Render(&td);      /* 翻页动画 + 按当前屏渲染（应用层分发，P2 由 Display_Update 改名） */
    }

    /* 冒号闪烁：消费 RTC 0.5s 中断置位的 g_colon_blink_flag，每半秒翻转冒号（仅主屏；设置模式停闪） */
    if (g_colon_blink_flag && UI_GetScreen() == UI_SCREEN_MAIN && !Screen_SetupActive())
    {
      g_colon_blink_flag = 0;
      Calendar_ColonBlink();
    }
  }
}

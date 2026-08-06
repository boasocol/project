/************************************************************************************************************
 * @brief  主函数：横屏日历(320x240) + 浅黄背景
 *          流程：初始化外设 -> 初始化屏幕 -> 铺浅黄底 -> 画日历 -> 每秒刷新
 * @note   温湿度当前为占位值，实际请替换为传感器读数（如 SHT30/AHT20 等）
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

/**
 * @brief  主函数：初始化各外设与界面，主循环中处理秒刷新与冒号闪烁
 * @note   两个事件标志均由中断置位、主循环消费（中断内不做 SPI 操作）：
 *         - g_time_update_flag：RTC 每 1s（两次 0.5s 进位）置位 -> Calendar_Refresh 增量刷新
 *         - g_colon_blink_flag：RTC 每 0.5s 置位 -> Calendar_ColonBlink 冒号闪烁
 * @retval 不返回（死循环）
 */
int main(void)
{
  UART0_Configuration();
  BFTM0_Configuration();
  TimeModule_Init();   /* RTC 现同时提供 1s 时间节拍与 0.5s 冒号闪烁节拍 */

  /* ---- SPI1 外部 Flash 初始化（必须！Calendar_Init 内 UI_LoadHeader/UI_UploadBackground
         依赖 Flash_Read 读字库与背景，未初始化会因 SPI1 时钟未使能而 HardFault） ---- */
  Flash_Init();

  Display_Init();

  /* 注意：背景由 Calendar_Init 内 UI_UploadBackground() 从 Flash 整屏上传覆盖，
     不再程序化铺浅黄底；Display_on 必须在上传前开启显示。 */
  Display_on();			// 先开启显示，再上传背景防止花屏
  /* 画静态标签（TEMP/HUMI） */
  Calendar_Init();

  /* YX8320B 温湿度传感器（软件 I2C：PB0=SCL, PB1=SDA）
     读数单位 0.1：temp=255 表示 25.5°C，humi=603 表示 60.3%RH */
  YX8320B_Init();
  {
    u8 aw = YX8320B_GetAddr();
    printf("[YX8320B] probe: %s\r\n",
           aw == 0 ? "addr 0x8A ACK (0x45)" : (aw == 1 ? "addr 0x88 ACK (0x44)" : "NO DEVICE (check wiring/pullup!)"));
  }
  int temp = 260;  /* 传感器未接/读失败时保留的默认显示值（26.0°C） */
  int humi = 600;  /* 60.0%RH */

#if HD9803_ENABLE
  /* HD9803 蓝牙授时模块（USART：PC4=TX, PC5=RX @9600）
     上电进入 20s 发现窗口（回连+可发现），窗口内收到手机授时则判定已连接，
     窗口结束无论结果如何都关闭模块电源省电；
     已连接状态每日 8/18 点自动回连校准时间，失败则降级为未连接。 */
  Hd9803_Init();
  Hd9803_RequestScan();   /* 启动上电发现窗口 */
#endif

  TimeStruct t;
  printf("Calendar demo (landscape) started\r\n");

  while (1)
  {
    if (HT_CKCU->APBCCR1 & (1 << 4))
      WDT_Restart();

#if HD9803_ENABLE
    /* 蓝牙授时轮询（每次循环快速检查，有数据才处理） */
    Hd9803_Poll();
    {
      Hd9803Time bt;
      u8 r = Hd9803_GetTime(&bt);
      if (r == 1)   /* 收到新的授时时间 -> 校准 RTC */
      {
        /* HD9803 时间字段为普通十进制（非 BCD），year 为年份后两位 */
        TimeStruct bt_t;
        bt_t.year   = 2000 + bt.year;
        bt_t.month  = bt.month;
        bt_t.day    = bt.day;
        bt_t.hour   = bt.hour;
        bt_t.minute = bt.minute;
        bt_t.second = bt.second;
        TimeModule_SetTime(&bt_t);
        /* 授时跳变：直接静态刷新（无动画），避免多位串行依次翻页 */
        Calendar_SetStaticNext();
        printf("[HD9803] RTC calibrated to 20%02u-%02u-%02u %02u:%02u:%02u\r\n",
               bt.year, bt.month, bt.day,
               bt.hour, bt.minute, bt.second);
      }
    }
#endif /* HD9803_ENABLE */

    /* 翻页动画帧推进（每 7-8ms 一帧，BFTM0 1ms 计数作为时间基准） */
    Calendar_FlipTick((u32)bftm0_ct);

    if (g_time_update_flag)
    {
      g_time_update_flag = 0;
      /* 每秒读取一次 YX8320B 温湿度（手册建议 <=1 次/秒避免自热影响）；
         读失败时保留上次有效值继续显示 */
      static u8 first_ok = 0;
      u8 err = YX8320B_ReadTempHumi(&temp, &humi);
      if (err != 0)
      {
        printf("[YX8320B] read fail(err=%u: %s), keep last: T=%d.%d H=%d.%d\r\n",
               err, err == 1 ? "W-addr no ACK" : (err == 2 ? "R-addr no ACK" :
                   (err == 3 ? "T CRC" : "RH CRC")),
               temp / 10, temp % 10, humi / 10, humi % 10);
      }
      else if (!first_ok)
      {
        first_ok = 1;
        printf("[YX8320B] OK: T=%d.%d C, H=%d.%d %%RH\r\n",
               temp / 10, temp % 10, humi / 10, humi % 10);
      }
      TimeModule_GetTime(&t);

#if HD9803_ENABLE
      /* 蓝牙状态机每秒心跳（驱动窗口计时/电源） */
      Hd9803_Tick(1);

      /* 界面蓝牙状态：已连接(收到过授时) / 未连接(其余) */
      Calendar_SetBtState(Hd9803_GetState() == HD9803_ST_CONNECTED);

      /* 已连接状态：每天 8:00 / 18:00 整点自动回连一次校准时间
         （回连失败由状态机内部降级为未连接） */
      if (Hd9803_GetState() == HD9803_ST_CONNECTED &&
          (t.minute == 0) && (t.hour == 8 || t.hour == 18))
      {
        static u8 last_trig_h = 0xFF;
        if (last_trig_h != t.hour)
        {
          last_trig_h = t.hour;
          printf("[HD9803] %02u:00 trigger daily relink\r\n", t.hour);
          Hd9803_RequestRelink();
        }
      }
#endif /* HD9803_ENABLE */

      Calendar_Refresh(&t, temp, humi);
    }

    /* 冒号闪烁已停用（翻页动画替代），代码保留：RTC 0.5s 中断仍置位标志但不再消费
    if (g_colon_blink_flag)
    {
      g_colon_blink_flag = 0;
      Calendar_ColonBlink();
    }
    */
  }
}

/************************************************************************************************************
 * @file    BFTM1.c
 * @brief   BFTM1 定时器（原用于 0.5s 冒号闪烁，现已改由 RTC 0.5s 中断驱动，本文件保留备用）
 * @note    芯片定时器资源：MCTM x1 / GPTM x1 / SCTM x2 / BFTM x2。
 *          BFTM0 已用作 1ms 系统节拍，此处使用空闲的 BFTM1。
 *          BFTM 是 32-bit 向上计数器，重复模式下比较匹配 -> 产生中断并自动清零重计，
 *          正好用来产生固定周期中断（无需分频器，直接用比较值定周期）。
 *          比较值 = SystemCoreClock / 2 = 0.5s（48MHz 时为 24,000,000，32-bit 足够）。
 *
 *          中断里只置标志不做 SPI 操作：屏幕刷新集中在主循环，
 *          避免中断里的 SPI 写与主循环刷新（日期/温湿度块）交错导致时序错乱。
 ************************************************************************************************************/
#include "BFTM1.h"

/* 半秒比较值：SystemCoreClock / 2（BFTM1 已停用，保留以备复用） */
#define BFTM1_TIMER_BASE   ((u32)(SystemCoreClock / 2))

//-----------------------------------------------------------------------------
/**
  * @brief  BFTM1 初始化：配置为重复模式，0.5s 比较匹配中断
  * @note   比较值 = SystemCoreClock / 2（48MHz 时为 24000000），匹配后自动清零重计。
  *         中断里只置位 g_half_sec_flag，不调用 SPI，屏幕刷新留给主循环。
  * @retval 无
  */
void BFTM1_Configuration(void)
{
  CKCU_PeripClockConfig_TypeDef CKCUClock = {{ 0 }};

  /* 使能 BFTM1 外设时钟 */
  CKCUClock.Bit.BFTM1 = 1;
  CKCU_PeripClockConfig(CKCUClock, ENABLE);

  /* NVIC 使能 BFTM1 中断 */
  NVIC_EnableIRQ(BFTM1_IRQn);

  /* 使能比较匹配中断，设半秒比较值，清零计数并启动（默认即重复模式） */
  BFTM_IntConfig(HT_BFTM1, ENABLE);
  BFTM_SetCompare(HT_BFTM1, BFTM1_TIMER_BASE);
  BFTM_SetCounter(HT_BFTM1, 0);
  BFTM_EnaCmd(HT_BFTM1, ENABLE);
}

//-----------------------------------------------------------------------------
/**
  * @brief  BFTM1 中断服务函数：每 0.5s 触发一次
  * @note   清除中断标志后把 g_half_sec_flag 置 1，由主循环检测并清 0 后刷新冒号。
  * @retval 无
  */
void BFTM1_IRQHandler(void)
{
  /* BFTM1 已不再用于冒号闪烁（改由 RTC 0.5s 中断驱动），此处仅清标志占位。 */
  BFTM_ClearFlag(HT_BFTM1);
}

/************************************************************************************************************
 * @attention
 *
 * Firmware Disclaimer Information
 *
 * 1. The customer hereby acknowledges and agrees that the program technical documentation, including the
 *    code, which is supplied by Holtek Semiconductor Inc., (hereinafter referred to as "HOLTEK") is the
 *    proprietary and confidential intellectual property of HOLTEK, and is protected by copyright law and
 *    other intellectual property laws.
 *
 * 2. The customer hereby acknowledges and agrees that the program technical documentation, including the
 *    code, is confidential information belonging to HOLTEK, and must not be disclosed to any third parties
 *    other than HOLTEK and the customer.
 *
 * 3. The program technical documentation, including the code, is provided "as is" and for customer reference
 *    only. After delivery by HOLTEK, the customer shall use the program technical documentation, including
 *    the code, at their own risk. HOLTEK disclaims any expressed, implied or statutory warranties, including
 *    the warranties of merchantability, satisfactory quality and fitness for a particular purpose.
 *
 * <h2><center>Copyright (C) Holtek Semiconductor Inc. All rights reserved</center></h2>
 ************************************************************************************************************/
//-----------------------------------------------------------------------------
#ifndef __TIME_MODULE_H
#define __TIME_MODULE_H

#include "ht32.h"

/**
  * @brief  时间结构体定义
  * @note   用于存储年月日时分秒格式的时间信息
  *         所有字段均为无符号整型，范围如下：
  *         - year:   1970-2106（Unix时间戳覆盖范围）
  *         - month:  1-12
  *         - day:    1-31
  *         - hour:   0-23
  *         - minute: 0-59
  *         - second: 0-59
  */
typedef struct
{
  u16 year;
  u8 month;
  u8 day;
  u8 hour;
  u8 minute;
  u8 second;
} TimeStruct;

/**
  * @brief  农历日期结构体
  * @note   用于存储农历年月日信息，由 TimeModule_GetLunar 计算得到。
  *         年份为农历干支纪年（如 2026 -> 农历 2026），含闰月信息。
  */
typedef struct
{
  u16 lunarYear;      /* 农历年份（同公历年份数字，如 2026） */
  u8  lunarMonth;     /* 农历月份 1..12 */
  u8  lunarDay;       /* 农历日 1..30 */
  u8  isLeapMonth;    /* 是否为闰月：1=闰月，0=非闰月 */
} LunarDate;

/**
  * @brief  农历转换函数
  * @note   将公历日期转换为农历日期。数据表覆盖 2000-2099 年。
  *         超出数据范围返回 0 并输出"数据越界"提示。
  * @param  year:  公历年（2000-2099）
  * @param  month: 公历月 1-12
  * @param  day:   公历日 1-31
  * @param  lunar: 输出参数，计算得到的农历日期（LunarDate*）
  * @retval 0=成功；-1=年份超出 2000-2099 数据范围
  */
int TimeModule_GetLunar(u16 year, u8 month, u8 day, LunarDate *lunar);

/* 半秒相位标志（RTC CSEC 中断翻转）：0=前半秒(0-500ms)、1=后半秒(500-1000ms)。
   main 据此分时隙：前半秒只做翻页动画，后半秒做其他静态刷新。 */
extern volatile u8 g_half_phase;

/**
  * @brief  时间模块初始化函数
  * @note   优化方案：使用RTC秒中断维护时间结构体，避免每次读取都做时间戳转换
  *         初始化流程：
  *         1. 使能PWRCU备份域时钟（BKP），否则访问RTC寄存器会HardFault
  *         2. 复位RTC外设
  *         3. 检查备份寄存器判断是否首次上电
  *         4. 首次上电：设置默认时间，转换为时间戳写入RTC计数器
  *         5. 非首次上电：读取RTC计数器，转换为时间结构体（仅初始化时执行一次）
  *         6. 配置RTC时钟源为LSI，预分频32768
  *         7. 使能RTC秒中断（CNT SECOND Event）
  *         8. 使能RTC计数器
  *         性能优势：主循环读取时间仅需读取全局变量，无复杂计算
  * @retval None
  */
void TimeModule_Init(void);

/**
  * @brief  设置RTC时间
  * @note   同时更新RTC计数器和全局时间结构体
  *         操作步骤：
  *         1. 将时间转换为Unix时间戳
  *         2. 禁用RTC计数器，写入时间戳
  *         3. 重新使能RTC计数器
  *         4. 同步更新全局时间结构体
  * @param  time: 指向TimeStruct结构体的指针，包含要设置的时间
  * @retval None
  */
void TimeModule_SetTime(TimeStruct *time);

/**
  * @brief  获取当前RTC时间
  * @note   性能优化点：直接读取中断维护的全局变量，无需时间戳转换
  *         使用双读取检测防止读取过程中秒进位导致的时间不一致
  *         读取流程：
  *         1. 保存秒字段快照
  *         2. 读取所有时间字段
  *         3. 检查秒字段是否变化，若变化则重新读取
  *         确保读取到的时间是完整的一秒内的快照
  * @param  time: 指向TimeStruct结构体的指针，用于存储读取的时间
  * @retval None
  */
void TimeModule_GetTime(TimeStruct *time);

/**
  * @brief  获取当前Unix时间戳
  * @note   直接读取RTC计数器值，不进行时间格式转换
  *         Unix时间戳：从1970年1月1日 00:00:00 UTC到当前时间的秒数
  * @retval 当前Unix时间戳（秒）
  */
u32 TimeModule_GetTimestamp(void);

/* 从 RTC 硬件计数器重新同步全局时间结构体。
   【已弃用，不再被调用】：时间源是结构体 g_current_time（RTC CSEC 中断逐秒推进），
   休眠期间 CSEC 中断保持使能、结构体持续走时，唤醒后无需硬件校准。
   保留实现仅为防御（读 CNT 前已加唤醒稳定延时 + CNT=0 保护）。 */
void TimeModule_ResyncFromRTC(void);

extern vu32 g_time_update_flag;
extern vu8 g_colon_blink_flag;

#endif
//-----------------------------------------------------------------------------
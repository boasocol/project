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

extern vu32 g_time_update_flag;

#endif
//-----------------------------------------------------------------------------
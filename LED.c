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
#include "LED.h"

//-----------------------------------------------------------------------------
/**
  * @brief  LED模块初始化函数
  * @note   执行以下操作：
  *         1. 使能GPIOA外设时钟
  *         2. 配置PA0引脚为推挽输出模式
  *         3. 设置PA0引脚输出高电平（LED熄灭，假设LED为低电平有效）
  * @retval None
  */
void LED_Configuration(void)
{
  /* 定义时钟配置结构体 */
  /* 用于指定要使能的外设时钟 */
  CKCU_PeripClockConfig_TypeDef CKCUClock = {{ 0 }};
  
  /* 设置PA位为1，使能GPIOA外设时钟 */
  /* 每个GPIO端口都需要独立使能时钟才能使用 */
  CKCUClock.Bit.PA = 1;
  
  /* 使能GPIOA外设时钟 */
  /* CKCU（Clock Control Unit）负责管理所有外设的时钟开关 */
  CKCU_PeripClockConfig(CKCUClock, ENABLE);
  
  /* 配置PA0引脚为输出方向 */
  /* GPIO_DIR_OUT: 输出模式 */
  /* GPIO_DIR_IN: 输入模式 */
  GPIO_DirectionConfig(HT_GPIOA, GPIO_PIN_0, GPIO_DIR_OUT);
  
  /* 设置PA0引脚输出高电平 */
  /* 如果LED是低电平点亮（共阴极），则此时LED熄灭 */
  GPIO_SetOutBits(HT_GPIOA, GPIO_PIN_0);
}

//-----------------------------------------------------------------------------
/**
  * @brief  LED状态翻转函数
  * @note   读取PA0当前输出状态，然后取反
  *         - 如果当前是高电平，则变为低电平（点亮LED）
  *         - 如果当前是低电平，则变为高电平（熄灭LED）
  * @retval None
  */
void LED_Toggle(void)
{
  /* 读取PA0引脚当前输出状态 */
  /* 返回SET（高电平）或RESET（低电平） */
  if(GPIO_ReadOutBit(HT_GPIOA, GPIO_PIN_0))
  {
    /* 当前是高电平，设置为低电平 */
    /* 假设LED为低电平有效，此时LED点亮 */
    GPIO_ClearOutBits(HT_GPIOA, GPIO_PIN_0);
  }
  else
  {
    /* 当前是低电平，设置为高电平 */
    /* 假设LED为低电平有效，此时LED熄灭 */
    GPIO_SetOutBits(HT_GPIOA, GPIO_PIN_0);
  }
}
//-----------------------------------------------------------------------------
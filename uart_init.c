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
#include "uart_init.h"

//-----------------------------------------------------------------------------
/**
  * @brief  UART0初始化函数
  * @note   完整配置UART0串口通信，包括：
  *         1. 使能GPIOB和UART0外设时钟
  *         2. 配置PB7为UART0_TX功能（AFIO_MODE_6）
  *         3. 配置PB8为UART0_RX功能（AFIO_MODE_6）
  *         4. 配置UART0参数（波特率9600，8位数据，1位停止，无校验）
  *         5. 使能UART0发送和接收
  *         6. 配置printf重定向到UART0
  * @note   使用PB7/PB8作为UART0引脚，避免使用PA9（boot引脚）和PB6（QFN32封装不存在）
  * @retval None
  */
void UART0_Configuration(void)
{
  CKCU_PeripClockConfig_TypeDef CKCUClock = {{0}};
  USART_InitTypeDef USART_InitStructure;
  
  /* 使能GPIOB、UART0和AFIO外设时钟 */
  /* GPIOB用于UART0的TX/RX引脚（PB7=TX, PB8=RX） */
  /* UART0外设需要时钟才能工作 */
  /* AFIO时钟必须使能，否则访问AFIO寄存器会导致HardFault */
  CKCUClock.Bit.PB     = 1;    // 使能GPIOB时钟
  CKCUClock.Bit.UART0  = 1;    // 使能UART0时钟
  CKCUClock.Bit.AFIO   = 1;    // 使能AFIO时钟（必须！否则AFIO_GPxConfig会HardFault）
  CKCU_PeripClockConfig(CKCUClock, ENABLE);
  
  /* 配置PB7为UART0_TX功能 */
  /* AFIO_FUN_USART_UART = AFIO_MODE_6，用于USART/UART功能 */
  /* PB7是UART0的替代TX引脚（QFN32封装无PB6，使用PB7替代） */
  AFIO_GPxConfig(GPIO_PB, AFIO_PIN_7, AFIO_FUN_USART_UART);
  
  /* 配置PB8为UART0_RX功能 */
  /* PB8是UART0的替代RX引脚 */
  AFIO_GPxConfig(GPIO_PB, AFIO_PIN_8, AFIO_FUN_USART_UART);
  
  /* 配置UART0参数 */
  /* 主频8MHz时，9600波特率更稳定可靠 */
  /* 8MHz / 9600 ≈ 833，分频误差更小 */
  USART_InitStructure.USART_BaudRate = 9600;        // 波特率9600
  USART_InitStructure.USART_WordLength = USART_WORDLENGTH_8B;  // 8位数据位
  USART_InitStructure.USART_StopBits = USART_STOPBITS_1;      // 1位停止位
  USART_InitStructure.USART_Parity = USART_PARITY_NO;         // 无校验位
  USART_InitStructure.USART_Mode = USART_MODE_NORMAL;         // 正常模式
  
  /* 初始化UART0 */
  USART_Init(HT_UART0, &USART_InitStructure);
  
  /* 使能UART0发送 */
  USART_TxCmd(HT_UART0, ENABLE);
  
  /* 使能UART0接收 */
  USART_RxCmd(HT_UART0, ENABLE);
  
  /* 调用RETARGET配置，设置printf重定向 */
  /* RETARGET_Configuration()会配置fputc/fgetc指向UART0 */
  /* 底层通过SERIAL_PutChar()/SERIAL_GetChar()实现 */
  RETARGET_Configuration();
}
//-----------------------------------------------------------------------------
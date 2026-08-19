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
#include "hd9803.h"    /* HD9803_ENABLE：蓝牙启用时 printf 调试口让位 */

//-----------------------------------------------------------------------------
/**
  * @brief  UART0初始化函数
  * @note   完整配置UART0串口通信，包括：
  *         1. 使能GPIOB和UART0外设时钟
  *         2. 配置PB7为UART0_TX功能（AFIO_MODE_6）
  *         3. 配置PB8为UART0_RX功能（AFIO_MODE_6）
  *         4. 配置UART0参数（波特率115200，8位数据，1位停止，无校验）
  *         5. 使能UART0发送和接收
  *         6. 配置printf重定向到UART0
  * @note   使用PB7/PB8作为UART0引脚，避免使用PA9（boot引脚）和PB6（QFN32封装不存在）
  * @retval None
  */
void UART0_Configuration(void)
{
  CKCU_PeripClockConfig_TypeDef CKCUClock = {{0}};
  USART_InitTypeDef USART_InitStructure;

#if !HD9803_ENABLE
  /* ========== printf 调试口 ==========
     USART0(PC4/PC5) @115200 —— 原蓝牙口。蓝牙(HD9803)禁用时作为串口调试打印；
     蓝牙启用时(HD9803_ENABLE=1) USART0 归蓝牙，printf 无口（调试与蓝牙二选一），
     此时此函数不初始化 USART0，防与蓝牙冲突。 */
  CKCUClock.Bit.PC      = 1;    // 使能GPIOC时钟
  CKCUClock.Bit.USART0  = 1;    // 使能USART0时钟
  CKCUClock.Bit.AFIO    = 1;    // 使能AFIO时钟（必须！否则AFIO_GPxConfig会HardFault）
  CKCU_PeripClockConfig(CKCUClock, ENABLE);

  AFIO_GPxConfig(GPIO_PC, AFIO_PIN_4, AFIO_FUN_USART_UART);   /* PC4 = USART_TX */
  AFIO_GPxConfig(GPIO_PC, AFIO_PIN_5, AFIO_FUN_USART_UART);   /* PC5 = USART_RX */

  USART_InitStructure.USART_BaudRate = 115200;
  USART_InitStructure.USART_WordLength = USART_WORDLENGTH_8B;
  USART_InitStructure.USART_StopBits = USART_STOPBITS_1;
  USART_InitStructure.USART_Parity = USART_PARITY_NO;
  USART_InitStructure.USART_Mode = USART_MODE_NORMAL;
  USART_Init(HT_USART0, &USART_InitStructure);

  USART_TxCmd(HT_USART0, ENABLE);
  USART_RxCmd(HT_USART0, ENABLE);

  /* RETARGET_PORT=12 已指向 USART0（conf.h），fputc 经 SERIAL_PutChar 发 USART0 */
  RETARGET_Configuration();
#else
  /* 蓝牙启用：printf 调试口让位，不初始化任何串口 */
  (void)CKCUClock; (void)USART_InitStructure;
#endif
}
//-----------------------------------------------------------------------------

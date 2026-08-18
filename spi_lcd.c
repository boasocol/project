/************************************************************************************************************
 * @file    spi_lcd.c
 * @brief   LCD 专用 SPI 通信层（与显示驱动分离）
 * @note    仅负责通过 SPI 把字节发送到屏幕（3 线 9bit：D8=D/C）。
 *          所有 ST7789 命令/绘图逻辑都在 Display.c，本文件不依赖它们。
 ************************************************************************************************************/
#include "spi_lcd.h"
#include "BFTM0.h"

//-----------------------------------------------------------------------------
/**
 * @brief  SPI 发送一个字节（带 D/C 位）
 * @note   通过数据最高位(D8)区分命令/数据：D8=0 命令，D8=1 数据。
 *         SPI 配置为 9 位数据宽度，D8 为 DC 位，D0-D7 为实际数据。
 * @param  data   : 要发送的 8 位数据（低 8 位）
 * @param  dc_bit : D/C 位（0=命令，1=数据）
 * @retval 0=成功，1=超时
 */
static u8 SPI_LCD_SPI_SendByte(u8 data, u8 dc_bit)
{
  u32 timeout = 100000;
  /* FIFO 模式下等 TX FIFO 有空位（深度 8）；等待时顺手清 RX，防 RX FIFO 溢出停 SPI */
  while (SPI_GetFIFOStatus(SPI_LCD_SPI, SPI_FIFO_TX) >= 8u)
  {
    if (--timeout == 0) { return 1; }
    if (SPI_GetFlagStatus(SPI_LCD_SPI, SPI_FLAG_RXBNE) != RESET)
      (void)SPI_ReceiveData(SPI_LCD_SPI);
  }

  SPI_SendData(SPI_LCD_SPI, ((u16)dc_bit << 8) | data);

  timeout = 100000;
  while (SPI_GetFlagStatus(SPI_LCD_SPI, SPI_FLAG_RXBNE) == RESET)
  {
    if (--timeout == 0) { return 1; }
  }

  (void)SPI_ReceiveData(SPI_LCD_SPI);
  return 0;
}

//-----------------------------------------------------------------------------
/**
 * @brief  等待 SPI 发送完成（TXE 与 BUSY 标志）
 * @retval 0=成功，1=超时
 */
static u8 SPI_LCD_SPI_WaitDone(void)
{
  u32 timeout = 100000;
  while ((SPI_GetFlagStatus(SPI_LCD_SPI, SPI_FLAG_TXE) == RESET) && (timeout--));
  if (timeout == 0) { return 1; }

  timeout = 100000;
  while ((SPI_GetFlagStatus(SPI_LCD_SPI, SPI_FLAG_BUSY) != RESET) && (timeout--));
  if (timeout == 0) { return 1; }

  return 0;
}

//-----------------------------------------------------------------------------
/**
 * @brief  拉低片选（软件 SEL），开始一次 SPI 传输会话
 * @retval 无
 */
void SPI_LCD_Select(void)
{
  SPI_SoftwareSELCmd(SPI_LCD_SPI, SPI_SEL_ACTIVE);
}

//-----------------------------------------------------------------------------
/**
 * @brief  等待发送完毕后释放片选，结束本次 SPI 传输会话
 * @retval 无
 */
void SPI_LCD_Deselect(void)
{
  SPI_LCD_SPI_WaitDone();
  SPI_SoftwareSELCmd(SPI_LCD_SPI, SPI_SEL_INACTIVE);
}

//-----------------------------------------------------------------------------
/**
 * @brief  发送一个 ST7789 命令字节（D/C 位=0）
 * @param  cmd : 命令码（如 0x2A CASET）
 * @retval 无
 */
void SPI_LCD_WriteCmd(u8 cmd)
{
  SPI_LCD_SPI_SendByte(cmd, 0);
}

//-----------------------------------------------------------------------------
/**
 * @brief  连续发送多个数据字节（D/C 位=1）
 * @param  buf : [输入] 数据缓冲区指针
 * @param  len : 字节数
 * @retval 无
 */
void SPI_LCD_WriteData(const u8 *buf, u32 len)
{
  /* FIFO 批量发送：每批 8 字节才轮询一次 TX FIFO（深度 8），RX 及时收走防溢出。
     注意：本 SPI 配 RXFTLS=0，RXBNE 标志恒为 1（RX FIFO 空也置位），
     因此收 RX 一律用 SPI_GetFIFOStatus(RX) > 0 判断真实计数，不能用 while(RXBNE)。 */
  while (len > 0)
  {
    /* 等 TX FIFO 有空位（<8）再填一批；等待时用 FIFO 计数收走 RX 防溢出 */
    while (SPI_GetFIFOStatus(SPI_LCD_SPI, SPI_FIFO_TX) >= 8u)
    {
      if (SPI_GetFIFOStatus(SPI_LCD_SPI, SPI_FIFO_RX) > 0u)
        (void)SPI_ReceiveData(SPI_LCD_SPI);
    }
    u32 n = (len >= 8u) ? 8u : len;
    for (u32 k = 0; k < n; k++)
    {
      SPI_SendData(SPI_LCD_SPI, ((u16)1u << 8) | *buf);
      buf++;
    }
    len -= n;
    /* 读走已到的 RX 帧（FIFO 计数判断，空即退出） */
    while (SPI_GetFIFOStatus(SPI_LCD_SPI, SPI_FIFO_RX) > 0u)
      (void)SPI_ReceiveData(SPI_LCD_SPI);
  }
  /* 等所有数据从 TX FIFO 移出（FIFO 保序；Deselect 的 WaitDone 再兜底等线上发完） */
  while (SPI_GetFlagStatus(SPI_LCD_SPI, SPI_FLAG_TXE) == RESET);
}

//-----------------------------------------------------------------------------
/**
 * @brief  发送单个数据字节（D/C 位=1）
 * @param  data : 数据字节（像素色高/低字节、命令参数等）
 * @retval 无
 */
void SPI_LCD_WriteDataByte(u8 data)
{
  SPI_LCD_SPI_SendByte(data, 1);
}

//-----------------------------------------------------------------------------
/**
 * @brief  毫秒级阻塞延时（基于 BFTM0 的 1ms 节拍计数器 bftm0_ct）
 * @param  ms : 延时毫秒数
 * @retval 无
 */
void Delay(u32 ms)
{
  vu32 target = bftm0_ct + ms;
  while (bftm0_ct < target);
}

//-----------------------------------------------------------------------------
/**
 * @brief  SPI 通信层初始化：时钟、引脚、SPI 外设、硬件复位
 * @note   仅建立“能通信”的硬件环境；ST7789 寄存器配置由 Display_Init() 完成。
 */
void SPI_LCD_Init(void)
{
  /* 1. 使能外设时钟 */
  CKCU_PeripClockConfig_TypeDef CKCUClock = {{0}};
  CKCUClock.Bit.SPI0 = 1;
  CKCUClock.Bit.AFIO = 1;
  CKCUClock.Bit.PA   = 1;
  CKCUClock.Bit.PB   = 1;
  CKCU_PeripClockConfig(CKCUClock, ENABLE);

  /* 2. 配置 SPI 引脚为 AFIO 复用 */
  /* PA4: SPI0_SCK，PA5: SPI0_MOSI，PB2: SPI0_SEL（软件控制） */
  GPIO_PullResistorConfig(SPI_LCD_BL_GPIO, AFIO_PIN_2, GPIO_PR_UP);  /* CS 上拉 */
  AFIO_GPxConfig(GPIO_PA, AFIO_PIN_4, AFIO_FUN_SPI);
  AFIO_GPxConfig(GPIO_PA, AFIO_PIN_5, AFIO_FUN_SPI);
  AFIO_GPxConfig(GPIO_PB, AFIO_PIN_2, AFIO_FUN_SPI);

  /* 3. 配置控制引脚 RST / BL */
  GPIO_DirectionConfig(SPI_LCD_RST_GPIO, SPI_LCD_RST_PIN, GPIO_DIR_OUT);
  GPIO_DirectionConfig(SPI_LCD_BL_GPIO, SPI_LCD_BL_PIN, GPIO_DIR_OUT);
  GPIO_DriveConfig(SPI_LCD_BL_GPIO, SPI_LCD_BL_PIN, GPIO_DV_16MA);
  GPIO_WriteOutBits(SPI_LCD_BL_GPIO, SPI_LCD_BL_PIN, RESET);  /* 低电平导通背光 */
  GPIO_WriteOutBits(SPI_LCD_RST_GPIO, SPI_LCD_RST_PIN, SET);  /* RST 初始高 */

  /* 4. SPI 初始化配置（主模式、9 位、软件 CS、CPOL=0、CPHA=0） */
  SPI_InitTypeDef SPI_InitStructure;
  SPI_InitStructure.SPI_Mode = SPI_MASTER;
  SPI_InitStructure.SPI_FIFO = SPI_FIFO_ENABLE;   /* 启用 FIFO：批量填充，吞吐接近标称 */
  SPI_InitStructure.SPI_DataLength = SPI_DATALENGTH_9;
  SPI_InitStructure.SPI_SELMode = SPI_SEL_SOFTWARE;
  SPI_InitStructure.SPI_SELPolarity = SPI_SELPOLARITY_LOW;
  SPI_InitStructure.SPI_CPOL = SPI_CPOL_LOW;
  SPI_InitStructure.SPI_CPHA = SPI_CPHA_FIRST;
  SPI_InitStructure.SPI_FirstBit = SPI_FIRSTBIT_MSB;
  SPI_InitStructure.SPI_RxFIFOTriggerLevel = 0;
  SPI_InitStructure.SPI_TxFIFOTriggerLevel = 0;
  /* 分频系数：fSCK = fPCLK / prescaler（库内 CPR=(prescaler/2)-1，硬件 fSCK=fPCLK/((CPR+1)*2)）。
   * 48MHz / 12MHz = 4，即 fPCLK/4；硬件最高速为 fPCLK/2(=24MHz)。
   * 注意：不要再乘 2，否则实际速率减半（旧代码曾因此只跑到目标的一半）。 */
  SPI_InitStructure.SPI_ClockPrescaler = SystemCoreClock / SPI_LCD_SPI_BAUD;

  SPI_Init(SPI_LCD_SPI, &SPI_InitStructure);
  SPI_SELOutputCmd(SPI_LCD_SPI, ENABLE);
  SPI_Cmd(SPI_LCD_SPI, ENABLE);

  /* 5. 硬件复位 */
  GPIO_WriteOutBits(SPI_LCD_RST_GPIO, SPI_LCD_RST_PIN, RESET);
  Delay(5);
  GPIO_WriteOutBits(SPI_LCD_RST_GPIO, SPI_LCD_RST_PIN, SET);
  Delay(10);
}

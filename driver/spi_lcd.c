/************************************************************************************************************
 * @file    spi_lcd.c
 * @brief   LCD 专用 SPI 通信层（与显示驱动分离）
 * @note    仅负责通过 SPI 把字节发送到屏幕（3 线 9bit：D8=D/C）。
 *          所有 ST7789 命令/绘图逻辑都在 Display.c，本文件不依赖它们。
 *
 *          传输方式由 bsp_pins.h 的 SPI_LCD_PDMA_ENABLE 二选一（2026-10-27 照项目 8
 *          HT32L52241 已验证 DMA 方案移植）：
 *            =1 非 FIFO + PDMA TX（官方唯一 DMA 组合）：SPI_FIFO_DISABLE + PDMA_CH1(TX, VH)；
 *               只配 TX、不配 RX PDMA——TFT 是 3-line 9bit 只写屏，MISO 不回读，RX 由等待
 *               循环手动排空（FIFO+PDMA 曾因 RXFTLS=0 → RXBNE 恒置位 → RX 请求风暴(VH)
 *               饿死 TX 卡死，见 notes 坑#88 修正4b/4c）。
 *            =0 FIFO 批量轮询（旧实现，回退用）。
 ************************************************************************************************************/
#include "spi_lcd.h"
#include "BFTM0.h"

#if (SPI_LCD_PDMA_ENABLE == 1)
/* ==================== 非 FIFO + PDMA（官方唯一 DMA 组合，照项目 8） ==================== */
#define SPI_LCD_DMA_MAX   256u        /* 每批 PDMA 最多帧数（staging 256×2B=512B） */
static u16 s_dma[SPI_LCD_DMA_MAX];    /* 9bit 帧 staging：bit8=DC 预置（数据=1） */

/* 单帧（命令/单字节数据）：非 FIFO 下等 TXBE → 写 DR → 等本帧回波并排 RX。
 * ⚠️ 先排干历史 RX：非 FIFO 每发一帧必回读一字节，上一批 PDMA 的最后一帧回波可能
 *   残留 RX 缓存（TX 与 RX 完成有先后），不清掉会导致本帧回波判断读到旧数据。 */
static u8 SPI_LCD_SPI_SendByte(u8 data, u8 dc_bit)
{
  u32 timeout;
  if (SPI_LCD_SPI->SR & SPI_FLAG_RXBNE) (void)SPI_LCD_SPI->DR;   /* 排旧回波 */
  timeout = 100000;
  while ((SPI_LCD_SPI->SR & SPI_FLAG_TXBE) == RESET)
  {
    if (--timeout == 0) { return 1; }
  }
  SPI_LCD_SPI->DR = ((u16)dc_bit << 8) | data;
  timeout = 100000;
  while ((SPI_LCD_SPI->SR & SPI_FLAG_RXBNE) == RESET)            /* 本帧移出完成 */
  {
    if (--timeout == 0) { return 1; }
  }
  (void)SPI_LCD_SPI->DR;
  return 0;
}

/* 等发送完成（TXE 且 BUSY 清）——Deselect 前必须等最后帧移出 */
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

void SPI_LCD_Select(void)
{
  SPI_SoftwareSELCmd(SPI_LCD_SPI, SPI_SEL_ACTIVE);
}

void SPI_LCD_Deselect(void)
{
  SPI_LCD_SPI_WaitDone();
  SPI_SoftwareSELCmd(SPI_LCD_SPI, SPI_SEL_INACTIVE);
}

void SPI_LCD_WriteCmd(u8 cmd)
{
  SPI_LCD_SPI_SendByte(cmd, 0);        /* D8=0 -> 命令 */
}

void SPI_LCD_WriteDataByte(u8 data)
{
  SPI_LCD_SPI_SendByte(data, 1);       /* D8=1 -> 数据 */
}

/* PDMA 直发已打包的 16bit 帧数组（bit8=DC 由调用方预置）：一次全发（BlkCnt=n, BlkLen=1），
   跳过 WriteData 的 staging 展开 + 分批（整行 640 帧 1 批），上传提速（2026-10-28）。
   ⚠️ 同步点取 TX 完成（CBLKCNT 到 0）；RX 手动排空（非 FIFO 每帧必回读一字节）。
   ⚠️ BlkLen 保持 1：曾试 BlkLen=8 实机花屏（PDMA 块间行为 + SPI 非 FIFO 交互），回退。 */
void SPI_LCD_WriteFrames16(const u16 *frames, u32 n)
{
  SPI_LCD_WriteFrames16Start(frames, n);
  SPI_LCD_WriteFrames16Wait();
}

/* 启动 TX PDMA（不等待）：流水线用——启动后 CPU 可并行读 Flash/打包（2026-10-28） */
void SPI_LCD_WriteFrames16Start(const u16 *frames, u32 n)
{
  if (n == 0u) return;
  PDMA_SrcAddrConfig(PDMA_SPI0_TX, (u32)frames);
  PDMA_TranSizeConfig(PDMA_SPI0_TX, (u16)n, 1u);
  PDMA_EnaCmd(PDMA_SPI0_TX, ENABLE);
}

/* 等 TX PDMA 完成（CBLKCNT=0）+ 停通道。RX 由 RxDrainStart 的 DMA 排空；此处兜底顺排
   （防 RX DMA 未生效/残留，无碍——RX 数据本就丢弃）。 */
void SPI_LCD_WriteFrames16Wait(void)
{
  u32 timeout = 1000000u;
  while (PDMA_GetRemainBlkCnt(PDMA_SPI0_TX) != 0u)
  {
    while (SPI_LCD_SPI->SR & SPI_FLAG_RXBNE) (void)SPI_LCD_SPI->DR;  /* 兜底排 RX */
    if (--timeout == 0u) { break; }
  }
  PDMA_EnaCmd(PDMA_SPI0_TX, DISABLE);
}

/* SPI0 RX 回显 PDMA 排空（CH0，非 FIFO 每帧一字节回读）：写屏期间硬件清 RX，CPU 不占，
   流水线必需（CPU 阻塞读 Flash 时 SPI0 RX 不溢出）。非 FIFO 下 RXBNE 每帧置位一次
   （非恒置位）→ 无 FIFO+PDMA 的请求风暴（坑#88 修正4b 场景）。RX 优先级 H（低于 TX VH，
   防 TX 被饿死）。 */
void SPI_LCD_RxDrainStart(void)
{
  static u16 s_rx_drain;   /* RX 丢弃缓冲（DST_FIX 覆盖写） */
  PDMACH_InitTypeDef pdi;
  pdi.PDMACH_SrcAddr  = (u32)&SPI_LCD_SPI->DR;   /* 读=RX（HT32 SPI 单一 DR） */
  pdi.PDMACH_DstAddr  = (u32)&s_rx_drain;
  pdi.PDMACH_BlkCnt   = 1024;                    /* ≥ 整行 640 帧 + 余量 */
  pdi.PDMACH_BlkLen   = 1;
  pdi.PDMACH_DataSize = WIDTH_16BIT;
  pdi.PDMACH_Priority = H_PRIO;
  pdi.PDMACH_AdrMod   = SRC_ADR_FIX | DST_ADR_FIX;
  PDMA_Config(PDMA_SPI0_RX, &pdi);
  SPI_PDMACmd(SPI_LCD_SPI, SPI_PDMAREQ_RX, ENABLE);
  PDMA_EnaCmd(PDMA_SPI0_RX, ENABLE);
}

void SPI_LCD_RxDrainStop(void)
{
  PDMA_EnaCmd(PDMA_SPI0_RX, DISABLE);
  SPI_PDMACmd(SPI_LCD_SPI, SPI_PDMAREQ_RX, DISABLE);
}

void SPI_LCD_WriteData(const u8 *buf, u32 len)
{
  /* 每批最多 SPI_LCD_DMA_MAX 帧：先展开为 16bit 帧（bit8=DC=1）再 PDMA 到 SPI0->DR。
   * ⚠️ 同步点取 TX 完成（CBLKCNT 到 0），不要用 RX：TFT 3-line 9bit 只写屏，MISO 不回读
   *    → RX 无有效数据 → RX PDMA 请求不产生/不可靠；RX 由本循环手动排空（非 FIFO
   *    每发一帧必回读一字节，不读会 RO 溢出标志堆积）。末批帧物理移出由 SPI 自行完成，
   *    最后再 SPI_LCD_SPI_WaitDone()（TXE+BUSY）等末帧真正移出。
   * ⚠️ 每批必须重设源地址：AUTORL=0（默认）时传输完成后通道「当前地址」保持末尾不
   *    回卷，若沿用上次 SADR 下一批会从中途开始读。
   * 2026-10-28：RX 排空改「排空已到帧」内联 while（data 提速）；
   *   ⚠️ BlkLen 保持 1（每帧一块）：曾试 BlkLen=8（8 帧/块）data 65.5ms 但实机花屏
   *   （PDMA 块间行为 + SPI 非 FIFO 交互问题），回退 BlkLen=1 保稳定（2026-10-28）。 */
  while (len > 0u)
  {
    u32 n = (len >= SPI_LCD_DMA_MAX) ? SPI_LCD_DMA_MAX : len;
    u32 k;
    for (k = 0; k < n; k++) s_dma[k] = (u16)(0x0100u | buf[k]);

    PDMA_SrcAddrConfig(PDMA_SPI0_TX, (u32)s_dma);
    PDMA_TranSizeConfig(PDMA_SPI0_TX, (u16)n, 1u);
    PDMA_EnaCmd(PDMA_SPI0_TX, ENABLE);
    {
      u32 timeout = 1000000u;
      while (PDMA_GetRemainBlkCnt(PDMA_SPI0_TX) != 0u)   /* TX CBLKCNT 递减到 0 = 本批搬完 */
      {
        while (SPI_LCD_SPI->SR & SPI_FLAG_RXBNE) (void)SPI_LCD_SPI->DR;  /* 排空已到 RX */
        if (--timeout == 0u) { break; }
      }
    }
    PDMA_EnaCmd(PDMA_SPI0_TX, DISABLE);
    buf += n; len -= n;
  }
  /* 不在此等 TXE/BUSY（2026-10-28：Deselect 已有 WaitDone；行间保序靠非 FIFO 的
     TXBE——下一命令/数据写 DR 前必等上一帧移出，故整行物理移出由后续操作自然串行，
     去掉每行 WaitDone 省 ~20µs/行）。 */
}

void SPI_LCD_Init(void)
{
  /* 1. 使能外设时钟（含 PDMA：非 FIFO + PDMA 组合必需） */
  CKCU_PeripClockConfig_TypeDef CKCUClock = {{0}};
  CKCUClock.Bit.SPI0 = 1;
  CKCUClock.Bit.AFIO = 1;
  CKCUClock.Bit.PA   = 1;
  CKCUClock.Bit.PB   = 1;
  CKCUClock.Bit.PDMA = 1;
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

  /* 4. SPI 初始化配置（主模式、9 位、软件 CS、CPOL=0、CPHA=0、非 FIFO=官方唯一 PDMA 组合） */
  SPI_InitTypeDef SPI_InitStructure;
  SPI_InitStructure.SPI_Mode = SPI_MASTER;
  SPI_InitStructure.SPI_FIFO = SPI_FIFO_DISABLE;   /* 非 FIFO：官方唯一 PDMA 组合 */
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

  /* 5. PDMA TX 通道（PDMA_SPI0_TX = CH1）：SRAM -> SPI0->DR，16bit/帧，每帧 1 块。
   *    ⚠️ 只配 TX、只开 TX 请求：TFT 只写屏，MISO 不回读 → RX PDMA 不可靠；RX 由
   *    WriteData 等待循环手动排空。TX 优先级 VH（防其他外设请求饿死 TX）。 */
  {
    PDMACH_InitTypeDef PDMACH_InitStructure;
    PDMACH_InitStructure.PDMACH_SrcAddr   = (u32)s_dma;
    PDMACH_InitStructure.PDMACH_DstAddr   = (u32)&SPI_LCD_SPI->DR;
    PDMACH_InitStructure.PDMACH_BlkCnt    = SPI_LCD_DMA_MAX;
    PDMACH_InitStructure.PDMACH_BlkLen    = 1;
    PDMACH_InitStructure.PDMACH_DataSize  = WIDTH_16BIT;
    PDMACH_InitStructure.PDMACH_Priority  = VH_PRIO;
    PDMACH_InitStructure.PDMACH_AdrMod    = SRC_ADR_LIN_INC | DST_ADR_FIX;
    PDMA_Config(PDMA_SPI0_TX, &PDMACH_InitStructure);
  }

  /* 使能 SPI0 TX PDMA 请求（官方示例同款：非 FIFO + PDMA；RX 不开） */
  SPI_PDMACmd(SPI_LCD_SPI, SPI_PDMAREQ_TX, ENABLE);

  /* 6. 硬件复位 */
  GPIO_WriteOutBits(SPI_LCD_RST_GPIO, SPI_LCD_RST_PIN, RESET);
  Delay(5);
  GPIO_WriteOutBits(SPI_LCD_RST_GPIO, SPI_LCD_RST_PIN, SET);
  Delay(10);
}

#else /* SPI_LCD_PDMA_ENABLE == 0：FIFO 批量轮询（旧实现，回退） */

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

#endif /* SPI_LCD_PDMA_ENABLE */

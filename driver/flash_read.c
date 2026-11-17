/************************************************************************************************************
 * @file    flash_read.c
 * @brief   SPI1 读取外部 NOR Flash 实现。
 * @note    独立编译单元（project 源组里单独勾选）。
 *          引脚：PA3=CS(GPIO), PA0=SCK(AFIO), PA1=MOSI(AFIO), PA2=MISO(AFIO)。
 *          SPI1 默认脚，需释放 PA0(原LED已移除)、PA1(已改PB5为RST)。
 *          SPI Mode 3 (CPOL=HIGH, CPHA=SECOND)，兼容市面多数 SPI NOR Flash。
 *          方案 A：全程 16-bit 单线（DFL=0x0）。命令/地址/数据均按 16-bit 帧
 *          收发（MSB 先传），无 DUALEN 切换；每帧 2 字节仅 2 次轮询，比 8-bit
 *          每字节 2 次轮询减半。
 ************************************************************************************************************/
#include "flash_read.h"
#include "bsp_pins.h"   /* FLASH_* 引脚/SPI 映射（P1 集中，2026-09-03） */
#include <stdio.h>
#include <string.h>

/* ==================== 片选宏（引脚见 bsp_pins.h：FLASH_CS_PIN=PA3） ==================== */
#define FLASH_CS_LOW()   GPIO_ClearOutBits(HT_GPIOA, FLASH_CS_PIN)
#define FLASH_CS_HIGH()  GPIO_SetOutBits(HT_GPIOA, FLASH_CS_PIN)

/* 双线读自检标志（Flash_Init 中校验 0x3B 后置位；UI 动画背景据此选单/双线） */
static u8 g_flash_dual_ok = 0;

/* ==================== 底层收发（直接寄存器版，2026-10-28：避开库函数 Assert+调用开销） ==================== */
/**
 * @brief  通过 SPI1 发送一个 16-bit 帧并读回。
 * @note   方案 A：全程 16-bit 单线（DFL=0x0，16 个 SCK/帧，MSB 先传）。
 *         一次收发 2 字节只做 2 次轮询（TXBE/RXBNE），比 8-bit 每字节 2 次减半。
 *         ⚠️ 直接操作 HT_SPI1->SR/DR：库函数 SPI_SendData/SPI_ReceiveData 每次调用
 *         带 Assert 检查+调用开销，批量读 640B/行（320 帧）会放大数倍（read 112ms
 *         瓶颈之一，2026-10-28 实测）。HT32 SPI 单一 DR（写=TX 读=RX），无独立 TXDR/RXDR。
 * @param  data : 要发送的 16-bit 数据（高字节先发）
 * @retval 接收到的 16-bit 数据（高字节先收）
 */
static u16 Flash_SPI_SendFrame(u16 data)
{
  while ((HT_SPI1->SR & SPI_FLAG_TXBE) == RESET);
  HT_SPI1->DR = data;
  while ((HT_SPI1->SR & SPI_FLAG_RXBNE) == RESET);
  return (u16)HT_SPI1->DR;
}

/* ==================== 初始化 ==================== */
/**
 * @brief  初始化 SPI1，连接外部 NOR Flash。
 * @note   时钟 -> CS(GPIO) -> SCK/MOSI/MISO(AFIO) -> SPI Mode3 -> 读 JEDEC ID 校验。
 * @retval 1=初始化成功，0=故障
 */
u32 Flash_Init(void)
{
  SPI_InitTypeDef SPI_InitStructure;

  /* ---- 1. 时钟 ---- */
  CKCU_PeripClockConfig_TypeDef CKCUClock = {{0}};
  CKCUClock.Bit.SPI1 = 1;
  CKCUClock.Bit.AFIO = 1;
  CKCUClock.Bit.PA   = 1;  /* PA0-PA3 用于 SPI1 */
  CKCU_PeripClockConfig(CKCUClock, ENABLE);

  /* ---- 2. CS 脚 (GPIO 输出, PA3) ---- */
  GPIO_SetOutBits(HT_GPIOA, FLASH_CS_PIN);
  GPIO_DirectionConfig(HT_GPIOA, FLASH_CS_PIN, GPIO_DIR_OUT);

  /* ---- 3. SCK(PA0) / MOSI(PA1) / MISO(PA2) 复用为 SPI ---- */
  AFIO_GPxConfig(GPIO_PA, AFIO_PIN_0, AFIO_FUN_SPI);
  AFIO_GPxConfig(GPIO_PA, AFIO_PIN_1, AFIO_FUN_SPI);
  AFIO_GPxConfig(GPIO_PA, AFIO_PIN_2, AFIO_FUN_SPI);

  /* ---- 4. SPI 配置：主机，16bit，Mode 3，软件 CS ----
         方案 A：全程 16-bit 单线。DFL=0x0（SPI_DATALENGTH_16），
         命令/地址/数据均按 16-bit 帧收发，无 DUALEN 切换，轮询次数减半。 */
  SPI_InitStructure.SPI_Mode           = SPI_MASTER;
  /* FIFO 模式 + RXFTLS=1：RX FIFO 满 1 帧即置 RXBNE（逐帧读、批量填，轮询减到 1/8）。
     曾因 RXFTLS=0 导致 JEDEC 隔帧读到 0，故显式设 1。 */
  SPI_InitStructure.SPI_FIFO           = SPI_FIFO_ENABLE;
  SPI_InitStructure.SPI_DataLength     = SPI_DATALENGTH_16;
  SPI_InitStructure.SPI_SELMode        = SPI_SEL_SOFTWARE;
  SPI_InitStructure.SPI_SELPolarity    = SPI_SELPOLARITY_LOW;
  SPI_InitStructure.SPI_FirstBit       = SPI_FIRSTBIT_MSB;
  SPI_InitStructure.SPI_CPOL           = SPI_CPOL_HIGH;
  SPI_InitStructure.SPI_CPHA           = SPI_CPHA_SECOND;
  SPI_InitStructure.SPI_RxFIFOTriggerLevel = 1;
  SPI_InitStructure.SPI_TxFIFOTriggerLevel = 0;
  SPI_InitStructure.SPI_ClockPrescaler = 2;   /* 48MHz / 2 = 24MHz */

  SPI_Init(HT_SPI1, &SPI_InitStructure);
  SPI_SELOutputCmd(HT_SPI1, ENABLE);
  SPI_Cmd(HT_SPI1, ENABLE);

  printf("[FLASH] SPI1 init done, prescaler=2 => 24MHz, 16-bit frames\r\n");

  /* ---- 诊断：读 JEDEC ID 验证 SPI 链路（方案 A 16-bit 帧） ---- */
  {
    u32 id = Flash_ReadJEDECID();
    printf("[FLASH] JEDEC ID = %06X  expect %06X  -> %s\r\n", (unsigned)id,
           (unsigned)C25VQ16E_JEDEC_ID,
           (id == C25VQ16E_JEDEC_ID) ? "MATCH" : "MISMATCH");
  }
  /* ---- 双线读自检：0x3B 读头 4 字节与 0x03 比对，通过后 UI 动画背景才走双线 ---- */
  {
    u8 a[4], b[4];
    Flash_Read(0, a, 4);
    Flash_ReadDual(0, b, 4);
    g_flash_dual_ok = (memcmp(a, b, 4) == 0) ? 1u : 0u;
    printf("[FLASH] dual read self-test: single=%02X%02X%02X%02X dual=%02X%02X%02X%02X -> %s\r\n",
           a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3],
           g_flash_dual_ok ? "OK (0x3B)" : "FAIL -> single-line");
  }
  return 1;
}

/* ==================== 读 JEDEC ID ==================== */
/**
 * @brief  发送 9Fh 读取 Flash 的 JEDEC ID（3 字节）。
 * @retval ID 值（0x5E4015 为 C25VQ16E）
 */
u32 Flash_ReadJEDECID(void)
{
  u32 id;
  u16 f0, f1;

  FLASH_CS_LOW();
  /* 0x9F 命令（单字节），随后 flash 连续输出 M1 M2 M3（每字节 8 clk）。
     16-bit 帧 MSB first，故：
       帧0 = 0x9F + 1 dummy 字节 -> 高字节=命令期 MISO(无效)，低字节=M1
       帧1 = 2 dummy 字节        -> 高字节=M2，低字节=M3
     实测 f0=FE5E f1=4015 => M1=5E M2=40 M3=15 = 0x5E4015（与期望一致）。 */
  f0 = Flash_SPI_SendFrame((u16)(FLASH_CMD_RDID << 8));
  f1 = Flash_SPI_SendFrame(0x0000);
  FLASH_CS_HIGH();

  printf("[FLASH] JEDEC f0=%04X f1=%04X\r\n", f0, f1);
  id = ((u32)(f0 & 0xFF) << 16) | ((u32)(f1 >> 8) << 8) | (u32)(f1 & 0xFF);
  return id;
}

/* ==================== 通用连续读 ==================== */
/**
 * @brief  从 Flash addr 起连续读 len 字节到 buf（0x03 标准读，地址 24bit MSB first）
 * @param  addr : 起始地址（0 ~ 0x1FFFFF）
 * @param  buf  : [输出] 接收缓冲
 * @param  len  : 字节数
 * @retval 无
 */
void Flash_Read(u32 addr, u8 *buf, u32 len)
{
  u32 frames;

  if (len == 0)
    return;

  FLASH_CS_LOW();

  /* ---- 命令 + 地址：0x03, A23-A16, A15-A8, A7-A0 = 恰好 2 个 16-bit 帧（MSB 先） ---- */
  Flash_SPI_SendFrame((u16)((FLASH_CMD_READ << 8) | ((addr >> 16) & 0xFF)));
  Flash_SPI_SendFrame((u16)((((addr >> 8) & 0xFF) << 8) | (addr & 0xFF)));

  /* 保险：清空 RX FIFO 中命令/地址阶段可能残留的帧，避免污染数据首帧 */
  while (HT_SPI1->SR & SPI_FLAG_RXBNE)
    (void)HT_SPI1->DR;

  /* ---- 数据阶段：FIFO 批量收发（每批 8 帧 = 16 字节） ----
     每批：等 TXBE（TX FIFO 空）-> 连填 8 个 0x0000 提供时钟 -> 逐帧等 RXBNE 读回。
     轮询次数降到逐帧版的 1/8，SPI 满速流水；RXFTLS=1 保证 RXBNE 在 RX FIFO>=1 即置位，
     避免旧版 FIFO 隔帧读到 0 的问题。
     ⚠️ 2026-10-28：直接寄存器（HT_SPI1->SR/DR），去掉库函数 Assert/调用开销（read 提速）。 */
  frames = (len + 1) / 2;
  {
    u32 done = 0;
    while (done < frames)
    {
      u32 nfill = frames - done;
      if (nfill > 8u) nfill = 8u;
      while ((HT_SPI1->SR & SPI_FLAG_TXBE) == RESET);
      for (u32 k = 0; k < nfill; k++)
        HT_SPI1->DR = 0x0000;
      for (u32 k = 0; k < nfill; k++)
      {
        while ((HT_SPI1->SR & SPI_FLAG_RXBNE) == RESET);
        u16 f = (u16)HT_SPI1->DR;
        u32 i = done * 2 + k * 2;
        buf[i] = (u8)(f >> 8);                  /* 高字节先收 */
        if (i + 1 < len) buf[i + 1] = (u8)(f & 0xFF);
      }
      done += nfill;
    }
  }
  FLASH_CS_HIGH();
}

/* ==================== 双线输出读（0x3B，数据阶段 2 线） ==================== */
/**
 * @brief  0x3B 双线输出快读：CMD(8)+A(24)+Dummy(8) 单线，数据阶段每 SCK 收 2bit。
 * @note   HT32 SPI 双线模式：DUALEN(CR0 bit6)=1 使 MOSI 变输入；DFL=0x8(CR1 低 4 位)
 *         表示 16-bit 双线帧 = 8 SCK（每帧收 2 字节但按位交织，
 *         flash_dual_unpack 解交织后还原字节顺序）。
 *         实测 0x3B 在 C25VQ16E 上由 Flash_Init 自检通过才被 UI 动画采用。
 * @retval 无
 */
/* 双线帧解交织（2026-10-28 修正）：实测反推（single=48545549 vs 当前解交织输出）确认——
   HT32 SPI DUAL 模式（DFL=8，16bit 双线帧 = 8 SCK）的 RX 数据硬件已按序排好：
   **低字节 = 第一个双线字节（原样）、高字节 = 第二个双线字节（原样）**，无需位重排。
   原「逐位抽取 DQ0=字节1/DQ1=字节2」解交织与 C25VQ16E 0x3B 格式不符，导致双线自检
   一直 FAIL（read 锁单线）；直接取高低字节即可。 */
static inline void flash_dual_unpack_fast(u16 f, u8 *b0, u8 *b1)
{
  *b0 = (u8)(f & 0xFF);   /* 第一个双线字节（先出的，D7..D0 原样） */
  *b1 = (u8)(f >> 8);     /* 第二个双线字节 */
}
u8 Flash_DualOK(void) { return g_flash_dual_ok; }
void Flash_ReadDual(u32 addr, u8 *buf, u32 len)
{
  u32 frames;
  if (len == 0) return;

  FLASH_CS_LOW();
  /* 命令 0x3B + 24bit 地址（单线 16-bit 帧，2 帧 = 32 SCK） */
  Flash_SPI_SendFrame((u16)((FLASH_CMD_DREAD << 8) | ((addr >> 16) & 0xFF)));
  Flash_SPI_SendFrame((u16)((((addr >> 8) & 0xFF) << 8) | (addr & 0xFF)));

  /* 切双线：DUALEN=1 + DFL=0x8（库函数，SPI_DUALCmd 内部等 BUSY 清） */
  SPI_DUALCmd(HT_SPI1, ENABLE);
  SPI_SetDataLength(HT_SPI1, 8);

  /* 清命令阶段残留帧 */
  while (HT_SPI1->SR & SPI_FLAG_RXBNE)
    (void)HT_SPI1->DR;

  /* dummy 8 SCK（双线 1 帧），Flash 之后开始双线输出 */
  (void)Flash_SPI_SendFrame(0x0000);

  /* ---- 数据阶段：FIFO 批量（每批 8 帧） + 查表解交织（2026-10-28 提速）----
     原逐帧 Flash_SPI_SendFrame（每帧 2 次轮询 + 逐位 unpack 32 次位运算），
     改每批 8 帧轮询一次 TXBE + 查表解交织，软件开销降 ~8x；
     ⚠️ 2026-10-28 再优化：直接寄存器（HT_SPI1->SR/DR）去掉库函数 Assert/调用开销
     （库函数每次调用开销大，批量 320 帧/行会放大数倍，read 112ms 瓶颈）。 */
  frames = (len + 1) / 2;
  {
    u32 done = 0;
    while (done < frames)
    {
      u32 nfill = frames - done;
      if (nfill > 8u) nfill = 8u;
      while ((HT_SPI1->SR & SPI_FLAG_TXBE) == RESET);
      for (u32 k = 0; k < nfill; k++)
        HT_SPI1->DR = 0x0000;
      for (u32 k = 0; k < nfill; k++)
      {
        while ((HT_SPI1->SR & SPI_FLAG_RXBNE) == RESET);
        u16 f = (u16)HT_SPI1->DR;
        u32 i = done * 2 + k * 2;
        u8 b0, b1;
        flash_dual_unpack_fast(f, &b0, &b1);
        buf[i] = b0;
        if (i + 1 < len) buf[i + 1] = b1;
      }
      done += nfill;
    }
  }

  /* 回单线：DFL 恢复 16-bit（0x00）、DUALEN 清零 */
  SPI_SetDataLength(HT_SPI1, SPI_DATALENGTH_16);
  SPI_DUALCmd(HT_SPI1, DISABLE);
  FLASH_CS_HIGH();
}

/* ==================== 深度掉电（休眠低功耗） ==================== */
/**
 * @brief  使 Flash 进入深度掉电模式（0xB9），电流由待机 ~15μA 降到 ~1μA。
 * @note   仅休眠前调用；掉电后除 0xAB 外所有命令被忽略，唤醒后必须先
 *         Flash_ReleasePowerDown() 才能再次读取。
 * @retval 无
 */
void Flash_PowerDown(void)
{
  FLASH_CS_LOW();
  Flash_SPI_SendFrame((u16)(FLASH_CMD_POWER_DOWN << 8)); /* 0xB9 + 1 dummy 时钟 */
  FLASH_CS_HIGH();
}

/**
 * @brief  退出深度掉电模式（0xAB），恢复待机，之后可正常读取。
 * @note   需等待 tRES1（典型 3μs），用简单延时保证稳定（48MHz 下约 30 轮/μs）。
 * @retval 无
 */
void Flash_ReleasePowerDown(void)
{
  FLASH_CS_LOW();
  Flash_SPI_SendFrame((u16)(FLASH_CMD_RELEASE_PD << 8)); /* 0xAB + 1 dummy 时钟 */
  FLASH_CS_HIGH();
  {
    volatile u32 n = 20 * (SystemCoreClock / 1000000u); /* 约 20μs */
    while (n--);
  }
}

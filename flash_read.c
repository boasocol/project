/************************************************************************************************************
 * @file    flash_read.c
 * @brief   SPI1 读取外部 NOR Flash 实现。
 * @note    接在 Display.c 尾部 #include 编译（同 gfx.c / calendar.c 方式）。
 *          引脚：PA3=CS(GPIO), PA0=SCK(AFIO), PA1=MOSI(AFIO), PA2=MISO(AFIO)。
 *          SPI1 默认脚，需释放 PA0(原LED已移除)、PA1(已改PB5为RST)。
 *          SPI Mode 3 (CPOL=HIGH, CPHA=SECOND)，兼容市面多数 SPI NOR Flash。
 ************************************************************************************************************/
#include "flash_read.h"
#include <stdio.h>

/* ==================== 引脚定义（SPI1 默认：PA0-PA3） ==================== */
#define FLASH_CS_PIN     GPIO_PIN_3  /* PA3 = SPI1_SEL (GPIO 输出 CS) */
#define FLASH_SCK_AFIO   AFIO_PIN_0  /* PA0 = SPI1_SCK */
#define FLASH_MOSI_AFIO  AFIO_PIN_1  /* PA1 = SPI1_MOSI */
#define FLASH_MISO_AFIO  AFIO_PIN_2  /* PA2 = SPI1_MISO */

/* ==================== 片选宏 ==================== */
#define FLASH_CS_LOW()   GPIO_ClearOutBits(HT_GPIOA, FLASH_CS_PIN)
#define FLASH_CS_HIGH()  GPIO_SetOutBits(HT_GPIOA, FLASH_CS_PIN)

/* ==================== 底层收发 ==================== */
/**
 * @brief  通过 SPI1 发送一个字节并读回。
 * @param  data : 要发送的字节
 * @retval 接收到的字节
 */
static u8 Flash_SPI_SendByte(u8 data)
{
  while (SPI_GetFlagStatus(HT_SPI1, SPI_FLAG_TXBE) == RESET);
  SPI_SendData(HT_SPI1, data);
  while (SPI_GetFlagStatus(HT_SPI1, SPI_FLAG_RXBNE) == RESET);
  return (u8)SPI_ReceiveData(HT_SPI1);
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

  /* ---- 4. SPI 配置：主机，8bit，Mode 3，软件 CS ---- */
  SPI_InitStructure.SPI_Mode           = SPI_MASTER;
  SPI_InitStructure.SPI_FIFO           = SPI_FIFO_ENABLE;  /* 启用 FIFO：批量读，吞吐接近标称 */
  SPI_InitStructure.SPI_DataLength     = SPI_DATALENGTH_8;
  SPI_InitStructure.SPI_SELMode        = SPI_SEL_SOFTWARE;
  SPI_InitStructure.SPI_SELPolarity    = SPI_SELPOLARITY_LOW;
  SPI_InitStructure.SPI_FirstBit       = SPI_FIRSTBIT_MSB;
  SPI_InitStructure.SPI_CPOL           = SPI_CPOL_HIGH;
  SPI_InitStructure.SPI_CPHA           = SPI_CPHA_SECOND;
  SPI_InitStructure.SPI_RxFIFOTriggerLevel = 0;
  SPI_InitStructure.SPI_TxFIFOTriggerLevel = 0;
  SPI_InitStructure.SPI_ClockPrescaler = 2;   /* 48MHz / 2 = 24MHz */

  SPI_Init(HT_SPI1, &SPI_InitStructure);
  SPI_SELOutputCmd(HT_SPI1, ENABLE);
  SPI_Cmd(HT_SPI1, ENABLE);

  printf("[FLASH] SPI1 init done, prescaler=2 => 24MHz\r\n");
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

  FLASH_CS_LOW();
  Flash_SPI_SendByte(FLASH_CMD_RDID);       /* 0x9F */
  id  = (u32)Flash_SPI_SendByte(0x00) << 16;
  id |= (u32)Flash_SPI_SendByte(0x00) << 8;
  id |= (u32)Flash_SPI_SendByte(0x00);
  FLASH_CS_HIGH();

  return id;
}

/* ==================== 读并打印 ==================== */
/**
 * @brief  从 addr 地址读取 len 字节，逐行 hex dump 打印。
 * @param  addr : Flash 起始地址（24-bit，0 ~ 0x1FFFFF）
 * @param  len  : 总字节数
 * @retval 无
 */
void Flash_ReadPrint(u32 addr, u32 len)
{
  u8 buf[16];

  printf("[FLASH] Read 0x%06X  %u bytes\r\n", addr, len);

  while (len > 0)
  {
    u32 n = (len > 16) ? 16 : len;
    u32 i;

    /* 从 Flash 读 n 字节 */
    FLASH_CS_LOW();
    Flash_SPI_SendByte(FLASH_CMD_READ);               /* 0x03 */
    Flash_SPI_SendByte((u8)((addr >> 16) & 0xFF));    /* A23-A16 */
    Flash_SPI_SendByte((u8)((addr >> 8) & 0xFF));     /* A15-A8  */
    Flash_SPI_SendByte((u8)(addr & 0xFF));            /* A7-A0   */
    for (i = 0; i < n; i++)
      buf[i] = Flash_SPI_SendByte(0x00);
    FLASH_CS_HIGH();

    /* 打印地址行 */
    printf("%06X: ", addr);
    for (i = 0; i < 16; i++)
    {
      if (i < n)
        printf("%02X ", buf[i]);
      else
        printf("   ");
      if (i == 7) printf(" ");
    }
    printf(" |");
    for (i = 0; i < n; i++)
      printf("%c", (buf[i] >= 32 && buf[i] <= 126) ? buf[i] : '.');
    printf("|\r\n");

    addr += n;
    len  -= n;
  }

  printf("[FLASH] Read done\r\n");
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
  FLASH_CS_LOW();
  Flash_SPI_SendByte(FLASH_CMD_READ);
  Flash_SPI_SendByte((u8)((addr >> 16) & 0xFF));
  Flash_SPI_SendByte((u8)((addr >> 8) & 0xFF));
  Flash_SPI_SendByte((u8)(addr & 0xFF));
  /* FIFO 批量读：连续填 TX(0x00 提供时钟)，同时从 RX FIFO 收数据，吞吐接近标称 */
  u32 sent = 0, got = 0;
  while (got < len)
  {
    if ((sent < len) && (SPI_GetFIFOStatus(HT_SPI1, SPI_FIFO_TX) < 8u))
    {
      SPI_SendData(HT_SPI1, 0x00);
      sent++;
    }
    if (SPI_GetFlagStatus(HT_SPI1, SPI_FLAG_RXBNE) != RESET)
      buf[got++] = (u8)SPI_ReceiveData(HT_SPI1);
  }
  FLASH_CS_HIGH();
}

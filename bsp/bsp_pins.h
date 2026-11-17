#ifndef __BSP_PINS_H
#define __BSP_PINS_H

#include "ht32.h"

/* ============================================================================
 * BSP 板级引脚/外设映射表（P1 集中，2026-09-03）
 * ----------------------------------------------------------------------------
 * 所有「哪个外设接在哪颗脚、用哪个外设实例」统一收敛到本文件，驱动/应用只
 * 引用宏、不在自己文件里定义引脚。换板/换 MCU 只需改这里 + HAL（库）+ 时钟。
 * 原定义散落于：spi_lcd.h / flash_read.c / yx8320b.c / voice.h / hd9803.h / key.c。
 * ========================================================================== */

/* ---- LCD（ST7789，SPI0：PA4=SCK PA5=MOSI PB2=CS 软件控制） ---- */
/* SPI 波特率取 ST7789 规格上限 15MHz（数据手册主机上限 fPCLK/2=24MHz，
   ST7789 串行写周期下限 66ns≈15MHz；超过即超规格，仅实测支持才可提） */
#define SPI_LCD_RST_GPIO  HT_GPIOB     /* 复位 PB5（腾 PA1 给 SPI1_MOSI） */
#define SPI_LCD_RST_PIN   GPIO_PIN_5
#define SPI_LCD_BL_GPIO   HT_GPIOB     /* 背光（三极管驱动，低电平导通） */
#define SPI_LCD_BL_PIN    GPIO_PIN_4
#define SPI_LCD_SPI       HT_SPI0
#define SPI_LCD_SPI_BAUD  15000000     /* 15MHz（ST7789 规格上限，48MHz/3.2） */
/* LCD 写屏传输方式（2026-10-27 照项目 8（HT32L52241，已实机 DMA 成功）移植）：
   1 = 非 FIFO + PDMA TX（官方唯一 DMA 组合：SPI_FIFO_DISABLE + PDMA；RX 由等待循环手动
       排空、不配 RX PDMA——FIFO+PDMA 曾因 RXFTLS=0 恒置位 → RX 请求风暴(VH)饿死 TX 卡死，
       见 notes 坑#88 修正4b/4c）；
   0 = FIFO 批量轮询（旧实现，回退用） */
#define SPI_LCD_PDMA_ENABLE 1

/* ---- 外部 NOR Flash（SPI1：PA0=SCK PA1=MOSI PA2=MISO，PA3=CS GPIO 输出） ---- */
#define FLASH_SPI         HT_SPI1
#define FLASH_SCK_AFIO    AFIO_PIN_0
#define FLASH_MOSI_AFIO   AFIO_PIN_1
#define FLASH_MISO_AFIO   AFIO_PIN_2
#define FLASH_CS_PIN      GPIO_PIN_3

/* ---- 温湿度 YX8320B（软件 I2C：PB0=SCL PB1=SDA，需外接 4.7k~10k 上拉） ---- */
#define YX_SCL_GPIO   HT_GPIOB
#define YX_SCL_PIN    GPIO_PIN_0
#define YX_SDA_GPIO   HT_GPIOB
#define YX_SDA_PIN    GPIO_PIN_1

/* ---- 语音模块（UART0：PB7=TX PB8=RX，9600；电源控制：PB3 原 printf 口让出） ---- */
#define VOICE_GPIO_ID         GPIO_PB
#define VOICE_GPIO_PORT       HT_GPIOB
#define VOICE_TX_AFIO_PIN     AFIO_PIN_7
#define VOICE_RX_AFIO_PIN     AFIO_PIN_8
#define VOICE_UART            HT_UART0
#define VOICE_UART_IRQn       UART0_IRQn
#define VOICE_UART_IRQHandler UART0_IRQHandler
#define VOICE_PWR_GPIO        HT_GPIOB
#define VOICE_PWR_GPIO_CLK    PB
#define VOICE_PWR_PIN         GPIO_PIN_3

/* ---- 蓝牙授时 HD9803（USART0：PC4=TX PC5=RX，9600；电源 PA15，低电平上电） ----
   ⚠️ 电源脚：模块峰值电流 ~100mA，GPIO 不能直驱，必须经开关管
   （PA15 ──> [PNP/PMOS] ──> 模块 VCC）；低电平=导通上电，高电平=断电（实测极性） */
#define HD9803_GPIO_ID         GPIO_PC
#define HD9803_GPIO_PORT       HT_GPIOC
#define HD9803_TX_AFIO_PIN     AFIO_PIN_4
#define HD9803_RX_AFIO_PIN     AFIO_PIN_5
#define HD9803_UART            HT_USART0
#define HD9803_UART_IRQn       USART0_IRQn
#define HD9803_UART_IRQHandler USART0_IRQHandler
#define HD9803_PWR_GPIO        HT_GPIOA
#define HD9803_PWR_GPIO_CLK    PA          /* clk.Bit.PA 位域名（配合 CKCU 结构体用，非独立宏） */
#define HD9803_PWR_PIN         GPIO_PIN_15
#define HD9803_PWR_ON_LEVEL    0u          /* 0=低电平上电（实测极性） */
#define HD9803_PWR_ON_DELAY_MS 200u

/* ---- 按键 PB12/PB13/PB14（内部上拉、接地按下，EXTI 下降沿 + 硬件消抖；PB12 兼低功耗唤醒） ----
   2026-09-04 扩为 3 键：PB13/PB14 原为 HSE 主晶振复用脚，本工程高频用 HSI->PLL（PLL_CLK_SRC=1）
   未启用 HSE，且 USART0-RX(PB14)/I2C0(PB13/PB14)/MCTM(PB13/PB14) 复用均未占用
   （见 notes/mcu-ht32l52241-pins.md §3），腾出做按键。EXTI13/14 与 EXTI12 同组 EXTI4_15。 */
#define KEY_COUNT     3u          /* 按键总数（KEY1..KEY3） */
#define KEY_GPIO      HT_GPIOB
#define KEY1_PIN      GPIO_PIN_12
#define KEY2_PIN      GPIO_PIN_13
#define KEY3_PIN      GPIO_PIN_14
#define KEY1_AFIO     AFIO_PIN_12
#define KEY2_AFIO     AFIO_PIN_13
#define KEY3_AFIO     AFIO_PIN_14
#define KEY1_EXTI     EXTI_CHANNEL_12
#define KEY2_EXTI     EXTI_CHANNEL_13
#define KEY3_EXTI     EXTI_CHANNEL_14
/* 兼容旧名（key.c 现有 PB12 关屏/唤醒逻辑）：KEY_PIN/KEY_AFIO/KEY_EXTI = KEY1 */
#define KEY_PIN       KEY1_PIN
#define KEY_AFIO      KEY1_AFIO
#define KEY_EXTI      KEY1_EXTI

#endif /* __BSP_PINS_H */

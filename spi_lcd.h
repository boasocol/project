#ifndef __SPI_LCD_H
#define __SPI_LCD_H

#include "ht32.h"

/* ==================== LCD 专用 SPI 通信层 ====================
 * 本文件只负责“把字节通过 SPI 发到屏幕”，不含任何 ST7789 命令/绘图逻辑，
 * 与显示驱动（Display.c）完全分离。
 * 屏幕为 3 线 SPI 模式：9 位数据，D8 = D/C（0=命令 / 1=数据），D7..D0 = 数据。
 */

/* 引脚与 SPI 参数（LCD 专用） */
#define SPI_LCD_RST_GPIO  HT_GPIOB    /* 复位引脚 GPIO 端口（为腾出 PA1 给 SPI1_MOSI，移到 PB5） */
#define SPI_LCD_RST_PIN   GPIO_PIN_5  /* 复位引脚 PB5 */
#define SPI_LCD_BL_GPIO   HT_GPIOB    /* 背光控制 GPIO 端口（三极管驱动，低电平导通） */
#define SPI_LCD_BL_PIN    GPIO_PIN_4  /* 背光控制引脚 PB4 */
#define SPI_LCD_SPI       HT_SPI0     /* SPI 外设 */
/* SPI 波特率：数据手册主机模式上限 = fPCLK/2。
 * 主频(=fPCLK) 48MHz 时 SPI 最高可达 24MHz(fPCLK/2)；
 * ST7789 串行写周期下限 66ns(≈15MHz)，此处取安全的 12MHz，
 * 如需更激进、且所用屏支持，可改 24000000(=24MHz 硬件上限)。 */
#define SPI_LCD_SPI_BAUD  12000000    /* SPI 波特率 12MHz（= 48MHz/4，留余量在 ST7789 15MHz 上限内） */

void SPI_LCD_Init(void);              /* 配置时钟/引脚/SPI/硬件复位，建立通信 */
void SPI_LCD_Select(void);            /* 拉低 CS，开始一次传输事务 */
void SPI_LCD_Deselect(void);          /* 等待发送完成并拉高 CS，结束事务 */
void SPI_LCD_WriteCmd(u8 cmd);        /* 发送命令字节（D/C=0） */
void SPI_LCD_WriteData(const u8 *buf, u32 len); /* 发送数据块（D/C=1） */
void SPI_LCD_WriteDataByte(u8 data);  /* 发送单个数据字节（D/C=1） */

void Delay(u32 ms);                   /* 通用毫秒延时（基于 BFTM0） */

#endif

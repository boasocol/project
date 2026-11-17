#ifndef __SPI_LCD_H
#define __SPI_LCD_H

#include "ht32.h"
#include "bsp_pins.h"   /* SPI_LCD_* 引脚/外设/波特率映射（P1 集中，2026-09-03） */
#include "bsp_delay.h"  /* Delay 已迁到 BSP 延时服务（P1，2026-09-03） */

/* ==================== LCD 专用 SPI 通信层 ====================
 * 本文件只负责“把字节通过 SPI 发到屏幕”，不含任何 ST7789 命令/绘图逻辑，
 * 与显示驱动（Display.c）完全分离。
 * 屏幕为 3 线 SPI 模式：9 位数据，D8 = D/C（0=命令 / 1=数据），D7..D0 = 数据。
 */

void SPI_LCD_Init(void);              /* 配置时钟/引脚/SPI/硬件复位，建立通信 */
void SPI_LCD_Select(void);            /* 拉低 CS，开始一次传输事务 */
void SPI_LCD_Deselect(void);          /* 等待发送完成并拉高 CS，结束事务 */
void SPI_LCD_WriteCmd(u8 cmd);        /* 发送命令字节（D/C=0） */
void SPI_LCD_WriteData(const u8 *buf, u32 len); /* 发送数据块（D/C=1，内部展开 DC 到 staging） */
void SPI_LCD_WriteDataByte(u8 data);  /* 发送单个数据字节（D/C=1） */
/* 2026-10-28：PDMA 直发已打包好的 16bit 帧数组（bit8=DC 由调用方预置），
   跳过 staging 展开 + 大批一次发（整行 640 帧 1 批），上传提速；
   源地址须 2/4 字节对齐（ARM PDMA 16bit 读）。 */
void SPI_LCD_WriteFrames16(const u16 *frames, u32 n);
/* 2026-10-28 流水线：拆 Start/Wait——Start 启动 TX PDMA 后返回（CPU 可并行读 Flash），
   Wait 等 CBLKCNT 完成；配合 RxDrainStart/Stop（SPI0 RX 回显 DMA 排空，非 FIFO 无风暴）
   让写行期间 CPU 空闲做读/打包。 */
void SPI_LCD_WriteFrames16Start(const u16 *frames, u32 n);
void SPI_LCD_WriteFrames16Wait(void);
void SPI_LCD_RxDrainStart(void);   /* SPI0 RX 回显 PDMA 排空（CH0）：写屏期间硬件清 RX，CPU 不占 */
void SPI_LCD_RxDrainStop(void);

#endif

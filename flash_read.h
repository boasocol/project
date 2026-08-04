/************************************************************************************************************
 * @file    flash_read.h
 * @brief   SPI1 读取外部 NOR Flash (C25VQ16E) 基础模块
 * @note    仅读不写，无擦除/编程逻辑；引脚：PC1=CS, PC2=SCK, PC3=MOSI, PB6=MISO。
 *          分步调试：先用本模块确认硬件连接和通信正常，再做流式显示。
 ************************************************************************************************************/
#ifndef __FLASH_READ_H
#define __FLASH_READ_H

#include "ht32.h"

/* 标准 SPI Flash 命令（C25VQ16E 兼容） */
#define FLASH_CMD_READ       0x03   /* 标准读 */
#define FLASH_CMD_FREAD      0x0B   /* 快速读 */
#define FLASH_CMD_RDID       0x9F   /* 读 JEDEC ID */

/* C25VQ16E 的 JEDEC ID：Manufacturer=5E, Memory Type=40, Capacity=15 */
#define C25VQ16E_JEDEC_ID    0x5E4015

/* ==================== API ==================== */
u32  Flash_Init(void);              /* 初始化 SPI1 并测试通信 */
u32  Flash_ReadJEDECID(void);       /* 读 JEDEC ID */
void Flash_ReadPrint(u32 addr, u32 len); /* 读并逐行 hex dump 打印 */
void Flash_Read(u32 addr, u8 *buf, u32 len); /* 从 Flash 连续读 len 字节到 buf */

#endif

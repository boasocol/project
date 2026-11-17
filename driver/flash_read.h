/************************************************************************************************************
 * @file    flash_read.h
 * @brief   SPI1 读取外部 NOR Flash (C25VQ16E) 基础模块
 * @note    仅读不写，无擦除/编程逻辑；引脚：PA3=CS(GPIO), PA0=SCK, PA1=MOSI, PA2=MISO。
 *          方案 A：全程 16-bit 单线（DFL=0x0），命令/地址/数据均按 16-bit 帧收发。
 ************************************************************************************************************/
#ifndef __FLASH_READ_H
#define __FLASH_READ_H

#include "ht32.h"

/* 标准 SPI Flash 命令（C25VQ16E 兼容） */
#define FLASH_CMD_READ       0x03   /* 标准读 */
#define FLASH_CMD_DREAD      0x3B   /* 双线输出快读（数据阶段 2 线，配合 DUALEN + DFL=0x8） */
#define FLASH_CMD_FREAD      0x0B   /* 快速读 */
#define FLASH_CMD_RDID       0x9F   /* 读 JEDEC ID */
#define FLASH_CMD_POWER_DOWN 0xB9   /* 深度掉电：待机 ~15μA -> 掉电 ~1μA */
#define FLASH_CMD_RELEASE_PD 0xAB   /* 退出掉电（tRES1 典型 3μs，实际多等几微秒） */

/* C25VQ16E 的 JEDEC ID：Manufacturer=5E, Memory Type=40, Capacity=15 */
#define C25VQ16E_JEDEC_ID    0x5E4015

/* ==================== API ==================== */
u32  Flash_Init(void);              /* 初始化 SPI1 并测试通信 */
u32  Flash_ReadJEDECID(void);       /* 读 JEDEC ID */
void Flash_Read(u32 addr, u8 *buf, u32 len); /* 从 Flash 连续读 len 字节到 buf */
void Flash_ReadDual(u32 addr, u8 *buf, u32 len); /* 双线输出读（0x3B），数据阶段每帧 8 SCK 收 2 字节 */
u8   Flash_DualOK(void);               /* 双线读自检结果：1=可用（Flash_Init 内校验） */
void Flash_PowerDown(void);         /* 进入深度掉电（休眠用，电流 ~1μA） */
void Flash_ReleasePowerDown(void);  /* 退出深度掉电（唤醒后必须先调用才能读） */

#endif

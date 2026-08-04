#ifndef __ST7789_H
#define __ST7789_H

#include "ht32.h"

/* ==================== 引脚定义 ==================== */
#define ST7789_RST_GPIO   HT_GPIOA    /* 复位引脚GPIO端口 */
#define ST7789_RST_PIN    GPIO_PIN_1  /* 复位引脚PA1 */
#define ST7789_BL_GPIO    HT_GPIOB    /* 背光控制引脚GPIO端口（三极管驱动，低电平导通） */
#define ST7789_BL_PIN     GPIO_PIN_4  /* 背光控制引脚PB4 */

#define ST7789_SPI        HT_SPI0     /* SPI外设 */
#define ST7789_SPI_BAUD   2000000     /* SPI波特率2MHz */

/* ==================== 屏幕参数 ==================== */
#define ST7789_WIDTH      240         /* 屏幕宽度（像素） */
#define ST7789_HEIGHT     320         /* 屏幕高度（像素） */
#define ST7789_X_SHIFT    0           /* X轴偏移量 */
#define ST7789_Y_SHIFT    0           /* Y轴偏移量 */

/* ==================== 预定义颜色（RGB565格式） ==================== */
#define WHITE       0xFFFF            /* 白色 */
#define BLACK       0x0000            /* 黑色 */
#define BLUE        0x001F            /* 蓝色 */
#define RED         0xF800            /* 红色 */
#define GREEN       0x07E0            /* 绿色 */
#define CYAN        0x07FF            /* 青色 */
#define MAGENTA     0xF81F            /* 品红色 */
#define YELLOW      0xFFE0            /* 黄色 */
#define GRAY        0x8430            /* 灰色 */

/* ==================== ST7789驱动IC指令集 ==================== */

/* ---------- 基础指令 ---------- */
#define ST7789_NOP         0x00       /* 空操作，无参数 */
#define ST7789_SWRESET     0x01       /* 软件复位，无参数。复位后需等待120ms */
#define ST7789_RDDID       0x04       /* 读取显示ID，返回3字节：MID/DID1/DID2 */
#define ST7789_RDDST       0x09       /* 读取显示状态 */
#define ST7789_RDDPM       0x0A       /* 读取电源模式 */
#define ST7789_RDD_MADCTL  0x0B       /* 读取内存访问控制 */
#define ST7789_RDD_COLMOD  0x0C       /* 读取颜色模式 */
#define ST7789_RDDIM       0x0D       /* 读取显示图像模式 */
#define ST7789_RDDSM       0x0E       /* 读取显示信号模式 */
#define ST7789_RDDSDR      0x0F       /* 读取显示自我诊断结果 */

/* ---------- 睡眠控制指令 ---------- */
#define ST7789_SLPIN       0x10       /* 进入睡眠模式，无参数 */
#define ST7789_SLPOUT      0x11       /* 退出睡眠模式，无参数。退出后需等待120ms */
#define ST7789_PTLON       0x12       /* 部分显示模式开启，无参数 */
#define ST7789_NORON       0x13       /* 正常显示模式开启，无参数 */

/* ---------- 显示控制指令 ---------- */
#define ST7789_INVOFF      0x20       /* 反色显示关闭，无参数 */
#define ST7789_INVON       0x21       /* 反色显示开启，无参数 */
#define ST7789_GAMSET      0x26       /* Gamma曲线选择，1参数：0x01~0x04 */
#define ST7789_DISPOFF     0x28       /* 关闭显示，无参数 */
#define ST7789_DISPON      0x29       /* 开启显示，无参数 */
#define ST7789_CASET       0x2A       /* 列地址设置，4参数：起始列高/低、结束列高/低 */
#define ST7789_RASET       0x2B       /* 行地址设置，4参数：起始行高/低、结束行高/低 */
#define ST7789_RAMWR       0x2C       /* 写入显示RAM，后续为像素数据 */
#define ST7789_RGBSET      0x2D       /* RGB接口信号设置 */
#define ST7789_RAMRD       0x2E       /* 读取显示RAM */
#define ST7789_PTLAR       0x30       /* 部分区域设置 */
#define ST7789_VSCRDEF     0x33       /* 垂直滚动定义 */
#define ST7789_TEOFF       0x34       /* 撕裂效果线关闭 */
#define ST7789_TEON        0x35       /* 撕裂效果线开启 */
#define ST7789_MADCTL      0x36       /* 内存访问控制，1参数 */
#define ST7789_VSCRSADD    0x37       /* 垂直滚动起始地址 */
#define ST7789_IDMOFF      0x38       /* 空闲模式关闭 */
#define ST7789_IDMON       0x39       /* 空闲模式开启 */
#define ST7789_COLMOD      0x3A       /* 颜色模式设置，1参数 */
#define ST7789_RAMWRC      0x3C       /* 连续写入显示RAM */
#define ST7789_RAMRDC      0x3E       /* 连续读取显示RAM */
#define ST7789_TESCAN      0x44       /* 设置撕裂扫描线 */
#define ST7789_RDTESCAN    0x45       /* 读取撕裂扫描线 */

/* ---------- 电源控制指令 ---------- */
#define ST7789_PWCTR1      0xC0       /* 电源控制1，2参数：VRH[4:0]/BT[2:0] */
#define ST7789_PWCTR2      0xC1       /* 电源控制2，1参数：VGH/VGL */
#define ST7789_PWCTR3      0xC2       /* 电源控制3（正常模式），2参数 */
#define ST7789_PWCTR4      0xC3       /* 电源控制4（休眠模式），2参数 */
#define ST7789_PWCTR5      0xC4       /* 电源控制5（休眠模式），2参数 */
#define ST7789_VMCTR1      0xC5       /* VCOM控制1，1参数：VCOMH/VCOML */
#define ST7789_VMOFCTR     0xC7       /* VCOM偏移控制 */

/* ---------- 帧速率控制指令 ---------- */
#define ST7789_FRMCTR1     0xB1       /* 帧速率控制1（正常模式），2参数 */
#define ST7789_FRMCTR2     0xB2       /* 帧速率控制2（空闲模式），2参数 */
#define ST7789_FRMCTR3     0xB3       /* 帧速率控制3（部分模式），4参数 */
#define ST7789_INVCTR      0xB4       /* 反色控制，1参数 */
#define ST7789_DFUNCTR     0xB6       /* 显示功能控制，4参数：RGB/MCU接口、显示模式、扫描方向 */

/* ---------- Gamma校正指令 ---------- */
#define ST7789_GMCTRP1     0xE0       /* 正Gamma校正表，14参数 */
#define ST7789_GMCTRN1     0xE1       /* 负Gamma校正表，14参数 */

/* ---------- 接口控制指令 ---------- */
#define ST7789_PWCTR6      0xFC       /* 电源控制6 */

/* ==================== MADCTL（内存访问控制）位定义 ==================== */
#define ST7789_MADCTL_MY   0x80       /* 行地址顺序（MY=1：从底到顶） */
#define ST7789_MADCTL_MX   0x40       /* 列地址顺序（MX=1：从右到左） */
#define ST7789_MADCTL_MV   0x20       /* 行/列交换（MV=1：交换） */
#define ST7789_MADCTL_ML   0x10       /* 垂直刷新顺序（ML=1：从左到右） */
#define ST7789_MADCTL_RGB  0x00       /* RGB色彩顺序 */
#define ST7789_MADCTL_BGR  0x08       /* BGR色彩顺序 */
#define ST7789_MADCTL_MH   0x04       /* 水平刷新顺序 */

/* ==================== COLMOD（颜色模式）参数 ==================== */
#define ST7789_COLOR_MODE_8bit    0x03   /* 8位颜色模式（RGB332） */
#define ST7789_COLOR_MODE_12bit   0x05   /* 12位颜色模式（RGB444） */
#define ST7789_COLOR_MODE_16bit   0x55   /* 16位颜色模式（RGB565） */
#define ST7789_COLOR_MODE_18bit   0x66   /* 18位颜色模式（RGB666） */

/* ==================== 函数声明 ==================== */
void ST7789_Init(void);
void ST7789_Fill_Color(uint16_t color);
void ST7789_Select(void);
void ST7789_Deselect(void);
void ST7789_WriteCommand(u8 cmd);
void ST7789_WriteSmallData(u8 data);
void ST7789_WriteData(u8 *buff, u32 buff_size);

#endif
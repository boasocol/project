/************************************************************************************************************
 * @file    Display.c
 * @brief   ST7789 显示驱动（与 SPI 通信层 spi_lcd.c 完全分离）
 * @note    Display 只调用 SPI_LCD_* 传输接口，不直接操作 SPI 寄存器；
 *          MADCTL 旋转由 lcd_map() 在绘制时换算坐标，原生竖屏 240x320 不会越界。
 ************************************************************************************************************/
#include "Display.h"

/* 软件层方向（与写入硬件的 MADCTL 解耦）：
 * 本模块 MADCTL 硬件旋转实测不生效，故用“软件坐标转置”得到 90° 横屏：
 *   lcd_map 恒为 列=逻辑y、行=逻辑x，把 320x240 的逻辑图转置写入 240x320 的 GRAM；
 *   硬件 MADCTL 保持原生 0x00，面板直接按 GRAM 显示即为横屏。
 * 若上电后画面上下/左右颠倒，按实际安装方向把 SW_MAP_MX / SW_MAP_MY 改成 0x40 / 0x80。 */
#define SW_MAP_MV  ST7789_MADCTL_MV   /* 恒为 1：软件行列交换(90°) */
#define SW_MAP_MX  0x00                 /* 列镜像(可选) */
#define SW_MAP_MY  0x80                /* 行镜像(可选) */

/* ------------------------------------------------------------------ */
/**
 * @brief  逻辑坐标 -> GRAM 地址换算（软件旋转，恒按 SW_MAP_* 转置）
 * @note   转置是单调映射，矩形窗口只需对四个角取 min/max（见 Display_FillRect）。
 * @param  x,y     : [输入] 逻辑坐标（应用层 320x240 坐标系）
 * @param  col,row : [输出] GRAM 列/行地址（写入 CASET/RASET）
 * @retval 无（通过指针参数输出）
 */
void lcd_map(u16 x, u16 y, u16 *col, u16 *row)
{
  if (SW_MAP_MV)
  {
    *col = (SW_MAP_MX) ? (ST7789_WIDTH  - 1 - y) : y;
    *row = (SW_MAP_MY) ? (ST7789_HEIGHT - 1 - x) : x;
  }
  else
  {
    *col = (SW_MAP_MX) ? (ST7789_WIDTH  - 1 - x) : x;
    *row = (SW_MAP_MY) ? (ST7789_HEIGHT - 1 - y) : y;
  }
}

/* ------------------------------------------------------------------ */
/**
 * @brief  GRAM 列/行 -> 逻辑坐标（lcd_map 的逆运算，供 FB_Blit 反查像素）
 * @param  col,row : [输入] GRAM 列/行地址
 * @param  x,y     : [输出] 逻辑坐标
 * @retval 无（通过指针参数输出）
 */
void lcd_unmap(u16 col, u16 row, u16 *x, u16 *y)
{
  if (SW_MAP_MV)
  {
    *y = (SW_MAP_MX) ? (ST7789_WIDTH  - 1 - col) : col;
    *x = (SW_MAP_MY) ? (ST7789_HEIGHT - 1 - row) : row;
  }
  else
  {
    *x = (SW_MAP_MX) ? (ST7789_WIDTH  - 1 - col) : col;
    *y = (SW_MAP_MY) ? (ST7789_HEIGHT - 1 - row) : row;
  }
}

//-----------------------------------------------------------------------------
/**
 * @brief  显示驱动初始化：先建立 SPI 通信，再发送 ST7789 上电命令序列
 * @note   序列：软复位 -> 退睡眠 -> 正常显示 -> RGB565 -> MADCTL(原生 0x00，
 *         本模块硬件旋转不生效，90° 横屏由软件层 lcd_map 转置实现) ->
 *         显示功能控制 -> 开显示。结束后屏幕即可接受绘图命令。
 * @retval 无
 */
void Display_Init(void)
{
  SPI_LCD_Init();   /* 建立通信（时钟/引脚/SPI/复位） */

  /* 软件复位 */
  SPI_LCD_Select();
  SPI_LCD_WriteCmd(ST7789_SWRESET);
  SPI_LCD_Deselect();
  Delay(120);

  /* 退出睡眠 */
  SPI_LCD_Select();
  SPI_LCD_WriteCmd(ST7789_SLPOUT);
  SPI_LCD_Deselect();
  Delay(120);

  /* 正常显示模式 */
  SPI_LCD_Select();
  SPI_LCD_WriteCmd(ST7789_NORON);
  SPI_LCD_Deselect();
  Delay(10);

  /* 颜色模式：16bit RGB565 */
  SPI_LCD_Select();
  SPI_LCD_WriteCmd(ST7789_COLMOD);
  SPI_LCD_WriteDataByte(ST7789_COLOR_MODE_16bit);
  SPI_LCD_Deselect();

  /* 内存访问控制：保持原生 0x00（硬件旋转在本模块不生效，
     90° 横屏由软件层 lcd_map 坐标转置实现） */
  SPI_LCD_Select();
  SPI_LCD_WriteCmd(ST7789_MADCTL);
  SPI_LCD_WriteDataByte(ST7789_MADCTL_DEFAULT);
  SPI_LCD_Deselect();

  /* 显示功能控制 */
  SPI_LCD_Select();
  SPI_LCD_WriteCmd(ST7789_DFUNCTR);
  SPI_LCD_WriteDataByte(0x0A);
  SPI_LCD_WriteDataByte(0x82);
  SPI_LCD_Deselect();

  /* 开启显示 */
/*  SPI_LCD_Select();
  SPI_LCD_WriteCmd(ST7789_DISPON);
  SPI_LCD_Deselect();
  Delay(50);*/
}

void Display_on(void)
{
	SPI_LCD_Select();
	SPI_LCD_WriteCmd(ST7789_DISPON);
	SPI_LCD_Deselect();
	Delay(1000);
}


//-----------------------------------------------------------------------------
/**
 * @brief  全屏填充指定颜色（开整屏窗口后连续写 240*320 个像素）
 * @param  color : RGB565 颜色值
 * @retval 无
 */
void Display_Fill_Color(u16 color)
{
  u32 i;
  u32 total = (u32)ST7789_WIDTH * ST7789_HEIGHT;

  SPI_LCD_Select();

  SPI_LCD_WriteCmd(ST7789_CASET);
  {
    u8 d[4] = {0, 0, HI8(ST7789_WIDTH - 1), LO8(ST7789_WIDTH - 1)};
    SPI_LCD_WriteData(d, 4);
  }
  SPI_LCD_WriteCmd(ST7789_RASET);
  {
    u8 d[4] = {0, 0, HI8(ST7789_HEIGHT - 1), LO8(ST7789_HEIGHT - 1)};
    SPI_LCD_WriteData(d, 4);
  }
  SPI_LCD_WriteCmd(ST7789_RAMWR);
  for (i = 0; i < total; i++)
  {
    SPI_LCD_WriteDataByte(HI8(color));
    SPI_LCD_WriteDataByte(LO8(color));
  }

  SPI_LCD_Deselect();
}

//-----------------------------------------------------------------------------
/**
 * @brief  填充单色矩形（逻辑坐标，原生竖屏 240x320）
 * @note   对矩形四个角做 lcd_map 映射后取 min/max 当窗口，兼容 MV/MX/MY 任意组合；
 *         逻辑坐标受 SCREEN_W/SCREEN_H 裁剪，不会写出 GRAM 范围。
 * @param  x,y   : 矩形左上角（逻辑坐标）
 * @param  w,h   : 矩形宽高（像素，超出屏幕自动裁剪）
 * @param  color : RGB565 填充色
 * @retval 无
 */
void Display_FillRect(u16 x, u16 y, u16 w, u16 h, u16 color)
{
  if (x >= SCREEN_W || y >= SCREEN_H) return;
  if (x + w > SCREEN_W) w = SCREEN_W - x;
  if (y + h > SCREEN_H) h = SCREEN_H - y;
  if (w == 0 || h == 0) return;

  /* 取矩形四个角的 GRAM 地址，再取 min/max 作为窗口。
   * 必须算全部四个角：MV=0 时某维度只随 x 或只随 y 变化，
   * 只算两角会让矩形塌缩成 1x1 像素（零散像素点的根因）。 */
  u16 c[4], r[4];
  lcd_map(x,         y,         &c[0], &r[0]);
  lcd_map(x,         y + h - 1, &c[1], &r[1]);
  lcd_map(x + w - 1, y,         &c[2], &r[2]);
  lcd_map(x + w - 1, y + h - 1, &c[3], &r[3]);
  u16 col0 = c[0], col1 = c[0], row0 = r[0], row1 = r[0];
  u8 k;
  for (k = 1; k < 4; k++)
  {
    if (c[k] < col0) col0 = c[k];
    if (c[k] > col1) col1 = c[k];
    if (r[k] < row0) row0 = r[k];
    if (r[k] > row1) row1 = r[k];
  }

  u32 n = (u32)w * h;
  u32 i;

  SPI_LCD_Select();
  SPI_LCD_WriteCmd(ST7789_CASET);
  {
    u8 d[4] = {HI8(col0), LO8(col0), HI8(col1), LO8(col1)};
    SPI_LCD_WriteData(d, 4);
  }
  SPI_LCD_WriteCmd(ST7789_RASET);
  {
    u8 d[4] = {HI8(row0), LO8(row0), HI8(row1), LO8(row1)};
    SPI_LCD_WriteData(d, 4);
  }
  SPI_LCD_WriteCmd(ST7789_RAMWR);
  for (i = 0; i < n; i++)
  {
    SPI_LCD_WriteDataByte(HI8(color));
    SPI_LCD_WriteDataByte(LO8(color));
  }
  SPI_LCD_Deselect();
}

//-----------------------------------------------------------------------------
/**
 * @brief  画一个逻辑像素点（内部即 1x1 的 FillRect）
 * @param  x,y   : 像素坐标（逻辑坐标）
 * @param  color : RGB565 颜色值
 * @retval 无
 */
void Display_DrawPixel(u16 x, u16 y, u16 color)
{
  Display_FillRect(x, y, 1, 1, color);
}

/* ===========================================================================
 * 把图形库与日历界面合并进本编译单元。
 * 原因：HT32-IDE 的 subdir.mk 为自动生成文件，手动添加源文件会被覆盖，
 *       故用 #include 方式让以下 .c 随 Display.c 一起链接进固件。
 * 注意：若将来在 IDE 中把这些 .c 正式加入工程源列表，请删除对应行，
 *       否则会出现“重复定义”链接错误。
 * =========================================================================== */
#include "ui.c"          /* Flash UI：背景上传 + 字库渲染（就地改造，复用 spi_lcd/Display/flash_read） */
#include "calendar.c"     /* 日历界面：调用 UI_* 在背景图上叠加动态文字 */
#include "flash_read.c"   /* SPI1 Flash 读取模块 */
#include "yx8320b.c"      /* YX8320B 温湿度传感器（软件 I2C PB0/PB1）；借此纳入编译 */
#include "BFTM1.c"        /* 半秒中断(冒号闪烁)；IDE 源列表固定，借此纳入编译 */

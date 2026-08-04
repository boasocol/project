/************************************************************************************************************
 * @file    st7789.c  （编译聚合文件 / 构建兼容用）
 * @brief   本文件已存在于 HT32-IDE 自动生成的源列表(subdir.mk)中，故用作“编译入口”，
 *          通过 #include 把 SPI 通信层(spi_lcd.c) 与 显示驱动(Display.c) 合并进同一编译单元。
 *          Display.c 内部再 #include gfx.c / calendar.c。
 * @warning 不要在 IDE 中把 Display.c / spi_lcd.c / gfx.c / calendar.c 单独加入工程源列表，
 *          否则会出现“重复定义”链接错误。需要正规工程结构时：删除本文件，
 *          并把 Display.c、spi_lcd.c 加入工程即可（gfx.c/calendar.c 仍由 Display.c 包含）。
 ************************************************************************************************************/
#include "spi_lcd.c"
#include "Display.c"

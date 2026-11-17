#ifndef __APP_RENDER_H
#define __APP_RENDER_H

#include "time_module.h"   /* TimeStruct */

/* ==================== 应用层渲染调度（P2 拆 Display.c，2026-09-03） ====================
 * 原 Display_Update 名义在“显示驱动”文件里，实际职责是应用层：
 * 每轮 翻页动画推进 + 形象动画推进 + 按当前屏分发渲染（主屏→calendar、倒计时/闹钟→screen）。
 * 已从 Display.c 迁出到本模块；Display.c 回归纯 ST7789 驱动（init/fill/pixel/on/off/lcd_map）。
 */

void App_Render(TimeStruct *t);   /* 主循环每轮调用：动画推进 + 按当前屏增量渲染 */

#endif /* __APP_RENDER_H */

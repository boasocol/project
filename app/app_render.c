/************************************************************************************************************
 * @file    app_render.c
 * @brief   应用层渲染调度（P2 拆自 Display.c，2026-09-03）
 * @note    原 Display_Update 实为应用层职责（按屏分发渲染），从显示驱动 Display.c 迁出：
 *          主屏 → calendar.c 增量刷新；倒计时/闹钟屏 → screen.c 增量绘制；
 *          外加翻页动画推进 + 形象状态动画推进。Display.c 只留纯 ST7789 驱动接口。
 *          行为与原 Display_Update 完全一致（主循环调用点改名 App_Render）。
 *          2026-09-05：静态区块（日期/温湿度/蓝牙）随每轮刷新，去掉后半秒时隙门控。
 ************************************************************************************************************/
#include "app_render.h"
#include "BFTM0.h"         /* bftm0_ct：翻页动画时间基准 */
#include "calendar.h"      /* Calendar_FlipTick / Refresh / RefreshStatic / SetBtState */
#include "screen.h"        /* Screen_Draw：多屏增量绘制 */
#include "ui.h"            /* UI_CharAnim_Tick：形象状态动画帧推进 / UI_GetScreen */
#include "yx8320b.h"       /* YX8320B_GetTemp / GetHumi */
#include "hd9803.h"        /* Hd9803_GetState（蓝牙图标，#if HD9803_ENABLE） */

/* ------------------------------------------------------------------ */
/**
 * @brief  显示刷新调度（主循环每轮调用一次）
 * @note   集中封装所有“界面渲染”逻辑，让 main 只负责事件分发：
 *         - 翻页动画帧推进（每轮）
 *         - 形象状态动画推进（每轮）
 *         - 每轮刷新时钟 + 静态区块（日期 / 温湿度 / 蓝牙图标，内部 c_* 缓存去重）
 *         温湿度与蓝牙状态由各自模块提供（YX8320B 与 Hd9803 系列函数），本函数只负责“画”。
 * @param  t : [输入] 当前时间快照
 * @retval 无
 */
void App_Render(TimeStruct *t)
{
  Calendar_FlipTick((u32)bftm0_ct);

  UI_CharAnim_Tick();   /* 形象状态动画：250ms/帧（4 fps），内部 g_cani.ok 门控（空容器即返回） */

  /* 像素 UI（2026-11-17）：整屏重画按「秒/状态」节流，机器人帧率会掉到 1 fps，
     这里每轮补一次「只重贴机器人」，帧号/状态/屏幕没变则内部直接返回。 */
  Px_RobotTick();

  /* 多屏分发（2026-08-27）：主屏走 calendar 增量刷新；倒计时/闹钟屏走 screen.c */
  if (UI_GetScreen() == UI_SCREEN_MAIN)
  {
    /* 每轮无条件增量刷新：Calendar_Refresh 内部用 c_* 缓存去重，只有时间/温湿度
       真正变化才重绘，开销极小（几十条比较指令）。时钟位 + 静态区块（日期/温湿度）
       均在此完成（RefreshStatic 已并入，2026-09-05 去掉后半秒时隙门控）。
       不能用 g_time_update_flag 门控——它是单 bit，主循环若在采样分支(YX8320B 读取
       /HD9803 轮询)被阻塞超过整秒，flag 多次置位会被合并成一次消费，整秒被吞，
       表现为"5 秒等两秒才翻页成 7 秒"。每轮对比最新 t 则永不丢秒。 */
    Calendar_Refresh(t, YX8320B_GetTemp(), YX8320B_GetHumi());

    /* 主屏后台计时沙漏闪烁（2026-09-05）：正/倒计时退出按钮回主屏后计时继续跑，
       主卡右上角 0.5s 亮/灭提示；内部用 c_* 缓存去重，计时停止自动擦除。 */
    Screen_CntBlink((u32)bftm0_ct);

    /* 按键设置模式黄线（主屏，2026-09-04）：切项时清旧画新，无设置时不动 */
    Screen_SetupLine();

    /* 蓝牙图标（状态内部去重，变化才重绘）：跟随每轮刷新，
       不再绑后半秒时隙（2026-09-05）。 */
#if HD9803_ENABLE
    Calendar_SetBtState(Hd9803_GetState() == HD9803_ST_CONNECTED);
#else
    Calendar_SetBtState(0);
#endif
  }
  else
  {
    Screen_Draw(t);   /* 倒计时/闹钟屏增量绘制 */
  }

  /* 顶层状态角标（2026-11-25 用户：「兼容版要适配各种 UI，统一放在正上方」）：
     整点报时 / 闹钟已开 / 贪睡待响（闪）。**所有屏都画**（放在分支外），
     位置固定在正上方居中、图标是固件内置位图，换皮肤不影响；
     内部按「状态 + 闪相 + 屏号 + 背景版本」去重，状态不变零开销。 */
  Screen_StatusIcons();

  /* 设置项 1Hz 闪灭：必须在整屏绘制**之后**执行——放前面会被当帧重绘覆盖（闹钟屏实测） */
  Screen_BlinkTick();
}

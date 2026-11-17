/************************************************************************************************************
 * @file    ui.c
 * @brief   Flash UI 层：整屏上传 UI 背景图 + 用 Flash 字库叠加动态文字 + 翻页动画帧合成
 *
 * @note    独立编译单元（project 源组里单独勾选）。不再用 #include 聚合，依赖通过对应 .h 暴露。
 *          依赖关系：
 *            - spi_lcd.c  : 3 线 SPI 把像素发到屏幕（9bit，D8=D/C）
 *            - Display.c  : ST7789 命令/窗口（CASET/RASET/RAMWR）
 *            - flash_read.c: SPI1 读外部 Flash（背景图、字库、动画帧）
 *
 *          坐标体系与 Display.c 一致：逻辑 320x240，软件转置
 *          （lcd_map: GRAM col=逻辑y, row=逻辑x）实现横屏。
 *          背景图 bin 为小端 RGB565；ST7789 收数高字节先，故发送前逐像素交换两字节。
 *
 *          数据对应（build_flash.py 生成）：
 *            - UI 背景图  @g_ui_base   （=0x001000，逻辑行连续存储）
 *            - 字库容器   @g_font_base （动态读分区头，当前 =0x0A0800：UI 区扩 450K 后后移）
 *            - 翻页动画   @UI_ANIM_OFF （=0x110000，ANI2 帧表）
 ************************************************************************************************************/
#include "ui.h"
#include "BFTM0.h"   /* bftm0_ct（1ms 计数，调试分段计时用） */
#include "flash_read.h"
#include "spi_lcd.h"
#include "Display.h"  /* Display_off/on：切屏 DISPOFF 消除扫描线（2026-10-12） */
#include <string.h>
#include <stdio.h>

/* ============ 全局状态 ============ */
static u32 g_ui_base   = 0;     /* UI 背景图区在 Flash 中的起始偏移（来自分区头） */
static u32 g_ui_bg     = 0;     /* 当前屏背景基址（= g_ui_base + 屏号×UI_SCREEN_BYTES，2026-08-27 多屏） */
static u8  g_ui_screen = UI_SCREEN_MAIN;  /* 当前屏号（UI_SCREEN_*，2026-08-27 多屏） */
volatile u16 g_ui_bg_ver = 0;   /* 背景重传版本号：UI_SetScreen 每次自增；screen.c 检测变化强制重绘（2026-08-28） */
static u32 g_font_base = 0;     /* 字库区在 Flash 中的偏移 */
static u32 g_config_base = 0;   /* 配置区在 Flash 中的偏移（CONF 容器） */
static u8  g_line[320 * 2] __attribute__((aligned(4)));  /* 一行像素临时缓冲（640B；aligned(4)：u16 指针字节交换用，2026-10-28） */

/* ============ 切屏加速/无扫描线（2026-10-12 客户要求）：全局开关 ============ */
/* 写屏加速由 spi_lcd.c 的 SPI_LCD_PDMA_ENABLE 控制（2026-10-27 照项目 8 HT32L52241
   ✓已实机 DMA 成功 移植：SPI0 非 FIFO + PDMA TX（官方唯一 DMA 组合）+ RX 手动排空，
   避免原 FIFO+PDMA 的 RX 请求风暴饿死 TX 卡死——坑#88 修正4b/4c）。本文件不再有
   独立 DMA 分支：上传统一走 SPI_LCD_WriteData（内部按开关选 PDMA/轮询）。 */
#define UI_SWITCH_BLANK 0    /* 1=切屏期间 DISPOFF 关显示刷完再开（扫描线彻底不可见，代价短暂黑屏）；0=直接刷（有扫描线） */

/* 翻页动画合成缓冲（动画已停用=空闲）& 切屏 DMA 源 共用——复用省 RAM（1680B ≥ u16[640]=1280B）。
   aligned(4)：DMA 源地址须 2/4 字节对齐（ARM PDMA 16-bit 读不保证奇数地址）。 */
static u8  g_blit[20 * 42 * 2] __attribute__((aligned(4)));
static u8  g_ui_disp_visible = 1;  /* 1=显示可见（未关屏），0=已关屏（语音 0xB5）；UI_SetScreen 据此决定是否 DISPOFF 包上传（2026-10-12） */

/* 每屏人物区左上角（72x72 人物区，CANI/MSCOT 定位；2026-08-27 多屏，gen_ui.py 各屏 paste_girl 位置一致）
   2026-09-03 同步 gen_ui 新布局：屏1/屏2 语音助手面板改右下角 82x72 贴底（_face_panel 214,156）-> 人物居中 219,156 */
/* 坑#84 新主屏：CANI/MSCOT 透明像素填充色按屏选——主屏人物区移到右下空卡
   （2026-10-10 用户重调背景色：右下空卡 肉粉 #FDAE8F -> 灰白 230,225,211
   #E6E1D3，位置未变；同步改填充色否则人物周围出现旧色块），
   倒计时/闹钟屏仍在深色面板（#1B2336）。
   统一填充否则人物周围出现深/浅色方块（三屏人物区背景色不同）。 */
static u16 cani_fill(void)
{
  if (UI_GetScreen() == UI_SCREEN_MAIN)  return UI_RGB(230, 225, 211);  /* 主屏右下空卡灰白 */
  if (UI_GetScreen() == UI_SCREEN_ALARM) return UI_RGB(250, 212, 189);  /* 坑#85 闹钟屏右下卡肉粉 */
  return UI_RGB(254, 155, 124);                                         /* 坑#88 倒计时屏右下人物框粉 #FE9B7C */
}

static const u8 g_zone[UI_SCREEN_COUNT][2] = {
  [UI_SCREEN_MAIN]  = {UI_ZONE_X, UI_ZONE_Y},  /* 主屏：底部状态条人物窗 (16,156) */
  [UI_SCREEN_CNT]   = {243, 160},              /* 坑#88 倒计时：右下粉色人物框 (3).png x240-318 y158-234 中心(279,196)，72x72 人物对齐中心 -> 左243 顶160（2026-10-12 用户反馈空间太小换 (3).png，原 245,159 框小 2-3px） */
  [UI_SCREEN_ALARM] = {219, 136},              /* 闹钟：右下卡内居中（卡 y120-225 高105，人物72高 -> 136..208 中心172=卡中心172.5；坑#86 修：原156..228 超卡底225 且偏下） */
};

/* 字库索引缓存：build_flash.py 生成 {u32 code, u32 off, u16 w, u16 h} 表，按 code 升序 */
typedef struct { u32 code; u32 off; u16 w; u16 h; } font_idx_t;
/* 当前字库 76 个字模（8/28 新增 20 汉字后超出原 64 上限，导致主屏温湿度数字 2-9、
   小数点 0xE200/0xE202、蓝牙图标 0xE300/0xE301 查不到——2026-08-28 修复，提至 96）。
   2026-09-05 内存优化：实际 76 个，余量留 4 -> 80（省 96*12-80*12=192B RAM）。
   2026-09-05 坑#84 新主屏：+30px 冒号 0xE304 + 蓝牙文字「连接未」3 汉字 -> 84 个。
   2026-10-10 试过 0xE500 日期小号段（+11 提至 96），用户要求回退原大小 -> 移除回 84。 */
#define MAX_FONT_IDX 91   /* 89 + 坑#88 蓝牙2汉字（89->91，2026-10-12；89=84+PAUSE5） */
static font_idx_t g_idx[MAX_FONT_IDX];   /* 字库索引缓存（二分查找用） */
static u32 g_font_count = 0;             /* 字库字符个数 */
static u8  g_glyph[512];                 /* 单个字形位图缓冲（最大 44x84 1bpp，行对齐 6B x84 = 504B；2026-09-05 由 640 缩，省 128B） */
static const u8 g_blank[512] = {0};      /* 全空字模（找不到字形时仅还原背景） */
static u16 g_cur_w = 0, g_cur_h = 0;     /* 最近一次查到的字形尺寸（绘制时 1:1 用） */

/* ============ CRC32（IEEE 802.3 / zlib, poly 0xEDB88320） ============ */
/* 参数: data=待校验数据，len=长度。返回: CRC32 值。用于分区头/字库头/动画头校验。 */
static u32 ui_crc32(const u8 *data, u32 len)
{
  u32 crc = 0xFFFFFFFF;
  for (u32 i = 0; i < len; i++)
  {
    crc ^= data[i];
    for (int k = 0; k < 8; k++)
      crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320 : (crc >> 1);
  }
  return crc ^ 0xFFFFFFFF;
}

/* ============ 读分区头并校验 ============ */
/* ------------------------------------------------------------------
 * UI_LoadHeader：从 Flash 0x000000 读 HTUI 分区头并校验，记录各分区偏移。
 * 返回: 0=成功；-1=magic 错误；-2=CRC 错误。
 * 头布局（build_flash.py build_header）：[0..32) 数据（含各分区偏移），
 * [32..36) CRC32。
 * ------------------------------------------------------------------ */
int UI_LoadHeader(void)
{
  u8 buf[36];
  Flash_Read(0, buf, 36);
  if (!(buf[0] == 'H' && buf[1] == 'T' && buf[2] == 'U' && buf[3] == 'I'))
    return -1;
  u32 crc    = ui_crc32(buf, 32);
  u32 stored = (u32)buf[32] | ((u32)buf[33] << 8) | ((u32)buf[34] << 16) | ((u32)buf[35] << 24);
  if (crc != stored)
    return -2;
  /* UI 背景偏移 @8、字库偏移 @16、配置偏移 @24（与 build_header '<4sIIIIIIII' 字段对应） */
  g_ui_base     = (u32)buf[8]  | ((u32)buf[9]  << 8) | ((u32)buf[10] << 16) | ((u32)buf[11] << 24);
  g_font_base   = (u32)buf[16] | ((u32)buf[17] << 8) | ((u32)buf[18] << 16) | ((u32)buf[19] << 24);
  g_config_base = (u32)buf[24] | ((u32)buf[25] << 8) | ((u32)buf[26] << 16) | ((u32)buf[27] << 24);
  g_ui_bg       = g_ui_base;   /* 默认屏0 主界面背景基址 */
  g_ui_screen   = UI_SCREEN_MAIN;
  return 0;
}

/* ============ 缓存字库索引表 ============ */
/* ------------------------------------------------------------------
 * UI_CacheFont：读取 FONT 容器头并缓存全部索引条目到 g_idx[]。
 * 返回: 0=成功；-1=magic 错误；-2=CRC 错误。
 * FONT 头布局（build_flash.py '<4sBBBBB HIIII'）：
 *   count@11(H)、index_off@15(I)、crc@23(I)。
 * ------------------------------------------------------------------ */
int UI_CacheFont(void)
{
  if (g_font_base == 0) return -1;
  u8 h[28];
  Flash_Read(g_font_base, h, 28);
  if (!(h[0] == 'F' && h[1] == 'O' && h[2] == 'N' && h[3] == 'T'))
    return -1;
  u32 crc    = ui_crc32(h, 23);
  u32 stored = (u32)h[23] | ((u32)h[24] << 8) | ((u32)h[25] << 16) | ((u32)h[26] << 24);
  if (crc != stored)
    return -2;
  g_font_count = (u32)h[11] | ((u32)h[12] << 8) | ((u32)h[13] << 16) | ((u32)h[14] << 24);
  if (g_font_count > MAX_FONT_IDX) g_font_count = MAX_FONT_IDX;
  u32 index_off = (u32)h[15] | ((u32)h[16] << 8) | ((u32)h[17] << 16) | ((u32)h[18] << 24);
  /* 逐条读索引 {code u32, off u32, w u16, h u16}，共 12B/条 */
  for (u32 i = 0; i < g_font_count; i++)
  {
    u8 e[12];
    Flash_Read(g_font_base + index_off + i * 12, e, 12);
    g_idx[i].code = (u32)e[0] | ((u32)e[1] << 8) | ((u32)e[2] << 16) | ((u32)e[3] << 24);
    g_idx[i].off  = (u32)e[4] | ((u32)e[5] << 8) | ((u32)e[6] << 16) | ((u32)e[7] << 24);
    g_idx[i].w    = (u16)e[8] | ((u16)e[9] << 8);
    g_idx[i].h    = (u16)e[10] | ((u16)e[11] << 8);
  }
  return 0;
}

/* ============ 读用户配置（CONF 容器，见 requirements.md 2.6） ============ */
/* ------------------------------------------------------------------
 * UI_LoadConfig：从 Flash 读 CONF 配置容器并校验。
 * 头布局（build_flash.py build_config '<4sIII' + payload）：
 *   [0..4) magic 'CONF'、[4..8) version、[8..12) payload_len、
 *   [12..16) crc32(payload)、[16..) payload=16B 应用默认值。
 * 返回: 0=成功；-1=magic 错误；-2=CRC 错误（失败则 g_cfg 保持全 0）。
 * ------------------------------------------------------------------ */
static ui_cfg_t g_cfg;

int UI_LoadConfig(void)
{
  u8 hdr[16];
  if (g_config_base == 0) return -1;
  Flash_Read(g_config_base, hdr, 16);
  if (!(hdr[0]=='C' && hdr[1]=='O' && hdr[2]=='N' && hdr[3]=='F'))
  {
    printf("[CONF] bad magic -> default\r\n");
    return -1;
  }
  u32 plen = (u32)hdr[8] | ((u32)hdr[9] << 8) | ((u32)hdr[10] << 16) | ((u32)hdr[11] << 24);
  u32 pcrc = (u32)hdr[12] | ((u32)hdr[13] << 8) | ((u32)hdr[14] << 16) | ((u32)hdr[15] << 24);
  if (plen > sizeof(ui_cfg_t)) plen = sizeof(ui_cfg_t);
  u8 pl[sizeof(ui_cfg_t)];
  Flash_Read(g_config_base + 16, pl, plen);
  if (ui_crc32(pl, plen) != pcrc)
  {
    printf("[CONF] crc fail: stored=%08X calc=%08X -> default\r\n", pcrc, ui_crc32(pl, plen));
    return -2;
  }
  memcpy(&g_cfg, pl, plen);
  /* 注意：newlib-nano printf 不支持 "+" 标志（%+d 会挂起），tz 用 %d */
  printf("[CONF] ok: bright=%u%% tz=%d alarm=%u@%02u:%02u bt=%u wifi=%u\r\n",
         g_cfg.brightness, (int)g_cfg.timezone, g_cfg.alarm_on,
         g_cfg.alarm_hour, g_cfg.alarm_min, g_cfg.bt_paired, g_cfg.wifi_on);
  return 0;
}

const ui_cfg_t *UI_GetConfig(void) { return &g_cfg; }

/* UI_SetAlarm：写运行时闹钟配置（仅内存，不写 Flash）。
   语音"设置闹钟"命令写入；响铃状态机接入后在此读取触发。 */
void UI_SetAlarm(u8 on, u8 hour, u8 min)
{
  g_cfg.alarm_on   = on;
  g_cfg.alarm_hour = hour;
  g_cfg.alarm_min  = min;
}

/* ============ 查字形位图（二分，code 升序） ============ */
/* ------------------------------------------------------------------
 * ui_get_glyph：按 code 在索引中二分查找字形，并读取其位图到 g_glyph。
 * 参数: code=字符 Unicode/字库 code 段（如 '0'+d、0xE000+d、汉字码点）。
 * 返回: 指向 g_glyph 的 1bpp 位图指针；找不到返回 NULL 并把 g_cur_w/h 清零
 *       （调用方据此仅还原背景）。
 * 副作用: 更新 g_cur_w/g_cur_h 为查到字形的尺寸。
 * ------------------------------------------------------------------ */
static const u8 *ui_get_glyph(u32 code)
{
  int lo = 0, hi = (int)g_font_count - 1;
  while (lo <= hi)
  {
    int mid = (lo + hi) / 2;
    if (g_idx[mid].code == code)
    {
      g_cur_w = g_idx[mid].w; g_cur_h = g_idx[mid].h;
      Flash_Read(g_font_base + g_idx[mid].off, g_glyph, (u32)g_cur_h * ((g_cur_w + 7u) / 8u));
      return g_glyph;
    }
    if (g_idx[mid].code < code) lo = mid + 1;
    else hi = mid - 1;
  }
  g_cur_w = 0; g_cur_h = 0;   /* 找不到：尺寸归零，调用方仅还原背景 */
  return NULL;
}

/* UI_GlyphWidth：查字形实际宽度 px（变宽字形：日期数字 '1' 窄 '8' 宽，按实际笔画宽推进）。
   找不到返回 0。calendar.c 画日期串/秒用（按实际宽居中）。 */
int UI_GlyphWidth(u32 code)
{
  (void)ui_get_glyph(code);
  return (int)g_cur_w;
}

/* ============ 整屏上传 UI 背景（小端 RGB565 -> 高字节先 + 软件转置横屏） ============ */
/* ------------------------------------------------------------------
 * UI_UploadBackground：把 Flash 中的某屏背景整屏上传到屏幕。
 * 参数: screen=UI_SCREEN_*（0 主界面 / 1 倒计时 / 2 闹钟）；背景偏移 = g_ui_base + 屏号×153600。
 * 流程（每行窗口 + 一次片选，2026-08-28 修正）：
 *   必须每行重设窗口（CASET=单列 y、RASET=行 0..319）：软件转置下 GRAM 列=逻辑 y、
 *   行=逻辑 x，而 ST7789 RAMWR 连续写的地址是【列优先】递增（MADCTL=0 时先走满 CASET
 *   列再推进 RASET 行）——整屏一次窗口 + 流式写会按"逻辑列"消费数据，与 Flash 行连续
 *   存储（逻辑行 y 连续）顺序错位，背景显示乱掉。锁单列窗口则写满 320px 正好填满该列，
 *   与递增方向无关，故正确。
 *   Select 只拉一次包整段（省 240 次片选拉放 + Deselect 的 WaitDone 停顿）：
 *     循环 240 次：读 Flash 一行(SPI1) -> 字节交换 -> CASET/RASET/RAMWR -> 写屏(SPI0)。
 *   WriteData 内部按 SPI_LCD_PDMA_ENABLE 选 PDMA TX（非 FIFO，2026-10-27 照项目 8）或
 *   FIFO 轮询——本文件无独立 DMA 分支。
 * 说明: 逻辑行 y 在 Flash 中连续存储，偏移 = (y*320 + x)*2。
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------
 * UI_UploadBackground：把指定屏背景从 Flash 上传到 LCD GRAM。
 * 返回前打印整页耗时 + 各段累计耗时（BFTM0 微秒级计时，2026-10-28）。
 * ------------------------------------------------------------------ */
/* 高精度计时：BFTM0 以 48MHz 计数（CMP=48000 -> 1ms 中断，bftm0_ct++）；
 * 当前微秒 = bftm0_ct*1000 + CNTVAL/48（CNTVAL 计数 48 个 = 1µs）。
 * ⚠️ bftm0_ct*1000 在 u32 下每 ~71min 回绕一次；本函数只用于短段差值（<1s），
 *    测量窗口内不回绕即正确。 */
static u32 ui_ts_us(void)
{
  return (u32)bftm0_ct * 1000u + (BFTM_GetCounter(HT_BFTM0) / 48u);
}

void UI_UploadBackground(int screen)
{
  if (screen < 0 || screen >= UI_SCREEN_COUNT) screen = UI_SCREEN_MAIN;
  u32 base = g_ui_base + (u32)screen * UI_SCREEN_BYTES;

  /* 2026-10-28 流水线：读行 y+1（SPI1 阻塞）与写行 y（SPI0 TX PDMA + RX DMA 排空）并行，
     隐藏 read。g_line 单缓冲足够：读行 y+1 在写行 y 期间写入，打包在写行 y 完成后
     （g_blit 空闲时），下一行读覆盖 g_line 时行 y+1 已打包完。 */
  u32 t_all0 = ui_ts_us();
  u32 t_read = 0, t_swap = 0, t_cmd = 0, t_data = 0;

  /* ---- 行 0 准备：读 + 打包 + 窗口命令（CASET0/RASET0/RAMWR，无并行前导） ---- */
  u32 s, e;
  s = ui_ts_us();
  if (Flash_DualOK()) Flash_ReadDual(base, g_line, 320u * 2u);
  else                Flash_Read(base, g_line, 320u * 2u);
  e = ui_ts_us(); t_read += e - s;
  {
    const u32 *p32 = (const u32 *)g_line;
    u16 *q = (u16 *)g_blit;
    for (int i = 0; i < 160; i++)
    {
      u32 v = p32[i];
      q[4 * i]     = (u16)0x0100u | (u8)(v >> 8);
      q[4 * i + 1] = (u16)0x0100u | (u8)(v);
      q[4 * i + 2] = (u16)0x0100u | (u8)(v >> 24);
      q[4 * i + 3] = (u16)0x0100u | (u8)(v >> 16);
    }
  }
  SPI_LCD_Select();   /* 一次 Select 包整段；中途 Flash_Read 走独立 SPI1，不影响 SPI0 片选 */
  s = ui_ts_us();
  SPI_LCD_WriteCmd(ST7789_CASET);
  { u8 d[4] = {0, 0, 0, 0}; SPI_LCD_WriteData(d, 4); }
  SPI_LCD_WriteCmd(ST7789_RASET);
  { u8 d[4] = {0, 0, HI8(319), LO8(319)}; SPI_LCD_WriteData(d, 4); }
  SPI_LCD_WriteCmd(ST7789_RAMWR);
  e = ui_ts_us(); t_cmd += e - s;

  for (int y = 0; y < 240; y++)
  {
    /* 1) 写当前行：TX PDMA 启动 + RX DMA 排空（硬件跑，CPU 空闲） */
    s = ui_ts_us();
    SPI_LCD_WriteFrames16Start((u16 *)g_blit, 320u * 2u);
    SPI_LCD_RxDrainStart();
    e = ui_ts_us(); t_data += e - s;

    /* 2) 并行读下一行（SPI1 阻塞；期间 SPI0 TX/RX DMA 后台跑） */
    s = ui_ts_us();
    if (y < 239)
    {
      if (Flash_DualOK()) Flash_ReadDual(base + ((u32)y + 1u) * 320u * 2u, g_line, 320u * 2u);
      else                Flash_Read(base + ((u32)y + 1u) * 320u * 2u, g_line, 320u * 2u);
    }
    e = ui_ts_us(); t_read += e - s;

    /* 3) 等当前行写屏完成 + 停 RX 排空 */
    s = ui_ts_us();
    SPI_LCD_WriteFrames16Wait();
    SPI_LCD_RxDrainStop();
    e = ui_ts_us(); t_data += e - s;

    /* 4) 打包下一行 + 窗口命令 + RAMWR（写屏完成后 g_blit 空闲；下一轮写前 RAMWR 已就绪） */
    if (y < 239)
    {
      s = ui_ts_us();
      {
        const u32 *p32 = (const u32 *)g_line;
        u16 *q = (u16 *)g_blit;
        for (int i = 0; i < 160; i++)
        {
          u32 v = p32[i];
          q[4 * i]     = (u16)0x0100u | (u8)(v >> 8);
          q[4 * i + 1] = (u16)0x0100u | (u8)(v);
          q[4 * i + 2] = (u16)0x0100u | (u8)(v >> 24);
          q[4 * i + 3] = (u16)0x0100u | (u8)(v >> 16);
        }
      }
      e = ui_ts_us(); t_swap += e - s;

      /* 窗口锁单列：列=逻辑 y+1（固定本行），行=逻辑 x 0..319 */
      s = ui_ts_us();
      SPI_LCD_WriteCmd(ST7789_CASET);
      { u8 d[4] = {0, (u8)(y + 1), 0, (u8)(y + 1)}; SPI_LCD_WriteData(d, 4); }
      SPI_LCD_WriteCmd(ST7789_RASET);
      { u8 d[4] = {0, 0, HI8(319), LO8(319)}; SPI_LCD_WriteData(d, 4); }
      SPI_LCD_WriteCmd(ST7789_RAMWR);
      e = ui_ts_us(); t_cmd += e - s;
    }
  }
  SPI_LCD_Deselect();
  u32 t_all = ui_ts_us() - t_all0;
  printf("[UI] Upload total=%u.%03ums read=%u.%03ums swap=%u.%03ums cmd=%u.%03ums data=%u.%03ums\r\n",
         (unsigned)(t_all / 1000u), (unsigned)(t_all % 1000u),
         (unsigned)(t_read / 1000u), (unsigned)(t_read % 1000u),
         (unsigned)(t_swap / 1000u), (unsigned)(t_swap % 1000u),
         (unsigned)(t_cmd / 1000u), (unsigned)(t_cmd % 1000u),
         (unsigned)(t_data / 1000u), (unsigned)(t_data % 1000u));
}

/* ------------------------------------------------------------------
 * UI_SetScreen：切屏。设当前屏号（决定背景基址 g_ui_bg 与人物区 g_zone）+ 上传该屏背景。
 * 参数: screen=UI_SCREEN_*。返回: 0=成功，-1=非法屏。
 * 说明: 调用方应在切屏后重画该屏动态元素（见 screen.c Screen_Draw）；
 *       UI_CharAnim 每帧按 g_zone[当前屏] 重画，本函数再调 UI_CharAnim_Redraw 消除旧位置残留。
 * ------------------------------------------------------------------ */
/* 显示可见性（1=未关屏，0=已关屏 0xB5/长按）：UI_SetScreen 据此决定是否 DISPOFF 包上传
   （已关屏时背光灭、扫描线本就不可见，直接刷不折腾）。screen.c 的 0xB4/0xB5/disp_wake 调。 */
void UI_SetDispVisible(int vis) { g_ui_disp_visible = vis ? 1u : 0u; }

int UI_SetScreen(int screen)
{
  if (screen < 0 || screen >= UI_SCREEN_COUNT) return -1;
  g_ui_screen = (u8)screen;
  g_ui_bg     = g_ui_base + (u32)screen * UI_SCREEN_BYTES;
  g_ui_bg_ver++;                 /* 背景将重传：screen.c 检测版本变化强制全绘该屏动态元素（防重传后缓存失效不重绘） */
#if UI_SWITCH_BLANK
  /* 消除切屏扫描线（2026-10-12 客户要求）：背景是 240 行逐行流式上传（含 DMA 也是逐行窗口），
     关显示期间刷新不可见 -> 刷完再开，整屏一次出现，无扫描线。DISPOFF 保留 GRAM。
     仅当显示可见（背光/面板正常）时才折腾；已关屏（0xB5 背光灭）直接刷。 */
  if (g_ui_disp_visible)
  {
    Display_off();
    UI_UploadBackground(screen);
    Display_on();
  }
  else
  {
    UI_UploadBackground(screen);
  }
#else
  UI_UploadBackground(screen);
#endif
  UI_CharAnim_Redraw();   /* 人物区随屏变，重画当前帧到新位置 */
  return 0;
}

int UI_GetScreen(void) { return (int)g_ui_screen; }

/* ============ 在逻辑矩形内画单字（先还原该区域背景，再叠加墨点） ============ */
/* ------------------------------------------------------------------
 * UI_DrawGlyphRect：在逻辑矩形 (rx,ry)-(rx+rw-1,ry+rh-1) 内画一个字模。
 * 参数:
 *   rx,ry : 矩形左上角逻辑坐标
 *   rw,rh : 矩形宽高（一般等于字模实际尺寸，1:1 不缩放）
 *   code  : 字库 code（决定字形与尺寸）
 *   color : 墨点颜色（RGB565）；背景保留原 UI 背景图
 * 流程: 逐行——Flash 读该行背景段 -> 字节交换 -> 按字模墨点叠色 -> RAMWR 写该行。
 *      字形尺寸取自 ui_get_glyph 的 g_cur_w/g_cur_h；找不到字形则仅还原背景。
 * ------------------------------------------------------------------ */
void UI_DrawGlyphRect(int rx, int ry, int rw, int rh, u32 code, u16 color)
{
  if (rw <= 0 || rh <= 0) return;
  const u8 *g = ui_get_glyph(code);
  if (!g) g = g_blank;   /* 找不到：仅还原背景 */
  int row_bytes = (g_cur_w + 7) / 8;   /* 字模每行字节数（变尺寸） */

  for (int yy = 0; yy < rh; yy++)
  {
    int ly = ry + yy;
    /* 读该逻辑行的背景段（连续）——当前屏背景基址 g_ui_bg（2026-08-27 多屏） */
    Flash_Read(g_ui_bg + (u32)ly * 320u * 2u + (u32)rx * 2u, g_line, (u32)rw * 2u);
    for (int i = 0; i < rw * 2; i += 2)
    {
      u8 t = g_line[i]; g_line[i] = g_line[i + 1]; g_line[i + 1] = t;
    }
    /* 叠加字墨点：字模行/列 1:1 映射到矩形内（rw/rh 等于字模尺寸时不缩放） */
    for (int xx = 0; xx < rw; xx++)
    {
      int gx = (g_cur_w > 1) ? xx * g_cur_w / rw : 0;
      int gy = (g_cur_h > 1) ? yy * g_cur_h / rh : 0;
      int bit = (g[gy * row_bytes + (gx >> 3)] >> (7 - (gx & 7))) & 1;
      if (bit) { g_line[xx * 2] = HI8(color); g_line[xx * 2 + 1] = LO8(color); }
    }
    /* 逻辑行 ly -> GRAM 列=ly，行=rx..rx+rw-1 */
    SPI_LCD_Select();
    SPI_LCD_WriteCmd(ST7789_CASET);
    { u8 d[4] = {0, (u8)ly, 0, (u8)ly}; SPI_LCD_WriteData(d, 4); }
    SPI_LCD_WriteCmd(ST7789_RASET);
    { u8 d[4] = {HI8((u16)rx), LO8((u16)rx), HI8((u16)(rx + rw - 1)), LO8((u16)(rx + rw - 1))};
      SPI_LCD_WriteData(d, 4); }
    SPI_LCD_WriteCmd(ST7789_RAMWR);
    SPI_LCD_WriteData(g_line, (u32)rw * 2u);
    SPI_LCD_Deselect();
  }
}
/* ============ 语音助手小女孩形象（MASCOT 容器，尺寸动态 RGB565 + 1bpp 掩码） ============ */
/* 资源布局（build_flash.py build_mascot）：
   [0..4) 'MSCT' | [4..8) ver | [8..12) w | [12..16) h |
   [16..20) pixel_off | [20..24) mask_off | [24..256) 保留 |
   [pixel_off..) 像素 RGB565 行优先小端 | [mask_off..) 掩码 1bpp 行优先。 */
void UI_DrawMascot(int rx, int ry)
{
  u8 hdr[24];
  u32 w, h, po, mo;
  Flash_Read(UI_MASCOT_OFF, hdr, 24);
  if (!(hdr[0] == 'M' && hdr[1] == 'S' && hdr[2] == 'C' && hdr[3] == 'T'))
    return;                                    /* 旧镜像无 mascot 区：背景已擦黑，跳过 */
  w  = (u32)hdr[8]  | ((u32)hdr[9]  << 8) | ((u32)hdr[10] << 16) | ((u32)hdr[11] << 24);
  h  = (u32)hdr[12] | ((u32)hdr[13] << 8) | ((u32)hdr[14] << 16) | ((u32)hdr[15] << 24);
  po = (u32)hdr[16] | ((u32)hdr[17] << 8) | ((u32)hdr[18] << 16) | ((u32)hdr[19] << 24);
  mo = (u32)hdr[20] | ((u32)hdr[21] << 8) | ((u32)hdr[22] << 16) | ((u32)hdr[23] << 24);
  if (w == 0 || h == 0 || w > 80 || h > 80) return;
  {
    u32 pbase = UI_MASCOT_OFF + po;
    u32 mbase = UI_MASCOT_OFF + mo;
    u32 mrow  = (w + 7u) / 8u;                 /* 掩码每行字节数 */
    for (u32 yy = 0; yy < h; yy++)
    {
      int  ly = ry + (int)yy;
      u8   msk[10];
      Flash_Read(pbase + yy * w * 2u, g_line, w * 2u);        /* 像素行（小端 RGB565） */
      Flash_Read(mbase + yy * mrow, msk, mrow);               /* 掩码行（1bpp） */
      for (u32 i = 0; i < w * 2u; i += 2)                     /* 小端 -> 高字节先 */
      {
        u8 t = g_line[i]; g_line[i] = g_line[i + 1]; g_line[i + 1] = t;
      }
      for (u32 xx = 0; xx < w; xx++)                          /* 透明像素填人物区背景色（按屏：主屏肉粉/副屏面板色） */
      {
        if (!((msk[xx >> 3] >> (7 - (xx & 7))) & 1u))
        {
          g_line[xx * 2u] = HI8(cani_fill()); g_line[xx * 2u + 1] = LO8(cani_fill());
        }
      }
      SPI_LCD_Select();
      SPI_LCD_WriteCmd(ST7789_CASET);
      { u8 d[4] = {0, (u8)ly, 0, (u8)ly}; SPI_LCD_WriteData(d, 4); }
      SPI_LCD_WriteCmd(ST7789_RASET);
      { u8 d[4] = {HI8((u16)rx), LO8((u16)rx), HI8((u16)(rx + (int)w - 1)), LO8((u16)(rx + (int)w - 1))};
        SPI_LCD_WriteData(d, 4); }
      SPI_LCD_WriteCmd(ST7789_RAMWR);
      SPI_LCD_WriteData(g_line, w * 2u);
      SPI_LCD_Deselect();
    }
  }
}

/* ============ 计时沙漏真彩图标（ICO1 容器 @0x1E0000，16x16 RGB565 + 1bpp 掩码） ============ */
/* 资源布局（build_flash.py build_icon，同 MSCT）：
   [0..4) 'ICO1' | [4..8) ver | [8..12) w | [12..16) h |
   [16..20) pixel_off | [20..24) mask_off | [24..256) 保留 |
   [pixel_off..) RGB565 行优先小端（行0=图标底部，垂直预翻转） | [mask_off..) 1bpp 行优先。
   坐标：x,y 为设计坐标左上角（与 Display_FillRect 同约定）；GRAM 列 = 239-y（行0=图标底边），
   GRAM 行 = x..x+w-1。透明像素(mask=0)填面板色 UI_C_PANEL（=主卡 #1B2336，擦除无缝）。
   2026-09-05：用户 hourglass-16x16.png 为多色调（5 色明暗），1bpp glyph 只画纯色剪影被
   用户否定，改此真彩容器原样还原（含阴影/高光）。 */
void UI_DrawHourglass(int x, int y)
{
  u8 hdr[24];
  u32 w, h, po, mo;
  Flash_Read(UI_ICON_OFF, hdr, 24);
  if (!(hdr[0] == 'I' && hdr[1] == 'C' && hdr[2] == 'O' && hdr[3] == '1'))
    return;                                    /* 旧镜像无 ICO1 区：跳过（同 mascot 行为） */
  w  = (u32)hdr[8]  | ((u32)hdr[9]  << 8) | ((u32)hdr[10] << 16) | ((u32)hdr[11] << 24);
  h  = (u32)hdr[12] | ((u32)hdr[13] << 8) | ((u32)hdr[14] << 16) | ((u32)hdr[15] << 24);
  po = (u32)hdr[16] | ((u32)hdr[17] << 8) | ((u32)hdr[18] << 16) | ((u32)hdr[19] << 24);
  mo = (u32)hdr[20] | ((u32)hdr[21] << 8) | ((u32)hdr[22] << 16) | ((u32)hdr[23] << 24);
  if (w != 16 || h != 16) return;
  {
    u32 pbase = UI_ICON_OFF + po;
    u32 mbase = UI_ICON_OFF + mo;
    u32 mrow  = (w + 7u) / 8u;                 /* 掩码每行字节数（16px -> 2B） */
    int col0  = 239 - (y + (int)h - 1);        /* 图标底边设计 y+h-1 -> GRAM 列（行0=图标底部） */
    for (u32 yy = 0; yy < h; yy++)
    {
      int  ly = col0 + (int)yy;                /* GRAM 列（CASET，逐行单列） */
      u8   msk[4];
      Flash_Read(pbase + yy * w * 2u, g_line, w * 2u);   /* 像素行（小端 RGB565） */
      Flash_Read(mbase + yy * mrow, msk, mrow);          /* 掩码行（1bpp） */
      for (u32 i = 0; i < w * 2u; i += 2)               /* 小端 -> 高字节先 */
      {
        u8 t = g_line[i]; g_line[i] = g_line[i + 1]; g_line[i + 1] = t;
      }
      for (u32 xx = 0; xx < w; xx++)                    /* 透明像素填人物区背景色（按屏） */
      {
        if (!((msk[xx >> 3] >> (7 - (xx & 7))) & 1u))
        {
          g_line[xx * 2u] = HI8(cani_fill()); g_line[xx * 2u + 1] = LO8(cani_fill());
        }
      }
      SPI_LCD_Select();
      SPI_LCD_WriteCmd(ST7789_CASET);
      { u8 d[4] = {HI8((u16)ly), LO8((u16)ly), HI8((u16)ly), LO8((u16)ly)}; SPI_LCD_WriteData(d, 4); }
      SPI_LCD_WriteCmd(ST7789_RASET);
      { u8 d[4] = {HI8((u16)x), LO8((u16)x), HI8((u16)(x + (int)w - 1)), LO8((u16)(x + (int)w - 1))};
        SPI_LCD_WriteData(d, 4); }
      SPI_LCD_WriteCmd(ST7789_RAMWR);
      SPI_LCD_WriteData(g_line, w * 2u);
      SPI_LCD_Deselect();
    }
  }
}

/* ============ 坑#85 闹钟屏动态素材（ALM2 容器，整块不透明流式覆盖） ============ */
/* UI_DrawAlm2：设计坐标 x,y 左上角，流式整块覆盖绘制 item 素材。
   item0=开关 ON 控件（68x33，含卡片背景——覆盖背景自带 OFF 控件）；
   item1=星期未选中灰白瓶身（17x27，覆盖背景彩色瓶身色块）；
   item2=倒计时按钮1「暂停 PAUSE」整块（69x42，覆盖背景「开始 START」）；
   item3=开关 OFF 控件（68x33，坑#91 新关闭态空槽样式——从客户新图 320x239-(3).png 截取，
          OFF 状态由「恢复背景」改为贴此素材，开关效果更明显）。
   素材源=效果图截取，静态部分与静态版背景逐像素一致 -> 覆盖无缝。
   布局：[0..8) 'ALM2'|ver | [8..12) n | [12..) item 表{w:u16,h:u16,pixel_off:u32}*n |
         [28..256) 保留 | [256..) 各 item 像素（行0=素材底部 垂直预翻转，RGB565 小端）。
   ⚠️ 头读取必须覆盖 item 表：n=3 时表项在 [12..36)，n=4（坑#91 增 item3）时在 [12..44)——
      hdr 读 44B；原读 28B 只够 2 项，item2 表项（偏移 28..36）读到栈上垃圾 ->
      暂停图随机不显示/乱画（2026-10-12 用户反馈「开始键不变暂停」，坑#88 修正1 后 n=2->3 引入）；
      n=3 时读 36B 恰好够，n=4 后若仍读 36B，item3 表项（偏移 36..44）同样读到垃圾。 */
void UI_DrawAlm2(int item, int x, int y)
{
  u8 hdr[44];
  u32 w, h, po, n;
  Flash_Read(UI_ALM2_OFF, hdr, 44);
  if (!(hdr[0] == 'A' && hdr[1] == 'L' && hdr[2] == 'M' && hdr[3] == '2'))
    return;                                    /* 旧镜像无 ALM2 区：跳过 */
  n = (u32)hdr[8] | ((u32)hdr[9] << 8) | ((u32)hdr[10] << 16) | ((u32)hdr[11] << 24);
  if (item < 0 || (u32)item >= n) return;
  {
    const u8 *e = hdr + 12 + (u32)item * 8;
    w  = (u32)e[0] | ((u32)e[1] << 8);
    h  = (u32)e[2] | ((u32)e[3] << 8);
    po = (u32)e[4] | ((u32)e[5] << 8) | ((u32)e[6] << 16) | ((u32)e[7] << 24);
    if (w == 0 || h == 0 || w > 120 || h > 80) return;
    u32 pbase = UI_ALM2_OFF + po;
    int col0  = 239 - (y + (int)h - 1);        /* 素材底边设计 y+h-1 -> GRAM 列（行0=素材底部） */
    for (u32 yy = 0; yy < h; yy++)
    {
      int ly = col0 + (int)yy;                 /* GRAM 列（CASET，逐行单列） */
      Flash_Read(pbase + yy * w * 2u, g_line, w * 2u);   /* 像素行（小端 RGB565） */
      for (u32 i = 0; i < w * 2u; i += 2)               /* 小端 -> 高字节先 */
      {
        u8 t = g_line[i]; g_line[i] = g_line[i + 1]; g_line[i + 1] = t;
      }
      SPI_LCD_Select();
      SPI_LCD_WriteCmd(ST7789_CASET);
      { u8 d[4] = {HI8((u16)ly), LO8((u16)ly), HI8((u16)ly), LO8((u16)ly)}; SPI_LCD_WriteData(d, 4); }
      SPI_LCD_WriteCmd(ST7789_RASET);
      { u8 d[4] = {HI8((u16)x), LO8((u16)x), HI8((u16)(x + (int)w - 1)), LO8((u16)(x + (int)w - 1))};
        SPI_LCD_WriteData(d, 4); }
      SPI_LCD_WriteCmd(ST7789_RAMWR);
      SPI_LCD_WriteData(g_line, w * 2u);
      SPI_LCD_Deselect();
    }
  }
}

/* 读 MSCT 尺寸，按当前屏 72x72 人物区公式自动定位绘制（2026-08-27 多屏：人物区随屏）。
   修复：UI_DrawMascot(6,15) 硬编码只对 69x69 正确，改尺寸后会与动画错位残留。 */
void UI_DrawMascotCentered(void)
{
  u8 hdr[24];
  u32 w, h;
  int rx, ry;
  int zx = g_zone[g_ui_screen][0], zy = g_zone[g_ui_screen][1];
  Flash_Read(UI_MASCOT_OFF, hdr, 24);
  if (!(hdr[0] == 'M' && hdr[1] == 'S' && hdr[2] == 'C' && hdr[3] == 'T'))
    return;
  w = (u32)hdr[8]  | ((u32)hdr[9]  << 8) | ((u32)hdr[10] << 16) | ((u32)hdr[11] << 24);
  h = (u32)hdr[12] | ((u32)hdr[13] << 8) | ((u32)hdr[14] << 16) | ((u32)hdr[15] << 24);
  if (w == 0 || h == 0 || w > 80 || h > 80)
    return;
  rx = zx + (UI_ZONE_W - (int)w) / 2;
  ry = 239 - (zy + (int)h) + 1;
  UI_DrawMascot(rx, ry);
}


/* ============ 语音助手 状态动画（CANI 容器 @0x150000，帧图 default_girl_1..48） ============ */
/* 容器布局（build_flash.py build_canim）：
   [0..256) 'CANI'|ver|w|h|nstates|nframes|状态表{start:u32,count:u32}...|pad
   [256..)  帧区：第 k 帧偏移 = 256 + k*(w*h*2 + ceil(w/8)*h)；
            每帧 = 像素(w*h*2 小端 RGB565) + 掩码(ceil(w/8)*h 1bpp)，垂直预翻转(行0=人物底部)。
   位置：按帧 w/h 在 72x72 人物区居中（UI_ZONE_*，与静态 mascot 同一区，方向一致）。 */
ui_cani_t g_cani;
static u8  g_cani_enable = 0;   /* 动画播放开关（calendar.c Enable(1) 开启，0=静态帧） */

int UI_CharAnim_Init(void)
{
  u8 hdr[32];                            /* 2026-09-05 内存优化：头只需 24B（原 256B 栈浪费） */
  u32 v;
  memset(&g_cani, 0, sizeof(g_cani));
  Flash_Read(UI_CANI_OFF, hdr, 24);
  if (!(hdr[0] == 'C' && hdr[1] == 'A' && hdr[2] == 'N' && hdr[3] == 'I'))
    return -1;                                   /* 旧镜像无 CANI：保持停用 */
  v = (u32)hdr[8]  | ((u32)hdr[9]  << 8) | ((u32)hdr[10] << 16) | ((u32)hdr[11] << 24);
  g_cani.w = (u16)v;
  v = (u32)hdr[12] | ((u32)hdr[13] << 8) | ((u32)hdr[14] << 16) | ((u32)hdr[15] << 24);
  g_cani.h = (u16)v;
  v = (u32)hdr[16] | ((u32)hdr[17] << 8) | ((u32)hdr[18] << 16) | ((u32)hdr[19] << 24);
  g_cani.nstates = (u8)v;
  v = (u32)hdr[20] | ((u32)hdr[21] << 8) | ((u32)hdr[22] << 16) | ((u32)hdr[23] << 24);
  g_cani.nframes = (u8)v;
  if (g_cani.w == 0 || g_cani.h == 0 || g_cani.w > 80 || g_cani.h > 80)
    return -1;
  if (g_cani.nstates > UI_CANI_MAX_STATE)
    g_cani.nstates = UI_CANI_MAX_STATE;
  for (u8 i = 0; i < g_cani.nstates; i++)
  {
    u8 e[8];
    Flash_Read(UI_CANI_OFF + 24u + (u32)i * 8u, e, 8);
    g_cani.state_start[i] = (u16)((u32)e[0] | ((u32)e[1] << 8) | ((u32)e[2] << 16) | ((u32)e[3] << 24));
    g_cani.state_count[i] = (u16)((u32)e[4] | ((u32)e[5] << 8) | ((u32)e[6] << 16) | ((u32)e[7] << 24));
  }
  g_cani.ok = (g_cani.nframes > 0) ? 1 : 0;
  g_cani.last_tick = bftm0_ct;
  return g_cani.ok ? 0 : -1;
}

int UI_CharAnim_Ready(void) { return g_cani.ok ? 1 : 0; }

void UI_CharAnim_Enable(int en) { g_cani_enable = en ? 1 : 0; }

int UI_CharAnim_SetState(int st)
{
  if (!g_cani.ok) return -1;
  if (st < 0 || st >= (int)g_cani.nstates) return -1;
  if (g_cani.state_count[st] == 0) return -1;
  g_cani.one_shot = 0;              /* 新 SetState 打断进行中的一次性响应动画 */
  if (st == g_cani.cur_state && !g_cani.trans)
  {
    g_cani.cur_frame = 0;            /* 同状态：重播循环 */
    return 0;
  }
  /* 状态编号宏见 ui.h UI_CANI_STATE_*（IDLE=0 / WAKE=1 / ALARM=2）。
     切到 WAKE/ALARM 走过渡（当前状态循环前 4 帧 -> 变化 4 帧 -> 目标状态循环）；
     切回 IDLE / 同状态 直切（无过渡帧定义）。 */
  if (st == UI_CANI_STATE_WAKE || st == UI_CANI_STATE_ALARM)
  {
    g_cani.trans        = 1;
    g_cani.trans_target = (u8)st;
    g_cani.trans_phase  = 0;         /* 阶段0：重播当前循环前 4 帧 */
    g_cani.trans_step   = 0;
  }
  else
  {
    g_cani.cur_state = (u8)st;
    g_cani.cur_frame = 0;
    g_cani.trans     = 0;
  }
  return 0;
}

/* 一次性响应动画（如"修改成功"）：切到 st 状态播完一遍后自动回到原状态。
   适合 4~8 帧短动画（如 UI_CANI_STATE_OK）；该状态无帧图时返回 -1 保持原状态。
   已在一次性动画中再次触发时：只重启动画本身，保留最初的返回状态。 */
int UI_CharAnim_PlayOnce(int state)
{
  if (!g_cani.ok) return -1;
  if (state < 0 || state >= (int)g_cani.nstates) return -1;
  if (g_cani.state_count[state] == 0) return -1;
  if (!g_cani.one_shot)
    g_cani.return_state = g_cani.cur_state;   /* 记录返回目标（仅首次） */
  g_cani.cur_state = (u8)state;
  g_cani.cur_frame = 0;
  g_cani.trans     = 0;              /* 打断过渡 */
  g_cani.one_shot  = 1;
  return 0;
}

/* 帧 w/h 在 当前屏 72x72 人物区居中 -> GRAM 左上 (rx,ry)（2026-08-27 多屏：人物区随屏） */
static void cani_pos(int w, int h, int *rx, int *ry)
{
  int zx = g_zone[g_ui_screen][0], zy = g_zone[g_ui_screen][1];
  *rx = zx + (UI_ZONE_W - w) / 2;
  *ry = 239 - (zy + h) + 1;
}

/* 画第 frame 帧（全帧流式：Flash 行读 -> GRAM 行写，透明像素填面板色 UI_C_PANEL） */
static void cani_draw_frame(u16 frame)
{
  int rx, ry;
  u32 mrow   = ((u32)g_cani.w + 7u) / 8u;
  u32 stride = (u32)g_cani.w * g_cani.h * 2u + mrow * g_cani.h;
  u32 base   = UI_CANI_OFF + 256u + (u32)frame * stride;

  cani_pos(g_cani.w, g_cani.h, &rx, &ry);
  for (u32 yy = 0; yy < g_cani.h; yy++)
  {
    int  ly = ry + (int)yy;
    u8   msk[10];
    Flash_Read(base + yy * g_cani.w * 2u, g_line, g_cani.w * 2u);
    Flash_Read(base + (u32)g_cani.w * g_cani.h * 2u + yy * mrow, msk, mrow);
    for (u32 i = 0; i < g_cani.w * 2u; i += 2)      /* 小端 -> 高字节先 */
    {
      u8 t = g_line[i]; g_line[i] = g_line[i + 1]; g_line[i + 1] = t;
    }
    for (u32 xx = 0; xx < g_cani.w; xx++)           /* 透明像素填人物区背景色（按屏：主屏右下空卡肉粉/副屏面板色） */
    {
      if (!((msk[xx >> 3] >> (7 - (xx & 7))) & 1u))
      {
        g_line[xx * 2u] = HI8(cani_fill()); g_line[xx * 2u + 1] = LO8(cani_fill());
      }
    }
    SPI_LCD_Select();
    SPI_LCD_WriteCmd(ST7789_CASET);
    { u8 d[4] = {0, (u8)ly, 0, (u8)ly}; SPI_LCD_WriteData(d, 4); }
    SPI_LCD_WriteCmd(ST7789_RASET);
    { u8 d[4] = {HI8((u16)rx), LO8((u16)rx), HI8((u16)(rx + (int)g_cani.w - 1)), LO8((u16)(rx + (int)g_cani.w - 1))};
      SPI_LCD_WriteData(d, 4); }
    SPI_LCD_WriteCmd(ST7789_RAMWR);
    SPI_LCD_WriteData(g_line, g_cani.w * 2u);
    SPI_LCD_Deselect();
  }
  /* [调试] 打印本帧实际绘制耗时（Flash 读 + SPI 写屏）。1ms 分辨率；调完可删。 */
 // printf("[CANI] frame %u 绘制 %lu ms（@%lu）\r\n",
   //      (unsigned)frame, (unsigned long)((u32)bftm0_ct - t0), (unsigned long)t0);
}

/* 画某状态的某帧（帧号越界则跳过不画） */
static void cani_draw_state_frame(int state, u16 fidx)
{
  u16 g;
 // printf("当前绘制序号为%x\n",g_cani.cur_frame);
  if (state < 0 || state >= (int)g_cani.nstates) return;
  if (fidx >= g_cani.state_count[state]) return;
  g = (u16)(g_cani.state_start[state] + fidx);
  if (g >= g_cani.nframes) return;
  cani_draw_frame(g);
}

/* 按当前屏人物区重画当前帧（切屏后旧位置残留由新位置覆盖；未启用/无容器则返回 -1）。 */
int UI_CharAnim_Redraw(void)
{
  if (!g_cani.ok || !g_cani_enable) return -1;
  cani_draw_state_frame(g_cani.cur_state, g_cani.cur_frame);
  return 0;
}

/* 目标状态的变化帧槽位：16 帧布局 = [0-8)循环 + [8-12)变化到唤醒 + [12-16)变化到闹铃。
   目标相对 WAKE 每高一档 +TRANS_CNT 帧：WAKE->8、ALARM->12。 */
static u16 cani_trans_slot(u8 target)
{
  return (u16)(UI_CANI_LOOP_CNT + ((u16)target - UI_CANI_STATE_WAKE) * UI_CANI_TRANS_CNT);
}

static u8 s_idle_hold = 0;   /* 待机停留帧计数（到 UI_CANI_IDLE_HOLD_INTERVALS 放行一次） */

void UI_CharAnim_Tick(void)
{
  if (!g_cani_enable) return;              /* 关闭时保持当前画面 */
  if (!g_cani.ok) return;
  if ((u32)(bftm0_ct - g_cani.last_tick) < UI_CANI_MS_PER_FRAME) return;  /* 0.125s/帧 */
  g_cani.last_tick = bftm0_ct;

  if (g_cani.trans)
  {
    /* 过渡：阶段0 = 当前状态循环前 4 帧；阶段1 = 变化帧（目标唤醒[8..12)，目标闹铃[12..16)） */
    if (g_cani.trans_phase == 0)
    {
      cani_draw_state_frame(g_cani.cur_state, (u16)g_cani.trans_step);   /* 循环[0..3] */
      g_cani.trans_step++;
      if (g_cani.trans_step >= UI_CANI_TRANS_PRE)
      {
        g_cani.trans_phase = 1;
        g_cani.trans_step  = 0;
      }
    }
    else
    {
      /* 变化帧槽位：WAKE->[8-12)，ALARM->[12-16)（见 cani_trans_slot） */
      u16 slot = cani_trans_slot(g_cani.trans_target);
      cani_draw_state_frame(g_cani.cur_state, (u16)(slot + g_cani.trans_step));
      g_cani.trans_step++;
      if (g_cani.trans_step >= UI_CANI_TRANS_CNT)
      {
        /* 过渡完成：切到目标状态的正常循环 */
        g_cani.cur_state = g_cani.trans_target;
        g_cani.cur_frame = 0;
        g_cani.trans     = 0;
      }
    }
  }
  else
  {
    /* 正常循环：只播每状态前 8 帧；不足 8 帧按实际帧数（如当前 idle 2 帧 -> 0,1 交替），
       避免帧号越界被跳过导致画面卡死静止。 */

    u16 nloop = g_cani.state_count[g_cani.cur_state];
    if (nloop > UI_CANI_LOOP_CNT) nloop = UI_CANI_LOOP_CNT;

    /* 用户需求：待机循环在指定帧停留 N 个帧间隔（1 帧间隔 = MS_PER_FRAME=125ms）。
       停留期间不换帧（保持当前画面），计数到 HOLD_INTERVALS 后清零放行。 */
    if (g_cani.cur_state == UI_CANI_STATE_IDLE &&
        g_cani.cur_frame == UI_CANI_IDLE_HOLD_FRAME)
    {
      s_idle_hold++;
      if (s_idle_hold < 16)
        return;
      s_idle_hold = 0;
    }
    g_cani.cur_frame++;
    if (g_cani.cur_frame >= nloop)
    {
      g_cani.cur_frame = 0;
      if (g_cani.one_shot)
      {
        /* 一次性响应动画播完一遍：回到原状态（下次 Tick 从 0 帧画起） */
        g_cani.cur_state = g_cani.return_state;
        g_cani.one_shot  = 0;
        return;
      }
    }
    cani_draw_state_frame(g_cani.cur_state, g_cani.cur_frame);
  }
}


/* ============ UTF-8 解析（取下一码点，*pp 前移） ============ */
/* ------------------------------------------------------------------
 * utf8_next：从字符串当前位置解析出一个 Unicode 码点。
 * 参数: pp=指向字符串指针的指针；调用后 *pp 前移到下一字符。
 * 返回: 解析出的码点（1-4 字节 UTF-8；非法序列按单字节原样返回）。
 * ------------------------------------------------------------------ */
static u32 utf8_next(const char **pp)
{
  const u8 *p = (const u8 *)*pp;
  u32 c; int n;
  if (p[0] < 0x80)              { c = p[0]; n = 1; }
  else if ((p[0] & 0xE0) == 0xC0) { c = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); n = 2; }
  else if ((p[0] & 0xF0) == 0xE0) { c = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); n = 3; }
  else if ((p[0] & 0xF8) == 0xF0) { c = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); n = 4; }
  else                          { c = p[0]; n = 1; }
  *pp = (const char *)(p + n);
  return c;
}

/* ============ 画 UTF-8 字符串（每字占 cell_w×cell_h，从左上 (x,y) 起水平排列） ============ */
/* 参数: str=UTF-8 字符串；x,y=起点；cell_w/h=每字格宽高；color=墨色。 */
void UI_DrawText(const char *str, int x, int y, int cell_w, int cell_h, u16 color)
{
  const char *p = str;
  int i = 0;
  while (*p)
  {
    u32 code = utf8_next(&p);
    UI_DrawGlyphRect(x + i * cell_w, y, cell_w, cell_h, code, color);
    i++;
  }
}

/* ============ 直接传 Unicode 码点数组（无需源文件中文字面量） ============ */
/* 参数: codes=码点数组；n=个数；x,y=起点；cell_w/h=每字格；color=墨色。 */
void UI_DrawGlyphs(const u32 *codes, int n, int x, int y, int cell_w, int cell_h, u16 color)
{
  for (int i = 0; i < n; i++)
    UI_DrawGlyphRect(x + i * cell_w, y, cell_w, cell_h, codes[i], color);
}

/* ============ 按变体 code 段画数字串（1:1 清晰） ============ */
/* 每个 '0'-'9' 映射到 base+d，其余字符用原 Unicode；横向按各字模实际宽排列
   （同尺寸数字等宽）。用于日期(0xE000)/温湿度(0xE100)的小号数字，
   避免误查到时钟大号字模(0x30)被缩放糊掉。
   参数: s=数字串（可含 '-'、'.'）；x,y=起点；color=墨色；base=数字 code 段基址。 */
void UI_DrawVarDigits(const char *s, int x, int y, u16 color, u32 base)
{
  int cx = x;
  for (const char *p = s; *p; p++)
  {
    u32 c = (u32)(unsigned char)*p;
    u32 code;
    if (c >= '0' && c <= '9') code = base + (c - '0');
    else if (c == '.')        code = 0xE200u;   /* 小数点（温湿度显示用） */
    else                      code = c;
    const u8 *g = ui_get_glyph(code);
    u16 w = g_cur_w, h = g_cur_h;
    if (!g) { w = 0; h = 0; }
    UI_DrawGlyphRect(cx, y, w, h, code, color);
    cx += w;
  }
}

/* ============ UI_Show 统一显示入口（2026-08 架构重构） ============ */
/* ------------------------------------------------------------------
 * 档位表（元素 -> {数字字模段基址, 点号 code, 字高, 统一格宽, 画前清除宽, 对齐}）。
 * 每个显示元素固定一块 clear_w × h 区域：UI_Show 整块"背景+新字形"合成直接覆盖
 * （clear_w 覆盖该元素最大内容，找不到字形 → 该行仅还原背景）。
 * 与 ui.h UI_SLOT_* 枚举一一对应；数字统一格宽（日期/秒 12px、温湿度 12px）。
 * ------------------------------------------------------------------ */
typedef struct {
  u32 base;    /* 数字字模段基址：0xE000 日期/秒、0xE100 温湿度、0x30 时钟大数字；汉字段用 0（直接码点查） */
  u16 dot;     /* 点号 code：0xE202 日期点、0xE200 温湿度点；无点号用 0 */
  u8  h;       /* 字形高 */
  u8  cell_w;  /* 统一格宽（数字/汉字推进步长） */
  u8  clear_w; /* 画前清除区域宽（固定，覆盖该元素最大内容） */
  u8  align;   /* UI_ALIGN_LEFT=0 / UI_ALIGN_CENTER=1 */
} ui_slot_t;

static const ui_slot_t g_slot[UI_SLOT_COUNT] = {
  [UI_SLOT_SEC]   = {0xE000, 0xE202, 22, UI_DATE_DIGIT_W, 31, UI_ALIGN_CENTER},
  [UI_SLOT_DATE]  = {0xE000, 0xE202, 22, UI_DATE_DIGIT_W, 141, UI_ALIGN_CENTER},
  [UI_SLOT_DATE2] = {0xE000, 0xE202, 22, UI_DATE_DIGIT_W, 110, UI_ALIGN_CENTER},  /* 坑#84 新主屏日期（右上卡） */
  [UI_SLOT_DIGIT] = {0xE000, 0xE202, 22, UI_DATE_DIGIT_W, UI_DATE_DIGIT_W, UI_ALIGN_LEFT},
  [UI_SLOT_DOT]   = {0,      0xE202, 22, UI_DATE_DOT_W,   UI_DATE_DOT_W,   UI_ALIGN_LEFT},
  [UI_SLOT_TH]    = {0xE100, 0xE200, 30, UI_TH_DIGIT_W,  69, UI_ALIGN_CENTER},
  [UI_SLOT_TH2]   = {0xE000, 0xE202, 22, UI_DATE_DIGIT_W, 50, UI_ALIGN_CENTER},  /* 坑#84 新主屏温湿度数值（22高） */
  [UI_SLOT_WEEK]  = {0,      0,      22, 18,              90, UI_ALIGN_CENTER},  /* 蓝牙胶囊文字"蓝牙已连接/蓝牙未连接"5汉字90px（2026-10-12） */
  [UI_SLOT_LUNAR] = {0,      0,      22, 18,              72, UI_ALIGN_CENTER},
  [UI_SLOT_CLOCK] = {0x30,   0,      84, 40,              40, UI_ALIGN_LEFT},
  [UI_SLOT_CHAR]  = {0,      0,      22, 18,              18, UI_ALIGN_LEFT},
  [UI_SLOT_EN]    = {0,      0,      8,  8,               8,  UI_ALIGN_LEFT},   /* 坑#88 英文小字（按钮1 PAUSE，8x8 ASCII 段） */
};

/* ------------------------------------------------------------------
 * UI_Show：统一显示入口。只输入 起点坐标 + 内容 + 档位宏 + 颜色。
 * 参数: x,y=设计坐标（顶原点；x 无镜像=GRAM 行，y=文字顶部设计 y，UI_Show 内部换算）；
 *       s=UTF-8 串（数字/点号/汉字）；
 *       slot=UI_SLOT_*（查 g_slot 得字模段/字高/格宽/清除宽/对齐）；color=墨色。
 * 流程: 1) 算串宽
 *       2) 按 align 对齐定内容起点（居中 = x + (clear_w - w)/2）
 *       3) **整块合成直接覆盖**（2026-08 优化，取代"先清再逐字形画"）：
 *          逐行 读背景(clear_w) -> 把所有字形本行墨点叠进缓冲 -> 一次写回整行。
 *          整块 clear_w×h 全由"背景+新字形"覆盖，旧内容被直接盖掉，无需先清、无残留；
 *          背景仅读一次、屏仅写一次（原方案：清整块+每字形各读各写，多次读写）。
 * 绘制方向: 从设计行 y（顶）往下逐行，GRAM 列 = 239 - 设计行（设备 GRAM 垂直镜像），
 *           字模已预翻转（存储行 0=字符底部），该设计行对应字模行 = (h-1)-yy。
 *           与背景(设计行 r -> 239-r)、MSCOT/CANI(239-(top+h)+1) 约定一致，像素级对齐。
 * 说明: 时钟大数字/秒/温湿度/日期 共用本函数；日期=整串一次合成覆盖整卡（calendar.c draw_date_string）。
 * ------------------------------------------------------------------ */
void UI_Show(int x, int y, const char *s, u8 slot, u16 color)
{
  if (slot >= UI_SLOT_COUNT) return;
  const ui_slot_t *sp = &g_slot[slot];

  /* 1) 算串宽 */
  int w = 0;
  for (const char *p = s; *p; )
  {
    u32 c = utf8_next(&p);
    w += (c == '.') ? UI_DATE_DOT_W : sp->cell_w;
  }

  /* 2) 对齐定内容起点 */
  int cx = x;
  if (sp->align == UI_ALIGN_CENTER)
    cx = x + (sp->clear_w - w) / 2;

  /* 3) 整块合成：逐行 读背景 -> 叠所有字形本行墨点 -> 写回（直接覆盖旧内容） */
  for (int yy = 0; yy < sp->h; yy++)
  {
    int  ly  = 239 - (y + yy);   /* 设计行 (y+yy) -> GRAM 列（设备垂直镜像：GRAM 列 = 239 - 设计行） */
    int  gyy = sp->h - 1 - yy;   /* 该设计行对应的字模存储行（字模已预翻转：存储行 0 = 字符底部） */
    Flash_Read(g_ui_bg + (u32)ly * 320u * 2u + (u32)x * 2u, g_line, (u32)sp->clear_w * 2u);
    for (int i = 0; i < sp->clear_w * 2; i += 2)
    {
      u8 t = g_line[i]; g_line[i] = g_line[i + 1]; g_line[i + 1] = t;
    }
    /* 叠加本行所有字形墨点（1:1，字形顶 = 区域顶，字形高 = sp->h） */
    int gx = cx - x;                     /* 当前字形在 clear_w 区域内的列偏移 */
    for (const char *p = s; *p; )
    {
      u32 c = utf8_next(&p);
      u32 code; u8 gw;
      if (c == '.')            { code = sp->dot; gw = UI_DATE_DOT_W; }
      else if (c >= '0' && c <= '9') { code = sp->base + (c - '0'); gw = sp->cell_w; }
      else                     { code = c;      gw = sp->cell_w; }
      const u8 *g = ui_get_glyph(code);
      if (g)
      {
        int row_bytes = (g_cur_w + 7) / 8;
        for (int xx = 0; xx < gw; xx++)
        {
          int gxi = (g_cur_w > 1) ? xx * g_cur_w / gw : 0;
          int bit = (g[gyy * row_bytes + (gxi >> 3)] >> (7 - (gxi & 7))) & 1;
          if (bit)
          {
            int pos = gx + xx;
            if (pos >= 0 && pos < sp->clear_w)
            {
              g_line[pos * 2] = HI8(color);
              g_line[pos * 2 + 1] = LO8(color);
            }
          }
        }
      }
      gx += gw;
    }
    /* GRAM 列=ly（=239-设计行），行=x..x+clear_w-1 */
    SPI_LCD_Select();
    SPI_LCD_WriteCmd(ST7789_CASET);
    { u8 d[4] = {0, (u8)ly, 0, (u8)ly}; SPI_LCD_WriteData(d, 4); }
    SPI_LCD_WriteCmd(ST7789_RASET);
    { u8 d[4] = {HI8((u16)x), LO8((u16)x), HI8((u16)(x + sp->clear_w - 1)), LO8((u16)(x + sp->clear_w - 1))};
      SPI_LCD_WriteData(d, 4); }
    SPI_LCD_WriteCmd(ST7789_RAMWR);
    SPI_LCD_WriteData(g_line, (u32)sp->clear_w * 2u);
    SPI_LCD_Deselect();
  }
}

/* 在 GRAM 列=ly、行=x0..x1 写一段纯色（chip 填底后的墨点行用） */
static void ui_run(int ly, int x0, int x1, u16 color)
{
  if (x0 > x1) return;
  SPI_LCD_Select();
  SPI_LCD_WriteCmd(ST7789_CASET);
  { u8 d[4] = {0, (u8)ly, 0, (u8)ly}; SPI_LCD_WriteData(d, 4); }
  SPI_LCD_WriteCmd(ST7789_RASET);
  { u8 d[4] = {HI8((u16)x0), LO8((u16)x0), HI8((u16)x1), LO8((u16)x1)};
    SPI_LCD_WriteData(d, 4); }
  SPI_LCD_WriteCmd(ST7789_RAMWR);
  for (int i = x0; i <= x1; i++)
  {
    SPI_LCD_WriteDataByte(HI8(color));
    SPI_LCD_WriteDataByte(LO8(color));
  }
  SPI_LCD_Deselect();
}

/* ------------------------------------------------------------------
 * UI_DrawTextInk：仅叠墨点（不清背景、不读 Flash 背景）。
 * 用途：倒计时/闹钟屏的 chip 文字——先 Display_FillRect 填 chip 底色，再本函数把字形墨点
 *       画上去（字形空白行不写，chip 内的底色不被覆盖）。1:1 无缩放，'.' 用 slot 点号。
 * 参数: x,y=文字左上角设计坐标；s=UTF-8；slot=UI_SLOT_*（用 cell_w 推进，h 约束行数）；
 *       color=墨色。2026-08-27 多屏 chip。
 * ------------------------------------------------------------------ */
void UI_DrawTextInk(int x, int y, const char *s, u8 slot, u16 color)
{
  if (slot >= UI_SLOT_COUNT) return;
  const ui_slot_t *sp = &g_slot[slot];
  int gx = x;
  for (const char *p = s; *p; )
  {
    u32 c = utf8_next(&p);
    u32 code; u8 gw;
    if (c == '.')            { code = sp->dot; gw = UI_DATE_DOT_W; }
    else if (c >= '0' && c <= '9') { code = sp->base + (c - '0'); gw = sp->cell_w; }
    else                     { code = c;      gw = sp->cell_w; }
    const u8 *g = ui_get_glyph(code);
    if (g)
    {
      int row_bytes = (g_cur_w + 7) / 8;
      int gh = (sp->h < g_cur_h) ? sp->h : (int)g_cur_h;
      for (int yy = 0; yy < gh; yy++)
      {
        int ly = 239 - (y + yy);           /* 设计行 -> GRAM 列（垂直镜像） */
        /* 修复（2026-08-28）：字模 flip_v 预补偿存储（存储行0=字符底部），
           顶对齐 ly=239-(y+yy) 必须配 字模行 = g_cur_h-1-yy（字符顶），
           否则 chip/星期文字上下颠倒（与 UI_Show 的 gyy=h-1-yy 一致）。 */
        int gyy = (int)g_cur_h - 1 - yy;
        int run0 = -1;
        for (int xx = 0; xx < gw && xx < (int)g_cur_w; xx++)
        {
          int gxi = (g_cur_w > 1) ? xx * g_cur_w / gw : 0;
          int bit = (g[gyy * row_bytes + (gxi >> 3)] >> (7 - (gxi & 7))) & 1;
          if (bit) { if (run0 < 0) run0 = gx + xx; }
          else if (run0 >= 0) { ui_run(ly, run0, gx + xx - 1, color); run0 = -1; }
        }
        if (run0 >= 0) ui_run(ly, run0, gx + gw - 1, color);
      }
    }
    gx += gw;
  }
}

/* ===========================================================================================================
 * 翻页动画（ANI2 区 @ Flash 0x110000）
 *
 * 数据布局（与 build_flash.py build_anim 对齐）：
 *   +0    magic[4]  "ANI2"
 *   +4    version   u32 (=2)
 *   +8    num       u8  (10)
 *   +9    halves    u8  (2)
 *   +10   frames    u16 (12)
 *   +12   w         u16 (40)
 *   +14   h         u16 (42)
 *   +16   crc32     u32（覆盖 [0..16)）
 *   +20   planes    u8  (2：ink + mask)
 *   +24..43  shade[0][20]  u8：half0（阶段B 展开）每帧明暗系数
 *   +44..63  shade[1][20]  u8：half1（阶段A 折叠）每帧明暗系数
 *   +64..255  reserved(0)
 *   +256.. 帧数据：digits(10) x halves(2) x frames(12) x (ink 210B + mask 210B)
 *
 * 双位面模型：
 *   mask=1 区域被折叠/展开的卡片纸覆盖：ink=1 画数字色，ink=0 画背景色变暗(shade)
 *   mask=0 区域是卡片纸后面的背景数字：bg_digit 该半幅 + 原 UI 背景
 * =========================================================================================================== */

/* 动画元数据（UI_Anim_Init 从数据头解析缓存） */
static u32 g_anim_frames = 0;    /* 每半幅帧数 */
static u32 g_anim_w      = 0;    /* 半幅宽（40） */
static u32 g_anim_h      = 0;    /* 半幅高（42） */
static u32 g_anim_num    = 0;    /* 数字个数（10） */
static u32 g_anim_half   = 0;    /* 半幅数（2） */
static u32 g_anim_planes = 0;    /* 位面数（2：ink+mask） */
static u8  g_anim_shade[2][20];  /* [half_idx][frame] 明暗系数（0..255） */
static u8  g_bgs_ok = 0;   /* 背景切片可用（UI_Anim_Init 校验 BGSC magic） */

/* 卡片左边界 x -> 背景切片索引（与 build_flash.py BG_CARD_X 一致） */
static int bg_slice_card(int card_x)
{
  static const int bx[6] = {13, 59, 117, 163, 221, 267};
  for (int k = 0; k < 6; k++)
    if (bx[k] == card_x) return k;
  return 0;
}

/* ------------------------------------------------------------------
 * anim_frame_off：计算某帧在帧数据区内的字节偏移（不含 256B 头）。
 * 参数: digit=数字 0..9；half_idx=0(下半)/1(上半)；frame_idx=帧号。
 * 返回: 帧数据偏移 = ((digit*halves + half)*frames + frame) * 每帧字节数。
 *       每帧字节数 per = h * ceil(w/8) * planes（ink+mask 双位面）。
 * ------------------------------------------------------------------ */
static u32 anim_frame_off(int digit, int half_idx, int frame_idx)
{
  u32 row = (g_anim_w + 7u) / 8u;
  u32 per = g_anim_h * row * g_anim_planes;   /* 双位面：ink + mask */
  return ((u32)digit * g_anim_half * g_anim_frames +
          (u32)half_idx * g_anim_frames +
          (u32)frame_idx) * per;
}

/* ------------------------------------------------------------------
 * UI_Anim_Init：读取 ANI2 数据头并校验，缓存动画元数据与亮度表。
 * 返回: 0=成功；-1=magic 错误；-2=CRC 错误（失败则界面退化为纯静态）。
 * ------------------------------------------------------------------ */
int UI_Anim_Init(void)
{
  u8 hdr[64];                            /* 2026-09-05 内存优化：ANIM 头只用到 0..63（亮度表 @24..63），原 256B 栈浪费 */
  Flash_Read(UI_ANIM_OFF, hdr, 64);

  /* 背景切片区自检（旧镜像无此区则回退逐行读背景） */
  {
    u8 bh[16];
    Flash_Read(UI_BGSLICE_OFF, bh, 16);
    g_bgs_ok = (bh[0]=='B' && bh[1]=='G' && bh[2]=='S' && bh[3]=='C') ? 1u : 0u;
    printf("[ANIM] bg slice: %s\r\n", g_bgs_ok ? "OK (3360B x12)" : "NONE -> per-row");
  }
  printf("[ANIM] flash@%X: %c%c%c%c\n", UI_ANIM_OFF,
         hdr[0], hdr[1], hdr[2], hdr[3]);
  if (!(hdr[0]=='A' && hdr[1]=='N' && hdr[2]=='I' && hdr[3]=='2'))
  {
    printf("[ANIM] bad magic -> static\n");
    return -1;
  }
  u32 crc      = (u32)hdr[16] | ((u32)hdr[17] << 8) | ((u32)hdr[18] << 16) | ((u32)hdr[19] << 24);
  u32 crc_calc = ui_crc32(hdr, 16);
  if (crc != crc_calc)
  {
    printf("[ANIM] crc fail: stored=%08X calc=%08X -> static\n", crc, crc_calc);
    return -2;
  }
  g_anim_num    = hdr[8];
  g_anim_half   = hdr[9];
  g_anim_frames = (u32)hdr[10] | ((u32)hdr[11] << 8);
  g_anim_w      = (u32)hdr[12] | ((u32)hdr[13] << 8);
  g_anim_h      = (u32)hdr[14] | ((u32)hdr[15] << 8);
  g_anim_planes = hdr[20];                       /* 应为 2 */
  if (g_anim_planes == 0) g_anim_planes = 2;
  memcpy(g_anim_shade[0], hdr + 24, 20);         /* half0 亮度表 @24 */
  memcpy(g_anim_shade[1], hdr + 44, 20);         /* half1 亮度表 @44 */
  printf("[ANIM] ok: digits=%u halves=%u frames=%u w=%u h=%u planes=%u -> flip\n",
         g_anim_num, g_anim_half, g_anim_frames, g_anim_w, g_anim_h, g_anim_planes);
  return 0;
}

/* 动画元数据 getter（供 calendar.c 查询） */
int  UI_Anim_GetFrames(void) { return (int)g_anim_frames; }
int  UI_Anim_GetHalfW(void)   { return (int)g_anim_w; }
int  UI_Anim_GetHalfH(void)   { return (int)g_anim_h; }
int  UI_Anim_Ready(void)      { return (g_anim_w > 0u && g_anim_h > 0u && g_anim_frames > 0u) ? 1 : 0; }

/* 帧数据缓冲：ink 210B + mask 210B = 420B（一次性读两个平面；2026-09-05 由 520 缩到 432，省 88B） */
static u8  g_anim_buf[432];
/* half 合成缓冲：分块后只容纳【单块 20 列 x 42 行】（RGB565 1680B），翻页动画把 40 列
   切成 2 块分别 读切片->合成->连续 RAMWR 写屏（2026-09-05 由 3360B 缩，省 1680B RAM；
   RAMWR 窗口保持整卡片，WriteData 分两段，SPI 连续写地址自动延续）。
   g_blit 数组已上移文件顶部（2026-10-12：动画停用，复用为切屏 DMA 源，省 1280B RAM）。 */

/* ------------------------------------------------------------------
 * ui_shade_color：按明暗系数缩放一个 RGB565 颜色。
 * 参数: c=原色（RGB565）；shade=0..256（256=不变）。
 * 返回: 缩放后的 RGB565。各通道先乘 shade 再 >>8（用 256 代替 /255）。
 * ------------------------------------------------------------------ */
static u16 ui_shade_color(u16 c, u16 shade)
{
  u32 r = ((c >> 11) & 0x1F) * shade >> 8;
  u32 g = ((c >> 5)  & 0x3F) * shade >> 8;
  u32 b = (c        & 0x1F) * shade >> 8;
  return (u16)((r << 11) | (g << 5) | b);
}

/* ==================================================================
 * UI_Anim_DrawFrame：绘制翻页动画的【一个半幅帧】（显示驱动最核心的合成函数）。
 *
 * 参数:
 *   card_x   : 卡片左边界逻辑 x（决定 RASET 窗口）。
 *   half_y0  : 该半幅的 GRAM 起始行（上半 183 / 下半 141）。
 *   digit    : 本帧使用的数字（阶段A=旧数字折叠，阶段B=新数字展开）。
 *   half_idx : 0=屏幕下半、1=屏幕上半（决定背景字模取哪 42 行、亮度表用哪个）。
 *   frame_idx: 帧号（0..帧数-1，决定读哪帧数据 + 亮度表取值）。
 *   bg_digit : 背景数字（阶段A=新数字，阶段B=旧数字；露出卡片后面的数字）。
 *   color    : 数字墨色（RGB565）。
 *
 * 流程:
 *   1) 一次性读该帧 ink+mask 两个位面（420B）。
 *   2) 读背景数字字模（bg_digit 的大号字模 40x84），按 half_idx 取相应 42 行。
 *   3) 取本帧亮度 shade（0..256）。
 *   4) 逐行读 UI 背景（40px 段），逐像素合成进 g_blit：
 *        mask=1 : 卡片纸——ink=1 画数字色(按 shade 变暗)，
 *                 ink=0 画背景像素(按 shade 变暗)；
 *        mask=0 : 背景——bg 字模墨点=1 画数字色，否则画背景像素。
 *   5) 整半幅一次 RAMWR 写屏（CASET=half_y0 起，RASET=card_x 起）。
 * ------------------------------------------------------------------ */
void UI_Anim_DrawFrame(int card_x, int half_y0, int digit, int half_idx, int frame_idx,
                       int bg_digit, u16 color)
{
  if (digit < 0 || digit >= (int)g_anim_num) return;
  if (bg_digit < 0 || bg_digit >= (int)g_anim_num) bg_digit = digit;
  if (half_idx < 0 || half_idx >= (int)g_anim_half) return;
  if (frame_idx < 0 || frame_idx >= (int)g_anim_frames) return;
  if (g_anim_w == 0 || g_anim_h == 0) return;

  u32 w = g_anim_w, h = g_anim_h;
  u32 row_bytes = (w + 7u) / 8u;
  u32 per       = h * row_bytes;                 /* 单平面大小（210B） */
  u32 off       = UI_ANIM_OFF + 256u + anim_frame_off(digit, half_idx, frame_idx);

  /* 1) 一次性读 ink + mask 两个平面（420B），取代原来逐像素 1680 次 Flash_Read */
  vu32 t_flash = bftm0_ct;
  Flash_Read(off, g_anim_buf, per * g_anim_planes);
  const u8 *ink  = g_anim_buf;                   /* 墨点位面 */
  const u8 *mask = g_anim_buf + per;             /* 卡片纸覆盖位面 */
  vu32 dt_flash = (u32)bftm0_ct - (u32)t_flash;

  /* 2) 读背景数字该 half 的 1bpp 位图（40x42）一次读出 */
  const u8 *bg = ui_get_glyph((u32)'0' + (u32)bg_digit);
  if (!bg) bg = g_blank;
  u32 bg_rb = (g_cur_w + 7u) / 8u;
  u32 bg_y0 = (half_idx == 1) ? 42u : 0u;        /* half1(屏幕上半)取 42..83 行，half0(屏幕下半)取 0..41 行 */

  /* 3) 本帧亮度（0..255 -> 1..256；shade=256 表示不调暗） */
  u16 shade = 256u;
  if (half_idx < 2 && frame_idx < 20)
    shade = (u16)g_anim_shade[half_idx][frame_idx] + 1u;

  /* 4)+5) 读背景并合成，分块（每块 20 列）连续 RAMWR 写屏（2026-09-05 内存优化：
     g_blit 缩为单块缓冲（20x42x2=1680B），把 40 列卡片切成 2 块，每块
     读切片->原地交换->合成->WriteData 一段；RAMWR 窗口保持整卡片，SPI 连续
     写地址自动延续（列优先切片与 LCD 地址流一致），总阻塞时间与整块相当。
     优先用【背景切片】：一次 Flash_Read(1680B) 取代原 42 次逐行事务；
     切片区带 BGSC magic，UI_Anim_Init 校验通过才启用，失败回退逐行读（安全）。 */
  vu32 t_synth = bftm0_ct;
  vu32 t_bgread = bftm0_ct;                      /* [计时] 背景读取 */
  const u32 BLOCK_COLS  = 20;                    /* 每块列数（40 列 -> 2 块） */
  const u32 BLOCK_BYTES = BLOCK_COLS * h * 2u;   /* 单块字节（1680B） */

  vu32 t_spi = bftm0_ct;
  SPI_LCD_Select();
  SPI_LCD_WriteCmd(ST7789_CASET);
  { u8 d[4] = {0, (u8)half_y0, 0, (u8)(half_y0 + (int)h - 1)}; SPI_LCD_WriteData(d, 4); }
  SPI_LCD_WriteCmd(ST7789_RASET);
  { u8 d[4] = {HI8((u16)card_x), LO8((u16)card_x),
               HI8((u16)(card_x + (int)w - 1)), LO8((u16)(card_x + (int)w - 1))};
    SPI_LCD_WriteData(d, 4); }
  SPI_LCD_WriteCmd(ST7789_RAMWR);

  for (u32 blk = 0; blk < (w + BLOCK_COLS - 1u) / BLOCK_COLS; blk++)
  {
    u32 bx0    = blk * BLOCK_COLS;                        /* 块起始列 */
    u32 bxn    = (bx0 + BLOCK_COLS > w) ? (w - bx0) : BLOCK_COLS;  /* 块列数（末块可能不满） */
    u32 bbytes = bxn * h * 2u;                            /* 块字节数 */

    if (g_bgs_ok)
    {
      /* 切片列优先，块 blk = 连续 BLOCK_BYTES（切片偏移 + blk*BLOCK_BYTES） */
      u32 bgoff = UI_BGSLICE_OFF + 256u + ((u32)bg_slice_card(card_x) * 2u + (u32)half_idx) * w * h * 2u
                  + blk * BLOCK_BYTES;
      Flash_Read(bgoff, g_blit, bbytes);
      for (u32 i = 0; i < bbytes; i += 2)                 /* 小端->大端 原地 */
      {
        u8 t = g_blit[i]; g_blit[i] = g_blit[i + 1]; g_blit[i + 1] = t;
      }
      for (u32 yy = 0; yy < h; yy++)
      {
        u32 src_row = yy * row_bytes;                     /* 当前帧位图的行偏移 */
        u32 bg_row  = (bg_y0 + yy) * bg_rb;               /* 背景字模对应行偏移 */
        u32 pos = yy * 2u;                                /* 块内 GRAM 索引：列x外、行yy内 */
        for (u32 xx = 0; xx < bxn; xx++)
        {
          u32 x   = bx0 + xx;
          u32 idx = src_row + (x >> 3);
          u8  mb  = (mask[idx] >> (7 - (x & 7))) & 1;
          u16 bpx = (u16)((u16)g_blit[pos] << 8) | g_blit[pos + 1];  /* 先读旧背景再写，安全 */
          u16 pix;
          if (mb)                                          /* 卡片纸内 */
          {
            u8 ib = (ink[idx] >> (7 - (x & 7))) & 1;
            pix = (ib) ? ((shade == 256u) ? color : ui_shade_color(color, shade))
                        : ((shade == 256u) ? bpx : ui_shade_color(bpx, shade));
          }
          else                                             /* 卡片外：露出背景数字或背景 */
          {
            u8 bb = (bg[bg_row + (x >> 3)] >> (7 - (x & 7))) & 1;
            pix = bb ? color : bpx;
          }
          g_blit[pos]     = HI8(pix);
          g_blit[pos + 1] = LO8(pix);
          pos += h * 2u;                                   /* 块内下一列：GRAM 索引 +h 行 */
        }
      }
    }
    else
    {
      /* 回退：逐行读背景（无切片或旧镜像），逻辑同历史实现 */
      for (u32 yy = 0; yy < h; yy++)
      {
        u16 line16[40];
        u32 ly = (u32)(half_y0 + (int)yy);
        if (Flash_DualOK())
          Flash_ReadDual(g_ui_base + ly * 320u * 2u + (u32)card_x * 2u, g_line, w * 2u);
        else
          Flash_Read(g_ui_base + ly * 320u * 2u + (u32)card_x * 2u, g_line, w * 2u);
        for (u32 i = 0; i < w; i++)                        /* 小端->大端 直接转 u16 */
        {
          u8 t = g_line[i * 2];
          g_line[i * 2] = g_line[i * 2 + 1];
          g_line[i * 2 + 1] = t;
          line16[i] = (u16)((u16)g_line[i * 2] << 8) | g_line[i * 2 + 1];
        }
        u32 src_row = yy * row_bytes;
        u32 bg_row  = (bg_y0 + yy) * bg_rb;
        u32 pos = yy * 2u;
        for (u32 xx = 0; xx < bxn; xx++)
        {
          u32 x   = bx0 + xx;
          u32 idx = src_row + (x >> 3);
          u8  mb  = (mask[idx] >> (7 - (x & 7))) & 1;
          u16 pix;
          if (mb)                                          /* 卡片纸内 */
          {
            u8 ib = (ink[idx] >> (7 - (x & 7))) & 1;
            if (ib)                                        /* 墨点：数字色 */
              pix = (shade == 256u) ? color : ui_shade_color(color, shade);
            else                                           /* 纸底：背景像素变暗 */
              pix = (shade == 256u) ? line16[x] : ui_shade_color(line16[x], shade);
          }
          else                                             /* 卡片外 */
          {
            u8 bb = (bg[bg_row + (x >> 3)] >> (7 - (x & 7))) & 1;
            pix = bb ? color : line16[x];
          }
          g_blit[pos]     = HI8(pix);
          g_blit[pos + 1] = LO8(pix);
          pos += h * 2u;
        }
      }
    }
    SPI_LCD_WriteData(g_blit, bbytes);                     /* 块内连续写屏（RAMWR 保持，GRAM 地址延续） */
  }
  SPI_LCD_Deselect();

  vu32 dt_bgread = (u32)bftm0_ct - (u32)t_bgread;  /* 背景读取耗时 */
  vu32 dt_synth = (u32)bftm0_ct - (u32)t_synth;    /* 含 bgread + 合成 */
  vu32 dt_spi = (u32)bftm0_ct - (u32)t_spi;

  /* [调试] 分段计时已注释（语音开发期），保留 dt_* 计算避免改结构 */
  (void)dt_flash; (void)dt_bgread; (void)dt_synth; (void)dt_spi;
  /* {
    static u32 n = 0;
    if ((n++ & 0x1F) == 0)
      printf("[T] flash=%u bgread=%u synth=%u spi=%u\r\n",
             (unsigned)dt_flash, (unsigned)dt_bgread,
             (unsigned)(dt_synth - dt_bgread), (unsigned)dt_spi);
  } */
}

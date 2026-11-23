#ifndef __UI_H
#define __UI_H

#include "ht32.h"
#include "Display.h"

/* ============================================================
 * Flash UI 层：从 C25VQ16E 读取 ui_320x240.bin 背景图并上传到 ST7789，
 * 并用 Flash 内的变尺寸 1bpp 字库（font.bin）按 flash_interface.md
 * 坐标叠加动态文字（时钟/日期/温湿度）。字模按各显示尺寸单独渲染，绘制时 1:1 不缩放。
 *
 * 本文件随 Display.c 的 #include 聚合一起编译，复用已验证的
 * spi_lcd / Display / flash_read 底层，不引入新的硬件配置。
 * ============================================================ */

/* RGB565 颜色宏（与 ui_spec.md 一致） */
#define UI_RGB(r,g,b)  ( (u16)( ( ((u16)(r)>>3) << 11 ) | ( ((u16)(g)>>2) << 5 ) | ( (u16)(b)>>3 ) ) )

#define UI_C_DIGIT   0xFFDFu                  /* 时钟大数字 白 #F8FAFC（概念B 主卡深底白字，ui-design-system text-primary） */
#define UI_C_COLON   UI_RGB(0xe0,0xe0,0xe0)   /* 冒号 #e0e0e0   */
#define UI_C_WHITE   0xFFFF                    /* 白字           */
#define UI_C_BLACK   0x0000                    /* 黑字           */
#define UI_C_PANEL   0x1906u                   /* 面板 #1B2336（人物/MSCOT/CANI 动画透明像素填充色，ui-design-system §6.7，2026-08-26 由纯黑改） */

/* 翻页动画区（位于 Reserved 0x110000，详见 build_flash.py build_anim） */
#define UI_ANIM_OFF      0x110000u
/* 动画卡片背景切片（build_flash.py 预提取的连续背景区，一次读 3360B） */
#define UI_BGSLICE_OFF   0x130000u
/* 语音助手小女孩形象（MSCT 容器：w/h/像素/掩码，build_flash.py build_mascot；尺寸动态，当前 69x69） */
#define UI_MASCOT_OFF    0x140000u
/* 语音助手 状态动画（CANI 容器：build_flash.py build_canim / gen_canim.py；帧图 default_girl_1..48） */
#define UI_CANI_OFF      0x150000u
/* 计时沙漏真彩图标（ICO1 容器 @0x1E0000：RGB565 像素+1bpp 掩码，build_flash.py build_icon；
   用户 hourglass-16x16.png 多色调原样，2026-09-05 由 1bpp glyph 0xE302 改；
   ⚠️ 须在 CANI 之后（当前 CANI 44 帧末址 ~0x1C6660 < 0x1E0000） */
#define UI_ICON_OFF      0x1E0000u
/* 坑#85 闹钟屏动态素材（ALM2 容器 @0x1F0000：从效果图截取，整块不透明 RGB565，
   item0=开关 ON 控件 68x33、item1=星期未选中灰白瓶身 30x30，build_flash.py build_alm2；
   坑#91 2026-10-30 增 item3=开关 OFF 控件 68x33（空槽样式，客户新图 320x239-(3).png 截取）；
   ⚠️ 须在 ICON 之后（ALM2 ~4KB << 0x200000 整片） */
#define UI_ALM2_OFF      0x1F0000u
/* 72x72 人物区（与 build_all.py ZONE_* 一致）
   坑#84 新主屏（AI 奶油风）：语音助手人物移到右下空卡 (220,128)；倒计时/闹钟屏仍 219,156。 */
#define UI_ZONE_X        220
#define UI_ZONE_Y        128
#define UI_ZONE_W        72
#define UI_ZONE_H        72
#define UI_CANI_MAX_STATE 8u

/* ==================== 多屏（2026-08-27 倒计时/闹钟） ====================
 * 背景区 UI_OFF.. 每屏 150KB（320x240 RGB565），屏偏移 = UI_OFF + 屏号×153600。
 * 屏0=主界面 / 屏1=倒计时 / 屏2=闹钟 / 屏3 预留（与 build_flash.py UI_SCREEN_FILES 一致）。 */
#define UI_SCREEN_MAIN    0u
#define UI_SCREEN_CNT     1u
#define UI_SCREEN_ALARM   2u
#define UI_SCREEN_COUNT   3u
#define UI_SCREEN_BYTES   153600u   /* 每屏背景字节（320x240x2） */

void UI_UploadBackground(int screen);  /* 上传指定屏背景到 ST7789 GRAM（小端->高字节先，软件转置横屏） */
int  UI_SetScreen(int screen);         /* 切屏：设当前屏（背景基址/人物区随屏）+ 上传背景；0=ok,-1=非法屏 */
void UI_SetDispVisible(int vis);       /* 显示可见性：1=未关屏；0=已关屏（0xB5/长按）。UI_SetScreen 据此决定切屏是否 DISPOFF 包上传（2026-10-12） */
int  UI_GetScreen(void);               /* 当前屏号（UI_SCREEN_*） */
extern volatile u16 g_ui_bg_ver;       /* 背景重传版本号：UI_SetScreen 每次自增；screen.c 据此在重传背景后强制全绘动态元素（2026-08-28） */
int  UI_CharAnim_Redraw(void);         /* 按当前屏人物区重画当前帧（切屏后消除旧位置残留） */

/* 用户配置容器（CONF @config_offset，对应 build_flash.py build_config 的 16B payload） */
typedef struct {
  u8 brightness;   /* 亮度 %（默认 80） */
  u8 timezone;     /* 时区 UTC+n（默认 8） */
  u8 alarm_on;     /* 闹钟开关（默认 0） */
  u8 alarm_hour;   /* 闹钟时（默认 7） */
  u8 alarm_min;    /* 闹钟分（默认 0） */
  u8 bt_paired;    /* 蓝牙已配对（默认 1） */
  u8 wifi_on;      /* WiFi 开关（默认 0） */
  u8 reserved[9];  /* 保留 */
} ui_cfg_t;

/* ==================== UI_Show 统一显示入口 ====================
 * 2026-08 架构重构：大部分显示更新收敛为一个函数
 *   UI_Show(x, y, s, slot, color) —— 只输入 起点坐标 + 内容 + 档位宏 + 颜色。
 * 坐标约定：x、y 都是【设计坐标】（顶原点，y=文字顶部设计 y，0 在顶）。
 *   UI_Show 内部按档位 h 逐行换算 GRAM 列 = 239 - 设计行（设备 GRAM 垂直镜像），
 *   调用方无需再算 YF；x 无镜像，直接为 GRAM 行。该约定与背景/MSCOT/CANI 一致。
 * 档位宏（slot）查 g_slot 表得到该元素的 {字模段, 字高, 统一格宽, 清除宽, 对齐}：
 *   - 数字统一格宽（UI_SLOT_DIGIT/SEC/DATE/TH 为 12px），每个数字长宽区域一致；
 *   - 整块合成直接覆盖（clear_w × h），旧内容被新内容盖掉，无残留、背景只读一次。
 */
#define UI_ALIGN_LEFT    0u
#define UI_ALIGN_CENTER  1u

#define UI_DATE_DIGIT_W  12u   /* 日期/秒数字统一格宽（px，与 build_flash.py UI_DATE_DIGIT_W 一致） */
#define UI_DATE_DOT_W     4u   /* 日期间隔点宽（超小字号，0xE202） */
#define UI_TH_DIGIT_W    12u   /* 温湿度数字格宽（px，固定 12x30） */

/* UI_Show 档位枚举（元素 -> 查 g_slot 表） */
enum {
  UI_SLOT_SEC,      /* 秒：DG_SMALL 22高 格12 清31 居中 */
  UI_SLOT_DATE,     /* 日期整串：DG_SMALL 22高 格12 清141 居中 */
  UI_SLOT_DATE2,    /* 坑#84 新主屏日期串：22高 格12 清110 居中（右上卡，2026-09-05） */
  UI_SLOT_DIGIT,    /* 日期单数字：DG_SMALL 22高 格12 清12 左（逐位） */
  UI_SLOT_DOT,      /* 日期点号：超小 22高 格4 清4 左（0xE202） */
  UI_SLOT_TH,       /* 温湿度：DG_TH 30高 格12 清69 居中 */
  UI_SLOT_TH2,      /* 坑#84 新主屏温湿度数值：22高 格12 清50 居中（左下卡，2026-09-05） */
  UI_SLOT_WEEK,     /* 星期汉字：18x22 清54 居中 */
  UI_SLOT_LUNAR,    /* 农历汉字：18x22 清72 居中 */
  UI_SLOT_CLOCK,    /* 时钟大数字：40x84 清40 左 */
  UI_SLOT_CHAR,     /* 单汉字（闹钟星期chip）：18x22 清18 左（2026-08-27） */
  UI_SLOT_EN,       /* 坑#88 按钮1 英文小字（8x8，ASCII 码段）：清8 左（2026-10-11） */
  UI_SLOT_COUNT
};

void UI_Show(int x, int y, const char *s, u8 slot, u16 color); /* 统一显示入口：查档位→对齐→整块合成覆盖；x,y=设计坐标（y=文字顶部） */
void UI_DrawTextInk(int x, int y, const char *s, u8 slot, u16 color); /* 仅叠墨点（不清背景/不读 Flash）：chip 文字配合 Display_FillRect 填底用（2026-08-27） */

/* ==================== API ==================== */
/* Optional block in [0x100, 0x1000). Returns 0=valid, 1=absent, -1=invalid. */
int UI_ReadLayout(u32 offset, const char magic[4], void *payload, u16 size);
int  UI_LoadHeader(void);   /* 读分区头并校验 magic+crc32，定位 ui/font/config 基址。0=成功 */
int  UI_LoadConfig(void);   /* 读 CONF 配置容器并校验 CRC。0=成功；-1=magic；-2=CRC */
const ui_cfg_t *UI_GetConfig(void); /* 访问已加载配置（未加载/失败为全 0） */
void UI_SetAlarm(u8 on, u8 hour, u8 min); /* 写运行时闹钟配置（仅内存，不写 Flash；响铃状态机接入后使用） */
int  UI_CacheFont(void);    /* 缓存字库索引表（按需二分查找） */
void UI_DrawGlyphRect(int rx, int ry, int rw, int rh, u32 code, u16 color); /* 在逻辑矩形内画单字（还原背景+叠墨点） */
void UI_DrawText(const char *str, int x, int y, int cell_w, int cell_h, u16 color); /* UTF-8 字符串（每字 cell_w×cell_h） */
void UI_DrawGlyphs(const u32 *codes, int n, int x, int y, int cell_w, int cell_h, u16 color); /* 直接传 Unicode 码点数组（避免源文件中文字面量） */
void UI_DrawVarDigits(const char *s, int x, int y, u16 color, u32 base); /* 数字串按变体 code 段绘制(1:1，base 区分尺寸) */
int  UI_GlyphWidth(u32 code); /* 查字形实际宽度 px（变宽字形按实际笔画宽；找不到返回 0） */
void UI_DrawMascot(int rx, int ry); /* 语音助手形象：GRAM 坐标 rx,ry，尺寸动态（容器头），透明像素填面板色 UI_C_PANEL */
void UI_DrawMascotCentered(void);   /* 读 MSCT 尺寸自动按 72x72 区定位绘制（与 CANI 动画同区对齐，尺寸无关） */
void UI_DrawHourglass(int x, int y); /* 计时沙漏真彩图标：设计坐标 x,y 左上角，16x16 RGB565+掩码流式绘制（ICO1 @0x160000） */
void UI_DrawAlm2(int item, int x, int y); /* 坑#85 闹钟屏动态素材：设计坐标 x,y 左上角，item0=开关 ON 68x33 / item1=灰白瓶 30x30 /
                                             item2=倒计时暂停按钮 69x42 / item3=开关 OFF（空槽）68x33（坑#91），
                                             整块不透明流式覆盖（ALM2 @0x1F0000） */

/* ---- 语音助手 状态动画（CANI） ----
   帧图放 HT\7\形象图\default_girl_N.png（1..52），gen_canim.py 打包。
   状态（0 基，见 UI_CANI_STATE_*）：
     IDLE/WAKE/ALARM 是粘性状态（常驻循环 + 过渡）；
     OK 是一次性响应动画（4 帧，播完自动回原状态，UI_CharAnim_PlayOnce）。
   粘性状态每状态 16 帧（容器状态表 count=16）：
     [0..8)   正常循环帧（8 帧，状态内循环播放）
     [8..12)  变化到唤醒(WAKE) 帧（4 帧）
     [12..16) 变化到闹铃(ALARM) 帧（4 帧）
   状态切换效果：当前状态 循环前 4 帧 -> 变化 4 帧 -> 目标状态循环。
   帧切换 0.125s/帧（8 次/秒，UI_CANI_MS_PER_FRAME）。 */
#define UI_CANI_STATE_IDLE   0u  /* 待机 idle   帧 1-16（gen_canim.py 打包） */
#define UI_CANI_STATE_WAKE   1u  /* 唤醒 wake   帧 17-32 */
#define UI_CANI_STATE_ALARM  2u  /* 闹铃 alarm  帧 33-48（闹钟响铃时切换，2026-09-04 接线） */
#define UI_CANI_STATE_OK     3u  /* 修改成功响应  帧 49-52（一次性，PlayOnce 播完自动回原状态） */
#define UI_CANI_STATE_COUNT  4u  /* 当前状态总数（≤ UI_CANI_MAX_STATE） */

#define UI_CANI_FRAME_PER_STATE 16u  /* 每状态帧数（容器 state_count 应=16） */
#define UI_CANI_LOOP_CNT        8u   /* 每状态正常循环帧（前 8） */
#define UI_CANI_TRANS_PRE       4u   /* 过渡先重播当前状态循环前 4 帧 */
#define UI_CANI_TRANS_CNT       4u   /* 变化帧数 */
#define UI_CANI_MS_PER_FRAME    125u /* 8 次刷新/秒（0.125s/帧）；单帧绘制 ~30ms 实测 */

/* 待机循环"停留帧"（用户需求）：idle 每次循环在指定帧停 N 个帧间隔再继续，
   形成"两次循环间停一拍"的呼吸感。帧号为状态内 0 基（6 = 第 7 帧）。 */
#define UI_CANI_IDLE_HOLD_FRAME     6u  /* 停留帧号（0 基） */
#define UI_CANI_IDLE_HOLD_INTERVALS 8u  /* 停留帧间隔数（8×125ms≈1s） */
typedef struct {
  u16 w, h;                        /* 帧尺寸（容器头读，≤80） */
  u8  nstates;                     /* 状态数 */
  u8  nframes;                     /* 总帧数 */
  u8  ok;                          /* 1=容器有效且有帧（可播） */
  u8  cur_state;                   /* 当前状态（UI_CANI_STATE_*） */
  u16 cur_frame;                   /* 当前状态内帧序号（循环段 0..LOOP_CNT-1） */
  u8  trans;                       /* 1=过渡中（SetState 触发） */
  u8  trans_target;                /* 过渡目标状态 */
  u8  one_shot;                    /* 1=一次性响应动画播放中（PlayOnce，播完回 return_state） */
  u8  return_state;                /* 一次性动画播完的返回状态 */
  u8  trans_phase;                 /* 过渡阶段：0=重播循环前 4 帧，1=播变化帧 */
  u8  trans_step;                  /* 过渡步进（0..3） */
  u16 state_start[UI_CANI_MAX_STATE];  /* 每状态起始全局帧 */
  u16 state_count[UI_CANI_MAX_STATE];  /* 每状态帧数（=16） */
  u32 last_tick;                   /* 上次换帧 bftm0_ct */
} ui_cani_t;
extern ui_cani_t g_cani;
int  UI_CharAnim_Init(void);       /* 读 CANI 头+状态表；0=可播，-1=无容器/空/非法 */
int  UI_CharAnim_Ready(void);      /* 1=可播（ok 且有帧） */
void UI_CharAnim_Enable(int en);   /* 开启动画播放（1=播放；0=静态帧） */
void UI_CharAnim_DrawEnable(int en); /* 2026-11-17 新 UI：1=画旧人物帧（默认）；0=只跑帧机不写屏
                                        （新 UI 机器人改用 SPR1 精灵，cur_state/cur_frame 仍由帧机推进） */
int  UI_CharAnim_SetState(int st); /* 切状态：目标 WAKE/ALARM 走过渡（前4帧+变化4帧），目标 IDLE/同状态直切 */
int  UI_CharAnim_PlayOnce(int st); /* 一次性响应动画：播完 st 状态一遍后自动回原状态（如 OK 短动画） */
void UI_CharAnim_Tick(void);       /* 0.125s 换帧并重绘（内部 g_cani.ok 与 Enable 门控） */

/* 翻页动画 API（ANI2 双位面折叠模型）：
   half_idx: 0=物理下半（阶段 B 新数字展开），1=物理上半（阶段 A 旧数字折叠）。
   digit   : 动画层的数字（阶段 A 为旧数字，阶段 B 为新数字）。
   bg_digit: 背景层的数字（阶段 A 为新数字，阶段 B 为旧数字）。
   frame_idx: 0..AnimFrames-1。 */
int  UI_Anim_Init(void);                                          /* 读 ANIM 头校验 + 缓存参数；0=成功 */
int  UI_Anim_Ready(void);                                         /* 动画区可用？1=可播放（w/h>0 且已初始化） */
int  UI_Anim_GetFrames(void);                                      /* 每半翻页帧数（20） */
int  UI_Anim_GetHalfW(void);                                      /* 半幅宽（40） */
int  UI_Anim_GetHalfH(void);                                      /* 半幅高（42） */
void UI_Anim_DrawFrame(int card_x, int half_y0, int digit, int half_idx, int frame_idx,
                       int bg_digit, u16 color);

/* ==================== 像素 UI（08 号设计 2026-11-17 / 国风 2026-11-19 两版共用） ====================
 * Flash 资源（build_px_image.py 产出 flash_image_px.bin）：
 *   0x001000 BG0 主屏 | 0x026800 BG1 倒计时 | 0x04C000 BG2 闹钟（各 150KB，row0=设计底部）
 *   0x072000 FONT（变宽，4~5 档字号，code = base + Unicode；位深 1bpp 或 4bpp alpha，见下）
 *   0x110000 SPR1 精灵容器
 * SPR1: [0..4)'SPR1' [4..8)ver | [8..12)n | 索引从 16 起，每条 16B
 *       {w:u16,h:u16,x:i16,y:i16,px_off:u32,mask_off:u32}
 *       像素 RGB565 行优先、row0=设计底部；掩码 1bpp 同行序。
 *       x/y = 该精灵在屏幕上的绝对落点（国风那版的 GIF 自带位置），
 *       需要「按精灵自带坐标贴」时用 UI_DrawSpriteAuto()。 */
#define UI_SPR_OFF       0x110000u
#define UI_PX_BG0        0x001000u
#define UI_PX_BG1        0x026800u
#define UI_PX_BG2        0x04C000u

/* 字号档位（字库 code 基址；每档 0x10000 一段，实际字号由素材清单的 font_sizes 决定）
 * 国风版（2026-11-19/20）：font_face = 霞鹜文楷 Medium(LXGWWenKai-Medium.ttf)，
 *   font_sizes = 12,13,14,16，font_bits = 4（4bpp alpha 抗锯齿，固件按背景混色）。
 *   0x00000=12px 日期 / 星期 / 农历 / 温湿度 / AI标题 / 倒计时中 / 倒计时气泡
 *   0x10000=13px 主屏气泡
 *   0x20000=14px 闹钟槽号
 *   0x30000=16px 闹钟屏气泡 / 温湿度数值(23°C)
 * 08 号基线：12/13/14/26（思源黑体，1bpp）。换风格时这些 base 要跟 font_sizes 一起改。 */
#define UI_PX_12         0x00000u   /* 日期 / 星期 / 农历 / 温湿度 / AI标题 / 倒计时中 / 倒计时气泡 */
#define UI_PX_13         0x10000u   /* 主屏气泡 */
#define UI_PX_14         0x20000u   /* 闹钟槽号 */
#define UI_PX_16         0x30000u   /* 闹钟屏气泡 */
#define UI_PX_10         UI_PX_12   /* 别名：10px 楷体在 1bpp 下笔画会断，不用；保留宏防编译错 */
#define UI_PX_11         UI_PX_12
#define UI_PX_26         UI_PX_16   /* 别名：当前风格没有 26px 档（仅自检打印用） */

/* 精灵 id（顺序与 build_px_flash.py 一致；换风格时这里要跟着精灵数一起改）
   ── 国风（新ui界面 2026-11-19）：183 个
   0..127   机器人段：0..31 主屏(2状态)、32..79 倒计时(3状态)、80..127 闹钟(3状态)
   128..138 主屏数字 0-9 + 冒号
   139..149 闹钟数字 0-9 + 冒号
   150..160 倒计时数字 0-9 + 冒号
   161..170 秒数字 0-9（10 张，旧版是 161/162 两张固定图）
   171..172 闹钟开关 ON/OFF
   173..178 星期可切换图标 五/六/日 各 开+关（173 五开 174 五关 175 六开 … 177 日开 178 日关）
   179..181 闹钟槽号数字 1/2/3（清单 dig_tag）
   182      「闹钟开关」四字灭相用的纸色贴片（清单 spr_swlbl）
   ── 08 号（旧）：167 个，秒=2 张、开关=2 张、星期钮=2 张（161..166） */
#define UI_SPR_ROBOT(screen, state, frame) \
  ((u32)((((screen) == 0) ? 0u : (((screen) == 1) ? 32u : 80u)) + (u32)((state) * 16u) + (u32)(frame)))
#define UI_SPR_DIGIT(style, d)              (128u + (u32)(style) * 11u + (u32)(d))  /* style 0=主屏 1=闹钟 2=倒计时 */
#define UI_SPR_COLON(style)                 (128u + (u32)(style) * 11u + 10u)
#define UI_SPR_SEC(d)                       (161u + (u32)(d))          /* 秒：0-9 */
#define UI_SPR_ALM_SW(on)                   (171u + ((on) ? 0u : 1u))
#define UI_SPR_ALM_TG(i, on)                (173u + (u32)(i) * 2u + ((on) ? 0u : 1u))  /* i=0 五 / 1 六 / 2 日 */
#define UI_SPR_ALM_TAG(n)                   (179u + (u32)(n) - 1u)     /* 槽号 1/2/3（宋体粗体 10x14） */
#define UI_SPR_ALM_SWLBL                     182u   /* 「闹钟开关」四个字：墨迹形状的纸色贴片 64x20 */

u32 UI_SpriteCount(void);
void UI_DrawSprite(u32 id, int x, int y);                        /* 设计坐标左上角贴精灵（带透明掩码） */
void UI_DrawSpriteAuto(u32 id);                                  /* 按精灵索引里自带的 x,y 贴（GIF 自带位置用） */
void UI_DrawStrPxBounded(int x, int y, const char *s, u32 base, u16 color, int right);
void UI_DrawStrPx(int x, int y, const char *s, u32 base, u16 color); /* 变宽字符串（base=UI_PX_*） */
int  UI_StrPxWidth(const char *s, u32 base);                     /* 字符串像素宽（居中排版用） */
void UI_PxSelfTest(void);                                       /* 串口自检：字库条数 + 关键字形/精灵尺寸 */

#endif


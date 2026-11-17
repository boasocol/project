/************************************************************************************************************
 * @file    screen.c
 * @brief   多屏状态机（2026-08-27）：主界面 / 倒计时(屏1) / 闹钟(屏2) + 倒计时&闹钟业务状态机
 *
 * 切屏模型：UI_SetScreen（ui.c）上传对应屏背景 + 设背景基址/人物区；本模块 Screen_Draw
 * 每帧按当前屏增量绘制动态元素（倒计时/闹钟屏），主屏仍由 calendar.c 负责。
 *
 * 倒计时屏1（MM:SS 大数字 40x84 白 + 状态chip + 进度条 + 开始/暂停/重置/退出）：
 *   槽中心 x=93/133/187/227、冒号 160、块居中 x[73..247]、y=56..140（UI_Show 设计坐标 y=56）。
 *   进度条 [40,182]-[204,190]：状态色按已用时比例填充（2026-09-03 对齐 gen_ui CD_PROG）。
 *   状态chip [238,17]-[300,35]：运行绿/暂停黄/结束红 + 深字（UI_DrawTextInk）。
 *   按钮文字：开始/暂停 [24,200]-[104,222]、重置 [112,200]-[150,222]（2026-09-03 对齐 gen_ui CD_BTN）、
 *   退出 [156,200]-[194,222]（2026-09-05 固件全画：EDGE 底 + 白字；倒计时后台继续跑，到点切回本屏响铃）。
 * 闹钟屏2（HH:MM 大数字 y=78..162 垂直居中 + 开关chip + 星期chip + 强调线）：
 *   槽同上；开关chip [132,17]-[196,35]（已开启绿/已关闭灰）；星期chips y=186..208、x=19+i*26、
 *   选中绿深字/未选面板灰；强调线 [16,170]-[304,171]（响铃红）。
 *
 * 业务状态机：
 *   倒计时：0x84 分钟(d0=1-120，坑#93)/0x88 番茄(25min) 启动切屏1；0x85 暂停/0x86 继续/0x87 结束
 *           （结束停 3s 回主屏）；到 0 → VOICE_TX_RING 响铃 + 人物切闹铃动画(ALARM) + 停 60s 回主屏
 *           （停响人物回待机 IDLE；2026-09-05 响铃中任意键打断停铃回主屏，坑#70）。
 *   正计时（2026-08-28）：0xBA 打开，从 00:00 递增（复用屏1 MM:SS）；0x85/0x86 暂停/继续、
 *           0x87 结束停 3s 回主屏；与倒计时互斥（启动其一先停另一）。
 *   闹钟：0x8A 设(写配置+切屏2)/0x8B 开关/0x8C 循环(0单次 1一~五 2一~六 3一~日)/0x8D 铃音/
 *           0xB9 查看(仅切屏2+回 OK)；到点切屏2 + VOICE_TX_RING + 人物切换闹铃动画(ALARM)；
 *           0xB1 别响/0x8B 关/60s 自动停回主屏（任何停响路径人物回待机 IDLE）；0xB0 贪睡 = 停响 + 闹钟时间 +10min。
 *   其他（2026-08-28）：0xB4/0xB5 显示开关（DISPOFF/ON + 背光）；0xB8 整点报时开关（内存，整点播 0x07）。
 *   按键事件分发（2026-09-05 重构）：Driver(key.c) 只出 KeyEvent{键号,短/长按} 统一事件流，
 *           本模块 Screen_HandleKey 按「事件类型->键号->当前状态」集中分发功能：
 *           KEY2 长按=关屏/深休眠（调 Key_ScreenOff）；KEY2 短按=进设置/下一项/完成退出；
 *           KEY1 减 / KEY3 加（仅设置模式）；项循环 时->分->秒->年->月->日->完成；
 *           时间冻结显示，退出写回 RTC（「完成」再按 / 15s 超时）。
 ************************************************************************************************************/
#include "screen.h"
#include "ui.h"
#include "Display.h"
#include "calendar.h"   /* CalcWeekday / Calendar_SetStaticNext */
#include "BFTM0.h"      /* bftm0_ct（1ms 计数，回主屏延时用） */
#include "key.h"        /* KeyEvent{键号,短/长按}：App 层集中分发（2026-09-05 重构） */

/* ==================== 颜色（概念B 色板 RGB565，与 ui-design-system 令牌一致） ==================== */
#define SC_C_GREEN   UI_RGB(34, 197, 94)    /* 状态绿 #22C55E（运行/选中） */
#define SC_C_YELLOW  UI_RGB(255, 213, 79)   /* 状态黄 #FFD54F（暂停） */
#define SC_C_RED     UI_RGB(239, 68, 68)    /* 危险红 #EF4444（结束/响铃） */
#define SC_C_BLUE    UI_RGB(37, 99, 235)    /* 主蓝 #2563EB（强调线正常） */
#define SC_C_GRAY    UI_RGB(148, 163, 184)  /* 次文字灰 #94A3B8（未选） */
#define SC_C_DARK    UI_RGB(10, 15, 30)     /* chip 深字（绿/黄底） */
#define SC_C_EDGE    UI_RGB(37, 47, 68)     /* 面板高亮底 #252F44（未选chip底） */
#define SC_C_PANEL   UI_RGB(27, 35, 54)     /* 卡片面板底 #1B2336（倒计时/闹钟主卡内背景，2026-09-05 清线用） */
#define SC_C_SCREEN  UI_RGB(15, 23, 42)     /* 主屏深背景 #0F172A（时间设置年/月/日黄线下方背景） */
#define SC_C_MAIN_BG UI_RGB(255, 241, 209)  /* 坑#84 新主屏背景米黄（顶部小图标/状态角标/沙漏擦除色，替代 SC_C_PANEL） */
#define C_TIME_ICO   UI_RGB(92, 58, 42)     /* 坑#84 新主屏顶部状态图标深棕（米黄背景上，替代原白） */
#define SC_C_MAIN_INK UI_RGB(92, 58, 42)    /* 坑#84 新主屏深棕（时间/日期/闹钟大数字用，米黄背景上对比强，2026-10-10；设置光标已改蓝 SC_C_BLUE，2026-11-03） */
/* 坑#85 闹钟屏：开关 ON 控件 / 星期未选中灰白瓶 从效果图截取存 ALM2 容器（UI_DrawAlm2 贴图，
   不再自绘几何近似；OFF/选中状态靠恢复背景）。闹钟大数字用 SC_C_MAIN_INK 深棕（同主屏）。 */
#define SC_C_WHITE   UI_C_DIGIT             /* 大数字白 #F8FAFC */

/* ==================== 坑#88 倒计时屏新 UI（奶油风，2026-10-11） ====================
   背景：静态版导入（ui_screen1.png，浅米 #F2DBBB 系）+ 动态元素固件画。
   布局（效果图像素实测）：大数字 MM:SS 20x40 黑（0xE100+d）居中 y69..108；
   进度条轨道 x57-261 y119-146 在背景，填充橙/黄/红 x57-260 y122-143；
   按钮1「开始 START」画在背景（绿底黑字），固件运行/暂停时切「暂停 PAUSE」；
   顶部粉色标签内「倒计时/正计时」18x22 黑字动态；右下粉色框人物动画区。
   坑#93（2026-11-04）：分钟扩 3 位（MMM:SS，"120:00"，0x84 支持 1~120 分钟）——
   组 20*5+12=112 居中 x160 -> 分百 x104 分十 x124 分个 x144 冒号 x164 秒十 x176 秒个 x196。 */
#define SC_C_CNT_INK    UI_RGB(0, 0, 0)         /* 倒计时屏动态文字墨色：纯黑（效果图 #000000） */
#define SC_C_CNT_ORANGE UI_RGB(255, 178, 14)    /* 进度条运行橙 #FFB20E（效果图） */
#define SC_C_BTN1_GREEN UI_RGB(167, 206, 113)   /* 按钮1 绿底 #A7CE71（覆盖文字区用） */
#define CNT_DIG_LEFT   104    /* 大数字 分百 左（20x40 数字 0xE100+d，组 20*5+12=112 居中 x160；坑#93 3 位分钟） */
#define CNT_DIG_STEP    20
#define CNT_COL_LEFT   164    /* 冒号左（12x40，0xE304；分个 144+20=164） */
#define CNT_DIG_Y       69    /* 数字顶 y（40 高 -> 69..108，效果图数字中心 y88.5 对齐） */
#define CNT_MIN_MAX    120    /* 分钟上限（坑#93：0x84 支持 1~120 分钟，显示 MMM:SS；cnt_adjust 回绕边界） */
#define CNT_PROG_X      57    /* 进度条填充左（轨道 x57-261 背景，填充满态到 x260 w204） */
#define CNT_PROG_Y     122    /* 进度条填充顶（轨道 y119-146，填充 y122-143 h22） */
#define CNT_PROG_H      22
#define CNT_PROG_MAX   204    /* 填充最大宽（x57..260，轨道槽 x57-261 圆头顶点 x260；2026-10-12 实测轨道全长） */
#define CNT_BTN1_X     13    /* 按钮1 整块素材（x13-81 y187-228 = 69x42，从暂停态效果图 320x239 (2).png 截） */
#define CNT_BTN1_Y     187
#define CNT_BTN1_W     69
#define CNT_BTN1_H     42
#define CNT_BTN1_ITEM  2     /* ALM2 容器 item2 = 按钮1「暂停 PAUSE」整块（2026-10-12 用户定：不自己画，整块替换） */
#define CNT_TITLE_X     16    /* 顶部标签内「倒计时/正计时」18x22 黑字（标签 x10-75 y9-48 居中） */
#define CNT_TITLE_Y     18

/* 主屏后台计时沙漏图标（2026-09-05）：坑#84 顶部小图标行 (84,2)（与秒/整点报时/贪睡横排）。
   真彩图标走 ICO1 容器（UI_DrawHourglass，保留用户 PNG 多色调）； */
#define CNT_ICO_X      84     /* 顶部小图标行：x84（秒 24、报时 44、贪睡 64、沙漏 84） */
#define CNT_ICO_Y      2      /* 顶部 y2（16x16，y2..18） */
#define CNT_ICO_W      16
#define CNT_ICO_H      16

/* 主屏状态角标（2026-09-05 坑#71 #1/#2）：整点报时符号/贪睡符号，16x16 单色 glyph
   （0xE302/0xE303，build_flash build_font 从用户 PNG 生成）；坑#84 顶部小图标行。
   整点报时开启时常驻 0xE302（x44），贪睡待响时常驻 0xE303（x64）。擦除填新背景米黄 SC_C_MAIN_BG 无缝。 */
#define ST_ICO_X      44     /* 顶部小图标行：整点报时符号 x44 */
#define ST_ICO_Y1     2      /* 整点报时符号左上角 y2 */
#define ST_ICO_Y2     2      /* 贪睡符号左上角 y2（同一行横排，用 X 区分） */
#define ST_ICO_X2     64     /* 贪睡符号 x64 */
#define ST_ICO_W      16
#define ST_ICO_H      16
#define UI_ICO_HOURLY 0xE302u  /* 整点报时符号 glyph（16x16 白色单色） */
#define UI_ICO_SNOOZE 0xE303u  /* 贪睡符号 glyph（16x16 白色单色） */

/* ==================== 大数字/冒号槽（屏1/屏2 共用，块居中 x[73..247]） ==================== */
#define SC_DIG_LEFT  73    /* 槽中心 93/133/187/227 -> 左 73/113/167/207 */
#define SC_DIG_STEP  40
#define SC_COL_LEFT  153   /* 冒号中心 160 -> 左 153（14 宽） */
#define SC_CNT_Y     56    /* 倒计时大数字顶 y（84 高 -> 56..140） */
#define SC_ALM_Y     78    /* 闹钟大数字顶 y（84 高 -> 78..162，垂直居中） */
#define SC_ALM_DIG_LEFT 17 /* 坑#85 闹钟屏大数字左起点：组 40*4+14=174 居中主卡(x14-195 中心104.5) -> 左17 */
#define SC_ALM_COL_LEFT 97 /* 坑#85 闹钟屏冒号左（14 宽：H2 57+40=97 与 M1 111 之间） */

/* ==================== 倒计时/正计时状态 ==================== */
static cnt_state_t g_cnt = CNT_STOP;   /* 当前状态（倒计时与正计时共用 RUN/PAUSE/DONE 状态机） */
static u16 g_cnt_total = 0;            /* 总秒数（进度条分母；正计时=0 不画进度条） */
static u16 g_cnt_remain = 0;           /* 剩余秒数（倒计时递减） */
static u32 g_cnt_ret_ms = 0;           /* 结束/停止后回主屏的 bftm0_ct 时刻（0=未定时） */
static u8  g_up = 0;                   /* 1=当前为【正计时】模式（g_cnt_remain 递增显示；0=倒计时） */
static u32 g_up_sec = 0;               /* 正计时已过秒数（从 0 往上；u32 防溢出） */
static u8  g_cnt_ringing = 0;          /* 倒计时到点响铃标志（DONE 60s 内响；回主屏/别响时停，2026-08-28） */
static u8  g_cnt_pending_ring = 0;     /* 坑#93：倒计时到点遇闹钟响铃/设置态时的【待响】标志——闹钟响完（Screen_AlarmRingStop）后补响倒计时钟声+切倒计时屏 */
static u32 g_cnt_idle_ms = 0;          /* 倒计时屏最后操作时刻（进屏/按键/命令刷新；未运行空闲 15s 自动回主屏，坑#71 2026-09-05） */
static u8  g_disp_off = 0;             /* 1=显示已关（语音 0xB5 关显示；倒计时/闹钟响铃时自动亮屏，坑#71 #15 2026-09-05） */

/* ==================== 倒计时屏按键选择（2026-09-05） ====================
   光标常驻五项：分 / 秒 / 暂停 / 重置 / 退出（正计时 g_up 时仅 暂停/重置/退出 三项）；
   左(PB12)右(PB14)切换选中，确定(PB13)：
     暂停=开始/暂停/继续切换；重置=回初始总时长并停止待启动；
     分/秒=进入调整态（光标变蓝，左右调整数值，再按确定退出；调整自动暂停）；
     退出=回主屏（倒计时/正计时后台继续跑；到点自动切回本屏响铃，2026-09-05）。 */
#define CNT_SEL_MODE   0   /* 正/倒计时模式（顶部标题处，2026-09-05 加入光标选择） */
#define CNT_SEL_MIN    1   /* 分（MM 大数字 M 段） */
#define CNT_SEL_SEC    2   /* 秒（SS 段） */
#define CNT_SEL_PAUSE  3   /* 暂停/继续（按钮1） */
#define CNT_SEL_RESET  4   /* 重置（按钮2） */
#define CNT_SEL_EXIT   5   /* 退出（按钮3）：回主屏，倒计时后台继续跑（2026-09-05） */
#define CNT_SEL_COUNT  6
static u8  g_cnt_sel = CNT_SEL_MODE;   /* 当前选中项（默认模式项） */
static u8  g_cnt_adj = 0;                /* 1=分/秒调整态（光标变色，左右调整数值） */
static s8  g_cnt_sel_line = -1;          /* 上次画光标的项（-1=未画；增量清旧画新） */
static u16 g_cnt_sel_bg_ver = 0xFFFF;    /* 上次画光标时的背景版本（背景重传后强制重画） */
static u16 g_cnt_sel_color = 0;          /* 上次画光标的颜色（黄/蓝；进出调整态变色检测，2026-09-05） */

/* ==================== 闹钟运行时状态（配置在 ui_cfg_t，此处补 loop/ringing 等） ==================== */
static struct {
  u8  loop;         /* 0单次(按日响) 1一~五 2一~六 3一~日 */
  u8  ring_sel;     /* 铃音 0/1 */
  u8  ringing;      /* 1=正在响铃 */
  u8  rung;         /* 本分钟已响过（防同分重复触发） */
  u16 rung_min;     /* 坑#90：rung 置位时的分钟数(hour*60+min)——跨分钟自动失效，防前一闹钟响过后把后续闹钟吞掉 */
  u32 ring_start;   /* 响铃起始 bftm0_ct（60s 自动停） */
  u8  snooze;       /* 贪睡待响标志（1=贪睡延迟重响待触发；0=无。2026-09-05 由计数改一次性待响标志） */
} g_alm;
static u32 g_alm_snooze_ms = 0;        /* 贪睡重响时刻 bftm0_ct（贪睡 = 停响 + 一次性延迟 10 分钟，配置不变，坑#71 #9） */

/* ---- 3 个闹钟槽（运行时内存态，2026-09-05：ui_cfg_t 单闹钟仅作槽1 初始值，闹钟配置不写 Flash） ---- */
static u8 g_alm_hour[3];   /* 槽 i 时 */
static u8 g_alm_min[3];    /* 槽 i 分 */
static u8 g_alm_on[3];     /* 槽 i 开关（1=开） */
static u8 g_alm_loop[3];   /* 槽 i 循环 0单次/1一~五/2一~六/3一~日 */
static u8 g_alm_sel      = 0;  /* 屏2当前显示/操作槽（0..2） */
static u8 g_alm_ring_idx = 0;  /* 当前正在响的槽（贪睡 +10min 用） */

/* ---- 闹钟屏按键设置（2026-09-05，参考时间模式：黄线+确定切项+哨兵自动退出） ---- */
#define ALM_SET_SEL   0   /* 闹钟选择（1/2/3） */
#define ALM_SET_ON    1   /* 开启关闭 */
#define ALM_SET_HOUR  2   /* 闹钟时（0-23） */
#define ALM_SET_MIN   3   /* 闹钟分（0-59） */
#define ALM_SET_LOOP  4   /* 闹钟循环 */
#define ALM_SET_DONE  5   /* 哨兵：切到即自动退出 */
static u8  g_alm_set_active = 0;   /* 1=闹钟设置模式 */
static u8  g_alm_set_item   = 0;   /* 当前设置项（ALM_SET_*） */
static s8  g_alm_set_line = -1;    /* 上次画的黄线项（-1=未画） */
static u16 g_alm_set_bg_ver = 0xFFFF;  /* 上次画黄线时的背景版本 */
static u32 g_alm_set_last_act = 0; /* 最近一次按键时刻（15s 无按键自动退出设置） */
static u32 g_alm_set_exit_ms  = 0; /* 退出设置时刻（0=未计时）；退出后 3s 自动回主屏（2026-09-05） */
static u32 g_alm_idle_ms      = 0; /* 闹钟屏最后操作时刻（进入/按键/命令刷新；0=不在闹钟屏）。
                                      闹钟屏空闲超时自动回主屏的常态判断用（2026-09-05） */

/* ==================== 整点报时（0xB8，2026-08-28） ==================== */
static u8 g_hourly_on = 0;        /* 整点报时开关（内存开关，掉电复位默认关） */
static u8 g_last_hourly = 0xFF;   /* 上次已播报的小时（防同小时重复播） */

/* ==================== 按键设置模式状态（2026-09-04；业务逻辑在文件尾 Screen_HandleKey） ==================== */
#define SET_ITEM_HOUR  0
#define SET_ITEM_MIN   1
#define SET_ITEM_SEC   2
#define SET_ITEM_YEAR  3
#define SET_ITEM_MONTH 4
#define SET_ITEM_DAY   5
#define SET_ITEM_DONE  6
#define SET_ITEM_COUNT 7
static u8         g_set_active = 0;      /* 1=设置模式（时间冻结显示，KEY1 减 / KEY3 加） */
static u8         g_set_item   = 0;      /* 当前设置项（SET_ITEM_*，KEY2 短按循环） */
static TimeStruct g_set_ws;              /* 设置工作区：进设置快照，退出时写回 RTC */
static u32        g_set_last_act = 0;    /* 最近一次按键时刻（15s 超时退出） */
static s8         g_set_line = -1;       /* 上次画的黄线项（-1=未画；切项清旧画新） */
static u16        g_set_bg_ver = 0xFFFF; /* 上次画黄线时的主屏背景版本（背景重传后强制重画） */
static s16        g_set_line_x = -1;     /* 上次画黄线的几何（坑#89 日期居中随位数浮动：画与清都用缓存，防清错位） */
static s16        g_set_line_w = 0;
static s8         g_set_line_y = 0;

static void cnt_ring_stop(void);  /* 前置声明：停倒计时到点响铃（goto_main 在定义之前使用） */
static void set_exit(void);       /* 前置声明：退出按键设置模式（Screen_OnSecond 15s 超时用） */
static void cnt_sel_line(void);   /* 前置声明：倒计时屏选择光标增量绘制（draw_cnt 调，2026-09-05） */
static void alm_set_exit(void);   /* 前置声明：退出闹钟按键设置（Screen_OnSecond 15s 超时用，2026-09-05） */
static void alm_set_line(void);   /* 前置声明：闹钟屏设置黄线增量绘制（alm_set_key/alm_set_exit 调，2026-09-05） */
static void screen_key_main(const KeyEvent *ke); /* 前置声明：主屏时间设置模式（Screen_HandleKey 的 MAIN 分流） */
/* 坑#92（2026-11-03）：三屏光标「几何+绘制+清除」三件套集中到文件后部统一管理，
   此处补充被前向调用的绘制/清除函数前置声明（cnt_sel_line / alm_set_line / alm_set_exit 在集中区之前） */
static void cnt_sel_draw(u8 item, u16 color);   /* 前置声明：倒计时屏光标绘制（集中区定义） */
static void cnt_sel_clear(u8 item);             /* 前置声明：倒计时屏光标清除（集中区定义） */
static void alm_set_draw(u8 item, u16 color);   /* 前置声明：闹钟屏黄线绘制（集中区定义） */
static void alm_set_clear(u8 item);             /* 前置声明：闹钟屏黄线清除（集中区定义） */

/* 各设置项黄线/光标下方的实际背景色（2026-09-05 修正：清线须清回该处背景色，
   主卡/面板内是 #1B2336（C_PANEL），主屏深背景是 #0F172A（C_SCREEN）——
   原统一用 #0F172A 清线，在面板背景上残留深一档"黑底"。）
   主屏：HOUR/MIN/SEC 在主卡(12,32,308,120)内 -> PANEL；YEAR/MONTH/DAY 在日期卡下方
         （12,126,152,152 底部 152 之外 3px）-> SCREEN。
   倒计时屏：光标 y=143/225 在主卡(12,48,308,228)内 -> PANEL（cnt_sel_line 直接用）。
   闹钟屏：黄线 y=37/165/210 均在顶部条/主卡(12,48,308,228)内 -> PANEL（alm_set_line 直接用）。
   坑#84 新主屏：主屏黄线已改用「恢复背景」清线（set_line_clear，2026-10-10）——
   不再依赖固定背景色，可精确还原日期卡内渐变/描边。倒计时/闹钟屏黄线仍用各自背景色
   （cnt_sel_line / alm_set_line 内直接处理）。 */
static void set_line_clear(u8 item);   /* 前置声明（set_exit 调用，定义见下） */

/* ==================== 增量刷新缓存（0xFF/非法 -> 首帧强制全绘） ==================== */
static u8  c_drawn_screen = 0xFF;   /* 已绘制的屏号（切屏触发静态元素重绘） */
static u16 c_bg_ver = 0xFFFF;       /* 上次已绘背景版本号（g_ui_bg_ver；初始不同=首次必全绘） */
static u8  c_cnt_m3 = 0xFF, c_cnt_m2 = 0xFF, c_cnt_m1 = 0xFF, c_cnt_s1 = 0xFF, c_cnt_s2 = 0xFF;  /* 大数字 5 位缓存（坑#93：分百/分十/分个/秒十/秒个） */
static u8  c_cnt_w3 = 0xFF;   /* 坑#93：上次分钟位数布局（1=3 位分钟≥100；0=2 位分钟<100；0xFF=首帧强制切换重画） */
static u8  c_cnt_state = 0xFF;
static u8  c_cnt_title = 0xFF;      /* 已绘顶部标题模式：0=倒计时 1=正计时（背景不再画静态字，2026-09-05） */
static int c_cnt_prog = -1;         /* 已绘制的进度条填充宽度 */
static u16 c_prog_bar = 0;          /* 已绘制的进度条填充色（换色时整条重画用，2026-08-28） */
static u8  c_cnt_blink_on  = 0xFF;  /* 上次沙漏亮/灭（0xFF=首帧必画，2026-09-05） */
static u8  c_cnt_blink_act = 0xFF;  /* 上次沙漏三态模式（0=无 1=暂停常亮 2=运行闪烁，0xFF=首帧，坑#77 #5） */
static u8  c_cnt_blink_drawn = 0;   /* 沙漏当前是否画在屏上（1=画着，擦除判断） */
static u8  c_ico_hourly = 0xFF;     /* 上次整点报时符号亮/灭（0xFF=首帧；0=灭 1=亮，坑#71 2026-09-05） */
static u8  c_ico_snooze = 0xFF;     /* 上次贪睡符号亮/灭（0xFF=首帧） */
static u8  c_ico_hourly_drawn = 0;  /* 整点报时符号是否画在屏上（擦除判断） */
static u8  c_ico_snooze_drawn = 0;  /* 贪睡符号是否画在屏上（擦除判断） */
static u8  c_alm_h1 = 0xFF, c_alm_h2 = 0xFF, c_alm_m1 = 0xFF, c_alm_m2 = 0xFF;
static u8  c_alm_on = 0xFF, c_alm_loop = 0xFF, c_alm_ring = 0xFF;
static u8  c_alm_sel = 0xFF;      /* 已绘制的闹钟槽号（坑#86 粉条「闹钟N」序号增量，原左上 chip） */

/* ==================== 小工具 ==================== */

static void disp_wake(void);   /* 前置声明：响铃自动亮屏（cnt_pending_ring_play 在定义前调用，防 C89 隐式 extern 与 static 定义冲突） */

/* 坑#93（2026-11-04）：倒计时到点待响（cnt_tick 置 g_cnt_pending_ring：遇闹钟响铃/设置态）——
   补响 = 切倒计时屏（显示 00:00 已结束红）+ 播倒计时钟声 + 60s 自动停回主屏。
   goto_main（闹钟响停/0xEF 等回主屏路径）与 set_exit（主屏设置退出，不回 goto_main）都调。 */
static void cnt_pending_ring_play(void)
{
  if (!g_cnt_pending_ring) return;
  g_cnt_pending_ring = 0;
  g_cnt_ringing = 1;
  g_cnt_ret_ms  = (u32)bftm0_ct + 60000u;   /* 补响 60s 自动停回主屏 */
  disp_wake();                              /* 响铃自动亮屏（坑#71 #15） */
  (void)UI_CharAnim_SetState(UI_CANI_STATE_ALARM);
  Voice_Send(VOICE_TX_RING, g_alm.ring_sel, 1, 0, 0);
  UI_SetScreen(UI_SCREEN_CNT);
  g_cnt_idle_ms = (u32)bftm0_ct;
  printf("[KEY] 倒计时待响补响（闹钟响停/设置退出）\r\n");
}

/* 回主屏统一入口：切屏上传干净主屏背景后，必须重置 calendar 增量缓存，
   否则 Calendar_Refresh 判定"没变化"不重绘 -> 数字/日期/小数点消失。
   2026-08-28 修复（回主屏所有路径都走这里）；2026-08-28 再：先停倒计时到点响铃。 */
static void goto_main(void)
{
  cnt_ring_stop();          /* DONE 60s 到点/0xB1 别响/15s 超时 任何回主屏路径都停铃 */
  /* 退出各设置态防残留（响铃/切屏打断后回主屏，按键不应停留在设置态，2026-09-05） */
  g_set_active = 0; g_set_item = 0; g_set_line = -1;
  g_alm_set_active = 0; g_alm_set_item = 0; g_alm_set_line = -1;
  g_alm_set_exit_ms = 0;   /* 取消「退出设置后自动回主屏」倒计时（回主屏了自然作废，2026-09-05） */
  g_alm_idle_ms = 0;       /* 离开闹钟屏：清空闲计时（2026-09-05） */
  c_cnt_blink_on = 0xFF; c_cnt_blink_act = 0xFF; c_cnt_blink_drawn = 0;  /* 回主屏：沙漏缓存重置（背景重传已覆盖） */
  c_ico_hourly = 0xFF; c_ico_snooze = 0xFF;      /* 回主屏：状态角标缓存重置（背景重传已覆盖，2026-09-05） */
  c_ico_hourly_drawn = c_ico_snooze_drawn = 0;
  g_cnt_idle_ms = (u32)bftm0_ct;                  /* 回主屏：倒计时屏空闲计时重新起算 */
  /* 坑#93（2026-11-04）：倒计时到点待响（遇闹钟响铃/设置态，cnt_tick 置 g_cnt_pending_ring）——
     任何回主屏路径（闹钟响停/设置退出/0xEF 等）在此补响倒计时钟声 + 切倒计时屏（显示 00:00 已结束红） */
  if (g_cnt_pending_ring)
  {
    cnt_pending_ring_play();
    return;                                   /* 已切倒计时屏响铃 */
  }
  UI_SetScreen(UI_SCREEN_MAIN);
  Calendar_ForceRedraw();
}

/* 响铃自动亮屏（坑#71 #15 2026-09-05）：倒计时/闹钟到点响铃时若显示已关（语音 0xB5），
   自动开显示 + 亮背光 + 强制重绘，保证响闹可见。 */
static void disp_wake(void)
{
  if (!g_disp_off) return;
  g_disp_off = 0;
  UI_SetDispVisible(1);          /* 响铃自动亮屏：显示可见（2026-10-12 切屏 DISPOFF 依据） */
  Display_on();
  GPIO_ClearOutBits(SPI_LCD_BL_GPIO, SPI_LCD_BL_PIN);
  g_ui_bg_ver++;          /* 强制下一帧重绘动态元素 */
}

/* 取消贪睡待响（2026-09-05）：重设闹钟时间/关闭响铃槽/别响了 时调用，防过期贪睡再响。 */
static void alm_snooze_clear(void)
{
  g_alm.snooze = 0;
  g_alm_snooze_ms = 0;
}

/* 主屏状态角标（坑#71 #1/#2 2026-09-05，App_Render 主屏分支每帧调）：
   整点报时开启时常驻 0xE302 符号、贪睡待响时常驻 0xE303 符号（主卡左上 16x16 白）；
   用 c_* 缓存去重，状态变化才画/擦，回主屏 goto_main 重置缓存强制重画。 */
void Screen_StatusIcons(void)
{
  u8 h = g_hourly_on ? 1 : 0;
  /* 贪睡符号闪烁（2026-09-05 用户需求「贪睡状态符号要闪烁」）：待响时每 0.5s 亮/灭
     （与后台沙漏同节奏）；非待响恒灭。s=0 的「闪烁灭相」与「无待响」都落到擦除，
     c_* 缓存去重即可。 */
  u8 s = g_alm.snooze ? (u8)((bftm0_ct / 500u) & 1u) : 0;
  if (h != c_ico_hourly)
  {
    if (h)
    {
      UI_DrawGlyphRect(ST_ICO_X, 239 - (ST_ICO_Y1 + ST_ICO_H) + 1, ST_ICO_W, ST_ICO_H,
                       UI_ICO_HOURLY, C_TIME_ICO);
      c_ico_hourly_drawn = 1;
    }
    else if (c_ico_hourly_drawn)
    {
      Display_FillRect(ST_ICO_X, ST_ICO_Y1, ST_ICO_W, ST_ICO_H, SC_C_MAIN_BG);
      c_ico_hourly_drawn = 0;
    }
    c_ico_hourly = h;
  }
  if (s != c_ico_snooze)
  {
    if (s)
    {
      UI_DrawGlyphRect(ST_ICO_X2, 239 - (ST_ICO_Y2 + ST_ICO_H) + 1, ST_ICO_W, ST_ICO_H,
                       UI_ICO_SNOOZE, C_TIME_ICO);
      c_ico_snooze_drawn = 1;
    }
    else if (c_ico_snooze_drawn)
    {
      Display_FillRect(ST_ICO_X2, ST_ICO_Y2, ST_ICO_W, ST_ICO_H, SC_C_MAIN_BG);
      c_ico_snooze_drawn = 0;
    }
    c_ico_snooze = s;
  }
}

static void draw_digit_at(int x, int y, u8 d, u16 color)
{
  char b[2] = { (char)('0' + d), 0 };
  UI_Show(x, y, b, UI_SLOT_CLOCK, color);
}

/* 冒号（14x84，屏1/2 共用；静态不闪；坑#85 闹钟屏改深棕传色） */
static void draw_colon_at(int x, int y, u16 color)
{
  UI_DrawGlyphRect(x, 239 - (y + 84) + 1, 14, 84, (u32)':', color);
}

/* ==================== 坑#88 倒计时屏 20x40 大数字 / 12x40 冒号（0xE100+d / 0xE304） ====================
   新背景数字区 y69..108（效果图 34px 黑字区域，用主屏 20x40 数字放大排版，用户 2026-10-11 定）。
   墨色 SC_C_CNT_INK 黑；清字/恢复背景用 code=0xFFFF（UI_DrawGlyphRect 仅还原背景）。 */
static void draw_cnt_digit_at(int x, int y, u8 d)
{
  UI_DrawGlyphRect(x, 239 - (y + 40) + 1, 20, 40, 0xE100u + d, SC_C_CNT_INK);
}
static void draw_cnt_colon_at(int x, int y)
{
  UI_DrawGlyphRect(x, 239 - (y + 40) + 1, 12, 40, 0xE304u, SC_C_CNT_INK);
}

/* ==================== 坑#88 进度条胶囊圆头填充（2026-10-12 用户反馈「两侧超界」） ====================
   原直角 Display_FillRect 方角会盖出轨道两端半圆（左端 x57-65 黑弧、右端 x251-260），
   效果图填充实为胶囊：逐列橙行数 [2,9,13,15,17,18,19,20,21,22] 反推 r≈9 圆头。
   本表 = 逐行两端缩进（行 0..21 = y122..143），与效果图左端逐列逐行吻合（右端同表镜像）。
   2026-10-12 修正「没走到头」后满态 w=204，右圆头随 xr=260 对称于左端。 */
static const u8 CNT_PROG_INSET[CNT_PROG_H] = {
  9, 7, 5, 4, 3, 2, 2, 1, 1, 1, 0, 0, 1, 1, 1, 1, 2, 2, 3, 4, 6, 8
};

/* 画进度条填充（胶囊圆头）：x0=CNT_PROG_X 起宽 w、y0..y0+21，逐行画水平段。
   w<18 时两端圆头重叠 -> 退化为直径 w 的圆（从 0 生长的小圆点，不会方角外露）。 */
static void cnt_prog_fill(int w, u16 color)
{
  int x0 = CNT_PROG_X, y0 = CNT_PROG_Y;
  if (w <= 0) return;
  if (w < 18)   /* 窄：两端圆头重叠 -> 直径 w 的圆 */
  {
    int cx = x0 + w / 2;
    int rr = (w + 1) / 2;
    for (int yy = 0; yy < CNT_PROG_H; yy++)
    {
      int dy = yy - (CNT_PROG_H / 2);
      int d2 = dy * dy;
      if (d2 > rr * rr) continue;
      int half = 0;
      for (int dx = rr; dx > 0; dx--)
        if (dx * dx + d2 <= rr * rr) { half = dx; break; }
      Display_FillRect(cx - half, y0 + yy, 2 * half + 1, 1, color);
    }
    return;
  }
  int xr = x0 + w - 1;   /* 填充右缘列 */
  for (int yy = 0; yy < CNT_PROG_H; yy++)
  {
    int ins = CNT_PROG_INSET[yy];
    Display_FillRect(x0 + ins, y0 + yy, (xr - ins) - (x0 + ins) + 1, 1, color);
  }
}

/* ==================== 倒计时 ==================== */
static int weekday_match(u8 loop)
{
  /* 用当前 RTC 的星期匹配循环模式；0=单次(按日响)。2026-09-05 改传 loop 参数（3 闹钟槽各自循环） */
  TimeStruct t;
  TimeModule_GetTime(&t);
  int wd = CalcWeekday(t.year, t.month, t.day);   /* 0=周一..6=周日 */
  switch (loop)
  {
    case 0:  return 1;                    /* 单次：任意一天（每日响） */
    case 1:  return (wd <= 4) ? 1 : 0;    /* 周一~五 */
    case 2:  return (wd <= 5) ? 1 : 0;    /* 周一~六 */
    default: return 1;                    /* 3=周一~日（每天） */
  }
}

void Screen_CntStart(u16 total_s)
{
  if (total_s == 0) return;
  cnt_ring_stop();                       /* 启动新倒计时前停残留响铃 */
  g_cnt = CNT_RUN;
  g_cnt_total  = total_s;
  g_cnt_remain = total_s;
  g_cnt_ret_ms = 0;
  g_up = 0;                            /* 切回倒计时模式（正计时未跑则无影响） */
  g_cnt_idle_ms = (u32)bftm0_ct;       /* 计时活动：刷新倒计时屏空闲计时（坑#71） */
  if (UI_GetScreen() != UI_SCREEN_CNT) UI_SetScreen(UI_SCREEN_CNT);  /* 已在倒计时屏则不重传背景，改状态由 draw_cnt 增量刷新 */
  Voice_Send(VOICE_TX_CNT_START, 0, 0, 0, 0);   /* 坑#93：倒计时启动播"计时开始"提示音（0x0D） */
}

/* 0xBA 打开正计时：从 00:00 递增，复用屏1 MM:SS；与倒计时互斥（2026-08-28） */
void Screen_UpStart(void)
{
  cnt_ring_stop();                       /* 启动正计时前停残留响铃 */
  g_cnt = CNT_RUN;
  g_cnt_total  = 0;                    /* 正计时无进度条（分母 0） */
  g_cnt_remain = 0;
  g_cnt_ret_ms = 0;
  g_up = 1;
  g_up_sec = 0;
  g_cnt_sel = CNT_SEL_MODE;              /* 正计时含模式项：选中项强制到模式（2026-09-05 模式入光标） */
  g_cnt_adj = 0;
  g_cnt_idle_ms = (u32)bftm0_ct;       /* 计时活动：刷新倒计时屏空闲计时（坑#71） */
  if (UI_GetScreen() != UI_SCREEN_CNT) UI_SetScreen(UI_SCREEN_CNT);  /* 已在倒计时屏则不重传背景 */
  Voice_Send(VOICE_TX_CNT_START, 0, 0, 0, 0);   /* 坑#93：正计时启动播"计时开始"提示音（0x0D） */
}

void Screen_CntPause(void)
{
  if (g_cnt == CNT_RUN) g_cnt = CNT_PAUSE;
  g_cnt_idle_ms = (u32)bftm0_ct;       /* 暂停是活动：刷新空闲计时（防刚暂停就被自动返回，坑#71） */
  Voice_Send(VOICE_TX_STOP_SOUND, 0, 0, 0, 0);   /* 坑#93：暂停停掉正在播的提示音/铃声（0x0C 声音停止） */
}

void Screen_CntResume(void)
{
  if (g_cnt == CNT_PAUSE) g_cnt = CNT_RUN;
  g_cnt_idle_ms = (u32)bftm0_ct;       /* 继续是活动：刷新空闲计时（坑#71） */
}

void Screen_CntStop(void)
{
  if (g_cnt == CNT_STOP) return;
  g_cnt = CNT_DONE;
  g_cnt_ret_ms = (u32)bftm0_ct + 3000u;   /* 停 3s 回主屏 */
  g_cnt_idle_ms = (u32)bftm0_ct;          /* 停止是活动：刷新空闲计时（坑#71） */
  Voice_Send(VOICE_TX_STOP_SOUND, 0, 0, 0, 0);   /* 坑#93：停止停掉正在播的声音（0x0C 声音停止） */
}

int Screen_CntActive(void)
{
  return (g_cnt == CNT_RUN || g_cnt == CNT_PAUSE) ? 1 : 0;
}

/* 主屏后台计时沙漏图标（App_Render 主屏分支每帧调，2026-09-05）：
   正/倒计时后台（退出按钮回主屏后计时继续）时，在主卡右上角
   (284,48,16x16) 画沙漏（ICO1 真彩容器 UI_DrawHourglass，保留用户 PNG 多色调）。
   三态（坑#77 #5 2026-09-05 用户「暂停时符号停止闪烁，结束符号消失」）：
   运行=0.5s 亮/灭闪烁提示仍在计时；暂停=常亮不闪（计时挂后台但未走）；停止/结束/待启动=消失。
   画：UI_DrawHourglass 用设计坐标；擦：Display_FillRect 用设计坐标（主卡背景 SC_C_PANEL）。
   两者背景同为 #1B2336，透明像素填面板色 -> 擦除/重画无缝（2026-09-05 教训：勿混 GRAM/设计坐标）。 */
void Screen_CntBlink(u32 now_ms)
{
  /* 三态模式（仅主屏考虑显示）：0=无 1=暂停常亮 2=运行闪烁 */
  u8 mode = 0;
  if (UI_GetScreen() == UI_SCREEN_MAIN)
  {
    if (g_cnt == CNT_RUN)        mode = 2;
    else if (g_cnt == CNT_PAUSE) mode = 1;
  }
  u8 on = (mode == 2) ? (u8)((now_ms / 500) & 1) : (mode == 1) ? 1 : 0;
  if (on == c_cnt_blink_on && mode == c_cnt_blink_act) return;   /* 无变化：不重画 */
  if (c_cnt_blink_drawn)                            /* 已画过：先擦旧图标 */
  {
    Display_FillRect(CNT_ICO_X, CNT_ICO_Y, CNT_ICO_W, CNT_ICO_H, SC_C_MAIN_BG);
    c_cnt_blink_drawn = 0;
  }
  if (on)
  {
    UI_DrawHourglass(CNT_ICO_X, CNT_ICO_Y);   /* 真彩图标：保留用户 PNG 多色调明暗（ICO1 容器） */
    c_cnt_blink_drawn = 1;
  }
  c_cnt_blink_on  = on;
  c_cnt_blink_act = mode;
}

/* 计时每秒推进（Screen_OnSecond 调）：倒计时递减，正计时递增 */
static void cnt_tick(void)
{
  if (g_cnt != CNT_RUN) return;
  if (g_up)                        /* 正计时：从 0 往上累计 */
  {
    g_up_sec++;
    g_cnt_remain = (u16)(g_up_sec > 0xFFFFu ? 0xFFFFu : g_up_sec);  /* 同步给屏显（u16 封顶防回绕） */
    return;
  }
  if (g_cnt_remain > 0)
  {
    g_cnt_remain--;
    if (g_cnt_remain == 0)
    {
      g_cnt = CNT_DONE;
      /* 坑#71（倒计时与闹钟同时到达 → 用户定"优先闹钟，只跳闹钟界面"）：Screen_OnSecond 已改为
         先 alarm_tick 后 cnt_tick——同秒两者都到点时闹钟先响（g_alm.ringing=1、已切闹钟屏），
         这里倒计时到点【不播铃/不切屏】，仅置 DONE 结束。
         坑#93（2026-11-04）：【不静默丢弃】——遇闹钟响铃/设置态置 g_cnt_pending_ring 待响，
         闹钟响完（Screen_AlarmRingStop：60s 自动停/0xB1 别响/按键打断）后补响倒计时钟声+切倒计时屏
         （用户实测"在闹钟响闹期间，倒计时到达。倒计时不会提醒"）。
         坑#79：设置模式（时间/闹钟设置）中倒计时到点同样不响不切屏——设置态待响，
         退出设置后由哪补响？设置退出走 alm_set_exit（3s 后 goto_main）不回 Screen_AlarmRingStop，
         故 g_cnt_pending_ring 在 goto_main 里统一补响（见 goto_main）。 */
      if (g_alm.ringing || g_set_active || g_alm_set_active)
      {
        g_cnt_ret_ms    = 0;      /* 不设自动回屏计时（闹钟响铃路径负责回主屏） */
        g_cnt_ringing   = 0;      /* 不立即播倒计时钟声（不打断闹钟铃声/不打断设置） */
        g_cnt_pending_ring = 1;   /* 坑#93：待响——闹钟响完/设置退出后补响 */
        printf("[KEY] 倒计时到点遇闹钟响铃/设置态：待响（稍后补响）\r\n");
      }
      else
      {
        g_cnt_ret_ms = (u32)bftm0_ct + 60000u;   /* 到点响铃停 60s 回主屏（与闹钟 1 分钟一致，2026-08-28） */
        g_cnt_ringing = 1;
        disp_wake();                              /* 响铃自动亮屏（显示若已关则开，坑#71 #15） */
        (void)UI_CharAnim_SetState(UI_CANI_STATE_ALARM);   /* 到点响铃：人物切闹铃动画（2026-09-04 接线） */
        Voice_Send(VOICE_TX_RING, g_alm.ring_sel, 1, 0, 0);   /* 到点响铃（2026-08-28 由 0x05 滴声改为播铃声） */
        /* 2026-09-05：倒计时可在后台运行（退出按钮回主屏不停止）——到点须切回倒计时屏
           显示 00:00 + 已结束 + 响铃，否则主屏上只会响铃无画面。
           正处闹钟响铃/任一设置态时不切走（不打断），照常响铃、60s 后自动回主屏。 */
        if (UI_GetScreen() != UI_SCREEN_CNT && !g_alm.ringing && !g_alm_set_active && !g_set_active)
        {
          UI_SetScreen(UI_SCREEN_CNT);
          g_cnt_idle_ms = (u32)bftm0_ct;   /* 坑#83：切回倒计时屏立即刷新空闲计时（防旧值被 15s 自动回误判） */
        }
      }
    }
  }
}

/* 停倒计时到点响铃（回主屏/0xB1 别响/启动新计时 都调；防铃声残留一直响） */
static void cnt_ring_stop(void)
{
  if (g_cnt_ringing)
  {
    g_cnt_ringing = 0;
    Voice_Send(VOICE_TX_RING, g_alm.ring_sel, 0, 0, 0);   /* 停播铃声 */
    (void)UI_CharAnim_SetState(UI_CANI_STATE_IDLE);       /* 停响回待机（2026-09-04 闹铃动画结束） */
  }
}

/* 坑#93（2026-11-04）：倒计时响铃【手动打断】（按键/0xB1 别响/0xEF，区别于 60s 自动停）——
   停铃 + 回设定值 + 停留倒计时屏（用户实测：a) "按键打断会直接返回到时间显示"应留屏；
   b) "打断响闹后去计时界面查看还是 0:00，未返回设置值"应回设定值）。
   60s 自动停（响满正常结束）不走本函数，保持 DONE 00:00。 */
static void cnt_ring_break(void)
{
  cnt_ring_stop();
  g_cnt_pending_ring = 0;               /* 无待响残留 */
  if (g_cnt == CNT_DONE)
  {
    g_cnt_remain = g_cnt_total;         /* 回设定值（total 可能已同步为当前值，显示设定时长） */
    g_cnt = CNT_STOP;
  }
  g_cnt_ret_ms = 0;                     /* 取消 DONE 自动回主屏计时 */
  if (UI_GetScreen() != UI_SCREEN_CNT) UI_SetScreen(UI_SCREEN_CNT);   /* 留在倒计时屏 */
  g_cnt_idle_ms = (u32)bftm0_ct;
  printf("[KEY] 倒计时响铃打断 -> 回设定值停留倒计时屏\r\n");
}

/* ==================== 倒计时屏按键选择系统（2026-09-05） ====================
   光标常驻五项：分/秒/暂停/重置/退出（正计时只 暂停/重置/退出）。左/右=切换选中 或 调整数值；
   确定=暂停/重置直接生效、分/秒进出调整态（光标黄↔蓝）、退出回主屏（计时后台继续跑）。见 Screen_HandleKey 分流。 */

/* 调整分/秒字段（自动暂停；待启动时同步 total=remain 保持进度条分母一致） */
static void cnt_adjust(int field, int delta)
{
  if (g_cnt == CNT_RUN) g_cnt = CNT_PAUSE;      /* 调整自动暂停 */
  u32 rem = g_cnt_remain;
  if (field == CNT_SEL_MIN)                     /* 分（M 段）：±1 分钟，0..120 回绕（坑#93 支持 120 分钟） */
  {
    int m = (int)(rem / 60) + delta;
    if (m < 0) m = CNT_MIN_MAX;
    if (m > CNT_MIN_MAX) m = 0;
    rem = (u32)m * 60u + (rem % 60);
  }
  else                                          /* 秒（S 段）：±1 秒，0..59 回绕 */
  {
    int s = (int)(rem % 60) + delta;
    if (s < 0) s = 59;
    if (s > 59) s = 0;
    rem = (rem / 60) * 60u + (u32)s;
  }
  g_cnt_remain = (u16)rem;
  /* 2026-09-05：调整后无条件同步 total=remain——原只在 STOP 态同步，
     暂停/运行中调整时 total 不更新 -> 进度条按旧比例算不归零，
     且 remain>total 时进度条 (total-remain) 下溢成巨大数超界。 */
  g_cnt_total = g_cnt_remain;
  printf("[KEY] 倒计时调整 %s%d -> %02u:%02u\r\n", field == CNT_SEL_MIN ? "分" : "秒", delta,
         (unsigned)(g_cnt_remain / 60), (unsigned)(g_cnt_remain % 60));
}

/* 「暂停」项确定：开始 / 暂停 / 继续 切换（与按钮1 文字一致） */
static void cnt_sel_pause_ok(void)
{
  if (g_cnt == CNT_RUN)
  {
    g_cnt = CNT_PAUSE;
    printf("[KEY] 倒计时暂停\r\n");
  }
  else if (g_cnt == CNT_PAUSE)
  {
    g_cnt = CNT_RUN;
    printf("[KEY] 倒计时继续\r\n");
  }
  else if (g_cnt == CNT_STOP || g_cnt == CNT_DONE)
  {
    /* 坑#78：倒计时 00:00（分秒都为 0）拒绝启动——否则进 RUN 后 cnt_tick 只在
       remain>0 时递减，永远卡在"运行中 00:00"。保持待启动，等用户先调时长。 */
    if (!g_up && g_cnt_remain == 0)
    {
      printf("[KEY] 倒计时 00:00 无法开始（先调整分/秒）\r\n");
      return;
    }
    /* 待启动：用当前剩余启动（分/秒调整后即设定时长）；正计时从 0 起 */
    cnt_ring_stop();                       /* DONE 到点响铃中重开：先停铃 */
    g_cnt_ret_ms = 0;
    if (g_up) g_up_sec = 0;
    g_cnt = CNT_RUN;
    printf("[KEY] 倒计时开始 %02u:%02u\r\n", (unsigned)(g_cnt_remain / 60), (unsigned)(g_cnt_remain % 60));
    Voice_Send(VOICE_TX_CNT_START, 0, 0, 0, 0);   /* 坑#93：按键启动倒计时/正计时也播"计时开始"（0x0D） */
  }
}

/* 「重置」项确定：清零（坑#93 2026-11-04：原倒计时回初始总时长，用户实测"无法手动清零"——改倒计时/正计时都归零 00:00 待启动；再启动需先调分/秒，坑#78 拒绝 00:00 启动） */
static void cnt_sel_reset(void)
{
  cnt_ring_stop();
  g_cnt_ret_ms = 0;
  g_cnt_remain = 0; g_cnt_total = 0; g_up_sec = 0;   /* 清零（total=0 进度条不画） */
  g_cnt = CNT_STOP;
  printf("[KEY] 倒计时重置清零 -> %02u:%02u\r\n", (unsigned)(g_cnt_remain / 60), (unsigned)(g_cnt_remain % 60));
}

/* 「退出」项确定：回主屏；进行中的倒计时/正计时【保留后台运行】，到点响铃自动切回倒计时屏
   （2026-09-05 用户定：倒计时屏手动设置时无法自动返回，才加退出按钮；退出不取消计时）。
   DONE 已结束态无后台意义 -> 复位防 Screen_Draw 60s 后再触发一次 goto_main 闪屏。 */
static void cnt_sel_exit(void)
{
  g_cnt_adj = 0;
  if (g_cnt == CNT_DONE) g_cnt = CNT_STOP;   /* 已结束：复位（goto_main 内部已停响铃） */
  g_cnt_ret_ms = 0;                          /* 取消 DONE 自动回主屏计时（已直接回） */
  goto_main();
  printf("[KEY] 倒计时退出回主屏（计时后台运行）\r\n");
}

/* 光标增量绘制（draw_cnt 每帧调）：切项/进出调整/背景重传时清旧画新；光标常驻 */
static void cnt_sel_line(void)
{
  if (UI_GetScreen() != UI_SCREEN_CNT) { g_cnt_sel_line = -1; g_cnt_sel_bg_ver = g_ui_bg_ver; return; }
  u16 color = g_cnt_adj ? SC_C_RED : SC_C_BLUE;   /* 坑#93（2026-11-04）：选中态黄看不清 -> 蓝；调整态蓝 -> 红（保留状态区分且都醒目） */
  /* 变化检测：选中项/背景版本/颜色 任一变化才重绘（2026-09-05 加颜色——
     进出调整态时 g_cnt_sel 不变，原来直接 return 导致黄↔蓝不变色） */
  if (g_cnt_sel == (u8)g_cnt_sel_line && g_cnt_sel_bg_ver == g_ui_bg_ver && color == g_cnt_sel_color) return;
  if (g_cnt_sel_line >= 0 && g_cnt_sel_bg_ver == g_ui_bg_ver)                     /* 清旧线（恢复背景） */
    cnt_sel_clear((u8)g_cnt_sel_line);
  g_cnt_sel_line = -1;
  cnt_sel_draw(g_cnt_sel, color);
  g_cnt_sel_line = (s8)g_cnt_sel;
  g_cnt_sel_bg_ver = g_ui_bg_ver;
  g_cnt_sel_color = color;
}

/* 「模式」项确定：正/倒计时模式切换（2026-09-05 入光标选择，替代原长按切换）。
   停当前计时 -> 清零归待启动 -> 光标/选项数适配（正计时只 模式/暂停/重置/退出，倒计时全 6 项） ->
   强制全绘（标题由 c_cnt_title 增量切换，大数字/进度条由 g_up 驱动）。
   UI 静态背景倒/正计时共用（标题固件按 g_up 动态画，坑#65）；此处 g_up 翻转。 */
static void cnt_sel_mode_ok(void)
{
  cnt_ring_stop();                       /* 停残留响铃 */
  g_up = !g_up;                          /* 翻转模式 */
  g_cnt = CNT_STOP; g_cnt_ret_ms = 0;    /* 停表回待启动（不保留旧计时） */
  g_cnt_remain = 0; g_up_sec = 0; g_cnt_total = 0;
  g_cnt_adj = 0;                         /* 退出分/秒调整态 */
  g_cnt_sel = CNT_SEL_MODE;              /* 光标停在模式项（顶部标题处），可再确定反复切换 */
  g_cnt_sel_line = -1;                   /* 强制重画光标 */
  g_ui_bg_ver++;                         /* 强制下一帧全绘（标题/大数字/进度条/按钮文字） */
  printf("[KEY] 计时模式 -> %s\r\n", g_up ? "正计时" : "倒计时");
}

/* 倒计时屏按键（Screen_HandleKey 分流调用）：左=KEY_ID_1 右=KEY_ID_3 确定=KEY_ID_2
   响铃中（g_cnt_ringing）：任意键都停铃回主屏（2026-09-05 用户需求「响闹可用按键打断」，
   与闹钟屏 screen_key_alarm 一致；此前只移动光标/激活选项，无法打断——坑#70）。 */
static void cnt_sel_key(const KeyEvent *ke)
{
  /* 响铃中：任意键打断 -> 停铃回主屏 */
  if (g_cnt_ringing)
  {
    (void)ke;
    if (g_cnt == CNT_DONE) g_cnt = CNT_STOP;   /* 已结束：复位（goto_main 内部已停响铃） */
    g_cnt_ret_ms = 0;                          /* 取消 DONE 自动回主屏计时（已直接回） */
    goto_main();                               /* 内部 cnt_ring_stop + 人物回待机 IDLE */
    printf("[KEY] 倒计时响铃按键打断 -> 回主屏\r\n");
    return;
  }
  g_cnt_idle_ms = (u32)bftm0_ct;   /* 倒计时屏按键=活动：刷新空闲计时（未运行空闲 15s 自动回主屏，坑#71 #11/#12） */
  /* 左/右 */
  if (ke->id == KEY_ID_1 || ke->id == KEY_ID_3)
  {
    int delta = (ke->id == KEY_ID_3) ? 1 : -1;
    if (g_cnt_adj)
    {
      if (g_cnt_sel == CNT_SEL_MIN || g_cnt_sel == CNT_SEL_SEC)  /* 调整态：只调当前字段 */
        cnt_adjust((int)g_cnt_sel, delta);
    }
    else
    {
      /* 选择态：在有效项列表移动（倒计时全 6 项：模式/分/秒/暂停/重置/退出；
         正计时只 模式/暂停/重置/退出 四项——2026-09-05 修坑#71 #13：原 n=4 取到
         模式/分/秒/暂停，重置/退出永远到不了，正计时改独立 4 项表） */
      if (g_up)
      {
        static const u8 up_items[4] = {CNT_SEL_MODE, CNT_SEL_PAUSE, CNT_SEL_RESET, CNT_SEL_EXIT};
        const u8 n2 = 4u;
        int i = -1;
        for (u8 k = 0; k < n2; k++) if (up_items[k] == g_cnt_sel) { i = (int)k; break; }
        if (i < 0) i = 0;
        i += delta;
        if (i < 0) i = (int)n2 - 1;
        if (i >= (int)n2) i = 0;
        g_cnt_sel = up_items[i];
      }
      else
      {
        static const u8 items[6] = {CNT_SEL_MODE, CNT_SEL_MIN, CNT_SEL_SEC,
                                    CNT_SEL_PAUSE, CNT_SEL_RESET, CNT_SEL_EXIT};
        const u8 n = 6u;
        int i = -1;
        for (u8 k = 0; k < n; k++) if (items[k] == g_cnt_sel) { i = (int)k; break; }
        if (i < 0) i = 0;
        i += delta;
        if (i < 0) i = (int)n - 1;
        if (i >= (int)n) i = 0;
        g_cnt_sel = items[i];
      }
    }
    return;
  }
  /* 确定 */
  if (ke->id == KEY_ID_2)
  {
    if (g_cnt_sel == CNT_SEL_MODE)       cnt_sel_mode_ok();
    else if (g_cnt_sel == CNT_SEL_PAUSE) cnt_sel_pause_ok();
    else if (g_cnt_sel == CNT_SEL_RESET) cnt_sel_reset();
    else if (g_cnt_sel == CNT_SEL_EXIT)  cnt_sel_exit();   /* 退出：回主屏，计时后台运行（2026-09-05） */
    else if (g_cnt_sel == CNT_SEL_MIN || g_cnt_sel == CNT_SEL_SEC)
    {
      if (!g_cnt_adj && g_cnt == CNT_RUN) g_cnt = CNT_PAUSE;  /* 进调整自动暂停 */
      g_cnt_adj = !g_cnt_adj;                                 /* 进出调整态（光标变色） */
    }
    return;
  }
}

/* 倒计时屏每帧增量绘制 */
static void draw_cnt(TimeStruct *t)
{
  (void)t;
  /* 顶部标签内标题：倒计时/正计时（坑#88：18x22 黑字画在粉色标签 x10-75 y9-48 内居中；
     清旧=恢复背景（UI_DrawGlyphRect 0xFFFF，标签内是纯粉渐变非纯色）；g_up 变化重画） */
  if (g_up != c_cnt_title)
  {
    UI_DrawGlyphRect(CNT_TITLE_X, 239 - (CNT_TITLE_Y + 22) + 1, 54, 22, 0xFFFFu, 0);
    UI_DrawTextInk(CNT_TITLE_X, CNT_TITLE_Y, g_up ? "正计时" : "倒计时", UI_SLOT_CHAR, SC_C_CNT_INK);
    c_cnt_title = g_up;
  }
  /* 大数字 MMM:SS（坑#88：20x40 黑 0xE100+d + 12x40 冒号 0xE304，居中 y69..108；
     坑#93（2026-11-12）：分钟 3 位支持 120 分钟，但【百位为 0 不显示】（用户实测"000:00 丑"）——
     分钟<100 用 2 位布局（92 宽居中 x160：分十 114 分个 134 冒号 154 秒十 166 秒个 186，坑#88 原布局），
     分钟≥100 用 3 位布局（112 宽居中：分百 104 分十 124 分个 144 冒号 164 秒十 176 秒个 196）；
     位数切换（99:59↔100:00）恢复 112 宽数字区背景 + 冒号画新位置 + 全量重画。 */
  u32 disp = g_up ? g_up_sec : (u32)g_cnt_remain;
  u8 m3 = (u8)(disp / 6000);               /* 分钟百位（分钟=disp/60，百位=disp/6000；120 分 -> 1；<100 为 0） */
  u8 m2 = (u8)((disp / 60) / 10 % 10);     /* 分钟十位 */
  u8 m1 = (u8)((disp / 60) % 10);          /* 分钟个位 */
  u8 s1 = (u8)((disp % 60) / 10), s2 = (u8)(disp % 10);
  int w3 = (m3 != 0);                      /* 1=3 位布局（分钟≥100，百位显示） */
  if (w3 != c_cnt_w3)
  {
    /* 位数切换（含首帧 0xFF）：恢复整个数字区（112 宽 40 高）背景 + 画新位置冒号 + 强制全画 */
    UI_DrawGlyphRect(CNT_DIG_LEFT, 239 - (CNT_DIG_Y + 40) + 1, 112, 40, 0xFFFFu, 0);
    draw_cnt_colon_at(w3 ? CNT_COL_LEFT : 154, CNT_DIG_Y);
    c_cnt_m3 = c_cnt_m2 = c_cnt_m1 = c_cnt_s1 = c_cnt_s2 = 0xFF;
    c_cnt_w3 = (u8)w3;
  }
  if (w3)
  {
    if (m3 != c_cnt_m3) { draw_cnt_digit_at(CNT_DIG_LEFT,                  CNT_DIG_Y, m3); c_cnt_m3 = m3; }
    if (m2 != c_cnt_m2) { draw_cnt_digit_at(CNT_DIG_LEFT + CNT_DIG_STEP,   CNT_DIG_Y, m2); c_cnt_m2 = m2; }
    if (m1 != c_cnt_m1) { draw_cnt_digit_at(CNT_DIG_LEFT + CNT_DIG_STEP*2, CNT_DIG_Y, m1); c_cnt_m1 = m1; }
    if (s1 != c_cnt_s1) { draw_cnt_digit_at(CNT_DIG_LEFT + 72,             CNT_DIG_Y, s1); c_cnt_s1 = s1; }  /* 冒号 164 后 12px = 176 */
    if (s2 != c_cnt_s2) { draw_cnt_digit_at(CNT_DIG_LEFT + 92,             CNT_DIG_Y, s2); c_cnt_s2 = s2; }  /* 176+20 = 196 */
  }
  else
  {
    if (m2 != c_cnt_m2) { draw_cnt_digit_at(CNT_DIG_LEFT + 10,             CNT_DIG_Y, m2); c_cnt_m2 = m2; }  /* 分十 114 */
    if (m1 != c_cnt_m1) { draw_cnt_digit_at(CNT_DIG_LEFT + 30,             CNT_DIG_Y, m1); c_cnt_m1 = m1; }  /* 分个 134 */
    if (s1 != c_cnt_s1) { draw_cnt_digit_at(CNT_DIG_LEFT + 62,             CNT_DIG_Y, s1); c_cnt_s1 = s1; }  /* 秒十 166 */
    if (s2 != c_cnt_s2) { draw_cnt_digit_at(CNT_DIG_LEFT + 82,             CNT_DIG_Y, s2); c_cnt_s2 = s2; }  /* 秒个 186 */
  }

  /* 按钮1「开始/暂停」切换（坑#88：背景绿按钮已画「开始 START」；运行/暂停时**整块贴图**
     ALM2 item2 = 从暂停态效果图 (2).png 截的「暂停 PAUSE」按钮 69x42，覆盖「开始 START」；
     结束/待启动恢复背景露出「开始 START」。2026-10-12 用户定：暂停显示不自己画文字，整块替换。
     删旧状态chip——新背景无 chip 位置。缓存 c_cnt_state 语义存 g_cnt 态。 */
  if (g_cnt != c_cnt_state)
  {
    if (g_cnt == CNT_RUN || g_cnt == CNT_PAUSE)
    {
      UI_DrawAlm2(CNT_BTN1_ITEM, CNT_BTN1_X, CNT_BTN1_Y);   /* 整块贴「暂停 PAUSE」按钮 */
    }
    else
    {
      UI_DrawGlyphRect(CNT_BTN1_X, 239 - (CNT_BTN1_Y + CNT_BTN1_H) + 1,
                       CNT_BTN1_W, CNT_BTN1_H, 0xFFFFu, 0);   /* 恢复背景 -> 露出「开始 START」 */
    }
    c_cnt_state = (u8)g_cnt;
  }

  /* 进度条（坑#88：轨道 x57-261 y119-146 在背景，填充 x57-260 y122-143 h22；按状态变色
     运行橙 #FFB20E / 暂停黄 SC_C_YELLOW / 结束红 SC_C_RED。2026-10-12 改胶囊圆头填充
     cnt_prog_fill 逐行水平段，替代直角 FillRect——方角盖出轨道两端半圆=用户反馈「两侧超界」；
     2026-10-12 再修「没走到头」：CNT_PROG_MAX 130→204（轨道实测 x57-261，满态贴右圆头 x260，
     原 130 只到 x186=轨道的 63%，用户看到走一大半就结束）。
     增长/换色=整条胶囊重画（不透明覆盖无闪烁），缩短=尾段恢复背景（UI_DrawGlyphRect 0xFFFF，
     轨道槽是渐变非纯色）再整条重画） */
  int filled = 0;
  u16 bar = SC_C_CNT_ORANGE;
  if (!g_up && g_cnt_total > 0)
  {
    /* 坑#93（2026-11-04）：进度条方向修正——原 (total-remain) 随时间【增加】（用户实测
       "应该随着计时慢慢消失"）；改 filled = remain/total（剩余比例），随时间减少=消失。
       缩短逻辑（下方 filled < c_cnt_prog 恢复背景尾段）正好适配。 */
    filled = (int)((u32)g_cnt_remain * CNT_PROG_MAX / g_cnt_total);
    bar = (g_cnt == CNT_PAUSE) ? SC_C_YELLOW : (g_cnt == CNT_DONE ? SC_C_RED : SC_C_CNT_ORANGE);
  }
  if (filled != c_cnt_prog || bar != c_prog_bar)
  {
    if (filled < c_cnt_prog && c_cnt_prog >= 0)
    {
      /* 缩短（重置/新倒计时）：旧填充超出新 filled 的尾段恢复背景 */
      UI_DrawGlyphRect(CNT_PROG_X + filled, 239 - (CNT_PROG_Y + CNT_PROG_H) + 1,
                       c_cnt_prog - filled, CNT_PROG_H, 0xFFFFu, 0);
    }
    if (filled > 0) cnt_prog_fill(filled, bar);   /* 整条胶囊重画（首帧/增长/换色/缩短统一走这） */
    c_cnt_prog = filled;
    c_prog_bar = bar;
  }

  cnt_sel_line();   /* 选择光标常驻（切项/进出调整/背景重传时清旧画新，2026-09-05） */
}

/* ==================== 闹钟 ==================== */
/* 0x8A 设置闹钟：n=闹钟号(1-3) hh/mm=时间。写槽内存 + 设当前显示槽 + 切屏2（2026-09-05 3 槽） */
void Screen_AlarmSet(u8 n, u8 hh, u8 mm)
{
  u8 idx = (n >= 1 && n <= 3) ? (u8)(n - 1) : 0;
  g_alm_hour[idx] = hh;
  g_alm_min[idx]  = mm;
  g_alm_on[idx]   = 1;                  /* 设闹钟即开启该槽 */
  g_alm_sel       = idx;                /* 切屏显示该槽 */
  g_alm.rung = 0;
  alm_snooze_clear();                   /* 重设闹钟时间：取消可能待响的贪睡（坑#71 #9） */
  if (g_alm.ringing)
  {
    g_alm.ringing = 0;
    Voice_Send(VOICE_TX_RING, g_alm.ring_sel, 0, 0, 0);
    (void)UI_CharAnim_SetState(UI_CANI_STATE_IDLE);   /* 响铃中改时间：停响回待机（2026-09-04 闹铃动画） */
  }
  /* 已在闹钟屏则不重传背景：配置改动由 draw_alarm 增量刷新（大数字变化即重画），只有从别的屏切过来才整屏切换（2026-08-28） */
  if (UI_GetScreen() != UI_SCREEN_ALARM) UI_SetScreen(UI_SCREEN_ALARM);
  g_alm_idle_ms = (u32)bftm0_ct;   /* 0x8A 设置闹钟：刷新闹钟屏空闲起点（切屏由 Screen_Draw 也刷；同屏兜底） */
}

/* 0x8B 打开/关闭闹钟、0x8C 循环模式：2026-09-05 起由 Screen_HandleVoice 内联按 d0/d1/d2
   三槽解析（协议表 XX/YY/MM 每字节对应闹钟1/2/3），本函数已删除（无调用者）。
   0x8B 关闭响铃槽的停铃逻辑内联在 Screen_HandleVoice 0x8B 分支。 */

void Screen_AlarmRingStop(void)
{
  if (g_cnt == CNT_DONE) { g_cnt = CNT_STOP; g_cnt_ret_ms = 0; }  /* 清倒计时结束残留，防 60s 后再触发一次回主屏 */
  if (g_alm.ringing)
  {
    g_alm.ringing = 0;
    Voice_Send(VOICE_TX_RING, g_alm.ring_sel, 0, 0, 0);   /* 停播铃声 */
    (void)UI_CharAnim_SetState(UI_CANI_STATE_IDLE);       /* 停响回待机（2026-09-04 闹铃动画结束） */
  }
  alm_snooze_clear();   /* 0xB1 别响了/60s 自动停：取消可能待响的贪睡（坑#71 #9） */
  goto_main();   /* 内部已停倒计时响铃 */
}

/* 改时间/日期后清除"本分钟已响"标志（坑#82 2026-09-05）：用户主动改时间（语音
   0x81/0x82、按键设置退出、蓝牙授时）把时间回拨后，闹钟到点应重新判定——否则
   g_alm.rung=1 残留（响铃/贪睡打断后不清），回拨后同一分钟再次到达被
   `if (g_alm.rung && g_alm.rung_min == cur_min) return;` 挡掉不响（实测：13:01 响闹
   ->贪睡或按键打断->改时间回拨到 13:00->13:01 到点不再响闹）。
   坑#90 分钟绑定不替代本函数：时间回拨后 rung_min 与重新到达的分钟仍相同，仍需清零。 */
void Screen_AlarmRungReset(void)
{
  g_alm.rung = 0;
  alm_snooze_clear();   /* 坑#93（2026-11-04）：语音改时间/日期也取消贪睡待响——贪睡依附闹钟时刻，时间变了不该再重响（用户实测"贪睡期间语音修改时间，贪睡未被打断"） */
}

/* 闹钟每秒判定（Screen_OnSecond 调）：遍历 3 个闹钟槽，任一命中响铃并记录响铃槽 */
static void alarm_tick(TimeStruct *t)
{
  u16 cur_min = (u16)(t->hour * 60u + t->minute);   /* 坑#90：rung 按分钟绑定，跨分钟自动失效 */
  if (g_alm.ringing)
  {
    if ((u32)(bftm0_ct - g_alm.ring_start) >= 60000u)
    {
      /* 坑#90：响满 60s 自动停后【不 return】落到下方分钟判定——前一闹钟恰在分钟边界
         （如 07:00 响到 07:01:00）时，相邻分钟的下一闹钟到点才不会被吞掉。
         Screen_AlarmRingStop 已回主屏；若本分钟有槽命中会在下方重新响铃切回闹钟屏。 */
      Screen_AlarmRingStop();
    }
    else
      return;
  }
  /* 坑#79：设置模式（时间设置/倒计时分秒调整/闹钟设置）不触发任何响闹——
     否则时间设置（主屏）中闹钟到点照样响（UI 仍是 MAIN 判定通过）；
     闹钟设置中到点若判定，设置完回主屏会补响。贪睡重响一并挡（会切屏打断设置）。
     响铃中本就进不了设置（响铃任意键停/响闹语音拦截），ringing 分支在前不受影响。 */
  if (g_set_active || g_alm_set_active || g_cnt_adj) return;
  /* 贪睡待响：到点一次性重响（配置不变，响完即结束——坑#71 #9 一次性延迟语义） */
  if (g_alm.snooze)
  {
    if ((u32)(bftm0_ct - g_alm_snooze_ms) < 0x80000000u)   /* 时间到（unsigned 回绕安全） */
    {
      u8 i = (g_alm_ring_idx < 3) ? g_alm_ring_idx : g_alm_sel;
      alm_snooze_clear();
      if (!g_alm_on[i]) return;              /* 防御：贪睡槽已被关（正常路径 0x8B 已清） */
      printf("[VOICE] 贪睡到点：闹钟%u 重响一次\r\n", (unsigned)i + 1);
      g_alm.ringing    = 1;
      g_alm.ring_start = (u32)bftm0_ct;
      g_alm.rung       = 1;
      g_alm.rung_min   = cur_min;        /* 坑#90：记分钟戳，防同分钟正常判定重复触发 */
      disp_wake();                       /* 响铃自动亮屏（显示若已关则开，坑#71 #15） */
      (void)UI_CharAnim_SetState(UI_CANI_STATE_ALARM);
      if (UI_GetScreen() != UI_SCREEN_ALARM) UI_SetScreen(UI_SCREEN_ALARM);
      Voice_Send(VOICE_TX_RING, g_alm.ring_sel, 1, 0, 0);
      return;
    }
    /* 坑#93（2026-11-04）：贪睡未到点【不 return】——落到下方分钟判定，贪睡期间
       其他闹钟到点照常响（用户实测"一个闹钟贪睡期间另外的闹钟不会响闹"）；
       贪睡重响与普通判定同分钟撞车时先到先响，另一条 rung 同分不重响 */
  }
  /* 坑#77 #3：闹钟只在「分钟进位」（秒=0）那一秒判定——否则闹钟时间=当前时间时，
     回主屏后当前分钟任一秒都命中误响（用户实测：0:00 设闹钟 0:00，设置完自动回
     时间显示后闹钟立即响闹）。改进位判定后：设置完成回主屏若已过进位点，本次不判，
     下一分钟不匹配则整天不再响（rung 自然清零）。 */
  if (t->second != 0) return;
  for (u8 i = 0; i < 3; i++)
  {
    if (!g_alm_on[i]) continue;                                        /* 槽关 */
    if (t->hour != g_alm_hour[i] || t->minute != g_alm_min[i]) continue;  /* 时间不匹配：下一槽 */
    if (g_alm.rung && g_alm.rung_min == cur_min) return;   /* 本分钟已响过：整分钟不再判（跨分钟 rung_min 不同则失效，坑#90） */
    if (!weekday_match(g_alm_loop[i])) continue;           /* 今天不匹配该槽循环：跳过本槽继续下一槽（原 return 挡后续槽，坑#90） */
    /* 坑#93（2026-11-04）：删除"只在主屏触发闹钟"守卫——倒计时/闹钟屏上闹钟到点照常响
       （响铃切闹钟屏；倒计时后台继续跑，到点再切回倒计时屏响铃，用户实测"倒计时进行中闹钟不会响闹"） */
    /* 到点响铃：切屏2（已在闹钟屏则只播铃不重传背景）+ 播铃声 + 人物切换闹铃动画 */
    g_alm_ring_idx = i;
    g_alm.ringing    = 1;
    g_alm.ring_start = (u32)bftm0_ct;
    g_alm.rung       = 1;
    g_alm.rung_min   = cur_min;      /* 坑#90：记分钟戳 */
    disp_wake();                       /* 响铃自动亮屏（显示若已关则开，坑#71 #15） */
    (void)UI_CharAnim_SetState(UI_CANI_STATE_ALARM);   /* 闹铃动画：走过渡后进入 alarm 循环（2026-09-04 接线） */
    if (UI_GetScreen() != UI_SCREEN_ALARM) UI_SetScreen(UI_SCREEN_ALARM);
    Voice_Send(VOICE_TX_RING, g_alm.ring_sel, 1, 0, 0);   /* 播放铃声 */
    return;
  }
  g_alm.rung = 0;   /* 当前分钟无任何槽命中：允许后续重新判定 */
}

/* 闹钟屏每帧增量绘制：显示目标槽（响铃中=响铃槽，否则当前选中槽 g_alm_sel） */
static void draw_alarm(TimeStruct *t)
{
  (void)t;
  u8 idx = g_alm.ringing ? g_alm_ring_idx : g_alm_sel;
  u8 hh = g_alm_hour[idx], mm = g_alm_min[idx];

  /* 大数字 HH:MM（当前槽闹钟时间；坑#85 深棕 40x84 居中主卡 x14-195：
     组 40*4+14=174 中心 104.5 -> 左 17，冒号 97；y78-162 顶原点） */
  u8 h1 = hh / 10, h2 = hh % 10, m1 = mm / 10, m2 = mm % 10;
  if (h1 != c_alm_h1) { draw_digit_at(SC_ALM_DIG_LEFT,          SC_ALM_Y, h1, SC_C_MAIN_INK); c_alm_h1 = h1; }
  if (h2 != c_alm_h2) { draw_digit_at(SC_ALM_DIG_LEFT + 40,     SC_ALM_Y, h2, SC_C_MAIN_INK); c_alm_h2 = h2; }
  if (m1 != c_alm_m1) { draw_digit_at(SC_ALM_DIG_LEFT + 94,     SC_ALM_Y, m1, SC_C_MAIN_INK); c_alm_m1 = m1; }
  if (m2 != c_alm_m2) { draw_digit_at(SC_ALM_DIG_LEFT + 134,    SC_ALM_Y, m2, SC_C_MAIN_INK); c_alm_m2 = m2; }

  /* 坑#85：左上「闹钟N」chip 移除（背景被小熊插画占据）。坑#86 序号改画到小熊怀里粉条
     （下方「闹钟N」块，由 c_alm_sel 缓存驱动：槽号/响铃槽变化即重画）。 */

  /* 开关：坑#85 从效果图截取 ON 控件存 ALM2 item0（68x33 整块含卡片背景），
     贴图整块覆盖背景自带 OFF 控件。坑#91（2026-10-30）：客户反馈开关效果不明显——
     旧关闭态=恢复背景（背景自带的粉红槽+滑块靠左），与开启态（粉红槽+滑块靠右）差异太小；
     客户新图 320x239-(3).png 将关闭态改为【空槽】样式（米色轨道无填充+滑块靠左+红心），
     从新图截取 OFF 控件存 ALM2 item3（68x33 @x228 y72，与 item0 同位置同尺寸），
     OFF 状态由「恢复背景」改为贴 item3（开关效果更明显）。 */
  u8 on = g_alm_on[idx] ? 1 : 0;
  if (on != c_alm_on)
  {
    if (on) UI_DrawAlm2(0, 228, 72);                                          /* 贴 ON 控件（粉红槽+滑块右） */
    else    UI_DrawAlm2(3, 228, 72);                                          /* 坑#91：贴 OFF 控件（空槽+滑块左） */
    c_alm_on = on;
  }

  /* 星期循环：坑#85 选中=保留背景彩色瓶、未选中=贴灰白瓶身（ALM2 item1 17x27 覆盖彩瓶色块）。
     坑#86 修正：a) 素材=瓶6 灰白瓶身【色块区】（源 x144-160 y181-207，17 宽），贴 x=24+i*24
     （素材中心=彩瓶中心 32.5+i*24）正好覆盖彩瓶色块、保留彩瓶轮廓 -> 未选瓶与彩瓶同宽同轮廓
     （26 宽完整瓶模板会因效果图各瓶手绘宽度不同而超出 -> 「星期天瓶子右边还有瓶子残留」；
     30x30 含两瓶 -> 3 个灰瓶）。b) 切换循环【不分段恢复整段背景】（会先亮彩色再贴灰白=闪烁）：
     逐瓶判断——只对「上一轮灰白、本轮变彩」的瓶恢复背景（目标就是彩，无中间态），只对
     「变灰」的瓶贴灰白，其余保持；首帧 c_alm_loop=0xFF 视作全彩(0) -> 未选瓶全部贴灰白。 */
  if (g_alm_loop[idx] != c_alm_loop)
  {
    u8 old = (c_alm_loop == 0xFF) ? 0u : c_alm_loop;   /* 首帧视作全彩(0)，未选瓶全部贴灰白 */
    u8 now = g_alm_loop[idx];
    for (int i = 0; i < 7; i++)
    {
      int was = (old == 1) ? (i > 4) : ((old == 2) ? (i > 5) : 0);   /* 上一轮 i 是否灰白 */
      int is  = (now == 1) ? (i > 4) : ((now == 2) ? (i > 5) : 0);   /* 本轮 i 是否灰白 */
      if (was && !is)      UI_DrawGlyphRect(23 + i * 24, 239 - (181 + 27) + 1, 20, 27, 0xFFFFu, 0);  /* 变彩：清灰白恢复背景（20 宽覆盖素材+轮廓边缘） */
      else if (!was && is) UI_DrawAlm2(1, 24 + i * 24, 181);                                        /* 变灰：贴灰白瓶身 */
    }
    c_alm_loop = g_alm_loop[idx];
  }

  /* 坑#86 闹钟序号「闹钟N」：小熊怀里粉条（x58-139 y58-76）即预留的序号空间——效果图1 在
     粉条上画 "ALARM 1" 深棕小字，静态版背景粉条留空 -> 固件补画。字库无英文字母
     （MAX_FONT_IDX=84 满），改画「闹钟」18x22 + 序号数字 0xE000+d 16x22（深棕 SC_C_MAIN_INK，
     与设备中文风格一致），居中于粉条文字区（中心 ~107/67）；槽号变化清旧（恢复背景）重画。 */
  if (c_alm_sel != idx)
  {
    if (c_alm_sel != 0xFF)
      UI_DrawGlyphRect(80, 239 - (57 + 22) + 1, 54, 22, 0xFFFFu, 0);   /* 清旧：恢复背景 */
    UI_DrawTextInk(81, 57, "闹钟", UI_SLOT_WEEK, SC_C_MAIN_INK);        /* 汉字 18x22 x81-116 */
    UI_DrawGlyphRect(117, 239 - (57 + 22) + 1, 16, 22, 0xE001u + idx, SC_C_MAIN_INK); /* 序号 1/2/3 */
    c_alm_sel = idx;
  }

  /* 强调线：正常蓝 / 响铃红（停用） */
 /* if (g_alm.ringing != c_alm_ring)
  {
    Display_FillRect(16, 170, 289, 2, g_alm.ringing ? SC_C_RED : SC_C_BLUE);
    c_alm_ring = g_alm.ringing;
  }*/

  alm_set_line();   /* 闹钟设置黄线（仅设置态画；切项/背景重传时清旧画新，2026-09-05） */
}

/* ==================== 屏静态元素（进入屏时画一次） ==================== */

static void draw_screen_static(int scr)
{
  if (scr == UI_SCREEN_CNT)
  {
    /* 坑#88：背景（ui_screen1.png 奶油风）已含按钮/装饰/进度条轨道/标签，静态只画冒号（12x40 黑）。
       坑#93：冒号位置随分钟位数——分钟<100（百位为 0 不显示）2 位布局冒号 154，≥100 3 位布局冒号 164 */
    draw_cnt_colon_at((g_cnt_remain / 6000) ? CNT_COL_LEFT : 154, CNT_DIG_Y);
    c_cnt_m3 = c_cnt_m2 = c_cnt_m1 = c_cnt_s1 = c_cnt_s2 = 0xFF;
    c_cnt_w3 = 0xFF;   /* 坑#93：强制首帧按当前分钟位数重画（含冒号/数字布局） */
    if (c_cnt_prog > 0)   /* 强制全绘未重传背景（模式切换 g_ui_bg_ver++ / 开显示）：旧进度条填充仍在屏上，先恢复背景（2026-10-12） */
      UI_DrawGlyphRect(CNT_PROG_X, 239 - (CNT_PROG_Y + CNT_PROG_H) + 1, c_cnt_prog, CNT_PROG_H, 0xFFFFu, 0);
    c_cnt_state = 0xFF; c_cnt_prog = -1; c_prog_bar = 0;
    c_cnt_title = 0xFF;   /* 背景重传后强制重画顶部标题（倒计时/正计时，坑#88 标签内） */
  }
  else if (scr == UI_SCREEN_ALARM)
  {
    draw_colon_at(SC_ALM_COL_LEFT, SC_ALM_Y, SC_C_MAIN_INK);   /* 坑#85 闹钟屏：冒号 97 深棕 */
    c_alm_h1 = c_alm_h2 = c_alm_m1 = c_alm_m2 = 0xFF;
    c_alm_on = 0xFF; c_alm_loop = 0xFF; c_alm_ring = 0xFF;
    c_alm_sel = 0xFF;   /* 强制重画「闹钟N」序号（坑#86 粉条序号由本缓存驱动） */
  }
}

/* ==================== 对外 API ==================== */

void Screen_Init(void)
{
  const ui_cfg_t *cfg = UI_GetConfig();
  g_alm.loop     = 0;
  g_alm.ring_sel = 0;
  g_alm.ringing  = 0;
  g_alm.rung     = 0;
  g_alm.snooze   = 0;
  g_up      = 0;
  g_up_sec  = 0;
  g_cnt_ringing = 0;
  g_hourly_on   = 0;
  g_last_hourly = 0xFF;
  /* 3 闹钟槽（2026-09-05）：槽1 取 Flash 配置初始值，槽2/3 默认 7:00 关。
     循环默认 0（单次=每天响）：星期chips 全高亮（draw_alarm 中 0 与 3 同全亮），
     上电即可见循环状态（用户 2026-09-05 定：默认 0，每天响显示 1~7 全高亮） */
  g_alm_hour[0] = cfg->alarm_hour;
  g_alm_min[0]  = cfg->alarm_min;
  g_alm_on[0]   = cfg->alarm_on ? 1 : 0;
  g_alm_hour[1] = 7; g_alm_min[1] = 0; g_alm_on[1] = 0;
  g_alm_hour[2] = 7; g_alm_min[2] = 0; g_alm_on[2] = 0;
  for (u8 i = 0; i < 3; i++) g_alm_loop[i] = 0;
  g_alm_sel      = 0;
  g_alm_ring_idx = 0;
  /* 倒计时屏选择 / 闹钟设置 初始态 */
  g_cnt_sel = CNT_SEL_MODE; g_cnt_adj = 0; g_cnt_sel_line = -1;   /* 默认选中模式项（2026-09-05） */
  g_alm_set_active = 0; g_alm_set_item = 0; g_alm_set_line = -1;
  /* 沙漏闪烁缓存初始非法：首帧必画/擦（2026-09-05） */
  c_cnt_blink_on = 0xFF; c_cnt_blink_act = 0xFF; c_cnt_blink_drawn = 0;
  (void)cfg;
  c_drawn_screen = 0xFF;
  goto_main();
}

/* 每帧：按当前屏增量绘制 */
void Screen_Draw(TimeStruct *t)
{
  int scr = UI_GetScreen();
  /* 切屏 或 背景被重新上传（UI_SetScreen 重传同屏背景，如闹钟设置后再切）都强制全绘，
     否则增量缓存认为"没变化"不重绘 -> 重传后数字/星期/开关等全部消失（2026-08-28 修复）。 */
  if (scr != c_drawn_screen || g_ui_bg_ver != c_bg_ver)
  {
    c_drawn_screen = (u8)scr;
    c_bg_ver = g_ui_bg_ver;
    draw_screen_static(scr);
    /* 闹钟屏空闲超时计时（2026-09-05）：进闹钟屏刷新起点（任何入口：语音/按键长按/命令）；
       离开闹钟屏清 0（防残留）。判断在 Screen_OnSecond（例外=设置态/响铃）。 */
    g_alm_idle_ms = (scr == UI_SCREEN_ALARM) ? (u32)bftm0_ct : 0;
    /* 倒计时屏空闲计时（坑#71）：进倒计时屏刷新起点（离开保持原值，回主屏 goto_main 重起） */
    if (scr == UI_SCREEN_CNT) g_cnt_idle_ms = (u32)bftm0_ct;
  }
  if (scr == UI_SCREEN_CNT)   draw_cnt(t);
  else if (scr == UI_SCREEN_ALARM) draw_alarm(t);
  /* 主屏（MAIN）由 calendar.c 负责，不在此绘制 */

  /* 倒计时 DONE 回主屏延时到点 */
  if (g_cnt == CNT_DONE && g_cnt_ret_ms && (u32)(bftm0_ct - g_cnt_ret_ms) < 0x80000000u)
  {
    g_cnt = CNT_STOP; g_cnt_ret_ms = 0;
    goto_main();
  }
}

/* 每秒：倒计时/正计时推进 + 闹钟判定 + 整点报时 + 设置模式超时退出 */
void Screen_OnSecond(TimeStruct *t)
{
  /* 坑#71（倒计时与闹钟同时到达 → 优先闹钟）：先判闹钟后判倒计时——
     同秒两者都到点时闹钟先响（g_alm.ringing=1 + 切闹钟屏），cnt_tick 看到闹钟响铃
     即让倒计时静默结束（不播铃/不切屏）。原顺序 cnt_tick 先会把屏抢成倒计时屏，
     闹钟被"只在主屏触发"挡住，等回主屏才补响。 */
  alarm_tick(t);
  cnt_tick();
  /* 倒计时到点响铃：若响铃瞬间正处设置态被拦住未切倒计时屏（cnt_tick 只判定一次），
     设置退出后每秒补切一次到倒计时屏，保证任意键可打断（坑#70 补充，2026-09-05）。 */
  if (g_cnt_ringing && UI_GetScreen() != UI_SCREEN_CNT &&
      !g_alm.ringing && !g_alm_set_active && !g_set_active)
  {
    UI_SetScreen(UI_SCREEN_CNT);
    g_cnt_idle_ms = (u32)bftm0_ct;   /* 坑#83：补切倒计时屏立即刷新空闲计时 */
  }
  /* 主屏时间设置 15s 无按键自动退出（写回时间） */
  if (g_set_active && (u32)(bftm0_ct - g_set_last_act) >= 15000u)
    set_exit();
  /* 闹钟设置 15s 无按键自动退出设置（仅退设置态，仍停闹钟屏；退出后 3s 再自动回主屏） */
  if (g_alm_set_active && (u32)(bftm0_ct - g_alm_set_last_act) >= 15000u)
    alm_set_exit();
  /* 闹钟设置退出后 3s 自动回主屏（看清设置结果再回；响铃中不回，待 60s 自动停或确定停） */
  if (!g_alm_set_active && !g_alm.ringing && g_alm_set_exit_ms &&
      (u32)(bftm0_ct - g_alm_set_exit_ms) >= 3000u)
  {
    g_alm_set_exit_ms = 0;
    goto_main();
  }
  /* 闹钟屏空闲超时自动回主屏（常态判断，2026-09-05）：
     进入闹钟屏（语音/按键/命令）后无任何操作超 15s 自动回主屏；
     例外 = 设置态(g_alm_set_active) / 响铃与语音播报中(g_alm.ringing)。
     原仅依赖语音模块 0xEF（15s 无命令）触发——按键长按进入闹钟屏时模块
     无命令不发 0xEF，导致闹钟屏回不去（用户反馈）。现固件自带常态判断兜底。 */
  if (UI_GetScreen() == UI_SCREEN_ALARM && !g_alm_set_active && !g_alm.ringing &&
      g_alm_idle_ms && (u32)(bftm0_ct - g_alm_idle_ms) >= 15000u)
  {
    g_alm_idle_ms = 0;
    printf("[KEY] 闹钟屏空闲 15s 自动回主屏\r\n");
    goto_main();
  }
  /* 倒计时屏未运行空闲 15s 自动回主屏（坑#71 #11/#12 2026-09-05）：
     暂停/待启动/分/秒调整态（g_cnt != RUN）空闲超时自动回主屏（用户：要能自动返回时间显示）；
     运行中不回（须一直显示倒计时）；响铃中不回（走 60s 自动停或任意键打断）。 */
  if (UI_GetScreen() == UI_SCREEN_CNT && g_cnt != CNT_RUN && !g_cnt_ringing &&
      g_cnt_idle_ms && (u32)(bftm0_ct - g_cnt_idle_ms) >= 15000u)
  {
    g_cnt_idle_ms = 0;
    printf("[KEY] 倒计时屏空闲 15s 自动回主屏（未运行）\r\n");
    goto_main();
  }
  /* 整点报时（0xB8 开关）：整点（分=0 秒=0）且小时变化时，用 0x07 播"现在是 X点0分"一次。
     坑#77 #2：响铃中（闹钟/倒计时）不播报时——模块单声道独占播放，报时会把响铃打断
     （用户实测：13:00 闹钟 + 整点报时，报时先播一半被闹铃打断；需求=只播闹铃）。
     跳过本次报时后 g_last_hourly 不更新，下个整点正常播报。 */
  if (g_hourly_on && !g_alm.ringing && !g_cnt_ringing &&
      t->minute == 0 && t->second == 0 && t->hour != g_last_hourly)
  {
    g_last_hourly = t->hour;
    Voice_Send(VOICE_TX_TELL_TIME, t->hour, 0, 0, 0);
  }
}

/* 语音模块 0xEF（15s 无命令）等超时回主屏——常态判断：默认回主屏，
   例外（设置状态/语音状态/倒计时活动屏）不回：
   - 倒计时屏活动（运行/暂停中）：不切走（倒计时屏须主动「退出」按钮离开，且不打断分/秒调整态）
   - 闹钟设置态 g_alm_set_active：不打断按键设置（0xEF 不应打断设置）
   - 响铃/语音播报中 g_alm.ringing：不打断播铃（60s 自动停或任意键停）
   已在主屏则直接返回：不重传主屏背景（重传会整屏逐行覆盖、中间出现混屏"错误一帧"，2026-08-28）。
   备注：按键进入闹钟屏的空闲自动返回由固件常态判断兜底（Screen_OnSecond，坑#68），不依赖 0xEF。 */
void Screen_ReturnMain(void)
{
  if (UI_GetScreen() == UI_SCREEN_CNT)
  {
    if (Screen_CntActive()) return;   /* 运行/暂停不回 */
    /* 坑#80：倒计时屏未运行（待启动/结束）时，进屏 15s 内不被 0xEF 踢回——否则主屏
       停留 >15s 后按键进倒计时屏，模块 0xEF(15s 无命令) 紧跟到达，本函数无守卫直接
       goto_main，表现为"进倒计时屏马上返回时间模式"。与固件 Screen_OnSecond 的
       g_cnt_idle_ms 15s 空闲自动回保持同一节奏（到时谁先到谁回）。 */
    if (g_cnt_idle_ms && (u32)(bftm0_ct - g_cnt_idle_ms) < 15000u) return;
  }
  if (UI_GetScreen() == UI_SCREEN_ALARM && g_alm_idle_ms &&
      (u32)(bftm0_ct - g_alm_idle_ms) < 15000u) return;   /* 坑#83：闹钟屏进屏 15s 内不被 0xEF 踢回（同倒计时屏） */
  if (g_alm_set_active) return;                  /* 例外：闹钟设置态 */
  if (g_alm.ringing || g_cnt_ringing) return;  /* 例外：闹钟/倒计时响铃中（坑#76 不切走、不打断响闹） */
  if (UI_GetScreen() == UI_SCREEN_MAIN) return;
  goto_main();
}

/* 语音设时/设日成功强制回主屏（坑#77 #1 2026-09-05）：与 0xEF 超时（Screen_ReturnMain）
   的区别是【不受"倒计时屏进屏 15s 空闲"限制】——用户明确要求"设置完成后跳转到时间显示"；
   仍保留倒计时活动/手动设置/响铃保护（不打断）。 */
void Screen_GoMain(void)
{
  if (UI_GetScreen() == UI_SCREEN_CNT && Screen_CntActive()) return;  /* 倒计时运行/暂停不回 */
  if (g_alm_set_active || g_set_active) return;   /* 手动设置模式中语音改时间：留在设置（坑#71 #10） */
  if (g_alm.ringing || g_cnt_ringing) return;     /* 响铃中不回 */
  if (UI_GetScreen() == UI_SCREEN_MAIN) return;
  goto_main();
}

/* ==================== 语音命令分发（voice.c 调；返回 1=已处理 0=未处理 2=响闹中静默拦截） ==================== */
int Screen_HandleVoice(const VoiceFrame *f)
{
  /* 响闹中语音拦截（坑#76 2026-09-05 用户需求「响闹状态只回应特定指令」）：
     倒计时/闹钟响铃中，只放行——
       0xB1 别响了（停铃，倒计时/闹钟响铃都行）；
       0xB0 贪睡（仅闹钟响铃，倒计时响闹不支持贪睡）。
     其它命令一律静默拦截（返回 2：不执行、不回 OK/FAIL、不播任何提示音），
     否则模块播"设置失败"等提示音会覆盖/打断正在播的铃声（用户实测：倒计时响闹中
     说"稍后提醒" -> 回设置失败 -> 响闹声没了）。 */
  if (g_cnt_ringing || g_alm.ringing)
  {
    u8 allow = (f->cmd == VOICE_CMD_ALARM_OFF) ||
               (f->cmd == VOICE_CMD_SNOOZE && g_alm.ringing);
    if (!allow)
    {
      printf("[VOICE] 响闹中静默拦截 cmd=0x%02X（仅响应 0xB1 别响了%s）\r\n",
             f->cmd, g_alm.ringing ? "/0xB0 贪睡" : "");
      return 2;
    }
  }
  switch (f->cmd)
  {
    case VOICE_CMD_CNT_MIN:    /* 0x84 分钟倒计时 d0=1-120（坑#93：新版指令集 V01 支持 1~120 分钟，原 1-60） */
      if (f->d0 >= 1 && f->d0 <= CNT_MIN_MAX) { Screen_CntStart((u16)f->d0 * 60u); return 1; }
      return 0;
    case VOICE_CMD_TOMATO_ON:  /* 0x88 番茄：自动 25 分钟 */
      Screen_CntStart(25u * 60u); return 1;
    case VOICE_CMD_PAUSE:      /* 0x85 */
      Screen_CntPause(); return 1;
    case VOICE_CMD_RESUME:     /* 0x86 */
      /* 坑#71 #8：结束/停止/待启动后"继续计时"应无效（只暂停态能继续）。
         return 0 -> 上层回"设置失败"，不误报"好的，设置成功"。 */
      if (g_cnt != CNT_PAUSE)
      {
        printf("[VOICE] 继续计时无效（当前非暂停态 g_cnt=%u）-> 回 FAIL\r\n", (unsigned)g_cnt);
        return 0;
      }
      Screen_CntResume(); return 1;
    case VOICE_CMD_STOP:       /* 0x87 */
    case VOICE_CMD_TOMATO_OFF: /* 0x89 */
      Screen_CntStop(); return 1;
    case VOICE_CMD_ALARM_SW:   /* 0x8B 打开/关闭闹钟：d0/d1/d2 = 闹钟1/2/3 状态（0xFF=不变，1=开，0=关，协议表）
                                  2026-09-05 修：原只解析 d0 且操作当前显示槽——打开闹钟2/3（状态在 d1/d2）
                                  被忽略（d0=0xFF 走"仅切屏"分支），打开闹钟1 也会误改当前显示槽。 */
    {
      u8 chg = 0xFF;                              /* 首个被显式设置的槽（切屏显示它，同 0xB9 选槽） */
      if (f->d0 != 0xFF) { g_alm_on[0] = (f->d0 == 1) ? 1 : 0; if (chg == 0xFF) chg = 0; }
      if (f->d1 != 0xFF) { g_alm_on[1] = (f->d1 == 1) ? 1 : 0; if (chg == 0xFF) chg = 1; }
      if (f->d2 != 0xFF) { g_alm_on[2] = (f->d2 == 1) ? 1 : 0; if (chg == 0xFF) chg = 2; }
      if (chg != 0xFF) g_alm_sel = chg;           /* 显示被操作的槽（draw_alarm 增量刷新开关chip） */
      /* 关闭了正在响铃的槽：停铃回待机（原 Screen_AlarmSwitch 停铃逻辑内联，2026-09-04 闹铃动画） */
      if (g_alm.ringing && !g_alm_on[g_alm_ring_idx])
      {
        g_alm.ringing = 0;
        Voice_Send(VOICE_TX_RING, g_alm.ring_sel, 0, 0, 0);
        (void)UI_CharAnim_SetState(UI_CANI_STATE_IDLE);
      }
      /* 关闭了贪睡待响的槽：取消贪睡（坑#71 #9：贪睡配置不变，槽关了自然不重响） */
      if (g_alm.snooze && g_alm_ring_idx < 3 && !g_alm_on[g_alm_ring_idx])
        alm_snooze_clear();
      g_alm.rung = 0;                             /* 开关变更后允许同分钟重新判定 */
      if (UI_GetScreen() != UI_SCREEN_ALARM) UI_SetScreen(UI_SCREEN_ALARM);
      g_alm_idle_ms = (u32)bftm0_ct;              /* 语音操作闹钟：刷新空闲起点（防 0xEF 后固件自动返回打断） */
      return 1;
    }
    case VOICE_CMD_ALARM_LOOP: /* 0x8C 闹钟循环模式：d0/d1/d2 = 闹钟1/2/3 循环（0单次/1一~五/2一~六/3一~日，协议表）
                                  2026-09-05 修：原只解析 d0 且操作当前显示槽（同 0x8B 问题）。 */
    {
      u8 chg = 0xFF;
      if (f->d0 != 0xFF) { g_alm_loop[0] = (f->d0 > 3) ? 0 : f->d0; if (chg == 0xFF) chg = 0; }
      if (f->d1 != 0xFF) { g_alm_loop[1] = (f->d1 > 3) ? 0 : f->d1; if (chg == 0xFF) chg = 1; }
      if (f->d2 != 0xFF) { g_alm_loop[2] = (f->d2 > 3) ? 0 : f->d2; if (chg == 0xFF) chg = 2; }
      if (chg != 0xFF) g_alm_sel = chg;           /* 显示被操作的槽（draw_alarm 增量刷新星期chips） */
      if (UI_GetScreen() != UI_SCREEN_ALARM) UI_SetScreen(UI_SCREEN_ALARM);
      g_alm_idle_ms = (u32)bftm0_ct;              /* 语音操作闹钟循环：刷新空闲起点 */
      return 1;
    }
    case VOICE_CMD_ALARM_RING: /* 0x8D 铃音选择 */
      g_alm.ring_sel = f->d0; return 1;
    case VOICE_CMD_SNOOZE:     /* 0xB0 贪睡/稍后提醒/再睡会儿（2026-09-05 重做：一次性延迟，配置不变，坑#71 #9/#14） */
    {
      /* 倒计时响闹中：贪睡【无效】——用户定"倒计时响闹不应被贪睡指令打断"（坑#71 #14）。
         return 0 -> 上层回"设置失败"，且不打断倒计时响铃。 */
      if (g_cnt_ringing)
      {
        printf("[VOICE] 贪睡：倒计时响闹中无效 -> 回 FAIL（不打断倒计时响铃）\r\n");
        return 0;
      }
      if (!g_alm.ringing)       /* 无闹铃可贪睡 */
      {
        printf("[VOICE] 贪睡：当前无响铃可贪睡 -> 回 FAIL\r\n");
        return 0;
      }
      /* 闹钟响铃中：贪睡 = 停响 + 一次性延迟 10 分钟重响（闹钟配置不变，
         贪睡待响由 alarm_tick 到点触发重响一次；重响完即结束，不再延迟） */
      printf("[VOICE] 贪睡：闹钟%u 延迟 10 分钟重响一次（配置不变）\r\n", (unsigned)g_alm_ring_idx + 1);
      g_alm.ringing = 0;
      Voice_Send(VOICE_TX_RING, g_alm.ring_sel, 0, 0, 0);
      (void)UI_CharAnim_SetState(UI_CANI_STATE_IDLE);   /* 贪睡：停响回待机（2026-09-04 闹铃动画） */
      g_alm.snooze    = 1;
      g_alm_snooze_ms = (u32)bftm0_ct + 300000u;        /* 坑#93（2026-11-04）：贪睡间隔 10 分钟 -> 5 分钟（用户实测"太长了，短一点"） */
      goto_main();
      return 1;
    }
    case VOICE_CMD_ALARM_OFF:  /* 0xB1 别响了 */
      if (g_cnt_ringing) { cnt_ring_break(); return 1; }   /* 坑#93：倒计时响铃打断=回设定值留倒计时屏（同按键打断） */
      Screen_AlarmRingStop(); return 1;
    case VOICE_CMD_QUERY_ALARM: /* 0xB9 查看闹钟：d0=闹钟号(1-3) 选中对应槽（2026-09-05 修：原忽略 d0 永远显示 g_alm_sel）；仅在不在闹钟屏时切屏 */
      if (f->d0 >= 1 && f->d0 <= 3) g_alm_sel = (u8)(f->d0 - 1);   /* 选中查看的槽，draw_alarm 增量刷新 chip/时间 */
      if (UI_GetScreen() != UI_SCREEN_ALARM) UI_SetScreen(UI_SCREEN_ALARM);
      g_alm_idle_ms = (u32)bftm0_ct;              /* 语音查看闹钟：刷新空闲起点 */
      return 1;
    case VOICE_CMD_CNT_UP:     /* 0xBA 打开正计时（秒表，0 往上走） */
      Screen_UpStart(); return 1;
    case VOICE_CMD_DISP_OFF:   /* 0xB5 关闭显示：DISPOFF + 灭背光（PB4 高电平关） */
      g_disp_off = 1;                        /* 记录显示关闭态（响铃时 disp_wake 自动开屏，坑#71 #15） */
      UI_SetDispVisible(0);                  /* 显示不可见：切屏不再 DISPOFF 包上传（2026-10-12） */
      GPIO_SetOutBits(SPI_LCD_BL_GPIO, SPI_LCD_BL_PIN);
      Display_off();
      return 1;
    case VOICE_CMD_DISP_ON:    /* 0xB4 打开显示：亮背光 + DISPON + 强制重绘 */
      g_disp_off = 0;
      UI_SetDispVisible(1);                  /* 显示可见（2026-10-12） */
      Display_on();
      GPIO_ClearOutBits(SPI_LCD_BL_GPIO, SPI_LCD_BL_PIN);
      g_ui_bg_ver++;           /* 版本号++：下一帧 Screen_Draw 检测到变化强制全绘动态元素 */
      return 1;
    case VOICE_CMD_HOURLY:     /* 0xB8 整点报时开关 d0=0/1（内存开关，2026-08-28） */
      g_hourly_on = f->d0 ? 1 : 0;
      if (g_hourly_on) g_last_hourly = 0xFF;   /* 开启后下一个整点立即播报 */
      return 1;
    default:
      return 0;
  }
}

/* ==================== 按键事件分发 + 设置模式（2026-09-05 重构） ====================
   App 层集中分发：Driver(key.c) 只产出 KeyEvent{键号, 短/长按}，不掺业务；
   本函数按「事件类型 -> 键号 -> 当前系统状态」映射功能，易读易扩展：
     长按 KEY2(PB13) = 关屏/深休眠（系统级动作 Key_ScreenOff；任何状态都生效）
     短按 KEY2        = 非设置态：进设置；设置态：下一项；「完成」项再按：退出写回
     短按 KEY1(PB12)  = 设置态：减；非设置态：无动作
     短按 KEY3(PB14)  = 设置态：加；非设置态：无动作
     KEY1/KEY3 长按暂未绑定功能（未来可连发/其它）
   设置项循环：时 -> 分 -> 年 -> 月 -> 日 -> 完成（-> 再按 KEY2 退出；坑#89 移除秒——
   主屏不显示秒，秒项光标指空，秒值保留进设置快照，退出写回不变）
   进设置：快照当前时间到 g_set_ws（显示冻结——main 传冻结值给 Calendar_Refresh）
   退出：到「完成」再按 KEY2（写回 RTC）；或 15s 无按键超时自动退出（Screen_OnSecond）
   黄线：当前项下方（时/分在时间区、年/月/日在日期卡下方且按居中公式动态定位；
   「完成」不画=黄线消失提示到末位）
   （状态变量 g_set_* 与 SET_ITEM_* 宏在文件头定义。） */

/* 回绕：越界回另一边界（时 0<->23、月 1<->12 等） */
static u8 set_wrap8(int v, int lo, int hi)
{
  if (v < lo) v = hi;
  if (v > hi) v = lo;
  return (u8)v;
}

/* 16 位回绕：仅年份用（2000-2099 超出 u8，2026->(u8)234 截断乱码，2026-09-05 修） */
static u16 set_wrap16(int v, int lo, int hi)
{
  if (v < lo) v = hi;
  if (v > hi) v = lo;
  return (u16)v;
}

/* 坑#77 #4：调年/月后，若日超出新月份最大天数则钳制到当月最大天数（2/31 -> 2/28 或 2/29），
   配合 Calendar_DaysInMonth 保证设置界面任何路径都进不了非法日期。 */
static void set_day_clamp(void)
{
  u8 dim = Calendar_DaysInMonth(g_set_ws.month, g_set_ws.year);
  if (g_set_ws.day > dim) g_set_ws.day = dim;
}

static void set_item_adjust(int delta)
{
  switch (g_set_item)
  {
    case SET_ITEM_HOUR:  g_set_ws.hour   = set_wrap8((int)g_set_ws.hour   + delta, 0, 23); break;
    case SET_ITEM_MIN:   g_set_ws.minute = set_wrap8((int)g_set_ws.minute + delta, 0, 59); break;
    case SET_ITEM_SEC:   g_set_ws.second = set_wrap8((int)g_set_ws.second + delta, 0, 59); break;  /* 坑#89 已从循环移除（主屏不显示秒），此 case 不可达，保留防回归 */
    case SET_ITEM_YEAR:
      g_set_ws.year = set_wrap16((int)g_set_ws.year + delta, 2000, 2099);
      set_day_clamp();                        /* 坑#77 #4：年变后日钳制到新月份最大天数 */
      break;
    case SET_ITEM_MONTH:
      g_set_ws.month = set_wrap8((int)g_set_ws.month + delta, 1, 12);
      set_day_clamp();                        /* 坑#77 #4：月变后日钳制（2/31 不允许） */
      break;
    case SET_ITEM_DAY:
      g_set_ws.day = set_wrap8((int)g_set_ws.day + delta, 1,
                               (int)Calendar_DaysInMonth(g_set_ws.month, g_set_ws.year));  /* 坑#77 #4：日项边界=当月最大天数 */
      break;
    default: return;
  }
  printf("[KEY] set item=%u -> %04u-%02u-%02u %02u:%02u:%02u\r\n",
         (unsigned)g_set_item, (unsigned)g_set_ws.year, (unsigned)g_set_ws.month, (unsigned)g_set_ws.day,
         (unsigned)g_set_ws.hour, (unsigned)g_set_ws.minute, (unsigned)g_set_ws.second);
}

/* 退出设置：清黄线 + 写回 RTC + 强制静态刷新（防时间跳变逐位翻页）+ 复位状态 */
static void set_exit(void)
{
  if (g_set_line >= 0) set_line_clear((u8)g_set_line);  /* 清黄线（恢复该处背景） */
  TimeModule_SetTime(&g_set_ws);
  Screen_AlarmRungReset();   /* 坑#82：按键设置改时间后清"本分钟已响"（回拨后闹钟重新判定） */
  g_set_active = 0;
  g_set_item   = 0;
  g_set_line   = -1;
  Calendar_SetStaticNext();
  printf("[KEY] 设置退出 RTC=%04u-%02u-%02u %02u:%02u:%02u\r\n",
         (unsigned)g_set_ws.year, (unsigned)g_set_ws.month, (unsigned)g_set_ws.day,
         (unsigned)g_set_ws.hour, (unsigned)g_set_ws.minute, (unsigned)g_set_ws.second);
  cnt_pending_ring_play();   /* 坑#93：主屏设置退出（不回 goto_main）——设置中倒计时到点待响在此补响（切倒计时屏响铃） */
}

/* ==================== 闹钟屏按键设置系统（2026-09-05，参考时间模式） ====================
   空闲态：确定=进入设置；响铃中：确定=停铃。
   设置态：黄线选中当前项，左/右调整，确定切下一项；5 项后哨兵 ALM_SET_DONE 自动退出
   （同时间模式用户改法：切到哨兵即退出，无需结束项）。设置项：
     闹钟选择(1/2/3) -> 开启关闭 -> 闹钟时 -> 闹钟分 -> 闹钟循环，作用于当前显示槽 g_alm_sel。
   时/分调整 draw_alarm 大数字增量缓存（c_alm_h*）自动重画。 */

/* 闹钟设置黄线增量绘制（draw_alarm 每帧调；背景重传/切项时清旧画新） */
static void alm_set_line(void)
{
  if (!g_alm_set_active) { g_alm_set_line = -1; g_alm_set_bg_ver = g_ui_bg_ver; return; }
  if (g_alm_set_item == (u8)g_alm_set_line && g_alm_set_bg_ver == g_ui_bg_ver)
  {
    /* 坑#93（2026-11-04）：几何未变也【只画不清】重画黄线——调整时/分大数字重绘
       （draw_alarm UI_Show 清除区）会干扰黄线（用户实测"调值蓝线短一半、进位消失"），
       每帧叠线恢复（位置没变无残留无闪烁）；同坑#92 Screen_SetupLine 方案。 */
    if (g_alm_set_item != ALM_SET_DONE)
      alm_set_draw(g_alm_set_item, SC_C_BLUE);
    return;
  }
  if (g_alm_set_line >= 0 && g_alm_set_bg_ver == g_ui_bg_ver)                          /* 清旧线 */
    alm_set_clear((u8)g_alm_set_line);
  g_alm_set_line = -1;
  if (g_alm_set_item != ALM_SET_DONE)
    alm_set_draw(g_alm_set_item, SC_C_BLUE);                                             /* 画当前项（蓝，2026-11-03 由深棕改蓝） */
  g_alm_set_line    = (s8)g_alm_set_item;
  g_alm_set_bg_ver  = g_ui_bg_ver;
}

/* 退出闹钟设置：清黄线 + 复位状态（内存配置即改即生效，无独立写回）；
   记退出时刻：3s 后 Screen_OnSecond 自动回主屏（2026-09-05） */
static void alm_set_exit(void)
{
  if (g_alm_set_line >= 0) alm_set_clear((u8)g_alm_set_line);  /* 清黄线（恢复背景） */
  g_alm_set_active = 0;
  g_alm_set_item   = 0;
  g_alm_set_line   = -1;
  g_alm_set_exit_ms = (u32)bftm0_ct;
  printf("[KEY] 闹钟设置退出（3s 后回主屏）\r\n");
}

/* 闹钟设置态按键：左/右=调整当前项，确定=下一项（哨兵自动退出） */
static void alm_set_key(const KeyEvent *ke)
{
  if (ke->id == KEY_ID_1 || ke->id == KEY_ID_3)
  {
    int delta = (ke->id == KEY_ID_3) ? 1 : -1;
    switch (g_alm_set_item)
    {
      case ALM_SET_SEL:                          /* 闹钟选择 1/2/3（回绕用 int 局部防负数转 u8 出错） */
      {
        int s = (int)g_alm_sel + delta;
        if (s < 0) s = 2;
        if (s > 2) s = 0;
        g_alm_sel = (u8)s;
        break;
      }
      case ALM_SET_HOUR:                         /* 闹钟时 0-23 回绕（大数字增量自动重画） */
      {
        int h = (int)g_alm_hour[g_alm_sel] + delta;
        if (h > 23) h = 0;
        if (h < 0) h = 23;
        g_alm_hour[g_alm_sel] = (u8)h;
        break;
      }
      case ALM_SET_MIN:                          /* 闹钟分 0-59 回绕 */
      {
        int m = (int)g_alm_min[g_alm_sel] + delta;
        if (m > 59) m = 0;
        if (m < 0) m = 59;
        g_alm_min[g_alm_sel] = (u8)m;
        break;
      }
      case ALM_SET_ON:                           /* 开启关闭：二态，左右任一翻转 */
        g_alm_on[g_alm_sel] ^= 1;
        break;
      case ALM_SET_LOOP:                         /* 循环 1..3（坑#71 #7：手动只循环 五天/六天/七天，
                                                    跳过"单次0"——单次与七天显示全高亮无法区分，避免看起来重复） */
      {
        int lp = (int)g_alm_loop[g_alm_sel] + delta;
        if (lp > 3) lp = 1;
        if (lp < 1) lp = 3;
        g_alm_loop[g_alm_sel] = (u8)lp;
        break;
      }
      default: break;
    }
    g_alm_set_last_act = (u32)bftm0_ct;
    printf("[KEY] 闹钟设置 槽%u %02u:%02u on=%u loop=%u\r\n", (unsigned)g_alm_sel + 1,
           (unsigned)g_alm_hour[g_alm_sel], (unsigned)g_alm_min[g_alm_sel],
           (unsigned)g_alm_on[g_alm_sel], (unsigned)g_alm_loop[g_alm_sel]);
    return;
  }
  if (ke->id == KEY_ID_2)                        /* 确定：下一项，到哨兵自动退出 */
  {
    g_alm_set_item++;
    if (g_alm_set_item >= ALM_SET_DONE)
    {
      alm_set_exit();
      return;
    }
    g_alm_set_last_act = (u32)bftm0_ct;
    printf("[KEY] 闹钟设置项 -> %u\r\n", (unsigned)g_alm_set_item);
  }
}

/* 闹钟屏按键（Screen_HandleKey 分流调用）：响铃=任意键打断停铃回主屏（2026-09-05 原仅确定键）；空闲=确定进设置；设置态=alm_set_key */
static void screen_key_alarm(const KeyEvent *ke)
{
  g_alm_idle_ms = (u32)bftm0_ct;         /* 闹钟屏任意按键刷新空闲计时起点（2026-09-05 常态自动返回） */
  if (g_alm.ringing)                     /* 响铃中：任意键都停铃回主屏（2026-09-05 用户需求「响闹可用按键打断」） */
  {
    (void)ke;
    Screen_AlarmRingStop();
    return;
  }
  if (!g_alm_set_active)                 /* 空闲：确定=进入闹钟设置；KEY1/KEY3 短按=手动返回主屏（坑#93 2026-11-04，原只能等 15s 自动回） */
  {
    if (ke->id == KEY_ID_2)
    {
      g_alm_set_exit_ms = 0;          /* 取消「退出后回主屏」倒计时（重新进入设置） */
      g_alm_set_active    = 1;
      g_alm_set_item      = ALM_SET_SEL;
      g_alm_set_last_act  = (u32)bftm0_ct;
      printf("[KEY] 进入闹钟设置（闹钟选择 %u）\r\n", (unsigned)g_alm_sel + 1);
    }
    else if (ke->id == KEY_ID_1 || ke->id == KEY_ID_3)
    {
      printf("[KEY] 闹钟屏 KEY%u 短按 -> 回主屏\r\n", (unsigned)ke->id + 1);
      goto_main();
    }
    return;
  }
  alm_set_key(ke);
}

/* ==================== 按键事件集中分发（main 每轮 Key_GetEvent 出队后调用） ====================
   按「事件类型 -> 键号 -> 当前系统状态（屏）」三层路由：
   长按：KEY2=关屏/深休眠（任何状态）；设置态 KEY1/KEY3=快加快减；
        主屏空闲 KEY1/KEY3 长按=无动作（坑#93 2026-11-04 切屏改短按）。
   短按按当前屏分流——主屏=时间设置模式（非设置态 KEY1 左->倒计时屏、KEY3 右->闹钟屏，坑#93）、
        倒计时屏=模式/分/秒/暂停/重置选择、闹钟屏=闹钟设置/响铃停。
   改功能只动这里。 */
void Screen_HandleKey(const KeyEvent *ke)
{
  /* 贪睡待响期间：任意按键（短按/长按/连发一律）结束贪睡状态（2026-09-05 用户需求）：
     清待响标志，贪睡符号停止闪烁，不再到点重响。响铃中 g_alm.snooze 已先清（alarm_tick
     贪睡重响前置 alm_snooze_clear），不冲突。
     坑#74：一次按压只能有一个功能——结束贪睡后立即 return，不再执行该键原有功能
     （KEY2 短按进时间设置/长按关屏、KEY1/KEY3 长按切屏等都不触发）。 */
  if (g_alm.snooze)
  {
    alm_snooze_clear();
    printf("[KEY] 任意键结束贪睡待响\r\n");
    return;
  }
  /* 1) 长按/连发（KEY_EV_LONG/KEY_EV_REPEAT，driver key.c 长按后按住期间持续发 REPEAT）：
        KEY2 长按=关屏/深休眠（任何屏/任何状态）；
        设置态（时间设置/闹钟设置/倒计时分秒调整）KEY1/KEY3 长按=快加快减（坑#71 #6，
        不再跳屏——原主屏长按切屏在设置中会误跳到闹钟/倒计时屏）；
        主屏空闲 KEY1/KEY3 长按=无动作（坑#93 2026-11-04：切屏由长按改短按触发）。 */
  if (ke->evt == KEY_EV_LONG || ke->evt == KEY_EV_REPEAT)
  {
    if (ke->id == KEY_ID_2)
    {
      if (ke->evt == KEY_EV_LONG) Key_ScreenOff();   /* KEY2 长按关屏；连发不重复关 */
      return;
    }
    if (ke->id == KEY_ID_1 || ke->id == KEY_ID_3)
    {
      int delta = (ke->id == KEY_ID_3) ? 1 : -1;
      if (g_set_active)                       /* 主屏时间设置：长按=快加快减 */
      {
        set_item_adjust(delta);
        g_set_last_act = (u32)bftm0_ct;
        return;
      }
      if (g_alm_set_active)                   /* 闹钟设置：长按=快加快减（复用短按调整逻辑） */
      {
        KeyEvent k2 = *ke; k2.evt = KEY_EV_SHORT;
        alm_set_key(&k2);
        return;
      }
      if (UI_GetScreen() == UI_SCREEN_CNT && g_cnt_adj &&
          (g_cnt_sel == CNT_SEL_MIN || g_cnt_sel == CNT_SEL_SEC))   /* 倒计时分/秒调整：长按=快加快速 */
      {
        cnt_adjust((int)g_cnt_sel, delta);
        return;
      }
      /* 坑#93（2026-11-04）：主屏空闲切屏改由短按触发（见 screen_key_main），长按不再切屏 */
    }
    return;
  }

  /* 坑#93（2026-11-04）：倒计时响铃中（倒计时屏）任意键打断——停铃+回设定值+停留倒计时屏
     （问题 8：原"按键打断会直接返回到时间显示"应留屏；问题 7：打断后回设定值而非 00:00 残留）。
     KEY2 长按关屏已在上面优先处理；此处拦截所有短按。 */
  if (g_cnt_ringing && UI_GetScreen() == UI_SCREEN_CNT)
  {
    cnt_ring_break();
    return;
  }

  /* 2) 短按：按当前屏分流 */
  switch (UI_GetScreen())
  {
    case UI_SCREEN_MAIN:  screen_key_main(ke);  break;   /* 时间设置模式 */
    case UI_SCREEN_CNT:   cnt_sel_key(ke);      break;   /* 倒计时选择系统 */
    case UI_SCREEN_ALARM: screen_key_alarm(ke); break;   /* 闹钟设置/响铃停 */
    default: break;
  }
}

/* ==================== 主屏时间设置模式（Screen_HandleKey 的 MAIN 分流） ====================
   设置项循环：时 -> 分 -> 年 -> 月 -> 日 -> 完成（坑#89 移除秒：主屏不显示秒，光标无处可指）；
   KEY2 短按：进设置 / 下一项 / 切到「完成」哨兵自动退出（用户 2026-09-05 改为一轮直接退出）；
   KEY1 减 / KEY3 加；非设置态 KEY1/KEY3 短按 = 切屏（KEY1 左->倒计时屏、KEY3 右->闹钟屏，坑#93 2026-11-04）。 */
static void screen_key_main(const KeyEvent *ke)
{
  /* 短按 KEY2(PB13)：进设置 / 下一项 / 哨兵退出 */
  if (ke->id == KEY_ID_2)
  {
    if (!g_set_active)
    {
      TimeModule_GetTime(&g_set_ws);        /* 进设置：快照当前时间（显示冻结） */
      g_set_active = 1;
      g_set_item   = SET_ITEM_HOUR;
      Calendar_ColonForceOn();              /* 进设置强制冒号亮（防落在闪烁"灭"相位设置期一直灭，坑#71 #3 2026-09-05） */
      printf("[KEY] 进入设置模式（时）\r\n");
    }
  /*  else if (g_set_item == SET_ITEM_DONE)
    {
      set_exit();                           // 「完成」项再按 KEY2：退出写回
      return;
    }*/
    else
    {
      g_set_item++;
      if (g_set_item >= SET_ITEM_COUNT) g_set_item = SET_ITEM_HOUR;
      if (g_set_item == SET_ITEM_SEC) g_set_item++;  /* 坑#89 主屏不显示秒，跳过秒项 */
      printf("[KEY] 设置项 -> %u\r\n", (unsigned)g_set_item);
      if (g_set_item == SET_ITEM_DONE)
      {
    	  set_exit();
    	  return;
      }
    }
    g_set_last_act = (u32)bftm0_ct;
    return;
  }

  /* 3) 短按 KEY1/KEY3：设置态加减；非设置态=左右键切屏（坑#93 2026-11-04：
     主屏切屏由长按改短按——KEY1 左->倒计时屏、KEY3 右->闹钟屏，仅切屏不打断） */
  if (!g_set_active)
  {
    if (ke->id == KEY_ID_1)
    {
      printf("[KEY] 短按左 -> 倒计时屏\r\n");
      UI_SetScreen(UI_SCREEN_CNT);
      g_cnt_idle_ms = (u32)bftm0_ct;   /* 坑#83：切屏动作点立即刷新倒计时屏空闲计时（同原长按切屏，
          防主屏停留>15s 后切屏被本轮"未运行 15s 空闲自动回"/0xEF 踢回） */
    }
    else if (ke->id == KEY_ID_3)
    {
      printf("[KEY] 短按右 -> 闹钟屏\r\n");
      UI_SetScreen(UI_SCREEN_ALARM);
      g_alm_idle_ms = (u32)bftm0_ct;   /* 坑#83：切闹钟屏动作点立即刷新空闲计时（防进屏瞬间被 0xEF 踢回） */
    }
    return;
  }
  if (ke->id == KEY_ID_1)      set_item_adjust(-1);
  else if (ke->id == KEY_ID_3) set_item_adjust(1);
  else return;
  g_set_last_act = (u32)bftm0_ct;
}

int Screen_SetupActive(void) { return g_set_active ? 1 : 0; }

const TimeStruct *Screen_SetupValue(void) { return &g_set_ws; }

/* 语音设时/设日成功时同步手动设置工作区（坑#71 #10 2026-09-05）：
   语音在手动设置模式中改时间/日期——原只改 RTC，冻结显示不变，退出时 set_exit 用旧快照
   覆盖回旧值（时间变回语音前）。这里把新值同步进 g_set_ws（仍停留设置模式，显示即时刷新），
   退出写回即新值。 */
void Screen_SetupVoiceSync(const TimeStruct *nt)
{
  if (!g_set_active) return;
  g_set_ws.hour   = nt->hour;
  g_set_ws.minute = nt->minute;
  g_set_ws.second = nt->second;
  g_set_ws.year   = nt->year;
  g_set_ws.month  = nt->month;
  g_set_ws.day    = nt->day;
  g_set_last_act  = (u32)bftm0_ct;   /* 刷新活动时刻：防被 15s 超时立即退出 */
  Calendar_SetStaticNext();          /* 下一帧静态全刷（避免逐位翻页动画） */
}

/* ==================== 三屏设置光标「几何+绘制+清除」集中管理（坑#92 后整理，2026-11-03） ====================
 * 三个屏的设置光标（下划线/黄线）坐标函数集中放一起，微调位置只看本区，避免散落各处：
 *   主屏   set_line_geom/draw/clear —— Screen_SetupLine 调用（3px 蓝 SC_C_BLUE，主屏时间/日期设置）
 *   倒计时 cnt_sel_geom/draw/clear  —— cnt_sel_line 调用（2px 黄=选中/蓝=调整，倒计时模式选择）
 *   闹钟   alm_set_geom/draw/clear  —— alm_set_line 调用（3px 蓝 SC_C_BLUE，闹钟设置）
 * 坐标约定：设计坐标顶原点（y 越小越靠上）；各 item 的 y 由【被指元素的绘制函数输入 + 字号】
 * 推导（如主屏时钟数字 CLK_Y=18+40 字高 -> 光标 y60），改字体大小/位置后必须同步重推。
 * ============================================================================================= */

/* 时间设置光标矩形坐标（设计坐标，顶原点；2026-10-10 适配坑#84 新主屏）：
   时/分：20x40 大数字 y18-58（CLK_Y=18+40），光标画数字正下方 y60（坑#92：
     由函数输入推导 = 数字底 58 + 2px；其下蓝牙胶囊框顶 ~y64 不压）——
     x 对齐对应数字列（与 calendar.c CLK_X_* 一致）：
       HOUR = H1(32)+H2(52) x32-72；MIN = M1(84)+M2(104) x84-124。
   年/月/日：22 高日期 y56-78 画右上卡（UI_Show(187,56,UI_SLOT_DATE2)），光标画数字
     正下方 y78（= 日期绘制输入 56 + 22 字高）；⚠️ 坑#89：日期是【110px 窗口整串居中】
     （UI_SLOT_DATE2 ALIGN_CENTER，cx = 187+(110-串宽)/2），月/日 1-2 位时串宽 80-104
     变化 → 实际数字列随位数浮动（"2026.1.5" 年起点 x202 vs "2026.10.12" 年起点 x190）——
     光标必须按同一公式动态算，不能写死 187/239/267（原固定坐标在月/日为 1 位时偏 15px 错位）。
   秒：坑#89 移除设置项（主屏不显示秒，光标无处可指）。 */
static void set_line_geom(u8 item, int *x, int *w, int *y)
{
  switch (item)
  {
    /* 坑#92（2026-11-03）：HOUR/MIN 光标 y 由绘制函数输入推导——时钟数字
       draw_clock_digit 输入 CLK_Y=18、字高 40 -> 数字区 y18-58；其下蓝牙胶囊
       （BT_Y=67 文字顶，胶囊框顶 ~y64）。光标 = 数字底 58 下方 2px -> y60
       （3px 线 y60-62，距数字 2px、距胶囊框顶 64 有 2px，不压任何元素）。
       原 y96 是坑#85 旧 30 高数字时代「胶囊 y60-92 下方 4px」的位置，2026-10-10
       数字放大上移后未同步 -> 实机光标比数字低 38px「往下偏很多」。 */
    case SET_ITEM_HOUR:  *x = 32;  *w = 40; *y = 56; break;
    case SET_ITEM_MIN:   *x = 84;  *w = 40; *y = 56; break;
    case SET_ITEM_YEAR:
    case SET_ITEM_MONTH:
    case SET_ITEM_DAY:
    {
      /* 与 UI_Show 串宽/居中公式一致（digit 12px、点 4px、clear_w 110、x0 187）：
         串宽 = 年4位48 + 点4 + 月(12|24) + 点4 + 日(12|24) = 56 + mw + dw */
      int mw = (g_set_ws.month >= 10) ? 24 : 12;
      int dw = (g_set_ws.day   >= 10) ? 24 : 12;
      int cx = 187 + (110 - (56 + mw + dw)) / 2;   /* 整串居中起点 */
      if (item == SET_ITEM_YEAR)      { *x = cx;          *w = 48; }
      else if (item == SET_ITEM_MONTH){ *x = cx + 52;     *w = mw; }
      else                            { *x = cx + 52 + mw + 4; *w = dw; }
      *y = 78;
      break;
    }
    default:             *x = 0;   *w = 0;  *y = 0;  break;
  }
}

/* 画某设置项下方的选择光标（3px 蓝 SC_C_BLUE 下划线；坑#84 新主屏奶油风，2026-11-03 由深棕改蓝；
   替代原 2px 黄线 #FFD54F，旧黄在米黄背景上不明显、且时/分/秒坐标还是老布局完全错位，2026-10-10 一并修正） */
static void set_line_draw(u8 item, u16 color)
{
  int x, w, y;
  set_line_geom(item, &x, &w, &y);
  if (w == 0) return;
  Display_FillRect(x, y, w, 3, color);
}

/* 清光标：UI_DrawGlyphRect 传非法码 -> 逐行「读背景->写回」仅还原该矩形背景。
   精确恢复日期卡内蓝粉渐变/描边等细节，比用固定背景色填涂无残留（2026-10-10）。
   ⚠️ ry 必须传 GRAM 行（=239-(设计y+h)+1，同 draw_colon_at），不能传设计 y。
   坑#89：优先用【绘制时的缓存几何】——日期居中随月/日位数浮动，若用当前 g_set_ws
   重算（调整后位数变了）会清到错误位置留下旧光标。 */
static void set_line_clear(u8 item)
{
  int x, w, y;
  if (item == (u8)g_set_line && g_set_line_x >= 0)
  {
    x = g_set_line_x; w = g_set_line_w; y = g_set_line_y;
  }
  else
    set_line_geom(item, &x, &w, &y);
  if (w <= 0) return;
  UI_DrawGlyphRect(x, 239 - (y + 3) + 1, w, 3, 0xFFFFu, 0);
}

/* ---- 倒计时屏（屏1）选中光标三件套（几何/画/清，从原 630 行集中至此，坑#92） ---- */

/* 选中项光标几何（坑#88：顶部标签下方 y51 / 大数字段 y111 / 按钮下方 y232） */
static void cnt_sel_geom(u8 item, int *x, int *w, int *y)
{
  *x = 0; *w = 0; *y = 143;
  switch (item)
  {
    case CNT_SEL_MODE:  *x = 16;  *w = 54; *y = 51;  break;  /* 顶部粉色标签（x10-75 y9-48）下方 */
    case CNT_SEL_MIN:   *x = 114; *w = 40; *y = 111; break;  /* M 段 x114-153（20x40 数字 y69-108） */
    case CNT_SEL_SEC:   *x = 166; *w = 40; *y = 111; break;  /* S 段 x166-205 */
    case CNT_SEL_PAUSE: *x = 13;  *w = 69; *y = 232; break;  /* 按钮1 x13-81 y187-228 下方 */
    case CNT_SEL_RESET: *x = 88;  *w = 69; *y = 232; break;  /* 按钮2 x88-156 */
    case CNT_SEL_EXIT:  *x = 164; *w = 68; *y = 232; break;  /* 按钮3 x164-231 */
    default: *w = -1; return;
  }
}

/* 画选中光标（2px 下划线；黄=选中、蓝=调整态） */
static void cnt_sel_draw(u8 item, u16 color)
{
  int x, w, y;
  cnt_sel_geom(item, &x, &w, &y);
  if (w <= 0) return;
  Display_FillRect(x, y, w, 2, color);
}

/* 清光标：恢复背景（坑#88：新背景浅色渐变非纯色，FillRect 单一清色会留痕，坑#60 教训） */
static void cnt_sel_clear(u8 item)
{
  int x, w, y;
  cnt_sel_geom(item, &x, &w, &y);
  if (w <= 0) return;
  UI_DrawGlyphRect(x, 239 - (y + 2) + 1, w, 2, 0xFFFFu, 0);
}

/* ---- 闹钟屏（屏2）设置黄线三件套（几何/画/清，从原 1454 行集中至此，坑#92） ---- */

/* 闹钟设置黄线（坑#85 3px，2026-11-03 由深棕改蓝 SC_C_BLUE；位置适配新背景；清线用恢复背景精确还原）：
   SEL  主卡顶 y77（数字上方，指示"正在选择闹钟槽"）
   HOUR/MIN 大数字下方 y164（数字底 162 +2，星期文字 172 前）
   ON   开关槽下方 y104（槽底 99 +5）
   LOOP 星期瓶子下方 y211（瓶底 207 +4，数字 219 前） */
static void alm_set_geom(u8 item, int *x, int *w, int *y)
{
  switch (item)
  {
    case ALM_SET_SEL:  *x = 60;  *w = 88; *y = 80;  break;
    case ALM_SET_HOUR: *x = 17;  *w = 80;  *y = 144; break;
    case ALM_SET_MIN:  *x = 111; *w = 80;  *y = 144; break;
    case ALM_SET_ON:   *x = 233; *w = 50;  *y = 100; break;
    case ALM_SET_LOOP: *x = 17;  *w = 174; *y = 211; break;
    default: *x = 0; *w = 0; *y = 0; break;
  }
}

/* 画闹钟设置黄线（3px 蓝 SC_C_BLUE，2026-11-03 由深棕改蓝） */
static void alm_set_draw(u8 item, u16 color)
{
  int x, w, y;
  alm_set_geom(item, &x, &w, &y);
  if (w == 0) return;
  Display_FillRect(x, y, w, 3, color);
}

/* 清闹钟设置黄线：恢复背景（非法码仅还原矩形，精确还原卡片渐变/描边，2026-10-10） */
static void alm_set_clear(u8 item)
{
  int x, w, y;
  alm_set_geom(item, &x, &w, &y);
  if (w == 0) return;
  UI_DrawGlyphRect(x, 239 - (y + 3) + 1, w, 3, 0xFFFFu, 0);
}

/* 主屏黄线绘制（App_Render 主屏分支调用）：切项/背景重传时清旧画新；「完成」不画（提示到末位）。
   背景版本号 g_ui_bg_ver 记录：切屏回主屏（UI_SetScreen 重传背景覆盖整屏）后强制重画黄线，
   否则 g_set_line 缓存认为"没变"不重画，黄线会一直消失。
   坑#89：日期光标几何随月/日位数浮动（居中），除切项/重传外还要检测【几何变化】
   （如 9 月->10 月 位数 1->2，日期重居中、数字列右移）→ 几何变了清旧画新。 */
void Screen_SetupLine(void)
{
  int x, w, y;
  set_line_geom(g_set_item, &x, &w, &y);
  if (!g_set_active) { g_set_line = -1; g_set_line_x = -1; g_set_bg_ver = g_ui_bg_ver; return; }
  if (g_set_item == (u8)g_set_line && x == g_set_line_x && w == g_set_line_w
      && g_set_bg_ver == g_ui_bg_ver)
  {
    /* 坑#92（2026-11-03）：几何未变也【只画不清】重画光标——日期/农历重绘
       （设置模式调整月/日、或跨天时 Calendar_Refresh 里 month/day 变 -> 农历
       UI_Show(202,76) 清除区 y76-98 覆盖日期光标 y78-81）会把光标抹掉；而此处
       原本几何未变直接 return 不重画 -> 光标「有时消失」。位置没变，直接叠线
       恢复即可（无残留无闪烁）；且本函数在 Calendar_Refresh 之后调用（app_render.c），
       光标总是最后画、永远可见。 */
    if (g_set_item != SET_ITEM_DONE)
      set_line_draw(g_set_item, SC_C_BLUE);                                             /* 画当前项（蓝，2026-11-03 由深棕改蓝） */
    return;
  }
  if (g_set_line >= 0 && g_set_bg_ver == g_ui_bg_ver)                       /* 清旧线（背景已重传则整屏已覆盖，无需清） */
    set_line_clear((u8)g_set_line);
  g_set_line = -1;
  if (g_set_item != SET_ITEM_DONE)
    set_line_draw(g_set_item, SC_C_BLUE);                               /* 画当前项（蓝，2026-11-03 由深棕改蓝） */
  g_set_line    = (s8)g_set_item;
  g_set_line_x  = (s16)x; g_set_line_w = (s16)w; g_set_line_y = (s8)y;
  g_set_bg_ver  = g_ui_bg_ver;
}


void debug_test(TimeStruct *t)
{
	static int test_count = 0;
	if (g_alm.ringing && t->second==20 && test_count==0)
	{
		test_count++;
		 Voice_Send(0x06,1,0,0,1);
	}


}




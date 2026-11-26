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
 *   槽同上；开关chip [132,17]-[196,35]（已开启绿/已关闭灰）；星期chips y=186..208、x=19+i*26。
 *   选中绿深字/未选面板灰；强调线 [16,170]-[304,171]（响铃红）。
 *
 * 业务状态机：
 *   倒计时：0x84 分钟(d0=1-120，坑#93)/0x88 番茄(25min) 启动切屏1；0x85 暂停/0x86 继续/0x87 结束
 *           （结束停 3s 回主屏）；到 0 → VOICE_TX_RING 响铃 + 人物切闹铃动画(ALARM) + 停 60s 回主屏
 *           （停响人物回待机 IDLE；2026-09-05 响铃中任意键打断停铃回主屏，坑#70）。
 *   正计时（2026-08-28）：0xBA 打开，从 00:00 递增（复用屏1 MM:SS）；0x85/0x86 暂停/继续。
 *           0x87 结束停 3s 回主屏；与倒计时互斥（启动其一先停另一）。
 *   闹钟：0x8A 设置(写配置+切屏2)/0x8B 开关/0x8C 循环(0单次 1一~五 2一~六 3一~日)/0x8D 铃音/
 *           0xB9 查看(仅切屏 + 回 OK)；到点切屏2 + VOICE_TX_RING + 人物切换闹铃动画(ALARM)，
 *           0xB1 别响/0x8B 关/60s 自动停回主屏（任何停响路径人物回待机 IDLE）；0xB0 贪睡 = 停响 + 闹钟时间 +10min。
 *   其他（2026-08-28）：0xB4/0xB5 显示开关（DISPOFF/ON + 背光）；0xB8 整点报时开关（内存，整点播 0x07）。
 *   按键事件分发（2026-09-05 重构）：Driver(key.c) 只出 KeyEvent{键号,短/长按} 统一事件流，
 *           本模块 Screen_HandleKey 按「事件类型 -> 键号 -> 当前状态」集中分发功能：
 *           KEY2 长按=关屏/深休眠（调 Key_ScreenOff）；KEY2 短按=进设置/下一项/完成退出；
 *           KEY1 减 / KEY3 加（仅设置模式）；项循环 时 -> 分 -> 秒 -> 年 -> 月 -> 日 -> 完成；
 *           时间冻结显示，退出写回 RTC（「完成」再按 / 15s 超时）。
 ************************************************************************************************************/
#include "screen.h"
#include "ui.h"
#include "countdown_view.h"
#include "ui_skin.h"
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
/* 坑#85 闹钟屏：开关 ON 控件 / 星期未选中灰白瓶 从效果图截取存 ALM2 容器（UI_DrawAlm2 贴图），
   不再自绘几何近似；OFF/选中状态靠恢复背景）。闹钟大数字用 SC_C_MAIN_INK 深棕（同主屏）。 */
#define SC_C_WHITE   UI_C_DIGIT             /* 大数字白 #F8FAFC */

/* ==================== 坑#88 倒计时屏新 UI（奶油风 2026-10-11） ====================
   背景：静态版导入（ui_screen1.png，浅米 #F2DBBB 系）+ 动态元素固件画。
   布局（效果图像素实测）：大数字 MM:SS 20x40 黑（0xE100+d）居中 y69..108；
   进度条轨道 x57-261 y119-146 在背景，填充橙/黄/红 x57-260 y122-143；
   按钮1「开始 START」画在背景（绿底黑字），固件运行/暂停时切「暂停 PAUSE」；
   顶部粉色标签内「倒计时/正计时」18x22 黑字动态；右下粉色框人物动画区。
   坑#93（2026-11-04）：分钟扩 3 位（MMM:SS，"120:00"），x84 支持 1~120 分钟——
   组 20*5+12=112 居中 x160 -> 分百 x104 分十 x124 分个 x144 冒号 x164 秒十 x176 秒个 x196。*/
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
   真彩图标存 ICO1 容器（UI_DrawHourglass，保留用户 PNG 多色调）：*/
#define CNT_ICO_X      84     /* 顶部小图标行：x84（秒 24、报时 44、贪睡 64、沙漏 84） */
#define CNT_ICO_Y      2      /* 顶部 y2（16x16，y2..18） */
#define CNT_ICO_W      16
#define CNT_ICO_H      16

/* 主屏状态角标（2026-09-05 坑#71 #1/#2）：整点报时符号/贪睡符号，16x16 单色 glyph
   （0xE302/0xE303，build_flash build_font 从用户 PNG 生成）；坑#84 顶部小图标行、
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
static u8  g_cnt_pending_ring = 0;     /* 只在手动设置期间延后倒计时提醒 */
static u32 g_cnt_idle_ms = 0;          /* 倒计时屏最后操作时刻（进屏/按键/命令刷新；未运行空闲 15s 自动回主屏，坑#71 2026-09-05） */
static u8  g_disp_off = 0;             /* 1=显示已关（语音 0xB5 关显示；倒计时/闹钟响铃时自动亮屏，坑#71 #15 2026-09-05） */

/* ==================== 倒计时屏按键选择（2026-09-05） ====================
   光标常驻五项：分 / 秒 / 暂停 / 重置 / 退出（正计时 g_up 时仅 暂停/重置/退出 三项）；
   左(PB12)右(PB14)切换选中，确定(PB13)；
     暂停=开始/暂停/继续切换；重置=回初始总时长并停止待启动；
     分秒进入调整态（光标变蓝，左右调整数值，再按确定退出；调整自动暂停）；
     退出=回主屏（倒计时/正计时后台继续跑；到点自动切回本屏响铃，2026-09-05）。 */
static u8  g_cnt_sel = CNT_SEL_MODE;   /* 当前选中项（默认模式项） */
static u8  g_cnt_adj = 0;                /* 1=分秒调整态（左右调整数值） */
/* 2026-11-19 用户要求：倒计时屏**常态不闪**（不显示选中项）；按左/右/中任一键先「唤醒」，
   选中态开始 1Hz 闪，且唤醒的那一次按键不执行任何功能；之后 5s 无操作自动回到不闪。 */
static u8  g_cnt_sel_awake = 0;          /* 1=选中项可见（1Hz 闪） */
static u32 g_cnt_sel_awake_ms = 0;       /* 最近一次按键时刻（算 5s 空闲） */
#define CNT_SEL_IDLE_MS  5000u
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
static u32 g_alm_snooze_ms = 0;        /* 贪睡重响时刻 bftm0_ct（贪睡 = 停响 + 一次性延迟 5 分钟，配置不变，坑#71 #9 / 坑#93 改 5 分钟） */

/* ---- 3 个闹钟槽（运行时内存态，2026-09-05：ui_cfg_t 单闹钟仅作槽1 初始值，闹钟配置不写 Flash） ---- */
static u8 g_alm_hour[3];   /* 槽i 时 */
static u8 g_alm_min[3];    /* 槽i 分 */
static u8 g_alm_on[3];     /* 槽i 开关（1=开） */
/* 槽i 循环模式（2026-11-19 由四档改三档）：
 *   0 = 单次：下一次到点响，响完自动关掉该槽（= 用户说的「明天叫我」）
 *   1 = 每天：一~日全响（等价于掩码 7）
 *   2 = 自定义：一~四恒定响，五/六/日 由 g_alm_wd 的 bit0/1/2 决定
 * 设计稿把星期做成「五/六/日 三个可切换图标」，所以板上的循环入口就是那三个开关；
 * 0/1 两档由语音（0x8C）设定，避免屏幕上多出一块设计稿没有的模式文字。 */
static u8 g_alm_loop[3];   /* 槽 i 循环 0单次/1一~五/2一~六/3一~日 */
static u8 g_alm_days[3] = { 7u, 7u, 7u };  /* 槽i 的星期循环天数 5/6/7（2026-11-19） */
static u8 g_alm_wd[3];     /* 槽i 五/六/日 掩码：bit0=五 bit1=六 bit2=日（1=响） */
static u8 g_alm_once = 0;  /* 1..3 = 该槽「单次」响完待自动关（0=无）；存 idx+1 便于判 0 */
static u8 g_alm_sel      = 0;  /* 屏当前显示/操作槽（0..2） */
static u8 g_alm_ring_idx = 0;  /* 当前正在响的槽（贪睡 +10min 用） */

/* ---- 闹钟屏按键设置（2026-09-05，参考时间模式：黄线+确定切项+哨兵自动退出） ---- */
#define ALM_SET_SEL   0   /* 闹钟选择 1/2/3 */
#define ALM_SET_ON    1   /* 开启关闭 */
#define ALM_SET_HOUR  2   /* 闹钟时（0-23） */
#define ALM_SET_MIN   3   /* 闹钟分（0-59） */
#define ALM_SET_DAYS  4   /* 星期循环 5/6/7 天（三个图标开关合并成一个值） */
#define ALM_SET_DONE  5   /* 哨兵：切到即自动退出 */

/* 天数 -> 五/六/日 掩码（bit0 五 / bit1 六 / bit2 日）：
 *   5 天 = 一~五（周五参与）→ bit0；6 天 = 一~六 → bit0|bit1；7 天 = 每天 → 全开。
 *   掩码既决定闹钟在哪天响，也决定闹钟屏上哪几个图标是彩色的（用户 2026-11-20 明确：
 *   彩色=参与，「（抠图）」里那套灰图=不参与）。 */
static u8 alm_days_mask(u8 d)
{
  if (d >= 7u) return 7u;
  if (d == 6u) return 3u;
  return 1u;
}

/* 掩码 -> 5/6/7 天（取最右边那个点亮的图标，语义与按键 5->6->7 递增一致）。
   只用于「闪灭矩形取哪一格」和按键调节的起点，不参与响铃判定。 */
static u8 alm_days_of_mask(u8 mask)
{
  mask &= 7u;
  if (mask & 4u) return 7u;
  if (mask & 2u) return 6u;
  return 5u;
}

/* ⚠️ 2026-11-24 用户：「语音设置闹钟七天循环，状态和显示不能对应，手动设置的可以」。
   根因是**两份数据源**：响铃判定用 (g_alm_loop + g_alm_wd)，而闹钟屏的 五/六/日 图标
   用 alm_days_mask(g_alm_days) 反推；语音 0x8C 只改 loop/wd、没改 days（也改不了：
   掩码 5/6/7 才有 days 对应值），于是「语音设了七天」屏上还是上次的 5 天。
   现在统一成：**显示与响铃都只看 (loop + wd)**，days 降级为「按键 5/6/7 的存档」。 */

/* 显示用掩码：这一槽 五/六/日 哪几天参与（与 weekday_match 同源） */
static u8 alm_show_mask(u8 idx)
{
  if (idx > 2u) idx = 0u;
  if (g_alm_loop[idx] == 1u) return 7u;   /* 每天：三个图标全亮 */
  return (u8)(g_alm_wd[idx] & 7u);        /* 单次 / 自定义：按掩码 */
}

/* 一次写三件套，保证 loop / wd / days 不再互相打架 */
static void alm_set_cycle(u8 idx, u8 loop, u8 wd)
{
  u8 m = (u8)(wd & 7u);
  if (idx > 2u) return;
  if (loop > 2u) loop = 2u;
  if (loop == 1u) m = 7u;                 /* 每天：掩码必然全满 */
  g_alm_loop[idx] = loop;
  g_alm_wd[idx]   = m;
  g_alm_days[idx] = alm_days_of_mask(m);
}

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
/* 2026-11-20：本次设置「动过哪一项」——退出写回时决定秒怎么处理。
   用户要的语义：改过分 -> 秒归零（从整分开始）；只改时/只改日期 -> 分秒接着走，不清零。 */
static u8         g_set_adj_hour = 0;
static u8         g_set_adj_min  = 0;
static u8         g_set_adj_sec  = 0;   /* 2026-11-24：动过秒 -> 退出时保留用户设的秒（见 set_exit） */

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
   原统一用 #0F172A 清线，在面板背景上残留深一档黑底）。
   主屏：HOUR/MIN/SEC 在主卡 (12,32,308,120) 内 -> PANEL；YEAR/MONTH/DAY 在日期卡下方
         (12,126,152,152 底部 152 之外 3px) -> SCREEN。
   倒计时屏：光标 y=143/225 在主卡 (12,48,308,228) 内 -> PANEL（cnt_sel_line 直接用）。
   闹钟屏：黄线 y=37/165/210 均在顶部条/主卡 (12,48,308,228) 内 -> PANEL（alm_set_line 直接用）。
   坑#84 新主屏：主屏黄线已改用「恢复背景」清线（set_line_clear，2026-10-10）——
   不再依赖固定背景色，可精确还原日期卡内渐变/描边。倒计时/闹钟屏黄线仍用各自背景色
   （cnt_sel_line / alm_set_line 内直接处理）。 */
static void set_line_clear(u8 item);   /* 前置声明（set_exit 调用，定义见下） */
void Screen_BlinkTick(void);          /* 前置声明：设置项 1Hz 闪灭（定义在 Px_DrawAlm 之前） */
static void Screen_BlinkNudge(void);  /* 前置声明：调值后立刻回亮相并重新计半秒 */
void Px_DrawAlmInvalidate(void);      /* 前置声明：作废闹钟屏增量缓存（闪灭结束后恢复真实状态） */
static void alm_swlbl_paint(void);    /* 前置声明：「闹钟开关」四个字（背景已抹掉，固件写） */
void Px_DrawAlm(void);

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
static u8  c_alm_h1 = 0xFF, c_alm_h2 = 0xFF, c_alm_m1 = 0xFF, c_alm_m2 = 0xFF;
static u8  c_alm_on = 0xFF, c_alm_loop = 0xFF, c_alm_ring = 0xFF;
static u8  c_alm_sel = 0xFF;      /* 已绘制的闹钟槽号（坑#86 粉条「闹钟N」序号增量，原左上 chip） */

/* ==================== 新 UI（像素游戏机）增量缓存（2026-11-17） ====================
   draw_countdown_view / Px_DrawAlm 一次重画整屏**动态元素**（精灵 + 变宽字串，约几十~上百次
   SPI 行事务），不能每轮主循环都跑：下面记录「上次画的内容」，只有内容/背景/气泡变化
   才重画一次（正常 ≈ 每秒 1 次）。旧 UI 的 c_cnt_* / c_alm_* 缓存保留给 #if 0 的旧绘制。 */
static u32 c_px_cnt_sec   = 0xFFFFFFFFu;  /* 上次 draw_countdown_view 的秒值（正计时=已过秒） */
static u8  c_px_cnt_state = 0xFF;         /* 上次 draw_countdown_view 的计时状态（CNT_*） */
static u8  c_px_cnt_up    = 0xFF;         /* 上次 draw_countdown_view 的模式（0=倒计时 1=正计时） */
static u16 c_px_cnt_bg    = 0xFFFF;       /* 上次 draw_countdown_view 的背景版本（g_ui_bg_ver） */
static u16 c_px_cnt_bub   = 0xFFFF;       /* 上次 draw_countdown_view 的气泡版本（g_bub_seq） */
static void draw_countdown_view(void);
static u8  c_px_alm_idx   = 0xFF;         /* 上次 Px_DrawAlm 的槽号 */
static u8  c_px_alm_hh    = 0xFF;         /* 上次 Px_DrawAlm 的时 */
static u8  c_px_alm_mm    = 0xFF;         /* 上次 Px_DrawAlm 的分 */
static u8  c_px_alm_on    = 0xFF;         /* 上次 Px_DrawAlm 的开关 */
static u8  c_px_alm_loop  = 0xFF;         /* 上次 Px_DrawAlm 的循环模式 */
static u8  c_px_alm_wd    = 0xFF;         /* 上次 Px_DrawAlm 的五/六/日 掩码（2026-11-19） */
static u16 c_px_alm_bg    = 0xFFFF;       /* 上次 Px_DrawAlm 的背景版本 */
static u16 c_px_alm_bub   = 0xFFFF;       /* 上次 Px_DrawAlm 的气泡版本 */

/* 作废倒计时屏动态层缓存：闪灭换项后要整层重画一次（同 Px_DrawAlmInvalidate 的道理，
   否则被「灭相」擦掉的那一项没人画回来）。 */
static void cnt_px_invalidate(void)
{
  c_px_cnt_sec = 0xFFFFFFFFu; c_px_cnt_state = 0xFF; c_px_cnt_up = 0xFF;
  c_px_cnt_bg = 0xFFFF; c_px_cnt_bub = 0xFFFF;
}

/* ==================== 气泡文本状态（2026-11-17 新 UI，接口见 screen.h） ====================
   三屏各一条默认文案；语音播报时 Screen_Bubble 临时覆盖，Screen_BubbleTick 8s 后恢复默认。
   绘制入口在 Px_Draw* 末尾（bubble_draw），按屏各自坐标/字号，闹钟屏窄区自动折行。 */
#define BUB_TIMEOUT_MS  8000u    /* 临时文案停留时长（8s） */
#define BUB_TEXT_MAX    64       /* 文案缓冲字节数（UTF-8；最多 60B 文本 + NUL） */
static char g_bub_text[BUB_TEXT_MAX];   /* 当前临时文案（空 = 用该屏默认文案） */
static u8   g_bub_on  = 0;              /* 1=显示临时文案（Screen_Bubble 设置） */
static u32  g_bub_ms  = 0;              /* 临时文案设置时刻（bftm0_ct，超时判定） */
static u16  g_bub_seq = 0;              /* 文案版本号（每次变更自增，绘制缓存据此重画） */
static u8   c_bub_scr = 0xFF;           /* 上次绘制气泡的屏号 */
static u16  c_bub_seq = 0xFFFF;         /* 上次绘制气泡的文案版本 */
static u16  c_bub_bg  = 0xFFFF;         /* 上次绘制气泡时的背景版本 */
static u16  c_bub_bottom = 0;          /* 上一条文案实际绘制的底边（不含此行） */

/* ==================== 小工具 ==================== */

static void disp_wake(void);
static void alm_ring_stop_core(void);

static void cnt_ring_start(void)
{
  g_cnt_pending_ring = 0;
  g_alm_set_exit_ms = 0;            /* 旧设置页的延迟返回不能带走新的响铃页 */
  printf("[CNT] 响铃：倒计时到点%s\r\n", g_alm.ringing ? "（接管正在响的闹钟）" : "");
  if (g_alm.ringing) alm_ring_stop_core();
  g_cnt_ringing = 1;
  g_cnt_ret_ms = (u32)bftm0_ct + 60000u;
  disp_wake();
  (void)UI_CharAnim_SetState(UI_CANI_STATE_ALARM);
  Voice_Send(VOICE_TX_RING, g_alm.ring_sel, 1, 0, 0);
  Screen_Bubble("时间到了");
  if (UI_GetScreen() != UI_SCREEN_CNT) UI_SetScreen(UI_SCREEN_CNT);
  g_cnt_idle_ms = (u32)bftm0_ct;
}

static int cnt_pending_ring_play(void)
{
  if (!g_cnt_pending_ring) return 0;
  g_cnt_pending_ring = 0;
  if (g_cnt != CNT_DONE || g_cnt_remain != 0u) return 0;
  cnt_ring_start();
  return 1;
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
  g_cnt_idle_ms = (u32)bftm0_ct;                  /* 回主屏：倒计时屏空闲计时重新起算 */
  if (cnt_pending_ring_play()) return;            /* 退出手动设置后的倒计时提醒 */
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

/* ==================== 顶层状态图标（2026-11-25）====================
   用户要求：「兼容版 + 闪烁固定图标素材」，图标由皮肤提供（SICO 容器，见 ui_skin.c
   Skin_DrawStatusIcons），固件只负责算状态 + 在**主屏**把它们按需亮/灭。
   皮肤没给图标位（08 内置版 / 没有 SICO 的旧镜像）就什么都不画 —— 这两类界面
   本来就没有状态位，硬画一个黑白小方块更丑（用户 2026-11-25 否掉了那版）。 */
void Screen_StatusIcons(void)
{
  if (UI_GetScreen() != UI_SCREEN_MAIN) return;   /* 图标位只在主屏 */
  u8 phase = (u8)((bftm0_ct / 500u) & 1u);        /* 0.5s 相位 */
  /* 闪烁集合：计时运行中 / 整点报时开 / 贪睡待响（贪睡闪的是闹钟图标） */
  unsigned blinking = (g_cnt == CNT_RUN ? SKIN_STATUS_TIMER : 0u) |
                      (g_hourly_on ? SKIN_STATUS_HOURLY : 0u) |
                      (g_alm.snooze ? SKIN_STATUS_ALARM : 0u);
  unsigned visible = phase ? SKIN_STATUS_ALL : (SKIN_STATUS_ALL & ~blinking);
  (void)Skin_DrawStatusIcons(visible);            /* 无 SICO：内部直接返回 0 */
}

/* ==================== 旧 UI 数字/冒号原语（奶油风，2026-11-17 新 UI 接线后停用） ====================
   这 4 个原语只被 draw_cnt / draw_alarm / draw_screen_static 里已 #if 0 的旧绘制调用，
   这里把定义也一起 #if 0（否则 -Wall 报 -Wunused-function）。
   回退旧 UI 时：把本块和那几处 #if 0 一起打开即可。
   当前倒计时进度条由 CountdownView_Draw 按布局统一绘制。 */
#if 0
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
#endif /* 旧 UI 数字/冒号原语（新 UI 停用） */

/* ==================== 坑#88 进度条胶囊圆头填充（2026-10-12 用户反馈「两侧超界」） ====================
   原直角 Display_FillRect 方角会盖出轨道两端半圆（左端 x57-65 黑弧、右端 x251-260），
   效果图填充实为胶囊：逐列橙行数 [2,9,13,15,17,18,19,20,21,22] 反推 r≈9 圆头。
   本表 = 逐行两端缩进（行 0..21 = y122..143），与效果图左端逐列逐行吻合（右端同表镜像）。
   2026-10-12 修正「没走到头」后满态 w=204，右圆头 xr=260 对称于左端。
   ⚠️ 新 UI（2026-11-17）：坐标仍是旧奶油风轨道位置，作为「辅助叠加」保留（用户要求
   进度条继续可用）；新背景若无对应轨道，可用 draw_cnt 里的 #if 0 一键关掉。 */
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

/* 用当前 RTC 的星期匹配循环模式（2026-11-19 三档：0 单次 / 1 每天 / 2 自定义）
 * 自定义模式下 一~四恒定参与，五/六/日 看掩码 bit0/1/2 —— 设计稿只有这三个图标可切换。 */
static int weekday_match(u8 loop, u8 wd_mask)
{
  TimeStruct t;
  TimeModule_GetTime(&t);
  int wd = CalcWeekday(t.year, t.month, t.day);   /* 0=周一..6=周日 */
  if (loop == 0u) return 1;                       /* 单次：下一次到点就响（响完自动关） */
  if (loop == 1u) return 1;                       /* 每天 */
  if (wd <= 3) return 1;                          /* 一~四 恒响 */
  return (int)((wd_mask >> (wd - 4)) & 1u);       /* 五/六/日 按掩码 */
}

void Screen_CntStart(u16 total_s)
{
  if (total_s == 0) return;
  cnt_ring_stop();                       /* 启动新倒计时前停残留响铃 */
  g_cnt = CNT_RUN;
  g_cnt_total  = total_s;
  g_cnt_remain = total_s;
  g_cnt_ret_ms = 0;
  g_cnt_pending_ring = 0;
  g_up = 0;                            /* 切回倒计时模式（正计时未跑则无影响） */
  g_cnt_idle_ms = (u32)bftm0_ct;       /* 计时活动：刷新倒计时屏空闲计时（坑#71） */
  if (UI_GetScreen() != UI_SCREEN_CNT) UI_SetScreen(UI_SCREEN_CNT);  /* 已在倒计时屏则不重传背景，改状态由 draw_cnt 增量刷新 */
  Voice_Send(VOICE_TX_CNT_START, 0, 0, 0, 0);   /* 坑#93：倒计时启动播"计时开始"提示音（0x0D） */
  Screen_Bubble("计时开始");                    /* 新 UI：气泡同步「计时开始」（0x0D 文案） */
}

/* 0xBA 打开正计时：从 00:00 递增，复用屏1 MM:SS；与倒计时互斥（2026-08-28） */
void Screen_UpStart(void)
{
  cnt_ring_stop();                       /* 启动正计时前停残留响铃 */
  g_cnt = CNT_RUN;
  g_cnt_total  = 0;                    /* 正计时无进度条（分母 0） */
  g_cnt_remain = 0;
  g_cnt_ret_ms = 0;
  g_cnt_pending_ring = 0;
  g_up = 1;
  g_up_sec = 0;
  g_cnt_sel = CNT_SEL_MODE;              /* 正计时含模式项：选中项强制到模式（2026-09-05 模式入光标） */
  g_cnt_adj = 0;
  g_cnt_idle_ms = (u32)bftm0_ct;       /* 计时活动：刷新倒计时屏空闲计时（坑#71） */
  if (UI_GetScreen() != UI_SCREEN_CNT) UI_SetScreen(UI_SCREEN_CNT);  /* 已在倒计时屏则不重传背景 */
  Voice_Send(VOICE_TX_CNT_START, 0, 0, 0, 0);   /* 坑#93：正计时启动播"计时开始"提示音（0x0D） */
  Screen_Bubble("计时开始");                    /* 新 UI：气泡同步「计时开始」（0x0D 文案） */
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
  g_cnt_pending_ring = 0;
  g_cnt_ret_ms = (u32)bftm0_ct + 3000u;   /* 停 3s 回主屏 */
  g_cnt_idle_ms = (u32)bftm0_ct;          /* 停止是活动：刷新空闲计时（坑#71） */
  Voice_Send(VOICE_TX_STOP_SOUND, 0, 0, 0, 0);   /* 坑#93：停止停掉正在播的声音（0x0C 声音停止） */
}

int Screen_CntActive(void)
{
  return (g_cnt == CNT_RUN || g_cnt == CNT_PAUSE) ? 1 : 0;
}

/* 主屏后台计时沙漏图标（App_Render 主屏分支每帧调，2026-09-05）：
   当倒计时后台（退出按钮回主屏后计时继续）时，在主卡右上角
   (284,48,16x16) 画沙漏（ICO1 真彩容器 UI_DrawHourglass，保留用户 PNG 多色调）。
   三态（坑#77 #5 2026-09-05 用户「暂停时符号停止闪烁，结束符号消失」）：
   运行=0.5s 亮/灭闪烁提示仍在计时；暂停=常亮不闪（计时挂后台但未走）；停止/结束/待启动=消失。
   画：UI_DrawHourglass 用设计坐标；擦：Display_FillRect 用设计坐标（主卡背景 SC_C_PANEL）。
   两者背景同为 #1B2336，透明像素填面板色 -> 擦除/重画无缝（2026-09-05 教训：别混 GRAM/设计坐标）。 */
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
#if 0
  /* 2026-11-17 新 UI：主屏无沙漏图标位（旧坐标 (84,2) + 米黄擦除色会在新背景留方块），
     绘制体 #if 0；状态缓存照旧更新，回退旧 UI 时打开即可。 */
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
#else
  c_cnt_blink_drawn = 0;   /* 新 UI：不画沙漏（计时状态在倒计时屏可见） */
  (void)on;
#endif
  c_cnt_blink_on  = on;
  c_cnt_blink_act = mode;
}

/* 计时每秒推进（Screen_OnSecond 调）：倒计时递减，正计时递增 */
static void cnt_tick(int alarm_started)
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
      g_cnt_ret_ms = 0;
      if (alarm_started) return;       /* 同一次秒节拍内，闹钟优先，倒计时结束 */
      if (g_set_active || g_alm_set_active)
      {
        g_cnt_pending_ring = 1;
        return;
      }
      cnt_ring_start();
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
   b) "打断响闹后去计时界面查看还是 0:00，未返回设定值"应回设定值）。
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
   光标常驻五项：分/秒/暂停/重置/退出（正计时只 暂停/重置/退出）。左/右切换选中 或 调整数值；
   确定=暂停/重置直接生效、分/秒进出调整态（光标黄↔蓝）、退出回主屏（计时后台继续跑）。见 Screen_HandleKey 分流。 */

/* 调整分/秒字段（自动暂停；待启动时同步 total=remain 保持进度条分母一致） */
static void cnt_adjust(int field, int delta)
{
  const u32 now = (u32)bftm0_ct;
  g_cnt_idle_ms = now;        /* 短按和长按连发都算倒计时屏操作 */
  g_cnt_sel_awake = 1;
  g_cnt_sel_awake_ms = now;   /* 调整期间保持选中态提示 */
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
  Screen_BlinkNudge();     /* 立刻把新值画出来并停闪半秒（按键反馈） */
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
    Screen_Bubble("计时开始");                    /* 新 UI：气泡同步「计时开始」（0x0D 文案） */
  }
}

/* 「重置」项确定：清零（坑#93 2026-11-04：原倒计时回初始总时长，用户实测"主人，我去休息了，有事再叫我"——改倒计时/正计时都归零 00:00 待启动；再启动需先调分/秒，坑#78 拒绝 00:00 启动） */
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
  /* 变化检测：选中项/背景版本/颜色 任一变化才重绘（2026-09-05 加颜色）——
     进出调整态时 g_cnt_sel 不变，原来直接 return 导致黄↔蓝不变色。 */
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
   UI 静态背景与正计时共用（标题固件按 g_up 动态画，坑#65）；此处 g_up 翻转。 */
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

/* 倒计时屏按键（Screen_HandleKey 分流调用）：左 KEY_ID_1 / 右 KEY_ID_3 确定=KEY_ID_2
   响铃中（g_cnt_ringing）：任意键都停铃回主屏（2026-09-05 用户需求「响闹可用按键打断」，
   与闹钟屏 screen_key_alarm 一致；此前只移动光标激活选项，无法打断——坑#70）。 */
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
  /* 2026-11-19 用户要求：常态不闪；**第一下按键只把选中态唤醒**（开始 1Hz 闪），不执行任何功能 */
  if (!g_cnt_sel_awake)
  {
    g_cnt_sel_awake    = 1;
    g_cnt_sel_awake_ms = (u32)bftm0_ct;
    printf("[KEY] 倒计时选中态唤醒（此键不执行功能）\r\n");
    return;
  }
  g_cnt_sel_awake_ms = (u32)bftm0_ct;   /* 有效动作：刷新 5s 空闲 */
  /* 左右键 */
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
    else if (g_cnt_sel >= CNT_SEL_PAUSE && g_cnt_sel < CNT_SEL_COUNT)
    {
      switch (Skin_CountdownAction(g_cnt_sel - CNT_SEL_PAUSE))
      {
        case SKIN_COUNT_TOGGLE: cnt_sel_pause_ok(); break;
        case SKIN_COUNT_START: if (g_cnt != CNT_RUN) cnt_sel_pause_ok(); break;
        case SKIN_COUNT_PAUSE: if (g_cnt == CNT_RUN) cnt_sel_pause_ok(); break;
        case SKIN_COUNT_RESET: cnt_sel_reset(); break;
        case SKIN_COUNT_EXIT: cnt_sel_exit(); break;
      }
    }
    else if (g_cnt_sel == CNT_SEL_MIN || g_cnt_sel == CNT_SEL_SEC)
    {
      if (!g_cnt_adj && g_cnt == CNT_RUN) g_cnt = CNT_PAUSE;  /* 进调整自动暂停 */
      g_cnt_adj = !g_cnt_adj;                                 /* 进出调整态（光标变色） */
    }
    return;
  }
}

/* 倒计时屏每帧增量绘制
   ==================== 2026-11-17 新 UI 接线 ====================
   动态元素改用 draw_countdown_view 画（像素游戏机风：精灵大数字/标题/按钮/机器人 + 气泡）。
   旧奶油风绘制整块 #if 0 保留便于回退（回退 = 本块 #if 1、上方新 UI 块 #if 0）。
   旧的光标绘制 cnt_sel_line() 仍在后面叠加（坐标是旧布局，位置可能不美观，
   但保证能编译、不越界；等新 UI 定稿再按新坐标重做）。 */
static void draw_cnt(TimeStruct *t)
{
  (void)t;
#if 0
  /* 顶部标签内标题：倒计时/正计时（坑#88：18x22 黑字画在粉色标签 x10-75 y9-48 内居中；
     清旧=恢复背景（UI_DrawGlyphRect 0xFFFF，标签内是纯粉渐变非纯色）；g_up 变化重画） */
  if (g_up != c_cnt_title)
  {
    UI_DrawGlyphRect(CNT_TITLE_X, 239 - (CNT_TITLE_Y + 22) + 1, 54, 22, 0xFFFFu, 0);
    UI_DrawTextInk(CNT_TITLE_X, CNT_TITLE_Y, g_up ? "正计时" : "倒计时", UI_SLOT_CHAR, SC_C_CNT_INK);
    c_cnt_title = g_up;
  }
  /* 大数字 MMM:SS（坑#88：0x40 黑 0xE100+d + 12x40 冒号 0xE304，居中 y69..108，
     坑#93（2026-11-12）：分钟 3 位支持 120 分钟，但【百位为 0 不显示】（用户实测"000:00 不好"）——
     分钟<100 用 2 位布局（12 宽居中 x160：分十 114 分个 134 冒号 154 秒十 166 秒个 186，坑#88 原布局），
     分钟≥100 用 3 位布局（12 宽居中：分百 104 分十 124 分个 144 冒号 164 秒十 176 秒个 196）；
     位数切换（99:59→100:00）恢复 112 宽数字区背景 + 冒号画新位置 + 全量重画。 */
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
     删旧状态chip——新背景无 chip 位置。缓存 c_cnt_state 语义与 g_cnt 同。 */
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

  /* 进度条（坑#88：轨道 x57-261 y119-146 在背景，填充 x57-260 y122-143 h22；按状态变色：
     运行橙 #FFB20E / 暂停黄 SC_C_YELLOW / 结束红 SC_C_RED。2026-10-12 改胶囊圆头填充：
     cnt_prog_fill 逐行水平段，替代直角 FillRect——方角盖出轨道两端半圆（用户反馈「两侧超界」）；
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
    if (filled > 0) { }   /* 新 UI：进度条在背景里（竖排 8 段），旧橙色横条停用 2026-11-17 */
    c_cnt_prog = filled;
    c_prog_bar = bar;
  }
#else
  /* 新 UI：秒值/状态/模式/背景/气泡 任一变化才整屏重画动态元素（≈每秒 1 次） */
  {
    u32 sec = g_up ? g_up_sec : (u32)g_cnt_remain;
    if (sec != c_px_cnt_sec || g_cnt != c_px_cnt_state || g_up != c_px_cnt_up ||
        g_ui_bg_ver != c_px_cnt_bg || g_bub_seq != c_px_cnt_bub)
    {
      c_px_cnt_sec   = sec;
      c_px_cnt_state = (u8)g_cnt;
      c_px_cnt_up    = g_up;
      c_px_cnt_bg    = g_ui_bg_ver;
      c_px_cnt_bub   = g_bub_seq;
      draw_countdown_view();
    }
  }

#endif

  cnt_sel_line();   /* 选择光标常驻（切项/进出调整/背景重传时清旧画新，2026-09-05）
                       ⚠️ 新 UI 下坐标仍是旧奶油风布局（y51/111/232），可能压在新背景元素上，
                       属"先跑通、后调位置"的已知项。 */
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
    Screen_Bubble("闹钟已结束");                       /* 新 UI：气泡同步「闹钟已结束」（0x04 文案） */
  }
  /* 已在闹钟屏则不重传背景：配置改动由 draw_alarm 增量刷新（大数字变化即重画），只有从别的屏切过来才整屏切换（2026-08-28） */
  if (UI_GetScreen() != UI_SCREEN_ALARM) UI_SetScreen(UI_SCREEN_ALARM);
  g_alm_idle_ms = (u32)bftm0_ct;   /* 0x8A 设置闹钟：刷新闹钟屏空闲起点（切屏由 Screen_Draw 也刷；同屏兜底） */
}

/* 0x8B 打开/关闭闹钟、0x8C 循环模式：2026-09-05 起由 Screen_HandleVoice 内联按 d0/d1/d2
   三槽解析（协议表 XX/YY/MM 每字节对应闹钟1/2/3），本函数已删除（无调用者）。
   0x8B 关闭响铃槽的停铃逻辑内联在 Screen_HandleVoice 0x8B 分支。 */

/* 1=闹钟或倒计时正在响铃。语音层的「15s 无命令 -> 回待机」要据此例外：
   单声道模块的休眠提示音会把还在响的铃声顶掉（2026-11-20 实测「闹钟只响几声就停」）。 */
int Screen_Ringing(void)
{
  return (g_alm.ringing || g_cnt_ringing) ? 1 : 0;
}

/* 结束当前闹钟及其单次状态，切屏由调用者决定。 */
static void alm_ring_stop_core(void)
{
  if (g_alm.ringing)
  {
    g_alm.ringing = 0;
    printf("[ALM] 停响：槽%u（按键/0xB1/0x8A/60s 自动）\r\n", (unsigned)g_alm_ring_idx + 1u);
    Voice_Send(VOICE_TX_RING, g_alm.ring_sel, 0, 0, 0);   /* 停播铃声 */
    (void)UI_CharAnim_SetState(UI_CANI_STATE_IDLE);       /* 停响回待机（2026-09-04 闹铃动画结束） */
  }
  alm_snooze_clear();   /* 0xB1 别响了/60s 自动停：取消可能待响的贪睡（坑#71 #9） */
  /* 2026-11-19 三档循环：单次（loop=0）响完就把该槽关掉，等于「明天叫我一次」 */
  if (g_alm_once >= 1u && g_alm_once <= 3u)
  {
    u8 oi = (u8)(g_alm_once - 1u);
    g_alm_once = 0;
    g_alm_on[oi] = 0;
    printf("[ALM] 单次闹钟响完 -> 槽%u 已自动关闭\r\n", (unsigned)(oi + 1u));
  }
}

void Screen_AlarmRingStop(void)
{
  u8 was_ringing = g_alm.ringing;
  alm_ring_stop_core();
  if (was_ringing) Screen_Bubble("闹钟已结束");   /* 新 UI：气泡同步「闹钟已结束」（0x04 文案） */
  goto_main();   /* 内部已停倒计时响铃 */
}

/* 改时间/日期后清除"本分钟已响"标志（坑#82 2026-09-05）：用户主动改时间（语音 0x81/0x82、
   按键设置退出、蓝牙授时）把时间回拨后，闹钟到点应重新判定——否则 g_alm.rung=1 残留，
   回拨后同一分钟再次到达会被 rung_min 挡掉。坑#90 的分钟绑定不替代本函数。 */
void Screen_AlarmRungReset(void)
{
  g_alm.rung = 0;
  alm_snooze_clear();
}

/* 闹钟每秒判定（Screen_OnSecond 调）：遍历 3 个闹钟槽，任一命中响铃并记录响铃槽
   ==================== 2026-11-25 坑#108：连续分钟把下一支吞掉 ====================
   用户：「连续三个闹钟 01/02/03 分，03 那个不响，不论闹钟几；隔开的正常」。
   旧顺序 =「先看响铃满 60s 没有，没满就 return」：上一支的起响时刻比整秒晚几十 ms
   （前面还有一次切屏/气泡），到下一分钟边界只过了 59935 ms < 60000 → return →
   那一刻的到点判定整段跳过，而判定只在秒=0 那一秒做 → 这支闹钟当天就再也不响。
   实测 el：08:02:00 → 60000（成功停响并响下一支）；08:03:00 → **59935**（被吞）。
   修法：**到点判定提到最前面**——本分钟有槽命中就响，哪怕上一支还在响（就地换支，
   不回主屏、不设"闹钟已结束"气泡，防闪两下）；响满 60s 的自动停退到后面兜底。 */
static void alm_ring_start(u8 index, u16 minute)
{
  if (g_alm.ringing) alm_ring_stop_core();
  cnt_ring_stop();
  g_cnt_ret_ms = 0;       /* 旧倒计时的停铃或延迟返回不能带走闹钟界面 */
  g_cnt_pending_ring = 0;
  g_alm_set_exit_ms = 0;
  g_alm_ring_idx = index;
  g_alm.ringing = 1;
  g_alm.ring_start = (u32)bftm0_ct;
  g_alm.rung = 1;
  g_alm.rung_min = minute;
  g_alm_once = g_alm_loop[index] == 0u ? (u8)(index + 1u) : 0u;
  disp_wake();
  (void)UI_CharAnim_SetState(UI_CANI_STATE_ALARM);
  if (UI_GetScreen() != UI_SCREEN_ALARM) UI_SetScreen(UI_SCREEN_ALARM);
  Voice_Send(VOICE_TX_RING, g_alm.ring_sel, 1, 0, 0);
}

/* 返回 1 表示这一秒有新闹钟起响，供同时到点的倒计时判断优先级。 */
static int alarm_tick(TimeStruct *t)
{
  u16 cur_min = (u16)(t->hour * 60u + t->minute);   /* 坑#90：rung 按分钟绑定，跨分钟自动失效 */

  /* ① 到点判定（优先级最高，坑#108） */
  if (t->second == 0)
  {
    /* 坑#79：设置模式（时间设置/倒计时分秒调整/闹钟设置）不触发任何响闹 */
    if (g_set_active || g_alm_set_active || g_cnt_adj) return 0;
    for (u8 i = 0; i < 3; i++)
    {
      if (!g_alm_on[i]) continue;                                          /* 槽关 */
      if (t->hour != g_alm_hour[i] || t->minute != g_alm_min[i]) continue;  /* 时间不匹配 */
      if (g_alm.ringing && g_alm_ring_idx == i) return 0;    /* 这一支正在响：不重入 */
      if (g_alm.rung && g_alm.rung_min == cur_min) return 0; /* 本分钟已响过：整分钟不再判 */
      if (!weekday_match(g_alm_loop[i], g_alm_wd[i])) continue;   /* 今天不匹配：看下一槽 */
      printf("[ALM] 响铃：槽%u %02u:%02u（loop=%u）\r\n", (unsigned)i + 1u,
             (unsigned)g_alm_hour[i], (unsigned)g_alm_min[i], (unsigned)g_alm_loop[i]);
      alm_ring_start(i, cur_min);
      return 1;
    }
    g_alm.rung = 0;   /* 当前分钟无任何槽命中：允许后续分钟重新判定 */
  }

  if (g_alm.ringing || g_set_active || g_alm_set_active || g_cnt_adj) return 0;
  /* 贪睡待响：到点一次性重响。 */
  if (g_alm.snooze)
  {
    if ((u32)(bftm0_ct - g_alm_snooze_ms) < 0x80000000u)   /* 时间到（unsigned 回绕安全） */
    {
      u8 i = (g_alm_ring_idx < 3) ? g_alm_ring_idx : g_alm_sel;
      alm_snooze_clear();
      if (!g_alm_on[i]) return 0;            /* 防御：贪睡槽已被关 */
      printf("[VOICE] 贪睡到点：闹钟%u 重响一次\r\n", (unsigned)i + 1);
      alm_ring_start(i, cur_min);
      return 1;
    }
    /* 坑#93：贪睡未到点不 return——贪睡期间其他闹钟到点照常响（上面①已处理） */
  }
  return 0;
}

/* 闹钟屏每帧增量绘制：显示目标槽（响铃中=响铃槽，否则当前选中槽 g_alm_sel）
   ==================== 2026-11-17 新 UI 接线 ====================
   动态元素改用 Px_DrawAlm 画（像素游戏机风：精灵大数字/开关/星期钮/机器人 + 气泡）。
   旧奶油风绘制整块 #if 0 保留便于回退。旧的设置黄线 alm_set_line() 仍在后面叠加
   （坐标是旧布局，位置可能不美观，但保证能编译、不越界）。 */
static void draw_alarm(TimeStruct *t)
{
  (void)t;
#if 0
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
     （MAX_FONT_IDX=84 满），改用「闹钟」18x22 + 序号数字 0xE000+d 16x22（深棕 SC_C_MAIN_INK，
     与设备中文风格一致），居中于粉条文字区（中心 ~107/67）；槽号变化清旧（恢复背景）重画。 */
  if (c_alm_sel != idx)
  {
    if (c_alm_sel != 0xFF)
      UI_DrawGlyphRect(80, 239 - (57 + 22) + 1, 54, 22, 0xFFFFu, 0);   /* 清旧：恢复背景 */
    UI_DrawTextInk(81, 57, "月", UI_SLOT_WEEK, SC_C_MAIN_INK);        /* 姹夊瓧 18x22 x81-116 */
    UI_DrawGlyphRect(117, 239 - (57 + 22) + 1, 16, 22, 0xE001u + idx, SC_C_MAIN_INK); /* 序号 1/2/3 */
    c_alm_sel = idx;
  }

  /* 强调线：正常蓝 / 响铃红（停用） */
 /* if (g_alm.ringing != c_alm_ring)
  {
    Display_FillRect(16, 170, 289, 2, g_alm.ringing ? SC_C_RED : SC_C_BLUE);
    c_alm_ring = g_alm.ringing;
  }*/
#else
  /* 新 UI：槽号/时间/开关/循环/背景/气泡 任一变化才整屏重画动态元素 */
  {
    u8 idx = g_alm.ringing ? g_alm_ring_idx : g_alm_sel;
    u8 hh = g_alm_hour[idx], mm = g_alm_min[idx];
    u8 on = g_alm_on[idx] ? 1 : 0;
    u8 loop = g_alm_loop[idx];
    u8 wdm = alm_show_mask(idx);   /* 与显示同源（2026-11-24）：原来用 g_alm_days 反推，语音设过就不同步 */
    if (idx != c_px_alm_idx || hh != c_px_alm_hh || mm != c_px_alm_mm || on != c_px_alm_on ||
        loop != c_px_alm_loop || wdm != c_px_alm_wd ||
        g_ui_bg_ver != c_px_alm_bg || g_bub_seq != c_px_alm_bub)
    {
      c_px_alm_idx  = idx;
      c_px_alm_hh   = hh;
      c_px_alm_mm   = mm;
      c_px_alm_on   = on;
      c_px_alm_loop = loop;
      c_px_alm_wd   = wdm;
      c_px_alm_bg   = g_ui_bg_ver;
      c_px_alm_bub  = g_bub_seq;
      Px_DrawAlm();
    }
  }
#endif

  alm_set_line();   /* 闹钟设置黄线（仅设置态画；切项/背景重传时清旧画新，2026-09-05）
                      坐标已按国风版布局重定（alm_set_geom：SEL 在「闹钟N」下、HOUR/MIN 在数字下、
                      ON 在开关下、WD5/6/7 在三个星期图标下），2026-11-19 */
}

/* ==================== 屏静态元素（进入屏时画一次） ====================
   2026-11-17 新 UI：新背景已含全部静态装饰，旧 UI 的冒号/进度条残留处理全部 #if 0；
   这里只做「缓存失效」——强制下一帧 Px_Draw* 整屏重画动态元素（背景刚重传，屏上空白）。 */
static void draw_screen_static(int scr)
{
  if (scr == UI_SCREEN_CNT)
  {
#if 0
    /* 坑#88：背景（ui_screen1.png 奶油风）已含按钮/装饰/进度条轨道/标签，静态只画冒号（12x40 黑）。
       坑#93：冒号位置随分钟位数——分钟<100（百位为 0 不显示）2 位布局冒号 154，≥100 3 位布局冒号 164 */
    draw_cnt_colon_at((g_cnt_remain / 6000) ? CNT_COL_LEFT : 154, CNT_DIG_Y);
    c_cnt_m3 = c_cnt_m2 = c_cnt_m1 = c_cnt_s1 = c_cnt_s2 = 0xFF;
    c_cnt_w3 = 0xFF;   /* 坑#93：强制首帧按当前分钟位数重画（含冒号/数字布局） */
    if (c_cnt_prog > 0)   /* 强制全绘未重传背景（模式切换 g_ui_bg_ver++ / 开显示）：旧进度条填充仍在屏上，先恢复背景（2026-10-12） */
      UI_DrawGlyphRect(CNT_PROG_X, 239 - (CNT_PROG_Y + CNT_PROG_H) + 1, c_cnt_prog, CNT_PROG_H, 0xFFFFu, 0);
    c_cnt_state = 0xFF; c_cnt_prog = -1; c_prog_bar = 0;
    c_cnt_title = 0xFF;   /* 背景重传后强制重画顶部标题（倒计时/正计时，坑#88 标签内） */
#else
    /* 新 UI：背景已重传 -> 强制 draw_countdown_view 下一帧整屏重画（旧 c_* 一并复位，便于回退） */
    c_cnt_m3 = c_cnt_m2 = c_cnt_m1 = c_cnt_s1 = c_cnt_s2 = 0xFF;
    c_cnt_w3 = 0xFF;
    c_cnt_state = 0xFF; c_cnt_prog = -1; c_prog_bar = 0; c_cnt_title = 0xFF;
    c_px_cnt_sec = 0xFFFFFFFFu; c_px_cnt_state = 0xFF; c_px_cnt_up = 0xFF;
    c_px_cnt_bg = 0xFFFF; c_px_cnt_bub = 0xFFFF;
#endif
  }
  else if (scr == UI_SCREEN_ALARM)
  {
#if 0
    draw_colon_at(SC_ALM_COL_LEFT, SC_ALM_Y, SC_C_MAIN_INK);   /* 坑#85 闹钟屏：冒号 97 深棕 */
    c_alm_h1 = c_alm_h2 = c_alm_m1 = c_alm_m2 = 0xFF;
    c_alm_on = 0xFF; c_alm_loop = 0xFF; c_alm_ring = 0xFF;
    c_alm_sel = 0xFF;   /* 强制重画「闹钟N」序号（坑#86 粉条序号由本缓存驱动） */
#else
    /* 新 UI：同上，强制 Px_DrawAlm 整屏重画 */
    c_alm_h1 = c_alm_h2 = c_alm_m1 = c_alm_m2 = 0xFF;
    c_alm_on = 0xFF; c_alm_loop = 0xFF; c_alm_ring = 0xFF;
    c_alm_sel = 0xFF;
    c_px_alm_idx = 0xFF; c_px_alm_hh = 0xFF; c_px_alm_mm = 0xFF;
    c_px_alm_on = 0xFF; c_px_alm_loop = 0xFF; c_px_alm_wd = 0xFF; c_px_alm_bg = 0xFFFF; c_px_alm_bub = 0xFFFF;
#endif
  }

  /* 旧 UI 增量缓存（c_cnt_* / c_alm_*）现在只被 #if 0 的旧绘制读取：
     这里统一读一次，防 -Wall 的 -Wunused-but-set-variable 噪声
     （回退旧 UI、把上面 #if 0 打开后，本行可删）。 */
  (void)c_cnt_m3; (void)c_cnt_m2; (void)c_cnt_m1; (void)c_cnt_s1; (void)c_cnt_s2;
  (void)c_cnt_w3; (void)c_cnt_state; (void)c_cnt_title; (void)c_cnt_prog; (void)c_prog_bar;
  (void)c_alm_h1; (void)c_alm_h2; (void)c_alm_m1; (void)c_alm_m2;
  (void)c_alm_on; (void)c_alm_loop; (void)c_alm_ring; (void)c_alm_sel;
  (void)c_cnt_blink_drawn;
}

/* ==================== 对外 API ==================== */

void Screen_Init(void)
{
  int skin_status = Skin_Load();
  int countdown_loaded = CountdownView_Init(skin_status);
  Skin_Activate(skin_status == 0 && countdown_loaded);
  if (skin_status < 0 || (skin_status == 0 && !countdown_loaded))
    printf("[UI] SKIN invalid, using built-in layout\r\n");
  const ui_cfg_t *cfg = UI_GetConfig();
  UI_PxSelfTest();          /* 像素 UI 资源自检（串口打印字库/精灵尺寸，2026-11-17） */
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
     循环默认 2（自定义）+ 掩码 7：五/六/日 全开 = 每天都响，三个图标上电即全彩色
     （2026-11-19 四档改三档；旧版默认「单次」也是全亮，视觉一致） */
  g_alm_hour[0] = cfg->alarm_hour;
  g_alm_min[0]  = cfg->alarm_min;
  g_alm_on[0]   = cfg->alarm_on ? 1 : 0;
  g_alm_hour[1] = 7; g_alm_min[1] = 0; g_alm_on[1] = 0;
  g_alm_hour[2] = 7; g_alm_min[2] = 0; g_alm_on[2] = 0;
  for (u8 i = 0; i < 3; i++) { g_alm_loop[i] = 2; g_alm_wd[i] = 7; g_alm_days[i] = 7u; }
  g_alm_once     = 0;
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

  /* 新 UI 气泡：开机文案（协议 V10 0x00 开机播报「欢迎使用语音闹钟」）。
     0x00 帧由 main.c 直接 Voice_Send 发出（不给 app_voice 回调），故在此同步气泡文案；
     8s 后 Screen_BubbleTick 自动恢复主屏默认文案「主人，有什么需要帮助的吗？」。 */
#if VOICE_ENABLE
  Screen_Bubble("欢迎使用语音闹钟");
#endif
}

/* 每帧：按当前屏增量绘制 */
/* ===== 临时验证钩子（验证完置 0 并删除）=====
   0=关；1=强制主屏时间设置（时项）；2=强制闹钟设置（开关项）；
   仿真里按键事件不触发，只能靠它把设置态摆出来看闪灭。 */
#define BLINK_TEST 0     /* 0=正式；非 0 会强制进入设置态 + 把闪灭周期缩到 200ms（仅仿真验证用） */
#if BLINK_TEST
#warning "BLINK_TEST 非 0：这是测试固件，会强制进设置态并加速闪灭，不要发出去！"
#endif

/* ===== 临时验证钩子（2026-11-20，验完置 0）=====
   伪造「正在响铃 + 贪睡待响」这个只有跨分钟才会出现的组合，用来在仿真里验证：
   ① 响铃中收到 0xEF(15s无命令) 不发休眠提示音  ② 响铃中按键一次就停铃（不被贪睡吃掉）。 */
#define RING_TEST 0
#if RING_TEST
#warning "RING_TEST 非 0：这是测试固件，会伪造响铃/贪睡状态，不要发出去！"
#endif

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
    if (scr == UI_SCREEN_ALARM) alm_swlbl_paint();   /* 背景刚重传，动态层可能被缓存跳过 -> 补画「闹钟开关」 */
    /* 闹钟屏空闲超时计时（2026-09-05）：进闹钟屏刷新起点（任何入口：语音/按键长按/命令）；
       离开闹钟屏清 0（防残留）。判断在 Screen_OnSecond（例外：设置态/响铃）。 */
    g_alm_idle_ms = (scr == UI_SCREEN_ALARM) ? (u32)bftm0_ct : 0;
    /* 倒计时屏空闲计时（坑#71）：进倒计时屏刷新起点（离开保持原值，回主屏 goto_main 重起） */
    if (scr == UI_SCREEN_CNT) g_cnt_idle_ms = (u32)bftm0_ct;
  }
  if (scr == UI_SCREEN_CNT)   draw_cnt(t);
  else if (scr == UI_SCREEN_ALARM) draw_alarm(t);
}

/* A countdown can finish or be stopped while the main page is visible. */
void Screen_Poll(void)
{
  if (g_cnt != CNT_DONE || !g_cnt_ret_ms || (u32)(bftm0_ct - g_cnt_ret_ms) >= 0x80000000u)
    return;
  g_cnt = CNT_STOP;
  g_cnt_ret_ms = 0;
  if (UI_GetScreen() != UI_SCREEN_MAIN) goto_main();
  else cnt_ring_stop();
}

/* 每秒：倒计时/正计时推进 + 闹钟判定 + 整点报时 + 设置模式超时退出 */
void Screen_OnSecond(TimeStruct *t)
{
  const int alarm_started = alarm_tick(t);
  cnt_tick(alarm_started);
  /* 先处理新到点事件，避免旧闹钟正好满 60 秒时先回主屏再切到倒计时。 */
  if (g_alm.ringing && (u32)(bftm0_ct - g_alm.ring_start) >= 60000u)
    Screen_AlarmRingStop();
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
     暂停/待启动/分秒调整态（g_cnt != RUN）空闲超时自动回主屏（用户：要能自动返回时间显示）；
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
    Screen_BubbleTime(t->hour, t->minute);   /* 新 UI：气泡同步「现在是XX点XX分」（0x07 文案） */
  }

  /* 新 UI 气泡：临时文案超时（8s）后恢复该屏默认文案（每秒一次，2026-11-17） */
  Screen_BubbleTick();
}

/* 语音模块 0xEF（15s 无命令）等超时回主屏——常态判断：默认回主屏，
   例外（设置状态/语音状态/倒计时活动屏）不回：
   - 倒计时屏活动（运行/暂停中）：不切走（倒计时屏须主动「退出」按键离开，且不打断分/秒调整态）
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
     说"稍后提醒" -> 回"设置失败" -> 响闹声没了）。 */
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
      printf("[VOICE] 倒计时分钟数非法 d0=%u（1~%u）-> 回 FAIL\r\n",
             (unsigned)f->d0, (unsigned)CNT_MIN_MAX);
      return 0;
    case VOICE_CMD_TOMATO_ON:  /* 0x88 番茄：自动 25 分钟 */
      Screen_CntStart(25u * 60u); return 1;
    case VOICE_CMD_PAUSE:      /* 0x85 */
      Screen_CntPause(); return 1;
    case VOICE_CMD_RESUME:     /* 0x86 */
      /* 坑#71 #8：结束/停止/待启动后"继续"应无效（只有暂停态能继续）。
         return 0 -> 上层回"设置失败"（好的，设置成功）。 */
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
        Screen_Bubble("闹钟已结束");               /* 新 UI：气泡同步「闹钟已结束」（0x04 文案） */
      }
      /* 关闭了贪睡待响的槽：取消贪睡（坑#71 #9：贪睡配置不变，槽关了自然不重响） */
      if (g_alm.snooze && g_alm_ring_idx < 3 && !g_alm_on[g_alm_ring_idx])
        alm_snooze_clear();
      g_alm.rung = 0;                             /* 开关变更后允许同分钟重新判定 */
      if (UI_GetScreen() != UI_SCREEN_ALARM) UI_SetScreen(UI_SCREEN_ALARM);
      g_alm_idle_ms = (u32)bftm0_ct;              /* 语音操作闹钟：刷新空闲起点（防 0xEF 后固件自动返回打断） */
      return 1;
    }
    case VOICE_CMD_ALARM_LOOP: /* 0x8C 闹钟循环模式：d0/d1/d2 = 闹钟1/2/3 循环
                                   ⚠️ 模块协议仍是**四档**（0单次/1一~五/2一~六/3一~日，见
                                  notes/voice-clock-protocol.md），固件模型是三档
                                  （0单次/1每天/2自定义）。2026-11-24 之前把 1 当成「每天」、
                                  把 3 夹成「自定义」→ 语音说「七天」结果既不是七天、屏上图标
                                  还是上次的（用户实测）。现在按「哪几天响」翻译：
                                    0 -> 单次（掩码保留，切回来还能用）
                                    1 -> 自定义 + 五            (一~五)
                                    2 -> 自定义 + 五、六         (一~六)
                                    3 -> 每天（掩码全满=7）       (一~日)
                                  统一走 alm_set_cycle，loop/wd/days 三件套一起写。 */
    {
      u8 chg = 0xFF;
      static const u8 loop_of[4] = { 0u, 2u, 2u, 1u };   /* 协议档 -> 模型档 */
      static const u8 mask_of[4] = { 0u, 1u, 3u, 7u };   /* 协议档 -> 五/六/日 掩码 */
      if (f->d0 != 0xFF) { u8 v = f->d0 & 3u; if (v == 0u) g_alm_loop[0] = 0; else alm_set_cycle(0, loop_of[v], mask_of[v]); if (chg == 0xFF) chg = 0; }
      if (f->d1 != 0xFF) { u8 v = f->d1 & 3u; if (v == 0u) g_alm_loop[1] = 0; else alm_set_cycle(1, loop_of[v], mask_of[v]); if (chg == 0xFF) chg = 1; }
      if (f->d2 != 0xFF) { u8 v = f->d2 & 3u; if (v == 0u) g_alm_loop[2] = 0; else alm_set_cycle(2, loop_of[v], mask_of[v]); if (chg == 0xFF) chg = 2; }
      if (chg != 0xFF)
      {
        g_alm_sel = chg;                          /* 显示被操作的槽（draw_alarm 增量刷新星期chips） */
        printf("[ALM] 0x8C 循环: 槽%u loop=%u mask=%u days=%u\r\n", (unsigned)chg + 1u,
               (unsigned)g_alm_loop[chg], (unsigned)alm_show_mask(chg), (unsigned)g_alm_days[chg]);
      }
      if (UI_GetScreen() != UI_SCREEN_ALARM) UI_SetScreen(UI_SCREEN_ALARM);
      g_alm_idle_ms = (u32)bftm0_ct;              /* 语音操作闹钟循环：刷新空闲起点 */
      return 1;
    }
    case VOICE_CMD_ALARM_RING: /* 0x8D 铃音选择 */
      g_alm.ring_sel = f->d0; return 1;
    case VOICE_CMD_SNOOZE:     /* 0xB0 贪睡/稍后提醒/再睡会儿（2026-09-05 重做：一次性延迟，配置不变，坑#71 #9/#14） */
    {
      /* 倒计时响闹中：贪睡【无效】——用户定（坑#71 #14）。
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
      /* 闹钟响铃中：贪睡 = 停响 + 一次性延迟 5 分钟重响（闹钟配置不变，
         贪睡待响由 alarm_tick 到点触发重响一次；重响完即结束，不再延迟）
         坑#93（2026-11-04）：间隔 10 分钟 -> 5 分钟（用户「太长了，短一点」），
         下面的 300000u 才是真值，日志/注释同步改，别再看文字以为是 10 分钟 */
      printf("[VOICE] 贪睡：闹钟%u 延迟 5 分钟重响一次（配置不变）\r\n", (unsigned)g_alm_ring_idx + 1);
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
   App 层集中分发：Driver(key.c) 只产出 KeyEvent{键号, 短/长按}，不碰业务；
   本函数按「事件类型 -> 键号 -> 当前系统状态」映射功能，易读易扩展：
     长按 KEY2(PB13) = 关屏/深休眠（系统级动作 Key_ScreenOff；任何状态都生效）
     短按 KEY2        = 非设置态：进设置；设置态：下一项；「完成」项再按：退出写回
     短按 KEY1(PB12)  = 设置态：减；非设置态：无动作
     短按 KEY3(PB14)  = 设置态：加；非设置态：无动作
     KEY1/KEY3 长按暂未绑定功能（未来可连发/其它）
   设置项循环：时 -> 分 -> 年 -> 月 -> 日 -> 完成；> 再按 KEY2 退出；坑#89 移除秒——
   主屏不显示秒，秒项光标指空，秒值保留进设置快照，退出写回不变）
   进设置：快照当前时间到 g_set_ws（显示冻结——main 传冻结值给 Calendar_Refresh）
   退出：到「完成」再按 KEY2（写回 RTC）；或 15s 无按键超时自动退出（Screen_OnSecond）
   黄线：当前项下方（时/分在时间区、年/月/日在日期卡下方且按居中公式动态定位；
   「完成」不画（黄线消失提示到末位）
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
    case SET_ITEM_HOUR:  g_set_ws.hour   = set_wrap8((int)g_set_ws.hour   + delta, 0, 23); g_set_adj_hour = 1; break;
    case SET_ITEM_MIN:   g_set_ws.minute = set_wrap8((int)g_set_ws.minute + delta, 0, 59); g_set_adj_min  = 1; break;
    case SET_ITEM_SEC:   g_set_ws.second = set_wrap8((int)g_set_ws.second + delta, 0, 59); g_set_adj_sec = 1; break;
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
  Screen_BlinkNudge();     /* 立刻把新值画出来并停闪半秒（按键反馈） */
}

/* 退出设置：清黄线 + 写回 RTC + 强制静态刷新（防时间跳变逐位翻页）+ 复位状态 */
static void set_exit(void)
{
  if (g_set_line >= 0) set_line_clear((u8)g_set_line);  /* 清黄线（恢复该处背景） */
  /* 写回规则（时/分/秒 各自"用户动过没有"）：
     ① 时、分：用户没动过的按当前 RTC 接续（进设置时是冻结快照，直接用会让时间倒退）——
        改过分 -> 分用用户设的；只改时 -> 分接续；只改日期 -> 时分都接续（2026-11-20 语义）。
     ② 秒：**用户动过秒就保留他设的秒**（2026-11-24 用户「设置完秒以后退出设置，秒设置没生效」，
        旧代码三条分支都会覆盖 g_set_ws.second：改过分归零、其余从 RTC 取 -> 秒项永远白设）；
        没动过秒才回到老规则：改过分 -> 秒归零（从整分开始计时），否则跟 RTC 接续。 */
  {
    TimeStruct now;
    TimeModule_GetTime(&now);
    if (!g_set_adj_hour && !g_set_adj_min)
    {
      g_set_ws.hour   = now.hour;
      g_set_ws.minute = now.minute;
    }
    else if (g_set_adj_hour && !g_set_adj_min)
    {
      g_set_ws.minute = now.minute;
    }
    if (g_set_adj_sec)
    {
      /* 保留用户设的秒（分也跟着用用户设的；时若没动过上面已接续） */
    }
    else if (g_set_adj_min)
    {
      g_set_ws.second = 0;
    }
    else
    {
      g_set_ws.second = now.second;
    }
  }
  TimeModule_SetTime(&g_set_ws);
  Screen_AlarmRungReset();   /* 坑#82：按键设置改时间后清"本分钟已响"（回拨后闹钟重新判定） */
  g_set_active = 0;
  g_set_item   = 0;
  g_set_line   = -1;
  g_set_adj_hour = 0;
  g_set_adj_min  = 0;
  g_set_adj_sec  = 0;
  Calendar_SetStaticNext();
  printf("[KEY] 设置退出 RTC=%04u-%02u-%02u %02u:%02u:%02u\r\n",
         (unsigned)g_set_ws.year, (unsigned)g_set_ws.month, (unsigned)g_set_ws.day,
         (unsigned)g_set_ws.hour, (unsigned)g_set_ws.minute, (unsigned)g_set_ws.second);
  (void)cnt_pending_ring_play();   /* 主屏手动设置退出后，处理延后的倒计时提醒 */
}

/* ==================== 闹钟屏按键设置系统（2026-09-05，参考时间模式） ====================
   空闲态：确定=进入设置；响铃中：确定=停铃。
   设置态：黄线选中当前项，左右调整，确定切下一项；5 项后哨兵 ALM_SET_DONE 自动退出
   （同时间模式用户改法：切到哨兵即退出，无需结束项）。设置项：
     闹钟选择(1/2/3) -> 开启关闭 -> 闹钟时 -> 闹钟分 -> 闹钟循环，作用于当前显示槽 g_alm_sel。
   时/分调整 draw_alarm 大数字增量缓存（c_alm_h*）自动重画。 */

/* 闹钟设置黄线增量绘制（draw_alarm 每帧调；背景重传/切项时清旧画新） */
static void alm_set_line(void)
{
  /* 2026-11-19：设置态不再画光标，改为「被选中项 1Hz 闪灭」（Screen_BlinkTick）。
     本函数停用，只做状态清理，防止旧线与新闪灭打架。 */
  g_alm_set_line = -1;
  g_alm_set_bg_ver = g_ui_bg_ver;
  return;
#if 0
  if (!g_alm_set_active) { g_alm_set_line = -1; g_alm_set_bg_ver = g_ui_bg_ver; return; }
  if (g_alm_set_item == (u8)g_alm_set_line && g_alm_set_bg_ver == g_ui_bg_ver)
  {
    /* 坑#93（2026-11-04）：几何未变也【只画不清】重画黄线——调整时/分大数字重绘
       （draw_alarm UI_Show 清除区）会干扰黄线（用户实测"调值蓝线短一截、进位消失"），
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
#endif
}

/* 退出闹钟设置：清黄线 + 复位状态（内存配置即改即生效，无独立写回）：
   设退出时刻：3s 后 Screen_OnSecond 自动回主屏（2026-09-05）*/
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
      case ALM_SET_DAYS:                         /* 星期循环 5/6/7 天：左右加减回绕 */
      {
        /* 起点取「屏上正在显示的」天数：语音刚设过（loop/wd 变了但 days 是旧值）时，
           按一下就该从显示的状态开始加减，而不是从记忆里的旧值跳。 */
        int cur = (int)alm_days_of_mask(alm_show_mask(g_alm_sel));
        int d = cur + delta;
        if (d > 7) d = 5;
        if (d < 5) d = 7;
        alm_set_cycle(g_alm_sel, 2u, alm_days_mask((u8)d));   /* 自定义 + 对应掩码 */
        break;
      }
      default: break;
    }
    g_alm_set_last_act = (u32)bftm0_ct;
    printf("[KEY] 闹钟设置 槽%u %02u:%02u on=%u loop=%u days=%u\r\n", (unsigned)g_alm_sel + 1,
           (unsigned)g_alm_hour[g_alm_sel], (unsigned)g_alm_min[g_alm_sel],
           (unsigned)g_alm_on[g_alm_sel], (unsigned)g_alm_loop[g_alm_sel], (unsigned)g_alm_days[g_alm_sel]);
    Screen_BlinkNudge();     /* 立刻把新值画出来并停闪半秒（按键反馈） */
    return;
  }
  if (ke->id == KEY_ID_2)                        /* 确定：下一项，到哨兵自动退出 */
  {
    g_alm_set_item++;
    /* 2026-11-19 修正：原来 loop != 2（单次/每天）时直接跳过五/六/日 -> 用户永远按不到、
       表现为"九"。现在照常进入；进入第一项时把掩码摆成"正"，
       再左右调整即自动切到自定义模式。 */
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
     清待响标志，贪睡符号停止闪烁，不再到点重响。
     坑#74：一次按压只能有一个功能——结束贪睡后立即 return，不再执行该键原有功能
     （KEY2 短按进时间设置/长按关屏、KEY1/KEY3 长按切屏等都不触发）。
     ⚠️ 2026-11-20：**响铃中不吃这一下**——贪睡待响期间别的闹钟到点响铃时贪睡标志还留着，
     第一次按键若只清贪睡，用户要按两次才停铃（实机实测「需按两次才打断」）。响铃优先，
     由下面按屏分流去停铃（Screen_AlarmRingStop 内部已 alm_snooze_clear）。 */
  if (g_alm.snooze && !g_alm.ringing)
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
          (g_cnt_sel == CNT_SEL_MIN || g_cnt_sel == CNT_SEL_SEC))   /* 倒计时分/秒调整：长按=快加快减 */
      {
        cnt_adjust((int)g_cnt_sel, delta);
        return;
      }
      if (UI_GetScreen() == UI_SCREEN_CNT && !g_cnt_adj && !g_cnt_ringing &&
          ke->id == KEY_ID_1 && ke->evt == KEY_EV_LONG && !Skin_CountdownHasExit())
        cnt_sel_exit();
    }
    return;
  }

  /* 坑#93（2026-11-04）：倒计时响铃中（倒计时屏）任意键打断——停铃 + 回设定值 + 停留倒计时屏
     （问题 8：原"叮"应留屏；问题 7：打断后回设定值而非 00:00 残留）。
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
   设置项循环：时 -> 分 -> [秒] -> 年 -> 月 -> 日 -> 完成
   （秒项只在当前皮肤显示秒时才进循环：换肤版国风主屏有秒，08 号内置版没有 -> 跳过；
    坑#89 当年是"主屏不显示秒"才移除的，2026-11-19 国风版加回秒显示后这一项又被打开了）；
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
      g_set_adj_hour = 0;
      g_set_adj_min  = 0;
      g_set_adj_sec  = 0;
      Calendar_ColonForceOn();              /* 进设置强制冒号亮（防落在闪烁"腊"相位设置期一直灭，坑#71 #3 2026-09-05） */
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
      if (g_set_item == SET_ITEM_SEC && (!Skin_Active() || !Skin_ClockHasSeconds())) ++g_set_item;
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

  /* 3) 短按 KEY1/KEY3：设置态加减；非设置态=左右键切屏（坑#93 2026-11-04）
     主屏切屏由长按改短按——KEY1 左->倒计时屏、KEY3 右->闹钟屏，仅切屏不打断。 */
  if (!g_set_active)
  {
    if (ke->id == KEY_ID_1)
    {
      printf("[KEY] 短按左 -> 倒计时屏\r\n");
      UI_SetScreen(UI_SCREEN_CNT);
      g_cnt_idle_ms = (u32)bftm0_ct;   /* 坑#83：切屏动作点立即刷新倒计时屏空闲计时（同原长按切屏，
          防主屏停留 15s 后切屏被本轮"未运行 15s 空闲自动回"/0xEF 踢回。 */
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

/* 画选中光标（2px 下划线；黄=选中、蓝=调整态）
   2026-11-17 像素游戏机 UI：新设计没有「选中下划线」这一元素，倒计时屏上会在
   (16,51) 画出一条 54x2 的蓝线压在天穹上（截图对比实测），故整体停用。 */
static void cnt_sel_draw(u8 item, u16 color)
{
  int x, w, y;
  (void)item; (void)color;
  cnt_sel_geom(item, &x, &w, &y);
  if (w <= 0) return;
  /* 停用：不画线 */
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

/* 闹钟设置光标（坑#85 3px，2026-11-03 由深棕改蓝 SC_C_BLUE，2026-11-19 按国风版布局重定）：
   SEL        「闹钟N」标签下方 y72（标签墨迹 57..70）；
   HOUR/MIN   大数字下方 y74（数字墨迹底 71）；
   ON         开关下方 y75（开关 48..72）；
   WD5/6/7    五/六/日 图标下方 y150（图标 104..148），分别只在各自图标宽度内；
   清线用恢复背景精确还原。 */
static void alm_set_geom(u8 item, int *x, int *w, int *y)
{
  switch (item)
  {
    case ALM_SET_SEL:  *x = 176; *w = 44;  *y = 72;  break;
    case ALM_SET_HOUR: *x = 25;  *w = 66;  *y = 74;  break;
    case ALM_SET_MIN:  *x = 104; *w = 64;  *y = 74;  break;
    case ALM_SET_ON:   *x = 245; *w = 60;  *y = 75;  break;
    case ALM_SET_DAYS: *x = 182; *w = 129; *y = 150; break;
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
   否则 g_set_line 缓存认为"分"不重绘，黄线会一直消失。
   坑#89：日期光标几何随月/日位数浮动（居中），除切项/重传外还要检测【几何变化】
   （如 9 月->10 月，位数 1->2，日期重居中、数字列右移）→ 几何变了清旧画新。 */
void Screen_SetupLine(void)
{
  /* 2026-11-19 用户要求：设置态不再画任何光标/下划线，改成「被选中项 1Hz 闪灭」
     （见 Screen_BlinkTick）。换皮肤时不用再适配光标图形，故整个函数停用，只保留
     状态清理，防止旧光标残留在屏上。 */
  g_set_line = -1;
  g_set_line_x = -1;
  g_set_bg_ver = g_ui_bg_ver;
  return;
#if 0
  int x, w, y;
  set_line_geom(g_set_item, &x, &w, &y);
  if (!g_set_active) { g_set_line = -1; g_set_line_x = -1; g_set_bg_ver = g_ui_bg_ver; return; }
  if (g_set_item == (u8)g_set_line && x == g_set_line_x && w == g_set_line_w
      && g_set_bg_ver == g_ui_bg_ver)
  {
    /* 坑#92（2026-11-03）：几何未变也【只画不清】重画光标——日期/农历重绘
       （设置模式调整月/日、或跨天时 Calendar_Refresh 里 month/day 变 -> 农历
       UI_Show(202,76) 清除区 y76-98 覆盖日期光标 y78-81）会把光标抹掉；而此处
       原来几何未变直接 return 不重画 -> 光标「有时消失」。位置没变，直接叠线
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
#endif
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




/* ============================================================================
 * 像素游戏机 UI（08 号设计）三屏绘制层 —— 2026-11-17 新增，2026-11-17 接线
 * 只依赖 ui.h 的 UI_DrawSprite / UI_DrawStrPx / UI_StrPxWidth；
 * 坐标全部是设计坐标（y=0 顶）。接线点：
 *   Calendar_Refresh -> Px_DrawMain（秒变化才重绘）
 *   draw_cnt         -> draw_countdown_view （秒/状态/模式变化才重绘）
 *   draw_alarm       -> Px_DrawAlm （槽/时间/开关/循环变化才重绘）
 * 旧奶油风绘制在对应位置 #if 0 保留，便于对比回退。
 * ========================================================================== */
#define PX_COL_WHITE  UI_RGB(252, 252, 255)
#define PX_COL_INK    UI_RGB(72, 34, 10)     /* 国风深棕：日期/温湿度/闹钟气泡字（设计稿取样 78,37,14） */
#define PX_COL_ALM_CARD UI_RGB(243, 223, 197) /* 闹钟右上卡片纸色（标签周边取样）。2026-11-20 起
                                                「闹钟开关」闪灭改用墨迹形状贴片 sw_lbl_off，这里只剩参考值 */
#define PX_COL_CARD   UI_RGB(245, 225, 200)  /* 倒计时卡片纸色（取样 247,228,205 / 244,222,197）：闪灭时平涂盖按钮用 */
#define PX_COL_BUB    UI_RGB(150, 120, 95)   /* 主屏气泡浅棕字（设计稿底色淡，取样 156,130,108） */

/* ==================== 国风（新ui界面 2026-11-19）布局常量 ====================
 * 全部用「设计坐标」（顶原点）。数值来源：三屏「全显稿 vs 抠图稿」求差 + 墨迹外接框实测，
 * 见 notes/ui-skin-新ui-20261119.md。换风格时只需改这一段 + ui.h 的精灵宏。 */
#define PX_MAIN_DIG_Y     15    /* 大数字墨迹顶（墨迹 41 高） */
#define PX_MAIN_H_X       81    /* 时十位 */
#define PX_MAIN_H2_X     111    /* 时个位 */
#define PX_MAIN_COL_X    145    /* 冒号（9 宽） */
#define PX_MAIN_M_X      158    /* 分十位 */
#define PX_MAIN_M2_X     188    /* 分个位 */
#define PX_MAIN_SEC_Y     31    /* 秒墨迹顶（16 高） */
#define PX_MAIN_SEC_X    228    /* 秒十位（位距 11） */
#define PX_MAIN_DATE_X    74   /* 文楷 12px 的 10 字符串宽 64（设计稿 56），左移 5px 免得压到右侧分隔线 */
#define PX_MAIN_WEEK_X   161
#define PX_MAIN_LUNAR_X  230   /* 48 宽 → 230..278，卡片右边框在 282 */
#define PX_MAIN_ROW_Y     68    /* 日期/星期/农历：文楷 12px 墨迹顶 y+3（数字也是 y+3）→ 71 落到设计稿的 71 */
#define PX_MAIN_TEMP_X    82    /* 「温度」标签 x82..101 */
#define PX_MAIN_HUMI_X   214    /* 「湿度」标签 x214..233 */
#define PX_MAIN_TEMPV_X  115    /* 温度数值（23°C）起点，设计稿实测 */
#define PX_MAIN_HUMIV_X  246    /* 湿度数值（58%）起点 */
#define PX_MAIN_TH_Y      98    /* 「温度/湿度」标签 12px，墨迹顶 y+3 → 101（设计稿 101..110） */
#define PX_MAIN_THV_Y     97    /* 数值 16px（°C 拆开写，与设计稿 23°C 同高），墨迹 100..112 */
#define PX_MAIN_AITITLE_X 109   /* 「AI语音助手」标题（纯净版没抠到，固件补画） */
#define PX_MAIN_AITITLE_Y 131   /* 12px 汉字墨迹顶 y+3 → 131 落到 134，设计稿 133..143 */

#define PX_ALM_DIG_Y      26    /* 闹钟数字墨迹顶（46 高） */
#define PX_ALM_H_X        27
#define PX_ALM_H2_X       60
#define PX_ALM_COL_X      92    /* 冒号（11 宽） */
#define PX_ALM_M_X       106
#define PX_ALM_M2_X      136
#define PX_ALM_TAG_X     214    /* 槽号精灵（11x14，墨迹居中 → 216..221），与 baked「闹钟」留出设计稿那样的间隔 */
#define PX_ALM_TAG_Y      57   /* 槽号改成精灵（11x14）后按坐标直贴：墨迹 57..70，与 baked「闹钟」同基线 */
#define PX_ALM_SW_X      247    /* 开关 57x25（开=蓝 / 关=灰） */
#define PX_ALM_SW_Y       48
/* 可切换的三个星期图标（一~四 恒亮，不用画） */
#define PX_ALM_TG_Y      104

/* 「闹钟开关」四个字：新版背景已把这四个字抹掉（闹钟界面_空标签.png），由固件写。
   这样设置态闪灭 = 恢复背景（字消失）/ 重画（字出现），开关本体不参与闪灭。 */
#define PX_ALM_SWLBL_X  244
#define PX_ALM_SWLBL_Y  23   /* 贴片 68x20 实际落在 (243,24) */

static void alm_swlbl_paint(void)
{
  /* 回退：不走贴片（落点不对），见 blink_apply 的平涂遮挡 */
}

#define PX_ALM_TG_X5     184    /* 五 43x44 */
#define PX_ALM_TG_X6     227    /* 六 40x44 */
#define PX_ALM_TG_X7     267    /* 日 42x44 */

/* ============================================================================
 * 气泡文本状态机 + 绘制（2026-11-17 新 UI，D 项）
 *
 * 三屏默认文案：
 *   主屏「主人，有什么需要帮助的吗？」  倒计时「主人，开启国风倒计时吧！」
 *   闹钟「主人，需要设置闹钟吗？」
 * 语音播报（0x00/0x01/0x02/0x03/0x04/0x07/0x09/0x0D/0x0E/0x0F）时由
 * app_voice.c / screen.c 调 Screen_Bubble 覆盖，8s 后 Screen_BubbleTick 恢复默认。
 *
 * 可用区（设计坐标，字色深色 PX_COL_INK）：
 *   主屏   ：x 112..292，y 154，13px（UI_PX_13）
 *   倒计时：x  82..278，y 188，13px（UI_PX_13）
 *   闹钟   ：x 252..316，y 108 起，12px（UI_PX_12）—— 只有 64px 宽，必须按宽度折行（最多 3 行）
 * ========================================================================== */

/* 该屏默认气泡文案 */
static const char *bub_default(int scr)
{
  const SkinBubble *bubble = Skin_GetBubble((unsigned)scr);
  if (bubble) return bubble->text;
  if (scr == UI_SCREEN_CNT)       return "主人，开启国风倒计时吧！";   /* 国风版文案（旧版是「冒险」） */
  if (scr == UI_SCREEN_ALARM)     return "主人，需要设置闹钟吗？";
  return "主人，有什么需要帮助的吗？";
}

/* UTF-8 单字符字节数（1..4；非法首字节按 1 处理，保证不卡死） */
static int bub_chlen(const char *s)
{
  u8 c = (u8)s[0];
  if (c < 0x80u) return 1;
  if ((c & 0xE0u) == 0xC0u) return 2;
  if ((c & 0xF0u) == 0xE0u) return 3;
  if ((c & 0xF8u) == 0xF0u) return 4;
  return 1;
}

/* 算「从 p 开始这一行能放多少」：返回字节数，*pxw 给该行像素宽。
   规则与 bub_wrap_draw 一致：单字超宽也放（不无限换行），缺字按 12px 占位。 */
static int bub_line_span(const char *p, int maxw, u32 base, int *pxw)
{
  char ch[8];
  int n = 0, w = 0, k, cw;
  while (p[n] != 0 && p[n] != '\n')
  {
    int cl = bub_chlen(p + n);
    for (k = 0; k < cl && p[n + k] != 0; k++) ch[k] = p[n + k];
    ch[k] = 0;
    cw = UI_StrPxWidth(ch, base);
    if (cw <= 0) cw = 12;
    if (n > 0 && (w + cw) > maxw) break;     /* 本行放不下 → 换行 */
    w += cw;
    n += cl;
  }
  *pxw = w;
  return n;
}

/* ------------------------------------------------------------------
 * bub_wrap_draw：按可用宽度逐字折行绘制（CJK 按实际字宽累加，超宽换行）。
 * 参数: x,y=左上角（设计坐标）；s=文本；maxw=可用宽度 px；maxlines=最多行数；
 *       base=字号档位（UI_PX_*）；col=字色；lh=行高（px）；center=1 时每行水平居中
 * 返回: 实际绘制的底边（不含此行）；没有绘制字模时返回 0。
 * 说明: center=1 用于闹钟屏的气泡——它是椭圆，左对齐会让首行第一个字压到左上弧线外；
 *       逐行居中后两行左右对称，和设计稿一致（2026-11-19）。
 * ------------------------------------------------------------------ */
static int bub_wrap_draw(int x, int y, const char *s, int maxw, int maxlines,
                         u32 base, u16 col, int lh, int center)
{
  char ch[8];
  const char *p = s;
  int line = 0;
  int drawn_bottom = 0;

  while (*p != 0 && line < maxlines)
  {
    int linew = 0;
    if (*p == '\n') { ++p; ++line; continue; }
    int nb = bub_line_span(p, maxw, base, &linew);
    int cx = center ? (x + (maxw - linew) / 2) : x;
    const int line_y = y + line * lh;
    int i = 0;

    if (nb <= 0) break;
    while (i < nb)
    {
      int cl = bub_chlen(p + i), k, cw;
      for (k = 0; k < cl && p[i + k] != 0; k++) ch[k] = p[i + k];
      ch[k] = 0;
      cw = UI_StrPxWidth(ch, base);
      if (cw <= 0) cw = 12;
      if (cx + cw > x + maxw) break;              /* 屏幕右缘保护：UI_DrawGlyphRect 不裁切 */
      const int height = UI_DrawStrPxBoundedHeight(cx, line_y, ch, base, col, x + maxw);
      if (height && line_y + height > drawn_bottom) drawn_bottom = line_y + height;
      cx += cw;
      i += cl;
    }
    p += nb;
    if (*p == '\n') ++p;
    line++;
  }
  return drawn_bottom;
}

/* ------------------------------------------------------------------
 * bubble_paint：某屏气泡区「先整块恢复背景，再折行重画」。
 * 参数: scr=屏号（UI_SCREEN_*）；s=文案（非空）
 * 说明: 字模实际高度可以超过行高。还原背景时覆盖上一条文案的实际底边，
 *       再按该屏原有坐标、字号和行高折行重画。
 * 国风版（2026-11-19，椭体）：
 *   主屏   ：浅棕字 13px，气泡内区 x117..307 / 墨迹顶 152
 *   倒计时：深棕气泡上的白字 12px，x99..245 / 墨迹顶 196
 *   闹钟   ：深棕字 14px，椭圆气泡内区 x143..231 折两行、行距 18。**逐行居中**
 *   ☆ 自动降档：文案太长（放不进 maxlines 行）时整块降一档字号，宁可小一点也不要截断/顶出气泡。
 * ------------------------------------------------------------------ */
static int bubble_paint(int scr, const char *s, int previous_bottom)
{
  int x, y, w, lh, maxlines, h, center;
  u32 base, small;
  u16 col;

  /* 字体的墨迹顶比 y 低 2px（椭体度量），下列 y 都是「目标墨迹顶 - 2」。 */
  const SkinBubble *page_bubble = Skin_GetBubble((unsigned)scr);
  if (page_bubble || scr == UI_SCREEN_CNT)
  {
    const CountdownBubbleLayout *bubble = page_bubble ? &page_bubble->layout : CountdownView_Bubble();
    if (!bubble->lines || !bubble->width) return 0;
    x = bubble->x; y = bubble->y; w = bubble->width;
    base = (u32)bubble->font << 16; small = (u32)bubble->small_font << 16;
    lh = bubble->line_height; maxlines = bubble->lines;
    col = bubble->color; center = bubble->centered;
  }
  else if (scr == UI_SCREEN_ALARM) { x = 143; y = 162; w = 88; base = UI_PX_14; small = UI_PX_12; lh = 18; maxlines = 2; col = PX_COL_INK; center = 1; }
  /* 闹钟气泡内区实测（抠图背景）：左右描边 x137..138 / x235，圆角处收窄到 143..231；
     上下 y152..198。文字安全框约 x142..229 / y158..196（14px 楷体 6 字 2 行 = 84px）。
     与设计稿「主人，需要 / 设置闹钟吗？」同样的 6+5 折行 */
  else                             { x = 117; y = 147; w = 190; base = UI_PX_13; small = UI_PX_12; lh = 17; maxlines = 2; col = PX_COL_BUB; center = 0; }

  /* 自动降档：按「总宽 > 可用宽 × 行数」判断放不下，降一档再算（长度仍超就按 maxlines 截断） */
  if (base != small && UI_StrPxWidth(s, base) > w * maxlines)
  {
    base = small;
    if (lh > 16) lh -= 2;                  /* 小一档行距也收一点，避免第二行贴气泡底边 */
  }

  if (x < 0) x = 0;
  if (y < 0) y = 0;
  h = lh * maxlines;
  if (previous_bottom > y + h) h = previous_bottom - y;
  if (x + w > 320) w = 320 - x;
  if (y + h > 240) h = 240 - y;
  if (w <= 0 || h <= 0) return 0;

  UI_DrawGlyphRect(x, 239 - (y + h) + 1, w, h, 0xFFFFu, 0);   /* 只还原背景（清旧文字） */
  return bub_wrap_draw(x, y, s, w, maxlines, base, col, lh, center);
}

/* ------------------------------------------------------------------
 * bubble_draw：按当前屏画气泡（文本/屏/背景版本变化才重绘）。
 * 由 Px_DrawMain / draw_countdown_view / Px_DrawAlm 末尾调用（它们的调用点已按秒/状态节流）。
 * ------------------------------------------------------------------ */
static void bubble_draw(void)
{
  int scr = UI_GetScreen();
  const char *s;

  if (scr == (int)c_bub_scr && g_bub_seq == c_bub_seq && g_ui_bg_ver == c_bub_bg) return;
  const int previous_bottom = scr == (int)c_bub_scr ? c_bub_bottom : 0;
  c_bub_scr = (u8)scr;
  c_bub_seq = g_bub_seq;
  c_bub_bg  = g_ui_bg_ver;
  s = (g_bub_on && g_bub_text[0] != 0) ? g_bub_text : bub_default(scr);
  c_bub_bottom = (u16)bubble_paint(scr, s, previous_bottom);
}

/* 设置气泡文本（NULL/空 -> 恢复该屏默认文案），见 screen.h */
void Screen_Bubble(const char *s)
{
  int i = 0, cl, k;

  if (s == 0 || s[0] == 0)
  {
    g_bub_on = 0;
    g_bub_text[0] = 0;
  }
  else
  {
    /* 按完整 UTF-8 字符拷贝（留 4 字节余量，防截断成半个汉字） */
    while (s[i] != 0 && i < BUB_TEXT_MAX - 4)
    {
      cl = bub_chlen(s + i);
      for (k = 0; k < cl && s[i + k] != 0; k++) g_bub_text[i + k] = s[i + k];
      i += k;
      if (k < cl) break;
    }
    g_bub_text[i] = 0;
    g_bub_on = 1;
    g_bub_ms = (u32)bftm0_ct;
  }
  g_bub_seq++;      /* 版本号自增：本屏下一次 Px_Draw* 会重画气泡 */
}

/* 每秒调一次：临时文案超时（8s）恢复默认文案，见 screen.h */
void Screen_BubbleTick(void)
{
  if (!g_bub_on) return;
  if ((u32)(bftm0_ct - g_bub_ms) < BUB_TIMEOUT_MS) return;
  g_bub_on = 0;
  g_bub_text[0] = 0;
  g_bub_seq++;
  printf("[UI] 气泡超时 -> 恢复默认文案\r\n");
}

/* 气泡显示「现在是XX点XX分」（0x07 报时文案），见 screen.h */
void Screen_BubbleTime(u8 hh, u8 mm)
{
  char b[24];
  const char *p;
  int n = 0;

  p = "现在是";  while (*p != 0) b[n++] = *p++;
  b[n++] = (char)('0' + (hh / 10u) % 10u);
  b[n++] = (char)('0' + hh % 10u);
  p = "点";      while (*p != 0) b[n++] = *p++;
  b[n++] = (char)('0' + (mm / 10u) % 10u);
  b[n++] = (char)('0' + mm % 10u);
  p = "分";      while (*p != 0) b[n++] = *p++;
  b[n] = 0;
  Screen_Bubble(b);
}

static void px_digit(int x, int y, u8 v, int style)
{
  if (v > 9u) return;
  UI_DrawSprite(UI_SPR_DIGIT(style, v), x, y);
}

/* 字符串居中：cx=中心 x（国风版三屏的静态文字都 baked 在背景里，暂无调用者；
   保留给后续可能的居中标题使用，用 __attribute__((unused)) 压掉 -Wall 告警） */
static void px_str_c(int cx, int y, const char *s, u32 base, u16 col) __attribute__((unused));
static void px_str_c(int cx, int y, const char *s, u32 base, u16 col)
{
  UI_DrawStrPx(cx - UI_StrPxWidth(s, base) / 2, y, s, base, col);
}

static void draw_skin_robot(unsigned page)
{
  Skin_Robot(page, g_cani.cur_state, g_cani.cur_frame);
}

static void draw_skin_main(const TimeStruct *t, int hidden)
{
  char lunar[20] = {0}, temp[12], humidity[12];
  if (Calendar_GetLunar(t, lunar, sizeof(lunar)) != 0) lunar[0] = 0;
  int sensors_valid = Calendar_GetTempHumi(temp, sizeof(temp), humidity, sizeof(humidity)) == 0;
  int weekday = CalcWeekday(t->year, t->month, t->day);
  Skin_Clock(t, weekday, lunar, sensors_valid ? temp : NULL, sensors_valid ? humidity : NULL, hidden);
}

/* ---------------- 主屏 ---------------- */
void Px_DrawMain(const TimeStruct *t)
{
  if (Skin_Active())
  {
    draw_skin_main(t, Screen_BlinkHiddenId());
    draw_skin_robot(UI_SCREEN_MAIN);
    bubble_draw();
    return;
  }
  char b[16];
  u8 hh = t->hour, mm = t->minute;

  /* 大数字 HH:MM（主屏 style=0，单元格 29x41）：设计稿实测 81/111/冒号 145/158/188，墨迹顶 y15 */
  px_digit(PX_MAIN_H_X,  PX_MAIN_DIG_Y, (u8)(hh / 10u), 0);
  px_digit(PX_MAIN_H2_X, PX_MAIN_DIG_Y, (u8)(hh % 10u), 0);
  UI_DrawSprite(UI_SPR_COLON(0), PX_MAIN_COL_X, PX_MAIN_DIG_Y);
  px_digit(PX_MAIN_M_X,  PX_MAIN_DIG_Y, (u8)(mm / 10u), 0);
  px_digit(PX_MAIN_M2_X, PX_MAIN_DIG_Y, (u8)(mm % 10u), 0);

  /* 秒（两位，精灵 161..170 的 0-9，单元格 12x17，位距 11，墨迹顶 y28） */
  {
    u8 ss = t->second;
    if (ss > 59u) ss = 0u;
    UI_DrawSprite(UI_SPR_SEC(ss / 10u), PX_MAIN_SEC_X,     PX_MAIN_SEC_Y);
    UI_DrawSprite(UI_SPR_SEC(ss % 10u), PX_MAIN_SEC_X + 11, PX_MAIN_SEC_Y);
  }

  /* 日期 2026-11-03：12px 深棕，墨迹顶 y71 */
  b[0] = '2'; b[1] = '0'; b[2] = (char)('0' + (u8)((t->year / 10u) % 10u)); b[3] = (char)('0' + (u8)(t->year % 10u));
  b[4] = '-';
  b[5] = (char)('0' + (u8)(t->month / 10u)); b[6] = (char)('0' + (u8)(t->month % 10u));
  b[7] = '-';
  b[8] = (char)('0' + (u8)(t->day / 10u));   b[9] = (char)('0' + (u8)(t->day % 10u)); b[10] = 0;
  UI_DrawStrPx(PX_MAIN_DATE_X, PX_MAIN_ROW_Y, b, UI_PX_12, PX_COL_INK);

  /* 星期（CalcWeekday: 1=周一 .. 7=周日）*/
  {
    static const char *wk[7] = { "星期一", "星期二", "星期三", "星期四", "星期五", "星期六", "星期日" };
    int wd = CalcWeekday((int)t->year, (int)t->month, (int)t->day);
    if (wd >= 0 && wd <= 6) UI_DrawStrPx(PX_MAIN_WEEK_X, PX_MAIN_ROW_Y, wk[wd], UI_PX_12, PX_COL_INK);
  }

  /* 农历 + 温湿度（2026-11-17 接线）：农历由 calendar.c 的取农历接口生成
     （Calendar_GetLunar 只输出「九月初四」月日，**不含「农历：」前缀**）；
      Calendar_GetTempHumi 输出「25.5」「60.0」，取最近一次 Calendar_Refresh 的采样值）。
     国风版坐标（2026-11-19）：农历在日期卡右段 x234；温湿度进各自小卡，
     数值 14px、单位单独按设计稿 x 定位（设计稿单位与数值间有固定空档）。 */
  {
    char lb[20];
    char tb[12], hb[12];

    if (Calendar_GetLunar(t, lb, (int)sizeof(lb)) == 0 && lb[0] != 0)
      UI_DrawStrPx(PX_MAIN_LUNAR_X, PX_MAIN_ROW_Y, lb, UI_PX_12, PX_COL_INK);

    if (Calendar_GetTempHumi(tb, (int)sizeof(tb), hb, (int)sizeof(hb)) == 0)
    {
      /* 「温度」「湿度」两个标签**纯净版背景里也被抠掉了**（实测 抠图 vs 全显 差分：
         温度 x82..101 / 湿度 x214..233，y101..110），必须固件补画，否则卡片左半边是空的。
         数值在标签右侧：温度 23°C 起点 x115、湿度 58% 起点 x246（设计稿实测），单位紧跟数字。 */
      char s[16];
      int n = 0, k;
      /* 单位用「°C」而不是合成字符 U+2103「℃」：楷体的 ℃ 是缩小版合成字，
         14px 时里面的 C 只有 7px 高，屏上像个小逗号（用户就是看着这个说「缺字」）。
         拆成 ° + C 后 C 是满高字母、圆圈也清楚，和设计稿的 23°C 一个观感（16px）。 */
      const char *uc = "\xC2\xB0" "C";      /* UTF-8: ° C */
      UI_DrawStrPx(PX_MAIN_TEMP_X, PX_MAIN_TH_Y, "温度", UI_PX_12, PX_COL_INK);   /* 标签墨迹 101..110 */
      UI_DrawStrPx(PX_MAIN_HUMI_X, PX_MAIN_TH_Y, "湿度", UI_PX_12, PX_COL_INK);

      while (tb[n] != 0 && tb[n] != '.' && n < 6) { s[n] = tb[n]; n++; }
      for (k = 0; uc[k] != 0; k++) s[n + k] = uc[k];
      s[n + k] = 0;
      UI_DrawStrPx(PX_MAIN_TEMPV_X, PX_MAIN_THV_Y, s, UI_PX_16, PX_COL_INK);   /* 23°C */

      n = 0;
      while (hb[n] != 0 && hb[n] != '.' && n < 6) { s[n] = hb[n]; n++; }
      s[n] = '%'; s[n + 1] = 0;
      UI_DrawStrPx(PX_MAIN_HUMIV_X, PX_MAIN_THV_Y, s, UI_PX_16, PX_COL_INK);   /* 58% */
    }
  }

  /* 「AI语音助手」标题：纯净版背景把这块也抠掉了（全显稿才有），固件补画。
     设计稿实测墨迹 x109..176 / y132..143，14px 深棕 */
  UI_DrawStrPx(PX_MAIN_AITITLE_X, PX_MAIN_AITITLE_Y, "AI语音助手", UI_PX_12, PX_COL_INK);
  Display_FillRect(111, 147, 71, 2, PX_COL_INK);   /* 标题下的短横线（设计稿 x111..181 y147..148） */

  /* 机器人（落点由素材 GIF 自带，精灵索引里的 x/y） */
  UI_DrawSpriteAuto(UI_SPR_ROBOT(0, (g_cani.cur_state == UI_CANI_STATE_WAKE) ? 1u : 0u, g_cani.cur_frame % 16u));

  /* 气泡文字（最后画：盖在机器人/文字之上，2026-11-17 D 项） */
  bubble_draw();
}

static CountdownViewState countdown_view_state(void)
{
  return (CountdownViewState){g_up ? g_up_sec : (u32)g_cnt_remain,
                              g_cnt_total, g_up, g_cnt == CNT_RUN};
}

static void draw_countdown_view(void)
{
  const CountdownViewState state = countdown_view_state();
  const u8 id = Screen_BlinkHiddenId();
  const CountdownItem hidden = (id >= 0x20u && id < 0x20u + CNT_SEL_COUNT)
      ? (CountdownItem)(id - 0x20u) : CNT_SEL_COUNT;
  CountdownView_Draw(&state, hidden);

  if (Skin_Active()) draw_skin_robot(UI_SCREEN_CNT);
  else UI_DrawSpriteAuto(UI_SPR_ROBOT(1, (g_cani.cur_state == UI_CANI_STATE_WAKE) ? 1u : 0u,
                                      g_cani.cur_frame % 16u));
  bubble_draw();
}

/* ==================== 设置项闪灭（1Hz）—— 替代光标（2026-11-19 用户要求） ===================
 * 缘起：后续要频繁换 UI，「光标/下划线」是最难适配的元素（形状、颜色、位置、与底图明暗都相关，
 *       每套皮肤都得重调一遍几何）。改成「被选中项自己闪灭」后，屏上不再出现任何皮肤相关的
 *       指示图形，换皮肤只要改布局常量，不用再写光标。
 * 交互（统一）：中键短按 = 进设置 / 切下一项 / 走完退出；设置态左键减、右键加；中键长按仍是关屏。
 * 实现：灭相 = UI_DrawGlyphRect(0xFFFF) 把该项矩形恢复成背景（不填色，避免留痕）；
 *       亮相 = 只把该项自己重画一遍（不整屏重刷，绝不闪整屏）。
 * 矩形：全部由布局常量 + UI_StrPxWidth 推导（日期按字宽算），不写死像素坐标。 */
#if BLINK_TEST
#define BLINK_HALF_MS   200u   /* 仅测试：仿真时钟 2.04s 就冻结，缩短便于抓两个相位 */
#else
#define BLINK_HALF_MS   500u   /* 半周期：500ms 亮 + 500ms 灭 = 1Hz */
#endif
#define BLINK_NONE      0xFFu

static u8  g_blink_on   = 1;     /* 当前相位：1=亮 0=灭 */
static u32 g_blink_ms   = 0;     /* 上次翻转时刻 */
static u8  g_blink_scr  = 0xFF;  /* 当前闪的屏 */
static u8  g_blink_id   = BLINK_NONE;
static s16 g_blink_x, g_blink_y, g_blink_w, g_blink_h;

/* 当前的「被选中项」编号：主屏 0..7（SET_ITEM_*），闹钟 0x10+0..7（ALM_SET_*）；255=不在设置态 */
static u8 blink_cur_id(void)
{
  if (UI_GetScreen() == UI_SCREEN_MAIN && Screen_SetupActive())  return (u8)g_set_item;
  if (UI_GetScreen() == UI_SCREEN_ALARM && g_alm_set_active)     return (u8)(0x10u + g_alm_set_item);
  if (UI_GetScreen() == UI_SCREEN_CNT)                           /* 常态不闪：唤醒后才闪 */
    return g_cnt_sel_awake ? (u8)(0x20u + g_cnt_sel) : BLINK_NONE;
  return BLINK_NONE;
}

static SkinAlarmState alarm_view_state(void)
{
  u8 index = g_alm.ringing ? g_alm_ring_idx : g_alm_sel;
  if (index > 2u) index = 0u;
  /* 五/六/日 图标与响铃判定同源（alm_show_mask），不再拿 g_alm_days 反推（2026-11-24） */
  u8 mask = alm_show_mask(index);
  u8 days = alm_days_of_mask(mask);
  return (SkinAlarmState){index, g_alm_hour[index], g_alm_min[index], g_alm_on[index],
                          0x0Fu | (mask << 4), days};
}

/* Layout-driven views derive blink rectangles from the same geometry used to draw them. */
static int blink_rect(u8 id, s16 *x, s16 *y, s16 *w, s16 *h)
{
  if (Skin_Active() && id < 0x20u)
  {
    SkinRect rect;
    int valid;
    if (id < 0x10u) valid = Skin_ClockItemRect(&g_set_ws, (SkinClockField)id, &rect);
    else
    {
      const SkinAlarmState state = alarm_view_state();
      valid = Skin_AlarmItemRect(&state, (SkinAlarmField)(id - 0x10u), &rect);
    }
    if (!valid) return 0;
    *x = rect.x; *y = rect.y; *w = rect.w; *h = rect.h;
    return 1;
  }
  if (id < 0x10u)                                   /* ---- 主屏时间设置 ---- */
  {
    switch (id)
    {
      case SET_ITEM_HOUR:
        *x = PX_MAIN_H_X; *y = PX_MAIN_DIG_Y;
        *w = (s16)(PX_MAIN_H2_X + 29 - PX_MAIN_H_X); *h = 41; break;
      case SET_ITEM_MIN:
        *x = PX_MAIN_M_X; *y = PX_MAIN_DIG_Y;
        *w = (s16)(PX_MAIN_M2_X + 29 - PX_MAIN_M_X); *h = 41; break;
      case SET_ITEM_YEAR:
      case SET_ITEM_MONTH:
      case SET_ITEM_DAY:
      {
        /* 日期串固定 "YYYY-MM-DD"（8 位月日），按字宽切出各自子矩形，
           换字体/字号自动跟着变，不用改这里。 */
        s16 wy = (s16)UI_StrPxWidth("2026", UI_PX_12);
        s16 wd = (s16)UI_StrPxWidth("-", UI_PX_12);
        s16 wm = (s16)UI_StrPxWidth("07", UI_PX_12);
        *y = PX_MAIN_ROW_Y; *h = 12;
        if (id == SET_ITEM_YEAR)       { *x = PX_MAIN_DATE_X;                     *w = wy; }
        else if (id == SET_ITEM_MONTH) { *x = (s16)(PX_MAIN_DATE_X + wy + wd);    *w = wm; }
        else                           { *x = (s16)(PX_MAIN_DATE_X + wy + wd + wm + wd); *w = (s16)UI_StrPxWidth("20", UI_PX_12); }
        break;
      }
      default: return 0;
    }
    return 1;
  }
  if (id >= 0x20u)
  {
    const CountdownViewState state = countdown_view_state();
    CountdownRect rect;
    if (!CountdownView_ItemRect(&state, (CountdownItem)(id - 0x20u), &rect))
      return 0;
    *x = rect.x; *y = rect.y; *w = rect.w; *h = rect.h;
    return 1;
  }
  /* ---- 闹钟屏设置 ---- */
  {
    u8 it = (u8)(id - 0x10u);
    static const u16 tgx[3] = { PX_ALM_TG_X5, PX_ALM_TG_X6, PX_ALM_TG_X7 };
    static const u16 tgw[3] = { 43, 40, 42 };
    switch (it)
    {
      case ALM_SET_SEL:  *x = PX_ALM_TAG_X; *y = PX_ALM_TAG_Y; *w = 11; *h = 14; break;
      case ALM_SET_ON:   *x = 240; *y = 25; *w = 64; *h = 20; break;   /* 「闹钟开关」四字墨迹 x241..303 / y26..43，各留 1px；必须与 sw_lbl_off.png 同框 */
      case ALM_SET_HOUR: *x = PX_ALM_H_X;   *y = PX_ALM_DIG_Y; *w = (s16)(PX_ALM_H2_X + 30 - PX_ALM_H_X); *h = 46; break;
      case ALM_SET_MIN:  *x = PX_ALM_M_X;   *y = PX_ALM_DIG_Y; *w = (s16)(PX_ALM_M2_X + 30 - PX_ALM_M_X); *h = 46; break;
      case ALM_SET_DAYS:
      {
        /* 闪灭矩形按「屏上正在显示的那几个图标」取（与显示同源，别再用 g_alm_days 反推） */
        u8 d = alm_days_of_mask(alm_show_mask((u8)((g_alm_sel > 2u) ? 0u : g_alm_sel)));
        *x = (s16)tgx[d - 5u]; *y = PX_ALM_TG_Y; *w = (s16)tgw[d - 5u]; *h = 44;
        break;
      }
      default: return 0;
    }
    return 1;
  }
}

/* 重画当前项（只画这一项，不动别的） */
static void blink_repaint(void)
{
  u8 id = g_blink_id;
  if (id == BLINK_NONE) return;
  /* 只在它自己那一屏上画：这个函数按 g_blink_scr 选分支、却都画到"当前屏"，
     少了这句守卫就会把 A 屏的元素画到 B 屏上（2026-11-20「退出」残留）。 */
  if (g_blink_scr != UI_GetScreen()) return;

  if (Skin_Active() && g_blink_scr != UI_SCREEN_CNT)
  {
    if (g_blink_scr == UI_SCREEN_MAIN)
    {
      /* 只作废被闪的那一项：亮相只重画它自己。整层作废会把 8 个字段再擦一遍（闪）。 */
      if (id < 0x10u) Skin_InvalidateField((SkinClockField)id);
      draw_skin_main(&g_set_ws, -1);
    }
    else { Px_DrawAlmInvalidate(); Px_DrawAlm(); }
    return;
  }

  if (g_blink_scr == UI_SCREEN_MAIN)
  {
    u8 hh = g_set_ws.hour, mm = g_set_ws.minute;
    switch (id)
    {
      case SET_ITEM_HOUR:
        px_digit(PX_MAIN_H_X,  PX_MAIN_DIG_Y, (u8)(hh / 10u), 0);
        px_digit(PX_MAIN_H2_X, PX_MAIN_DIG_Y, (u8)(hh % 10u), 0);
        break;
      case SET_ITEM_MIN:
        px_digit(PX_MAIN_M_X,  PX_MAIN_DIG_Y, (u8)(mm / 10u), 0);
        px_digit(PX_MAIN_M2_X, PX_MAIN_DIG_Y, (u8)(mm % 10u), 0);
        break;
      case SET_ITEM_YEAR: case SET_ITEM_MONTH: case SET_ITEM_DAY:
      {
        char b[11];
        u16 yr = g_set_ws.year;
        b[0] = '2'; b[1] = '0';
        b[2] = (char)('0' + (u8)((yr / 10u) % 10u));
        b[3] = (char)('0' + (u8)(yr % 10u));
        b[4] = '-';
        b[5] = (char)('0' + (u8)(g_set_ws.month / 10u));
        b[6] = (char)('0' + (u8)(g_set_ws.month % 10u));
        b[7] = '-';
        b[8] = (char)('0' + (u8)(g_set_ws.day / 10u));
        b[9] = (char)('0' + (u8)(g_set_ws.day % 10u));
        b[10] = 0;
        UI_DrawStrPx(PX_MAIN_DATE_X, PX_MAIN_ROW_Y, b, UI_PX_12, PX_COL_INK);
        break;
      }
      default: break;
    }
    return;
  }

  if (g_blink_scr == UI_SCREEN_CNT)
  {
    const CountdownViewState state = countdown_view_state();
    CountdownView_DrawItem(&state, (CountdownItem)(id - 0x20u));
    return;
  }

  if (g_blink_scr == UI_SCREEN_ALARM)
  {
    /* 闹钟屏的增量缓存会认为"星期二"而不再重画，亮相必须先把缓存全作废再整屏重画，
       否则灭相擦掉的内容永远回不来（实测开关擦掉后不恢复）。 */
    Px_DrawAlmInvalidate();
    Px_DrawAlm();
    (void)0;
    u8 idx = (g_alm_sel > 2u) ? 0u : g_alm_sel;
    u8 hh = g_alm_hour[idx], mm = g_alm_min[idx];
    switch (id - 0x10u)
    {
      case ALM_SET_SEL:  UI_DrawSprite(UI_SPR_ALM_TAG((u32)idx + 1u), PX_ALM_TAG_X, PX_ALM_TAG_Y); break;
      case ALM_SET_ON:   alm_swlbl_paint(); break;
      case ALM_SET_HOUR:
        px_digit(PX_ALM_H_X,  PX_ALM_DIG_Y, (u8)(hh / 10u), 1);
        px_digit(PX_ALM_H2_X, PX_ALM_DIG_Y, (u8)(hh % 10u), 1);
        break;
      case ALM_SET_MIN:
        px_digit(PX_ALM_M_X,  PX_ALM_DIG_Y, (u8)(mm / 10u), 1);
        px_digit(PX_ALM_M2_X, PX_ALM_DIG_Y, (u8)(mm % 10u), 1);
        break;
      case ALM_SET_DAYS:
        /* 整屏重画已经把「该彩/该灰」画对了（Px_DrawAlm 只贴开态精灵），这里不用补 */
        break;
      default: break;
    }
    return;
  }
}

/* 按相位执行：灭=恢复背景，亮=重画该项 */
/* 灭相时要被隐藏的项 id（0xFF=没有）。绘制函数里查它来决定"星期三"，
   否则整屏每秒重绘会把灭相又画回来（倒计时跑起来时每秒都会重绘）。 */
u8 Screen_BlinkHiddenId(void)
{
  return g_blink_on ? 0xFFu : g_blink_id;
}

static void blink_apply(void)
{
  /* 离开该屏就不要画了（否则会在别的屏上留下痕迹，用户实测"星期四"） */
  if (g_blink_scr != UI_GetScreen()) return;

  /* 星期循环项走通用路径即可：亮相整屏重画（该图标是彩色的）、灭相恢复背景
     （背景里就是灰图）——五/六/日 三态都适用，不必再单独分支。 */

  if (Skin_Active() && g_blink_id == (u8)(0x10u + ALM_SET_ON))
  {
    if (g_blink_on)
      Skin_Clear((SkinRect){g_blink_x, g_blink_y, g_blink_w, g_blink_h});
    else if (Skin_AlarmSwitchCover()) return;
    if (g_blink_on) { blink_repaint(); return; }
  }

  if (!Skin_Active() && g_blink_id == (u8)(0x10u + ALM_SET_ON))
  {
    /* 「闹钟开关」四字在背景里是 baked 的：
         亮相 = 从背景恢复（与设计稿逐像素一致，不用字库重排）；
         灭相 = 贴一张「墨迹形状」的纸色贴片（sw_lbl_off），只把字盖成纸——
                以前整块平涂卡片色，纸上会留下一个偏黄的方块（2026-11-20 修）。 */
    if (g_blink_on)
      UI_DrawGlyphRect(g_blink_x, 239 - (g_blink_y + g_blink_h) + 1,
                       (u16)g_blink_w, (u16)g_blink_h, 0xFFFFu, 0);
    else
      UI_DrawSprite(UI_SPR_ALM_SWLBL, g_blink_x, g_blink_y);
    return;
  }

  if (g_blink_on) { blink_repaint(); return; }
  UI_DrawGlyphRect(g_blink_x, 239 - (g_blink_y + g_blink_h) + 1,
                   (u16)g_blink_w, (u16)g_blink_h, 0xFFFFu, 0);
}

/* 把「灭相」在 g_blink_* 那块矩形上留下的痕迹还原成背景，并把该屏动态层整层重画成真实状态。
   闹钟屏的背景常驻 GRAM：只有整层重画才能把背景里 baked 的「闹钟开关」四字画回来
   （2026-11-20 用户实测：灭相时切设置项，四个字一直不显示）。 */
static void blink_restore(u8 scr)
{
  if (g_blink_x >= 0)
    UI_DrawGlyphRect(g_blink_x, 239 - (g_blink_y + g_blink_h) + 1,
                     (u16)g_blink_w, (u16)g_blink_h, 0xFFFFu, 0);
  if (scr == UI_SCREEN_ALARM)     { Px_DrawAlmInvalidate(); Px_DrawAlm(); }
  else if (scr == UI_SCREEN_MAIN)
  {
    /* 2026-11-25 用户「切换设置项还是会有抖动」：原来这里 Skin_Invalidate() 把**整层**
       字段缓存全作废，下一次 Px_DrawMain 就会把 日期/星期/农历/温湿度/AI标题 逐个
       Skin_Clear 再重画 —— 切一次项整屏抖一下。其实只需要把「刚被上面那行擦掉的那一项」
       补回来：按段/按位作废对应字段即可，其它字段缓存照旧命中、一个像素都不用动。 */
    if (g_blink_id < 0x10u) Skin_InvalidateField((SkinClockField)g_blink_id);
    Calendar_SetStaticNext();      /* 让下一次 Calendar_Refresh 真的跑一遍 Px_DrawMain */
  }
  else if (scr == UI_SCREEN_CNT)  { cnt_px_invalidate(); draw_countdown_view(); }
}

/* 调值后的「按键反馈」：立刻回亮相 + 半秒计时重来（2026-11-25 用户「调整设置项数值的时候
   按键没有反馈，即显示没有及时更新为设置值并保持半秒不闪烁」）。
   闪灭只按「项 id / 矩形」判断要不要重画，调值时矩形不变 -> 不重画；要是此刻正停在灭相，
   新值要等下一次亮相（最多 500ms）才出现，用户按了键像没反应。 */
static void Screen_BlinkNudge(void)
{
  if (g_blink_id == BLINK_NONE) return;           /* 没有在闪（常态/倒计时休眠）：不管 */
  if (g_blink_scr != UI_GetScreen()) return;      /* 不在它那一屏：不画（防跨屏残留） */
  g_blink_on = 1;                                 /* 强制回亮相 */
  g_blink_ms = (u32)bftm0_ct;                     /* 半秒计时重来：新值至少看满半秒 */
  blink_repaint();                                /* 立刻把新值画上去 */
}

/* 每帧调用（Screen_Draw 开头）：换项立刻回亮相；每 500ms 翻转一次 */
void Screen_BlinkTick(void)
{
  u8 id;
#if RING_TEST
  {   /* 临时自测钩子：伪造「响铃中 + 贪睡待响」（真实现只有跨分钟才出现） */
    static u8 once = 0;
    if (!once && (u32)bftm0_ct > 2500u)
    {
      once = 1;
      g_alm_ring_idx   = 0;
      g_alm.ringing    = 1;
      g_alm.ring_start = (u32)bftm0_ct;
      g_alm.snooze     = 1;
      g_alm_snooze_ms  = (u32)bftm0_ct + 300000u;
      printf("[TEST] 伪造：响铃中 + 贪睡待响（槽1）\r\n");
    }
  }
#endif
#if BLINK_TEST
  {   /* 临时验证钩子（仿真按键不触发，强制摆设置态；验证完 BLINK_TEST 归 0） */
    static u8 once = 0;
    if (!once) {
      once = 1;
#if BLINK_TEST == 1
      TimeModule_GetTime(&g_set_ws); g_set_active = 1; g_set_item = SET_ITEM_HOUR;
#elif BLINK_TEST == 2 || BLINK_TEST == 6
      UI_SetScreen(UI_SCREEN_ALARM); g_alm_set_active = 1;
#if BLINK_TEST == 6
      g_alm_set_item = ALM_SET_DAYS; g_alm_days[0] = 5u; g_alm_wd[0] = alm_days_mask(5u); g_alm_loop[0] = 2u;
#else
      g_alm_set_item = ALM_SET_ON;
#endif
#else
      UI_SetScreen(UI_SCREEN_CNT); g_cnt_sel = CNT_SEL_MIN;   /* 3/4 = 分 / 暂停按钮 */
#if BLINK_TEST == 4
      g_cnt_sel = CNT_SEL_PAUSE;
#endif
#if BLINK_TEST == 5
      g_cnt_sel_awake = 0;                 /* 5=常态（不闪）对照 */
#else
      g_cnt_sel_awake = 1;
#endif
      g_cnt_sel_awake_ms = (u32)bftm0_ct;
#endif
    }
  }
#endif
  /* 倒计时屏：5s 无操作 -> 回到「不闪」；若正停在灭相，先把该项画回来 */
  if (UI_GetScreen() == UI_SCREEN_CNT && g_cnt_sel_awake &&
      (u32)(bftm0_ct - g_cnt_sel_awake_ms) >= CNT_SEL_IDLE_MS)
  {
    g_cnt_sel_awake = 0;
    if (!g_blink_on && g_blink_id != BLINK_NONE) { g_blink_on = 1; blink_apply(); }
    printf("[KEY] 倒计时选中态 5s 无操作 -> 停闪\r\n");
  }
  id = blink_cur_id();
  u8 scr = UI_GetScreen();

  if (id == BLINK_NONE)
  {
    /* 退出闪灭（退出设置/5s 休眠）：把最后闪过的项恢复成真实状态——
       开关项可能是平涂盖着的，星期项可能是灰图，都要重画整屏动态层。 */
    if (g_blink_id != BLINK_NONE)
    {
      u8 was_scr = g_blink_scr;
      /* 先把最后一项恢复成真实状态（可能正停在"星期五"相），再清状态 */
      g_blink_on = 1;
      if (was_scr == UI_GetScreen()) blink_restore(was_scr);
      /* 换屏了：整屏背景（+ 该屏动态层）已经重传/重画过，没有"灭相"痕迹需要还原，
         这里什么都不该画。2026-11-20 修：原来对倒计时屏补调一次 blink_repaint()，
         而它对当前屏没有守卫 -> 从倒计时屏按「退出」回主屏时，把按钮字「退出」
         画到了主屏上（用户实测残留）。 */
      g_blink_id = BLINK_NONE; g_blink_scr = 0xFF;
    }
    g_blink_on = 1; g_blink_scr = 0xFF;
    return;
  }
  /* 每次都比对矩形：星期循环这类「同一个项、值变了」的情况，矩形会随值和图标变，
     只在 id 变化时重算的话会一直闪旧的那个图标（实测：调了值但闪的永远是「日」）。 */
  {
    s16 nx = 0, ny = 0, nw = 0, nh = 0;
    int have  = blink_rect(id, &nx, &ny, &nw, &nh);
    int moved = have && (nx != g_blink_x || ny != g_blink_y || nw != g_blink_w || nh != g_blink_h);
    if (id != g_blink_id || scr != g_blink_scr || g_blink_x < 0 || moved)
    {
      if (!have) { g_blink_id = BLINK_NONE; return; }
      /* 换项/换矩形：先把上一项「灭相」的痕迹还原、整层重画成真实状态，再从亮相开始。
         （同屏才需要还原；换屏时背景已整屏重传） */
      g_blink_on = 1;
      if (scr == g_blink_scr && g_blink_x >= 0) blink_restore(scr);
      g_blink_id = id; g_blink_scr = scr; g_blink_ms = (u32)bftm0_ct;
      g_blink_x = nx; g_blink_y = ny; g_blink_w = nw; g_blink_h = nh;
      return;
    }
  }
  if ((u32)(bftm0_ct - g_blink_ms) < BLINK_HALF_MS) return;
  g_blink_ms = (u32)bftm0_ct;
  g_blink_on = g_blink_on ? 0u : 1u;
  blink_apply();
}

/* ---------------- 闹钟屏 ----------------
 * 国风版（2026-11-19）：设计稿只把「五/六/日」三个图标做成可切换（关=变灰），
 * 一~四 恒亮；开关在右上 247,48（57x25）；「闹钟N」的 N 是 14px 数字。
 * 星期说明：一~四 恒定参与；五/六/日 由 g_alm_wd[idx] 的 bit0/1/2 决定。
 *   循环模式 0 单次（响一次自动关）/ 1 每天（三个全开）/ 2 自定义（按掩码） */
void Px_DrawAlm(void)
{
  if (Skin_Active())
  {
    const SkinAlarmState state = alarm_view_state();
    u8 hidden = Screen_BlinkHiddenId();
    Skin_Alarm(&state, hidden >= 0x10u && hidden < 0x20u ? (int)hidden - 0x10 : -1);
    draw_skin_robot(UI_SCREEN_ALARM);
    bubble_draw();
    return;
  }
  /* 显示槽：响铃中 = 响铃槽（与旧 draw_alarm 一致，2026-11-17 接线时对齐），否则当前选中槽 */
  u8 idx = g_alm.ringing ? g_alm_ring_idx : g_alm_sel;
  u8 i, hh, mm;
  static const u16 tg_x[3] = { PX_ALM_TG_X5, PX_ALM_TG_X6, PX_ALM_TG_X7 };
  u8 wd_mask;

  if (idx > 2u) idx = 0u;
  hh = g_alm_hour[idx];
  mm = g_alm_min[idx];

  /* HH:MM（style=1，单元格 30x46）：设计稿实测 27/60/冒号 92/106/136，墨迹顶 y26
     （这版纯净版背景原本还留着 08:35，已在素材阶段用清单 blank2 抹掉，固件不必平涂） */
  px_digit(PX_ALM_H_X,  PX_ALM_DIG_Y, (u8)(hh / 10u), 1);
  px_digit(PX_ALM_H2_X, PX_ALM_DIG_Y, (u8)(hh % 10u), 1);
  UI_DrawSprite(UI_SPR_COLON(1), PX_ALM_COL_X, PX_ALM_DIG_Y);
  px_digit(PX_ALM_M_X,  PX_ALM_DIG_Y, (u8)(mm / 10u), 1);
  px_digit(PX_ALM_M2_X, PX_ALM_DIG_Y, (u8)(mm % 10u), 1);

  /* 槽号：紧接背景已 baked 的「闹钟」二字（设计稿「闹钟1」的 1 在 x212..217 / y57..70） */
  UI_DrawSprite(UI_SPR_ALM_TAG((u32)idx + 1u), PX_ALM_TAG_X, PX_ALM_TAG_Y);

  /* 「闹钟开关」四个字（背景已抹掉；闪灭灭相时跳过） */
  if (Screen_BlinkHiddenId() != (u8)(0x10u + ALM_SET_ON)) alm_swlbl_paint();

  /* 开关（开=蓝 / 关=灰，两张精灵都从设计稿来） */
  UI_DrawSprite(UI_SPR_ALM_SW(g_alm_on[idx] ? 1 : 0), PX_ALM_SW_X, PX_ALM_SW_Y);

  /* 五/六/日 三个图标：**每帧两态都显式贴一张**，不能只在「参与」时贴——
     闹钟屏是「背景常驻 GRAM + 只重画动态层」，不贴就会留着上一次的彩色像素
     （实测：7 天→5 天后「六/日」仍是彩色）。两张精灵都由素材生成：
       开 = 「（打开状态）」的彩色插图（带墨迹 alpha，纸面不贴，所以没有方块补丁）
       关 = 「（抠图）」同框原样（= 背景本身），贴了等于恢复背景。 */
  wd_mask = alm_show_mask(idx);   /* 与响铃判定同源（2026-11-24：原来用 g_alm_days 反推，语音设过就不同步） */
  for (i = 0; i < 3u; i++)
    UI_DrawSprite(UI_SPR_ALM_TG(i, ((wd_mask >> i) & 1u) ? 1u : 0u), tg_x[i], PX_ALM_TG_Y);

  /* 机器人（落点由素材 GIF 自带） */
  UI_DrawSpriteAuto(UI_SPR_ROBOT(2, (g_cani.cur_state == UI_CANI_STATE_WAKE) ? 1u : 0u, g_cani.cur_frame % 16u));

  /* 气泡文字（闹钟屏：x140..236 / y160 起，20px，窄区自动折行，最多 2 行） */
  bubble_draw();
}

/* 作废闹钟屏的全部增量缓存：闪灭的「亮相」要整屏重画时调（2026-11-19）
   2026-11-23：同时作废 ui_skin 的内容缓存——否则 Skin_Alarm 认为「内容没变」
   不重画，灭相擦掉的槽号/开关永远回不来。 */
void Px_DrawAlmInvalidate(void)
{
  Skin_Invalidate();
  c_px_alm_idx = 0xFF; c_px_alm_hh = 0xFF; c_px_alm_mm = 0xFF;
  c_px_alm_on  = 0xFF; c_px_alm_loop = 0xFF; c_px_alm_wd = 0xFF;
  c_px_alm_bg  = 0xFFFF; c_px_alm_bub = 0xFFFF;
}

/* ---------------- 机器人单独重绘（4 fps） ----------------
 * 三屏的整屏重画分别绑在「秒变化 / 状态变化」上（1 fps），机器人帧只有在整屏重画时
 * 才会被贴一次，所以动画看起来是 1 fps。UI_CharAnim_Tick() 本身 250 ms 推进一帧
 * （2026-11-23 用户要求频率减半，原 125 ms），
 * 这里跟着它只把机器人那一个精灵重贴一次（一个 45~52 px 宽的块，开销很小）。
 * 帧号/状态/屏幕都没变时直接返回，避免无谓的 Flash 读 + SPI 写。 */
void Px_RobotTick(void)
{
  static u16 c_f = 0xFFFFu;
  static u8  c_s = 0xFFu, c_st = 0xFFu;
  u8  s  = (u8)UI_GetScreen();
  u16 f  = (u16)(g_cani.cur_frame % 16u);
  u8  st = Skin_Active() ? g_cani.cur_state : (g_cani.cur_state == UI_CANI_STATE_WAKE ? 1u : 0u);

  if (!g_cani.ok) return;                 /* CANI/精灵容器没就绪：不画 */
  if (f == c_f && s == c_s && st == c_st) return;
  c_f = f; c_s = s; c_st = st;

  if (Skin_Active()) { draw_skin_robot(s); return; }
  switch (s)
  {
    case UI_SCREEN_MAIN:  UI_DrawSpriteAuto(UI_SPR_ROBOT(0, st, f)); break;
    case UI_SCREEN_CNT:   UI_DrawSpriteAuto(UI_SPR_ROBOT(1, st, f)); break;
    case UI_SCREEN_ALARM: UI_DrawSpriteAuto(UI_SPR_ROBOT(2, st, f)); break;
    default: break;
  }
}










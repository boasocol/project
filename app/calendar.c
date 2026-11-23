/************************************************************************************************************
 * @file    calendar.c
 * @brief   日历界面：翻页动画状态机 + 增量刷新（横屏 320x240）
 *
 * @note    本模块不程序化绘制背景，而是在 Flash 中的 UI 背景图（ui_320x240.bin，
 *          由 gen_ui.py 生成）上用 Flash 字库（ui.c）按坐标叠加动态文字。
 *
 *          界面各区块逻辑坐标（x, y 起点，y 为设计坐标顶原点，UI_Show 内部换算 GRAM）——概念B 卡片仪表盘：
 *            - 时钟：主卡内 HH:MM 大数字 40x84 白，x=[64,110,170,216]，y=32；
 *              冒号 x=153；秒 16x22 青（x=270,y=89）
 *            - 日期三卡：日期串(中心 x=82)/星期(164)/农历(232)，y=130
 *            - 蓝牙勾/叉：底部条右上 中心(290,170)
 *            - 温湿度：两张迷你卡内 12x30 白字（温度[128..196]、湿度[232..300]，y≈197）
 *
 *          核心机制：
 *            1. Calendar_Refresh()：每秒由 main 调用，逐区块比较“上次绘制值 c_*”
 *               vs 当前时间，只重绘变化区块（增量刷新）；时钟为纯静态直绘。
 *            2. 翻页动画（ANI2）数据保留但已停用（g_anim_ok=0），概念B 布局不再使用；
 *               旧翻页队列代码保留为死代码。
 *
 *          设备显示约定：设备对 GRAM 做垂直镜像，背景与字模均已 FV 预补偿，
 *          动画帧表也已按设备镜像构建（见 build_flash.py build_anim）。
 ************************************************************************************************************/
#include "calendar.h"
#include "ui.h"
#include "time_module.h"
#include "BFTM0.h"   /* bftm0_ct（1ms 计数，调试测帧耗时） */
#include "screen.h"  /* Px_DrawMain：新 UI 像素风主屏绘制（2026-11-17 接线） */

/* ------------------------------------------------------------------
 * YF(y, h)：设计坐标 -> GRAM 列换算宏（仅低层原语 UI_DrawGlyphRect 用）。
 * 设备对 GRAM 做垂直镜像：GRAM 列 = 239 - 设计行。一个高 h 的块（设计行
 * y..y+h-1）的"起始 GRAM 列" = 239 - (y) - (h) + 1（对应 GRAM 列 239-y-h+1..239-y）。
 * 2026-08 改为带 +1，与 MSCOT/CANI 的 239-(top+h)+1 一致——文字/人物/背景像素级对齐
 * （原 239-y-h 少 1，文字比背景低 1px）。
 * 注意: UI_Show 已内部按"逐行 239-设计行"换算，调用方直接传设计坐标（y=文字顶部），
 *       勿再对 UI_Show 的 y 用 YF；此处只给 UI_DrawGlyphRect（冒号/蓝牙图标）这类原语用。
 * 参数: y=区块顶部的设计 y，h=区块高度。
 * 返回: 区块起始 GRAM 列。
 * ------------------------------------------------------------------ */
#define YF(y, h) (239 - (y) - (h) + 1)

/* ------------------------------------------------------------------
 * CalcWeekday：由年月日算星期（蔡勒公式 Zeller）。
 * 参数: y=年（如 2026），m=月（1..12），d=日。
 * 返回: 0=周一 .. 6=周日。
 * ------------------------------------------------------------------ */
int CalcWeekday(int y, int m, int d)
{
  if (m < 3) { m += 12; y--; }                       /* 1、2 月按上一年的 13、14 月算 */
  int K = y % 100, J = y / 100;
  int h = (d + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
  return (h + 5) % 7;                                /* 蔡勒默认 0=周日，换算成 0=周一 */
}

/* 中文星期表：2026-08 架构重构改字符串字面量直接喂 UI_Show（UI_SLOT_WEEK），
   原 Unicode 码点数组 WD_CN 已删除（2026-10-12 清 -Wunused-variable 警告）。 */

/* 公历某月天数（万年历，坑#77 #4 2026-09-05）：大/小月 + 闰年 2 月 29 天。
   month 非法（<1 或 >12）返回 0（调用方先验）。日期设置校验（语音 0x82 / 按键
   设置模式日项）都走这里，杜绝 2/31 等非法日期被设进 RTC。 */
u8 Calendar_DaysInMonth(u8 month, u16 year)
{
  static const u8 MD[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (month < 1 || month > 12) return 0;
  if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
    return 29;
  return MD[month - 1];
}

/* ------------------------------------------------------------------
 * 增量刷新缓存：记录“上次已绘制到屏幕”的各值。
 * Calendar_Refresh 每次把当前值与这里比较，不同才重绘该区块；
 * 初值全部设成非法值（0xFF/0xFFFF/0x7FFF），保证首帧强制全重绘。
 *   c_hh/c_mm/c_ss ：时/分/秒（与当前值比较判断哪些位翻页）
 *   c_yy/c_mo/c_dd ：年/月/日
 *   c_wd           ：星期
 *   c_temp/c_humi  ：温度/湿度（值单位 0.1）
 * ------------------------------------------------------------------ */
static u8  c_hh = 0xFF, c_mm = 0xFF, c_ss = 0xFF;
static u16 c_yy = 0xFFFF;
static u8  c_mo = 0xFF, c_dd = 0xFF, c_wd = 0xFF;
static int c_temp = 0x7FFF, c_humi = 0x7FFF;

/* 最近一次采样值（Calendar_Refresh 入口写入，Calendar_GetTempHumi 取用；单位 0.1）。
   初值 0x7FFF = 从未采样（与 c_temp 同"非法值"约定），GetTempHumi 内部按 0 处理防乱码。 */
static int s_last_temp = 0x7FFF, s_last_humi = 0x7FFF;

/* ------------------------------------------------------------------
 * cal_strcpy：带长度的字符串拷贝（C89，不引 libc 的 strncpy 语义坑）。
 * 参数: d=目标, dn=目标字节数（含 NUL）, s=源（非空）
 * 说明: 超长截断并补 NUL；dn<=0 或 d 空时不动。
 * ------------------------------------------------------------------ */
static void cal_strcpy(char *d, int dn, const char *s)
{
  int i = 0;
  if (d == 0 || dn <= 0) return;
  while (s[i] != 0 && i < dn - 1) { d[i] = s[i]; i++; }
  d[i] = 0;
}

/* ------------------------------------------------------------------
 * Calendar_GetLunar：公历 -> 农历「月+日」串（不含「农历：」前缀）。
 *
 * 逻辑与旧 Calendar_RefreshStatic 内联代码完全一致（只抽出来复用，文案格式不变）：
 *   月份：中文月名表（正/二/…/十/冬/腊）+「月」；
 *   日：初一..初十 -> 初+一..十；十一..十九 -> 十+一..九；二十 -> 二十；
 *       廿一..廿九 -> 廿+一..九；三十 -> 三十。
 *   坑#71 #5：初十边界必须是 <11（写成 <10 会取 N01[-1] 越界，农历串损坏消失）。
 *
 * 参数: t=公历时间；buf=输出缓冲；n=buf 字节数（含 NUL，建议 >= 16）
 * 返回: 0=成功；-1=日期/农历表越界；-2=参数非法
 * 注意: 最长输出 12 字节（「九月十四」），n < 13 时会被安全截断（可能截断在汉字中间，
 *       故调用方请给 >= 16 字节）。
 * ------------------------------------------------------------------ */
int Calendar_GetLunar(const TimeStruct *t, char *buf, int n)
{
  static const char *LUNAR_MONTH[12] = { /* 正 二 三 四 五 六 七 八 九 十 冬 腊 */
    "正","二","三","四","五","六","七","八","九","十","冬","腊"};
  static const char *N01[10] = { /* 一 二 三 四 五 六 七 八 九 十 */
    "一","二","三","四","五","六","七","八","九","十"};
  LunarDate ld;
  const char *q;
  int ln = 0;

  if (buf == 0 || n <= 0) return -2;
  buf[0] = 0;
  if (t == 0) return -2;

  if (TimeModule_GetLunar(t->year, t->month, t->day, &ld) != 0) return -1;
  if (ld.lunarMonth < 1 || ld.lunarMonth > 12) return -1;
  if (ld.lunarDay < 1 || ld.lunarDay > 30) return -1;

  /* 月：中文月名 + 「月」 */
  q = LUNAR_MONTH[ld.lunarMonth - 1];
  while (*q != 0 && ln < n - 1) buf[ln++] = *q++;
  q = "月";
  while (*q != 0 && ln < n - 1) buf[ln++] = *q++;

  /* 日：初一/十一/二十/廿X/三十 规则（与旧代码逐字一致） */
  if (ld.lunarDay == 20)
  {
    q = "二十";
  }
  else if (ld.lunarDay == 30)
  {
    q = "三十";
  }
  else if (ld.lunarDay < 11)
  {
    q = "初";
    while (*q != 0 && ln < n - 1) buf[ln++] = *q++;
    q = N01[ld.lunarDay - 1];
  }
  else if (ld.lunarDay < 20)
  {
    q = "十";
    while (*q != 0 && ln < n - 1) buf[ln++] = *q++;
    q = N01[ld.lunarDay - 11];
  }
  else
  {
    q = "廿";
    while (*q != 0 && ln < n - 1) buf[ln++] = *q++;
    q = N01[ld.lunarDay - 21];
  }
  while (*q != 0 && ln < n - 1) buf[ln++] = *q++;
  buf[ln] = 0;
  return 0;
}

/* ------------------------------------------------------------------
 * Calendar_GetTempHumi：温度/湿度数值串（格式同旧界面，单位 0.1）。
 *
 * 格式逻辑与旧 Calendar_RefreshStatic 内联代码一致（一位小数；负数带 '-'；
 * 百位仅在 >=100 时出现），只是改成写进调用方缓冲。
 * 数据源 = 最近一次 Calendar_Refresh 传入的 temp/humi（s_last_*），
 * 故调用顺序必须保证 Calendar_Refresh 已跑过（主循环每轮都跑，满足）。
 *
 * 参数: tb/tn=温度缓冲+字节数（>= 8）；hb/hn=湿度缓冲+字节数（>= 8）
 * 返回: 0=成功；-1=两个缓冲都非法
 * ------------------------------------------------------------------ */
int Calendar_GetTempHumi(char *tb, int tn, char *hb, int hn)
{
  char s[12];              /* 单串中间缓冲：最长 "-40.0" / "125.0" 6 字节 */
  int ti, tf, hi, hf, m;
  int temp, humi;

  if ((tb == 0 || tn <= 0) && (hb == 0 || hn <= 0)) return -1;

  temp = s_last_temp;
  humi = s_last_humi;
  /* 采样异常（未读到 / 超量程）：按 0 处理，防把 0x7FFF 格式化成乱码 */
  if (temp < -400 || temp > 1250) temp = 0;
  if (humi < 0    || humi > 1000) humi = 0;

  if (tb != 0 && tn > 0)
  {
    ti = temp / 10;
    tf = temp % 10;
    if (tf < 0) tf = -tf;
    m = 0;
    if (ti < 0) { s[m++] = '-'; ti = -ti; }
    if (ti >= 100) s[m++] = (char)('0' + (ti / 100) % 10);
    if (ti >= 10 || m > 0) s[m++] = (char)('0' + (ti / 10) % 10);
    s[m++] = (char)('0' + ti % 10);
    s[m++] = '.';
    s[m++] = (char)('0' + tf);
    s[m] = 0;
    cal_strcpy(tb, tn, s);
  }

  if (hb != 0 && hn > 0)
  {
    hi = humi / 10;
    hf = humi % 10;
    if (hf < 0) hf = -hf;
    m = 0;
    if (hi >= 100) s[m++] = (char)('0' + (hi / 100) % 10);
    if (hi >= 10 || m > 0) s[m++] = (char)('0' + (hi / 10) % 10);
    s[m++] = (char)('0' + hi % 10);
    s[m++] = '.';
    s[m++] = (char)('0' + hf);
    s[m] = 0;
    cal_strcpy(hb, hn, s);
  }
  return 0;
}

/* ------------------------------------------------------------------
 * 概念B 布局常量（ui-design-system.md §3 时间主卡）：
 *   大数字 HH:MM 白色 40x84 画主卡内、块水平居中 x[64..255]；
 *   冒号 8 宽居中于 H2 与 M1 之间 x=97；秒 16x22 青色画顶部小图标行 (24,2)。
 *   坑#84 新主屏（AI 奶油风）：时间 30 高（0xE100 温湿度数字段 12x30 + 0xE304 冒号 8x30）
 *   画左上粉色时间胶囊 x30-172 y60-92 内居中：组宽 12*4+8=56，中心 x101
 *   -> H1=73 H2=85 COL=97 M1=109 M2=121；蓝牙勾/叉 14x30 顶部右侧 x150 y0。
 * ------------------------------------------------------------------ */
#define CLK_Y     18      /* 40 高时间数字顶部（对齐示例时间 y18-52，40 高 y18-58） */
#define CLK_X_H1  32      /* 2026-10-10 唐美人逐级放大：数字 20x40 + 冒号 12x40，组宽 92 居中于示例框 */
#define CLK_X_H2  52
#define CLK_X_COL 72
#define CLK_X_M1  84
#define CLK_X_M2  104
#define SEC_X     113     /* 秒（22 高）紧贴时间 MM 右侧（HH:MM SS，示例框 x24-131 内右缘） */
#define SEC_Y     22
#define BT_X      33      /* 蓝牙胶囊 x14-170 y64-88 内画"蓝牙已连接/蓝牙未连接"（5 汉字 90px 居中，2026-10-12 加「蓝牙」二字） */
#define BT_Y      67

/* 坑#84 新主屏配色：深棕文字（AI 奶油风描边色），温度红/湿度蓝沿用 */
#define C_TIME  UI_RGB(92, 58, 42)
#define C_SEC   UI_RGB(64, 196, 255)

/* ==================================================================================
 * 旧主屏绘制原语（奶油风概念B，UI_Show / UI_DrawGlyphRect 版）—— 2026-11-17 新 UI 接线后
 * 全部停用：#if 0 保留便于回退（回退旧 UI = 本块 + 下方各 #if 0 一起改回 #if 1）。
 * 新 UI 对应实现：screen.c 的 Px_DrawMain（精灵 UI_SPR_* + UI_DrawStrPx）。
 * ================================================================================== */
#if 0
/* 画单个时钟数字（深棕 40 高，0xE100 时间数字段 20x40，顶部 y=CLK_Y）
   2026-09-05 坑#84：原 40x84 大数字换 30 高（新图时间胶囊仅 32px 高，84 高放不下）；
   2026-10-10 唐美人笔画粗 12x30 显小 -> 16x34 -> 20x40（build_flash 同步）；
   UI_DrawGlyphRect 逐位还原背景+叠墨（1:1）。 */
static void draw_clock_digit(int x, int d)
{
  UI_DrawGlyphRect(x, YF(CLK_Y, 40), 20, 40, 0xE100u + (u32)d, C_TIME);
}

/* ---- 冒号：12x40（0xE304），画时间胶囊 H2 与 M1 之间（x=72）；
       on=0 用非法 code（找不到字形=仅还原背景）清背景（坑#84） ---- */
static void draw_colon(int x, int on)
{
  UI_DrawGlyphRect(x, YF(CLK_Y, 40), 12, 40, on ? 0xE304u : 0xFFFFu, C_TIME);
}

/* 日期 "YYYY.M.D"（月/日不补前导零）：深棕数字(0xE000+d，统一12px格) + 4px 点号(0xE202)，
   坑#84 新主屏：整串一次 UI_Show(UI_SLOT_DATE2：格12/清110/居中) 画右上卡。
   位置【像素级对齐带文字版示例日期 x187-276 y61-73】（2026-09-05 实测深棕文字扫描）：
   22 高对齐参考中心 y67 -> y56 起。（2026-10-10 用户试过小号 18 高后要求回退原大小） */
static void draw_date_string(u16 yy, u8 mo, u8 da)
{
  char ds[12]; int n = 0;
  ds[n++] = (char)('0' + (yy / 1000) % 10); ds[n++] = (char)('0' + (yy / 100) % 10);
  ds[n++] = (char)('0' + (yy / 10) % 10);  ds[n++] = (char)('0' + yy % 10);
  ds[n++] = '.';
  if (mo >= 10) ds[n++] = (char)('0' + mo / 10);
  ds[n++] = (char)('0' + mo % 10);
  ds[n++] = '.';
  if (da >= 10) ds[n++] = (char)('0' + da / 10);
  ds[n++] = (char)('0' + da % 10);
  ds[n] = 0;
  UI_Show(187, 56, ds, UI_SLOT_DATE2, C_TIME);
}
#endif /* 旧主屏绘制原语（新 UI 停用） */

/* ==================================================================
 * 翻页动画状态（6 张卡片对应 6 个时/分/秒位）
 * ==================================================================
 * 卡片 x 顺序（与 gen_ui.py clock_xs 一致）：
 *   H1=13, H2=59, M1=117, M2=163, S1=221, S2=267
 *   （slot 下标：0=时十位,1=时个位,2=分十位,3=分个位,4=秒十位,5=秒个位）
 *
 * ANI2 双位面模型（一次数字变化 = 两阶段，帧数由数据头决定）：
 *   阶段 A(state=1)：旧数字【上半】绕中缝向下倒，背景露出新数字上半
 *   阶段 B(state=2)：新数字【下半】从中缝向下展开，背景是旧数字下半
 *   翻完整卡 = 新数字。
 *   half_idx 语义（与 ui.c UI_Anim_DrawFrame 一致）：0=屏幕下半，1=屏幕上半。
 * ------------------------------------------------------------------ */
typedef struct
{
  u8 state;       /* 0=空闲, 1=阶段A(上半折叠), 2=阶段B(下半展开) */
  u8 new_digit;   /* 翻页目标数字（翻完后显示） */
  u8 old_digit;   /* 翻页起始数字（阶段A 折叠的卡片内容） */
  u8 frame;       /* 当前阶段内的帧计数（0..帧数-1） */
} FlipState;

static FlipState g_flip[6];                 /* 6 个槽各自的翻页状态 */
static const int g_card_x[6] = {13, 59, 117, 163, 221, 267};   /* 各卡片左边界 x */

/* 卡片上下半的 GRAM 行：整卡 GRAM 起始 = YF(14,84)=142（2026-08 YF 改 +1 后）。
   上半 = 142 起始，下半 = 142+42 = 184 起始。（翻页动画已停用，本块为死代码。） */
#define FLIP_CARD_Y0     (YF(14, 84))     /* 整卡 GRAM 起始行（=142） */
#define FLIP_HALF_H      42               /* 半幅高（整卡 84 / 2） */

/* ------------------------------------------------------------------
 * 双卡并行翻页队列变量：
 * 最多同时翻 2 张卡（如秒十位+秒个位同时进位），每张卡帧率同为 10ms/帧，
 * 速度与单卡翻页完全一致。同一帧内画两张卡 = 每帧 SPI 写屏量约 2 倍，
 * 故帧间隔放宽到 10ms（单卡绘制约 3~4ms，双卡约 7~8ms，10ms 门槛仍余量）。
 *   g_active_a/g_active_b：当前正在翻页的槽下标，-1 表示空闲
 *   g_pending[]：该槽是否有“待翻页”任务（flip_start 置位，FlipTick 消费）
 *   g_pend_old[]/g_pend_new[]：待翻任务的起始/目标数字
 *   g_disp[]   ：该槽当前“实际显示”的数字（翻完或静态绘制时更新，
 *                用作下一次翻页的起始数字）
 * ------------------------------------------------------------------ */
static int g_active_a = -1;
static int g_active_b = -1;
static u8  g_pending[6];
static u8  g_pend_old[6], g_pend_new[6];
static u8  g_disp[6];

/* g_anim_ok：动画区是否可用（Calendar_Init 时记录 UI_Anim_Ready() 结果）。
   0=不可用（Flash 未烧新 ANIM 区等），时钟变化位直接静态直绘。
   概念B 起翻页动画停用（g_anim_ok 恒 0），下方翻页队列/flip_start 已无调用方（保留 ANI2 数据）。 */
static u8 g_anim_ok = 0;
/* g_static_next：蓝牙授时等大幅跳变标志。置位后下一次 Refresh 强制静态刷新
   （不做逐位连翻动画），Refresh 读到即清零。 */
static u8 g_static_next = 0;

/* ------------------------------------------------------------------
 * Calendar_FlipTick：翻页动画帧推进（双卡并行状态机）。
 *
 * 调用: main 主循环每次循环调用，传 BFTM0 的 1ms 计数 now_ms。
 * 节奏: 内部以 10ms 为帧间隔门槛（now_ms - last_ms >= 10 才推一帧）。
 *       单卡绘制约 3~4ms（SPI 15MHz + FIFO 后），双卡并行约 7~8ms，
 *       10ms 门槛即“固定匀速”，且与单卡翻页帧率完全一致。
 *
 * 流程:
 *   1) 若还有空闲活动槽，按“秒个位→…→时十位”优先级从 g_pending 取待翻槽
 *      （最多同时 2 张卡），取 g_disp 为起始数字（无显示值时用 g_pend_old），
 *      目标相同则跳过；启动后 state=1, frame=0。
 *   2) 对每个活动槽按当前 state 决定画哪个半幅：
 *        state=1 阶段A：画屏幕上半(GRAM183-224)，折叠【旧数字】，背景=新数字
 *        state=2 阶段B：画屏幕下半(GRAM141-182)，展开【新数字】，背景=旧数字
 *      调用 UI_Anim_DrawFrame 绘制一帧（两张卡各一帧，同一 tick 内先后绘制）。
 *   3) 帧计数+1，翻满一阶段后：state1->2 切阶段B；state2->0 翻完，
 *      更新 g_disp=新数字并释放该活动槽。
 * ------------------------------------------------------------------ */

/* 从 g_pending 按优先级取一个待翻槽启动（返回取到的槽，无则 -1）。
   同一次调用会依次取两个（用于同时启动双卡）。 */
static int anim_start_next(void)
{
  static const int order[6] = {5, 4, 3, 2, 1, 0};  /* 秒个位,秒十位,分个位,分十位,时个位,时十位 */
  for (int k = 0; k < 6; k++)
  {
    int s = order[k];
    if (g_pending[s])
    {
      u8 new_d = g_pend_new[s];
      u8 old_d = (g_disp[s] <= 9) ? g_disp[s] : g_pend_old[s];   /* 起始取实际显示值 */
      if (old_d == new_d) { g_pending[s] = 0; continue; }   /* 已是目标：无需翻 */
      g_flip[s].old_digit = old_d;
      g_flip[s].new_digit = new_d;
      g_flip[s].state     = 1;       /* 进入阶段 A */
      g_flip[s].frame     = 0;
      g_pending[s]        = 0;       /* 任务被取走 */
      return s;
    }
  }
  return -1;
}

/* 对单个活动槽推一帧：返回 0=还在翻，1=该槽本次翻完。 */
static int anim_advance(int i)
{
  u8 st = g_flip[i].state;
  if (st == 0) return 1;

  /* 按阶段决定本次画哪个半幅、用哪个数字 */
  int hy, digit, bg_digit, half_idx;
  if (st == 1)
  {   /* 阶段 A：屏幕上半(half_idx=1, GRAM183-224)折叠旧数字，背景是新数字上半 */
      half_idx = 1;
      digit    = g_flip[i].old_digit;
      bg_digit = g_flip[i].new_digit;
      hy       = FLIP_CARD_Y0 + FLIP_HALF_H;   /* GRAM 起始 = 183 */
  }
  else
  {   /* 阶段 B：屏幕下半(half_idx=0, GRAM141-182)展开新数字，背景是旧数字下半 */
      half_idx = 0;
      digit    = g_flip[i].new_digit;
      bg_digit = g_flip[i].old_digit;
      hy       = FLIP_CARD_Y0;                 /* GRAM 起始 = 141 */
  }

#if 1
  {
    static u8 n = 0;
    vu32 t0 = bftm0_ct;
    UI_Anim_DrawFrame(g_card_x[i], hy, digit, half_idx, g_flip[i].frame,
                      bg_digit, UI_C_DIGIT);
    (void)n;                                  /* 帧计时打印已注释（语音开发期） */
    /* if ((n++ & 0x07) == 0)
      printf("[DRAW] frame dt=%u ms slot=%d\r\n", (unsigned)((u32)bftm0_ct - t0), i); */
    (void)t0;
  }
#else
  UI_Anim_DrawFrame(g_card_x[i], hy, digit, half_idx, g_flip[i].frame,
                    bg_digit, UI_C_DIGIT);
#endif

  /* 推进帧计数，阶段切换/翻完判定（帧数从动画数据头读取） */
  g_flip[i].frame++;
  if (g_flip[i].frame >= (u8)UI_Anim_GetFrames())
  {
    g_flip[i].frame = 0;
    if (st == 1)
      g_flip[i].state = 2;         /* 上半翻完，切阶段 B（仍为本槽） */
    else
    {   /* 下半翻完：整卡 = 新数字，更新已显示值并释放活动槽 */
        g_flip[i].state = 0;
        g_disp[i]       = g_flip[i].new_digit;
        return 1;
    }
  }
  return 0;
}

void Calendar_FlipTick(u32 now_ms)
{
  /* 每帧点只画【一张】卡（A、B 交替），保证每轮主循环只做一次绘制：
     - 单卡：每帧点画 A -> 帧率 = 单帧绘制耗时（~15ms）
     - 双卡：帧点交替画 A、B -> 每卡仍隔一次绘制才推进一帧，
       但主循环轮转不被"一次画两卡"拖慢（保持 ~15ms/轮），
       故每卡帧率 ≈ 单卡帧率，双卡总时长与单卡一致。
     若帧点画A+半帧点画B，每轮两次绘制(30ms) -> 每卡 30ms/帧，双卡慢2倍。
     帧节拍用 last_frame=now_ms 对齐（绘制时间自然限速，无需固定间隔）。 */
  static u32 last_frame = 0;   /* 帧点时间 */
  static u8  turn = 0;         /* 0=画A，1=画B */

  if (!g_anim_ok) return;              /* 动画区不可用：纯静态显示 */

  if (last_frame == 0) last_frame = now_ms;

  /* 距上次 ≥8ms：推进一帧（绘制本身 ~15ms 会自然限速） */
  if (now_ms - last_frame >= 8u)
  {
    last_frame = now_ms;               /* 对齐（绘制时间决定实际帧间隔） */

    /* A/B 槽各自独立判断：空闲即取任务（真正双卡并行启动） */
    if (g_active_a < 0)
      g_active_a = anim_start_next();
    if (g_active_b < 0)
      g_active_b = anim_start_next();

    /* 交替只画一张卡：优先画 A，A 空闲才画 B */
    if (turn == 0)
    {
      if (g_active_a >= 0 && anim_advance(g_active_a))
        g_active_a = -1;               /* 卡 A 翻完 */
      if (g_active_a < 0) turn = 1;    /* A 空 -> 下轮画 B */
    }
    else
    {
      if (g_active_b >= 0 && anim_advance(g_active_b))
        g_active_b = -1;               /* 卡 B 翻完 */
      if (g_active_b < 0) turn = 0;    /* B 空 -> 下轮画 A */
    }

    /* 调试：打印整段动画【开始】和【结束】 */
    {
      static u32 anim_start_ms = 0;
      static u8  anim_running = 0;
      u8 running = (g_active_a >= 0 || g_active_b >= 0) ? 1 : 0;

      if (running && !anim_running)
      {
        anim_start_ms = (u32)bftm0_ct;
        /* [ANIM] start/end 打印已注释（语音开发期） */
        /* printf("[ANIM] start @%u ms A=%d B=%d\r\n",
               anim_start_ms, g_active_a, g_active_b); */
      }
      else if (!running && anim_running)
      {
        u32 dur = (u32)bftm0_ct - anim_start_ms;
        (void)dur;
        /* printf("[ANIM] end @%u ms dur=%u ms\r\n", (u32)bftm0_ct, dur); */
      }
      anim_running = running;
    }
  }
}

/* ============ 蓝牙状态动态绘制 ============
   背景图药丸：(x=196, y=156, w=108, h=24)；图标已占左侧，右缘 x=217。
   状态文字"已连接/未连接"共 3 个 18x22 汉字，在图标右缘与药丸右缘(303)
   之间居中绘制。字体颜色沿用图标蓝。 */
/* 蓝牙状态用勾/叉图标字模（build_flash.py 手绘 0xE300/0xE301，14x30）：
   0xE300=对勾（已连接），0xE301=叉（未连接）。
   背景图：蓝牙符文在 x[170..196]（中心 x=183，y 175..201）；勾/叉图标在其
   右侧 x=198..211，竖向居中于温湿度整体区域 y[154..223]（中点 188）-> 顶部 y=173。
   （无药丸方框，蓝牙图标直接画在背景上） */

/* ------------------------------------------------------------------
 * draw_bt_state：绘制蓝牙连接状态图标（勾=已连接，叉=未连接）。
 * 参数: connected=1 画对勾，0 画叉。
 * 2026-11-17 新 UI：旧主屏的蓝牙胶囊位置（x33,y67）在新背景上没有对应位置，
 * 本绘制 #if 0 停用（状态缓存 s_bt_connected 仍在维护，将来按新设计补坐标再打开）。
 * ------------------------------------------------------------------ */
static void draw_bt_state(int connected)
{
#if 0
  /* 坑#84 新主屏：蓝牙胶囊 x14-170 y64-88 内画"蓝牙已连接/蓝牙未连接"（5 汉字 22 高 90px，
     UI_SLOT_WEEK 清90 居中；2026-10-12 用户要求加「蓝牙」二字） */
  const char *s = connected ? "蓝牙已连接" : "蓝牙未连接";
  UI_Show(BT_X, BT_Y, s, UI_SLOT_WEEK, C_TIME);
#else
  (void)connected;   /* 新 UI 暂不显示蓝牙状态（保留去重逻辑） */
#endif
}

static u8 s_bt_connected = 0xFF;            /* 上次绘制状态（0xFF=未画过，强制首画） */

/* ------------------------------------------------------------------
 * Calendar_SetBtState：设置蓝牙显示状态（内部去重）。
 * 参数: connected=1 已连接，0 未连接。
 * 说明: 状态没变化则不重绘（避免每秒反复写屏）。
 * ------------------------------------------------------------------ */
void Calendar_SetBtState(int connected)
{
  u8 c = connected ? 1 : 0;
  if (c == s_bt_connected) return;          /* 状态没变不重绘 */
  s_bt_connected = c;
  draw_bt_state(c);
}

/* ============ 初始化：上传背景 + 画初始静态元素 ============ */
/* ------------------------------------------------------------------
 * Calendar_Init：界面初始化（上电调用一次）。
 * 流程:
 *   1) 读取 Flash 分区头并校验；损坏则回退淡黄底（至少屏有显示）。
 *   2) 上传整屏 UI 背景图、缓存字库索引表。
 *   3) 初始化动画区（读 ANI2 数据头），记录 g_anim_ok。
 *   4) 清零翻页状态、把 c_* 缓存置为非法值强制首帧全重绘。
 *   5) 置 g_static_next=1：上电首次刷新直接静态显示（不做翻页动画）。
 *   6) 蓝牙状态默认“未连接”。
 * ------------------------------------------------------------------ */
void Calendar_Init(void)
{
  if (UI_LoadHeader() != 0)
  {
    Display_Fill_Color(0xFF94);   /* 分区头损坏：回退淡黄底，至少屏有显示 */
    return;
  }
  UI_SetScreen(UI_SCREEN_MAIN);   /* 切主屏：上传 Flash 屏0 背景 + 设背景基址/人物区（2026-08-27 多屏） */
  UI_CacheFont();                 /* 缓存字库索引表 */
  UI_LoadConfig();                /* 读 CONF 配置容器（亮度/时区/闹钟/蓝牙） */
  /* 翻页动画停用（语音开发期，省开销）：数字走静态直画、冒号恢复闪烁。
     恢复翻页：g_anim_ok = (UI_Anim_Init()==0 && UI_Anim_Ready()) ? 1 : 0 */
  (void)UI_Anim_Init();   /* 仍初始化动画区（bg slice 自检），但不启用翻页 */
  g_anim_ok = 0;

  /* 翻页状态清零（首次 Refresh 若动画可用则按双卡队列启动翻页） */
  for (int i = 0; i < 6; i++) { g_flip[i].state = 0; g_pending[i] = 0; g_disp[i] = 0; }
  g_active_a = -1;
  g_active_b = -1;

  /* 初始冒号：亮起（概念B 单冒号 x=153；冒号闪烁已恢复） */
#if 0
  draw_colon(CLK_X_COL, 1);   /* 旧 UI 冒号（新 UI 冒号是 Px_DrawMain 里的 UI_SPR_COLON 精灵） */
#endif

  /* 语音助手小女孩：按 MSCT 尺寸自动定位（72x72 区，尺寸无关，与动画同区对齐） */
#if 0
  UI_DrawMascotCentered();    /* 旧 UI 静态人物（新 UI 用 SPR1 机器人精灵） */
#endif

  /* 形象状态动画（CANI）：帧图=形象图\default_girl_1..48.png（101+ 编号为暂存不入脚本）。
     每状态 16 帧（8 循环 + 4 变化到2 + 4 变化到3），0.25s/帧（4 次/秒）。
     要恢复静态只显示第 1 帧：改回 UI_CharAnim_Enable(0)。 */
  (void)UI_CharAnim_Init();
  UI_CharAnim_Enable(1);
  /* 2026-11-17 新 UI：CANI 帧机继续跑（cur_state/cur_frame 被 Px_DrawMain 的机器人精灵取用），
     但**不再画旧人物**——旧人物区（主屏 220,128）会盖在新 UI 背景/气泡上。 */
  UI_CharAnim_DrawEnable(0);

  /* 强制首帧重绘所有动态块 */
  c_yy = 0xFFFF; c_mo = 0xFF; c_dd = 0xFF; c_wd = 0xFF;
  c_hh = c_mm = c_ss = 0xFF;
  c_temp = c_humi = 0x7FFF;

  /* 上电首帧刷新直接静态显示（不做翻页动画） */
  g_static_next = 1;

  /* 蓝牙状态默认"未连接"（等状态机首次判定后更新） */
  s_bt_connected = 0xFF;
  draw_bt_state(0);
}

/* ============ 增量刷新：仅重绘变化区块 ============ */
/* ------------------------------------------------------------------
 * Calendar_Refresh：每秒增量刷新界面（main 在 RTC 秒更新后调用）。
 *
 * 参数:
 *   t    : 当前时间（TimeStruct*，指向 RTC 读取的时间）。
 *   temp : 温度值，单位 0.1（如 255 表示 25.5）。
 *   humi : 湿度值，单位 0.1（如 600 表示 60.0）。
 * 返回: 无。
 *
 * 流程（每个区块与 c_* 缓存比较，变了才重绘）:
 *   1) 若 g_static_next 置位：本次所有时钟位强制静态直绘（并取消进行中动画）。
 *   2) 时钟 6 位：逐位比较，变化位走动画(flip_start)或静态直绘(draw_clock_digit)；
 *      静态直绘时同步更新 g_disp（保证后续翻页起始数字正确）。
 *   3) 日期徽章：年/月/日/星期/农历任一变化整串重绘。
 *   4) 温湿度：数值变化才重绘，并在各自格内按实际串宽居中。
 * ------------------------------------------------------------------ */
void Calendar_Refresh(TimeStruct *t, int temp, int humi)
{
  /* 蓝牙授时等大幅跳变：本次强制静态刷新（不做逐位连翻动画），并取消进行中的动画 */
  int force_static = g_static_next;
  g_static_next = 0;
  if (force_static && g_anim_ok)
  {
    g_active_a = -1;
    g_active_b = -1;
    for (int i = 0; i < 6; i++) { g_flip[i].state = 0; g_pending[i] = 0; }
  }

  /* 采样值进缓存：新 UI 的 Calendar_GetTempHumi 从这里取（无参数接口） */
  s_last_temp = temp;
  s_last_humi = humi;

#if 0
  /* ================= 旧 UI（奶油风概念B，UI_Show 版主屏）—— 2026-11-17 新 UI 接线后停用，
     以 #if 0 保留便于回退（回退 = 本块与下方新 UI 块互换启用状态）。 ================= */

  /* ---- 时钟 HH:MM 大数字 + 秒（概念B 主卡；翻页动画停用 g_anim_ok=0，纯静态直绘） ----
     4 个大数字白色 40x84 画主卡内（x[64,110,170,216]、y=CLK_Y=32），块水平居中；
     秒 16x22 青色画主卡右下（x=270,y=89）。变化位才重绘（增量）。 */
  /* 时钟位比较：先判缓存非法（ForceRedraw/上电置 0xFF），非法必重绘。
     ⚠️ 不能只比 (t->hour % 10) != (c_hh % 10)：0xFF % 10 == 5，个位为 5 时
     （15 点等）被误判"没变化"→ 回主屏后个位消失（2026-09-05 用户复现）。 */
  if (c_hh == 0xFF || (t->hour / 10) != (c_hh / 10)) draw_clock_digit(CLK_X_H1, t->hour / 10);
  if (c_hh == 0xFF || (t->hour % 10) != (c_hh % 10)) draw_clock_digit(CLK_X_H2, t->hour % 10);
  c_hh = t->hour;
  if (c_mm == 0xFF || (t->minute / 10) != (c_mm / 10)) draw_clock_digit(CLK_X_M1, t->minute / 10);
  if (c_mm == 0xFF || (t->minute % 10) != (c_mm % 10)) draw_clock_digit(CLK_X_M2, t->minute % 10);
  c_mm = t->minute;
  /* 坑#84 主屏不显示秒（用户嫌左上角秒数字突兀；AI 示例时间无秒）。c_ss 缓存保留。 */
  if (t->second != c_ss) c_ss = t->second;

  /* 静态区块（日期/温湿度）每轮无条件刷新：Calendar_RefreshStatic 内部 c_* 缓存
     去重，无变化仅几次比较，开销可忽略；force_static（授时/上电）时时钟位已静态
     直画，这里顺带把静态区块一次画全。
     2026-09-05：去掉后半秒时隙门控（g_half_phase，原为与翻页动画错开时间片，
     动画已停用）——日期/温湿度/设置模式按键调整即时反映，不再滞后最多 500ms。 */
  Calendar_RefreshStatic(t, temp, humi);
#else
  /* ================= 新 UI（像素游戏机，2026-11-17 接线） =================
   * Px_DrawMain 一次性重画主屏全部**动态元素**（时钟数字/冒号/秒/日期/星期/农历/
   * 温湿度/机器人精灵/气泡文字）。背景不重传（由 UI_SetScreen 负责）。
   *
   * 重画节流：只在「秒变化」或「强制刷新」（上电/切屏回主屏/授时跳变/开显示，
   * 经 g_static_next -> force_static）时跑一次 —— 绝不每轮主循环重画：
   * 一屏精灵+字串约 500 次 SPI 行事务，每帧都跑会把主循环拖垮（语音轮询/按键会卡）。
   * c_ss 复用旧秒缓存（0xFF=非法 -> 首帧必画；Calendar_ForceRedraw 会重置它）。
   * 代价：机器人精灵在整屏重画里只有 1 帧/秒；单独的 Px_RobotTick（App_Render
   * 每轮调）按 UI_CharAnim_Tick 的换帧节拍补贴，2026-11-23 起换帧 250ms（4 fps）。 */
  if (force_static || t->second != c_ss)
  {
    c_ss = t->second;
    Px_DrawMain(t);
  }
#endif
}

/* ------------------------------------------------------------------
 * Calendar_RefreshStatic：静态区块增量刷新（日期徽章 + 温湿度）。
 * 由 Calendar_Refresh 每轮调用（2026-09-05 起不再绑后半秒时隙——
 * 原为与翻页动画错开时间片，动画已停用）。
 * 参数: t=当前时间；temp/humi=温湿度（单位 0.1）。
 * 流程: 各区块与 c_* 缓存比较，变化才重绘（增量）。
 *
 * ⚠️ 2026-11-17 新 UI 接线：本函数的绘制体已 #if 0 停用（旧奶油风坐标/字号）。
 *    - 农历/温湿度「取串」逻辑已抽成 Calendar_GetLunar / Calendar_GetTempHumi（上方），
 *      新 UI 由 Px_DrawMain 统一按新坐标重画；
 *    - 保留函数本体与签名（calendar.h 公开 API），便于回退。
 * ------------------------------------------------------------------ */
void Calendar_RefreshStatic(TimeStruct *t, int temp, int humi)
{
  (void)t; (void)temp; (void)humi;   /* 新 UI：参数不再使用（取串走 Calendar_GetLunar/GetTempHumi） */
#if 0
  int wd = CalcWeekday(t->year, t->month, t->day);

  /* ---- 日期三卡（概念B，卡 y=126..152 高26，文字顶部 y≈130） ----
     日期卡[12..152]："2026.8.26" 白 16x22 数字+4x22 点号(0xE202)，中心 x=82；
     星期卡[160..222]："星期二" 绿 #22C55E，中心 x=191；
     农历卡[230..306]："八月初八" 青 #40C4FF，中心 x=268。 */
  if (t->year != c_yy || t->month != c_mo || t->day != c_dd || wd != c_wd)
  {
    /* 坑#84 新主屏（AI 奶油风）布局：日期+农历 合并到右上日期卡 [168..312]x[45..125]：
       日期串 y75（22 高）、农历 y98（22 高）；新图无星期卡 -> 去掉星期绘制。 */
    draw_date_string((u16)t->year, t->month, t->day);

    /* 农历 X月XX（右上卡，日期下方；深棕，UI_SLOT_LUNAR 档位：18px 格、清72、居中）
       对齐带文字版示例农历 x210-290 y102-118，22 高贴卡底（卡描边 y122-125，勿超）：
       月份用中文月名表 + "月"，日期按初一/初十/廿X/三十 规则转两字。 */
    { LunarDate ld;
      if (TimeModule_GetLunar(t->year, t->month, t->day, &ld) == 0)
      {
        static const char *LUNAR_MONTH[12] = { /* 正 二 三 四 五 六 七 八 九 十 冬 腊 */
          "正","二","三","四","五","六","七","八","九","十","冬","腊"};
        static const char *N01[10] = { /* 一 二 三 四 五 六 七 八 九 十 */
          "一","二","三","四","五","六","七","八","九","十"};
        char lb[12]; int ln = 0;
        const char *q = LUNAR_MONTH[ld.lunarMonth - 1]; while (*q) lb[ln++] = *q++;
        q = "月"; while (*q) lb[ln++] = *q++;
        /* 日：初一..初十 -> 初+一..十；十一..十九 -> 十+一..九；
           二十 -> 二十；廿一..廿九 -> 廿+一..九；三十 -> 三十
           （坑#71 #5：初十边界原写 <10，初十(lunarDay==10) 落入 <20 分支取 N01[-1] 越界
           -> 农历串损坏显示消失。改 <11 后 初十 = "初"+N01[9]="初十"。） */
        if (ld.lunarDay == 20)      { q = "二十"; }
        else if (ld.lunarDay == 30) { q = "三十"; }
        else if (ld.lunarDay < 11)  { q = "初"; while (*q) lb[ln++] = *q++;
                                      q = N01[ld.lunarDay - 1]; }
        else if (ld.lunarDay < 20)  { q = "十"; while (*q) lb[ln++] = *q++;
                                      q = N01[ld.lunarDay - 11]; }
        else                        { q = "廿"; while (*q) lb[ln++] = *q++;
                                      q = N01[ld.lunarDay - 21]; }
        while (*q) lb[ln++] = *q++;
        lb[ln] = 0;
        UI_Show(202, 76, lb, UI_SLOT_LUNAR, C_TIME);   /* 对齐参考农历 x202-276 y80-93：22高对齐中心 y86.5 -> y76 */
      }
      else
      {
        /* 年份超出 2000-2099 或农历表外：清空农历区（防旧值误导） */
        UI_Show(202, 76, "    ", UI_SLOT_LUNAR, C_TIME);
      }
    }
    c_yy = t->year; c_mo = t->month; c_dd = t->day; c_wd = (u8)wd;
  }

  /* ---- 温湿度数值（一位小数；值单位 0.1，如 temp=255 -> "25.5"） ----
     概念B 两张迷你卡：温度卡[100..198]、湿度卡[204..304]（图标 26x26 @113/217,212 已入背景），
     数字 12x30 白字分别居中于卡内图标右侧（温度[128..196]、湿度[232..300]），y≈197。
     2026-08 架构重构：统一走 UI_Show（UI_SLOT_TH 档位：格12、清69、居中）。 */
  if (temp != c_temp)                    /* 温度变化才重绘 */
  {
    char tb[8]; int n = 0;
    int ti = temp / 10, tf = temp % 10;
    if (tf < 0) tf = -tf;
    if (ti < 0) { tb[n++] = '-'; ti = -ti; }
    if (ti >= 100) tb[n++] = (char)('0' + ti / 100);
    if (ti >= 10 || n) tb[n++] = (char)('0' + (ti / 10) % 10);
    tb[n++] = (char)('0' + ti % 10);
    tb[n++] = '.';
    tb[n++] = (char)('0' + tf); tb[n] = 0;
    UI_Show(43, 196, tb, UI_SLOT_TH2, UI_RGB(239, 68, 68));   /* 坑#84 左下温湿度卡：温度红 22高（对齐示例数值 x43-92 y198-214） */
    c_temp = temp;
  }
  if (humi != c_humi)                    /* 湿度变化才重绘 */
  {
    char hb[8]; int n = 0;
    int hi = humi / 10, hf = humi % 10; if (hf < 0) hf = -hf;
    if (hi >= 100) hb[n++] = (char)('0' + hi / 100);
    if (hi >= 10 || n) hb[n++] = (char)('0' + (hi / 10) % 10);
    hb[n++] = (char)('0' + hi % 10);
    hb[n++] = '.';
    hb[n++] = (char)('0' + hf); hb[n] = 0;
    UI_Show(100, 196, hb, UI_SLOT_TH2, UI_RGB(64, 196, 255));  /* 坑#84：湿度蓝 22高（x115→100 左移防小数位超界，2026-10-10） */
    c_humi = humi;
  }
#endif /* 旧 UI 静态区块绘制（新 UI 停用，逻辑见 Calendar_GetLunar/GetTempHumi） */
}

/* ============ 冒号闪烁（0.5s 中断驱动，g_colon_blink_flag 触发） ============ */
static u8 colon_on = 1;   /* 冒号当前亮灭（文件级，供设置模式强制亮，坑#71 #3 2026-09-05） */
void Calendar_ColonBlink(void)
{
  colon_on ^= 1;                 /* 相位照常翻转（回退旧 UI 时状态连续，新 UI 不画） */
#if 0
  draw_colon(CLK_X_COL, colon_on);   /* 旧 UI 冒号闪烁（新 UI 冒号是静态精灵） */
#endif
}

/* 强制冒号亮起（坑#71 #3 2026-09-05）：进时间设置模式时调用——设置期间冒号停闪，
   若进设置瞬间恰好处于闪烁"灭"相位，设置阶段会一直是灭的（用户反馈）。
   强制亮起并复位闪烁相位（下次从亮开始翻灭），退出设置后恢复闪烁。 */
void Calendar_ColonForceOn(void)
{
  colon_on = 1;
#if 0
  draw_colon(CLK_X_COL, 1);          /* 旧 UI 冒号（新 UI 冒号常亮，无需重画） */
#endif
  (void)colon_on;                    /* 相位变量仅旧 UI 用（防 -Wunused-but-set-variable） */
}

/* ------------------------------------------------------------------
 * Calendar_SetStaticNext：请求下一次 Refresh 强制静态刷新。
 * 用途: 蓝牙授时等大幅跳变（时分秒同时变化）后调用——直接静态显示，
 *       避免逐位串行翻页动画（看起来像“连翻多次”）。
 * ------------------------------------------------------------------ */
void Calendar_SetStaticNext(void)
{
  g_static_next = 1;
}

/* ------------------------------------------------------------------
 * Calendar_ForceRedraw：切屏回主屏后调用——主屏背景已由 UI_SetScreen 重新上传
 * （背景上无任何动态文字），必须重置全部增量缓存（c_* 设非法值）+ 置 g_static_next，
 * 让下一次 Calendar_Refresh 把时钟/日期/温湿度/蓝牙 全量重绘。
 * 若不重置：Calendar_Refresh 用 c_* 与当前值比较判定"没变化"，切屏期间缓存仍等于
 * 当前时间 -> 回主屏后数字/日期/小数点不重绘（消失）、温湿度因数值波动只部分重绘。
 * 2026-08-28 修复。
 * ------------------------------------------------------------------ */
void Calendar_ForceRedraw(void)
{
  c_yy = 0xFFFF; c_mo = 0xFF; c_dd = 0xFF; c_wd = 0xFF;
  c_hh = c_mm = c_ss = 0xFF;
  c_temp = c_humi = 0x7FFF;
  /* 旧 UI 增量缓存（c_hh/c_mm/c_yy/c_mo/c_dd/c_wd/c_temp/c_humi）现在只被 #if 0 的
     旧绘制读取：这里统一读一次，防 -Wall 的 -Wunused-but-set-variable 噪声
     （回退旧 UI 后本行可删）。c_ss 仍是新 UI 的「秒变化」判据，不用在此读。 */
  (void)c_hh; (void)c_mm; (void)c_yy; (void)c_mo; (void)c_dd; (void)c_wd;
  (void)c_temp; (void)c_humi;
  s_bt_connected = 0xFF;
  g_static_next   = 1;
}

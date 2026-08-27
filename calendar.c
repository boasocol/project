/************************************************************************************************************
 * @file    calendar.c
 * @brief   日历界面：翻页动画状态机 + 增量刷新（横屏 320x240）
 *
 * @note    本模块不程序化绘制背景，而是在 Flash 中的 UI 背景图（ui_320x240.bin，
 *          由 gen_ui.py 生成）上用 Flash 字库（ui.c）按坐标叠加动态文字。
 *
 *          界面各区块逻辑坐标（x, y 起点，y 为设计坐标，见 YF 宏）：
 *            - 时钟数字：6 张卡片 40x84，x=[13,59,117,163,221,267]，y=14
 *            - 日期徽章：y=116（年 19 / 月 95 / 日 139 / 星期 180 / 农历 240）
 *            - 蓝牙状态：x[180..189] 蓝牙符文 + x[190..203] 勾/叉图标（无药丸），
 *              竖向居中于温湿度区域 y[154..223]
 *            - 温湿度格：一上一下两块（温度 y=154、湿度 y=190，各高 34，
 *              横跨 x[206..303]，对齐星期框左边缘 x=180），12x30 加大版数字
 *
 *          核心机制：
 *            1. Calendar_Refresh()：每秒由 main 调用，逐区块比较“上次绘制值 c_*”
 *               vs 当前时间，只重绘变化区块（增量刷新）；时钟变化位交给翻页动画。
 *            2. Calendar_FlipTick()：每 12ms 由 main 按 BFTM0 计数调用，双卡并行
 *               推进翻页动画（最多同时翻 2 张卡，每张帧率同为 12ms/帧，与单卡一致）。
 *            3. 动画数据 ANI2 在 Flash @0x110000，由 build_flash.py 生成；
 *               本文件只按数据头的帧数/亮度表播放，不硬编码帧数。
 *
 *          设备显示约定：设备对 GRAM 做垂直镜像，背景与字模均已 FV 预补偿，
 *          动画帧表也已按设备镜像构建（见 build_flash.py build_anim）。
 ************************************************************************************************************/
#include "calendar.h"
#include "ui.h"
#include "time_module.h"
#include "BFTM0.h"   /* bftm0_ct（1ms 计数，调试测帧耗时） */

/* ------------------------------------------------------------------
 * YF(y, h)：设计坐标 -> GRAM 行换算宏。
 * 设备对 GRAM 做垂直镜像，逻辑(设计)坐标 y（0 在顶）需换算为 GRAM 行：
 *   GRAM 行 = 239 - y - h   （x 不变）
 * 背景 bin 与字库字形均已做同样预补偿，二者一致文字才能落在正确位置。
 * 参数: y=区块顶部的设计 y，h=区块高度。
 * 返回: 区块顶部对应的 GRAM 行。
 * ------------------------------------------------------------------ */
#define YF(y, h) (239 - (y) - (h))

/* ------------------------------------------------------------------
 * 数字尺寸变体 code 段（与 build_flash.py 的 build_font 一一对应）：
 *   0x30+d       时钟大数字 40x84
 *   0xE000+d     日期小号数字 16x22（DG_SMALL）
 *   0xE100+d     温湿度数字 8x22（DG_TH）
 * 绘制时按 code 段取字模，1:1 不缩放，避免误查别的大号字模被缩放。
 * ------------------------------------------------------------------ */
#define DG_SMALL 0xE000
#define DG_TH    0xE100

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

/* ------------------------------------------------------------------
 * WD_CN[7][3]：中文星期表（用 Unicode 码点，避免源文件中文字面量被 gcc 误解析）。
 * 下标 = CalcWeekday 返回值（0=周一）。每项 3 个汉字（“星期一”…“星期日”）。
 * 星=0x661F 期=0x671F 一=0x4E00 二=0x4E8C 三=0x4E09 四=0x56DB 五=0x4E94 六=0x516D 日=0x65E5
 * ------------------------------------------------------------------ */
static const u32 WD_CN[7][3] = {
  {0x661F, 0x671F, 0x4E00},
  {0x661F, 0x671F, 0x4E8C},
  {0x661F, 0x671F, 0x4E09},
  {0x661F, 0x671F, 0x56DB},
  {0x661F, 0x671F, 0x4E94},
  {0x661F, 0x671F, 0x516D},
  {0x661F, 0x671F, 0x65E5},
};

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

/* ------------------------------------------------------------------
 * draw_clock_digit：把单个时钟大数字静态画到卡片上（无动画）。
 * 参数: x=卡片左边界逻辑 x；d=数字 0..9。
 * 用途: 动画不可用 / 强制静态刷新（上电首刷、蓝牙授时跳变）时直绘。
 * ------------------------------------------------------------------ */
static void draw_clock_digit(int x, int d)
{
  UI_DrawGlyphRect(x, YF(14, 84), 40, 84, (u32)('0' + d), UI_C_DIGIT);
}

/* ---- 冒号：14x84 区域（组间 18 间隙中居中，左右各 2px 呼吸）；on=0 时用空格字模 ----
   冒号闪烁恢复启用（翻页动画停用，见 g_anim_ok）。背景图未画冒号，由 MCU 绘制。 */
static void draw_colon(int x, int on)
{
  UI_DrawGlyphRect(x, YF(14, 84), 14, 84, on ? (u32)':' : (u32)' ', UI_C_COLON);
}

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

/* 卡片上下半的 GRAM 行：整卡 GRAM 起始 = YF(14,84)=141。
   上半 = 141 起始，下半 = 141+42 = 183 起始。 */
#define FLIP_CARD_Y0     (YF(14, 84))     /* 整卡 GRAM 起始行（=141） */
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

/* ------------------------------------------------------------------
 * flip_start：登记一次翻页任务（排队，不立即播放）。
 * 参数: slot=槽下标(0..5)；old_digit=起始数字；new_digit=目标数字。
 * 说明: 一律记录目标并置 g_pending。即使该位正在翻页/已排队，也更新目标，
 *       避免“翻到一半来了新变化”时把这次翻转弄丢（旧逻辑会导致跳数字）。
 *       old_digit>9（如 0xFF）表示首次无旧值，退化用 new 当起点。
 * ------------------------------------------------------------------ */
static void flip_start(int slot, u8 old_digit, u8 new_digit)
{
  if (old_digit > 9) old_digit = new_digit;   /* 首次无旧值：退化为静态刷新 */
  g_pend_old[slot] = old_digit;
  g_pend_new[slot] = new_digit;
  g_pending[slot]  = 1;
}

/* g_anim_ok：动画区是否可用（Calendar_Init 时记录 UI_Anim_Ready() 结果）。
   0=不可用（Flash 未烧新 ANIM 区等），时钟变化位直接静态直绘。 */
static u8 g_anim_ok = 0;
/* g_static_next：蓝牙授时等大幅跳变标志。置位后下一次 Refresh 强制静态刷新
   （不做逐位连翻动画），Refresh 读到即清零。 */
static u8 g_static_next = 0;
/* g_static_defer：小时进位顺延标志。Calendar_Refresh 检测到【小时】变化时置位，
   表示本秒多位动画可能占满两个半秒 -> 本秒后半秒的静态刷新（日期/温湿度）被抑制，
   顺延到下一秒后半秒执行。main 每秒调 Refresh 时开头清零、重新判定。 */
static u8 g_static_defer = 0;

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

/* ------------------------------------------------------------------
 * Calendar_AnimBusy：查询是否有翻页动画正在进行。
 * 返回: 1=有活动动画（任一活动槽>=0），0=动画空闲。
 * 用途: main 后半秒调度判断——动画还在翻则先不刷静态区块。
 * ------------------------------------------------------------------ */
int Calendar_AnimBusy(void)
{
  return (g_active_a >= 0 || g_active_b >= 0) ? 1 : 0;
}

/* ------------------------------------------------------------------
 * Calendar_StaticDeferred：查询小时进位顺延标志。
 * 返回: 1=本秒静态刷新被顺延（等待下一秒后半秒补刷），0=正常。
 * ------------------------------------------------------------------ */
int Calendar_StaticDeferred(void)
{
  return (int)g_static_defer;
}

/* ------------------------------------------------------------------
 * Calendar_ClearStaticDeferred：清除小时进位顺延标志。
 * 用途: main 在下一秒后半秒补刷静态区块前调用。
 * ------------------------------------------------------------------ */
void Calendar_ClearStaticDeferred(void)
{
  g_static_defer = 0;
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
 * ------------------------------------------------------------------ */
static void draw_bt_state(int connected)
{
  u32 icon = connected ? 0xE300u : 0xE301u;   /* 对勾 / 叉 */
  /* 蓝牙图标已左移（中心 x=183，占 x[170..196]），勾/叉在其右侧 x=198..211 */
  UI_DrawGlyphRect(198, YF(173, 30), 14, 30, icon, UI_RGB(255, 60, 60));
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
  UI_UploadBackground();          /* 整屏上传 Flash 中的新 UI 背景图 */
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

  /* 初始冒号：亮起（冒号闪烁已恢复） */
  draw_colon(101, 1);
  draw_colon(205, 1);

  /* 语音助手小女孩：按 MSCT 尺寸自动定位（72x72 区，尺寸无关，与动画同区对齐） */
  UI_DrawMascotCentered();

  /* 形象状态动画（CANI）：帧图=形象图\default_girl_1..48.png（101+ 编号为暂存不入脚本）。
     每状态 16 帧（8 循环 + 4 变化到2 + 4 变化到3），0.125s/帧（8 次/秒）。
     要恢复静态只显示第 1 帧：改回 UI_CharAnim_Enable(0)。 */
  (void)UI_CharAnim_Init();
  UI_CharAnim_Enable(1);

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
  int wd = CalcWeekday(t->year, t->month, t->day);

  /* 每秒先清小时进位顺延标志；本次检测到小时变化会重新置位 */
  g_static_defer = 0;

  /* 蓝牙授时等大幅跳变：本次强制静态刷新（不做逐位连翻动画），并取消进行中的动画 */
  int force_static = g_static_next;
  g_static_next = 0;
  if (force_static && g_anim_ok)
  {
    g_active_a = -1;
    g_active_b = -1;
    for (int i = 0; i < 6; i++) { g_flip[i].state = 0; g_pending[i] = 0; }
  }

  /* ---- 时钟数字（6 位独立变化检测，只翻真正变化的那一位） ----
     动画可用且非强制静态：变化位启动翻页（Calendar_FlipTick 推进帧）；
     否则直接画静态字模并更新 g_disp（保证后续翻页起始数字正确）：
     覆盖上电首刷、蓝牙授时跳变、Flash 无 ANIM 区等场景。 */
  /* slot 0：时十位，卡片 x=13。
     小时变化=多位进位（时分秒可能同时翻），动画可能占满两个半秒 ->
     置 g_static_defer 让本秒后半秒的静态刷新顺延到下一秒。 */
  if ((t->hour / 10) != (c_hh / 10))
  {
    if (g_anim_ok && !force_static) { flip_start(0, c_hh / 10, t->hour / 10); g_static_defer = 1; }
    else                            { draw_clock_digit(13, t->hour / 10); g_disp[0] = (u8)(t->hour / 10); }
  }
  /* slot 1：时个位，卡片 x=59 */
  if ((t->hour % 10) != (c_hh % 10))
  {
    if (g_anim_ok && !force_static) { flip_start(1, c_hh % 10, t->hour % 10); g_static_defer = 1; }
    else                            { draw_clock_digit(59, t->hour % 10); g_disp[1] = (u8)(t->hour % 10); }
  }
  c_hh = t->hour;

  /* slot 2：分十位，卡片 x=117 */
  if ((t->minute / 10) != (c_mm / 10))
  {
    if (g_anim_ok && !force_static) flip_start(2, c_mm / 10, t->minute / 10);
    else                            { draw_clock_digit(117, t->minute / 10); g_disp[2] = (u8)(t->minute / 10); }
  }
  /* slot 3：分个位，卡片 x=163 */
  if ((t->minute % 10) != (c_mm % 10))
  {
    if (g_anim_ok && !force_static) flip_start(3, c_mm % 10, t->minute % 10);
    else                            { draw_clock_digit(163, t->minute % 10); g_disp[3] = (u8)(t->minute % 10); }
  }
  c_mm = t->minute;

  /* slot 4：秒十位，卡片 x=221 */
  if ((t->second / 10) != (c_ss / 10))
  {
    if (g_anim_ok && !force_static) flip_start(4, c_ss / 10, t->second / 10);
    else                            { draw_clock_digit(221, t->second / 10); g_disp[4] = (u8)(t->second / 10); }
  }
  /* slot 5：秒个位，卡片 x=267 */
  if ((t->second % 10) != (c_ss % 10))
  {
    if (g_anim_ok && !force_static) flip_start(5, c_ss % 10, t->second % 10);
    else                            { draw_clock_digit(267, t->second % 10); g_disp[5] = (u8)(t->second % 10); }
  }
  c_ss = t->second;

  /* 强制静态刷新（授时/上电）时：时钟位已静态直画，这里顺带把静态区块
     （日期/温湿度）一次画全，避免授时改日期后不同步。 */
  if (force_static)
    Calendar_RefreshStatic(t, temp, humi);
}

/* ------------------------------------------------------------------
 * Calendar_RefreshStatic：静态区块增量刷新（日期徽章 + 温湿度）。
 * 由 main 在【后半秒】时隙、动画空闲时调用，与翻页动画错开时间片；
 * 小时进位秒被 g_static_defer 抑制，顺延到下一秒后半秒补刷。
 * 参数: t=当前时间；temp/humi=温湿度（单位 0.1）。
 * 流程: 各区块与 c_* 缓存比较，变化才重绘（增量）。
 * ------------------------------------------------------------------ */
void Calendar_RefreshStatic(TimeStruct *t, int temp, int humi)
{
  int wd = CalcWeekday(t->year, t->month, t->day);

  /* ---- 日期徽章（y=116, h=22，字高 22，整串在徽章内居中） ----
     年 4 位小号数字(白)、月 2 位(白)、日 2 位(黑)、星期 3 汉字(白)、农历 4 汉字(白)。
     任一字段变化即整串重绘。 */
  if (t->year != c_yy || t->month != c_mo || t->day != c_dd || wd != c_wd)
  {
    char yb[8], mb[4], db[4];
    int n = 0; int y = t->year;
    yb[n++] = (char)('0' + y / 1000); yb[n++] = (char)('0' + (y / 100) % 10);
    yb[n++] = (char)('0' + (y / 10) % 10); yb[n++] = (char)('0' + y % 10); yb[n] = 0;
    mb[0] = (char)('0' + t->month / 10); mb[1] = (char)('0' + t->month % 10); mb[2] = 0;
    db[0] = (char)('0' + t->day / 10);   db[1] = (char)('0' + t->day % 10);   db[2] = 0;

    UI_DrawVarDigits(yb, 19, YF(116, 22), UI_C_WHITE, DG_SMALL);   /* 年 小号数字 */
    UI_DrawVarDigits(mb, 95, YF(116, 22), UI_C_WHITE, DG_SMALL);   /* 月 小号数字 */
    UI_DrawVarDigits(db, 139, YF(116, 22), UI_C_BLACK, DG_SMALL);  /* 日 小号数字(黑) */
    UI_DrawGlyphs(WD_CN[wd], 3, 180, YF(116, 22), 18, 22, UI_C_WHITE);  /* 星期(3*18=54) */
    /* 农历 X月XX（4 汉字，如"六月廿五"，4*18=72px）：
       月份用中文月名表 + "月"，日期按初一/初十/廿X/三十 规则转两字。 */
    { LunarDate ld;
      if (TimeModule_GetLunar(t->year, t->month, t->day, &ld) == 0)
      {
        static const u32 LUNAR_MONTH[12] = { /* 正 二 三 四 五 六 七 八 九 十 冬 腊 */
          0x6B63,0x4E8C,0x4E09,0x56DB,0x4E94,0x516D,0x4E03,0x516B,0x4E5D,0x5341,0x51AC,0x814A};
        static const u32 N01[10] = { /* 一 二 三 四 五 六 七 八 九 十 */
          0x4E00,0x4E8C,0x4E09,0x56DB,0x4E94,0x516D,0x4E03,0x516B,0x4E5D,0x5341};
        u32 b[4]; u8 lm = ld.lunarMonth, ld2 = ld.lunarDay;
        b[0] = LUNAR_MONTH[lm - 1];
        b[1] = 0x6708;                          /* 月 */
        /* 日：初一..初十 -> 初+一..十；十一..十九 -> 十+一..九；
           二十 -> 二十；廿一..廿九 -> 廿+一..九；三十 -> 三十 */
        if (ld2 == 20)      { b[2] = 0x4E8C; b[3] = 0x5341; }        /* 二十 */
        else if (ld2 == 30) { b[2] = 0x4E09; b[3] = 0x5341; }        /* 三十 */
        else if (ld2 < 10)  { b[2] = 0x521D; b[3] = N01[ld2 - 1]; }  /* 初X */
        else if (ld2 < 20)  { b[2] = 0x5341; b[3] = N01[ld2 - 11]; } /* 十X */
        else                { b[2] = 0x5EFF; b[3] = N01[ld2 - 21]; } /* 廿X */
        UI_DrawGlyphs(b, 4, 240, YF(116, 22), 18, 22, UI_C_WHITE);
      }
      else
      {
        /* 年份超出 2000-2099：显示"数据越界"占位，避免空白 */
        static const u32 err[4] = {0x6570, 0x636E, 0x8D8A, 0x754C};   /* 数据越界 */
        UI_DrawGlyphs(err, 4, 240, YF(116, 22), 18, 22, UI_C_WHITE);
      }
    }
    c_yy = t->year; c_mo = t->month; c_dd = t->day; c_wd = (u8)wd;
  }

  /* ---- 温湿度数值（一位小数；值单位 0.1，如 temp=255 -> "25.5"） ----
     一上一下两块：温度格 y=154、湿度格 y=190，各横跨 x[206..303]（宽 98）。
     每格内【图标 + 12x30 数字】整体居中：
       温度图标在 x=210（宽 8，右缘 ~218），数字串居中于 [218..303]
       湿度图标在 x=210（宽 12，右缘 ~222），数字串居中于 [222..303]
     数字 12 宽 + 小数点 4 宽，按实际串宽居中，任何位数都不超界。
     数值垂直居中于 34px 高格内（数字高 30 -> 顶部 y=格y+2）。 */
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
    /* 温度格：图标右缘 ~218 到格右缘 303，数字居中；y=154+2=156（高30居中于高34格） */
    int nw = 0; for (char *q = tb; *q; q++) nw += (*q == '.') ? 4 : 12;
    int tx = 218 + (303 - 218 - nw) / 2;
    UI_DrawVarDigits(tb, tx, YF(156, 30), UI_C_WHITE, DG_TH);
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
    /* 湿度格：图标右缘 ~222 到格右缘 303，数字居中；y=190+2=192 */
    int nw = 0; for (char *q = hb; *q; q++) nw += (*q == '.') ? 4 : 12;
    int hx = 222 + (303 - 222 - nw) / 2;
    UI_DrawVarDigits(hb, hx, YF(192, 30), UI_C_WHITE, DG_TH);
    c_humi = humi;
  }
}

/* ============ 冒号闪烁（0.5s 中断驱动，g_colon_blink_flag 触发） ============ */
void Calendar_ColonBlink(void)
{
  static u8 colon_on = 1;
  colon_on ^= 1;
  draw_colon(101, colon_on);
  draw_colon(205, colon_on);
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

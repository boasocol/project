/************************************************************************************************************
 * @file    key.c
 * @brief   按键模块（Driver 层）：PB12/PB13/PB14 三按键，产出统一事件流。
 *
 * 职责边界（2026-09-05 重构：按键事件流化）：
 *   - 物理采集（EXTI 下降沿 + 硬件消抖）-> 三键统一长/短按判定 -> KeyEvent{键号,短/长按} 入队；
 *     Key_GetEvent 出队供应用层消费。本模块【不包含任何业务语义】——
 *     哪个键长按/短按在当前状态干什么，由应用层 Screen_HandleKey 集中分发。
 *   - 关屏/开屏/深休眠1 是系统级动作（WFI/外设时钟/LCD 电源/Flash 掉电），仍在本模块；
 *     【何时关屏】的决策在应用层：应用层对 KEY2 长按事件调 Key_ScreenOff()。
 *
 * 事件流（三键统一判定）：
 *   ISR(Key_HandleIrq)  : 三键下降沿 -> 置 g_pend[i] + 记按下时刻（主循环判定用）
 *   Key_Process->key_scan: 软件消抖（电平稳定 ≥20ms 才认定按下/松开）-> 松开=短按入队；按住≥1s=长按入队
 *   main.c               : Key_GetEvent 出队 -> Screen_HandleKey 按「事件类型->键号->状态」分发
 *
 * 消抖（2026-09-05 双层）：
 *   硬件：EXTI_DEBOUNCE_ENABLE + DebounceCnt=40（过滤 EXTI 边沿检测抖动）
 *   软件：key_scan 读 GPIO 实际电平，电平变化后需连续稳定 KEY_DEBOUNCE_MS(20ms) 才判定，
 *         按下/松开瞬间接触抖动（<10ms）被窗口滤除——解决"一次按压误发多个短按"
 *         与"长按后松开抖动残留补发短按"（g_press_conf 按压确认标志）。
 *
 * 低功耗全链路：关屏由应用层 KEY2 长按调 Key_ScreenOff()（DISPOFF+背光灭+SLPIN+蓝牙断电+
 *   Flash 掉电 -> DeepSleep1(WFI)）；唤醒仍走 KEY1(PB12) 低电平唤醒事件（EVWUP），
 *   唤醒后自动开屏并丢弃唤醒键事件。KEY2/KEY3 暂不参与唤醒。
 ************************************************************************************************************/
#include "ht32.h"
#include "ht32l5xxxx_pwrcu.h"  /* PWRCU_DeepSleep1 / PWRCU_SLEEP_ENTRY_WFI */
#include "key.h"
#include "bsp_pins.h"     /* KEY_* 引脚/EXTI 映射（P1 集中，2026-09-03） */
#include "spi_lcd.h"      /* 背光控制脚 PB4 + SPI_LCD_WriteCmd */
#include "Display.h"      /* ST7789_DISPOFF / SLPIN / SLPOUT / DISPON 命令（原 st7789.h，2026-09-03 合并） */
#include "flash_read.h"   /* Flash_PowerDown / Flash_ReleasePowerDown */
#include "time_module.h"  /* RTC_IntConfig（休眠/唤醒保持 CSEC 走时） */
#include "BFTM0.h"        /* bftm0_ct：1ms 节拍（长按计时 + 唤醒后完整恢复） */

/* P2-3（2026-09-03）分层反转：本模块不再 include calendar.h / hd9803.h。
   关屏/开屏的应用层副作用（日历强制刷新、蓝牙断电/上电）由应用层经
   Key_SetScreenOffCb / Key_SetScreenOnCb 注册的回调执行（Key_ScreenOff / key_screen_on 内调用）。 */
static KeyEventCb g_off_cb = 0;   /* 关屏回调（应用层注册，可空） */
static KeyEventCb g_on_cb  = 0;   /* 开屏回调（应用层注册，可空） */

void Key_SetScreenOffCb(KeyEventCb cb) { g_off_cb = cb; }
void Key_SetScreenOnCb(KeyEventCb cb)  { g_on_cb  = cb; }

/* 按键引脚/EXTI 映射见 bsp_pins.h（KEY_GPIO/PIN/AFIO/EXTI，2026-09-04 三键）。
   KEY1=PB12（EXTI12，兼低功耗唤醒）、KEY2=PB13（EXTI13）、KEY3=PB14（EXTI14），
   三键同端口 B、同属 EXTI4_15 中断组（共享 EXTI4_15_IRQn）。
   数组下标 = key_id_t（KEY_ID_1/2/3 = 0/1/2）。 */
static const struct {
  u16  pin;    /* GPIO_PIN_x */
  u16  afio;   /* AFIO_PIN_x（复用/上拉参数） */
  u8   num;    /* GPIO_PIN_NUM_x（AFIO_EXTISourceConfig 用） */
  u32  exti;   /* EXTI_CHANNEL_x */
} g_keys[KEY_COUNT] = {
  { KEY1_PIN, KEY1_AFIO, GPIO_PIN_NUM_12, EXTI_CHANNEL_12 },
  { KEY2_PIN, KEY2_AFIO, GPIO_PIN_NUM_13, EXTI_CHANNEL_13 },
  { KEY3_PIN, KEY3_AFIO, GPIO_PIN_NUM_14, EXTI_CHANNEL_14 },
};

/* ---- 事件队列（仅主循环单线程读写：key_scan 入队、Key_GetEvent 出队，ISR 不碰；满则丢弃） ---- */
#define KEY_Q_MAX  8
static KeyEvent g_key_q[KEY_Q_MAX];
static u8       g_q_rd = 0, g_q_wr = 0;

static void key_q_push(key_id_t id, key_evt_t evt)
{
  u8 n = (u8)(g_q_wr + 1);
  if (n >= KEY_Q_MAX) n = 0;
  if (n == g_q_rd) return;             /* 队列满：丢弃（主循环每轮消费，正常不应发生） */
  g_key_q[g_q_wr].id  = id;
  g_key_q[g_q_wr].evt = evt;
  g_q_wr = n;
}

int Key_GetEvent(KeyEvent *out)
{
  if (g_q_rd == g_q_wr) return 0;
  *out = g_key_q[g_q_rd];
  g_q_rd++;
  if (g_q_rd >= KEY_Q_MAX) g_q_rd = 0;
  return 1;
}

static void key_q_clear(void) { g_q_rd = g_q_wr = 0; }

/* ---- 按键判定状态（ISR 写 pend/press_ms，主循环 key_scan 消费） ---- */
static volatile u8  g_pend[KEY_COUNT];      /* 1=该键已按下（ISR 下降沿置位 / key_scan 稳定确认补置），等待松开(短按)/超时(长按)判定 */
static volatile u32 g_press_ms[KEY_COUNT];  /* 按下时刻（bftm0_ct，长按计时） */
static volatile u8  g_key_wake = 0;         /* KEY1(PB12) 低功耗唤醒标志（EVWUP 置位，深休眠循环消费） */
static u8           g_screen_off = 0;       /* 1=关屏低功耗 */
static u8           g_wait_release = 0;     /* 1=等待唤醒键松开（防唤醒事件电平触发反复开关屏） */
volatile u8 g_key_deep_sleep_flag = 0;      /* 深度休眠测试标志：进休眠置1，开屏退出清0（main 打印用） */

/* ---- 软件消抖状态（2026-09-05：key_scan 稳定窗口判定，硬件 EXTI 消抖之外的第二层） ----
   硬件消抖只过滤 EXTI 下降沿检测；主循环直接读 GPIO 电平，按下/松开瞬间的接触抖动
   会让电平在窗口内反复翻转，若按"读到高=松开"立即入队，一次物理按下会误发多个短按。
   故 key_scan 以"电平连续稳定 ≥KEY_DEBOUNCE_MS"为准：电平变化重启稳定计时，
   稳定满窗口才做 按下确认/松开入队/长按计时 判定。 */
#define KEY_DEBOUNCE_MS  20u   /* 软件消抖窗口：电平需连续稳定 20ms（接触抖动一般 <10ms） */
static u8  g_scan_lvl[KEY_COUNT];   /* 上一次读到的电平：1=按下(低)，0=松开(高) */
static u32 g_stable_ms[KEY_COUNT];  /* 当前电平首次出现时刻（bftm0_ct，稳定计时起点） */
static u8  g_press_conf[KEY_COUNT]; /* 1=本次按压已确认（稳定低电平 ≥窗口 后置位；入队短按/长按时清零）——
                                       防止松开抖动时 ISR 重新置 pend 导致 长按后误补短按 / 一次按压多事件 */
static u8  g_rep_armed[KEY_COUNT];  /* 1=该键长按已触发、进入连发态（按住期间发 KEY_EV_REPEAT，松开清零，2026-09-05） */
static u32 g_rep_next_ms[KEY_COUNT];/* 下次连发时刻（bftm0_ct） */

/* 复位软件消抖状态到当前实际电平：上电 / 进休眠前 / 开屏等按键松开期间调用，
   防休眠/唤醒期间电平变化被当作一次真实按下（唤醒键松开不产生短按）。 */
static void key_scan_state_reset(void)
{
  for (u8 i = 0; i < KEY_COUNT; i++)
  {
    g_scan_lvl[i] = (GPIO_ReadInBit(KEY_GPIO, g_keys[i].pin) == RESET) ? 1u : 0u;
    g_stable_ms[i] = (u32)bftm0_ct;
    g_pend[i]      = 0;
    g_press_conf[i] = 0;
    g_rep_armed[i] = 0;             /* 复位连发态（上电/休眠/唤醒期间无连发残留，2026-09-05） */
  }
}

#define KEY_LONG_MS  1000u   /* 长按阈值：按住 ≥1000ms = 长按；1000ms 内松开 = 短按 */
#define KEY_REPEAT_MS 180u   /* 连发间隔：长按触发后按住期间每隔 180ms 发一次 KEY_EV_REPEAT（快加快减，2026-09-05） */

/* 进入深度休眠1：直接调用库函数 PWRCU_DeepSleep1。
   库内部已完整封装 VDDREN 保持、SLEEPDEEP 置位、LDO off 与 WFI 唤醒流程。
   唤醒源为“事件模式的 EXTI / RTC / LVD / WAKEUPn”：KEY1(PB12) 低电平唤醒事件
   经 EVWUP 中断线唤醒 WFI（NVIC_EnableIRQ(EVWUP_IRQn) 已在 Key_Init 使能）。
   每次进休眠前重开 KEY1 低电平唤醒事件（Key_ClearWakeup 会暂关，防按键按住时电平粘滞）。
   KEY2/KEY3 暂不参与唤醒（见 key.h 说明）。 */
static void key_enter_deep_sleep1(void)
{
  EXTI_WakeupEventConfig(KEY1_EXTI, EXTI_WAKEUP_LOW_LEVEL, ENABLE);  /* 进休眠前重开 PB12 低电平唤醒事件 */
  PWRCU_DeepSleep1(PWRCU_SLEEP_ENTRY_WFI);
}

/* 进深度休眠1前：关闭与唤醒无关的外设时钟，进一步压低电流。
   保留 PB / AFIO / EXTI / BKP（按键 EXTI 唤醒事件与 RTC 走时所需）。
   注意：Deep Sleep 1（LDO 保留）下不能关闭内部 Flash 缓存——
   唤醒瞬间 CPU 需从 Flash 取指，缓存关闭会导致取指异常（HardFault）而“假死”/按键不唤醒，
   故只关外设时钟、不动 Flash 缓存。 */
static void key_periph_off(void)
{
  CKCU_PeripClockConfig_TypeDef clk = {{ 0 }};
  clk.Bit.SPI0   = 1;   /* 屏 SPI */
  clk.Bit.USART0 = 1;   /* 蓝牙串口 */
  clk.Bit.UART0  = 1;   /* 调试串口 */
  clk.Bit.BFTM0  = 1;   /* 1ms 节拍（中断已禁，时钟一并关掉） */
  clk.Bit.BFTM1  = 1;   /* 未使用 */
  clk.Bit.PA     = 1;   /* SPI / Flash-CS / 蓝牙电源脚所在端口 */
  clk.Bit.PC     = 1;   /* 蓝牙 RX 所在端口 */
  CKCU_PeripClockConfig(clk, DISABLE);
}

/* 唤醒后：恢复外设时钟。必须在任何 SPI / Flash / BFTM / GPIO 访问之前调用。
   注意：GPIO 库 AUTO_CK_CONTROL 机制会按需开关 AHB 端口时钟（PA~PE），
   因此恢复时必须把项目用到的端口（含 PB 背光/按键、PA Flash/蓝牙电源、PC 蓝牙 RX）
   以及 AFIO/EXTI/RTC(BKP) 一并恢复，缺一个都可能在访问该端口寄存器时 HardFault。 */
static void key_periph_on(void)
{
  CKCU_PeripClockConfig_TypeDef clk = {{ 0 }};
  clk.Bit.SPI0   = 1;   /* 屏 SPI */
  clk.Bit.SPI1   = 1;   /* Flash SPI */
  clk.Bit.USART0 = 1;   /* 蓝牙串口 */
  clk.Bit.UART0  = 1;   /* 调试串口 */
  clk.Bit.BFTM0  = 1;   /* 1ms 节拍 */
  clk.Bit.BFTM1  = 1;   /* 未使用 */
  clk.Bit.PA     = 1;   /* SPI / Flash-CS / 蓝牙电源脚所在端口 */
  clk.Bit.PB     = 1;   /* 背光 / LCD-RST / 按键 */
  clk.Bit.PC     = 1;   /* 蓝牙 RX 所在端口 */
  clk.Bit.AFIO   = 1;   /* GPIO 复用 */
  clk.Bit.EXTI   = 1;   /* 按键 EXTI */
  clk.Bit.BKP    = 1;   /* RTC 备份域（时基） */
  CKCU_PeripClockConfig(clk, ENABLE);
}

static void key_screen_on(void);            /* 前向声明：Key_ScreenOff 的休眠循环里在按键唤醒时调用 */
static void key_sleep_until_key_pressed(void); /* 前向声明：循环休眠直到按键唤醒 */

/************************************************************************************************************
 * @brief  上电「左右键同按」检测 —— 外部烧录存储 IC 时让主控闭嘴。
 *
 * 场景（2026-11-23 用户提出）：用烧录器给板上的 SPI Flash 烧录时，烧录器会把电反灌到板子，
 * 主控因此被"半上电"启动；如果主控这时又初始化 SPI1 去读 Flash，就和烧录器抢同一组总线
 * （SCK/MOSI/CS），烧录失败或校验错。约定：**上电时同时按住 左键(KEY1/PB12) + 右键(KEY3/PB14)**
 * 就当作"正在烧录"，main 据此跳过全部外设初始化并停在原地等断电。
 *
 * @note  只配置 PB12/PB14（GPIO 输入 + 上拉），**绝不碰 PA0..PA3**：SPI1 的 SCK/MOSI/MISO/CS
 *        保持复位后的高阻输入，不驱动总线（CS 也不能推挽拉高，会和烧录器的驱动打架）。
 *        消抖用忙等，不依赖 BFTM0/Delay（此时还没初始化）。
 * @retval 1=两键都按住（已消抖） 0=不是（正常启动）
 ************************************************************************************************************/
int Key_BootHoldDetect(void)
{
  CKCU_PeripClockConfig_TypeDef clk = {{ 0 }};
  clk.Bit.PB   = 1;
  clk.Bit.AFIO = 1;
  CKCU_PeripClockConfig(clk, ENABLE);

  /* 输入 + 输入功能使能 + 内部上拉（按下=低）；PB14 复位可能映射其它 AF，显式拉回 GPIO */
  GPIO_DirectionConfig(KEY_GPIO, KEY1_PIN | KEY3_PIN, GPIO_DIR_IN);
  GPIO_InputConfig(KEY_GPIO, KEY1_PIN | KEY3_PIN, ENABLE);
  GPIO_PullResistorConfig(KEY_GPIO, KEY1_AFIO | KEY3_AFIO, GPIO_PR_UP);
  AFIO_GPxConfig(GPIO_PB, KEY1_AFIO, AFIO_FUN_GPIO);
  AFIO_GPxConfig(GPIO_PB, KEY3_AFIO, AFIO_FUN_GPIO);

  /* 先立刻采一次：正常开机（没按住）在这里就返回，**不给正常启动加任何延时**
     （Voice_Init 要抢语音模块上电帧 0xEF，启动路径越短越好）。 */
  if (GPIO_ReadInBit(KEY_GPIO, KEY1_PIN) != RESET || GPIO_ReadInBit(KEY_GPIO, KEY3_PIN) != RESET)
    return 0;

  /* 两键都按住：约 30ms 忙等滤掉接触抖动，再确认一次（此时才付这段延时） */
  for (volatile u32 i = 0; i < 400000u; i++) { }
  return (GPIO_ReadInBit(KEY_GPIO, KEY1_PIN) == RESET) &&
         (GPIO_ReadInBit(KEY_GPIO, KEY3_PIN) == RESET);
}

/************************************************************************************************************
 * @brief  初始化 PB12/PB13/PB14：GPIO 输入 + EXTI 下降沿 + 硬件消抖 + 低功耗唤醒事件（KEY1）。
 * @retval None
 ************************************************************************************************************/
void Key_Init(void)
{
  /* 1. 时钟：PB 端口 + AFIO + EXTI（APB） */
  CKCU_PeripClockConfig_TypeDef clk = {{ 0 }};
  clk.Bit.PB   = 1;
  clk.Bit.AFIO = 1;
  clk.Bit.EXTI = 1;
  CKCU_PeripClockConfig(clk, ENABLE);

  /* 2. 逐键：GPIO 输入 + 输入功能使能（INER 不使能则 EXTI 检测不到电平变化）+ 内部上拉
        + 强制普通 GPIO（PB13/PB14 复位可能映射 HSE 主晶振/其它 AF，显式 AFIO_FUN_GPIO 保险）
        + EXTI 源选择 + 下降沿 + 硬件消抖 + 中断使能（三键同组 EXTI4_15，共用 NVIC）。 */
  for (u8 i = 0; i < KEY_COUNT; i++)
  {
    GPIO_DirectionConfig(KEY_GPIO, g_keys[i].pin, GPIO_DIR_IN);
    GPIO_InputConfig(KEY_GPIO, g_keys[i].pin, ENABLE);
    GPIO_PullResistorConfig(KEY_GPIO, g_keys[i].afio, GPIO_PR_UP);
    AFIO_GPxConfig(GPIO_PB, g_keys[i].afio, AFIO_FUN_GPIO);

    AFIO_EXTISourceConfig(g_keys[i].num, GPIO_PB);
    EXTI_InitTypeDef ei;
    ei.EXTI_Channel     = g_keys[i].exti;
    ei.EXTI_Debounce    = EXTI_DEBOUNCE_ENABLE;
    ei.EXTI_DebounceCnt = 40;            /* 硬件消抖计数 */
    ei.EXTI_IntType     = EXTI_NEGATIVE_EDGE;
    EXTI_Init(&ei);
    EXTI_IntConfig(g_keys[i].exti, ENABLE);
  }
  NVIC_EnableIRQ(EXTI4_15_IRQn);   /* EXTI12/13/14 -> EXTI4_15_IRQn */

  /* 3. 低功耗唤醒事件：全局使能 EXTI 唤醒事件，并打开 EVWUP 这一 NVIC 中断线。
        深度休眠1 下 WFI 正是靠 EVWUP 把“EXTI 事件”送达内核从而被唤醒。
        通道级低电平唤醒在每次进休眠前由 key_enter_deep_sleep1 使能（当前仅 KEY1/PB12）。 */
  EXTI_WakeupEventIntConfig(ENABLE);
  NVIC_EnableIRQ(EVWUP_IRQn);

  g_screen_off = 0;
  g_wait_release = 0;
  g_key_wake = 0;
  key_q_clear();
  key_scan_state_reset();   /* 软件消抖状态同步到当前实际电平（上电全松开） */
}

int Key_IsScreenOff(void)
{
  return (int)g_screen_off;
}

/************************************************************************************************************
 * @brief  EXTI4_15 中断入口（KEY1/KEY2/KEY3 下降沿，由中断文件 ht32l5xxxx_01_it.c 调用）。
 *         只置“按下待判定”标志 + 记按下时刻（短/长按判定在主循环 key_scan，中断内不碰队列/SPI）。
 *         KEY1 的深休眠低电平唤醒标志也在此顺带检查（EVWUP 路径另见 Key_ClearWakeup）。
 * @retval None
 ************************************************************************************************************/
void Key_HandleIrq(void)
{
  for (u8 i = 0; i < KEY_COUNT; i++)
  {
    if (EXTI_GetEdgeFlag(g_keys[i].exti) != RESET)
    {
      EXTI_ClearEdgeFlag(g_keys[i].exti);
      g_pend[i] = 1;
      g_press_ms[i] = (u32)bftm0_ct;
    }
  }
  if (EXTI_GetWakeupFlagStatus(KEY1_EXTI) != RESET)
  {
    EXTI_ClearWakeupFlag(KEY1_EXTI);
    g_key_wake = 1;
  }
}

/************************************************************************************************************
 * @brief  EVWUP（EXTI 事件唤醒 / WAKEUP 引脚）中断专用：清唤醒/边沿标志，并临时关闭 KEY1(PB12)
 *         低电平唤醒事件，防止按键按住期间电平持续触发、反复进入该中断。
 *         置 KEY1 唤醒标志 g_key_wake，主循环深休眠循环据此开屏。
 * @retval None
 ************************************************************************************************************/
void Key_ClearWakeup(void)
{
  EXTI_WakeupEventConfig(KEY1_EXTI, EXTI_WAKEUP_LOW_LEVEL, DISABLE);  /* 暂关，下次进休眠由 key_enter_deep_sleep1 重开 */
  EXTI_ClearWakeupFlag(KEY1_EXTI);
  EXTI_ClearEdgeFlag(KEY1_EXTI);
  /* 记录按键唤醒事件（EVWUP 只在 KEY1 低电平唤醒时触发，RTC 心跳唤醒不进来）：
     让主循环据此开屏，支持短按唤醒——否则短按松开后 PB12 已回高，
     深休眠循环读电平/标志会误判为“没按”而继续休眠，导致“能进中断但不亮屏”。 */
  g_key_wake = 1;
}

/************************************************************************************************************
 * @brief  关屏：关显示 + 灭背光 + 蓝牙断电，然后进入 DeepSleep1（WFI，RTC 走时）。
 *         由应用层 KEY2 长按事件触发调用（决策在 App，执行在本模块）。
 *         阻塞：先等触发键松开，再进深休眠，直到 KEY1(PB12) 按键唤醒开屏才返回。
 * @retval None
 ************************************************************************************************************/
void Key_ScreenOff(void)
{
  /* 关显示：DISPOFF 后等 ~10 帧（120ms），背光灭，再进睡眠。
     Delay 依赖 BFTM0，必须在下方 BFTM_IntConfig(DISABLE) 之前完成。 */
  SPI_LCD_Select();
  SPI_LCD_WriteCmd(ST7789_DISPOFF);   /* 关显示 */
  SPI_LCD_Deselect();
  Delay(120);

  GPIO_SetOutBits(SPI_LCD_BL_GPIO, SPI_LCD_BL_PIN);   /* 背光灭（高电平关） */

  /* ST7789 睡眠模式（0x10）：仅 DISPOFF 时驱动 IC 仍全功率（数 mA），
     SLPIN 后降到 μA 级（屏幕功耗大头）。睡眠中 RAM 内容保留，
     SLPOUT 唤醒后直接可显示，无需重新初始化。 */
  SPI_LCD_Select();
  SPI_LCD_WriteCmd(ST7789_SLPIN);
  SPI_LCD_Deselect();

  /* 关屏副作用回调：应用层做蓝牙断电等（P2-3 反转，原 Hd9803_SetPower(0)）。
     回调无条件调用——HD9803_ENABLE 等条件编译由应用层回调内部自行处理。 */
  if (g_off_cb) g_off_cb();

  /* Flash 深度掉电（0xB9）：待机 ~15μA -> 掉电 ~1μA */
  Flash_PowerDown();

  /* 禁用 BFTM0(每 1ms)避免频繁唤醒；保留/使能 RTC CSEC(每 0.5s)作兜底心跳。
     Deep Sleep 1 下 PB12 的 EXTI 低电平唤醒事件经 EVWUP 中断线可直接唤醒 WFI（即时响应）。
     CSEC 中断同时驱动 g_current_time 结构体走时，休眠期间时间持续准确。 */
  RTC_IntConfig(RTC_INT_CSEC, ENABLE);    /* 保留 CSEC 作兜底心跳 */
  BFTM_IntConfig(HT_BFTM0, DISABLE);

  /* === 关键修复：等按键松开后再允许休眠 ===
     唤醒是“电平触发（LOW_LEVEL）”，而本函数由“KEY2(PB13) 长按”触发，执行到这里时
     PB13 往往仍处低电平。若此时直接使能唤醒，条件立即成立 ->
     WFI 瞬间返回 -> CPU 卡在运行态（~4mA），根本没真正休眠。
     因此必须先把 KEY2 等松开（电平回高），再使能唤醒、进 WFI。 */
  while (GPIO_ReadInBit(KEY_GPIO, KEY2_PIN) == RESET)
    ;
  /* 清掉等待期间可能残留的边沿/唤醒/待判定标志，保证干净进入 Deep-Sleep */
  EXTI_ClearEdgeFlag(KEY1_EXTI);
  EXTI_ClearWakeupFlag(KEY1_EXTI);
  g_key_wake = 0;
  key_scan_state_reset();   /* 消抖状态复位到当前电平（KEY2 已松开），防休眠后电平状态错乱 */
  key_q_clear();

  /* 关闭与唤醒无关的外设时钟，进一步压低 Deep-Sleep1 电流 */
  key_periph_off();

  g_screen_off = 1;
  g_key_deep_sleep_flag = 1;                     /* 进入深度休眠：置测试标志（唤醒开屏时清零） */

  /* === 循环休眠直到按键唤醒：WFI 可能被 RTC CSEC 兜底(每0.5s)或按键(EVWUP)唤醒。
     必须在此循环直到【按键】唤醒才开屏返回——若 WFI 一被唤醒就返回，
     主循环会带着已关闭的 PA/SPI0/BFTM0 时钟继续执行 App_Render(读 Flash)
     /YX8320B(延时) → 访问无时钟外设寄存器 → HardFault。
     循环内用 g_key_wake/g_pend[0] 判定：仅 KEY1 边沿或低电平唤醒置位，
     RTC CSEC 兜底唤醒不置位 → 自动继续睡。 */
  key_sleep_until_key_pressed();
  key_screen_on();             /* 按键唤醒：恢复外设时钟 + 开屏 + 重绘 */
}

/* 循环进入 DeepSleep1，直到 KEY1(PB12) 唤醒才返回。
   关键：WFI 可能被 RTC CSEC 兜底唤醒(每0.5s)，此时 g_key_wake/g_pend[0] 未置位，
   必须继续睡；只有 KEY1 边沿(EXTI)或低电平唤醒(EVWUP)置位才退出。
   避免假唤醒返回主循环后带着已关闭的外设时钟访问寄存器 → HardFault。 */
static void key_sleep_until_key_pressed(void)
{
  g_key_wake = 0;
  g_pend[0]  = 0;
  while (!(g_key_wake || g_pend[0]))
  {
    /* 每次进休眠前清残留的唤醒/边沿标志，避免按键电平残留导致 WFI 立即返回空转 */
    EXTI_ClearEdgeFlag(KEY1_EXTI);
    EXTI_ClearWakeupFlag(KEY1_EXTI);
    key_enter_deep_sleep1();   /* 阻塞直到唤醒：HSI/PLL 自动停，仅 LSI/RTC 走时，电流 ~1.15μA */
  }
  g_key_wake = 0;
  g_pend[0]  = 0;
}

/************************************************************************************************************
 * @brief  开屏：亮背光 + 开显示 + 强制重绘 + 蓝牙上电 + 禁用唤醒事件。
 * @retval None
 ************************************************************************************************************/
static void key_screen_on(void)
{
  g_screen_off = 0;
  g_key_deep_sleep_flag = 0;                     /* 退出深度休眠：清测试标志 */

  /* 恢复休眠期间关闭的外设时钟与 Flash 缓存，必须在下方 SPI/Flash/BFTM 访问之前 */
  key_periph_on();

  /* 恢复休眠期间被禁用的中断：RTC 秒中断、BFTM0。
     注意：BFTM0 必须【完整重初始化】而非只开中断——
     深度休眠唤醒后 BFTM0 外设被复位（CR=0、CMP=0），仅 BFTM_IntConfig(ENABLE)
     定时器不计数，bftm0_ct 不再递增，所有基于它的延时(YX_DelayMs/Delay)会永久卡死。
     BFTM0_Configuration() 会重设 时钟/NVIC/中断/CMP/计数器/使能 全套。 */
  RTC_IntConfig(RTC_INT_CSEC, ENABLE);
  BFTM0_Configuration();

  /* 时间源是结构体 g_current_time（RTC CSEC 中断每 0.5s 推进），休眠期间该中断一直
     ENABLE，结构体持续走时，唤醒后时间天然准确——【不要】用硬件 CNT 换算覆盖它：
     唤醒后立即读 CNT 违反库时序要求（需 1/CK_RTC 延时），会读到旧值并把正确时间覆盖错。 */

  /* 开屏后禁用唤醒事件，防止低电平残留反复触发再次关屏 */
  EXTI_WakeupEventConfig(KEY1_EXTI, EXTI_WAKEUP_LOW_LEVEL, DISABLE);

  GPIO_ClearOutBits(SPI_LCD_BL_GPIO, SPI_LCD_BL_PIN);  /* 背光亮（低电平导通） */

  /* Flash 退出深度掉电（0xAB），恢复待机后才能读字库/背景 */
  Flash_ReleasePowerDown();

  /* ST7789 退出睡眠（SLPOUT）：需等待 120ms 再发后续命令。BFTM0 中断已恢复，Delay 可用。 */
  SPI_LCD_Select();
  SPI_LCD_WriteCmd(ST7789_SLPOUT);
  SPI_LCD_Deselect();
  Delay(120);

  SPI_LCD_Select();
  SPI_LCD_WriteCmd(ST7789_DISPON);    /* 开显示 */
  SPI_LCD_Deselect();

  /* 开屏副作用回调：应用层做强制重绘 + 蓝牙上电（P2-3 反转，原 Calendar_SetStaticNext + Hd9803_SetPower(1)）。
     回调无条件调用——HD9803_ENABLE 等条件编译由应用层回调内部自行处理。 */
  if (g_on_cb) g_on_cb();

  g_wait_release = 1;                 /* 等按键松开，防低电平残留/反复触发再次关屏 */
}

/* ---- 三键统一长/短按判定（Key_Process 每轮调用） ----
   软件消抖（2026-09-05）：对每个键读实际电平，电平变化后需连续稳定 KEY_DEBOUNCE_MS
   （接触抖动窗口）才认定一次有效的按下/松开，杜绝"一次物理按压因抖动误发多个短按"：
     稳定按下(低) 且 ISR 已置 pend：按住 ≥KEY_LONG_MS -> 长按入队（判定后 pend 已清，松开不再产生短按）
     稳定松开(高) 且 pend 仍在   ：短按入队（pend 在长按判定/消抖复位时已清则不重复）
   不掺任何业务语义：事件怎么用由应用层 Screen_HandleKey 按状态决定。 */
static void key_scan(void)
{
  for (u8 i = 0; i < KEY_COUNT; i++)
  {
    u32 now = (u32)bftm0_ct;
    u8  lvl = (GPIO_ReadInBit(KEY_GPIO, g_keys[i].pin) == RESET) ? 1u : 0u;  /* 1=按下(低)，0=松开(高) */

    if (lvl != g_scan_lvl[i])              /* 电平变化：重启稳定计时，窗口内不再判定（消抖） */
    {
      g_scan_lvl[i]  = lvl;
      g_stable_ms[i] = now;
      continue;
    }
    if ((u32)(now - g_stable_ms[i]) < KEY_DEBOUNCE_MS)
      continue;                            /* 尚未稳定满窗口：抖动中，继续观察 */

    if (lvl)                               /* 稳定按下（低电平 ≥窗口） */
    {
      g_press_conf[i] = 1;                 /* 确认本次按压有效（防松开抖动重复入队） */
      if (g_pend[i] && (u32)(now - g_press_ms[i]) >= KEY_LONG_MS)   /* 按住超时 = 长按 */
      {
        g_pend[i] = 0;
        g_press_conf[i] = 0;
        key_q_push((key_id_t)i, KEY_EV_LONG);
        g_rep_armed[i]   = 1;              /* 长按后进入连发态（2026-09-05） */
        g_rep_next_ms[i] = (u32)(now + KEY_REPEAT_MS);
      }
      else if (g_rep_armed[i] && (u32)(now - g_rep_next_ms[i]) < 0x80000000u)   /* 连发到点 */
      {
        key_q_push((key_id_t)i, KEY_EV_REPEAT);   /* 按住持续连发（快加快减用） */
        g_rep_next_ms[i] = (u32)(now + KEY_REPEAT_MS);
      }
    }
    else                                   /* 稳定松开（高电平 ≥窗口） = 短按 */
    {
      if (g_pend[i] && g_press_conf[i])    /* pend 与按压确认都在：真实短按（长按判定/复位已清则不重复） */
      {
        g_pend[i] = 0;
        g_press_conf[i] = 0;
        key_q_push((key_id_t)i, KEY_EV_SHORT);
      }
      /* EXTI can record a fresh press after the GPIO sample above. Keep the
         new edge until the confirmation window has elapsed. */
      else if (g_pend[i] && (s32)(now - g_press_ms[i]) >= (s32)KEY_DEBOUNCE_MS)
      {
        g_pend[i] = 0;
        g_press_conf[i] = 0;
      }
      g_rep_armed[i] = 0;                  /* 松开退出连发态（2026-09-05） */
    }
  }
}

/************************************************************************************************************
 * @brief  主循环调用：三键长/短按判定入队（应用层 Key_GetEvent 消费）。
 *         附带处理：唤醒开屏后等待按键松开（防电平残留反复触发）。
 * @retval None
 ************************************************************************************************************/
void Key_Process(void)
{
  /* 开屏后等待唤醒键松开：若按键一直按着（低电平），唤醒标志会反复置位 -> 会再次关屏。
     等待期间丢弃一切按键事件（含唤醒键残留的 pend/队列），并把消抖状态同步到当前电平，
     防唤醒键松开被 key_scan 误判为一次真实短按。 */
  if (g_wait_release)
  {
    if (GPIO_ReadInBit(KEY_GPIO, KEY1_PIN) != RESET)   /* 已松开（高电平） */
      g_wait_release = 0;
    key_q_clear();
    key_scan_state_reset();
    return;
  }

  /* 关屏态兜底：正常关屏流程由 Key_ScreenOff 阻塞完成（本分支不会走到）；
     防御性保留——若 g_screen_off 被置位且主循环继续，则循环深休眠直到 KEY1 唤醒再开屏。 */
  if (g_screen_off)
  {
    key_sleep_until_key_pressed();
    key_screen_on();
    return;
  }

  /* 正常态：三键统一判定，事件入队供应用层消费 */
  key_scan();
}

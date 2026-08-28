/************************************************************************************************************
 * @file    key.c
 * @brief   按键功能模块（原 power.c）：PB12 按键（EXTI12 下降沿 + 硬件消抖）翻转屏幕开关，
 *          并负责关屏低功耗（Deep Sleep Mode 1）。
 *
 * 本模块集中管理“按键 -> 关屏/开屏 -> 深度休眠”全链路，main 只需：
 *   Key_Init() 一次；Key_Process() 每轮调用；Key_IsScreenOff() 供显示层判断是否跳过刷新。
 * 中断文件 ht32l5xxxx_01_it.c 调用 Key_HandleIrq() / Key_ClearWakeup() 转发按键/唤醒事件。
 ************************************************************************************************************/
#include "ht32.h"
#include "ht32l5xxxx_pwrcu.h"  /* PWRCU_DeepSleep1 / PWRCU_SLEEP_ENTRY_WFI */
#include "key.h"
#include "spi_lcd.h"     /* 背光控制脚 PB4 */
#include "Display.h"     /* Display_Off/Display_On（关屏/唤醒开屏，同步显示点亮状态） */
#include "st7789.h"      /* SLPIN / SLPOUT 命令 */
#include "calendar.h"    /* Calendar_SetStaticNext */
#include "hd9803.h"      /* Hd9803_SetPower */
#include "flash_read.h"  /* Flash_PowerDown / Flash_ReleasePowerDown */
#include "time_module.h" /* RTC_IntConfig（休眠/唤醒保持 CSEC 走时） */
#include "BFTM0.h"       /* BFTM0_Configuration 唤醒后完整恢复 1ms 节拍 */

/* ---- 按键：PB12（QFN32 脚15，内部上拉 + 接地按下） ---- */
#define KEY_GPIO    HT_GPIOB
#define KEY_PIN     GPIO_PIN_12      /* 位图掩码 */
#define KEY_AFIO    AFIO_PIN_12      /* AFIO 复用位 */
#define KEY_NUM     GPIO_PIN_NUM_12  /* EXTI 源引脚号 */
#define KEY_EXTI    EXTI_CHANNEL_12  /* EXTI 通道 */

/* 进入深度休眠1：直接调用库函数 PWRCU_DeepSleep1。
   库内部已完整封装 VDDREN 保持、SLEEPDEEP 置位、LDO off 与 WFI 唤醒流程。
   唤醒源为“事件模式的 EXTI / RTC / LVD / WAKEUPn”：PB12 低电平唤醒事件
   经 EVWUP 中断线唤醒 WFI（NVIC_EnableIRQ(EVWUP_IRQn) 已在 Key_Init 使能）。
   每次进休眠前重开 PB12 低电平唤醒事件（Key_ClearWakeup 会暂关，防按键按住时电平粘滞）。 */
static void key_enter_deep_sleep1(void)
{
  EXTI_WakeupEventConfig(KEY_EXTI, EXTI_WAKEUP_LOW_LEVEL, ENABLE);  /* 进休眠前重开 PB12 低电平唤醒事件 */
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

static volatile u8 g_key_pressed = 0;  /* 按键事件标志（ISR 置位，主循环消费） */
static u8 g_screen_off = 0;            /* 1=关屏低功耗 */
static u8 g_wait_release = 0;          /* 1=等待按键松开（防唤醒事件电平触发反复开关屏） */
volatile u8 g_key_deep_sleep_flag = 0; /* 深度休眠测试标志：进休眠置1，开屏退出清0（main 打印用） */

static void key_screen_on(void);            /* 前向声明：key_screen_off 的休眠循环里在按键唤醒时调用 */
static void key_sleep_until_key_pressed(void); /* 前向声明：循环休眠直到按键唤醒 */

/************************************************************************************************************
 * @brief  初始化 PB12：GPIO 输入 + EXTI12 下降沿 + 低功耗唤醒事件。
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

  /* 2. PB12 输入 + 输入功能使能（INER 不使能则 EXTI 检测不到电平变化）+ 内部上拉 */
  GPIO_DirectionConfig(KEY_GPIO, KEY_PIN, GPIO_DIR_IN);
  GPIO_InputConfig(KEY_GPIO, KEY_PIN, ENABLE);
  GPIO_PullResistorConfig(KEY_GPIO, KEY_AFIO, GPIO_PR_UP);
  /* 确保 PB12 为普通 GPIO（非 RTCOUT/WAKEUP 复用）；复位默认 AF0=GPIO，显式设置更保险 */
  AFIO_GPxConfig(GPIO_PB, KEY_AFIO, AFIO_FUN_GPIO);

  /* 3. EXTI12 下降沿 + 硬件消抖（按键按下=低=下降沿） */
  AFIO_EXTISourceConfig(KEY_NUM, GPIO_PB);
  EXTI_InitTypeDef ei;
  ei.EXTI_Channel     = KEY_EXTI;
  ei.EXTI_Debounce    = EXTI_DEBOUNCE_ENABLE;
  ei.EXTI_DebounceCnt = 40;            /* 硬件消抖计数 */
  ei.EXTI_IntType     = EXTI_NEGATIVE_EDGE;
  EXTI_Init(&ei);
  EXTI_IntConfig(KEY_EXTI, ENABLE);
  NVIC_EnableIRQ(EXTI_GetIRQn(KEY_EXTI));   /* EXTI12 -> EXTI4_15_IRQn */

  /* 4. 低功耗唤醒事件：全局使能 EXTI 唤醒事件，并打开 EVWUP 这一 NVIC 中断线。
       深度休眠1 下 WFI 正是靠 EVWUP 把“EXTI 事件”送达内核从而被唤醒。
       通道级低电平唤醒在每次进休眠前由 key_enter_deep_sleep1 使能。 */
  EXTI_WakeupEventIntConfig(ENABLE);
  NVIC_EnableIRQ(EVWUP_IRQn);

  g_screen_off = 0;
  g_key_pressed = 0;
  g_wait_release = 0;
}

int Key_IsScreenOff(void)
{
  return (int)g_screen_off;
}

/************************************************************************************************************
 * @brief  EXTI4_15 中断入口（EXTI12 按键/唤醒，由中断文件 ht32l5xxxx_01_it.c 调用）。
 *         合并检查边沿标志（正常态按键）和唤醒标志（低功耗唤醒），置按键标志；
 *         具体翻转在主循环 Key_Process 处理（中断内不碰 SPI）。
 * @retval None
 ************************************************************************************************************/
void Key_HandleIrq(void)
{
  u8 ev = 0;
  if (EXTI_GetEdgeFlag(KEY_EXTI) != RESET)
  {
    EXTI_ClearEdgeFlag(KEY_EXTI);
    ev = 1;
  }
  if (EXTI_GetWakeupFlagStatus(KEY_EXTI) != RESET)
  {
    EXTI_ClearWakeupFlag(KEY_EXTI);
    ev = 1;
  }
  if (ev) g_key_pressed = 1;
}

/************************************************************************************************************
 * @brief  EVWUP（EXTI 事件唤醒 / WAKEUP 引脚）中断专用：清唤醒/边沿标志，并临时关闭 PB12
 *         低电平唤醒事件，防止按键按住期间电平持续触发、反复进入该中断。
 *         真正的开/关屏由主循环 Key_Process 读 PB12 电平决定，避免在中断内操作 SPI/LCD。
 * @retval None
 ************************************************************************************************************/
void Key_ClearWakeup(void)
{
  EXTI_WakeupEventConfig(KEY_EXTI, EXTI_WAKEUP_LOW_LEVEL, DISABLE);  /* 暂关，下次进休眠由 key_enter_deep_sleep1 重开 */
  EXTI_ClearWakeupFlag(KEY_EXTI);
  EXTI_ClearEdgeFlag(KEY_EXTI);
  /* 记录按键唤醒事件（EVWUP 只在按键低电平唤醒时触发，RTC 心跳唤醒不进来）：
     让主循环据此开屏，支持短按唤醒——否则短按松开后 PB12 已回高，
     Key_Process 读电平会误判为“没按”而继续休眠，导致“能进中断但不亮屏”。 */
  g_key_pressed = 1;
}

/************************************************************************************************************
 * @brief  关屏：关显示 + 灭背光 + 蓝牙断电，然后进入 DeepSleep1（WFI，RTC 走时）。
 * @retval None
 ************************************************************************************************************/
static void key_screen_off(void)
{
  /* 关显示：DISPOFF 后等 ~10 帧（120ms），背光灭，再进睡眠。
     Delay 依赖 BFTM0，必须在下方 BFTM_IntConfig(DISABLE) 之前完成。
     Display_Off() 同时清显示点亮状态（整屏刷新保护据此判断）。 */
  Display_Off();
  Delay(120);

  GPIO_SetOutBits(SPI_LCD_BL_GPIO, SPI_LCD_BL_PIN);   /* 背光灭（高电平关） */

  /* ST7789 睡眠模式（0x10）：仅 DISPOFF 时驱动 IC 仍全功率（数 mA），
     SLPIN 后降到 μA 级（屏幕功耗大头）。睡眠中 RAM 内容保留，
     SLPOUT 唤醒后直接可显示，无需重新初始化。 */
  SPI_LCD_Select();
  SPI_LCD_WriteCmd(ST7789_SLPIN);
  SPI_LCD_Deselect();

#if HD9803_ENABLE
  Hd9803_SetPower(0);                 /* 蓝牙断电（PA15 拉高） */
#endif

  /* Flash 深度掉电（0xB9）：待机 ~15μA -> 掉电 ~1μA */
  Flash_PowerDown();

  /* 禁用 BFTM0(每 1ms)避免频繁唤醒；保留/使能 RTC CSEC(每 0.5s)作兜底心跳。
     Deep Sleep 1 下 PB12 的 EXTI 低电平唤醒事件经 EVWUP 中断线可直接唤醒 WFI（即时响应）。
     CSEC 中断同时驱动 g_current_time 结构体走时，休眠期间时间持续准确。 */
  RTC_IntConfig(RTC_INT_CSEC, ENABLE);    /* 保留 CSEC 作兜底心跳 */
  BFTM_IntConfig(HT_BFTM0, DISABLE);

  /* === 关键修复：等按键松开后再允许休眠 ===
     唤醒是“电平触发（LOW_LEVEL）”，而本函数由“按住按键”触发，执行到这里时
     PB12 往往仍处低电平。若此时直接使能 LOW_LEVEL 唤醒，条件立即成立 ->
     WFI 瞬间返回 -> CPU 卡在运行态（~4mA），根本没真正休眠。
     因此必须先把按键等松开（电平回高），再使能唤醒、进 WFI。 */
  while (GPIO_ReadInBit(KEY_GPIO, KEY_PIN) == RESET)
    ;
  /* 清掉等待期间可能残留的边沿/唤醒标志，保证干净进入 Deep-Sleep */
  EXTI_ClearEdgeFlag(KEY_EXTI);
  EXTI_ClearWakeupFlag(KEY_EXTI);

  /* 关闭与唤醒无关的外设时钟，进一步压低 Deep-Sleep1 电流 */
  key_periph_off();

  g_screen_off = 1;
  g_key_deep_sleep_flag = 1;                     /* 进入深度休眠：置测试标志（唤醒开屏时清零） */

  /* === 循环休眠直到按键唤醒：WFI 可能被 RTC CSEC 兜底(每0.5s)或按键(EVWUP)唤醒。
     必须在此循环直到【按键】唤醒才开屏返回——若 WFI 一被唤醒就返回，
     主循环会带着已关闭的 PA/SPI0/BFTM0 时钟继续执行 Display_Update(读 Flash)
     /YX8320B(延时) → 访问无时钟外设寄存器 → HardFault。
     循环内用 g_key_pressed 事件判定：仅 EVWUP(按键低电平唤醒)置位，
     RTC CSEC 兜底唤醒不置位 → 自动继续睡。 */
  key_sleep_until_key_pressed();
  key_screen_on();             /* 按键唤醒：恢复外设时钟 + 开屏 + 重绘 */
}

/* 循环进入 DeepSleep1，直到按键(EVWUP)唤醒才返回。
   关键：WFI 可能被 RTC CSEC 兜底唤醒(每0.5s)，此时 g_key_pressed 未置位，
   必须继续睡；只有 EVWUP(按键低电平唤醒)置位 g_key_pressed 才退出。
   避免假唤醒返回主循环后带着已关闭的外设时钟访问寄存器 → HardFault。 */
static void key_sleep_until_key_pressed(void)
{
  g_key_pressed = 0;
  while (g_key_pressed == 0)
  {
    /* 每次进休眠前清残留的唤醒/边沿标志，避免按键电平残留导致 WFI 立即返回空转 */
    EXTI_ClearEdgeFlag(KEY_EXTI);
    EXTI_ClearWakeupFlag(KEY_EXTI);
    key_enter_deep_sleep1();   /* 阻塞直到唤醒：HSI/PLL 自动停，仅 LSI/RTC 走时，电流 ~1.15μA */
  }
  g_key_pressed = 0;
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
  EXTI_WakeupEventConfig(KEY_EXTI, EXTI_WAKEUP_LOW_LEVEL, DISABLE);

  GPIO_ClearOutBits(SPI_LCD_BL_GPIO, SPI_LCD_BL_PIN);  /* 背光亮（低电平导通） */

  /* Flash 退出深度掉电（0xAB），恢复待机后才能读字库/背景 */
  Flash_ReleasePowerDown();

  /* ST7789 退出睡眠（SLPOUT）：需等待 120ms 再发后续命令。BFTM0 中断已恢复，Delay 可用。 */
  SPI_LCD_Select();
  SPI_LCD_WriteCmd(ST7789_SLPOUT);
  SPI_LCD_Deselect();
  Delay(120);

  /* 开显示：Display_On() 同时置显示点亮状态（整屏刷新保护据此判断）。 */
  Display_On();

  Calendar_SetStaticNext();           /* 下一整秒强制全量重绘（不翻动画） */

#if HD9803_ENABLE
  Hd9803_SetPower(1);                 /* 蓝牙重新上电 */
#endif

  g_wait_release = 1;                 /* 等按键松开，防低电平残留/反复触发再次关屏 */
}

/************************************************************************************************************
 * @brief  主循环调用：检测按键事件，翻转 关屏/开屏。
 *         开屏后等待按键松开（唤醒事件是电平触发，若一直按着会反复触发）。
 * @retval None
 ************************************************************************************************************/
void Key_Process(void)
{
  /* 开屏后等待按键松开：若按键一直按着（低电平），唤醒标志会反复置位 -> 会再次关屏 */
  if (g_wait_release)
  {
    if (GPIO_ReadInBit(KEY_GPIO, KEY_PIN) != RESET)   /* 已松开（高电平） */
      g_wait_release = 0;
    g_key_pressed = 0;                               /* 丢弃等待期间的任何按键 */
    return;
  }

  if (g_screen_off)
  {
    /* 从 Deep-Sleep1 唤醒：PB12 按下经 EXTI 低电平事件 + EVWUP 直接唤醒 WFI（即时）；
       若无 EXTI 事件则依赖 RTC CSEC 兜底(每 0.5s)唤醒。
       这里与 key_screen_off 内一致地【循环休眠直到按键唤醒】再开屏——
       绝不能单次 WFI 后返回主循环（此时外设时钟仍是关的，主循环访问会 HardFault）。 */
    key_sleep_until_key_pressed();
    g_key_pressed = 0;
    key_screen_on();                               /* 内部已设 g_wait_release=1 */
    return;
  }

  if (!g_key_pressed) return;
  g_key_pressed = 0;

  key_screen_off();
}

/************************************************************************************************************
 * @file    yx8320b.c
 * @brief   YX8320B 数字温湿度传感器驱动（软件模拟 I2C，标准模式 100kHz）
 * @note    参考《YX8320B Datasheet Ver1.0 20241205》
 *          - 从机地址：7bit 0x44（写 0x88 / 读 0x89），兼容 SMBus/I2C，最高 1MHz
 *          - 单次测量命令（时钟拉伸关闭）：0x240B，测量时长 tMEAS <= 60ms
 *          - 读回 6 字节：温度MSB/LSB + CRC + 湿度MSB/LSB + CRC
 *          - 换算：RH(%RH) = 100 * raw / (2^16-1)
 *                   T(°C)   = -45 + 175 * raw / (2^16-1)
 *          - CRC-8：多项式 0x31(x8+x5+x4+1)、初值 0xFF、输入/输出不取反（例 CRC(0xABCD)=0x6F）
 *          引脚（软件 I2C，仅两根通信脚，需外接 4.7k~10k 上拉到 VCC）：
 *            PB0 = SCL，PB1 = SDA
 ************************************************************************************************************/
#include "yx8320b.h"
#include "BFTM0.h"          /* bftm0_ct 1ms 节拍，用于 ms 级延时 */

/* ==================== 引脚定义 ==================== */
#define YX_SCL_GPIO   HT_GPIOB
#define YX_SCL_PIN    GPIO_PIN_0    /* PB0 = SCL */
#define YX_SDA_GPIO   HT_GPIOB
#define YX_SDA_PIN    GPIO_PIN_1    /* PB1 = SDA */

/* ==================== SCL 操作（推挽输出） ==================== */
#define YX_SCL_HIGH()  GPIO_SetOutBits(YX_SCL_GPIO, YX_SCL_PIN)
#define YX_SCL_LOW()   GPIO_ClearOutBits(YX_SCL_GPIO, YX_SCL_PIN)

/* ==================== SDA 操作（双向：输出/输入切换） ==================== */
#define YX_SDA_OUT()   GPIO_DirectionConfig(YX_SDA_GPIO, YX_SDA_PIN, GPIO_DIR_OUT)
#define YX_SDA_IN()    GPIO_DirectionConfig(YX_SDA_GPIO, YX_SDA_PIN, GPIO_DIR_IN)
#define YX_SDA_HIGH()  GPIO_SetOutBits(YX_SDA_GPIO, YX_SDA_PIN)
#define YX_SDA_LOW()   GPIO_ClearOutBits(YX_SDA_GPIO, YX_SDA_PIN)
#define YX_SDA_READ()  GPIO_ReadInBit(YX_SDA_GPIO, YX_SDA_PIN)

/* ==================== 地址/命令 ==================== */
/* 用户实测：YX8320B 7 位地址 = 0x45（写 0x8A / 读 0x8B），经 8 位机验证可用。
   手册 Ordering 表亦写 0x8A（仅 I2C 时序图 Fig4/5 画 0x88/0x89）。
   故 0x8A 优先，0x88 留作备选探测。 */
#define YX_ADDR_W_LIST  { 0x8Au, 0x88u }
#define YX_ADDR_R(aw)   ((aw) | 0x01u)   /* 读地址 = 写地址 | 1 */
#define YX_CMD_MSB     0x24u        /* 单次测量命令 MSB（时钟拉伸关闭） */
#define YX_CMD_LSB     0x0Bu        /* 单次测量命令 LSB */
#define YX_T_MEAS_MS   100u         /* 等待测量完成（手册 <=60ms，留足余量 100ms） */

/* ==================== 微秒级延时（基于 SystemCoreClock 精确校准） ==================== */
/* 48MHz 下 1us = 48 周期；Cortex-M0+ 空循环约 3 周期/轮（含 volatile 读写） */
static void YX_DelayUs(volatile u32 us)
{
  volatile u32 n = us * (SystemCoreClock / 1000000u) / 3u + 1u;
  while (n--);
}

/* ==================== 毫秒级延时（基于 BFTM0 1ms 节拍） ==================== */
static void YX_DelayMs(u32 ms)
{
  vu32 target = bftm0_ct + ms;
  while ((vu32)bftm0_ct < target);
}

/* ==================== 软件 I2C 底层（标准 100kHz：半周期 5us） ==================== */
#define YX_HALF_US   5u

static void YX_I2C_Start(void)
{
  YX_SDA_OUT();
  YX_SDA_HIGH();
  YX_SCL_HIGH();
  YX_DelayUs(YX_HALF_US);
  YX_SDA_LOW();                 /* SCL 高时 SDA 拉低 = START */
  YX_DelayUs(YX_HALF_US);
  YX_SCL_LOW();
  YX_DelayUs(YX_HALF_US);
}

static void YX_I2C_Stop(void)
{
  YX_SDA_OUT();
  YX_SDA_LOW();
  YX_SCL_HIGH();
  YX_DelayUs(YX_HALF_US);
  YX_SDA_HIGH();                /* SCL 高时 SDA 拉高 = STOP */
  YX_DelayUs(YX_HALF_US);
}

/* 发送一字节，返回从机 ACK（0=有 ACK，1=无 ACK/NACK） */
static u8 YX_I2C_WriteByte(u8 data)
{
  u8 ack;
  YX_SDA_OUT();
  for (u8 i = 0; i < 8; i++)
  {
    if (data & 0x80u) YX_SDA_HIGH();
    else              YX_SDA_LOW();
    data <<= 1;
    YX_DelayUs(YX_HALF_US / 2);
    YX_SCL_HIGH();
    YX_DelayUs(YX_HALF_US);
    YX_SCL_LOW();
    YX_DelayUs(YX_HALF_US / 2);
  }
  /* 读 ACK：释放 SDA，时钟第 9 个上升沿采样 */
  YX_SDA_IN();
  YX_DelayUs(YX_HALF_US / 2);
  YX_SCL_HIGH();
  YX_DelayUs(YX_HALF_US);
  ack = (u8)YX_SDA_READ();      /* 低=ACK，高=NACK */
  YX_SCL_LOW();
  YX_DelayUs(YX_HALF_US / 2);
  YX_SDA_OUT();
  return ack;
}

/* 接收一字节；ack=1 主发 ACK，ack=0 主发 NACK（最后一字节） */
static u8 YX_I2C_ReadByte(u8 ack)
{
  u8 data = 0;
  YX_SDA_IN();
  for (u8 i = 0; i < 8; i++)
  {
    data <<= 1;
    YX_SCL_HIGH();
    YX_DelayUs(YX_HALF_US);
    if (YX_SDA_READ()) data |= 0x01u;
    YX_SCL_LOW();
    YX_DelayUs(YX_HALF_US);
  }
  /* 应答位 */
  YX_SDA_OUT();
  if (ack) YX_SDA_LOW();
  else     YX_SDA_HIGH();
  YX_DelayUs(YX_HALF_US / 2);
  YX_SCL_HIGH();
  YX_DelayUs(YX_HALF_US);
  YX_SCL_LOW();
  YX_DelayUs(YX_HALF_US / 2);
  YX_SDA_IN();
  return data;
}

/* 探测指定写地址是否有从机 ACK（1=有应答，0=无） */
static u8 YX_ProbeAck(u8 addr_w)
{
  u8 ok;
  YX_I2C_Start();
  ok = (YX_I2C_WriteByte(addr_w) == 0);
  YX_I2C_Stop();
  YX_DelayMs(1);
  return ok;
}

/* ==================== CRC-8（多项式 0x31，初值 0xFF，非反射） ==================== */
static u8 YX_CRC8(const u8 *data, u8 len)
{
  u8 crc = 0xFFu;
  for (u8 i = 0; i < len; i++)
  {
    crc ^= data[i];
    for (u8 j = 0; j < 8; j++)
      crc = (crc & 0x80u) ? (u8)((crc << 1) ^ 0x31u) : (u8)(crc << 1);
  }
  return crc;
}

/* ==================== 初始化 ==================== */
/**
 * @brief  初始化软件 I2C 引脚（PB0=SCL, PB1=SDA）
 * @note   使能 GPIOB 时钟；SCL 推挽输出；SDA 默认释放（输入+上拉），待用时切换方向。
 *         上电后传感器需 <=3ms 准备，此处延时 10ms 保证就绪。
 *         初始化末尾对 0x88/0x8A 两个候选地址做 ACK 探测，可通过
 *         YX8320B_GetAddr() 查询实际应答地址（默认 0x88，未应答返回 0）。
 * @retval 无
 */
static u8 g_yx_addr = 0;   /* 0=0x8A 应答，1=0x88 应答，2=均无应答 */

/* 最近一次有效温湿度缓存（读数失败时保留上次值，界面始终有数可显示） */
static int g_temp_cache = 260;   /* 默认 26.0°C（传感器未接/读失败时） */
static int g_humi_cache = 600;   /* 默认 60.0%RH */
static u8  g_first_ok   = 0;     /* 是否已成功读到过一次（用于首条成功日志） */

void YX8320B_Init(void)
{
  CKCU_PeripClockConfig_TypeDef CKCUClock = {{0}};
  CKCUClock.Bit.PB = 1;                     /* 使能 GPIOB 时钟 */
  CKCU_PeripClockConfig(CKCUClock, ENABLE);

  /* SCL：推挽输出，初始高 */
  GPIO_DirectionConfig(YX_SCL_GPIO, YX_SCL_PIN, GPIO_DIR_OUT);
  GPIO_PullResistorConfig(YX_SCL_GPIO, YX_SCL_PIN, GPIO_PR_UP);
  YX_SCL_HIGH();

  /* SDA：双向（软件 I2C）。
     注意：HT32 读 GPIO 输入需先 GPIO_InputConfig() 使能数字输入(INER)，
     否则 GPIO_ReadInBit() 读 DINR 恒为 0——这正是之前读到全 0 的根因。
     这里把 SDA 配成输入+输入使能+上拉；输出时由 YX_SDA_OUT() 切方向。 */
  GPIO_DirectionConfig(YX_SDA_GPIO, YX_SDA_PIN, GPIO_DIR_IN);
  GPIO_InputConfig(YX_SDA_GPIO, YX_SDA_PIN, ENABLE);   /* 使能数字输入(INER) */
  GPIO_PullResistorConfig(YX_SDA_GPIO, YX_SDA_PIN, GPIO_PR_UP);

  YX_DelayMs(10);                           /* 等待传感器上电就绪（<=3ms，留余量） */

  /* ACK 探测：0x8A(用户验证) 优先，0x88 备选 */
  g_yx_addr = 2;
  if (YX_ProbeAck(0x8Au))       g_yx_addr = 0;
  else if (YX_ProbeAck(0x88u))  g_yx_addr = 1;
}

/* 返回确认的地址索引：0=0x8A 应答，1=0x88 应答，2=均无应答（硬件问题） */
u8 YX8320B_GetAddr(void)
{
  return g_yx_addr;
}

/* ==================== 读取温湿度 ==================== */
/**
 * @brief  单次测量并读取温度与相对湿度
 * @note   流程（时钟拉伸关闭）：
 *         1. Start + 写地址 0x88 + 命令 0x24 0x0B + Stop
 *         2. 等待测量完成（>= tMEAS = 60ms）
 *         3. Start + 读地址 0x89，读 6 字节：T_MSB T_LSB CRC RH_MSB RH_LSB CRC
 *         4. 校验两组 CRC，换算为 °C 与 %RH
 * @param  temp : [输出] 温度（单位 0.1°C，如 25.5°C -> 255）
 * @param  humi : [输出] 相对湿度（单位 0.1%RH，如 60.3% -> 603）
 * @retval 0=成功；1=写地址无 ACK；2=读地址无 ACK；3=温度 CRC 错；4=湿度 CRC 错
 */
u8 YX8320B_ReadTempHumi(int *temp, int *humi)
{
  static const u8 addr_w_list[] = YX_ADDR_W_LIST;
  u8 aw = addr_w_list[g_yx_addr];   /* 使用 Init 探测确认的地址 */
  u8 err = 1;

  /* 失败重试：一次通信失败（SMBus 超时/从机忙）时释放总线后重试 */
  for (u8 attempt = 0; attempt < 3; attempt++)
  {
    u8 buf[6];

    /* ---- 1. 发送单次测量命令 ---- */
    YX_I2C_Start();
    if (YX_I2C_WriteByte(aw) != 0)
    {
      YX_I2C_Stop();
      YX_DelayMs(2);                    /* 总线恢复 */
      err = 1;                          /* 写地址无 ACK */
      continue;
    }
    /* 命令字节必须被 ACK，否则从机未进入测量 */
    if (YX_I2C_WriteByte(YX_CMD_MSB) != 0 ||
        YX_I2C_WriteByte(YX_CMD_LSB) != 0)
    {
      YX_I2C_Stop();
      YX_DelayMs(2);
      err = 5;                          /* 命令字节无 ACK */
      continue;
    }
    YX_I2C_Stop();

    /* ---- 2. 等待测量完成 ---- */
    YX_DelayMs(YX_T_MEAS_MS);

    /* ---- 3. 读 6 字节数据 ---- */
    YX_I2C_Start();
    if (YX_I2C_WriteByte(YX_ADDR_R(aw)) != 0)
    {
      YX_I2C_Stop();
      YX_DelayMs(2);
      err = 2;                          /* 读地址无 ACK */
      continue;
    }
    for (u8 i = 0; i < 6; i++)
      buf[i] = YX_I2C_ReadByte(i < 5 ? 1u : 0u);
    YX_I2C_Stop();

    /* ---- 4. CRC 校验 + 换算 ---- */
    if (YX_CRC8(&buf[0], 2) != buf[2])
    {
      printf("[YX8320B] raw: %02X %02X |crc%02X   %02X %02X |crc%02X\r\n",
             buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
      err = 3; continue;                /* 温度 CRC 错 */
    }
    if (YX_CRC8(&buf[3], 2) != buf[5]) { err = 4; continue; }   /* 湿度 CRC 错 */

    u32 traw = ((u32)buf[0] << 8) | buf[1];
    u32 hraw = ((u32)buf[3] << 8) | buf[4];

    float tc = -45.0f + 175.0f * (float)traw / 65535.0f;
    float rh = 100.0f * (float)hraw / 65535.0f;

    *temp = (int)(tc >= 0.0f ? tc * 10.0f + 0.5f : tc * 10.0f - 0.5f);  /* 0.1°C */
    *humi = (int)(rh * 10.0f + 0.5f);                                    /* 0.1%RH */
    return 0;
  }
  return err;
}

/************************************************************************************************************
 * 每秒调用：读一次温湿度（手册建议 <=1 次/秒，避免自热），失败时保留上一次有效值。
 *   首次成功/失败会通过 printf 打印调试信息；界面通过 YX8320B_GetTemp/GetHumi 取当前值。
 ************************************************************************************************************/
void YX8320B_Task(void)
{
  int t, h;
  u8 err = YX8320B_ReadTempHumi(&t, &h);
  if (err != 0)
  {
    printf("[YX8320B] read fail(err=%u: %s), keep last: T=%d.%d H=%d.%d\r\n",
           err, err == 1 ? "W-addr no ACK" : (err == 2 ? "R-addr no ACK" :
               (err == 3 ? "T CRC" : "RH CRC")),
           g_temp_cache / 10, g_temp_cache % 10, g_humi_cache / 10, g_humi_cache % 10);
  }
  else
  {
    g_temp_cache = t;
    g_humi_cache = h;
    if (!g_first_ok)
    {
      g_first_ok = 1;
      printf("[YX8320B] OK: T=%d.%d C, H=%d.%d %%RH\r\n",
             g_temp_cache / 10, g_temp_cache % 10, g_humi_cache / 10, g_humi_cache % 10);
    }
  }
}

int YX8320B_GetTemp(void) { return g_temp_cache; }
int YX8320B_GetHumi(void) { return g_humi_cache; }

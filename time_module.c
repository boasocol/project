/************************************************************************************************************
 * @attention
 *
 * Firmware Disclaimer Information
 *
 * 1. The customer hereby acknowledges and agrees that the program technical documentation, including the
 *    code, which is supplied by Holtek Semiconductor Inc., (hereinafter referred to as "HOLTEK") is the
 *    proprietary and confidential intellectual property of HOLTEK, and is protected by copyright law and
 *    other intellectual property laws.
 *
 * 2. The customer hereby acknowledges and agrees that the program technical documentation, including the
 *    code, is confidential information belonging to HOLTEK, and must not be disclosed to any third parties
 *    other than HOLTEK and the customer.
 *
 * 3. The program technical documentation, including the code, is provided "as is" and for customer reference
 *    only. After delivery by HOLTEK, the customer shall use the program technical documentation, including
 *    the code, at their own risk. HOLTEK disclaims any expressed, implied or statutory warranties, including
 *    the warranties of merchantability, satisfactory quality and fitness for a particular purpose.
 *
 * <h2><center>Copyright (C) Holtek Semiconductor Inc. All rights reserved</center></h2>
 ************************************************************************************************************/
//-----------------------------------------------------------------------------
#include "time_module.h"
#include "stdio.h"

/* 半秒相位标志：RTC CSEC 中断每 0.5s 翻转一次。
   0=前半秒（整秒进位后 0-500ms），1=后半秒（500-1000ms）。
   main 据此分时隙调度：前半秒只做翻页动画，后半秒做其他静态刷新。 */
volatile u8 g_half_phase = 0;

/**
 * @brief  RTC��ʼ��ħ����
 * @note   д�뱸�ݼĴ��������ڱ��RTC�Ƿ��ѳ�ʼ����
 *         �״��ϵ�ʱ����Ϊ0x5A5A�������ϵ��⵽��ֵ������ʱ���ʼ��
 *         ���ݼĴ�����VDD�������VBAT�������ݣ�ȷ��ʱ���������
 */
#define RTC_INIT_MAGIC 0x5A5A

/**
 * @brief  ���ݼĴ�����ţ����ڴ洢��ʼ�����
 * @note   PWRCU_BAKREG_0~9��10�����ݼĴ�������
 *         ����ʹ��BAKREG_0�洢��ʼ��״̬
 */
#define BAKREG_INIT_FLAG PWRCU_BAKREG_0

/**
 * @brief  ����ģʽ�궨��
 * @note   ��DEBUG_FORCE_RTC_REINITΪ1ʱ��ÿ���ϵ綼�����³�ʼ��ʱ��
 *         �ʺϿ������Խ׶�ʹ�ã�����ʱӦ��Ϊ0
 */
#define DEBUG_FORCE_RTC_REINIT 1

/**
 * @brief  ȫ��ʱ�����
 * @note   ʹ��volatile�ؼ���ȷ�����������Ż��Ըñ����ķ���
 *         �ñ�����RTC���ж��б����£�����ѭ���б���ȡ
 *         ����ʹ��volatile��ֹ��̬����
 */
static volatile TimeStruct g_current_time;

vu32 g_time_update_flag = 0;

/**
 * @brief  ð����˸��־��0.5s ���ģ�
 * @note   �� RTC 0.5s �ж�ÿ������ 1����ѭ���������㲢��תð�š�
 *         �ж���ֻ�ñ�־������ SPI����������ѭ��ˢ�½�����
 */
vu8 g_colon_blink_flag = 0;

/**
 * @brief  ��������
 * @note   ���º���Ϊģ���ڲ�ʹ�ã������Ⱪ¶
 */
static u8 is_leap_year(u16 year);                     // �ж��Ƿ�Ϊ����
static u8 get_days_in_month(u16 year, u8 month);      // ��ȡָ���·ݵ�����
static u32 time_to_timestamp(TimeStruct *time);       // ��ʱ��ת��ΪUnixʱ���
static void timestamp_to_time(u32 timestamp, TimeStruct *time);  // ��Unixʱ���ת��Ϊʱ��
static void time_add_second(TimeStruct *time);        // ʱ��ṹ���һ��

//-----------------------------------------------------------------------------
/**
 * @brief  ʱ��ģ���ʼ������
 * @note   �Ż�������ʹ��RTC���ж�ά��ʱ��ṹ�壬����ÿ�ζ�ȡ����ʱ���ת��
 *         �������ƣ���ѭ����ȡʱ������ȡȫ�ֱ������޸��Ӽ���
 *         
 *         ��ʼ�����̣�
 *         1. ��λRTC����Ĵ���
 *         2. ��鱸�ݼĴ����еĳ�ʼ�����
 *         3. �״��ϵ磺����Ĭ��ʱ��Ϊ2024-01-01 00:00:00��ת��Ϊʱ���д��RTC������
 *         4. ���״��ϵ磺��ȡRTC��������ת��Ϊʱ��ṹ�壨����ʼ��ʱִ��һ�Σ�
 *         5. ����RTCʱ��ԴΪLSI���ڲ�����ʱ��Լ32kHz��
 *         6. ����Ԥ��ƵΪ32768��ʹRTCÿ�����1��
 *         7. ʹ��NVIC�е�RTC�ж�
 *         8. ʹ��RTC���жϣ�CNT SECOND Event��
 *         9. ʹ��RTC������
 *         
 * @retval None
 */
void TimeModule_Init(void)
{
  u32 init_flag;   // ���ݼĴ����еĳ�ʼ�����ֵ
  u8 is_first_init = 0;  // �Ƿ�Ϊ�״γ�ʼ��
  
  /* ʹ��PWRCUʱ�ӣ�VDDREN�����������RTC�Ĵ����ᵼ��HardFault */
  /* ��HT32L52231�У�RTC����PWRCU�����򣬱�����ʹ��APBCCR1��Bit 6 */
  /* CKCU_PeripClockConfig�е�BKPλ��ӦAPBCCR1��Bit 6 */
  CKCU_PeripClockConfig_TypeDef CKCUClock = {{0}};
  CKCUClock.Bit.BKP     = 1;    // ʹ�ܱ�����ʱ�ӣ�PWRCU/RTC��
  CKCU_PeripClockConfig(CKCUClock, ENABLE);
  
  /* ����Ƿ�Ϊ�״��ϵ磨������RTC_DeInit֮ǰ������CNT�ᱻ���㣩 */
  /* ͨ����ȡ���ݼĴ����ж�RTC�Ƿ��ѳ�ʼ���� */
  /* ���ݼĴ�����VDD��������ɺ󱸵�ر������� */
  #if (LIBCFG_BAKREG)
  init_flag = PWRCU_ReadBackupRegister(BAKREG_INIT_FLAG);
  
  /* �ж��Ƿ���Ҫ���³�ʼ����������
   * 1. DEBUG_FORCE_RTC_REINIT=1������ģʽ��ǿ�����³�ʼ��
   * 2. ���ݼĴ�����ǲ�����ħ���֣��״��ϵ�
   * 3. RTC_CNT=0��֮ǰ�ĳ�ʼ����Ǵ��ڵ�CNT�����㣨���������������
   */
  if (DEBUG_FORCE_RTC_REINIT || (init_flag != RTC_INIT_MAGIC) || (RTC_GetCounter() == 0))
  {
    is_first_init = 1;
    
    /* ����Ĭ��ʱ��Ϊ2026��7��20�� 12:00:00 */
    g_current_time.year = 2026;
    g_current_time.month = 7;
    g_current_time.day = 20;
    g_current_time.hour = 12;
    g_current_time.minute = 0;
    g_current_time.second = 0;
  }
  else
  {
    /* ���״��ϵ磬��ȡRTC��������ǰֵ��CNT Ϊ�����������2 ��ԭ�룩 */
    u32 timestamp = RTC_GetCounter() >> 1;
    
    /* ��Unixʱ���ת��Ϊʱ��ṹ�壬��ʼ��ȫ�ֱ��� */
    /* ��ת�����ڳ�ʼ��ʱִ��һ�Σ��������ж�ά�� */
    timestamp_to_time(timestamp, (TimeStruct *)&g_current_time);
  }
  #else
  /* δʹ�ܱ��ݼĴ������ܣ�ÿ���ϵ綼����Ĭ��ʱ�� */
  is_first_init = 1;
  g_current_time.year = 2026;
  g_current_time.month = 7;
  g_current_time.day = 20;
  g_current_time.hour = 12;
  g_current_time.minute = 0;
  g_current_time.second = 0;
  #endif
  
  /* ��鱸�����Ƿ���Է��� */
  if (PWRCU_CheckReadyAccessed() != PWRCU_OK)
  {
    printf("TimeModule_Init: Backup domain access failed!\r\n");
    while (1);
  }
  
  /* ֻ���״γ�ʼ���Ÿ�λRTC�����������ѱ����CNTֵ */
  if (is_first_init)
  {
    /* ��λ������ */
    PWRCU_DeInit();
    
    /* ����LSE��������ģʽ */
    RTC_LSESMConfig(RTC_LSESM_FAST);
    
    /* ʹ��LSE���ⲿ32.768kHz���� */
    RTC_LSECmd(ENABLE);
    
    /* �ȴ�LSE���� */
    while (CKCU_GetClockReadyStatus(CKCU_FLAG_LSERDY) == RESET);
    
    printf("TimeModule_Init: LSE ready!\r\n");
  }
  
  /* ����RTCʱ��Դ������������ʱ���֮ǰ�� */
  /* RTC_SRC_LSE: �ⲿ32.768kHz���񣬾��ȸ��� */
  RTC_ClockSourceConfig(RTC_SRC_LSE);
  
  /* ����RTCԤ��Ƶֵ������������ʱ���֮ǰ�� */
  /* RTC_RPRE_16384: 16384��Ƶ������ʱ��32768Hzʱ��CK_SECOND = 2Hz */
  /* �� RTC ������ÿ 0.5s �� 1��CSEC �ж�ÿ 0.5s ����һ�Σ���
   *  ʱ���԰����롱ά����ISR ��ÿ���� CSEC ����һ�� time_add_second��
   *  �� CNT ʵ���ǡ����롱������ʱ����� ��2 / ��2 ���㣨�� Get/SetTime���� */
  RTC_SetPrescaler(RTC_RPRE_16384);
  
  /* ������״γ�ʼ����д��ʱ����ͳ�ʼ����� */
  if (is_first_init)
  {
    /* ��ʱ��ת��ΪUnixʱ��� */
    u32 timestamp = time_to_timestamp((TimeStruct *)&g_current_time);
    
    /* ����RTC��������׼��д���µļ���ֵ */
    RTC_Cmd(DISABLE);
    
    /* ���ü�����ֱ��д�루0.5s ����ֵ = ʱ��� ��2����
     * ע��ԭ���롰while(HT_RTC->CNT)���ȴ������������ڻ���ѭ�������Ƴ��� */
    HT_RTC->CNT = (u32)((u64)timestamp << 1);
    
    #if (LIBCFG_BAKREG)
    /* ����ʼ��ħ����д�뱸�ݼĴ���������ѳ�ʼ�� */
    /* �����ϵ�ʱ��⵽��ֵ������ʱ������ */
    PWRCU_WriteBackupRegister(BAKREG_INIT_FLAG, RTC_INIT_MAGIC);
    #endif
  }
  
  /* ����RTC�ж����ȼ� */
  /* HT32L52231��Cortex-M0+�ںˣ���2λ���ȼ�λ��__NVIC_PRIO_BITS = 2�� */
  /* ���ȼ���Χ��0~3������ԽС���ȼ�Խ�� */
  /* RTCΪ�����ȼ��жϣ����ȼ�2��������ռBFTM0����Ҫ�ж� */
  NVIC_SetPriority(RTC_IRQn, 2);
  
  /* ʹ��NVIC�е�RTC�ж� */
  /* RTC_IRQnΪRTC�ж������ţ���ht32l5xxxx_01.h�ж��� */
  NVIC_EnableIRQ(RTC_IRQn);
  
  /* ʹ��RTC���ж� */
  /* RTC_INT_CSEC: CNT SECOND Event�жϣ�Ԥ��Ƶ16384��ÿ 0.5s ����һ�� */
  RTC_IntConfig(RTC_INT_CSEC, ENABLE);

  /* ʹ��RTC������ */
  /* ��������ʼ�ӵ�ǰCNTֵ���� */
  RTC_Cmd(ENABLE);
}

//-----------------------------------------------------------------------------
/**
 * @brief  ����RTCʱ��
 * @note   ͬʱ����RTC��������ȫ��ʱ��ṹ��
 *         �������裺
 *         1. ��ʱ��ת��ΪUnixʱ���
 *         2. ����RTC��������д��ʱ���
 *         3. ����ʹ��RTC������
 *         4. ͬ������ȫ��ʱ��ṹ��
 *         
 *         ע�⣺�˺������ж�RTC������Ӧ�����ٵ���
 * @param  time: ָ��TimeStruct�ṹ���ָ�룬����Ҫ���õ�ʱ��
 * @retval None
 */
void TimeModule_SetTime(TimeStruct *time)
{
  /* ��ʱ��ṹ��ת��ΪUnixʱ��� */
  u32 timestamp = time_to_timestamp(time);
  
  /* ����RTC������ */
  RTC_Cmd(DISABLE);
  
  /* ֱ��д�루0.5s ����ֵ = ʱ��� ��2����
   * ע��ԭ��while(HT_RTC->CNT)���ȴ������������ڻ���ѭ�������Ƴ��� */
  HT_RTC->CNT = (u32)((u64)timestamp << 1);
  
  /* ����ʹ��RTC������ */
  RTC_Cmd(ENABLE);
  
  /* ͬ������ȫ��ʱ��ṹ�� */
  /* ȷ���жϸ��µ�ʱ����RTC������һ�� */
  g_current_time.year = time->year;
  g_current_time.month = time->month;
  g_current_time.day = time->day;
  g_current_time.hour = time->hour;
  g_current_time.minute = time->minute;
  g_current_time.second = time->second;
}

//-----------------------------------------------------------------------------
/**
 * @brief  ��ȡ��ǰRTCʱ��
 * @note   �����Ż��㣺ֱ�Ӷ�ȡ�ж�ά����ȫ�ֱ���������ʱ���ת��
 *         ʹ��˫��ȡ����ֹ��ȡ���������λ���µ�ʱ�䲻һ��
 *         
 *         ��ȡ���̣�
 *         1. �������ֶο���
 *         2. ��ȡ����ʱ���ֶε���ʱ����
 *         3. ������ֶ��Ƿ�仯�����仯�����¶�ȡ
 *         4. ȷ����ȡ����ʱ����������һ���ڵĿ���
 *         
 *         Ϊʲô��Ҫ˫��ȡ��⣿
 *         ��Ϊg_current_time�����ж��б����µģ���ѭ����ȡʱ���ܷ����ж�
 *         �����ȡ��һ��ʱ���λ�����ܵõ�"23:59:59"�����"00:00:00"�ķ�
 *         ͨ��������ֶ��Ƿ�仯�����Լ�⵽������������¶�ȡ
 *         
 * @param  time: ָ��TimeStruct�ṹ���ָ�룬���ڴ洢��ȡ��ʱ��
 * @retval None
 */
void TimeModule_GetTime(TimeStruct *time)
{
  u8 second_snapshot;   // ���ֶεĿ��գ����ڼ���ȡ�������Ƿ�����λ
  
  /* ѭ����ȡ��ֱ�����ζ�ȡ�����ֶ���ͬ */
  do
  {
    /* �������ֶο��� */
    second_snapshot = g_current_time.second;
    
    /* ��ȡ����ʱ���ֶ� */
    /* ����ʹ��volatile�������������Ż���Щ��ȡ */
    time->year = g_current_time.year;
    time->month = g_current_time.month;
    time->day = g_current_time.day;
    time->hour = g_current_time.hour;
    time->minute = g_current_time.minute;
    time->second = g_current_time.second;
    
    /* ������ֶα仯��˵����ȡ�����з������жϣ����¶�ȡ */
  } while (second_snapshot != g_current_time.second);
}

//-----------------------------------------------------------------------------
/**
 * @brief  ��ȡ��ǰUnixʱ���
 * @note   ֱ�Ӷ�ȡRTC������ֵ��������ʱ���ʽת��
 *         Unixʱ�������1970��1��1�� 00:00:00 UTC����ǰʱ�������
 *         RTC_CNT��32λ�Ĵ��������ɱ�ʾԼ136�������
 * @retval ��ǰUnixʱ������룩
 */
u32 TimeModule_GetTimestamp(void)
{
  return RTC_GetCounter() >> 1;   /* CNT Ϊ�����������2 ��ԭ��ʵ��ʱ��� */
}

//-----------------------------------------------------------------------------
/**
  * @brief  Resync g_current_time from RTC hardware counter.
  * @note   During Deep Sleep, the RTC second interrupt is disabled to stop
  *         waking the CPU, so g_current_time stops updating; but the RTC
  *         counter (LSI) keeps counting. Call this after wakeup to restore
  *         the correct time (it uses TimeModule_GetTimestamp which reads
  *         the hardware counter).
  * @retval None
  */
void TimeModule_ResyncFromRTC(void)
{
  /* 库要求：从 DeepSleep1/2 唤醒后【立即】读 CNT 需等待至少 1/CK_RTC(约30us)，
     否则读到的是未同步的旧值，导致时间被回拨/重置。
     RTC 用 LSE=32768Hz，1/CK_RTC ≈ 30us，取 60us 留余量。 */
  volatile u32 dly = (SystemCoreClock / 1000000u) * 60u;
  while (dly--);

  u32 ts = TimeModule_GetTimestamp();   /* RTC hardware counter, accurate */
  if (ts == 0) return;                  /* CNT=0 说明备份域被复位，保留现有软时钟，避免时间变 1970 */
  timestamp_to_time(ts, (TimeStruct *)&g_current_time);
}

//-----------------------------------------------------------------------------
/**
 * @brief  RTC�жϴ�������
 * @note   ÿ�봥��һ�Σ�ִ�����²�����
 *         1. ����ȫ��ʱ��ṹ�壨���λ��
 *         2. ͨ�����ڴ�ӡ��ǰʱ��
 *         
 *         �ж����ȼ���RTC�ж����ȼ��ϵͣ�������ռ������Ҫ�ж�
 *         ִ��ʱ�䣺time_add_second()�Ǽ򵥵Ľ�λ���㣬ִ��ʱ�伫��
 *         printf()���ж���ʹ�ã�����printfͨ��fputc���ַ����ͣ��Ҳ�����Ϊ9600��
 *         ÿ�뷢��Լ30���ַ�����������̫��
 *         
 * @retval None
 */
void RTC_IRQHandler(void)
{
  /* ����Ƿ�Ϊ���жϱ�־ */
  /* RTC_FLAG_CSEC: CNT SECOND Event��־��Ԥ��Ƶ16384��ÿ 0.5s ����һ�Σ� */
  if (RTC_GetFlagStatus() & RTC_FLAG_CSEC)
  {
    /* ð����˸���ģ�ÿ 0.5s �ñ�־����ѭ�����Ѻ�תð�ţ�1Hz ��˸����
     * �ж���ֻ�ñ�־������ SPI����������ѭ��ˢ�½����� */
    g_colon_blink_flag = 1;   /* 冒号闪烁节拍：0.5s 一次，主循环消费翻转冒号 */

    /* ÿ���� CSEC = 1 �룺��λʱ�䲢֪ͨ��ѭ��ˢ�¡�
     * ��CNT ��Ϊ���������ʱ���԰����롱ά����
     * g_half_phase 翻转即半秒时隙切换（0=前半秒, 1=后半秒）。 */
    g_half_phase ^= 1;
    if (g_half_phase == 0)
    {
      time_add_second((TimeStruct *)&g_current_time);
      g_time_update_flag = 1;
    }
  }
}

//-----------------------------------------------------------------------------
/**
 * @brief  ʱ��ṹ���һ��
 * @note   ִ�м򵥵Ľ�λ���㣬������֡�ʱ���ա��¡���
 *         �������ƣ����������ֻ�����ֶε��������踴�Ӽ���
 *         ��λ��������60������60��ʱ��24��������������������12�������
 *         
 *         ע�⣺�˺������ж��е��ã�ִ��ʱ����뾡���ܶ�
 *         �Ż����ԣ�ʹ��ifǶ�׶���switch��ֻ�н�λʱ�ż����ж�
 * @param  time: ָ��TimeStruct�ṹ���ָ��
 * @retval None
 */
static void time_add_second(TimeStruct *time)
{
  /* ���ֶε��� */
  time->second++;
  
  /* ���λ��� */
  if (time->second >= 60)
  {
    time->second = 0;   // �����
    time->minute++;     // �ֵ���
    
    /* �ֽ�λ��� */
    if (time->minute >= 60)
    {
      time->minute = 0; // �ֹ���
      time->hour++;     // ʱ����
      
      /* ʱ��λ��� */
      if (time->hour >= 24)
      {
        time->hour = 0; // ʱ����
        time->day++;    // �յ���
        
        /* �ս�λ��� */
        /* ��Ҫ�жϵ�ǰ�·ݵ���������������2�µ�������� */
        if (time->day > get_days_in_month(time->year, time->month))
        {
          time->day = 1;   // �չ��㣨��Ϊ1�ţ�
          time->month++;   // �µ���
          
          /* �½�λ��� */
          if (time->month > 12)
          {
            time->month = 1; // �¹��㣨��Ϊ1�£�
            time->year++;    // �����
          }
        }
      }
    }
  }
}

//-----------------------------------------------------------------------------
/**
 * @brief  �ж��Ƿ�Ϊ����
 * @note   �����жϹ���
 *         1. �ܱ�400�����������꣨��2000�꣩
 *         2. �ܱ�100���������ܱ�400�����Ĳ������꣨��1900�꣩
 *         3. �ܱ�4���������ܱ�100�����������꣨��2024�꣩
 *         4. ���������������
 *         
 *         �˺��������ս�λʱ���ã���Ӱ��������ʱ����
 * @param  year: Ҫ�жϵ����
 * @retval 1: ������, 0: ��������
 */
static u8 is_leap_year(u16 year)
{
  if (year % 400 == 0)
    return 1;           // �ܱ�400������������
  if (year % 100 == 0)
    return 0;           // �ܱ�100���������ܱ�400��������������
  if (year % 4 == 0)
    return 1;           // �ܱ�4���������ܱ�100������������
  return 0;             // �����������������
}

//-----------------------------------------------------------------------------
/**
 * @brief  ��ȡָ���·ݵ�����
 * @note   ������ݺ��·ݷ��ظ��µ�����
 *         2����Ҫ���⴦��������29�죬ƽ��28��
 *         
 *         ƽ�������������
 *         1��:31  2��:28  3��:31  4��:30  5��:31  6��:30
 *         7��:31  8��:31  9��:30  10��:31 11��:30 12��:31
 *         
 *         �˺��������ս�λʱ���ã���Ӱ��������ʱ����
 * @param  year: ���
 * @param  month: �·ݣ�1-12��
 * @retval ���µ�����
 */
static u8 get_days_in_month(u16 year, u8 month)
{
  /* ƽ����������� */
  /* ����0��Ӧ1�£�����11��Ӧ12�� */
  static const u8 days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  
  /* 2�����⴦�� */
  if (month == 2 && is_leap_year(year))
    return 29;          // ����2��29��
  
  /* ���ض�Ӧ�·ݵ����� */
  return days_in_month[month - 1];
}

//-----------------------------------------------------------------------------
/**
 * @brief  ��ʱ��ת��ΪUnixʱ���
 * @note   Unixʱ������壺��1970��1��1�� 00:00:00 UTC��ָ��ʱ�������
 *         ���㷽����
 *         1. �����1970�굽Ŀ�����֮ǰ��������ݵ�������
 *         2. ����Ŀ������д�1�µ�Ŀ���·�֮ǰ�������·ݵ�������
 *         3. ����Ŀ���·��д�1�յ�Ŀ������֮ǰ��������
 *         4. ����ʱ���֡����Ӧ������
 *         
 *         �˺�����������ʱ��ʱ���ã���Ӱ��������ʱ����
 * @param  time: ָ��TimeStruct�ṹ���ָ��
 * @retval Unixʱ������룩
 */
static u32 time_to_timestamp(TimeStruct *time)
{
  u32 timestamp = 0;    // ʱ������
  u16 year;             // ���ѭ������
  u8 month;             // �·�ѭ������
  
  /* �����1970�굽Ŀ�����֮ǰ��������ݵ������� */
  for (year = 1970; year < time->year; year++)
  {
    /* ����366�죬ƽ��365�� */
    /* һ�� = 24Сʱ * 3600�� = 86400�� */
    timestamp += is_leap_year(year) ? 366 * 24 * 3600UL : 365 * 24 * 3600UL;
  }
  
  /* ����Ŀ������д�1�µ�Ŀ���·�֮ǰ�������·ݵ������� */
  for (month = 1; month < time->month; month++)
  {
    timestamp += get_days_in_month(time->year, month) * 24 * 3600UL;
  }
  
  /* ����Ŀ���·��д�1�յ�Ŀ������֮ǰ�������� */
  timestamp += (time->day - 1) * 24 * 3600UL;
  
  /* ����Сʱ��Ӧ������ */
  timestamp += time->hour * 3600UL;
  
  /* ���Ϸ��Ӷ�Ӧ������ */
  timestamp += time->minute * 60UL;
  
  /* �������� */
  timestamp += time->second;
  
  return timestamp;
}

//-----------------------------------------------------------------------------
/**
 * @brief  ��Unixʱ���ת��Ϊʱ��ṹ��
 * @note   ��time_to_timestamp()������Ϊ������
 *         ���㷽����
 *         1. ��1970�꿪ʼ�����ȥ��ֱ��ʣ����������һ��
 *         2. ��1�¿�ʼ���¼�ȥ��ֱ��ʣ����������һ����
 *         3. ����ʣ��������Ӧ���ա�ʱ���֡���
 *         
 *         �˺������ڳ�ʼ��ʱ����һ�Σ���Ӱ��������ʱ����
 * @param  timestamp: Unixʱ������룩
 * @param  time: ָ��TimeStruct�ṹ���ָ�룬���ڴ洢ת�����
 * @retval None
 */
static void timestamp_to_time(u32 timestamp, TimeStruct *time)
{
  u32 remaining = timestamp;   // ʣ������
  u16 year = 1970;            // ��ݣ���1970�꿪ʼ
  u8 month = 1;               // �·ݣ���1�¿�ʼ
  u8 day, hour, minute, second;
  
  /* ������� */
  /* �����ȥ��ֱ��ʣ����������һ�� */
  while (1)
  {
    /* ��ȡ��ǰ��ݵ����� */
    u32 days_in_year = is_leap_year(year) ? 366 : 365;
    
    /* ���㵱ǰ��ݵ������� */
    u32 seconds_in_year = days_in_year * 24 * 3600UL;
    
    /* ���ʣ����������һ�꣬����ѭ�� */
    if (remaining < seconds_in_year)
      break;
    
    /* ��ȥ��ǰ��ݵ����� */
    remaining -= seconds_in_year;
    
    /* ��ݼ�1 */
    year++;
  }
  
  /* �����·� */
  /* ���¼�ȥ��ֱ��ʣ����������һ���� */
  while (1)
  {
    /* ��ȡ��ǰ�·ݵ����� */
    u32 days_in_month = get_days_in_month(year, month);
    
    /* ���㵱ǰ�·ݵ������� */
    u32 seconds_in_month = days_in_month * 24 * 3600UL;
    
    /* ���ʣ����������һ���£�����ѭ�� */
    if (remaining < seconds_in_month)
      break;
    
    /* ��ȥ��ǰ�·ݵ����� */
    remaining -= seconds_in_month;
    
    /* �·ݼ�1 */
    month++;
  }
  
  /* �������� */
  /* ʣ������ / ÿ������ = ��������1����Ϊ���ڴ�1��ʼ */
  day = remaining / (24 * 3600UL) + 1;
  
  /* ��ȥ����ռ�õ����� */
  remaining %= (24 * 3600UL);
  
  /* ����Сʱ */
  hour = remaining / 3600UL;
  
  /* ��ȥСʱռ�õ����� */
  remaining %= 3600UL;
  
  /* ������� */
  minute = remaining / 60UL;
  
  /* �������� */
  second = remaining % 60UL;
  
  /* ������������ʱ��ṹ�� */
  time->year = year;
  time->month = month;
  time->day = day;
  time->hour = hour;
  time->minute = minute;
  time->second = second;
}
//-----------------------------------------------------------------------------

/* ============================================================================
 * Lunar (Chinese calendar) conversion
 * Data table covers 2000-2099. Each entry encodes one lunar year:
 *   bit 0-3  : leap month number (0 = no leap month)
 *   bit 4-15 : big(1)/small(0) month flags for 1..12 (30/29 days)
 *   bit 16   : leap month is big(1)/small(0)
 * Reference date: 1900-01-31 = lunar 1900 first day of first month.
 * Algorithm validated against known dates (Spring Festival 2023/24/25/26,
 * 2023 leap-2nd-month, 2024 Lantern). 2026-08-07 = lunar 6th month 25th.
 * ========================================================================== */
#define LUNAR_BASE_YEAR   2000u
/* 农历数据表覆盖 2000-2099（索引 = year-2000）。
   offset 以【2000-02-05 = 农历 2000 年正月初一】为基准（见 TimeModule_GetLunar），
   故数组只含 2000 年起的数据，无需 1900-1999（省一半表空间）。
   数据与标准农历（春节/闰月/元宵等已知日期）一致。 */
static const u32 s_lunar_info[100] = {
  /* 2000-2009 */
  0x0c960,0x0d954,0x0d4a0,0x0da50,0x07552,0x056a0,0x0abb7,0x025d0,0x092d0,0x0cab5,
  /* 2010-2019 */
  0x0a950,0x0b4a0,0x0baa4,0x0ad50,0x055d9,0x04ba0,0x0a5b0,0x15176,0x052b0,0x0a930,
  /* 2020-2029 */
  0x07954,0x06aa0,0x0ad50,0x05b52,0x04b60,0x0a6e6,0x0a4e0,0x0d260,0x0ea65,0x0d530,
  /* 2030-2039 */
  0x05aa0,0x076a3,0x096d0,0x04afb,0x04ad0,0x0a4d0,0x1d0b6,0x0d250,0x0d520,0x0dd45,
  /* 2040-2049 */
  0x0b5a0,0x056d0,0x055b2,0x049b0,0x0a577,0x0a4b0,0x0aa50,0x0b255,0x06d20,0x0ada0,
  /* 2050-2059 */
  0x14b63,0x09370,0x049f8,0x04970,0x064b0,0x168a6,0x0ea50,0x06b20,0x1a6c4,0x0aae0,
  /* 2060-2069 */
  0x0a2e0,0x0d2e3,0x0c960,0x0d557,0x0d4a0,0x0da50,0x05d55,0x056a0,0x0a6d0,0x055d4,
  /* 2070-2079 */
  0x052d0,0x0a9b8,0x0a950,0x0b4a0,0x0b6a6,0x0ad50,0x055a0,0x0aba4,0x0a5b0,0x052b0,
  /* 2080-2089 */
  0x0b273,0x06930,0x07337,0x06aa0,0x0ad50,0x14b55,0x04b60,0x0a570,0x054e4,0x0d160,
  /* 2090-2099 */
  0x0e968,0x0d520,0x0daa0,0x16aa6,0x056d0,0x04ae0,0x0a9d4,0x0a2d0,0x0d150,0x0f252
};

/** Index into s_lunar_info for a lunar year (only valid 2000-2099). */
static u16 lunar_info_index(u16 year)
{
  return (u16)(year - LUNAR_BASE_YEAR);
}

/** Get leap month number (0 = none) for a lunar year. */
static u8 lunar_leap_month(u16 year)
{
  return (u8)(s_lunar_info[lunar_info_index(year)] & 0x0Fu);
}

/** Days in the leap month of a lunar year (0 if no leap). */
static u8 lunar_leap_days(u16 year)
{
  if (lunar_leap_month(year) == 0) return 0;
  return (s_lunar_info[lunar_info_index(year)] & 0x10000u) ? 30u : 29u;
}

/** Days in month m (1..12) of a lunar year. */
static u8 lunar_month_days(u16 year, u8 month)
{
  return (s_lunar_info[lunar_info_index(year)] & (0x10000u >> month)) ? 30u : 29u;
}

/** Total days in a lunar year. */
static u16 lunar_year_days(u16 year)
{
  u16 sum = 348u;                    /* 12 months x 29 min */
  u32 info = s_lunar_info[lunar_info_index(year)];
  u16 i = 0x8000u;
  while (i > 0x8u) { if (info & i) sum++; i >>= 1; }
  return (u16)(sum + lunar_leap_days(year));
}

/**
  * @brief  Convert Gregorian date to lunar date.
  * @param  year:  Gregorian year (2000-2099)
  * @param  month: Gregorian month 1-12
  * @param  day:   Gregorian day 1-31
  * @param  lunar: output LunarDate*
  * @retval 0=success; -1=year out of range
  */
int TimeModule_GetLunar(u16 year, u8 month, u8 day, LunarDate *lunar)
{
  if (year < 2000u || year > 2099u)
  {
    lunar->lunarYear = year; lunar->lunarMonth = 0; lunar->lunarDay = 0;
    lunar->isLeapMonth = 0;
    return -1;
  }
  /* offset = 目标日期距 2000-02-05（农历2000正月初一）的天数。
     offset = Σ(2000..year-1 各年天数) + 目标日期当年序数 - 36。
     其中 36 = 2000-02-05 在 2000 年的序数（1月31天+5天），
     使 2000-02-05 时 offset=0，正月初一=0。 */
  u32 offset = 0;
  u16 yy;
  for (yy = LUNAR_BASE_YEAR; yy < year; yy++)
    offset += ((yy % 4 == 0 && yy % 100 != 0) || yy % 400 == 0) ? 366u : 365u;
  /* 目标日期当年序数 day_of_year（1月1日=1） */
  static const u8 md[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  u8 m;
  for (m = 0; m < month - 1; m++) offset += md[m];
  if (month > 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
    offset += 1;                       /* 闰年 2 月 +1 */
  offset += (u32)day;                  /* day_of_year = 前月天数 + day */
  offset -= 36u;                       /* 减 2000-02-05 的序数 36，基准归零 */

  /* locate lunar year（从 2000 起，数组只含 2000 起数据） */
  u16 ly = LUNAR_BASE_YEAR;
  while (offset >= lunar_year_days(ly)) { offset -= lunar_year_days(ly); ly++; }

  /* locate lunar month */
  u8 leap = lunar_leap_month(ly);
  u8 is_leap = 0;
  u8 lm = 1;
  for (;;)
  {
    u8 dim;
    if (leap > 0 && lm == leap + 1 && !is_leap)
    {
      is_leap = 1; lm--; dim = lunar_leap_days(ly);
    }
    else
    {
      dim = lunar_month_days(ly, lm);
    }
    if (offset < dim) break;
    offset -= dim;
    if (is_leap && lm == leap) is_leap = 0;
    lm++;
  }

  lunar->lunarYear = ly;
  lunar->lunarMonth = lm;
  lunar->lunarDay = (u8)(offset + 1);
  lunar->isLeapMonth = is_leap;
  return 0;
}
//-----------------------------------------------------------------------------

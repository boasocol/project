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
    g_colon_blink_flag = 1;

    /* ÿ���� CSEC = 1 �룺��λʱ�䲢֪ͨ��ѭ��ˢ�¡�
     * ��CNT ��Ϊ���������ʱ���԰����롱ά���� */
    static u8 half_tick = 0;
    half_tick ^= 1;
    if (half_tick == 0)
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

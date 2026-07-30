#include "env_manager.h"
#include "lcd_driver.h"
#include "lyric_service.h"
#include "rtc.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CELSIUS "\02"

// --- 静态资源：中英文星期查表 ---
static const char *WEEK_NAMES[] = {"SUN", "MON", "TUE", "WED",
                                   "THU", "FRI", "SAT"};
// GB2312: 星期日一二三四五六
static const uint8_t WEEK_CN[7][3] = {
    {0xC8, 0xD5, 0x00}, // 日
    {0xD2, 0xBB, 0x00}, // 一
    {0xB6, 0xFE, 0x00}, // 二
    {0xC8, 0xFD, 0x00}, // 三
    {0xCB, 0xC4, 0x00}, // 四
    {0xCE, 0xE5, 0x00}, // 五
    {0xC1, 0xF9, 0x00}, // 六
};
static const uint8_t STR_XING[2] = {0xD0, 0xC7}; // 星
static const uint8_t STR_QI[2] = {0xC6, 0xDA};   // 期
static const uint8_t STR_NIAN[2] = {0xC4, 0xEA}; // 年
static const uint8_t STR_YUE[2] = {0xD4, 0xC2};  // 月
static const uint8_t STR_RI[2] = {0xC8, 0xD5};   // 日

// --- 全局影子变量初始化 ---
char g_str_time[9] = "00:00:00";
char g_str_time_s[7] = "00:00";
char g_str_date_std[11] = "2000-01-01";
char g_str_date_md[6] = "01/01";
char g_str_week_en[4] = "???";
char g_str_week_cn[7] = "?";
char g_str_date_cn[17] = "2000?01?01?";
char g_str_temp[7] = "--.-" CELSIUS;
char g_str_humi[5] = "--%";
char g_str_ampm[3] = "";

TimeFormat_t g_time_format_config = TIME_FORMAT_24H;

// 环境数据超时（30 秒无更新则清空）
#define ENV_TIMEOUT_MS 30000
static uint32_t last_temp_update_tick = 0;
static uint32_t last_humi_update_tick = 0;

// 内部状态记录
static RTC_DateTimeTypeDef last_sync_time;
static uint32_t last_rtc_read_tick = 0;

/**
 * @brief 手搓数字转字符（补零）
 */
static void _uitoa_2d(uint8_t val, char *out) {
  out[0] = (val / 10 % 10) + '0';
  out[1] = (val % 10) + '0';
}

/**
 * @brief 精炼时间字符串 (逻辑核心)
 */
static void _Env_Refine_Time(RTC_DateTimeTypeDef *t) {
  uint8_t h = t->hours;

  // 1. 处理 AM/PM 和 12/24H 转换
  if (g_time_format_config == TIME_FORMAT_12H) {
    if (h >= 12) {
      strcpy(g_str_ampm, "PM");
      if (h > 12)
        h -= 12;
    } else {
      strcpy(g_str_ampm, "AM");
      if (h == 0)
        h = 12;
    }
  } else {
    strcpy(g_str_ampm, " ");
  }

  // 2. 填充 g_str_time "HH:MM:SS" 和 g_str_time_s "HH:MM"
  _uitoa_2d(h, &g_str_time[0]);
  _uitoa_2d(h, &g_str_time_s[0]);
  g_str_time[2] = ':';
  g_str_time_s[2] = ':';
  _uitoa_2d(t->minutes, &g_str_time[3]);
  _uitoa_2d(t->minutes, &g_str_time_s[3]);
  g_str_time[5] = ':';
  _uitoa_2d(t->seconds, &g_str_time[6]);
  g_str_time[8] = '\0';
  g_str_time_s[5] = '\0';

  if (g_str_time[0] == '0') {
    g_str_time[0] = ' ';
  }
}

/**
 * @brief 精炼日期与星期
 */
static void _Env_Refine_Date(RTC_DateTimeTypeDef *t) {
  // yyyy-mm-dd
  uint16_t full_year = t->year;
  g_str_date_std[0] = (full_year / 1000) + '0';
  g_str_date_std[1] = (full_year / 100 % 10) + '0';
  g_str_date_std[2] = (full_year / 10 % 10) + '0';
  g_str_date_std[3] = (full_year % 10) + '0';
  g_str_date_std[4] = '-';
  _uitoa_2d(t->month, &g_str_date_std[5]);
  g_str_date_std[7] = '-';
  _uitoa_2d(t->day, &g_str_date_std[8]);
  g_str_date_std[10] = '\0';

  // 填充 g_str_date_md "MM/DD"
  _uitoa_2d(t->month, &g_str_date_md[0]);
  g_str_date_md[2] = '/';
  _uitoa_2d(t->day, &g_str_date_md[3]);
  g_str_date_md[5] = '\0';

  // Week: SUN, MON... (STM32 HAL RTC: 1=Mon, 2=Tue...6=Sat, 7=Sun)
  uint8_t wix = (t->weekday == 7) ? 0 : t->weekday;
  strncpy(g_str_week_en, WEEK_NAMES[wix], 3);
  g_str_week_en[3] = '\0';

  // 星期 (GB2312): "星期X"
  uint8_t *p = (uint8_t *)g_str_week_cn;
  *p++ = STR_XING[0];
  *p++ = STR_XING[1];
  *p++ = STR_QI[0];
  *p++ = STR_QI[1];
  *p++ = WEEK_CN[wix][0];
  *p++ = WEEK_CN[wix][1];
  *p = 0;

  // 日期 (GB2312): "2026年02月21日"
  p = (uint8_t *)g_str_date_cn;
  // 年 4 位
  uint16_t y = t->year;
  *p++ = (y / 1000) + '0';
  *p++ = (y / 100 % 10) + '0';
  *p++ = (y / 10 % 10) + '0';
  *p++ = (y % 10) + '0';
  *p++ = STR_NIAN[0];
  *p++ = STR_NIAN[1];
  // 月 2 位
  *p++ = (t->month / 10) + '0';
  *p++ = (t->month % 10) + '0';
  *p++ = STR_YUE[0];
  *p++ = STR_YUE[1];
  // 日 2 位
  *p++ = (t->day / 10) + '0';
  *p++ = (t->day % 10) + '0';
  *p++ = STR_RI[0];
  *p++ = STR_RI[1];
  *p = 0;
}

void Env_Manager_Init(void) {
  // --- 强制进行第一次同步读取 ---
  RTC_GetDateTime(&hrtc, &last_sync_time);
  _Env_Refine_Time(&last_sync_time);
  _Env_Refine_Date(&last_sync_time);

  // --- 初始化计时器 ---
  last_rtc_read_tick = HAL_GetTick();
  last_temp_update_tick = HAL_GetTick();
  last_humi_update_tick = HAL_GetTick();
}

void Env_Manager_Tick(void) {
  uint32_t now = HAL_GetTick();

  // --- 1. 检查并处理串口发来的对时请求 ---
  if (g_sys.rtc_dirty) {
    RTC_DateTimeTypeDef sync_time;

    // 将 rtc_buf[7] 转换为结构体 (顺序是 Y, M, D, h, m, s, w)
    sync_time.year = 2000U + g_sys.rtc_buf[0];
    sync_time.month = g_sys.rtc_buf[1];
    sync_time.day = g_sys.rtc_buf[2];
    sync_time.hours = g_sys.rtc_buf[3];
    sync_time.minutes = g_sys.rtc_buf[4];
    sync_time.seconds = g_sys.rtc_buf[5];
    // 协议: 0~6 (0=Sun, 1=Mon...6=Sat) → STM32 HAL RTC: 1~7 (1=Mon...7=Sun)
    sync_time.weekday = (g_sys.rtc_buf[6] == 0) ? 7 : g_sys.rtc_buf[6];

    // 调用 HAL RTC 设置
    RTC_SetDateTime(&hrtc, &sync_time);

    // 立即更新影子字符串
    _Env_Refine_Time(&sync_time);
    _Env_Refine_Date(&sync_time);

    g_sys.rtc_dirty = 0;
  }

  // --- 2. RTC 定时读取 (每 250ms 读一次) ---
  if (now - last_rtc_read_tick > 250) {
    RTC_DateTimeTypeDef cur;
    RTC_GetDateTime(&hrtc, &cur);

    if (cur.seconds != last_sync_time.seconds) {
      _Env_Refine_Time(&cur);
      if (cur.day != last_sync_time.day) {
        _Env_Refine_Date(&cur);
      }
      last_sync_time = cur;
    }
    last_rtc_read_tick = now;
  }

  // --- 3. 环境数据超时清空（30 秒无更新） ---
  if (now - last_temp_update_tick > ENV_TIMEOUT_MS) {
    strcpy(g_str_temp, "--.-" CELSIUS);
  }
  if (now - last_humi_update_tick > ENV_TIMEOUT_MS) {
    strcpy(g_str_humi, "--%");
  }
}

/**
 * @brief  刷新日期/时间到 LCD 屏幕（每 250ms 限频一次）
 */
void Env_Manager_ShowDateTime(void) {
  static uint32_t last_lcd_tick = 0;
  if (HAL_GetTick() - last_lcd_tick >= 250) {
    LCD_ShowString(1, 1, g_str_date_cn);
    LCD_ShowString(2, 1, g_str_week_cn);
    LCD_ShowString(2, 5, g_str_time);
    last_lcd_tick = HAL_GetTick();
  }
}

void Env_Manager_SetFormat(TimeFormat_t format) {
  g_time_format_config = format;
  RTC_DateTimeTypeDef cur;
  RTC_GetDateTime(&hrtc, &cur);
  _Env_Refine_Time(&cur);
}

void RTC_OnTimeSyncReceived(uint8_t *payload, uint8_t len) {
  if (len < 7)
    return;
  for (int i = 0; i < 7; i++) {
    g_sys.rtc_buf[i] = payload[i];
  }
  g_sys.rtc_dirty = 1;
}

void Env_OnTemperatureReceived(uint8_t *payload, uint8_t len) {
  if (len < 2)
    return;
  int t_int = (int8_t)payload[0];
  int t_dec = payload[1];
  if (t_dec < 0)
    t_dec = -t_dec;
  snprintf(g_str_temp, sizeof(g_str_temp), "%2d.%d" CELSIUS, t_int, t_dec);
  last_temp_update_tick = HAL_GetTick();
}

void Env_OnHumidityReceived(uint8_t *payload, uint8_t len) {
  if (len < 2)
    return;
  int h_int = payload[0];
  snprintf(g_str_humi, sizeof(g_str_humi), "%2d%%", h_int);
  last_humi_update_tick = HAL_GetTick();
}
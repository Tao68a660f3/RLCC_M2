#include "bsp_rtc.h"
#include <time.h>

#define RTC_BKP_SIGNATURE 0xA5A5

/* 寄存器级读写辅助 */
static HAL_StatusTypeDef Direct_RTC_WriteCounter(RTC_HandleTypeDef *hrtc,
                                                 uint32_t cnt) {
  hrtc->Instance->CRL |= RTC_CRL_CNF;
  hrtc->Instance->CNTH = (uint16_t)(cnt >> 16);
  hrtc->Instance->CNTL = (uint16_t)(cnt & 0xFFFF);
  hrtc->Instance->CRL &= ~RTC_CRL_CNF;
  return HAL_RTC_WaitForSynchro(hrtc);
}

static uint32_t Direct_RTC_ReadCounter(RTC_HandleTypeDef *hrtc) {
  uint32_t high1, high2, low;
  do {
    high1 = hrtc->Instance->CNTH;
    low = hrtc->Instance->CNTL;
    high2 = hrtc->Instance->CNTH;
  } while (high1 != high2);
  return (high1 << 16) | low;
}

uint8_t RTC_App_Init(RTC_HandleTypeDef *hrtc) {
  if (HAL_RTCEx_BKUPRead(hrtc, RTC_BKP_DR1) != RTC_BKP_SIGNATURE) {
    RTC_DateTimeTypeDef default_dt = {.year = 2026,
                                      .month = 7,
                                      .day = 23,
                                      .weekday = 4,
                                      .hours = 12,
                                      .minutes = 0,
                                      .seconds = 0};
    RTC_SetDateTime(hrtc, &default_dt);
    HAL_RTCEx_BKUPWrite(hrtc, RTC_BKP_DR1, RTC_BKP_SIGNATURE);
    return 1;
  }
  return 0;
}

uint8_t RTC_SetDateTime(RTC_HandleTypeDef *hrtc, RTC_DateTimeTypeDef *dt) {
  struct tm tm_time;

  tm_time.tm_year = dt->year - 1900;
  tm_time.tm_mon = dt->month - 1;
  tm_time.tm_mday = dt->day;
  tm_time.tm_hour = dt->hours;
  tm_time.tm_min = dt->minutes;
  tm_time.tm_sec = dt->seconds;
  tm_time.tm_isdst = 0;

  time_t epoch_seconds = mktime(&tm_time);

  if (epoch_seconds == (time_t)-1) {
    return 0;
  }

  /* 使用安全直接写入函数 */
  return (Direct_RTC_WriteCounter(hrtc, (uint32_t)epoch_seconds) == HAL_OK) ? 1
                                                                            : 0;
}

uint8_t RTC_GetDateTime(RTC_HandleTypeDef *hrtc, RTC_DateTimeTypeDef *dt) {
  /* 使用安全直接读取函数 */
  uint32_t epoch_seconds = Direct_RTC_ReadCounter(hrtc);

  time_t raw_time = (time_t)epoch_seconds;
  struct tm *ptm = localtime(&raw_time);

  if (ptm == NULL) {
    return 0;
  }

  dt->year = ptm->tm_year + 1900;
  dt->month = ptm->tm_mon + 1;
  dt->day = ptm->tm_mday;
  dt->hours = ptm->tm_hour;
  dt->minutes = ptm->tm_min;
  dt->seconds = ptm->tm_sec;
  dt->weekday = (ptm->tm_wday == 0) ? 7 : ptm->tm_wday;

  return 1;
}
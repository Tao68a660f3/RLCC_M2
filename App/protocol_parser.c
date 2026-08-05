#include "protocol_parser.h"
#include "env_manager.h"
#include "lyric_service.h"
#include "ui_manager.h"
#include <stdio.h>

// #define DEBUG_PRT

typedef enum {
  ST_IDLE = 0,
  ST_CMD,
  ST_LEN_H,
  ST_LEN_L,
  ST_PAYLOAD,
  ST_CHECK,
  ST_ERROR
} ParseState;

static ParseState current_state = ST_IDLE;
static uint8_t calc_checksum = 0;
static uint16_t payload_cnt = 0;

void Protocol_Init(void) {
  current_state = ST_IDLE;
  calc_checksum = 0;
  payload_cnt = 0;
}

uint8_t Protocol_FeedByte(uint8_t byte, ProtocolPacket *out_pkt) {
  // 错误态：只有 0xAA 能重置，避免错位
  if (current_state == ST_ERROR) {
    if (byte == 0xAA) {
      current_state = ST_CMD;
      calc_checksum = 0xAA; // 全帧异或：帧头计入
    }
    return 0;
  }

  switch (current_state) {
  case ST_IDLE:
    if (byte == 0xAA) {
      current_state = ST_CMD;
      calc_checksum = 0xAA; // 全帧异或：帧头计入
    }
    break;

  case ST_CMD:
    out_pkt->cmd = byte;
    calc_checksum ^= byte; // 全帧异或：Cmd 计入
    current_state = ST_LEN_H;
    break;

  case ST_LEN_H:
    out_pkt->len = (uint16_t)byte << 8;
    calc_checksum ^= byte; // 全帧异或：LenH 计入
    current_state = ST_LEN_L;
    break;

  case ST_LEN_L:
    out_pkt->len |= byte;
    calc_checksum ^= byte; // 全帧异或：LenL 计入
    // 长度超限 → 错误态，防止写爆 payload
    if (out_pkt->len > MAX_PAYLOAD_SIZE) {
      current_state = ST_ERROR;
      return 0;
    }
    payload_cnt = 0;
    current_state = (out_pkt->len == 0) ? ST_CHECK : ST_PAYLOAD;
    break;

  case ST_PAYLOAD:
    out_pkt->payload[payload_cnt++] = byte;
    calc_checksum ^= byte;
    if (payload_cnt >= out_pkt->len) {
      current_state = ST_CHECK;
    }
    break;

  case ST_CHECK:
    current_state = ST_IDLE;
    if (byte == calc_checksum) {
      // 为字符串补齐结束符，确保安全
      out_pkt->payload[out_pkt->len] = '\0';
      return 1;
    } else {
      current_state = ST_ERROR;
    }
    break;
  }
  return 0;
}

// ---------------------------------------------------------
// 协议分发器：解析成功后，会根据命令码跳转到对应的处理函数
// ---------------------------------------------------------
void Protocol_Dispatcher(ProtocolPacket *pkt) {
  // 先通知 UI 管理器协议命令（用于模式切换）
  UI_Manager_OnProtocolCmd(pkt->cmd, pkt->payload, pkt->len);

  switch (pkt->cmd) {
  case 0x10:
    Lyric_OnMetadataReceived(pkt->payload, pkt->len);
    break;
  case 0x11:
    Lyric_OnSyncReceived(pkt->payload, pkt->len);
    break;
  case 0x12:
  case 0x13:
  case 0x14:
  case 0x15:
    Lyric_OnContentReceived(pkt->cmd, pkt->payload, pkt->len);
    break;
  case 0x16:
    // cmd=0x16 纯文本：由 UI_Manager_OnProtocolCmd 直接处理，不进歌词池
    break;
  case 0x20:
    RTC_OnTimeSyncReceived(pkt->payload, pkt->len);
    break;
  case 0x21:
    Env_OnTemperatureReceived(pkt->payload, pkt->len);
    break;
  case 0x22:
    Env_OnHumidityReceived(pkt->payload, pkt->len);
    break;
  case 0x30:
    // 同 0x16（非媒体文本透传），UI_Manager_OnProtocolCmd 已在顶部统一处理
    break;
  }

#ifdef DEBUG_PRT
  // debug
  for (uint8_t i = 0; i < LYRIC_POOL_SIZE; i++) {
    if (g_sys.sorted_lyrics[i]) {
      printf("Content: %s\r\n", g_sys.sorted_lyrics[i]->text);
      // printf("Start/Duration: %d/%d\r\n",
      // g_sys.sorted_lyrics[i]->start_time_ms,
      //        g_sys.sorted_lyrics[i]->duration);
      // if (g_sys.sorted_lyrics[i]->word_count > 0) {
      //   printf("Times: ");
      //   for (uint8_t j = 0; j < g_sys.sorted_lyrics[i]->word_count; j++) {
      //     printf("%d ", g_sys.sorted_lyrics[i]->time_offsets[j]);
      //   }
      //   printf("\r\n");
      // }
    }
    printf("\r\n");
  }
#endif
}
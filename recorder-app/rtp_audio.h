#ifndef E1_RTP_AUDIO_H
#define E1_RTP_AUDIO_H
#include "audio_format.h"
#include <stddef.h>
#include <stdint.h>

#define RTP_RATE E1_SAMPLE_RATE
#define RTP_FRAMES E1_PACKET_FRAMES
#define RTP_BYTES (RTP_FRAMES * 6)
#define RTP_SLOTS 256
#define RTP_MAX_PACKET 1472
#define RTP_PT_L24 96
#define RTP_PT_RED 97
#define RTP_RED_DISTANCE 12

typedef struct {
  uint16_t sequence;
  uint32_t timestamp, ssrc;
  uint64_t packets, octets, generated;
  unsigned history_count, red_distance;
  uint8_t history[RTP_RED_DISTANCE][RTP_BYTES];
} RtpTx;
typedef struct {
  uint32_t timestamp;
  int valid, redundant;
  int32_t samples[RTP_FRAMES * 2];
} RtpSlot;
typedef struct {
  RtpSlot slots[RTP_SLOTS];
  uint32_t ssrc, origin, cursor, latest, base_sequence, max_sequence;
  int initialized, started, sequence_ready, transit_ready;
  int32_t transit;
  uint64_t sequence_mask, packets, unique_packets, invalid, late, duplicates;
  uint64_t recovered, concealed, played, overruns;
  uint64_t report_expected, report_received;
  double jitter, target, filtered_queue, ratio, phase;
  unsigned offset;
  int32_t block[RTP_FRAMES * 2], next[2], previous[2];
  int have_block, gap, low_latency;
  unsigned fade;
} RtpRx;

void rtp_tx_init(RtpTx *tx, uint32_t ssrc, uint16_t seq, uint32_t timestamp);
size_t rtp_encode(RtpTx *tx, const int32_t *samples, int redundancy,
                  uint8_t out[RTP_MAX_PACKET]);
int rtp_tx_set_distance(RtpTx *tx, unsigned distance);
void rtp_tx_sent(RtpTx *tx, size_t packet_size);
void rtp_rx_init(RtpRx *rx, uint32_t ssrc, unsigned buffer_ms);
int rtp_receive(RtpRx *rx, const uint8_t *packet, size_t size,
                uint64_t arrival_us);
/* Produces silence before prefill. Ratio adjusts only clock drift, not pitch.
 */
void rtp_render(RtpRx *rx, int32_t *out, unsigned frames);
unsigned rtp_queue_frames(const RtpRx *rx);
size_t rtp_rtcp(RtpTx *tx, RtpRx *rx, uint64_t unix_us, uint8_t *out,
                size_t capacity);
int rtp_is_bye(const uint8_t *packet, size_t size, uint32_t ssrc);
#endif

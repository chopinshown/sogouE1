#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../rtp_audio.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  RtpRx *rx = calloc(1, sizeof *rx);
  assert(rx);
  RtpTx tx;
  rtp_tx_init(&tx, 0x12345678, 65530, 0xfffff000);
  rtp_rx_init(rx, tx.ssrc, 120);
  uint8_t packet[RTP_MAX_PACKET], good[RTP_MAX_PACKET];
  int32_t samples[RTP_FRAMES * 2], out[RTP_FRAMES * 2];
  size_t good_size = 0;
  for (unsigned p = 0; p < 50; ++p) {
    for (unsigned f = 0; f < RTP_FRAMES; ++f) {
      samples[f * 2] = p * 1000 + f;
      samples[f * 2 + 1] = -samples[f * 2] - 1;
    }
    size_t n = rtp_encode(&tx, samples, 1, packet);
    assert(n <= 1337);
    if (p >= 10 && p < 14)
      continue;
    assert(!rtp_receive(rx, packet, n,
                        UINT64_C(100000000) +
                            p * RTP_FRAMES * 1000000ULL / RTP_RATE));
    if (p >= RTP_RED_DISTANCE) {
      assert(packet[12] == (0x80 | RTP_PT_L24));
      assert(packet[16] == RTP_PT_L24);
    }
    if (p == 20) {
      memcpy(good, packet, n);
      good_size = n;
    }
  }
  assert(rx->max_sequence > 65535);
  assert(rx->unique_packets == 46);
  for (unsigned p = 10; p < 14; ++p) {
    RtpSlot *slot = &rx->slots[p % RTP_SLOTS];
    assert(slot->valid && slot->redundant);
    assert(slot->samples[0] == (int32_t)p * 1000 &&
           slot->samples[1] == -(int32_t)p * 1000 - 1);
  }
  for (unsigned i = 0; i < 40; ++i)
    rtp_render(rx, out, RTP_FRAMES);
  assert(rx->recovered == 4);
  assert(rx->concealed == 0);
  assert(rx->played == 40 * RTP_FRAMES);
  assert(rx->ratio >= 0.998 && rx->ratio <= 1.002);
  uint64_t packets = rx->packets;
  for (size_t n = 0; n < 12; ++n)
    assert(rtp_receive(rx, good, n, 100000) < 0);
  good[8] ^= 1;
  assert(rtp_receive(rx, good, good_size, 100000) < 0);
  good[8] ^= 1;
  good[15] ^= 1;
  assert(rtp_receive(rx, good, good_size, 100000) < 0);
  good[15] ^= 1;
  assert(rx->packets == packets);
  /* Exercise malformed lengths, RTP extension/padding and header variations. */
  for (unsigned i = 0; i < 20000; ++i) {
    uint8_t fuzz[RTP_MAX_PACKET];
    memcpy(fuzz, good, good_size);
    size_t n = rand() % (good_size + 1);
    for (unsigned j = 0; j < 3; ++j)
      fuzz[rand() % good_size] = (uint8_t)rand();
    rtp_receive(rx, fuzz, n, 100000 + i);
  }
  uint8_t report[256];
  size_t n = rtp_rtcp(&tx, rx, 1700000000000000ULL, report, sizeof report);
  assert(n >= 64 && report[1] == 200 && report[0] == 0x81 && report[53] == 202);
  uint8_t bye[] = {0x81, 203, 0, 1, 0x12, 0x34, 0x56, 0x78};
  assert(rtp_is_bye(bye, sizeof bye, tx.ssrc));
  assert(!rtp_is_bye(bye, 7, tx.ssrc));
  assert(!rtp_is_bye(bye, sizeof bye, tx.ssrc + 1));
  /* Fifteen-ms mode uses a two-packet RED delay and stays below twenty ms. */
  rtp_tx_init(&tx, 42, 65530, 0xfffff000);
  assert(!rtp_tx_set_distance(&tx, 2));
  assert(rtp_tx_set_distance(&tx, 0) < 0);
  rtp_rx_init(rx, tx.ssrc, 15);
  for (unsigned p = 0; p < 200; ++p) {
    memset(samples, 0, sizeof samples);
    size_t size = rtp_encode(&tx, samples, 1, packet);
    if (p != 30 && p != 31)
      assert(!rtp_receive(rx, packet, size, 100000 + p * 2494ULL));
    rtp_render(rx, out, RTP_FRAMES);
  }
  assert(rx->recovered == 2 && rx->concealed == 0 && rx->overruns == 0);
  assert(rtp_queue_frames(rx) * 1000.0 / RTP_RATE < 20);
  uint64_t before_gap = rx->played;
  for (unsigned i = 0; i < 40; ++i)
    rtp_render(rx, out, RTP_FRAMES);
  assert(rx->played - before_gap == 40 * RTP_FRAMES);
  assert(rx->concealed > 0 && rx->started);
  assert(rtp_tx_set_distance(&tx, 12) < 0);
  free(rx);
  puts("RTP_UNIT_PASS: L24 stereo, RED four-packet burst recovery, "
       "sequence/timestamp wrap, RTCP, malformed-packet fuzz");
  return 0;
}

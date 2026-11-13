#include "rtp_audio.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t be16(const uint8_t *p) { return (uint16_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
         p[3];
}
static void put16(uint8_t *p, uint16_t n) {
  p[0] = n >> 8;
  p[1] = n;
}
static void put32(uint8_t *p, uint32_t n) {
  p[0] = n >> 24;
  p[1] = n >> 16;
  p[2] = n >> 8;
  p[3] = n;
}
void rtp_tx_init(RtpTx *tx, uint32_t ssrc, uint16_t seq, uint32_t timestamp) {
  memset(tx, 0, sizeof *tx);
  tx->ssrc = ssrc;
  tx->sequence = seq;
  tx->timestamp = timestamp;
  tx->red_distance = RTP_RED_DISTANCE;
}
size_t rtp_encode(RtpTx *tx, const int32_t *samples, int redundancy,
                  uint8_t out[RTP_MAX_PACKET]) {
  uint8_t pcm[RTP_BYTES];
  for (unsigned i = 0; i < RTP_FRAMES * 2; ++i) {
    uint32_t v = (uint32_t)samples[i];
    pcm[i * 3] = v >> 16;
    pcm[i * 3 + 1] = v >> 8;
    pcm[i * 3 + 2] = v;
  }
  int red = redundancy && tx->history_count >= tx->red_distance;
  out[0] = 0x80;
  out[1] = redundancy ? RTP_PT_RED : RTP_PT_L24;
  if (!tx->generated)
    out[1] |= 0x80;
  put16(out + 2, tx->sequence++);
  put32(out + 4, tx->timestamp);
  put32(out + 8, tx->ssrc);
  size_t n = 12;
  if (red) {
    unsigned offset = tx->red_distance * RTP_FRAMES;
    out[n++] = 0x80 | RTP_PT_L24;
    out[n++] = offset >> 6;
    out[n++] = (offset << 2) | (RTP_BYTES >> 8);
    out[n++] = (uint8_t)RTP_BYTES;
  }
  if (redundancy)
    out[n++] = RTP_PT_L24;
  unsigned slot = tx->history_count % tx->red_distance;
  if (red) {
    memcpy(out + n, tx->history[slot], RTP_BYTES);
    n += RTP_BYTES;
  }
  memcpy(out + n, pcm, RTP_BYTES);
  n += RTP_BYTES;
  memcpy(tx->history[slot], pcm, RTP_BYTES);
  ++tx->history_count;
  tx->timestamp += RTP_FRAMES;
  ++tx->generated;
  return n;
}
int rtp_tx_set_distance(RtpTx *tx, unsigned distance) {
  if (!distance || distance > RTP_RED_DISTANCE || tx->generated)
    return -1;
  tx->red_distance = distance;
  return 0;
}
void rtp_tx_sent(RtpTx *tx, size_t packet_size) {
  ++tx->packets;
  tx->octets += packet_size - 12;
}
void rtp_rx_init(RtpRx *rx, uint32_t ssrc, unsigned buffer_ms) {
  memset(rx, 0, sizeof *rx);
  rx->ssrc = ssrc;
  rx->low_latency = buffer_ms < 40;
  rx->target = fmax(2 * RTP_FRAMES,
                    floor(RTP_RATE * buffer_ms / (1000.0 * RTP_FRAMES) + .5) *
                        RTP_FRAMES);
  rx->filtered_queue = rx->target;
  rx->ratio = 1;
  rx->fade = 220;
}
static void insert(RtpRx *rx, uint32_t timestamp, const uint8_t *pcm, int red) {
  if (!rx->initialized) {
    rx->origin = rx->cursor = rx->latest = timestamp;
    rx->initialized = 1;
  }
  int32_t position = (int32_t)(timestamp - rx->origin);
  if (position < 0 || position % RTP_FRAMES) {
    ++rx->late;
    return;
  }
  int32_t ahead = (int32_t)(timestamp - rx->cursor);
  if (ahead < 0) {
    ++rx->late;
    return;
  }
  if (ahead >= (RTP_SLOTS - 2) * RTP_FRAMES) {
    ++rx->overruns;
    /* Rebuffer a new bounded window after a long scheduling/network gap. */
    for (unsigned i = 0; i < RTP_SLOTS; ++i)
      rx->slots[i].valid = 0;
    rx->cursor = timestamp;
    rx->started = rx->have_block = 0;
    rx->phase = 0;
    rx->filtered_queue = rx->target;
  }
  unsigned index =
      ((uint32_t)(timestamp - rx->origin) / RTP_FRAMES) % RTP_SLOTS;
  RtpSlot *slot = &rx->slots[index];
  if (slot->valid && slot->timestamp == timestamp) {
    if (!slot->redundant || red) {
      ++rx->duplicates;
      return;
    }
  }
  slot->valid = 1;
  slot->timestamp = timestamp;
  slot->redundant = red;
  for (unsigned i = 0; i < RTP_FRAMES * 2; ++i) {
    uint32_t v = (uint32_t)pcm[i * 3] << 16 | (uint32_t)pcm[i * 3 + 1] << 8 |
                 pcm[i * 3 + 2];
    slot->samples[i] = (int32_t)(v << 8) >> 8;
  }
  if ((int32_t)(timestamp + RTP_FRAMES - rx->latest) > 0)
    rx->latest = timestamp + RTP_FRAMES;
}
int rtp_receive(RtpRx *rx, const uint8_t *packet, size_t size,
                uint64_t arrival_us) {
  if (size < 12 || size > RTP_MAX_PACKET || packet[0] >> 6 != 2 ||
      be32(packet + 8) != rx->ssrc)
    goto bad;
  size_t offset = 12 + 4 * (packet[0] & 15);
  if (offset > size)
    goto bad;
  if (packet[0] & 0x10) {
    if (offset + 4 > size)
      goto bad;
    size_t extra = 4 + 4 * (size_t)be16(packet + offset + 2);
    if (extra > size - offset)
      goto bad;
    offset += extra;
  }
  if (packet[0] & 0x20) {
    unsigned pad = packet[size - 1];
    if (!pad || pad > size - offset)
      goto bad;
    size -= pad;
  }
  unsigned type = packet[1] & 127;
  uint32_t timestamp = be32(packet + 4), old_time = 0;
  const uint8_t *old = NULL, *primary = NULL;
  if (type == RTP_PT_L24) {
    if (size - offset != RTP_BYTES)
      goto bad;
    primary = packet + offset;
  } else if (type == RTP_PT_RED) {
    if (offset >= size)
      goto bad;
    if (packet[offset] & 0x80) {
      if (size - offset != 5 + 2 * RTP_BYTES ||
          (packet[offset] & 127) != RTP_PT_L24)
        goto bad;
      unsigned delta =
          (unsigned)packet[offset + 1] << 6 | packet[offset + 2] >> 2;
      unsigned length = (packet[offset + 2] & 3) << 8 | packet[offset + 3];
      if (length != RTP_BYTES ||
          (!delta || delta % RTP_FRAMES ||
           delta > RTP_RED_DISTANCE * RTP_FRAMES) ||
          packet[offset + 4] != RTP_PT_L24)
        goto bad;
      old_time = timestamp - delta;
      old = packet + offset + 5;
      primary = old + RTP_BYTES;
    } else {
      if (packet[offset] != RTP_PT_L24 || size - offset != 1 + RTP_BYTES)
        goto bad;
      primary = packet + offset + 1;
    }
  } else
    goto bad;
  /* Validate the complete packet before changing any receive state. */
  if (rx->initialized && (int32_t)(timestamp - rx->origin) % RTP_FRAMES)
    goto bad;
  uint16_t seq = be16(packet + 2);
  if (!rx->sequence_ready) {
    rx->base_sequence = rx->max_sequence = seq;
    rx->sequence_mask = 1;
    rx->sequence_ready = 1;
    ++rx->unique_packets;
  } else {
    int16_t delta = (int16_t)(seq - (uint16_t)rx->max_sequence);
    if (delta > 0) {
      rx->max_sequence += delta;
      rx->sequence_mask = delta >= 64 ? 1 : (rx->sequence_mask << delta) | 1;
      ++rx->unique_packets;
    } else if (delta > -64) {
      uint64_t mask = UINT64_C(1) << -delta;
      if (!(rx->sequence_mask & mask)) {
        rx->sequence_mask |= mask;
        ++rx->unique_packets;
      }
    }
  }
  int32_t transit =
      (int32_t)((uint32_t)((arrival_us * RTP_RATE) / 1000000) - timestamp);
  if (rx->transit_ready)
    rx->jitter +=
        (fabs((double)(int32_t)((uint32_t)transit - (uint32_t)rx->transit)) -
         rx->jitter) /
        16;
  rx->transit = transit;
  rx->transit_ready = 1;
  ++rx->packets;
  /* Establish the primary origin first; very early RED history is optional. */
  if (!rx->initialized) {
    insert(rx, timestamp, primary, 0);
    return 0;
  }
  if (old)
    insert(rx, old_time, old, 1);
  insert(rx, timestamp, primary, 0);
  return 0;
bad:
  ++rx->invalid;
  return -1;
}
unsigned rtp_queue_frames(const RtpRx *rx) {
  if (!rx->initialized)
    return 0;
  int32_t ahead = (int32_t)(rx->latest - rx->cursor);
  int used = rx->have_block ? (int)rx->offset - RTP_FRAMES : 0;
  return ahead > used ? (unsigned)(ahead - used) : 0;
}
static void load_block(RtpRx *rx) {
  unsigned index =
      ((uint32_t)(rx->cursor - rx->origin) / RTP_FRAMES) % RTP_SLOTS;
  RtpSlot *slot = &rx->slots[index];
  if (slot->valid && slot->timestamp == rx->cursor) {
    memcpy(rx->block, slot->samples, sizeof rx->block);
    if (rx->gap) {
      for (unsigned i = 0; i < RTP_FRAMES; ++i)
        for (unsigned ch = 0; ch < 2; ++ch)
          rx->block[i * 2 + ch] =
              (int32_t)((int64_t)rx->block[i * 2 + ch] * i / RTP_FRAMES);
      rx->gap = 0;
    }
    if (slot->redundant)
      ++rx->recovered;
    slot->valid = 0;
  } else {
    ++rx->concealed;
    rx->gap = 1;
    /* A bounded fade to silence avoids holding stale audio on a dead peer. */
    for (unsigned i = 0; i < RTP_FRAMES; ++i)
      for (unsigned ch = 0; ch < 2; ++ch)
        rx->block[i * 2 + ch] = (int32_t)((int64_t)rx->previous[ch] *
                                          (RTP_FRAMES - i - 1) / RTP_FRAMES);
  }
  rx->cursor += RTP_FRAMES;
  rx->offset = 0;
  rx->have_block = 1;
}
static void source_frame(RtpRx *rx, int32_t frame[2]) {
  if (!rx->have_block || rx->offset == RTP_FRAMES)
    load_block(rx);
  for (unsigned ch = 0; ch < 2; ++ch)
    frame[ch] = rx->block[rx->offset * 2 + ch];
  ++rx->offset;
}
void rtp_render(RtpRx *rx, int32_t *out, unsigned frames) {
  if (((rx->started && !rx->low_latency) ||
       (!rx->started && rx->low_latency)) &&
      rtp_queue_frames(rx) >
          rx->target + (rx->low_latency ? RTP_RATE * .005 : RTP_RATE / 10)) {
    unsigned blocks = (unsigned)ceil(rx->target / RTP_FRAMES);
    rx->cursor = rx->latest - blocks * RTP_FRAMES;
    rx->have_block = 0;
    rx->phase = 0;
    rx->started = 0;
    rx->fade = 220;
    rx->filtered_queue = rx->target;
    ++rx->overruns;
    for (unsigned i = 0; i < RTP_SLOTS; ++i)
      if (rx->slots[i].valid &&
          (int32_t)(rx->slots[i].timestamp - rx->cursor) < 0)
        rx->slots[i].valid = 0;
    for (unsigned i = 0; i < frames; ++i)
      for (unsigned ch = 0; ch < 2; ++ch)
        out[i * 2 + ch] =
            (int32_t)((int64_t)rx->previous[ch] * (frames - i - 1) / frames);
    return;
  }
  if (!rx->started) {
    if (!rx->initialized || rtp_queue_frames(rx) < rx->target) {
      memset(out, 0, frames * 2 * sizeof *out);
      return;
    }
    rx->started = 1;
    source_frame(rx, rx->previous);
    source_frame(rx, rx->next);
  }
  if (!rx->low_latency && rtp_queue_frames(rx) < frames * 1.002 + 2) {
    for (unsigned i = 0; i < frames; ++i)
      for (unsigned ch = 0; ch < 2; ++ch)
        out[i * 2 + ch] =
            (int32_t)((int64_t)rx->previous[ch] * (frames - i - 1) / frames);
    rx->previous[0] = rx->previous[1] = 0;
    rx->started = 0;
    rx->fade = 220;
    ++rx->concealed;
    return;
  }
  double queue = rtp_queue_frames(rx);
  if (!rx->low_latency || queue >= 2 * RTP_FRAMES) {
    rx->filtered_queue += (queue - rx->filtered_queue) * 0.01;
    rx->ratio = fmin(
        1.002, fmax(0.998, 1 + (rx->filtered_queue - rx->target) *
                                   (rx->low_latency ? 0.000005 : 0.0000005)));
  }
  for (unsigned i = 0; i < frames; ++i) {
    for (unsigned ch = 0; ch < 2; ++ch)
      out[i * 2 + ch] = (int32_t)llround(
          rx->previous[ch] +
          rx->phase * ((double)rx->next[ch] - rx->previous[ch]));
    if (rx->fade) {
      for (unsigned ch = 0; ch < 2; ++ch)
        out[i * 2 + ch] =
            (int32_t)((int64_t)out[i * 2 + ch] * (220 - rx->fade) / 220);
      --rx->fade;
    }
    rx->phase += rx->ratio;
    while (rx->phase >= 1) {
      rx->phase -= 1;
      memcpy(rx->previous, rx->next, sizeof rx->previous);
      source_frame(rx, rx->next);
    }
  }
  rx->played += frames;
}
size_t rtp_rtcp(RtpTx *tx, RtpRx *rx, uint64_t unix_us, uint8_t *out,
                size_t capacity) {
  int report = rx->sequence_ready;
  size_t sr = 28 + (report ? 24 : 0);
  char cname[32];
  int cn = snprintf(cname, sizeof cname, "e1-%08x", tx->ssrc);
  size_t sdes = (4 + 4 + 2 + cn + 1 + 3) & ~(size_t)3;
  if (capacity < sr + sdes)
    return 0;
  memset(out, 0, sr + sdes);
  out[0] = 0x80 | report;
  out[1] = 200;
  put16(out + 2, sr / 4 - 1);
  put32(out + 4, tx->ssrc);
  put32(out + 8, (uint32_t)(unix_us / 1000000 + UINT64_C(2208988800)));
  put32(out + 12, (uint32_t)(((unix_us % 1000000) << 32) / 1000000));
  put32(out + 16, tx->timestamp);
  put32(out + 20, tx->packets);
  put32(out + 24, tx->octets);
  if (report) {
    put32(out + 28, rx->ssrc);
    uint64_t expected = (uint32_t)(rx->max_sequence - rx->base_sequence) + 1ULL;
    uint64_t lost =
        expected > rx->unique_packets ? expected - rx->unique_packets : 0;
    if (lost > 0x7fffff)
      lost = 0x7fffff;
    uint64_t interval_expected = expected - rx->report_expected;
    uint64_t interval_received = rx->unique_packets - rx->report_received;
    uint64_t interval_lost = interval_expected > interval_received
                                 ? interval_expected - interval_received
                                 : 0;
    out[32] =
        interval_expected
            ? (uint8_t)fmin(255, interval_lost * 256.0 / interval_expected)
            : 0;
    rx->report_expected = expected;
    rx->report_received = rx->unique_packets;
    out[33] = lost >> 16;
    out[34] = lost >> 8;
    out[35] = lost;
    put32(out + 36, rx->max_sequence);
    put32(out + 40, (uint32_t)rx->jitter);
  }
  uint8_t *s = out + sr;
  s[0] = 0x81;
  s[1] = 202;
  put16(s + 2, sdes / 4 - 1);
  put32(s + 4, tx->ssrc);
  s[8] = 1;
  s[9] = cn;
  memcpy(s + 10, cname, cn);
  return sr + sdes;
}
int rtp_is_bye(const uint8_t *packet, size_t size, uint32_t ssrc) {
  while (size >= 4) {
    if (packet[0] >> 6 != 2)
      return 0;
    size_t n = ((size_t)be16(packet + 2) + 1) * 4;
    if (n > size)
      return 0;
    if (packet[1] == 203) {
      unsigned count = packet[0] & 31;
      if (n < 4 + count * 4)
        return 0;
      for (unsigned i = 0; i < count; ++i)
        if (be32(packet + 4 + i * 4) == ssrc)
          return 1;
    }
    packet += n;
    size -= n;
  }
  return 0;
}

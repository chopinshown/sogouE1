#ifndef E1_SPEECH_CHECKS_H
#define E1_SPEECH_CHECKS_H
#include "../speech_decimator.h"
#include <assert.h>
#include <math.h>
#include <opus.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
static void codec_test(void) {
  int error = 0;
  SpeechDecimator src;
  speech_decimator_init(&src);
  OpusEncoder *enc =
      opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &error);
  OpusDecoder *dec = opus_decoder_create(16000, 1, &error);
  assert(enc && dec);
  opus_encoder_ctl(enc, OPUS_SET_BITRATE(24000));
  float input[(SPEECH_INPUT_RATE / 100)], output[256];
  int16_t frame[960], decoded[960];
  int used = 0, total = 0, packets = 0;
  double energy = 0;
  for (int block = 0; block < 300; block++) {
    for (int i = 0; i < (SPEECH_INPUT_RATE / 100); i++)
      input[i] = 0.25f * sinf(2 * M_PI * 1000 *
                              (block * (SPEECH_INPUT_RATE / 100) + i) /
                              (double)SPEECH_INPUT_RATE);
    int produced = speech_decimator_process(
        &src, input, (SPEECH_INPUT_RATE / 100), output, 256);
    assert(produced == 160);
    total += produced;
    for (int i = 0; i < produced; ++i) {
      frame[used++] = lrintf(output[i] * 32768);
      if (used == 960) {
        unsigned char packet[4096];
        int n = opus_encode(enc, frame, 960, packet, sizeof packet);
        assert(n > 0 && n < 4096);
        assert(opus_packet_get_nb_samples(packet, n, 16000) == 960);
        assert(opus_decode(dec, packet, n, decoded, 960, 0) == 960);
        for (int k = 0; k < 960; k++)
          energy += (double)decoded[k] * decoded[k];
        used = 0;
        packets++;
      }
    }
  }
  assert(total == 48000 && packets == 50 && energy > 1e9);
  opus_encoder_destroy(enc);
  opus_decoder_destroy(dec);
  printf("VOICE_CODEC_PASS samples=%d packets=%d\n", total, packets);
}
static double decimated_tone_rms(double frequency) {
  SpeechDecimator state;
  speech_decimator_init(&state);
  float input[480], output[160];
  double energy = 0;
  unsigned count = 0;
  for (unsigned block = 0; block < 100; ++block) {
    for (unsigned i = 0; i < 480; ++i)
      input[i] =
          .25f * sin(2 * M_PI * frequency * (block * 480 + i) / 48000 + .31);
    assert(speech_decimator_process(&state, input, 480, output, 160) == 160);
    if (block >= 10)
      for (unsigned i = 0; i < 160; ++i) {
        energy += (double)output[i] * output[i];
        ++count;
      }
  }
  return sqrt(energy / count);
}
static void decimator_test(void) {
  double pass = decimated_tone_rms(1000);
  double upper = decimated_tone_rms(7000);
  assert(pass > .17 && pass < .18 && upper > .17 && upper < .18);
  const unsigned stops[] = {8000, 9000, 12000, 20000, 23000};
  for (unsigned i = 0; i < sizeof stops / sizeof *stops; ++i)
    assert(decimated_tone_rms(stops[i]) < .0001);
  SpeechDecimator whole, chunked;
  speech_decimator_init(&whole);
  speech_decimator_init(&chunked);
  float input[4800], expected[1600], actual[1600];
  for (unsigned i = 0; i < 4800; ++i)
    input[i] = .3f * sin(2 * M_PI * 1234 * i / 48000) +
               .1f * sin(2 * M_PI * 11000 * i / 48000);
  SpeechDecimator before = whole;
  assert(speech_decimator_process(&whole, input, 4800, expected, 1599) < 0);
  assert(!memcmp(&before, &whole, sizeof whole));
  assert(speech_decimator_process(&whole, input, 4800, expected, 1600) == 1600);
  const unsigned sizes[] = {1, 2, 7, 480, 23};
  unsigned used = 0, produced = 0, block = 0;
  while (used < 4800) {
    unsigned n = sizes[block++ % 5];
    if (n > 4800 - used)
      n = 4800 - used;
    int count = speech_decimator_process(&chunked, input + used, n,
                                         actual + produced, 1600 - produced);
    assert(count >= 0);
    produced += (unsigned)count;
    used += n;
  }
  assert(produced == 1600 && !memcmp(expected, actual, sizeof actual));
  printf("VOICE_DECIMATOR_PASS pass_rms=%.6f upper_rms=%.6f; alias rejection "
         "and chunk continuity\n",
         pass, upper);
}

#endif

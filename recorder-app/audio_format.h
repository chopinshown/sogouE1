#ifndef E1_AUDIO_FORMAT_H
#define E1_AUDIO_FORMAT_H
#include <stdint.h>

#define E1_SAMPLE_RATE 48000
#define E1_CAPTURE_CHANNELS 8
#define E1_MIC_LEFT 6
#define E1_MIC_RIGHT 7
#define E1_STEREO_CHANNELS 2
#define E1_PACKET_FRAMES 120

/* Keep the native S32 carrier intact; packing/normalization happens later. */
static inline void e1_select_microphones(const int32_t *input, int32_t *output,
                                         unsigned frames) {
  for (unsigned i = 0; i < frames; ++i) {
    output[i * 2] = input[i * E1_CAPTURE_CHANNELS + E1_MIC_LEFT];
    output[i * 2 + 1] = input[i * E1_CAPTURE_CHANNELS + E1_MIC_RIGHT];
  }
}
#endif

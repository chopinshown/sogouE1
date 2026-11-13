#ifndef E1_SPEECH_DECIMATOR_H
#define E1_SPEECH_DECIMATOR_H

#define SPEECH_INPUT_RATE 48000
#define SPEECH_OUTPUT_RATE 16000
#define SPEECH_DECIMATION 3
#define SPEECH_FILTER_TAPS 257

typedef struct {
  float coefficients[SPEECH_FILTER_TAPS];
  float history[SPEECH_FILTER_TAPS];
  unsigned write, phase;
} SpeechDecimator;

void speech_decimator_init(SpeechDecimator *state);
/* Stateful low-pass + 3:1 decimation. Returns output count, or -1 if capacity
 * is insufficient (without changing state). Arbitrary chunk sizes are valid. */
int speech_decimator_process(SpeechDecimator *state, const float *input,
                             unsigned frames, float *output, unsigned capacity);
#endif

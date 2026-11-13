#include "speech_decimator.h"
#include <math.h>
#include <string.h>

void speech_decimator_init(SpeechDecimator *state) {
  memset(state, 0, sizeof *state);
  const double pi = 3.14159265358979323846;
  const double cutoff = 7500.0 / SPEECH_INPUT_RATE;
  const unsigned center = (SPEECH_FILTER_TAPS - 1) / 2;
  double sum = 0;
  for (unsigned i = 0; i < SPEECH_FILTER_TAPS; ++i) {
    double x = (double)i - center;
    double sinc =
        i == center ? 2 * cutoff : sin(2 * pi * cutoff * x) / (pi * x);
    double angle = 2 * pi * i / (SPEECH_FILTER_TAPS - 1);
    double window = .42 - .5 * cos(angle) + .08 * cos(2 * angle);
    state->coefficients[i] = (float)(sinc * window);
    sum += sinc * window;
  }
  for (unsigned i = 0; i < SPEECH_FILTER_TAPS; ++i)
    state->coefficients[i] = (float)(state->coefficients[i] / sum);
}

int speech_decimator_process(SpeechDecimator *state, const float *input,
                             unsigned frames, float *output,
                             unsigned capacity) {
  if (frames / SPEECH_DECIMATION +
          (frames % SPEECH_DECIMATION + state->phase) / SPEECH_DECIMATION >
      capacity)
    return -1;
  unsigned count = 0;
  for (unsigned i = 0; i < frames; ++i) {
    state->history[state->write] = input[i];
    if (++state->write == SPEECH_FILTER_TAPS)
      state->write = 0;
    if (++state->phase != SPEECH_DECIMATION)
      continue;
    state->phase = 0;
    /* Compute only retained outputs, using FIR symmetry to halve multiplies. */
    unsigned newest = state->write ? state->write - 1 : SPEECH_FILTER_TAPS - 1;
    unsigned oldest = state->write;
    float sum = 0;
    for (unsigned tap = 0; tap < SPEECH_FILTER_TAPS / 2; ++tap) {
      sum += state->coefficients[tap] *
             (state->history[newest] + state->history[oldest]);
      newest = newest ? newest - 1 : SPEECH_FILTER_TAPS - 1;
      if (++oldest == SPEECH_FILTER_TAPS)
        oldest = 0;
    }
    sum += state->coefficients[SPEECH_FILTER_TAPS / 2] * state->history[newest];
    output[count++] = sum;
  }
  return (int)count;
}

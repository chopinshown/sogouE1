#ifdef NDEBUG
#undef NDEBUG
#endif
#include "speech_checks.h"
int main(void) {
  decimator_test();
  codec_test();
  puts("SPEECH_UNIT_PASS 48000 Hz mono -> low-pass/3 -> 16000 Hz mono Opus");
  return 0;
}

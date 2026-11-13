/* Runs on the E1 with the same codec, resampler and protocol parser as the app.
 * Does not open ALSA or contact a network; synthetic audio only. */
#include "../xiaozhi.c"
#undef NDEBUG
#include <assert.h>

#include "speech_checks.h"

static void json_test(void) {
  int hello = 0;
  const char *good = "{\"type\":\"hello\",\"transport\":\"websocket\","
                     "\"session_id\":\"test\",\"audio_params\":{\"format\":"
                     "\"opus\",\"sample_rate\":24000,\"channels\":1}}";
  assert(on_json("{", 2, &hello) < 0);
  assert(on_json("[]", 3, &hello) < 0);
  const char *bad = "{\"type\":\"hello\",\"transport\":\"mqtt\"}";
  assert(on_json(bad, strlen(bad) + 1, &hello) < 0 && !hello);
  bad = "{\"type\":\"hello\",\"transport\":\"websocket\",\"audio_params\":{"
        "\"sample_rate\":44100}}";
  assert(on_json(bad, strlen(bad) + 1, &hello) < 0 && !hello);
  assert(!on_json(good, strlen(good) + 1, &hello) && hello &&
         server_rate == 24000 && !strcmp(session, "test"));
  assert(on_json(good, strlen(good) + 1, &hello) <
         0); /* Reject renegotiation mid-session. */
  const char *stt =
      "{\"type\":\"stt\",\"session_id\":\"test\",\"text\":\"你好 E1\"}";
  assert(!on_json(stt, strlen(stt) + 1, &hello) && !strcmp(A.user, "你好 E1"));
  stt = "{\"type\":\"stt\",\"session_id\":\"stale\",\"text\":\"stale\"}";
  assert(!on_json(stt, strlen(stt) + 1, &hello) && !strcmp(A.user, "你好 E1"));
  const char *system = "{\"type\":\"system\",\"command\":\"reboot\"}";
  assert(!on_json(system, strlen(system) + 1, &hello));
  puts("VOICE_JSON_PASS malformed hello negotiation Unicode stale-session "
       "remote-system");
}
int main(void) {
  assert(!curl_global_init(CURL_GLOBAL_DEFAULT));
  assert(secure_url("https://api.tenclass.net/xiaozhi/ota/", "https"));
  assert(secure_url("wss://example.com/voice", "wss"));
  assert(!secure_url("ws://example.com/voice", "wss"));
  assert(!secure_url("wss://secret@example.com/voice", "wss"));
  assert(!safe_header("token\r\nInjected: yes"));
  int32_t raw[3 * E1_CAPTURE_CHANNELS];
  float mono[3];
  for (unsigned i = 0; i < 3 * E1_CAPTURE_CHANNELS; ++i)
    raw[i] = INT32_MAX;
  raw[6] = INT32_MAX;
  raw[7] = INT32_MIN;
  raw[8 + 6] = 1073741824;
  raw[8 + 7] = 536870912;
  raw[16 + 6] = -1073741824;
  raw[16 + 7] = -536870912;
  mix_main_channels(raw, mono, 3);
  assert(INPUT_RATE == 48000 && INPUT_FRAMES == 480);
  assert(fabs(mono[0]) < 1e-6 && fabs(mono[1] - .375) < 1e-6 &&
         fabs(mono[2] + .375) < 1e-6);
  puts("VOICE_MAIN_CHANNELS_PASS 48000 Hz, eight-channel capture, selected "
       "channels 6/7");
  decimator_test();
  codec_test();
  json_test();
  curl_global_cleanup();
  puts("VOICE_UNIT_PASS");
  return 0;
}

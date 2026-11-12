/* Runs on the E1 with the same codec, resampler and protocol parser as the app.
 * Does not open ALSA or contact a network; synthetic audio only. */
#include "../xiaozhi.c"
#undef NDEBUG
#include <assert.h>

static void codec_test(void) {
  int error = 0;
  SRC_STATE *src = src_new(SRC_SINC_FASTEST, 1, &error);
  OpusEncoder *enc =
      opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &error);
  OpusDecoder *dec = opus_decoder_create(16000, 1, &error);
  assert(src && enc && dec);
  opus_encoder_ctl(enc, OPUS_SET_BITRATE(24000));
  float input[441], output[256];
  int16_t frame[960], decoded[960];
  int used = 0, total = 0, packets = 0;
  double energy = 0;
  for (int block = 0; block < 300; block++) {
    for (int i = 0; i < 441; i++)
      input[i] = 0.25f * sinf(2 * M_PI * 1000 * (block * 441 + i) / 44100.0);
    SRC_DATA d = {.data_in = input,
                  .input_frames = 441,
                  .data_out = output,
                  .output_frames = 256,
                  .src_ratio = 16000.0 / 44100};
    assert(!src_process(src, &d) && d.input_frames_used == 441);
    total += d.output_frames_gen;
    for (int i = 0; i < d.output_frames_gen; i++) {
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
  assert(total > 47900 && total <= 48000 && packets >= 49 && energy > 1e9);
  src_delete(src);
  opus_encoder_destroy(enc);
  opus_decoder_destroy(dec);
  printf("VOICE_CODEC_PASS samples=%d packets=%d\n", total, packets);
}
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
  codec_test();
  json_test();
  curl_global_cleanup();
  puts("VOICE_UNIT_PASS");
  return 0;
}

#define _GNU_SOURCE
#include "xiaozhi.h"
#include "vendor/cjson/cJSON.h"
#include <alsa/asoundlib.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <opus.h>
#include <pthread.h>
#include <samplerate.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ROOT "/userdata/e1-recorder"
#define CONFIG ROOT "/xiaozhi.json"
#define ID_FILE ROOT "/xiaozhi-client-id"
#define OTA "https://api.tenclass.net/xiaozhi/ota/"
#define MAX_MESSAGE 32768
#define INPUT_RATE 44100
#define VOICE_RATE 16000
#define VOICE_FRAME 960
/* Each queue is bounded to 100 x 60ms, never blocks the recorder/UI thread. */
#define QUEUE 100
#define MAX_PACKET 4096

typedef struct {
  int size;
  unsigned char bytes[MAX_PACKET];
} Packet;
static struct {
  pthread_mutex_t mutex;
  pthread_t thread;
  int running, started, active, job, cancel, finish, volume;
  unsigned sent, received, played;
  double level;
  char state[32], message[256], code[64], user[2048], reply[4096], emotion[64];
} A = {.mutex = PTHREAD_MUTEX_INITIALIZER,
       .volume = 65,
       .level = -96,
       .state = "idle"};
/* Worker-owned configuration/transport/audio. Never returned by the HTTP API.
 */
static char ota_url[1024] = OTA, ws_url[2048], token[2048], device_id[64],
            client_id[64], session[256];
static char ca_bundle[512] = "/etc/ssl/certs/ca-certificates.crt";
static CURL *ws;
static int protocol_version = 1, server_rate = 24000;
static snd_pcm_t *mic, *speaker;
static OpusEncoder *encoder;
static OpusDecoder *decoder;
static SRC_STATE *resampler;
static SRC_STATE *output_resampler;
static float resampled[256];
static int16_t input_frame[VOICE_FRAME];
static int frame_used;
static Packet queue[QUEUE];
static int qhead, qcount, tts_started, tts_stopped;
static unsigned char incoming[MAX_MESSAGE + 1];
static size_t incoming_used;
static int incoming_type;
static uint64_t last_rx, last_ping, deadline;

static uint64_t now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void copy(char *out, size_t n, const char *value) {
  snprintf(out, n, "%s", value ? value : "");
}
static const char *string(cJSON *obj, const char *name) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, name);
  return cJSON_IsString(v) ? v->valuestring : "";
}
static int number(cJSON *obj, const char *name, int fallback) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, name);
  return cJSON_IsNumber(v) ? v->valueint : fallback;
}
static int equal(cJSON *obj, const char *name, const char *value) {
  return !strcmp(string(obj, name), value);
}
static void state(const char *value, const char *message) {
  pthread_mutex_lock(&A.mutex);
  copy(A.state, sizeof A.state, value);
  copy(A.message, sizeof A.message, message);
  pthread_mutex_unlock(&A.mutex);
}
static int cancelled(void) {
  pthread_mutex_lock(&A.mutex);
  int v = !A.running || A.cancel;
  pthread_mutex_unlock(&A.mutex);
  return v;
}
static int progress(void *p, curl_off_t a, curl_off_t b, curl_off_t c,
                    curl_off_t d) {
  (void)p;
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  return cancelled();
}
static cJSON *read_json(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  char buf[16385];
  size_t n = fread(buf, 1, sizeof buf, f);
  fclose(f);
  if (n >= sizeof buf)
    return NULL;
  buf[n] = 0;
  cJSON *json = cJSON_ParseWithLength(buf, n + 1);
  explicit_bzero(buf, sizeof buf);
  return json;
}
static int save_config(void) {
  cJSON *j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "ota_url", ota_url);
  cJSON_AddStringToObject(j, "websocket_url", ws_url);
  cJSON_AddStringToObject(j, "token", token);
  cJSON_AddNumberToObject(j, "protocol_version", protocol_version);
  cJSON_AddStringToObject(j, "ca_bundle", ca_bundle);
  char *data = cJSON_PrintUnformatted(j);
  cJSON_Delete(j);
  if (!data)
    return -1;
  int fd = open(CONFIG ".tmp", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600),
      result = -1;
  size_t len = strlen(data), done = 0;
  if (fd >= 0) {
    fchmod(fd, 0600);
    while (done < len) {
      ssize_t n = write(fd, data + done, len - done);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        break;
      done += n;
    }
    if (done == len && !fsync(fd))
      result = 0;
    close(fd);
    if (!result)
      result = rename(CONFIG ".tmp", CONFIG);
  }
  explicit_bzero(data, len);
  free(data);
  return result;
}
static void load_config(void) {
  copy(ota_url, sizeof ota_url, OTA);
  ws_url[0] = token[0] = 0;
  protocol_version = 1;
  copy(ca_bundle, sizeof ca_bundle, "/etc/ssl/certs/ca-certificates.crt");
  cJSON *j = read_json(CONFIG);
  if (j) {
    if (*string(j, "ota_url"))
      copy(ota_url, sizeof ota_url, string(j, "ota_url"));
    copy(ws_url, sizeof ws_url, string(j, "websocket_url"));
    copy(token, sizeof token, string(j, "token"));
    protocol_version = number(j, "protocol_version", 1);
    if (*string(j, "ca_bundle"))
      copy(ca_bundle, sizeof ca_bundle, string(j, "ca_bundle"));
    cJSON_Delete(j);
    chmod(CONFIG, 0600);
  }
}
static int read_line(const char *path, char *out, size_t size) {
  FILE *f = fopen(path, "r");
  if (!f)
    return -1;
  char *p = fgets(out, size, f);
  fclose(f);
  if (!p)
    return -1;
  out[strcspn(out, "\r\n")] = 0;
  return 0;
}
static void identity(void) {
  read_line("/sys/class/net/wlan0/address", device_id, sizeof device_id);
  if (read_line(ID_FILE, client_id, sizeof client_id) ||
      strlen(client_id) != 36) {
    read_line("/proc/sys/kernel/random/uuid", client_id, sizeof client_id);
    int fd = open(ID_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
      write(fd, client_id, strlen(client_id));
      fsync(fd);
      close(fd);
    }
  }
}
static int secure_url(const char *url, const char *scheme) {
  CURLU *u = curl_url();
  if (!u)
    return 0;
  char *s = NULL, *host = NULL, *user = NULL;
  int ok = !curl_url_set(u, CURLUPART_URL, url, 0) &&
           !curl_url_get(u, CURLUPART_SCHEME, &s, 0) && !strcmp(s, scheme) &&
           !curl_url_get(u, CURLUPART_HOST, &host, 0) && *host &&
           curl_url_get(u, CURLUPART_USER, &user, 0) == CURLUE_NO_USER;
  curl_free(s);
  curl_free(host);
  curl_free(user);
  curl_url_cleanup(u);
  return ok;
}
static int safe_header(const char *s) {
  return !strchr(s, '\r') && !strchr(s, '\n');
}
static struct curl_slist *headers(int websocket) {
  struct curl_slist *h = NULL;
  char line[2200];
  snprintf(line, sizeof line, "Device-Id: %s", device_id);
  h = curl_slist_append(h, line);
  snprintf(line, sizeof line, "Client-Id: %s", client_id);
  h = curl_slist_append(h, line);
  h = curl_slist_append(h, "Accept-Language: zh-CN");
  h = curl_slist_append(h, websocket ? "Protocol-Version: 1"
                                     : "Activation-Version: 1");
  if (websocket && *token) {
    snprintf(line, sizeof line, "Authorization: %s%s",
             strchr(token, ' ') ? "" : "Bearer ", token);
    h = curl_slist_append(h, line);
    explicit_bzero(line, sizeof line);
  }
  if (!websocket)
    h = curl_slist_append(h, "Content-Type: application/json");
  return h;
}
static CURL *http_init(const char *url, struct curl_slist *h) {
  CURL *c = curl_easy_init();
  if (!c)
    return NULL;
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "sogou-e1-linux/1.0.0");
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 12L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_PROXY, "");
  curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https,wss");
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(c, CURLOPT_CAINFO, ca_bundle);
  curl_easy_setopt(c, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
  curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress);
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  return c;
}
typedef struct {
  char bytes[65536];
  size_t used;
} Body;
static size_t receive_body(char *p, size_t size, size_t count, void *data) {
  Body *b = data;
  size_t n = size * count;
  if (n > sizeof b->bytes - b->used - 1)
    return 0;
  memcpy(b->bytes + b->used, p, n);
  b->used += n;
  b->bytes[b->used] = 0;
  return n;
}
static cJSON *post_json(const char *url, const char *data, long *status) {
  struct curl_slist *h = headers(0);
  CURL *c = http_init(url, h);
  Body *body = calloc(1, sizeof *body);
  cJSON *j = NULL;
  *status = 0;
  if (c && body) {
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, data);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, receive_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, body);
    CURLcode e = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, status);
    if (!e) {
      j = cJSON_ParseWithLength(body->bytes, body->used + 1);
    } else if (!cancelled()) {
      char msg[240];
      snprintf(msg, sizeof msg, "HTTPS: %s", curl_easy_strerror(e));
      state("error", msg);
    }
  }
  if (c)
    curl_easy_cleanup(c);
  curl_slist_free_all(h);
  if (body) {
    explicit_bzero(body, sizeof *body);
    free(body);
  }
  return j;
}
static int provision(void) {
  if (!secure_url(ota_url, "https")) {
    state("error", "Activation URL must use HTTPS");
    return -1;
  }
  state("activating", "Checking official service…");
  pthread_mutex_lock(&A.mutex);
  int pending = *A.code;
  pthread_mutex_unlock(&A.mutex);
  if (pending) {
    char url[1100];
    snprintf(url, sizeof url, "%s%sactivate", ota_url,
             ota_url[strlen(ota_url) - 1] == '/' ? "" : "/");
    long status = 0;
    cJSON *reply = post_json(url, "{}", &status);
    cJSON_Delete(reply);
    if (status == 202) {
      state("activation",
            "Enter this code at xiaozhi.me, then press M to check");
      return 1;
    }
    if (status != 200) {
      state("activation",
            "Activation is pending. Add the code at xiaozhi.me, then press M");
      return 1;
    }
  }
  cJSON *j = cJSON_CreateObject();
  cJSON_AddNumberToObject(j, "version", 2);
  cJSON_AddStringToObject(j, "language", "zh-CN");
  cJSON_AddStringToObject(j, "mac_address", device_id);
  cJSON_AddStringToObject(j, "uuid", client_id);
  cJSON_AddStringToObject(j, "chip_model_name", "rk3308");
  cJSON *app = cJSON_AddObjectToObject(j, "application");
  cJSON_AddStringToObject(app, "name", "sogou-e1-linux");
  cJSON_AddStringToObject(app, "version", "1.0.0");
  cJSON *board = cJSON_AddObjectToObject(j, "board");
  cJSON_AddStringToObject(board, "type", "sogou-e1-linux");
  cJSON_AddStringToObject(board, "name", "Sogou E1");
  cJSON_AddStringToObject(board, "mac", device_id);
  char *data = cJSON_PrintUnformatted(j);
  cJSON_Delete(j);
  long status = 0;
  j = post_json(ota_url, data, &status);
  free(data);
  if (!j || status != 200) {
    cJSON_Delete(j);
    if (status) {
      char msg[128];
      snprintf(msg, sizeof msg, "Activation service returned HTTP %ld", status);
      state("error", msg);
    }
    return -1;
  }
  cJSON *activation = cJSON_GetObjectItemCaseSensitive(j, "activation");
  const char *code = string(activation, "code");
  if (*code) {
    pthread_mutex_lock(&A.mutex);
    copy(A.code, sizeof A.code, code);
    pthread_mutex_unlock(&A.mutex);
    cJSON_Delete(j);
    state("activation", "Enter this code at xiaozhi.me, then press M to check");
    return 1;
  }
  cJSON *settings = cJSON_GetObjectItemCaseSensitive(j, "websocket");
  if (!secure_url(string(settings, "url"), "wss") ||
      !safe_header(string(settings, "token")) ||
      strlen(string(settings, "token")) >= sizeof token ||
      strlen(string(settings, "url")) >= sizeof ws_url ||
      number(settings, "version", 1) != 1) {
    cJSON_Delete(j);
    state("error", "Service did not provide compatible WebSocket v1 settings");
    return -1;
  }
  copy(ws_url, sizeof ws_url, string(settings, "url"));
  copy(token, sizeof token, string(settings, "token"));
  protocol_version = 1;
  cJSON_Delete(j);
  if (save_config()) {
    state("error", "Cannot save private connection settings");
    return -1;
  }
  pthread_mutex_lock(&A.mutex);
  A.code[0] = 0;
  pthread_mutex_unlock(&A.mutex);
  state("idle", "Activated. Press REC to talk");
  return 0;
}
static int ws_send(const void *bytes, size_t size, int flags) {
  size_t done = 0;
  uint64_t end = now_ms() + 2000;
  do {
    size_t n = 0;
    CURLcode e =
        curl_ws_send(ws, (const char *)bytes + done, size - done, &n, 0, flags);
    done += n;
    if (e != CURLE_OK && e != CURLE_AGAIN)
      return -1;
    if (done == size)
      return 0;
    if (cancelled() || now_ms() > end)
      return -1;
    usleep(2000);
  } while (1);
}
static int send_json(cJSON *j) {
  char *data = cJSON_PrintUnformatted(j);
  cJSON_Delete(j);
  if (!data)
    return -1;
  int e = ws_send(data, strlen(data), CURLWS_TEXT);
  free(data);
  return e;
}
static int listen_message(const char *action) {
  cJSON *j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "session_id", session);
  cJSON_AddStringToObject(j, "type", "listen");
  cJSON_AddStringToObject(j, "state", action);
  if (!strcmp(action, "start"))
    cJSON_AddStringToObject(j, "mode", "manual");
  return send_json(j);
}
static void close_audio(void) {
  if (mic) {
    snd_pcm_drop(mic);
    snd_pcm_close(mic);
    mic = NULL;
  }
  if (speaker) {
    snd_pcm_drop(speaker);
    snd_pcm_close(speaker);
    speaker = NULL;
  }
  if (encoder) {
    opus_encoder_destroy(encoder);
    encoder = NULL;
  }
  if (decoder) {
    opus_decoder_destroy(decoder);
    decoder = NULL;
  }
  if (resampler) {
    src_delete(resampler);
    resampler = NULL;
  }
  if (output_resampler) {
    src_delete(output_resampler);
    output_resampler = NULL;
  }
  frame_used = qhead = qcount = tts_started = tts_stopped = 0;
  pthread_mutex_lock(&A.mutex);
  A.level = -96;
  pthread_mutex_unlock(&A.mutex);
}
static int open_input(void) {
  int error = 0;
  encoder = opus_encoder_create(VOICE_RATE, 1, OPUS_APPLICATION_VOIP, &error);
  if (!encoder)
    return -1;
  opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000));
  opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(3));
  resampler = src_new(SRC_SINC_FASTEST, 1, &error);
  if (!resampler)
    return -1;
  error =
      snd_pcm_open(&mic, "hw:2,0", SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
  if (error >= 0)
    error = snd_pcm_set_params(mic, SND_PCM_FORMAT_S32_LE,
                               SND_PCM_ACCESS_RW_INTERLEAVED, 2, INPUT_RATE, 0,
                               100000);
  if (error < 0)
    return -1;
  /* Nonblocking reads smaller than the configured start threshold otherwise
   * remain EAGAIN forever on the E1's capture driver. */
  return snd_pcm_start(mic) < 0 ? -1 : 0;
}
static int capture_tick(void) {
  int32_t pcm[441 * 2];
  float mono[441];
  snd_pcm_sframes_t n = snd_pcm_readi(mic, pcm, 441);
  if (n == -EAGAIN)
    return 0;
  if (n < 0) {
    if (snd_pcm_recover(mic, n, 1) < 0)
      return -1;
    return snd_pcm_state(mic) == SND_PCM_STATE_PREPARED &&
                   snd_pcm_start(mic) < 0
               ? -1
               : 0;
  }
  double peak = 0;
  for (int i = 0; i < n; i++) {
    mono[i] = (float)(((double)pcm[2 * i] + pcm[2 * i + 1]) / 4294967296.0);
    peak = fmax(peak, fabs(mono[i]));
  }
  pthread_mutex_lock(&A.mutex);
  A.level = peak > 0 ? fmax(-96, 20 * log10(peak)) : -96;
  pthread_mutex_unlock(&A.mutex);
  SRC_DATA d = {.data_in = mono,
                .input_frames = n,
                .data_out = resampled,
                .output_frames = 256,
                .src_ratio = (double)VOICE_RATE / INPUT_RATE};
  if (src_process(resampler, &d) || d.input_frames_used != n)
    return -1;
  for (int i = 0; i < d.output_frames_gen; i++) {
    input_frame[frame_used++] =
        (int16_t)lrintf(fmaxf(-1, fminf(0.999969f, resampled[i])) * 32768);
    if (frame_used == VOICE_FRAME) {
      unsigned char out[MAX_PACKET];
      int size =
          opus_encode(encoder, input_frame, VOICE_FRAME, out, sizeof out);
      frame_used = 0;
      if (size < 0 || ws_send(out, size, CURLWS_BINARY))
        return -1;
      pthread_mutex_lock(&A.mutex);
      A.sent++;
      pthread_mutex_unlock(&A.mutex);
    }
  }
  return 0;
}
static int stop_listening(void) {
  if (mic) {
    snd_pcm_drop(mic);
    snd_pcm_close(mic);
    mic = NULL;
  }
  if (frame_used) {
    memset(input_frame + frame_used, 0,
           (VOICE_FRAME - frame_used) * sizeof *input_frame);
    unsigned char out[MAX_PACKET];
    int n = opus_encode(encoder, input_frame, VOICE_FRAME, out, sizeof out);
    frame_used = 0;
    if (n < 0 || ws_send(out, n, CURLWS_BINARY))
      return -1;
    pthread_mutex_lock(&A.mutex);
    A.sent++;
    pthread_mutex_unlock(&A.mutex);
  }
  pthread_mutex_lock(&A.mutex);
  A.level = -96;
  pthread_mutex_unlock(&A.mutex);
  if (listen_message("stop"))
    return -1;
  state("thinking", "Waiting for reply…");
  deadline = now_ms() + 60000;
  return 0;
}
static int start_output(void) {
  if (mic) {
    snd_pcm_drop(mic);
    snd_pcm_close(mic);
    mic = NULL;
  }
  int error = 0;
  decoder = opus_decoder_create(server_rate, 1, &error);
  if (!decoder)
    return -1;
  output_resampler = src_new(SRC_SINC_FASTEST, 1, &error);
  if (!output_resampler)
    return -1;
  error = snd_pcm_open(&speaker, "default", SND_PCM_STREAM_PLAYBACK,
                       SND_PCM_NONBLOCK);
  if (error >= 0)
    error = snd_pcm_set_params(speaker, SND_PCM_FORMAT_S24_LE,
                               SND_PCM_ACCESS_RW_INTERLEAVED, 2, INPUT_RATE, 1,
                               100000);
  if (error < 0)
    return -1;
  tts_started = 1;
  state("speaking", "");
  deadline = now_ms() + 120000;
  return 0;
}
/* Nonblocking ALSA output: the worker services control/socket traffic between
 * writes. */
static int32_t output_pcm[6000 * 2];
static int output_count, output_offset;
static int playback_tick(void) {
  if (!speaker)
    return 0;
  if (output_offset == output_count && qcount) {
    int16_t mono[5760];
    Packet *p = &queue[qhead];
    int n = opus_decode(decoder, p->bytes, p->size, mono, 5760, 0);
    if (n < 0)
      return -1;
    qhead = (qhead + 1) % QUEUE;
    qcount--;
    int volume;
    pthread_mutex_lock(&A.mutex);
    volume = A.volume;
    pthread_mutex_unlock(&A.mutex);
    float input[5760], output[6000];
    for (int i = 0; i < n; i++)
      input[i] = mono[i] / 32768.0f;
    SRC_DATA data = {.data_in = input,
                     .input_frames = n,
                     .data_out = output,
                     .output_frames = 6000,
                     .src_ratio = (double)INPUT_RATE / server_rate};
    if (src_process(output_resampler, &data) || data.input_frames_used != n)
      return -1;
    for (int i = 0; i < data.output_frames_gen; i++)
      output_pcm[i * 2] = output_pcm[i * 2 + 1] = (int32_t)lrintf(
          fmaxf(-1, fminf(0.999999f, output[i])) * 8388607 * volume / 100);
    output_offset = 0;
    output_count = data.output_frames_gen;
  }
  if (output_offset < output_count) {
    snd_pcm_sframes_t n = snd_pcm_writei(
        speaker, output_pcm + output_offset * 2, output_count - output_offset);
    if (n > 0)
      output_offset += n;
    else if (n != -EAGAIN && snd_pcm_recover(speaker, n, 1) < 0)
      return -1;
  }
  if (output_count && output_offset == output_count) {
    pthread_mutex_lock(&A.mutex);
    A.played++;
    pthread_mutex_unlock(&A.mutex);
    output_count = output_offset = 0;
  }
  return 0;
}
static int output_done(void) {
  if (qcount || output_count != output_offset)
    return 0;
  snd_pcm_sframes_t delay = 0;
  if (speaker)
    snd_pcm_delay(speaker, &delay);
  return delay <= 0;
}
static int mcp(cJSON *root) {
  cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload"),
        *id = cJSON_GetObjectItemCaseSensitive(payload, "id");
  if (!id || (!cJSON_IsString(id) && !cJSON_IsNumber(id)))
    return 0;
  const char *method = string(payload, "method");
  cJSON *result = NULL;
  int error = 0;
  if (!strcmp(method, "initialize")) {
    result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");
    cJSON *caps = cJSON_AddObjectToObject(result, "capabilities");
    cJSON_AddObjectToObject(caps, "tools");
    cJSON *info = cJSON_AddObjectToObject(result, "serverInfo");
    cJSON_AddStringToObject(info, "name", "sogou-e1-linux");
    cJSON_AddStringToObject(info, "version", "1.0.0");
  } else if (!strcmp(method, "tools/list")) {
    result =
        cJSON_Parse("{\"tools\":[{\"name\":\"self.get_device_status\","
                    "\"description\":\"Get Sogou E1 voice state and speaker "
                    "volume\",\"inputSchema\":{\"type\":\"object\","
                    "\"properties\":{}}},{\"name\":\"self.audio_speaker.set_"
                    "volume\",\"description\":\"Set speaker volume from 0 to "
                    "100\",\"inputSchema\":{\"type\":\"object\",\"properties\":"
                    "{\"volume\":{\"type\":\"integer\",\"minimum\":0,"
                    "\"maximum\":100}},\"required\":[\"volume\"]}}]}");
  } else if (!strcmp(method, "tools/call")) {
    cJSON *params = cJSON_GetObjectItemCaseSensitive(payload, "params");
    const char *name = string(params, "name");
    char text[256];
    if (!strcmp(name, "self.get_device_status")) {
      pthread_mutex_lock(&A.mutex);
      snprintf(text, sizeof text,
               "{\"model\":\"Sogou "
               "E1\",\"state\":\"%s\",\"speaker\":{\"volume\":%d}}",
               A.state, A.volume);
      pthread_mutex_unlock(&A.mutex);
    } else if (!strcmp(name, "self.audio_speaker.set_volume")) {
      cJSON *v = cJSON_GetObjectItemCaseSensitive(
          cJSON_GetObjectItemCaseSensitive(params, "arguments"), "volume");
      if (!cJSON_IsNumber(v) || v->valuedouble != v->valueint ||
          v->valueint < 0 || v->valueint > 100)
        error = -32602;
      else {
        pthread_mutex_lock(&A.mutex);
        A.volume = v->valueint;
        pthread_mutex_unlock(&A.mutex);
      }
      copy(text, sizeof text, "true");
    } else
      error = -32602;
    if (!error) {
      result = cJSON_CreateObject();
      cJSON *content = cJSON_AddArrayToObject(result, "content"),
            *entry = cJSON_CreateObject();
      cJSON_AddItemToArray(content, entry);
      cJSON_AddStringToObject(entry, "type", "text");
      cJSON_AddStringToObject(entry, "text", text);
      cJSON_AddBoolToObject(result, "isError", 0);
    }
  } else
    error = -32601;
  cJSON *reply = cJSON_CreateObject();
  cJSON_AddStringToObject(reply, "type", "mcp");
  cJSON_AddStringToObject(reply, "session_id", session);
  cJSON *rpc = cJSON_AddObjectToObject(reply, "payload");
  cJSON_AddStringToObject(rpc, "jsonrpc", "2.0");
  cJSON_AddItemToObject(rpc, "id", cJSON_Duplicate(id, 1));
  if (error) {
    cJSON *e = cJSON_AddObjectToObject(rpc, "error");
    cJSON_AddNumberToObject(e, "code", error);
    cJSON_AddStringToObject(e, "message",
                            error == -32601 ? "Method not found"
                                            : "Invalid tool or arguments");
  } else
    cJSON_AddItemToObject(rpc, "result", result);
  return send_json(reply);
}
static int on_json(const char *data, size_t size, int *hello) {
  cJSON *j = cJSON_ParseWithLength(data, size);
  if (!cJSON_IsObject(j)) {
    cJSON_Delete(j);
    return -1;
  }
  int result = 0;
  if (equal(j, "type", "hello")) {
    cJSON *params = cJSON_GetObjectItemCaseSensitive(j, "audio_params");
    int rate = number(params, "sample_rate", 24000);
    if (*hello || !equal(j, "transport", "websocket") ||
        strlen(string(j, "session_id")) >= sizeof session ||
        (rate != 8000 && rate != 12000 && rate != 16000 && rate != 24000 &&
         rate != 48000) ||
        (*string(params, "format") && !equal(params, "format", "opus")) ||
        number(params, "channels", 1) != 1)
      result = -1;
    else {
      server_rate = rate;
      copy(session, sizeof session, string(j, "session_id"));
      *hello = 1;
    }
  } else if (!*hello)
    result = -1;
  else if (*string(j, "session_id") &&
           strcmp(string(j, "session_id"),
                  session)) { /* Ignore stale sessions. */
  } else if (equal(j, "type", "stt")) {
    pthread_mutex_lock(&A.mutex);
    copy(A.user, sizeof A.user, string(j, "text"));
    pthread_mutex_unlock(&A.mutex);
  } else if (equal(j, "type", "llm")) {
    pthread_mutex_lock(&A.mutex);
    copy(A.emotion, sizeof A.emotion, string(j, "emotion"));
    pthread_mutex_unlock(&A.mutex);
  } else if (equal(j, "type", "tts")) {
    if (equal(j, "state", "start")) {
      if (tts_started)
        result = -1;
      else
        result = start_output();
    } else if (equal(j, "state", "stop")) {
      tts_stopped = 1;
    } else if (equal(j, "state", "sentence_start")) {
      pthread_mutex_lock(&A.mutex);
      copy(A.reply, sizeof A.reply, string(j, "text"));
      pthread_mutex_unlock(&A.mutex);
    }
  } else if (equal(j, "type", "mcp"))
    result = mcp(j);
  else if (equal(j, "type", "alert")) {
    pthread_mutex_lock(&A.mutex);
    copy(A.message, sizeof A.message, string(j, "message"));
    pthread_mutex_unlock(&A.mutex);
  }
  /* Remote system/reboot/OTA commands intentionally have no Linux
   * implementation. */
  cJSON_Delete(j);
  return result;
}
static int receive_tick(int *hello) {
  /* Drain at most 16 frames per tick so keys/audio/cancellation cannot starve.
   */
  for (int i = 0; i < 16; i++) {
    unsigned char chunk[4096];
    size_t n = 0;
    const struct curl_ws_frame *meta = NULL;
    CURLcode e = curl_ws_recv(ws, chunk, sizeof chunk, &n, &meta);
    if (e == CURLE_AGAIN)
      return 0;
    if (e != CURLE_OK)
      return -1;
    last_rx = now_ms();
    if (meta->flags & CURLWS_CLOSE)
      return -1;
    if (meta->flags & (CURLWS_PING | CURLWS_PONG))
      continue; /* libcurl automatically replies to ping. */
    int type = meta->flags & (CURLWS_TEXT | CURLWS_BINARY);
    if (!type || ((incoming_used > 0) && type != incoming_type) ||
        n > MAX_MESSAGE - incoming_used ||
        meta->bytesleft > (curl_off_t)(MAX_MESSAGE - incoming_used - n))
      return -1;
    incoming_type = type;
    memcpy(incoming + incoming_used, chunk, n);
    incoming_used += n;
    if (meta->bytesleft || (meta->flags & CURLWS_CONT))
      continue;
    int result = 0;
    if (type == CURLWS_TEXT) {
      incoming[incoming_used] = 0;
      result = on_json((char *)incoming, incoming_used + 1, hello);
    } else {
      if (!*hello || !tts_started || tts_stopped || !incoming_used ||
          incoming_used > MAX_PACKET || qcount == QUEUE)
        return -1;
      Packet *p = &queue[(qhead + qcount) % QUEUE];
      p->size = incoming_used;
      memcpy(p->bytes, incoming, incoming_used);
      qcount++;
      pthread_mutex_lock(&A.mutex);
      A.received++;
      pthread_mutex_unlock(&A.mutex);
    }
    incoming_used = 0;
    if (result)
      return -1;
  }
  return 0;
}
static int run_session(void) {
  load_config();
  if (!*ws_url && provision())
    return -1;
  if (!secure_url(ws_url, "wss") || !safe_header(token) ||
      protocol_version != 1) {
    state("error", "Need secure WebSocket v1 configuration");
    return -1;
  }
  state("connecting", "Connecting to Xiaozhi…");
  struct curl_slist *h = headers(1);
  ws = http_init(ws_url, h);
  if (!ws) {
    curl_slist_free_all(h);
    return -1;
  }
  curl_easy_setopt(ws, CURLOPT_CONNECT_ONLY, 2L);
  CURLcode e = curl_easy_perform(ws);
  if (e != CURLE_OK) {
    if (!cancelled()) {
      char msg[240];
      snprintf(msg, sizeof msg,
               "Voice connection: %s. M checks activation again.",
               curl_easy_strerror(e));
      state("error", msg);
    }
    curl_easy_cleanup(ws);
    ws = NULL;
    curl_slist_free_all(h);
    return -1;
  }
  int result = 0, hello = 0;
  session[0] = 0;
  incoming_used = 0;
  output_count = output_offset = 0;
  cJSON *j = cJSON_Parse(
      "{\"type\":\"hello\",\"version\":1,\"features\":{\"mcp\":true},"
      "\"transport\":\"websocket\",\"audio_params\":{\"format\":\"opus\","
      "\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60}}");
  if (send_json(j)) {
    result = -1;
    goto end;
  }
  deadline = now_ms() + 10000;
  last_rx = last_ping = now_ms();
  while (!cancelled()) {
    if (receive_tick(&hello)) {
      result = -1;
      break;
    }
    if (hello && !encoder && !tts_started) {
      if (open_input() || listen_message("start")) {
        result = -1;
        break;
      }
      state("listening", "Speak, then press STOP to send");
      deadline = now_ms() + 120000;
    }
    int finish = 0;
    pthread_mutex_lock(&A.mutex);
    if (mic && A.finish) {
      finish = 1;
      A.finish = 0;
    }
    pthread_mutex_unlock(&A.mutex);
    if (finish && stop_listening()) {
      result = -1;
      break;
    }
    if (mic && capture_tick()) {
      result = -1;
      break;
    }
    if (playback_tick()) {
      result = -1;
      break;
    }
    if (tts_stopped && output_done()) {
      state("idle", "Reply finished. REC talks again");
      break;
    }
    if (now_ms() > deadline || now_ms() - last_rx > 120000) {
      state("error", "Voice session timed out. REC retries");
      result = -2;
      break;
    }
    if (now_ms() - last_ping > 30000) {
      if (ws_send("e1", 2, CURLWS_PING)) {
        result = -1;
        break;
      }
      last_ping = now_ms();
    }
    usleep(5000);
  }
end:
  close_audio();
  /* Closing the channel is the reliable abort; do not wait for network on
   * cancel. */
  curl_easy_cleanup(ws);
  ws = NULL;
  curl_slist_free_all(h);
  if (cancelled())
    state("idle", "Session cancelled");
  else if (result == -1)
    state("error",
          "Voice connection or audio failed. REC retries; M checks activation");
  return result;
}
static void *worker(void *unused) {
  (void)unused;
  identity();
  load_config();
  while (1) {
    pthread_mutex_lock(&A.mutex);
    int running = A.running, job = A.job;
    A.job = 0;
    pthread_mutex_unlock(&A.mutex);
    if (!running)
      break;
    if (job) {
      if (job == 1) {
        run_session();
        close_audio();
      } else {
        load_config();
        provision();
      }
      if (cancelled())
        state("idle", "Session cancelled");
      pthread_mutex_lock(&A.mutex);
      A.active = 0;
      pthread_mutex_unlock(&A.mutex);
    }
    usleep(20000);
  }
  close_audio();
  explicit_bzero(token, sizeof token);
  return NULL;
}
int xiaozhi_start(void) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  pthread_mutex_lock(&A.mutex);
  A.running = 1;
  A.cancel = 0;
  A.job = 0;
  A.active = 0;
  pthread_mutex_unlock(&A.mutex);
  A.started = pthread_create(&A.thread, NULL, worker, NULL) == 0;
  return A.started ? 0 : -1;
}
void xiaozhi_stop(void) {
  pthread_mutex_lock(&A.mutex);
  A.running = 0;
  A.cancel = 1;
  A.job = 0;
  pthread_mutex_unlock(&A.mutex);
  if (A.started)
    pthread_join(A.thread, NULL);
  A.started = 0;
  curl_global_cleanup();
}
int xiaozhi_begin(void) {
  pthread_mutex_lock(&A.mutex);
  if (!A.started) {
    copy(A.state, sizeof A.state, "error");
    copy(A.message, sizeof A.message, "Voice worker unavailable");
    pthread_mutex_unlock(&A.mutex);
    return 0;
  }
  A.active = 1;
  A.cancel = A.finish = 0;
  A.job = 1;
  A.sent = A.received = A.played = 0;
  A.user[0] = A.reply[0] = A.emotion[0] = 0;
  copy(A.state, sizeof A.state, "connecting");
  A.message[0] = 0;
  pthread_mutex_unlock(&A.mutex);
  return 1;
}
int xiaozhi_active(void) {
  pthread_mutex_lock(&A.mutex);
  int v = A.active;
  pthread_mutex_unlock(&A.mutex);
  return v;
}
void xiaozhi_cancel(void) {
  pthread_mutex_lock(&A.mutex);
  A.cancel = 1;
  pthread_mutex_unlock(&A.mutex);
}
int xiaozhi_submit(const char *action, const char **error) {
  int status = 200;
  *error = NULL;
  pthread_mutex_lock(&A.mutex);
  if (!A.started) {
    status = 503;
    *error = "Voice worker unavailable";
  } else if (!strcmp(action, "activate")) {
    if (A.active) {
      status = 409;
      *error = "Finish the current voice operation first";
    } else {
      A.cancel = A.finish = 0;
      A.active = 1;
      A.job = 2;
      copy(A.state, sizeof A.state, "activating");
      A.message[0] = 0;
    }
  } else if (!strcmp(action, "finish")) {
    if (strcmp(A.state, "listening")) {
      status = 409;
      *error = "Voice is not listening";
    } else
      A.finish = 1;
  } else if (!strcmp(action, "cancel"))
    A.cancel = 1;
  else if (!strcmp(action, "volume-up")) {
    A.volume += 5;
    if (A.volume > 100)
      A.volume = 100;
  } else if (!strcmp(action, "volume-down")) {
    A.volume -= 5;
    if (A.volume < 0)
      A.volume = 0;
  } else {
    status = 404;
    *error = "Unknown voice action";
  }
  pthread_mutex_unlock(&A.mutex);
  return status;
}
char *xiaozhi_json(void) {
  cJSON *j = cJSON_CreateObject();
  pthread_mutex_lock(&A.mutex);
  cJSON_AddBoolToObject(j, "available", A.started);
  cJSON_AddBoolToObject(j, "active", A.active);
  cJSON_AddStringToObject(j, "state", A.state);
  cJSON_AddStringToObject(j, "message", A.message);
  cJSON_AddStringToObject(j, "activationCode", A.code);
  cJSON_AddStringToObject(j, "userText", A.user);
  cJSON_AddStringToObject(j, "assistantText", A.reply);
  cJSON_AddStringToObject(j, "emotion", A.emotion);
  cJSON_AddNumberToObject(j, "sentPackets", A.sent);
  cJSON_AddNumberToObject(j, "receivedPackets", A.received);
  cJSON_AddNumberToObject(j, "playedPackets", A.played);
  cJSON_AddNumberToObject(j, "levelDb", A.level);
  cJSON_AddNumberToObject(j, "volume", A.volume);
  pthread_mutex_unlock(&A.mutex);
  char *out = cJSON_PrintUnformatted(j);
  cJSON_Delete(j);
  return out;
}

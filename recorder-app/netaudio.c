#define _GNU_SOURCE
#include "netaudio.h"
#include "rtp_audio.h"
#include <alsa/asoundlib.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CONTROL_PORT 18772
#define AUDIO_PORT 18774
#define RTCP_PORT 18775
static struct {
  pthread_mutex_t lock;
  pthread_t thread;
  int running, armed, busy, connected;
  char state[32], message[160], peer[INET_ADDRSTRLEN];
  uint64_t sent, received, recovered, concealed, invalid, send_drops, xruns,
      played, rtcp;
  double input[2], output[2], jitter, queue, ppm;
  unsigned buffer, red_distance;
  unsigned capture_buffer, capture_period, playback_buffer, playback_period;
} N = {.lock = PTHREAD_MUTEX_INITIALIZER,
       .state = "off",
       .message = "REC enables Wi-Fi Audio",
       .input = {-96, -96},
       .output = {-96, -96}};
static uint64_t now_us(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static int enabled(void) {
  pthread_mutex_lock(&N.lock);
  int result = N.running && N.armed;
  pthread_mutex_unlock(&N.lock);
  return result;
}
/* WEXT's generic power switch is a no-op in this vendor driver. Its private
 * pm_set ioctl controls real LPS/IPS state. Preserve the current runtime modes.
 */
static int radio_mode(const char *key) {
  FILE *f = fopen("/proc/net/rtl8723ds/wlan0/ps_info", "r");
  if (!f)
    return -1;
  char line[160];
  int mode = -1;
  while (fgets(line, sizeof line, f)) {
    char *p = strstr(line, key);
    if (!p)
      continue;
    if (strstr(p, "NO LPS") || strstr(p, "NO IPS"))
      mode = 0;
    else if (strstr(p, "MIN") || strstr(p, "Card Disable"))
      mode = 1;
    else if (strstr(p, "MAX") || strstr(p, "Level 2"))
      mode = 2;
    else if (strstr(p, "DTIM"))
      mode = 3;
  }
  fclose(f);
  return mode;
}
static int radio_set(const char *key, int mode) {
  if (mode < 0 || mode > 3)
    return -1;
  char argument[24];
  snprintf(argument, sizeof argument, "%s=%d", key, mode);
  pid_t pid = fork();
  if (!pid) {
    execl("/sbin/iwpriv", "iwpriv", "wlan0", "pm_set", argument, (char *)NULL);
    _exit(127);
  }
  if (pid < 0)
    return -1;
  int status;
  while (waitpid(pid, &status, 0) < 0)
    if (errno != EINTR)
      return -1;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}
static void state(const char *s, const char *message) {
  pthread_mutex_lock(&N.lock);
  snprintf(N.state, sizeof N.state, "%s", s);
  snprintf(N.message, sizeof N.message, "%s", message);
  pthread_mutex_unlock(&N.lock);
}
static int make_socket(int type, unsigned port) {
  int fd = socket(AF_INET, type | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0)
    return -1;
  int one = 1, buffer = 256 * 1024;
  if (type == SOCK_DGRAM) {
    int tos =
        0xb8; /* DSCP EF: request interactive audio service where honored. */
    setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
  }
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof buffer);
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof buffer);
  struct sockaddr_in address = {.sin_family = AF_INET,
                                .sin_port = htons(port),
                                .sin_addr.s_addr = htonl(INADDR_ANY)};
  if (bind(fd, (void *)&address, sizeof address)) {
    close(fd);
    return -1;
  }
  return fd;
}
static void pcm_close(snd_pcm_t **pcm) {
  if (*pcm) {
    snd_pcm_drop(*pcm);
    snd_pcm_close(*pcm);
    *pcm = NULL;
  }
}
static int pcm_open(snd_pcm_t **pcm, const char *device,
                    snd_pcm_stream_t stream, snd_pcm_format_t format,
                    unsigned latency_us) {
  unsigned channels = stream == SND_PCM_STREAM_CAPTURE ? E1_CAPTURE_CHANNELS
                                                       : E1_STEREO_CHANNELS;
  int error = snd_pcm_open(pcm, device, stream, SND_PCM_NONBLOCK);
  if (error >= 0 && latency_us <= 10000) {
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_uframes_t period = RTP_FRAMES, buffer = RTP_FRAMES * 4;
    int direction = 0;
    if ((error = snd_pcm_hw_params_any(*pcm, hw)) >= 0 &&
        (error = snd_pcm_hw_params_set_access(
             *pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) >= 0 &&
        (error = snd_pcm_hw_params_set_format(*pcm, hw, format)) >= 0 &&
        (error = snd_pcm_hw_params_set_channels(*pcm, hw, channels)) >= 0 &&
        (error = snd_pcm_hw_params_set_rate(*pcm, hw, RTP_RATE, 0)) >= 0 &&
        (error = snd_pcm_hw_params_set_period_size_near(*pcm, hw, &period,
                                                        &direction)) >= 0 &&
        (error = snd_pcm_hw_params_set_buffer_size_near(*pcm, hw, &buffer)) >=
            0)
      error = snd_pcm_hw_params(*pcm, hw);
    if (error >= 0) {
      snd_pcm_sw_params_t *sw;
      snd_pcm_sw_params_alloca(&sw);
      if ((error = snd_pcm_sw_params_current(*pcm, sw)) >= 0 &&
          (error = snd_pcm_sw_params_set_avail_min(*pcm, sw, period)) >= 0 &&
          (error = snd_pcm_sw_params_set_start_threshold(
               *pcm, sw,
               stream == SND_PCM_STREAM_CAPTURE ? 1 : buffer - period)) >= 0)
        error = snd_pcm_sw_params(*pcm, sw);
    }
    /* Some ALSA plugins impose their own period sizes; keep a bounded fallback.
     */
    if (error < 0)
      error = snd_pcm_set_params(*pcm, format, SND_PCM_ACCESS_RW_INTERLEAVED,
                                 channels, RTP_RATE, 1, latency_us);
  } else if (error >= 0)
    error = snd_pcm_set_params(*pcm, format, SND_PCM_ACCESS_RW_INTERLEAVED,
                               channels, RTP_RATE, 1, latency_us);
  if (error >= 0 && stream == SND_PCM_STREAM_CAPTURE)
    error = snd_pcm_start(*pcm);
  if (error < 0) {
    pcm_close(pcm);
    return error;
  }
  snd_pcm_uframes_t buffer_frames = 0, period_frames = 0;
  snd_pcm_get_params(*pcm, &buffer_frames, &period_frames);
  pthread_mutex_lock(&N.lock);
  if (stream == SND_PCM_STREAM_CAPTURE) {
    N.capture_buffer = buffer_frames;
    N.capture_period = period_frames;
  } else {
    N.playback_buffer = buffer_frames;
    N.playback_period = period_frames;
  }
  pthread_mutex_unlock(&N.lock);
  fprintf(stderr, "E1_RTP_PCM %s stream=%d buffer=%lu period=%lu\n", device,
          (int)stream, (unsigned long)buffer_frames,
          (unsigned long)period_frames);
  return 0;
}
static int recover_pcm(snd_pcm_t *pcm, int error, int capture) {
  if (error == -EAGAIN)
    return 0;
  pthread_mutex_lock(&N.lock);
  ++N.xruns;
  pthread_mutex_unlock(&N.lock);
  if (snd_pcm_recover(pcm, error, 1) < 0)
    return -1;
  if (capture && snd_pcm_state(pcm) == SND_PCM_STATE_PREPARED &&
      snd_pcm_start(pcm) < 0)
    return -1;
  return 0;
}
static int settle_microphone(snd_pcm_t *mic) {
  /* The PDM stream can contain near-full-scale startup transients. Do not
   * publish those samples to desktop applications when opening a session. */
  int32_t samples[512 * E1_CAPTURE_CHANNELS];
  unsigned frames = 0;
  uint64_t deadline = now_us() + 2000000;
  while (enabled() && frames < RTP_RATE / 4 && now_us() < deadline) {
    snd_pcm_sframes_t n = snd_pcm_readi(mic, samples, 512);
    if (n > 0)
      frames += (unsigned)n;
    else if (n < 0 && recover_pcm(mic, (int)n, 1))
      return -1;
    else
      usleep(1000);
  }
  return enabled() && frames >= RTP_RATE / 4 ? 0 : -1;
}
static int peer_matches(const struct sockaddr_in *a,
                        const struct sockaddr_in *b) {
  return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}
static int line_read(int fd, char *line, size_t cap, uint64_t timeout) {
  size_t n = 0;
  uint64_t until = now_us() + timeout;
  while (enabled() && now_us() < until && n + 1 < cap) {
    char c;
    ssize_t k = recv(fd, &c, 1, 0);
    if (k == 1) {
      if (c == '\n') {
        line[n] = 0;
        return 0;
      }
      if (c == '\r' || c < 32)
        return -1;
      line[n++] = c;
    } else if (k == 0)
      return -1;
    else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
      return -1;
    else
      usleep(1000);
  }
  return -1;
}
static int send_line(int fd, const char *line) {
  size_t len = strlen(line), sent = 0;
  uint64_t deadline = now_us() + 1000000;
  while (sent < len && enabled() && now_us() < deadline) {
    ssize_t n = send(fd, line + sent, len - sent, MSG_NOSIGNAL);
    if (n > 0)
      sent += n;
    else if (n < 0 &&
             (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
      usleep(1000);
    else
      return -1;
  }
  return sent == len ? 0 : -1;
}
static void session(int control, int udp, int rtcp, struct sockaddr_in peer) {
  char line[256], extra;
  unsigned rate, bits, channels, frames, port, rtcp_port, buffer, red;
  uint32_t ssrc;
  if (line_read(control, line, sizeof line, 3000000) ||
      sscanf(line, "E1RTP/1 %u %u %u %u %u %u %u %u %u %c", &rate, &bits,
             &channels, &frames, &port, &rtcp_port, &ssrc, &buffer, &red,
             &extra) != 9 ||
      rate != RTP_RATE || bits != 24 || channels != 2 || frames != RTP_FRAMES ||
      port < 1024 || port > 65535 || rtcp_port < 1024 || rtcp_port > 65535 ||
      port == rtcp_port || buffer < 5 || buffer > 200 || (red && buffer < 10) ||
      red > 1 || !ssrc) {
    send_line(control, "ERROR unsupported session\n");
    return;
  }
  snd_pcm_t *mic = NULL, *speaker = NULL;
  RtpRx *rx = calloc(1, sizeof *rx);
  if (!rx) {
    state("error", "Not enough audio memory");
    goto done;
  }
  uint32_t randoms[3];
  if (getrandom(randoms, sizeof randoms, 0) != (ssize_t)sizeof randoms) {
    state("error", "Cannot create RTP session identity");
    free(rx);
    goto done;
  }
  if (!randoms[0])
    randoms[0] = 1;
  RtpTx tx;
  rtp_tx_init(&tx, randoms[0], randoms[1], randoms[2]);
  rtp_rx_init(rx, ssrc, buffer);
  rtp_tx_set_distance(&tx, buffer < 40 ? 2 : RTP_RED_DISTANCE);
  struct sockaddr_in remote = peer, remote_rtcp = peer;
  remote.sin_port = htons(port);
  remote_rtcp.sin_port = htons(rtcp_port);
  if (connect(udp, (void *)&remote, sizeof remote)) {
    state("error", "Cannot connect RTP peer");
    free(rx);
    goto done;
  }
  snprintf(line, sizeof line, "E1RTP/1 48000 24 2 120 %u %u %u %u\n",
           AUDIO_PORT, RTCP_PORT, tx.ssrc, buffer);
  if (send_line(control, line)) {
    free(rx);
    goto done;
  }
  if (line_read(control, line, sizeof line, 15000000) ||
      strcmp(line, "START")) {
    free(rx);
    goto done;
  }
  int error = pcm_open(&mic, "hw:2,0", SND_PCM_STREAM_CAPTURE,
                       SND_PCM_FORMAT_S32_LE, buffer < 40 ? 10000 : 40000);
  if (error >= 0)
    error = pcm_open(&speaker, "plughw:0,0", SND_PCM_STREAM_PLAYBACK,
                     SND_PCM_FORMAT_S24_LE, buffer < 40 ? 10000 : 40000);
  if (error < 0) {
    char message[160];
    snprintf(message, sizeof message, "Full-duplex audio: %s",
             snd_strerror(error));
    state("error", message);
    send_line(control, "ERROR ALSA full duplex unavailable\n");
    free(rx);
    goto done;
  }
  if (settle_microphone(mic)) {
    if (enabled()) {
      state("error", "Microphone did not become ready");
      send_line(control, "ERROR microphone warmup failed\n");
    }
    free(rx);
    goto done;
  }
  if (send_line(control, "READY\n")) {
    free(rx);
    goto done;
  }
  pthread_mutex_lock(&N.lock);
  N.connected = 1;
  inet_ntop(AF_INET, &peer.sin_addr, N.peer, sizeof N.peer);
  N.buffer = buffer;
  N.red_distance = red ? tx.red_distance : 0;
  N.sent = N.received = N.recovered = N.concealed = N.invalid = N.send_drops =
      N.xruns = N.played = N.rtcp = 0;
  pthread_mutex_unlock(&N.lock);
  state("streaming", "Stereo audio connected");
  fprintf(stderr,
          "E1_RTP_DUPLEX_READY peer=%s rate=48000 bits=24 channels=2 buffer=%u "
          "red=%u\n",
          N.peer, buffer, red);
  uint64_t heartbeat = now_us(), last_rtcp = 0, last_stats = 0;
  int32_t captured[512 * E1_CAPTURE_CHANNELS], selected[512 * 2],
      pending[RTP_FRAMES * 2], output[RTP_FRAMES * 2];
  unsigned fill = 0, written = RTP_FRAMES;
  struct QueuedPacket {
    uint8_t data[RTP_MAX_PACKET];
    size_t size;
    uint64_t created;
  };
  struct QueuedPacket *queue = calloc(128, sizeof *queue);
  if (!queue) {
    free(rx);
    goto done;
  }
  unsigned queue_head = 0, queue_count = 0;
  double next_send = (double)now_us();
  uint8_t packet[RTP_MAX_PACKET + 1];
  char command[32];
  size_t command_size = 0;
  double input_peak[2] = {0}, output_peak[2] = {0};
  int failed = 0;
  while (enabled() && !failed) {
    uint64_t now = now_us();
    if (now - heartbeat > 5000000) {
      state("waiting", "Computer disconnected; waiting");
      break;
    }
    char commands[128];
    ssize_t n = recv(control, commands, sizeof commands, 0);
    if (n == 0)
      break;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
      break;
    for (ssize_t i = 0; i < n; ++i) {
      if (commands[i] == '\n') {
        command[command_size] = 0;
        command_size = 0;
        if (!strcmp(command, "PING")) {
          heartbeat = now;
          if (send_line(control, "PONG\n"))
            failed = 1;
        } else if (!strcmp(command, "STOP"))
          failed = 1;
        else {
          failed = 1;
          break;
        }
      } else if (command_size + 1 < sizeof command && commands[i] >= 32)
        command[command_size++] = commands[i];
      else {
        failed = 1;
        break;
      }
    }
    for (unsigned i = 0; i < 64; ++i) {
      struct sockaddr_in source;
      socklen_t length = sizeof source;
      n = recvfrom(udp, packet, sizeof packet, 0, (void *)&source, &length);
      if (n < 0)
        break;
      if (peer_matches(&source, &remote))
        rtp_receive(rx, packet, n, now_us());
    }
    for (unsigned i = 0; i < 8; ++i) {
      struct sockaddr_in source;
      socklen_t length = sizeof source;
      n = recvfrom(rtcp, packet, sizeof packet, 0, (void *)&source, &length);
      if (n < 0)
        break;
      if (peer_matches(&source, &remote_rtcp)) {
        pthread_mutex_lock(&N.lock);
        ++N.rtcp;
        pthread_mutex_unlock(&N.lock);
        if (rtp_is_bye(packet, n, ssrc))
          failed = 1;
      }
    }
    for (unsigned batch = 0; batch < 4; ++batch) {
      snd_pcm_sframes_t count = snd_pcm_readi(mic, captured, 512);
      if (count < 0) {
        if (recover_pcm(mic, count, 1))
          failed = 1;
        break;
      }
      if (!count)
        break;
      e1_select_microphones(captured, selected, (unsigned)count);
      for (snd_pcm_sframes_t f = 0; f < count; ++f) {
        for (unsigned ch = 0; ch < 2; ++ch) {
          int32_t value = selected[f * 2 + ch] >> 8;
          pending[fill * 2 + ch] = value;
          input_peak[ch] = fmax(input_peak[ch], fabs(value / 8388608.0));
        }
        if (++fill == RTP_FRAMES) {
          if (queue_count == 128) {
            queue_head = (queue_head + 1) % 128;
            --queue_count;
            pthread_mutex_lock(&N.lock);
            ++N.send_drops;
            pthread_mutex_unlock(&N.lock);
          }
          struct QueuedPacket *queued =
              &queue[(queue_head + queue_count) % 128];
          queued->size = rtp_encode(&tx, pending, red, queued->data);
          queued->created = now_us();
          ++queue_count;
          fill = 0;
        }
      }
    }
    for (unsigned batch = 0; batch < 4 && queue_count; ++batch) {
      struct QueuedPacket *queued = &queue[queue_head];
      uint64_t send_now = now_us();
      if (send_now - queued->created > (buffer < 40 ? buffer * 500 : 120000)) {
        queue_head = (queue_head + 1) % 128;
        --queue_count;
        pthread_mutex_lock(&N.lock);
        ++N.send_drops;
        pthread_mutex_unlock(&N.lock);
        continue;
      }
      if (send_now < next_send)
        break;
      ssize_t sent = send(udp, queued->data, queued->size, 0);
      if (sent < 0 &&
          (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        break;
      if (sent == (ssize_t)queued->size)
        rtp_tx_sent(&tx, queued->size);
      if (sent != (ssize_t)queued->size) {
        pthread_mutex_lock(&N.lock);
        ++N.send_drops;
        pthread_mutex_unlock(&N.lock);
      }
      queue_head = (queue_head + 1) % 128;
      --queue_count;
      next_send = fmax(next_send, (double)send_now - 10000) +
                  RTP_FRAMES * 1000000.0 / RTP_RATE;
    }
    for (unsigned batch = 0; batch < 8; ++batch) {
      snd_pcm_sframes_t available = snd_pcm_avail_update(speaker);
      if (available < 0) {
        if (recover_pcm(speaker, available, 0))
          failed = 1;
        break;
      }
      if (available <= 0)
        break;
      if (written == RTP_FRAMES) {
        rtp_render(rx, output, RTP_FRAMES);
        written = 0;
        for (unsigned f = 0; f < RTP_FRAMES; ++f)
          for (unsigned ch = 0; ch < 2; ++ch)
            output_peak[ch] =
                fmax(output_peak[ch], fabs(output[f * 2 + ch] / 8388608.0));
      }
      snd_pcm_sframes_t count =
          snd_pcm_writei(speaker, output + written * 2,
                         (snd_pcm_uframes_t)available < RTP_FRAMES - written
                             ? (snd_pcm_uframes_t)available
                             : RTP_FRAMES - written);
      if (count < 0) {
        if (recover_pcm(speaker, count, 0))
          failed = 1;
        break;
      }
      if (!count)
        break;
      written += count;
    }
    if (now - last_rtcp >= 5000000) {
      struct timeval tv;
      gettimeofday(&tv, NULL);
      size_t size =
          rtp_rtcp(&tx, rx, (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec, packet,
                   sizeof packet);
      sendto(rtcp, packet, size, 0, (void *)&remote_rtcp, sizeof remote_rtcp);
      last_rtcp = now;
    }
    if (now - last_stats >= 100000) {
      pthread_mutex_lock(&N.lock);
      N.sent = tx.packets;
      N.received = rx->packets;
      N.recovered = rx->recovered;
      N.concealed = rx->concealed;
      N.invalid = rx->invalid;
      N.played = rx->played;
      N.jitter = rx->jitter * 1000 / RTP_RATE;
      N.queue = rtp_queue_frames(rx) * 1000.0 / RTP_RATE;
      N.ppm = (rx->ratio - 1) * 1000000;
      for (unsigned ch = 0; ch < 2; ++ch) {
        N.input[ch] =
            input_peak[ch] > 0 ? fmax(-96, 20 * log10(input_peak[ch])) : -96;
        N.output[ch] =
            output_peak[ch] > 0 ? fmax(-96, 20 * log10(output_peak[ch])) : -96;
        input_peak[ch] = output_peak[ch] = 0;
      }
      pthread_mutex_unlock(&N.lock);
      last_stats = now;
    }
    usleep(1000);
  }
  packet[0] = 0x81;
  packet[1] = 203;
  packet[2] = 0;
  packet[3] = 1;
  uint32_t bye_ssrc = htonl(tx.ssrc);
  memcpy(packet + 4, &bye_ssrc, 4);
  sendto(rtcp, packet, 8, 0, (void *)&remote_rtcp, sizeof remote_rtcp);
  fprintf(stderr,
          "E1_RTP_SESSION_END sent=%llu received=%llu recovered=%llu "
          "concealed=%llu invalid=%llu\n",
          (unsigned long long)tx.packets, (unsigned long long)rx->packets,
          (unsigned long long)rx->recovered, (unsigned long long)rx->concealed,
          (unsigned long long)rx->invalid);
  pthread_mutex_lock(&N.lock);
  N.sent = tx.packets;
  N.received = rx->packets;
  N.recovered = rx->recovered;
  N.concealed = rx->concealed;
  N.invalid = rx->invalid;
  N.played = rx->played;
  N.jitter = rx->jitter * 1000 / RTP_RATE;
  N.queue = rtp_queue_frames(rx) * 1000.0 / RTP_RATE;
  N.ppm = (rx->ratio - 1) * 1000000;
  pthread_mutex_unlock(&N.lock);
  free(queue);
  free(rx);
done: {
  struct sockaddr disconnected = {.sa_family = AF_UNSPEC};
  connect(udp, &disconnected, sizeof disconnected);
}
  pcm_close(&mic);
  pcm_close(&speaker);
  pthread_mutex_lock(&N.lock);
  N.connected = 0;
  N.peer[0] = 0;
  N.input[0] = N.input[1] = N.output[0] = N.output[1] = -96;
  pthread_mutex_unlock(&N.lock);
}
static void *worker(void *unused) {
  (void)unused;
  for (;;) {
    pthread_mutex_lock(&N.lock);
    int running = N.running, armed = N.armed;
    pthread_mutex_unlock(&N.lock);
    if (!running)
      break;
    if (!armed) {
      usleep(10000);
      continue;
    }
    pthread_mutex_lock(&N.lock);
    N.busy = 1;
    pthread_mutex_unlock(&N.lock);
    int old_lps = radio_mode("LPS mode:"), old_ips = radio_mode("IPS mode:");
    int radio_ok = old_lps >= 0 && old_ips >= 0 && radio_set("lps", 0) == 0 &&
                   radio_set("ips", 0) == 0;
    fprintf(stderr, "E1_RTP_RADIO active=%d previous_lps=%d previous_ips=%d\n",
            radio_ok, old_lps, old_ips);
    int listener = make_socket(SOCK_STREAM, CONTROL_PORT),
        udp = make_socket(SOCK_DGRAM, AUDIO_PORT),
        rtcp = make_socket(SOCK_DGRAM, RTCP_PORT);
    if (!radio_ok || listener < 0 || udp < 0 || rtcp < 0 || listen(listener, 2))
      state("error", radio_ok ? "Cannot open Wi-Fi Audio ports"
                              : "Cannot disable radio power saving");
    else {
      state("waiting", "Open the computer bridge to connect");
      while (enabled()) {
        struct sockaddr_in peer;
        socklen_t len = sizeof peer;
        int fd = accept4(listener, (void *)&peer, &len,
                         SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd >= 0) {
          /* Flush packets left behind by a previous session. */
          uint8_t stale[2048];
          while (recv(udp, stale, sizeof stale, 0) > 0) {
          }
          while (recv(rtcp, stale, sizeof stale, 0) > 0) {
          }
          session(fd, udp, rtcp, peer);
          close(fd);
          if (enabled()) {
            pthread_mutex_lock(&N.lock);
            int had_error = !strcmp(N.state, "error");
            pthread_mutex_unlock(&N.lock);
            if (!had_error)
              state("waiting", "Open the computer bridge to reconnect");
          }
        } else
          usleep(10000);
      }
    }
    if (old_lps >= 0)
      radio_set("lps", old_lps);
    if (old_ips >= 0)
      radio_set("ips", old_ips);
    if (listener >= 0)
      close(listener);
    if (udp >= 0)
      close(udp);
    if (rtcp >= 0)
      close(rtcp);
    pthread_mutex_lock(&N.lock);
    N.busy = 0;
    N.armed = 0;
    pthread_mutex_unlock(&N.lock);
    pthread_mutex_lock(&N.lock);
    int had_error = !strcmp(N.state, "error");
    N.input[0] = N.input[1] = N.output[0] = N.output[1] = -96;
    pthread_mutex_unlock(&N.lock);
    if (!had_error)
      state("off", "REC enables Wi-Fi Audio");
  }
  return NULL;
}
void netaudio_start(void) {
  pthread_mutex_lock(&N.lock);
  N.running = 1;
  pthread_mutex_unlock(&N.lock);
  if (pthread_create(&N.thread, NULL, worker, NULL)) {
    pthread_mutex_lock(&N.lock);
    N.running = 0;
    pthread_mutex_unlock(&N.lock);
    state("error", "Cannot start audio worker");
  }
}
int netaudio_arm(void) {
  pthread_mutex_lock(&N.lock);
  int ok = N.running && !N.armed && !N.busy;
  if (ok)
    N.armed = 1;
  pthread_mutex_unlock(&N.lock);
  return ok;
}
void netaudio_disarm(void) {
  pthread_mutex_lock(&N.lock);
  N.armed = 0;
  pthread_mutex_unlock(&N.lock);
}
int netaudio_armed(void) {
  pthread_mutex_lock(&N.lock);
  int v = N.armed || N.busy;
  pthread_mutex_unlock(&N.lock);
  return v;
}
void netaudio_stop(void) {
  pthread_mutex_lock(&N.lock);
  int join = N.running;
  N.running = N.armed = 0;
  pthread_mutex_unlock(&N.lock);
  if (join)
    pthread_join(N.thread, NULL);
}
char *netaudio_json(void) {
  char *body = NULL;
  size_t size = 0;
  FILE *f = open_memstream(&body, &size);
  if (!f)
    return NULL;
  pthread_mutex_lock(&N.lock);
  fprintf(f,
          "{\"armed\":%s,\"active\":%s,\"state\":\"%s\",\"message\":\"%s\","
          "\"peer\":\"%s\",\"controlPort\":%d,\"rtpPort\":%d,\"rtcpPort\":%d,"
          "\"sampleRate\":48000,\"bits\":24,\"channels\":2,\"framesPerPacket\":"
          "120,\"bufferMs\":%u,\"sentPackets\":%llu,\"receivedPackets\":%llu,"
          "\"recoveredPackets\":%llu,\"concealedPackets\":%llu,"
          "\"invalidPackets\":%llu,\"sendDrops\":%llu,\"xruns\":%llu,"
          "\"playedFrames\":%llu,\"rtcpPackets\":%llu,\"inputLeftDb\":%.2f,"
          "\"inputRightDb\":%.2f,\"outputLeftDb\":%.2f,\"outputRightDb\":%.2f,"
          "\"jitterMs\":%.3f,\"queueMs\":%.3f,\"clockPpm\":%.1f",
          N.armed ? "true" : "false", N.connected ? "true" : "false", N.state,
          N.message, N.peer, CONTROL_PORT, AUDIO_PORT, RTCP_PORT, N.buffer,
          (unsigned long long)N.sent, (unsigned long long)N.received,
          (unsigned long long)N.recovered, (unsigned long long)N.concealed,
          (unsigned long long)N.invalid, (unsigned long long)N.send_drops,
          (unsigned long long)N.xruns, (unsigned long long)N.played,
          (unsigned long long)N.rtcp, N.input[0], N.input[1], N.output[0],
          N.output[1], N.jitter, N.queue, N.ppm);
  fprintf(f,
          ",\"redDistancePackets\":%u,\"captureChannels\":8,\"sourceChannels\":"
          "[6,7],\"captureBufferFrames\":%u,"
          "\"capturePeriodFrames\":%u,\"playbackBufferFrames\":%u,"
          "\"playbackPeriodFrames\":%u}",
          N.red_distance, N.capture_buffer, N.capture_period, N.playback_buffer,
          N.playback_period);
  pthread_mutex_unlock(&N.lock);
  fclose(f);
  return body;
}

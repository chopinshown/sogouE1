#ifndef E1_NETAUDIO_H
#define E1_NETAUDIO_H
void netaudio_start(void);
void netaudio_stop(void);
/* Called by the recorder's audio owner after releasing its PCMs. */
int netaudio_arm(void);
void netaudio_disarm(void);
int netaudio_armed(void);
char *netaudio_json(void);
#endif

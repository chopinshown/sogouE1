#ifndef E1_XIAOZHI_H
#define E1_XIAOZHI_H
int xiaozhi_start(void);
void xiaozhi_stop(void);
/* Called only after the recorder audio thread releases its ALSA handles. */
int xiaozhi_begin(void);
int xiaozhi_active(void);
void xiaozhi_cancel(void);
int xiaozhi_submit(const char *action, const char **error);
char *xiaozhi_json(void);
#endif

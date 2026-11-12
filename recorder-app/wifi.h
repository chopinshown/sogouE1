#ifndef E1_WIFI_H
#define E1_WIFI_H
int wifi_start(void);
void wifi_stop(void);
char *wifi_json(void);
/* Returns an HTTP status; error is a static, credential-free message. */
int wifi_submit(const char *action, const char *form, const char **error);
#endif

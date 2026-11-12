/* Exercise the same evdev path as the E1's physical ADC buttons. */
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

static int emit(int fd, unsigned short type, unsigned short code, int value) {
    struct input_event event = {0};
    gettimeofday(&event.time, NULL);
    event.type = type; event.code = code; event.value = value;
    return write(fd, &event, sizeof event) == sizeof event ? 0 : -1;
}
int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) { fprintf(stderr,"Usage: keytap mark|stop|record|up|down [hold-ms]\n"); return 2; }
    int hold=argc==3?atoi(argv[2]):60;
    if(hold<1||hold>2500)return 2;
    const char *names[] = {"mark","stop","record","up","down"};
    int code = 0;
    for(int i=0;i<5;i++) if(!strcmp(argv[1],names[i])) code=KEY_1+i;
    if(!code)return 2;
    int fd=open("/dev/input/event3",O_WRONLY);
    if(fd<0){perror("event3");return 1;}
    char device[128]={0};
    if(ioctl(fd,EVIOCGNAME(sizeof device),device)<0||strcmp(device,"adc-keys")) {
        fprintf(stderr,"Refusing to inject into unexpected input device: %s\n",device);close(fd);return 1;
    }
    if(emit(fd,EV_KEY,code,1)||emit(fd,EV_SYN,SYN_REPORT,0)){perror("press");close(fd);return 1;}
    usleep(hold*1000);
    int failed=emit(fd,EV_KEY,code,0)||emit(fd,EV_SYN,SYN_REPORT,0);
    close(fd);return failed?1:0;
}

/* Exercise the same evdev path as the E1's physical ADC buttons. */
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int emit(int fd, unsigned short type, unsigned short code, int value) {
    struct input_event event = {0};
    gettimeofday(&event.time, NULL);
    event.type = type; event.code = code; event.value = value;
    return write(fd, &event, sizeof event) == sizeof event ? 0 : -1;
}
int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) { fprintf(stderr,"Usage: keytap mark|stop|record|up|down|power [hold-ms]\n"); return 2; }
    int hold=argc==3?atoi(argv[2]):60;
    if(hold<1||hold>4500)return 2;
    const char *names[] = {"mark","stop","record","up","down"};
    int code = 0;
    for(int i=0;i<5;i++) if(!strcmp(argv[1],names[i])) code=KEY_1+i;
    /* This E1's rk8xx driver reports its power switch as KEY_6 (code 7). */
    int power=!strcmp(argv[1],"power");if(power)code=KEY_6;
    if(!code)return 2;
    int fd=open(power?"/dev/input/event1":"/dev/input/event3",O_WRONLY);
    if(fd<0){perror("input device");return 1;}
    char device[128]={0};
    if(ioctl(fd,EVIOCGNAME(sizeof device),device)<0||strcmp(device,power?"rk8xx_pwrkey":"adc-keys")) {
        fprintf(stderr,"Refusing to inject into unexpected input device: %s\n",device);close(fd);return 1;
    }
    unsigned char capabilities[(KEY_MAX+8)/8]={0};
    if(ioctl(fd,EVIOCGBIT(EV_KEY,sizeof capabilities),capabilities)<0||
       !(capabilities[code/8]&(1<<(code%8)))) {
        fprintf(stderr,"Input device does not expose key code %d\n",code);close(fd);return 1;
    }
    struct timespec start,end;clock_gettime(CLOCK_MONOTONIC,&start);
    if(emit(fd,EV_KEY,code,1)||emit(fd,EV_SYN,SYN_REPORT,0)){perror("press");close(fd);return 1;}
    usleep(hold*1000);
    int failed=emit(fd,EV_KEY,code,0)||emit(fd,EV_SYN,SYN_REPORT,0);
    clock_gettime(CLOCK_MONOTONIC,&end);
    printf("KEY_PRESS_MS=%llu KEY_RELEASE_MS=%llu\n",(unsigned long long)start.tv_sec*1000+start.tv_nsec/1000000,(unsigned long long)end.tv_sec*1000+end.tv_nsec/1000000);
    close(fd);return failed?1:0;
}

/* Inject a tap or drag through the E1's real touchscreen event interface. */
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

static int emit(int fd,unsigned short type,unsigned short code,int value) {
    struct input_event event={0};gettimeofday(&event.time,NULL);
    event.type=type;event.code=code;event.value=value;
    return write(fd,&event,sizeof event)==sizeof event?0:-1;
}
static int coordinate(int value,const struct input_absinfo *info) {
    return info->minimum+value*(info->maximum-info->minimum)/319;
}
int main(int argc,char **argv) {
    if(argc!=3&&argc!=6){fprintf(stderr,"Usage: touch x y [end-x end-y duration-ms]\n");return 2;}
    int x=atoi(argv[1]),y=atoi(argv[2]),ex=argc==6?atoi(argv[3]):x,ey=argc==6?atoi(argv[4]):y;
    int duration=argc==6?atoi(argv[5]):70;
    if(x<0||x>319||y<0||y>319||ex<0||ex>319||ey<0||ey>319||duration<30||duration>2000)return 2;
    int fd=open("/dev/input/event0",O_RDWR);if(fd<0){perror("event0");return 1;}
    char name[128]={0};struct input_absinfo ax,ay,slot;
    if(ioctl(fd,EVIOCGNAME(sizeof name),name)<0||strcmp(name,"st16xx")||
       ioctl(fd,EVIOCGABS(ABS_MT_POSITION_X),&ax)<0||ioctl(fd,EVIOCGABS(ABS_MT_POSITION_Y),&ay)<0||
       ioctl(fd,EVIOCGABS(ABS_MT_SLOT),&slot)<0){fprintf(stderr,"Unexpected touchscreen\n");close(fd);return 1;}
    unsigned char keys[(KEY_MAX+8)/8]={0};
    if(ioctl(fd,EVIOCGKEY(sizeof keys),keys)<0||(keys[BTN_TOUCH/8]&(1<<(BTN_TOUCH%8)))) {
        fprintf(stderr,"Touchscreen is busy\n");close(fd);return 1;
    }
    int failed=emit(fd,EV_ABS,ABS_MT_SLOT,slot.minimum)||emit(fd,EV_ABS,ABS_MT_TRACKING_ID,12345)||
        emit(fd,EV_ABS,ABS_MT_POSITION_X,coordinate(x,&ax))||emit(fd,EV_ABS,ABS_MT_POSITION_Y,coordinate(y,&ay))||
        emit(fd,EV_KEY,BTN_TOUCH,1)||emit(fd,EV_SYN,SYN_REPORT,0);
    int steps=duration/20;if(steps<1)steps=1;
    for(int i=1;i<=steps&&!failed;i++) {
        usleep(duration*1000/steps);
        failed=emit(fd,EV_ABS,ABS_MT_POSITION_X,coordinate(x+(ex-x)*i/steps,&ax))||
            emit(fd,EV_ABS,ABS_MT_POSITION_Y,coordinate(y+(ey-y)*i/steps,&ay))||emit(fd,EV_SYN,SYN_REPORT,0);
    }
    if(emit(fd,EV_ABS,ABS_MT_TRACKING_ID,-1)||emit(fd,EV_KEY,BTN_TOUCH,0)||emit(fd,EV_SYN,SYN_REPORT,0))failed=1;
    close(fd);return failed?1:0;
}

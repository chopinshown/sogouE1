#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "wifi.h"

#define RATE 44100
#define CHANNELS 2
#define FRAMES 441
#define MAX_MARKS 256
#define PEAKS 1024
#define PORT 18770
#define VERSION "1.0.0"
#define MIB (1024ULL*1024ULL)
enum { IDLE, RECORDING, PLAYING };
enum { NONE, RECORD, PLAY, STOP };
typedef struct {
    float l[PEAKS], r[PEAKS], partial_l, partial_r;
    int count;
    uint64_t step, partial_frames;
} Envelope;
typedef struct {
    pthread_mutex_t mutex;
    int mode, command, mark_count, xruns;
    int capture_ready;
    int shutting_down, shutdown_failed;
    double left_db, right_db, left_rms, right_rms;
    double position, duration, requested_position, seek;
    uint64_t frames, marks[MAX_MARKS];
    char name[128], requested_file[128], error[256];
    Envelope wave;
} State;
static State S = { .mutex=PTHREAD_MUTEX_INITIALIZER, .left_db=-96, .right_db=-96, .seek=-1 };
static volatile sig_atomic_t alive = 1;
static int shutdown_requested, poweroff_test, shutdown_was_recording;
static uint64_t shutdown_requested_ms;
static char storage[512] = "/mnt/sdcard/Recordings";
static int record_fd = -1;
static snd_pcm_t *capture, *playback;
static FILE *play_file;
static uint64_t play_frames, play_total;
static off_t play_data;

static void signal_stop(int sig) { (void)sig; alive=0; }
static uint64_t monotonic_ms(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000; }
static void le16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }
static void le32(uint8_t *p, uint32_t v) { for(int i=0;i<4;i++)p[i]=v>>(i*8); }
static uint16_t u16(const uint8_t *p) { return p[0]|p[1]<<8; }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static double db(double level) { return level>0 ? fmax(-96,20*log10(level)) : -96; }
static int write_all(int fd, const void *data, size_t size) {
    const uint8_t *p=data;
    while(size) { ssize_t n=write(fd,p,size); if(n<0&&errno==EINTR)continue; if(n<=0)return -1; p+=n;size-=n; }
    return 0;
}
static int valid_name(const char *s) {
    size_t n=strlen(s);
    if(n<5||n>120||strcmp(s+n-4,".wav")||strstr(s,".."))return 0;
    for(size_t i=0;i<n;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='z')||(s[i]>='A'&&s[i]<='Z')||s[i]=='_'||s[i]=='-'||s[i]=='.'))return 0;
    return 1;
}
static void path_for(char *dest, size_t size, const char *name, const char *suffix) { snprintf(dest,size,"%s/%s%s",storage,name,suffix?suffix:""); }
static void json_string(FILE *f,const char *s) {
    fputc('"',f);
    for(const unsigned char *p=(const unsigned char*)s;*p;p++) {
        if(*p=='"'||*p=='\\')fprintf(f,"\\%c",*p);
        else if(*p<32)fprintf(f,"\\u%04x",*p);else fputc(*p,f);
    }
    fputc('"',f);
}
static void set_error(const char *prefix,int code) {
    snprintf(S.error,sizeof S.error,"%s: %s",prefix,snd_strerror(code));
    fprintf(stderr,"%s\n",S.error);
}
static void envelope_reset(Envelope *w) { memset(w,0,sizeof *w);w->step=882; }
static void envelope_add(Envelope *w,float l,float r) {
    w->partial_l=fmaxf(w->partial_l,l);w->partial_r=fmaxf(w->partial_r,r);
    if(++w->partial_frames<w->step)return;
    if(w->count==PEAKS) {
        for(int i=0;i<PEAKS/2;i++) {w->l[i]=fmaxf(w->l[2*i],w->l[2*i+1]);w->r[i]=fmaxf(w->r[2*i],w->r[2*i+1]);}
        w->count=PEAKS/2;w->step*=2;
        /* The pending half-bucket becomes the first half of the wider bucket. */
        return;
    }
    w->l[w->count]=w->partial_l;w->r[w->count++]=w->partial_r;
    w->partial_frames=0;w->partial_l=w->partial_r=0;
}
static void print_envelope(FILE *f,const Envelope *w,int right,int max) {
    int count=w->count+(w->partial_frames?1:0), stride=(count+max-1)/max;
    if(stride<1)stride=1;
    fputc('[',f);
    for(int i=0;i<count;i+=stride) {
        float p=0;
        for(int j=i;j<i+stride&&j<count;j++)p=fmaxf(p,j<w->count?(right?w->r[j]:w->l[j]):(right?w->partial_r:w->partial_l));
        if(i)fputc(',',f);fprintf(f,"%.6f",p);
    }
    fputc(']',f);
}
static void print_marks(FILE *f) {
    fputc('[',f);for(int i=0;i<S.mark_count;i++){if(i)fputc(',',f);fprintf(f,"{\"id\":%d,\"frame\":%llu,\"seconds\":%.6f}",i+1,(unsigned long long)S.marks[i],S.marks[i]/(double)RATE);}fputc(']',f);
}
static void save_marks(void) {
    char path[800],temp[810];path_for(path,sizeof path,S.name,".marks.json");snprintf(temp,sizeof temp,"%s.tmp",path);
    FILE *f=fopen(temp,"w");if(!f)return;
    fprintf(f,"{\"sampleRate\":44100,\"marks\":");print_marks(f);fprintf(f,"}\n");
    fflush(f);fsync(fileno(f));fclose(f);rename(temp,path);
}
static void save_wave(void) {
    char path[800],temp[810];path_for(path,sizeof path,S.name,".wave.json");snprintf(temp,sizeof temp,"%s.tmp",path);
    FILE *f=fopen(temp,"w");if(!f)return;
    fprintf(f,"{\"name\":");json_string(f,S.name);
    fprintf(f,",\"sampleRate\":44100,\"bits\":24,\"channels\":2,\"duration\":%.6f,\"frames\":%llu,\"xruns\":%d,\"marks\":",S.frames/(double)RATE,(unsigned long long)S.frames,S.xruns);
    print_marks(f);fputs(",\"left\":",f);print_envelope(f,&S.wave,0,512);fputs(",\"right\":",f);print_envelope(f,&S.wave,1,512);fputs("}\n",f);
    fflush(f);fsync(fileno(f));fclose(f);rename(temp,path);
}
static void wave_header(uint8_t *h,uint64_t frames,uint32_t extra) {
    memset(h,0,44);memcpy(h,"RIFF",4);le32(h+4,36+(uint32_t)(frames*6)+extra);memcpy(h+8,"WAVEfmt ",8);
    le32(h+16,16);le16(h+20,1);le16(h+22,2);le32(h+24,RATE);le32(h+28,RATE*6);le16(h+32,6);le16(h+34,24);memcpy(h+36,"data",4);le32(h+40,frames*6);
}
static void finish_record(void) {
    if(record_fd<0)return;
    off_t end=44+(off_t)S.frames*6;
    ftruncate(record_fd,end);lseek(record_fd,end,SEEK_SET);
    uint32_t extra=0;
    if(S.mark_count) {
        uint8_t cue[12+MAX_MARKS*24];memcpy(cue,"cue ",4);le32(cue+4,4+S.mark_count*24);le32(cue+8,S.mark_count);
        for(int i=0;i<S.mark_count;i++) {uint8_t *p=cue+12+i*24;le32(p,i+1);le32(p+4,S.marks[i]);memcpy(p+8,"data",4);le32(p+12,0);le32(p+16,0);le32(p+20,S.marks[i]);}
        extra=12+S.mark_count*24;
        if(write_all(record_fd,cue,extra)) {extra=0;ftruncate(record_fd,end);}
    }
    uint8_t h[44];wave_header(h,S.frames,extra);pwrite(record_fd,h,44,0);fsync(record_fd);close(record_fd);record_fd=-1;
    S.duration=S.frames/(double)RATE;S.position=0;save_marks();save_wave();
    fprintf(stderr,"SAVED %s frames=%llu marks=%d xruns=%d\n",S.name,(unsigned long long)S.frames,S.mark_count,S.xruns);
}
static int read_wave(FILE *f,off_t *offset,uint64_t *frames) {
    uint8_t h[12];if(fread(h,1,12,f)!=12||memcmp(h,"RIFF",4)||memcmp(h+8,"WAVE",4))return -1;
    int fmt=0;
    while(fread(h,1,8,f)==8) {
        uint32_t size=u32(h+4);off_t next=ftello(f)+size+(size&1);
        if(!memcmp(h,"fmt ",4)) {
            uint8_t p[40];if(size<16||fread(p,1,size<40?size:40,f)<16)return -1;
            if(u16(p)!=1||u16(p+2)!=2||u32(p+4)!=RATE||u16(p+12)!=6||u16(p+14)!=24)return -1;fmt=1;
        } else if(!memcmp(h,"data",4)) {if(!fmt||size%6)return -1;*offset=ftello(f);*frames=size/6;return 0;}
        if(fseeko(f,next,SEEK_SET))return -1;
    }
    return -1;
}
static int capture_open(void) {
    snd_pcm_hw_params_t *p=NULL;
    int e=snd_pcm_open(&capture,"hw:2,0",SND_PCM_STREAM_CAPTURE,0);
    if(e<0)goto bad;
    if((e=snd_pcm_hw_params_malloc(&p))<0)goto bad;
    if((e=snd_pcm_hw_params_any(capture,p))<0)goto bad;
    if((e=snd_pcm_hw_params_set_access(capture,p,SND_PCM_ACCESS_RW_INTERLEAVED))<0)goto bad;
    if((e=snd_pcm_hw_params_set_format(capture,p,SND_PCM_FORMAT_S32_LE))<0)goto bad;
    if((e=snd_pcm_hw_params_set_channels(capture,p,2))<0)goto bad;
    if((e=snd_pcm_hw_params_set_rate(capture,p,RATE,0))<0)goto bad;
    unsigned period=10000,buffer=100000;int direction=0;
    if((e=snd_pcm_hw_params_set_period_time_near(capture,p,&period,&direction))<0)goto bad;
    if((e=snd_pcm_hw_params_set_buffer_time_near(capture,p,&buffer,&direction))<0)goto bad;
    if((e=snd_pcm_hw_params(capture,p))<0)goto bad;
    unsigned rate=0,channels=0;snd_pcm_format_t format;
    snd_pcm_hw_params_get_rate(p,&rate,&direction);snd_pcm_hw_params_get_channels(p,&channels);snd_pcm_hw_params_get_format(p,&format);
    if(rate!=RATE||channels!=2||format!=SND_PCM_FORMAT_S32_LE){e=-EINVAL;goto bad;}
    int significant_bits=snd_pcm_hw_params_get_sbits(p);
    snd_pcm_hw_params_free(p);S.capture_ready=1;
    fprintf(stderr,"CAPTURE hw:2,0 rate=%u channels=%u format=S32_LE significant_bits=%d; PDM 24-bit samples occupy bits 31..8\n",rate,channels,significant_bits);
    return 0;
bad:
    if(p)snd_pcm_hw_params_free(p);if(capture){snd_pcm_close(capture);capture=NULL;}S.capture_ready=0;set_error("Microphone",e);return -1;
}
static void close_capture(void) {if(capture){snd_pcm_drop(capture);snd_pcm_close(capture);capture=NULL;}S.capture_ready=0;}
static void close_playback(int drain) {
    if(playback){if(drain)snd_pcm_drain(playback);else snd_pcm_drop(playback);snd_pcm_close(playback);playback=NULL;}
    if(play_file){fclose(play_file);play_file=NULL;}
}
static void start_record(void) {
    if(!capture&&capture_open())return;
    struct statvfs fs;if(statvfs(storage,&fs)||((uint64_t)fs.f_bavail*fs.f_frsize)<10*MIB){snprintf(S.error,sizeof S.error,"Not enough free recording storage");return;}
    time_t now=time(NULL);struct tm tm;localtime_r(&now,&tm);char stamp[64],path[800];strftime(stamp,sizeof stamp,"%Y%m%d-%H%M%S",&tm);
    for(int i=0;i<1000;i++) {
        snprintf(S.name,sizeof S.name,"REC-%s-%03d.wav",stamp,i);path_for(path,sizeof path,S.name,NULL);
        record_fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0660);if(record_fd>=0)break;if(errno!=EEXIST)break;
    }
    if(record_fd<0){snprintf(S.error,sizeof S.error,"Create WAV: %s",strerror(errno));return;}
    uint8_t h[44];wave_header(h,0,0);if(write_all(record_fd,h,44)){close(record_fd);record_fd=-1;snprintf(S.error,sizeof S.error,"Cannot write WAV header");return;}
    S.frames=0;S.position=0;S.duration=0;S.mark_count=0;S.xruns=0;S.error[0]=0;envelope_reset(&S.wave);S.mode=RECORDING;
    save_marks();fprintf(stderr,"RECORD %s\n",S.name);
}
static void start_playback(void) {
    char path[800];path_for(path,sizeof path,S.requested_file,NULL);
    play_file=fopen(path,"rb");
    if(!play_file||read_wave(play_file,&play_data,&play_total)) {snprintf(S.error,sizeof S.error,"Unsupported or unreadable WAV");close_playback(0);return;}
    close_capture();
    int e=snd_pcm_open(&playback,"default",SND_PCM_STREAM_PLAYBACK,0);
    if(e>=0)e=snd_pcm_set_params(playback,SND_PCM_FORMAT_S24_LE,SND_PCM_ACCESS_RW_INTERLEAVED,2,RATE,1,100000);
    if(e<0){set_error("Playback",e);close_playback(0);return;}
    play_frames=(uint64_t)(fmax(0,S.requested_position)*RATE);if(play_frames>=play_total)play_frames=0;
    fseeko(play_file,play_data+(off_t)play_frames*6,SEEK_SET);
    snprintf(S.name,sizeof S.name,"%s",S.requested_file);S.position=play_frames/(double)RATE;S.duration=play_total/(double)RATE;S.mode=PLAYING;S.error[0]=0;
    fprintf(stderr,"PLAY %s at frame=%llu\n",S.name,(unsigned long long)play_frames);
}
static void *audio_thread(void *unused) {
    (void)unused;
    int32_t samples[FRAMES*2];uint8_t packed[FRAMES*6];uint64_t last_sync=0;
    while(alive) {
        pthread_mutex_lock(&S.mutex);
        int command=S.command;S.command=NONE;
        if(command==STOP){finish_record();close_playback(0);S.mode=IDLE;S.seek=-1;}
        if(command==RECORD&&S.mode==IDLE){start_record();last_sync=0;}
        if(command==PLAY&&S.mode==IDLE)start_playback();
        if(S.mode==PLAYING&&S.seek>=0) {
            play_frames=(uint64_t)(fmin(S.duration,fmax(0,S.seek))*RATE);if(play_frames>play_total)play_frames=play_total;
            snd_pcm_drop(playback);snd_pcm_prepare(playback);fseeko(play_file,play_data+(off_t)play_frames*6,SEEK_SET);S.position=play_frames/(double)RATE;S.seek=-1;
        }
        int mode=S.mode;
        if(mode!=PLAYING&&!capture)capture_open();
        int ready=capture!=NULL;
        pthread_mutex_unlock(&S.mutex);
        if(mode==PLAYING) {
            size_t n=fread(packed,6,FRAMES,play_file);
            if(!n) {close_playback(1);pthread_mutex_lock(&S.mutex);S.mode=IDLE;S.position=S.duration;S.left_db=S.right_db=-96;pthread_mutex_unlock(&S.mutex);continue;}
            double peak[2]={0},square[2]={0};
            for(size_t j=0;j<n*2;j++) {int32_t v=packed[j*3]|packed[j*3+1]<<8|packed[j*3+2]<<16;if(v&0x800000)v|=~0xffffff;samples[j]=v;double a=v/8388608.0;peak[j%2]=fmax(peak[j%2],fabs(a));square[j%2]+=a*a;}
            size_t sent=0;int failed=0;
            while(sent<n&&alive) {snd_pcm_sframes_t k=snd_pcm_writei(playback,samples+sent*2,n-sent);if(k<0){if(snd_pcm_recover(playback,k,1)<0){failed=1;break;}}else sent+=k;}
            play_frames+=sent;snd_pcm_sframes_t delay=0;snd_pcm_delay(playback,&delay);
            pthread_mutex_lock(&S.mutex);
            S.position=(play_frames>(uint64_t)(delay>0?delay:0)?play_frames-(delay>0?delay:0):0)/(double)RATE;
            S.left_db=db(peak[0]);S.right_db=db(peak[1]);S.left_rms=db(sqrt(square[0]/n));S.right_rms=db(sqrt(square[1]/n));
            if(failed){snprintf(S.error,sizeof S.error,"Audio output failed");close_playback(0);S.mode=IDLE;}
            pthread_mutex_unlock(&S.mutex);
            continue;
        }
        if(!ready){usleep(200000);continue;}
        snd_pcm_sframes_t count=snd_pcm_readi(capture,samples,FRAMES);
        if(count<0) {
            pthread_mutex_lock(&S.mutex);S.xruns++;
            if(snd_pcm_recover(capture,count,1)<0){set_error("Capture interrupted",count);finish_record();S.mode=IDLE;close_capture();}
            pthread_mutex_unlock(&S.mutex);continue;
        }
        if(!count)continue;
        double peak[2]={0},square[2]={0};
        pthread_mutex_lock(&S.mutex);
        for(int j=0;j<count*2;j++) {
            /* The PDM ADC delivers signed 24-bit data left-aligned in 32 bits. */
            int32_t v=samples[j]>>8;
            double a=v/8388608.0;peak[j%2]=fmax(peak[j%2],fabs(a));square[j%2]+=a*a;
            packed[j*3]=v;packed[j*3+1]=v>>8;packed[j*3+2]=v>>16;
            if(S.mode==RECORDING&&j%2==1){int32_t left=samples[j-1]>>8;envelope_add(&S.wave,fabs(left/8388608.0),fabs(a));}
        }
        S.left_db=db(peak[0]);S.right_db=db(peak[1]);S.left_rms=db(sqrt(square[0]/count));S.right_rms=db(sqrt(square[1]/count));
        if(S.mode==RECORDING&&record_fd>=0) {
            if((S.frames+count)*6>UINT32_MAX-8192||write_all(record_fd,packed,count*6)) {
                snprintf(S.error,sizeof S.error,"Recording stopped: storage or WAV size limit");finish_record();S.mode=IDLE;
            } else {
                S.frames+=count;S.duration=S.frames/(double)RATE;
                if(S.frames-last_sync>=RATE) {
                    uint8_t h[44];wave_header(h,S.frames,0);pwrite(record_fd,h,44,0);last_sync=S.frames;
                    if(S.frames%(RATE*5)<FRAMES)fdatasync(record_fd);
                    struct statvfs fs;
                    if(!statvfs(storage,&fs)&&(uint64_t)fs.f_bavail*fs.f_frsize<MIB){snprintf(S.error,sizeof S.error,"Storage full; WAV saved");finish_record();S.mode=IDLE;}
                }
            }
        }
        pthread_mutex_unlock(&S.mutex);
    }
    pthread_mutex_lock(&S.mutex);finish_record();close_playback(0);close_capture();S.mode=IDLE;pthread_mutex_unlock(&S.mutex);
    return NULL;
}
static void response(int fd,int status,const char *body) {
    char header[512];int n=snprintf(header,sizeof header,"HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",status,status==200?"OK":"Error",strlen(body));
    write_all(fd,header,n);write_all(fd,body,strlen(body));
}
static void query_value(const char *query,const char *key,char *out,size_t size) {
    out[0]=0;if(!query)return;size_t n=strlen(key);
    for(const char *p=query;*p;) {
        const char *end=strchr(p,'&');if(!end)end=p+strlen(p);
        if((size_t)(end-p)>n&&p[n]=='='&&!strncmp(p,key,n)) {
            size_t k=0;p+=n+1;
            while(p<end&&k+1<size) {if(*p=='%'&&p+2<end){char b[3]={p[1],p[2],0};out[k++]=strtol(b,NULL,16);p+=3;}else{out[k++]=*p=='+'?' ':*p;p++;}}
            out[k]=0;return;
        }
        p=*end?end+1:end;
    }
}
static char *status_json(void) {
    char *body=NULL;size_t len=0;FILE *f=open_memstream(&body,&len);
    pthread_mutex_lock(&S.mutex);
    fprintf(f,"{\"version\":\"%s\",\"state\":\"%s\",\"captureReady\":%s,\"rate\":44100,\"bits\":24,\"channels\":2,\"leftDb\":%.2f,\"rightDb\":%.2f,\"leftRms\":%.2f,\"rightRms\":%.2f,\"elapsed\":%.6f,\"position\":%.6f,\"duration\":%.6f,\"xruns\":%d,\"file\":",VERSION,S.mode==RECORDING?"recording":S.mode==PLAYING?"playing":"idle",S.capture_ready?"true":"false",S.left_db,S.right_db,S.left_rms,S.right_rms,S.mode==RECORDING?S.frames/(double)RATE:S.position,S.position,S.duration,S.xruns);
    json_string(f,S.name);fputs(",\"error\":",f);json_string(f,S.error);fputs(",\"marks\":",f);print_marks(f);
    fprintf(f,",\"shuttingDown\":%s,\"shutdownFailed\":%s",S.shutting_down?"true":"false",S.shutdown_failed?"true":"false");
    fputs(",\"left\":",f);if(S.mode==RECORDING)print_envelope(f,&S.wave,0,140);else fputs("[]",f);
    fputs(",\"right\":",f);if(S.mode==RECORDING)print_envelope(f,&S.wave,1,140);else fputs("[]",f);
    pthread_mutex_unlock(&S.mutex);fputs("}",f);fclose(f);return body;
}
static int sort_names(const struct dirent **a,const struct dirent **b) {return strcmp((*b)->d_name,(*a)->d_name);}
static char *files_json(void) {
    char *body=NULL;size_t len=0;FILE *f=open_memstream(&body,&len);struct dirent **list=NULL;
    int n=scandir(storage,&list,NULL,sort_names),first=1;
    fputs("{\"files\":[",f);
    for(int i=0;i<n;i++) {
        const char *name=list[i]->d_name;
        if(valid_name(name)) {
            char path[800];path_for(path,sizeof path,name,NULL);FILE *in=fopen(path,"rb");off_t offset;uint64_t frames;
            if(in&&!read_wave(in,&offset,&frames)) {
                struct stat st;stat(path,&st);
                if(!first)fputc(',',f);first=0;fputs("{\"name\":",f);json_string(f,name);fprintf(f,",\"duration\":%.6f,\"bytes\":%lld}",frames/(double)RATE,(long long)st.st_size);
            }
            if(in)fclose(in);
        }
        free(list[i]);
    }
    free(list);struct statvfs fs;uint64_t avail=!statvfs(storage,&fs)?(uint64_t)fs.f_bavail*fs.f_frsize:0;
    fprintf(f,"],\"freeBytes\":%llu,\"storage\":",(unsigned long long)avail);json_string(f,storage);fputs("}",f);fclose(f);return body;
}
static char *wave_json(const char *name) {
    char path[800];path_for(path,sizeof path,name,".wave.json");FILE *f=fopen(path,"rb");
    if(f) {fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);if(size>0&&size<262144){char *body=calloc(1,size+1);size_t n=fread(body,1,size,f);fclose(f);if(n==(size_t)size)return body;free(body);}else fclose(f);}
    return strdup("{\"error\":\"Waveform is available after the recording is stopped\"}");
}
static void handle_client(int fd) {
    char request[4096]={0};struct timeval timeout={2,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
    size_t used=0;char *form=NULL;
    while(used+1<sizeof request) {ssize_t n=read(fd,request+used,sizeof request-used-1);if(n<=0)return;used+=n;request[used]=0;if((form=strstr(request,"\r\n\r\n")))break;}
    if(!form){response(fd,400,"{\"error\":\"Request header too large\"}");return;}
    size_t header_bytes=(size_t)(form-request)+4,content_length=0;form+=4;
    for(char *p=strstr(request,"\r\n")+2;p<form-2;) {
        char *end=strstr(p,"\r\n");if(!end)break;
        if(!strncasecmp(p,"Content-Length:",15)) {
            char *tail;unsigned long n=strtoul(p+15,&tail,10);
            while(tail<end&&(*tail==' '||*tail=='\t'))tail++;
            if(tail!=end||n>2048){response(fd,400,"{\"error\":\"Invalid body size\"}");return;}content_length=n;
        }
        if(!strncasecmp(p,"Transfer-Encoding:",18)){response(fd,400,"{\"error\":\"Chunked requests unsupported\"}");return;}
        p=end+2;
    }
    if(header_bytes+content_length>=sizeof request){response(fd,400,"{\"error\":\"Request too large\"}");return;}
    while(used<header_bytes+content_length){ssize_t n=read(fd,request+used,header_bytes+content_length-used);if(n<=0)return;used+=n;}
    request[header_bytes+content_length]=0;
    char method[12],url[1024];if(sscanf(request,"%11s %1023s",method,url)!=2){response(fd,400,"{\"error\":\"Bad request\"}");return;}
    char *query=strchr(url,'?');if(query)*query++=0;
    char name[128],position[64];query_value(query,"file",name,sizeof name);query_value(query,"position",position,sizeof position);
    if(!strcmp(method,"GET")) {
        char *body=NULL;
        if(!strcmp(url,"/api/status"))body=status_json();
        else if(!strcmp(url,"/api/wifi"))body=wifi_json();
        else if(!strcmp(url,"/api/files"))body=files_json();
        else if(!strcmp(url,"/api/wave")&&valid_name(name))body=wave_json(name);
        if(body){response(fd,200,body);free(body);}else response(fd,404,"{\"error\":\"Unknown resource\"}");
        return;
    }
    if(strcmp(method,"POST")){response(fd,405,"{\"error\":\"Use POST\"}");return;}
    int status=200;const char *error=NULL;
    if(!strncmp(url,"/api/wifi/",10)) {
        status=wifi_submit(url+10,form,&error);
        explicit_bzero(request,sizeof request);
        if(error){char *body=NULL;size_t size=0;FILE *f=open_memstream(&body,&size);fputs("{\"error\":",f);json_string(f,error);fputs("}",f);fclose(f);response(fd,status,body);free(body);}
        else response(fd,200,"{\"ok\":true}");
        return;
    }
    pthread_mutex_lock(&S.mutex);
    if(!strcmp(url,"/api/poweroff")) {
        S.shutting_down=1;S.shutdown_failed=0;S.command=STOP;
        shutdown_was_recording=S.mode==RECORDING;
        shutdown_requested_ms=monotonic_ms();shutdown_requested=1;alive=0;
        fprintf(stderr,"E1_POWEROFF_REQUEST recording=%d\n",shutdown_was_recording);
    }
    else if(!strcmp(url,"/api/stop"))S.command=STOP;
    else if(!strcmp(url,"/api/record")) {
        if(S.mode!=IDLE||S.command!=NONE){status=409;error="Stop the current operation first";}
        else if(!S.capture_ready){status=503;error="Microphone is not ready";}
        else {S.error[0]=0;S.command=RECORD;}
    } else if(!strcmp(url,"/api/mark")) {
        if(S.mode!=RECORDING){status=409;error="Marks require an active recording";}
        else if(S.mark_count==MAX_MARKS){status=409;error="Maximum 256 marks per recording";}
        else {S.marks[S.mark_count++]=S.frames;save_marks();}
    } else if(!strcmp(url,"/api/play")) {
        if(!valid_name(name)){status=400;error="Invalid recording name";}
        else if(S.mode!=IDLE||S.command!=NONE){status=409;error="Stop the current operation first";}
        else {snprintf(S.requested_file,sizeof S.requested_file,"%s",name);S.requested_position=fmax(0,atof(position));S.command=PLAY;}
    } else if(!strcmp(url,"/api/seek")) {
        if(S.mode!=PLAYING){status=409;error="Playback is not active";}else S.seek=fmax(0,atof(position));
    } else {status=404;error="Unknown command";}
    pthread_mutex_unlock(&S.mutex);
    if(error){char buf[320];snprintf(buf,sizeof buf,"{\"error\":\"%s\"}",error);response(fd,status,buf);}else response(fd,200,"{\"ok\":true}");
}
static void save_poweroff_audit(void) {
    FILE *f=fopen("/userdata/e1-recorder/poweroff-last.json","w");
    if(!f){perror("Poweroff audit");return;}
    fprintf(f,"{\"testOnly\":%s,\"hadActiveRecording\":%s,\"recordingFinalized\":%s,\"requestedAtMs\":%llu,\"audioClosedAtMs\":%llu,\"file\":",poweroff_test?"true":"false",shutdown_was_recording?"true":"false",record_fd<0?"true":"false",(unsigned long long)shutdown_requested_ms,(unsigned long long)monotonic_ms());
    json_string(f,S.name);fprintf(f,",\"frames\":%llu,\"marks\":",(unsigned long long)S.frames);print_marks(f);fputs("}\n",f);
    fflush(f);fsync(fileno(f));fclose(f);
}
static int prepare_panel(void) {
    int fd=open("/proc/simplae_panel",O_WRONLY);
    if(fd<0)return -1;
    int result=write_all(fd,"1",1);int saved=errno;close(fd);errno=saved;return result;
}
int main(int argc,char **argv) {
    if(argc>=2)snprintf(storage,sizeof storage,"%s",argv[1]);
    if(argc==3&&!strcmp(argv[2],"--poweroff-test"))poweroff_test=1;
    else if(argc>2){fprintf(stderr,"Usage: recorderd [STORAGE] [--poweroff-test]\n");return 2;}
    if(mkdir(storage,0770)&&errno!=EEXIST){perror(storage);return 1;}
    signal(SIGTERM,signal_stop);signal(SIGINT,signal_stop);signal(SIGPIPE,SIG_IGN);envelope_reset(&S.wave);
    int server=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0),one=1;setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(PORT),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    if(server<0||bind(server,(void*)&addr,sizeof addr)||listen(server,8)){perror("Recorder API");return 1;}
    fprintf(stderr,"E1_RECORDER_BACKEND_READY %s storage=%s\n",VERSION,storage);
    wifi_start();
    for(;;) {
        pthread_t thread;if(pthread_create(&thread,NULL,audio_thread,NULL)){perror("Audio thread");close(server);return 1;}
        while(alive) {
            fd_set set;FD_ZERO(&set);FD_SET(server,&set);struct timeval wait={0,200000};
            int r=select(server+1,&set,NULL,NULL,&wait);if(r>0){int fd=accept4(server,NULL,NULL,SOCK_CLOEXEC);if(fd>=0){handle_client(fd);close(fd);}}
        }
        pthread_join(thread,NULL);
        wifi_stop();
        if(!shutdown_requested)break;
        /* The audio thread has finalized WAV/cue data and closed both PCMs. */
        save_poweroff_audit();sync();
        if(poweroff_test){fprintf(stderr,"E1_POWEROFF_TEST_COMPLETE audio_closed=1 recording_saved=1\n");break;}
        int result=prepare_panel();
        if(!result) {
            sync();fprintf(stderr,"E1_POWEROFF_EXEC /sbin/poweroff\n");fflush(stderr);
            result=system("/sbin/poweroff");
            if(!result) {
                /* Let init run the factory rcK/unmount sequence. */
                signal(SIGTERM,SIG_DFL);signal(SIGINT,SIG_DFL);
                for(;;)pause();
            }
        }
        fprintf(stderr,"E1_POWEROFF_FAILED result=%d errno=%d\n",result,errno);
        int panel=open("/proc/simplae_panel",O_WRONLY);if(panel>=0){write_all(panel,"0",1);close(panel);}
        pthread_mutex_lock(&S.mutex);
        S.shutting_down=0;S.shutdown_failed=1;S.command=NONE;
        snprintf(S.error,sizeof S.error,"Poweroff failed; release POWER and try again");
        pthread_mutex_unlock(&S.mutex);
        shutdown_requested=0;alive=1;
        wifi_start();
    }
    close(server);fprintf(stderr,"E1_RECORDER_SHUTDOWN\n");return 0;
}

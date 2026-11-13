/* Diagnostic capture only. Never changes mixer, firmware, or audio routes. */
#define _POSIX_C_SOURCE 200809L
#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include "../audio_format.h"
static void put16(FILE *f,unsigned v){fputc(v,f);fputc(v>>8,f);}
static void put32(FILE *f,unsigned v){put16(f,v);put16(f,v>>16);}
int main(int argc,char **argv) {
    if(argc!=3)return 2;
    unsigned seconds=(unsigned)strtoul(argv[2],NULL,10);
    if(seconds<3||seconds>60)return 2;
    alarm(seconds+8);
    snd_pcm_t *pcm=NULL;int e=snd_pcm_open(&pcm,"hw:2,0",SND_PCM_STREAM_CAPTURE,0);
    if(e>=0)e=snd_pcm_set_params(pcm,SND_PCM_FORMAT_S32_LE,SND_PCM_ACCESS_RW_INTERLEAVED,8,48000,0,100000);
    if(e<0){fprintf(stderr,"Capture: %s\n",snd_strerror(e));if(pcm)snd_pcm_close(pcm);return 1;}
    int32_t data[480*8];uint8_t packed[480*24];unsigned frames=seconds*48000,warm=0;
    e=snd_pcm_start(pcm);if(e<0)goto bad;
    while(warm<12000){snd_pcm_sframes_t n=snd_pcm_readi(pcm,data,480);if(n<=0){e=(int)n;goto bad;}warm+=(unsigned)n;}
    FILE *f=fopen(argv[1],"wb");if(!f)goto bad;
    fwrite("RIFF",1,4,f);put32(f,36+frames*24);fwrite("WAVEfmt ",1,8,f);put32(f,16);
    put16(f,1);put16(f,8);put32(f,48000);put32(f,48000*24);put16(f,24);put16(f,24);
    fwrite("data",1,4,f);put32(f,frames*24);
    while(frames) {
        unsigned want=frames>480?480:frames;
        snd_pcm_sframes_t n=snd_pcm_readi(pcm,data,want);
        if(n<=0){e=(int)n;fclose(f);goto bad;}
        for(int j=0;j<n*8;j++){uint32_t v=(uint32_t)data[j]>>8;packed[j*3]=v;packed[j*3+1]=v>>8;packed[j*3+2]=v>>16;}
        if(fwrite(packed,24,(size_t)n,f)!=(size_t)n){e=-EIO;fclose(f);goto bad;}frames-=(unsigned)n;
    }
    int failed=fflush(f)||fsync(fileno(f));failed|=fclose(f)!=0;
    snd_pcm_drop(pcm);snd_pcm_close(pcm);
    if(failed)return 1;
    printf("ARRAY_CAPTURE_PASS rate=48000 channels=8 bits=24 seconds=%u xruns=0\n",seconds);return 0;
bad:
    fprintf(stderr,"Array capture incomplete: %s\n",snd_strerror(e));snd_pcm_close(pcm);return 1;
}

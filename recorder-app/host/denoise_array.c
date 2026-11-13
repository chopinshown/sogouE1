/* Offline A/B render using the exact device DSP. Input: capture_array.py WAV. */
#include "../noise_reduce.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static E1NoiseReducer nr;
static unsigned u32(const unsigned char *p){return (unsigned)p[0]|(unsigned)p[1]<<8|(unsigned)p[2]<<16|(unsigned)p[3]<<24;}
static void put32(unsigned char *p,unsigned v){for(unsigned j=0;j<4;j++)p[j]=v>>(j*8);}
int main(int argc,char **argv) {
    if(argc!=4){fprintf(stderr,"Usage: denoise-array array.wav original.wav reduced.wav\n");return 2;}
    FILE *in=fopen(argv[1],"rb");unsigned char h[44];
    if(!in||fread(h,1,44,in)!=44||memcmp(h,"RIFF",4)||memcmp(h+8,"WAVEfmt ",8)||u32(h+16)!=16||h[20]!=1||h[22]!=8||u32(h+24)!=48000||h[34]!=24||memcmp(h+36,"data",4)){fprintf(stderr,"Expected 48 kHz eight-channel PCM24 WAV\n");if(in)fclose(in);return 1;}
    unsigned frames=u32(h+40)/24;
    if(frames<48000*3||frames>48000*60){fclose(in);return 1;}
    FILE *dry=fopen(argv[2],"wb"),*wet=fopen(argv[3],"wb");if(!dry||!wet){fclose(in);return 1;}
    h[22]=2;h[32]=6;put32(h+4,36+frames*6);put32(h+28,48000*6);put32(h+40,frames*6);
    fwrite(h,1,44,dry);fwrite(h,1,44,wet);
    e1_nr_init(&nr);e1_nr_learn(&nr);unsigned emitted=0;
    for(unsigned i=0;i<frames+E1_NR_DELAY;i++) {
        int32_t raw[8]={0},out[2];unsigned char b[24]={0};
        if(i<frames) {
            if(fread(b,1,24,in)!=24)return 1;
            for(unsigned c=0;c<8;c++){uint32_t v=(uint32_t)b[c*3]<<8|(uint32_t)b[c*3+1]<<16|(uint32_t)b[c*3+2]<<24;memcpy(raw+c,&v,4);}
            if(fwrite(b+18,1,6,dry)!=6)return 1;
        }
        if(nr.ready)nr.enabled=1;
        e1_nr_process(&nr,raw,out,1);
        if(i>=E1_NR_DELAY) {
            for(unsigned c=0;c<2;c++){uint32_t v=(uint32_t)out[c]>>8;b[c*3]=v;b[c*3+1]=v>>8;b[c*3+2]=v>>16;}
            if(fwrite(b,1,6,wet)!=6)return 1;
            emitted++;
        }
    }
    int failed=ferror(in)||ferror(dry)||ferror(wet);failed|=fclose(in)!=0;failed|=fclose(dry)!=0;failed|=fclose(wet)!=0;
    if(failed||emitted!=frames||!nr.ready)return 1;
    printf("ARRAY_DENOISE_PASS frames=%u learnedHops=%u delayTrimmed=%u\n",frames,nr.learned,E1_NR_DELAY);return 0;
}

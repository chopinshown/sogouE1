/* Run on the E1. Uses temporary files and synthetic samples, opens no PCMs. */
#undef NDEBUG
#define main e1_recorder_main
#include "../recorderd.c"
#undef main
#include <assert.h>
int main(void) {
    snprintf(storage,sizeof storage,"/tmp/e1-nr-record-%ld",(long)getpid());assert(!mkdir(storage,0700));
    for(unsigned test=0;test<3;test++) {
        unsigned total=test==0?73:test==1?12345:48000;
        char path[800];snprintf(S.name,sizeof S.name,"REC-test%u.wav",test);path_for(path,sizeof path,S.name,NULL);
        record_fd=open(path,O_CREAT|O_EXCL|O_RDWR,0600);assert(record_fd>=0);
        uint8_t header[44];wave_header(header,0,0);assert(!write_all(record_fd,header,44));
        e1_nr_init(&NR);NR.enabled=NR.ready=1;NR.mix=1;
        S.frames=0;S.mark_count=1;S.marks[0]=total/2;S.xruns=0;envelope_reset(&S.wave);
        record_nr=1;record_skip=E1_NR_DELAY;record_input_frames=0;
        int32_t input[FRAMES*8],output[FRAMES*2];
        for(unsigned t=0;t<total;) {
            unsigned n=total-t;if(n>FRAMES)n=FRAMES;
            memset(input,0,sizeof input);
            for(unsigned i=0;i<n;i++) {
                input[i*8+6]=(int32_t)(sinf((t+i)*.073f)*2000000)*256;
                input[i*8+7]=(int32_t)(cosf((t+i)*.041f)*1000000)*256;
            }
            e1_nr_process(&NR,input,output,n);assert(!append_record(output,n));
            t+=n;record_input_frames+=n;
        }
        finish_record();assert(S.frames==total);
        FILE *f=fopen(path,"rb");assert(f);off_t offset;uint64_t frames;unsigned rate;
        assert(!read_wave(f,&offset,&frames,&rate));assert(frames==total&&rate==48000);
        int max_error=0;
        for(unsigned i=0;i<total;i++)for(unsigned c=0;c<2;c++) {
            uint8_t b[3];assert(fread(b,1,3,f)==3);int32_t v=b[0]|b[1]<<8|b[2]<<16;if(v&0x800000)v|=~0xffffff;
            int32_t expected=c?(int32_t)(cosf(i*.041f)*1000000):(int32_t)(sinf(i*.073f)*2000000);
            int error=abs(v-expected);if(error>max_error)max_error=error;
        }
        assert(max_error<=3);fclose(f);unlink(path);
        path_for(path,sizeof path,S.name,".marks.json");unlink(path);path_for(path,sizeof path,S.name,".wave.json");unlink(path);
        printf("NR_RECORD_PASS input=%u output=%llu maxError24bit=%d; leading delay trimmed, tail flushed, cue retained\n",total,(unsigned long long)S.frames,max_error);
    }
    rmdir(storage);return 0;
}

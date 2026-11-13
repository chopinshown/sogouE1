#include "noise_reduce.h"
#include "audio_format.h"
#include <limits.h>
#include <math.h>
#include <string.h>

#define PI 3.14159265358979323846
static void fft(E1NoiseReducer *s, E1Complex *a, int inverse) {
    for(unsigned i=0;i<E1_NR_N;i++) {
        unsigned j=s->reverse[i];
        if(j>i){E1Complex t=a[i];a[i]=a[j];a[j]=t;}
    }
    for(unsigned n=2;n<=E1_NR_N;n*=2) {
        for(unsigned i=0;i<E1_NR_N;i+=n)for(unsigned j=0;j<n/2;j++) {
            E1Complex w=s->twiddle[j*E1_NR_N/n], b=a[i+j+n/2], u=a[i+j];
            if(inverse)w.im=-w.im;
            float re=b.re*w.re-b.im*w.im, im=b.re*w.im+b.im*w.re;
            a[i+j]=(E1Complex){u.re+re,u.im+im};
            a[i+j+n/2]=(E1Complex){u.re-re,u.im-im};
        }
    }
    if(inverse)for(unsigned i=0;i<E1_NR_N;i++){a[i].re/=E1_NR_N;a[i].im/=E1_NR_N;}
}
void e1_nr_reset_stream(E1NoiseReducer *s) {
    memset(s->input,0,sizeof s->input);
    memset(s->overlap,0,sizeof s->overlap);
    memset(s->output,0,sizeof s->output);
    memset(s->dry,0,sizeof s->dry);
    memset(s->smooth,0,sizeof s->smooth);
    for(unsigned k=0;k<E1_NR_BINS;k++)s->gain[k]=1;
    s->fill=s->output_at=s->dry_at=0;s->reduction_db=s->mix=0;
}
void e1_nr_init(E1NoiseReducer *s) {
    memset(s,0,sizeof *s);
    for(unsigned i=0;i<E1_NR_N;i++) {
        s->window[i]=sinf((float)(PI*(i+.5)/E1_NR_N));
        unsigned r=0;for(unsigned j=0;j<9;j++)r=(r<<1)|((i>>j)&1);
        s->reverse[i]=r;
    }
    for(unsigned i=0;i<E1_NR_N/2;i++)s->twiddle[i]=(E1Complex){cosf((float)(-2*PI*i/E1_NR_N)),sinf((float)(-2*PI*i/E1_NR_N))};
    e1_nr_reset_stream(s);
}
void e1_nr_learn(E1NoiseReducer *s) {
    s->learned=0;s->ready=0;s->learning=1;s->enabled=0;
    memset(s->profile,0,sizeof s->profile);
    e1_nr_reset_stream(s);
}
static float median(float *x,unsigned n) {
    for(unsigned i=1;i<n;i++){float v=x[i];unsigned j=i;while(j&&x[j-1]>v){x[j]=x[j-1];j--;}x[j]=v;}
    return n?(n%2?x[n/2]:(x[n/2-1]+x[n/2])*.5f):1;
}
static void block(E1NoiseReducer *s) {
    int learn=s->learning;
    for(unsigned c=0;c<8;c++) {
        for(unsigned i=0;i<E1_NR_N;i++)s->spectrum[c][i]=(E1Complex){s->input[c][i]*s->window[i],0};
        fft(s,s->spectrum[c],0);
        for(unsigned k=0;k<E1_NR_BINS;k++) {
            E1Complex v=s->spectrum[c][k];float p=v.re*v.re+v.im*v.im;
            s->smooth[c][k]=.8f*s->smooth[c][k]+.2f*p;
            /* The first window includes startup padding; never learn it. */
            if(learn&&s->learned)s->profile[c][k]+=p;
        }
        memmove(s->input[c],s->input[c]+E1_NR_HOP,sizeof(float)*E1_NR_HOP);
    }
    if(learn&&++s->learned==E1_NR_LEARN_HOPS+1) {
        for(unsigned c=0;c<8;c++)for(unsigned k=0;k<E1_NR_BINS;k++)s->profile[c][k]/=E1_NR_LEARN_HOPS;
        double main=0,aux=0;
        for(unsigned k=0;k<E1_NR_BINS;k++){main+=s->profile[6][k]+s->profile[7][k];for(unsigned c=0;c<6;c++)aux+=s->profile[c][k];}
        s->ready=main>1e-10&&aux>1e-10;s->learning=0;
    }
    float target[E1_NR_BINS];
    for(unsigned k=0;k<E1_NR_BINS;k++) {
        target[k]=1;
        if(s->ready&&!learn) {
            float ratios[6];unsigned count=0;
            for(unsigned c=0;c<6;c++)if(s->profile[c][k]>1e-14)ratios[count++]=s->smooth[c][k]/s->profile[c][k];
            /* A body mic also hears speech. Bound tracking to +/-6 dB of the
             * calibrated noise, rather than treating that speech as noise. */
            float scale=fminf(4,fmaxf(.25f,median(ratios,count)));
            float noise=(float)((s->profile[6][k]+s->profile[7][k])*.5)*scale;
            /* One shared gain protects the relative L/R phase and balance. */
            float main=fmaxf(s->smooth[6][k],s->smooth[7][k]);
            target[k]=fmaxf(.25118864f,1-noise/(main+1e-20f));
        }
    }
    float total=0;
    for(unsigned k=0;k<E1_NR_BINS;k++) {
        float v=(target[k]*2+target[k?k-1:k]+target[k+1<E1_NR_BINS?k+1:k])*.25f;
        float a=v>s->gain[k]?.6f:.08f;
        s->gain[k]+=a*(v-s->gain[k]);total+=s->gain[k]*s->gain[k];
    }
    s->reduction_db=-10*log10f(fmaxf(1e-20f,total/E1_NR_BINS));
    for(unsigned c=0;c<2;c++) {
        E1Complex *a=s->spectrum[c+6];
        for(unsigned k=0;k<E1_NR_N;k++){float g=s->gain[k<E1_NR_BINS?k:E1_NR_N-k];a[k].re*=g;a[k].im*=g;}
        fft(s,a,1);
        for(unsigned i=0;i<E1_NR_HOP;i++) {
            s->output[c][i]=s->overlap[c][i]+a[i].re*s->window[i];
            s->overlap[c][i]=a[i+E1_NR_HOP].re*s->window[i+E1_NR_HOP];
        }
    }
}
static int32_t sample(float x) {
    double v=(double)x*2147483648.0;
    if(v>=INT32_MAX)return INT32_MAX;
    if(v<=INT32_MIN)return INT32_MIN;
    return (int32_t)v;
}
void e1_nr_process(E1NoiseReducer *s,const int32_t *input,int32_t *stereo,unsigned frames) {
    for(unsigned i=0;i<frames;i++) {
        float target=s->enabled&&s->ready?1:0;
        s->mix+=fmaxf(-1/960.0f,fminf(1/960.0f,target-s->mix));
        for(unsigned c=0;c<2;c++) {
            float dry=s->dry[c][s->dry_at];
            s->dry[c][s->dry_at]=input[i*8+c+6]/2147483648.0f;
            /* Delay-match A/B, including while learning. */
            stereo[i*2+c]=sample(dry+s->mix*(s->output[c][s->output_at]-dry));
        }
        s->dry_at=(s->dry_at+1)%E1_NR_N;
        for(unsigned c=0;c<8;c++)s->input[c][E1_NR_HOP+s->fill]=input[i*8+c]/2147483648.0f;
        s->output_at++;
        if(++s->fill==E1_NR_HOP){block(s);s->fill=s->output_at=0;}
    }
}

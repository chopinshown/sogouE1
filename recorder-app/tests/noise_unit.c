#undef NDEBUG
#include "../noise_reduce.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PI 3.14159265358979323846
static E1NoiseReducer nr;
static uint32_t rng=1234567;
static float noise(void){rng=rng*1664525+1013904223;return ((rng>>8)/8388608.0f-1)*.02f;}
static int32_t pack(float v){return (int32_t)(v*8388608)*256;}
static void frame(int32_t *v,unsigned t,int speech) {
    float n=noise(), s=speech?.12f*sinf((float)(2*PI*1500*t/48000)):0;
    for(unsigned c=0;c<6;c++)v[c]=pack(n*(.5f+c*.15f)+s*.4f);
    v[6]=pack(n+s);v[7]=pack((n+s)*.5f);
}
int main(void) {
    e1_nr_init(&nr);
    int32_t in[8*997],out[2*997];
    static int32_t original[5000*2],result[5000*2];
    for(unsigned t=0;t<5000;) {
        unsigned n=t%997+1;if(n>5000-t)n=5000-t;
        for(unsigned j=0;j<n;j++){frame(in+j*8,t+j,1);original[(t+j)*2]=in[j*8+6];original[(t+j)*2+1]=in[j*8+7];}
        e1_nr_process(&nr,in,out,n);memcpy(result+t*2,out,n*2*sizeof *out);t+=n;
    }
    for(unsigned t=E1_NR_DELAY;t<5000;t++)for(unsigned c=0;c<2;c++)assert(result[t*2+c]==original[(t-E1_NR_DELAY)*2+c]);
    puts("NR_BYPASS_PASS exact 24-bit main pair; constant 512-frame delay; arbitrary chunk sizes");
    e1_nr_learn(&nr);
    for(unsigned t=0;t<48000*3;t+=480){for(unsigned j=0;j<480;j++)frame(in+j*8,t+j,0);e1_nr_process(&nr,in,out,480);}
    assert(nr.ready&&!nr.learning);
    double raw=0,wet=0;nr.enabled=1;
    for(unsigned t=0;t<48000*2;t+=480) {
        for(unsigned j=0;j<480;j++)frame(in+j*8,t+j,0);
        e1_nr_process(&nr,in,out,480);
        if(t>48000)for(unsigned j=0;j<480;j++){raw+=pow(in[j*8+6]/2147483648.0,2);wet+=pow(out[j*2]/2147483648.0,2);}
    }
    double attenuation=10*log10(raw/wet);assert(attenuation>5&&attenuation<13);
    printf("NR_NOISE_PASS stationary-noise attenuation %.2f dB\n",attenuation);
    double re=0,im=0,ratio_error=0;
    for(unsigned t=0;t<48000*2;t+=480) {
        for(unsigned j=0;j<480;j++)frame(in+j*8,t+j,1);
        e1_nr_process(&nr,in,out,480);
        if(t>=48000)for(unsigned j=0;j<480;j++) {
            double a=2*PI*1500*((int)t+(int)j-E1_NR_DELAY)/48000;
            double x=out[j*2]/2147483648.0;re+=x*cos(a);im+=x*sin(a);
            ratio_error=fmax(ratio_error,fabs(out[j*2]/2147483648.0-2*out[j*2+1]/2147483648.0));
        }
    }
    double amplitude=2*hypot(re,im)/48000;assert(amplitude>.105&&amplitude<.13);assert(ratio_error<1e-5);
    printf("NR_SPEECH_PASS tone amplitude %.6f (input 0.12), stereo shared-gain error %.8f\n",amplitude,ratio_error);
    /* Hold main inputs fixed, change only auxiliary power. This catches a
     * suppressor that accidentally ignores the six reference channels. */
    static E1NoiseReducer quiet_aux,loud_aux;
    quiet_aux=nr;loud_aux=nr;quiet_aux.enabled=loud_aux.enabled=1;
    double quiet_power=0,loud_power=0;
    for(unsigned t=0;t<48000;t+=480) {
        for(unsigned j=0;j<480;j++)frame(in+j*8,t+j,0);
        int32_t changed[8*480],loud[2*480];memcpy(changed,in,sizeof changed);
        for(unsigned j=0;j<480;j++)for(unsigned c=0;c<6;c++){in[j*8+c]/=2;changed[j*8+c]*=2;}
        e1_nr_process(&quiet_aux,in,out,480);e1_nr_process(&loud_aux,changed,loud,480);
        if(t>24000)for(unsigned j=0;j<480;j++){quiet_power+=pow(out[j*2]/2147483648.0,2);loud_power+=pow(loud[j*2]/2147483648.0,2);}
    }
    double reference_effect=10*log10(quiet_power/loud_power);assert(reference_effect>3);
    printf("NR_AUX_PASS changing only six auxiliary inputs changes suppression by %.2f dB\n",reference_effect);
    e1_nr_init(&nr);e1_nr_learn(&nr);memset(in,0,sizeof in);
    for(unsigned t=0;t<48000*3;t+=480)e1_nr_process(&nr,in,out,480);
    assert(!nr.ready&&!nr.learning);
    puts("NR_SILENCE_PASS invalid calibration stays in bypass");
    return 0;
}

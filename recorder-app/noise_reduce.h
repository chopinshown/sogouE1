#ifndef E1_NOISE_REDUCE_H
#define E1_NOISE_REDUCE_H
#include <stdint.h>
#define E1_NR_N 512
#define E1_NR_HOP 256
#define E1_NR_BINS (E1_NR_N / 2 + 1)
/* Constant delay in both dry and wet modes: 512 frames / 48 kHz. */
#define E1_NR_DELAY E1_NR_N
#define E1_NR_LEARN_HOPS 375
typedef struct { float re, im; } E1Complex;
typedef struct {
    int enabled, ready, learning;
    unsigned learned, fill, output_at;
    float input[8][E1_NR_N], window[E1_NR_N];
    float overlap[2][E1_NR_HOP], output[2][E1_NR_HOP];
    float dry[2][E1_NR_N];
    unsigned dry_at;
    double profile[8][E1_NR_BINS];
    float smooth[8][E1_NR_BINS], gain[E1_NR_BINS];
    E1Complex spectrum[8][E1_NR_N], twiddle[E1_NR_N / 2];
    unsigned reverse[E1_NR_N];
    float reduction_db, mix;
} E1NoiseReducer;
void e1_nr_init(E1NoiseReducer *s);
void e1_nr_reset_stream(E1NoiseReducer *s);
void e1_nr_learn(E1NoiseReducer *s);
void e1_nr_process(E1NoiseReducer *s, const int32_t *input, int32_t *stereo, unsigned frames);
#endif

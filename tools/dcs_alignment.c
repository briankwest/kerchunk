/*
 * dcs_alignment.c — Show WHERE each code matches in the bit stream.
 * Helps understand alignment patterns.
 *
 * cc -O2 -o dcs_alignment tools/dcs_alignment.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define POLY 0xC75u

static uint32_t golay_encode(uint16_t d) {
    uint32_t cw = (uint32_t)(d & 0xFFF) << 11, r = cw;
    for (int i = 11; i >= 0; i--)
        if (r & ((uint32_t)1 << (i+11))) r ^= (POLY << i);
    return cw | (r & 0x7FF);
}
static int golay_check(uint32_t cw) {
    uint32_t r = cw & 0x7FFFFF;
    for (int i = 11; i >= 0; i--)
        if (r & ((uint32_t)1 << (i+11))) r ^= (POLY << i);
    return r == 0;
}

static const uint16_t dcs_codes[] = {
    023,025,026,031,032,036,043,047,051,053,054,065,071,072,073,074,
    0114,0115,0116,0122,0125,0131,0132,0134,0143,0145,0152,0155,0156,
    0162,0165,0172,0174,0205,0212,0223,0225,0226,0243,0244,0245,0246,
    0251,0252,0255,0261,0263,0265,0266,0271,0274,0306,0311,0315,0325,
    0331,0332,0343,0346,0351,0356,0364,0365,0371,0411,0412,0413,0423,
    0431,0432,0445,0446,0452,0454,0455,0462,0464,0465,0466,0503,0506,
    0516,0523,0526,0532,0546,0565,0606,0612,0624,0627,0631,0632,0654,
    0662,0664,0703,0712,0723,0731,0732,0734,0743,0754
};
#define NC (sizeof(dcs_codes)/sizeof(dcs_codes[0]))

static int code_label(uint16_t c) {
    int r=0,m=1; while(c>0){r+=(c&7)*m;c>>=3;m*=10;} return r;
}

static int16_t *read_wav(const char *p, int *n) {
    FILE *f=fopen(p,"rb"); if(!f) return NULL;
    uint8_t h[44]; fread(h,1,44,f);
    *n=(h[40]|(h[41]<<8)|(h[42]<<16)|(h[43]<<24))/2;
    int16_t *b=malloc(*n*2); fread(b,2,*n,f); fclose(f); return b;
}

int main(int argc, char **argv) {
    if (argc<2) { fprintf(stderr,"Usage: %s wav\n",argv[0]); return 1; }
    int n; int16_t *s = read_wav(argv[1],&n);
    if (!s) return 1;
    printf("Read %d samples\n", n);

    /* Build codeword table */
    uint32_t cws[NC];
    for (int i=0;i<(int)NC;i++) cws[i]=golay_encode(0x800|(dcs_codes[i]&0x1FF));

    /* Extract bits with PLL (25% correction) */
    double wc=tan(M_PI*300.0/8000), wc2=wc*wc, sq=1.414213562;
    double nm=1.0/(1.0+sq*wc+wc2);
    double b0=wc2*nm,b1=2*b0,b2=b0,a1=2*(wc2-1)*nm,a2=(1-sq*wc+wc2)*nm;
    double x1=0,x2=0,y1=0,y2=0;
    double pll_ph=0, pll_inc=134.4/8000;
    int pb=0;

    int bits[8192]; int nb=0;
    for (int i=0;i<n && nb<8192;i++) {
        double x=(double)s[i];
        double y=b0*x+b1*x1+b2*x2-a1*y1-a2*y2;
        x2=x1;x1=x;y2=y1;y1=y;
        int bit=y>500?1:y<-500?0:pb;
        if(bit!=pb){double e=pll_ph-0.5;pll_ph-=e*0.25;}
        pb=bit;
        double pp=pll_ph; pll_ph+=pll_inc;
        if(pp<0.5&&pll_ph>=0.5) bits[nb++]=bit;
        if(pll_ph>=1.0)pll_ph-=1.0;
    }
    printf("%d bits extracted\n\n", nb);

    /* For codes 026 and 464, show every bit position where they match */
    int targets[] = {26, 464};
    for (int t=0;t<2;t++) {
        int idx=-1;
        for(int i=0;i<(int)NC;i++) if(code_label(dcs_codes[i])==targets[t]){idx=i;break;}
        if(idx<0) continue;

        printf("DCS %03d matches (exact Golay) at bit positions:\n  ", targets[t]);
        uint32_t sr=0;
        int count=0;
        for(int b=0;b<nb;b++){
            sr=((sr>>1)|((uint32_t)bits[b]<<22))&0x7FFFFF;
            if(b<22) continue;
            if(golay_check(sr)){
                uint16_t d=(uint16_t)(sr>>11);
                if((d&0xE00)==0x800){
                    int c9=d&0x1FF;
                    if(c9==dcs_codes[idx]){printf("%d ",b);count++;}
                }
            }
            /* Also inverted */
            uint32_t comp=sr^0x7FFFFF;
            if(golay_check(comp)){
                uint16_t d=(uint16_t)(comp>>11);
                if((d&0xE00)==0x800){
                    int c9=d&0x1FF;
                    if(c9==dcs_codes[idx]){printf("%d(I) ",b);count++;}
                }
            }
        }
        printf("\n  Total: %d matches\n\n", count);
    }

    /* Show spacing between consecutive 026 matches */
    printf("DCS 026 match spacing:\n  ");
    {
        int idx=-1;
        for(int i=0;i<(int)NC;i++) if(code_label(dcs_codes[i])==26){idx=i;break;}
        uint32_t sr=0; int prev=-1;
        for(int b=0;b<nb;b++){
            sr=((sr>>1)|((uint32_t)bits[b]<<22))&0x7FFFFF;
            if(b<22) continue;
            int match=0;
            if(golay_check(sr)){uint16_t d=(uint16_t)(sr>>11);if((d&0xE00)==0x800&&(d&0x1FF)==dcs_codes[idx])match=1;}
            uint32_t comp=sr^0x7FFFFF;
            if(golay_check(comp)){uint16_t d=(uint16_t)(comp>>11);if((d&0xE00)==0x800&&(d&0x1FF)==dcs_codes[idx])match=1;}
            if(match){
                if(prev>=0) printf("%d ", b-prev);
                prev=b;
            }
        }
    }
    printf("\n");

    free(s);
    return 0;
}

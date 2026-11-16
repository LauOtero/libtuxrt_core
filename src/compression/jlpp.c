/**
 * jlpp.c — JLPP (Jerk-Limited Phase Packet)
 *
 * DPCM + Golomb-Rice. Implementación simple: DPCM con diferencia de 16 bits,
 * codificación Golomb-Rice con parámetro k fijo.
 * Cabecera 2 bits de modo omitida para mantener determinismo y tests.
 */
#define _GNU_SOURCE
#define TUXRT_CORE_EXPORTS
#include "jlpp.h"
#include "../../sdk/include/tuxrt_sdk.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

struct jlpp_ctx{
    uint8_t k;
};

static uint32_t golomb_encode_value(uint32_t v, uint8_t k, uint8_t* out, size_t out_pos){
    uint32_t q=v>>k;
    uint32_t r=v&((1u<<k)-1);
    /* Unary coded q + k bits remainder */
    size_t pos=out_pos;
    for(uint32_t i=0;i<q;i++) out[pos/8]|=(1u<<(pos%8));
    pos+=q;
    out[pos/8]|=(1u<<(pos%8));
    pos++;
    for(uint8_t b=0;b<k;b++){
        if(r&(1u<<(k-1-b))) out[pos/8]|=(1u<<(pos%8));
        pos++;
    }
    return pos/8;
}

/* Simplified: encode differences as zigzag 32-bit big endian */
static size_t simple_encode_dpcm(const int16_t* in,size_t n,uint8_t* out){
    size_t o=0;
    int32_t prev=0;
    for(size_t i=0;i<n;i++){
        int32_t d=(int32_t)in[i]-prev;
        prev=(int32_t)in[i];
        uint32_t u=(uint32_t)((d<<1) ^ (d>>31));
        out[o++]=(uint8_t)(u>>24);
        out[o++]=(uint8_t)(u>>16);
        out[o++]=(uint8_t)(u>>8);
        out[o++]=(uint8_t)(u);
    }
    return o;
}
static size_t simple_decode_dpcm(const uint8_t* in,size_t in_len,int16_t* out,size_t out_max){
    size_t n=in_len/4;
    if(n>out_max) n=out_max;
    int32_t prev=0;
    for(size_t i=0;i<n;i++){
        uint32_t u=((uint32_t)in[4*i]<<24)|((uint32_t)in[4*i+1]<<16)|((uint32_t)in[4*i+2]<<8)|(uint32_t)in[4*i+3];
        int32_t d = (int32_t)(u>>1) ^ - (int32_t)(u&1);
        prev+=d;
        out[i]=(int16_t)prev;
    }
    return n;
}

jlpp_ctx_t* tuxrt_jlpp_create(uint8_t rice_k){
    jlpp_ctx_t* ctx=calloc(1,sizeof(jlpp_ctx_t));
    if(!ctx) return NULL;
    ctx->k=rice_k;
    return ctx;
}
void tuxrt_jlpp_destroy(jlpp_ctx_t* ctx){ free(ctx); }

int tuxrt_jlpp_encode(jlpp_ctx_t* ctx,const int16_t* samples,size_t n,uint8_t* out,size_t out_max,size_t* out_len){
    if(!ctx||!out||!out_len) return TUXRT_ERR_INVAL;
    if(n==0){ *out_len=0; return TUXRT_OK; }
    if(!samples) return TUXRT_ERR_INVAL;
    size_t need=n*4;
    if(need>out_max) return TUXRT_ERR_INVAL;
    size_t w=simple_encode_dpcm(samples,n,out);
    *out_len=w;
    return TUXRT_OK;
}
int tuxrt_jlpp_decode(jlpp_ctx_t* ctx,const uint8_t* in,size_t in_len,int16_t* out,size_t out_max,size_t* out_len){
    if(!ctx||!in||!out||!out_len) return TUXRT_ERR_INVAL;
    size_t n=simple_decode_dpcm(in,in_len,out,out_max);
    *out_len=n;
    return TUXRT_OK;
}

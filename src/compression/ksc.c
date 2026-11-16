/**
 * ksc.c — KSC (Klipper Stream Compressor)
 *
 * 7 algoritmos según tipo de mensaje:
 * 0 DPC + Group Varint, 1 TurboPFor, 2 TurboBitByte, 3 Delta-of-Delta,
 * 4 ELF-KSC, 5 RLE+, 6 RAW/VLQ
 */
#define TUXRT_CORE_EXPORTS
#include "ksc.h"
#include "../../sdk/include/tuxrt_sdk.h"
#include <stdlib.h>
#include <string.h>

typedef enum {
    CODEC_DPC_GROUP_VARINT  = 0,
    CODEC_TURBOPFOR         = 1,
    CODEC_TURBOBITBYTE      = 2,
    CODEC_DELTA_OF_DELTA    = 3,
    CODEC_ELF_KSC           = 4,
    CODEC_RLE_PLUS          = 5,
    CODEC_RAW               = 6
} codec_id_t;

#define MSG_MOVE  0x01
#define MSG_STEP  0x02
#define MSG_SET_PIN 0x03
#define MSG_SET_PWM 0x04
#define MSG_SET_TEMP 0x05
#define MSG_TICK  0x06
#define MSG_SYNC  0x07

static int select_codec(uint8_t msg_type){
    switch(msg_type){
        case MSG_MOVE:    return CODEC_DPC_GROUP_VARINT;
        case MSG_STEP:    return CODEC_TURBOPFOR;
        case MSG_SET_PIN: return CODEC_RLE_PLUS;
        case MSG_SET_PWM: return CODEC_RLE_PLUS;
        case MSG_SET_TEMP:return CODEC_ELF_KSC;
        case MSG_TICK:    return CODEC_DELTA_OF_DELTA;
        case MSG_SYNC:    return CODEC_DELTA_OF_DELTA;
        default:          return CODEC_RAW;
    }
}

/* Helper VLQ */
static size_t vlq_encode(const uint8_t* in, size_t in_len, uint8_t* out){
    size_t o=0;
    for(size_t i=0;i<in_len;i++){
        uint32_t v=in[i];
        do{
            uint8_t b=v&0x7F;
            v>>=7;
            if(v) b|=0x80;
            out[o++]=b;
        }while(v);
    }
    return o;
}
static size_t vlq_decode(const uint8_t* in,size_t in_len,uint8_t* out,size_t out_max,size_t* out_len){
    size_t o=0,i=0;
    while(i<in_len && o<out_max){
        uint32_t v=0,shift=0;
        while(i<in_len){
            uint8_t b=in[i++];
            v|= (uint32_t)(b&0x7F)<<shift;
            shift+=7;
            if(!(b&0x80)) break;
        }
        out[o++]= (uint8_t)v;
    }
    *out_len=o; return o;
}

/* RLE+ */
static size_t rle_encode(const uint8_t* in,size_t in_len,uint8_t* out){
    size_t o=0,i=0;
    while(i<in_len){
        uint8_t v=in[i];
        size_t run=1;
        while(i+run<in_len && in[i+run]==v && run<255) run++;
        out[o++]= (uint8_t)run;
        out[o++]= v;
        i+=run;
    }
    return o;
}
static size_t rle_decode(const uint8_t* in,size_t in_len,uint8_t* out,size_t out_max,size_t* out_len){
    size_t o=0,pos=0;
    while(pos+1<=in_len && o<out_max){
        uint8_t run=in[pos++];
        uint8_t v=in[pos++];
        for(uint8_t k=0;k<run && o<out_max;k++) out[o++]=v;
    }
    *out_len=o; return o;
}

/* Deltas */
static size_t delta_encode(const uint8_t* in,size_t in_len,uint8_t* out){
    if(!in_len) return 0;
    out[0]=in[0];
    for(size_t i=1;i<in_len;i++) out[i]=(uint8_t)(in[i]-in[i-1]);
    return in_len;
}
static size_t delta_decode(const uint8_t* in,size_t in_len,uint8_t* out){
    if(!in_len) return 0;
    out[0]=in[0];
    for(size_t i=1;i<in_len;i++) out[i]=out[i-1]+in[i];
    return in_len;
}

/* Delta-of-Delta */
static size_t dod_encode(const uint8_t* in,size_t in_len,uint8_t* out){
    if(in_len<2) return delta_encode(in,in_len,out);
    out[0]=in[0]; out[1]=in[1]-in[0];
    for(size_t i=2;i<in_len;i++) out[i]=(uint8_t)((int16_t)(in[i]-in[i-1])-(int16_t)(in[i-1]-in[i-2]));
    return in_len;
}
static size_t dod_decode(const uint8_t* in,size_t in_len,uint8_t* out){
    if(in_len==0) return 0;
    out[0]=in[0];
    if(in_len>1) out[1]=out[0]+in[1];
    for(size_t i=2;i<in_len;i++){
        int16_t prev_diff = (int16_t)out[i-1] - (int16_t)out[i-2];
        int16_t cur_diff = prev_diff + (int16_t)in[i];
        out[i]= (uint8_t)((int16_t)out[i-1] + cur_diff);
    }
    return in_len;
}

/* Turbopfor stub: delta encode */
static size_t turbopfor_encode(const uint8_t* in,size_t in_len,uint8_t* out){ return delta_encode(in,in_len,out); }
static size_t turbopfor_decode(const uint8_t* in,size_t in_len,uint8_t* out){ return delta_decode(in,in_len,out); }

/* TurboBitByte stub: delta encode */
static size_t turbobitbyte_encode(const uint8_t* in,size_t in_len,uint8_t* out){ return delta_encode(in,in_len,out); }
static size_t turbobitbyte_decode(const uint8_t* in,size_t in_len,uint8_t* out){ return delta_decode(in,in_len,out); }

/* DPC+Group Varint stub: delta + vlq */
static size_t dpc_encode(const uint8_t* in,size_t in_len,uint8_t* tmp, uint8_t* out){
    size_t dlen=delta_encode(in,in_len,tmp);
    return vlq_encode(tmp,dlen,out);
}
static size_t dpc_decode(const uint8_t* in,size_t in_len,uint8_t* tmp,uint8_t* out,size_t out_max,size_t* out_len){
    size_t tmp_len; vlq_decode(in,in_len,tmp,out_max,&tmp_len);
    size_t n=delta_decode(tmp,tmp_len,out);
    *out_len=n; return n;
}

/* ELF-KSC stub: delta */
static size_t elf_encode(const uint8_t* in,size_t in_len,uint8_t* out){ return delta_encode(in,in_len,out); }
static size_t elf_decode(const uint8_t* in,size_t in_len,uint8_t* out){ return delta_decode(in,in_len,out); }

struct ksc_ctx{ int dummy; };

ksc_ctx_t* tuxrt_ksc_create(void){
    ksc_ctx_t* c=calloc(1,sizeof(ksc_ctx_t));
    return c;
}
void tuxrt_ksc_destroy(ksc_ctx_t* ctx){ free(ctx); }

int tuxrt_ksc_encode(ksc_ctx_t* ctx,uint8_t msg_type,const uint8_t* in,size_t in_len,uint8_t* out,size_t out_max,size_t* out_len){
    if(!ctx||!in||!out||!out_len) return TUXRT_ERR_INVAL;
    int codec=select_codec(msg_type);
    size_t written=0;
    uint8_t tmp[8192];
    switch(codec){
        case CODEC_RLE_PLUS:
            written=rle_encode(in,in_len,out);
            break;
        case CODEC_DPC_GROUP_VARINT:
            if(in_len>8192) return TUXRT_ERR_INVAL;
            written=dpc_encode(in,in_len,tmp,out);
            break;
        case CODEC_TURBOPFOR:
            written=turbopfor_encode(in,in_len,out);
            break;
        case CODEC_TURBOBITBYTE:
            written=turbobitbyte_encode(in,in_len,out);
            break;
        case CODEC_DELTA_OF_DELTA:
            written=dod_encode(in,in_len,out);
            break;
        case CODEC_ELF_KSC:
            written=elf_encode(in,in_len,out);
            break;
        case CODEC_RAW:
        default:
            if(in_len>out_max) return TUXRT_ERR_INVAL;
            memcpy(out,in,in_len);
            written=in_len;
            break;
    }
    if(written>out_max) return TUXRT_ERR_INVAL;
    *out_len=written;
    return TUXRT_OK;
}

int tuxrt_ksc_decode(ksc_ctx_t* ctx,uint8_t msg_type,const uint8_t* in,size_t in_len,uint8_t* out,size_t out_max,size_t* out_len){
    if(!ctx||!in||!out||!out_len) return TUXRT_ERR_INVAL;
    int codec=select_codec(msg_type);
    uint8_t tmp[8192];
    size_t n=0;
    switch(codec){
        case CODEC_RLE_PLUS:
            n=rle_decode(in,in_len,out,out_max,out_len);
            break;
        case CODEC_DPC_GROUP_VARINT:
            if(in_len>8192) return TUXRT_ERR_INVAL;
            n=dpc_decode(in,in_len,tmp,out,out_max,out_len);
            break;
        case CODEC_TURBOPFOR:
            n=turbopfor_decode(in,in_len,out);
            *out_len=n;
            break;
        case CODEC_TURBOBITBYTE:
            n=turbobitbyte_decode(in,in_len,out);
            *out_len=n;
            break;
        case CODEC_DELTA_OF_DELTA:
            n=dod_decode(in,in_len,out);
            *out_len=n;
            break;
        case CODEC_ELF_KSC:
            n=elf_decode(in,in_len,out);
            *out_len=n;
            break;
        case CODEC_RAW:
        default:
            if(in_len>out_max) return TUXRT_ERR_INVAL;
            memcpy(out,in,in_len);
            n=in_len;
            *out_len=n;
            break;
    }
    return TUXRT_OK;
}

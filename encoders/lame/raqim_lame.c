/* Raqim: minimal LAME wrapper for WebAssembly */
#include <stdlib.h>
#include "lame.h"
#define EXPORT __attribute__((visibility("default")))
typedef struct { lame_global_flags *gfp; float *l,*r; int cap; unsigned char *out; int outcap; } ENC;
EXPORT ENC* enc_init(int in_sr,int in_ch,int out_sr,int kbps,int quality,int force_joint){
  ENC *e=calloc(1,sizeof(ENC)); if(!e) return 0;
  e->gfp=lame_init(); if(!e->gfp){free(e);return 0;}
  /* force_joint=1: always 2-channel JOINT STEREO output (mono input duplicated)
     force_joint=0: LAME defaults (stereo in -> joint stereo, mono in -> mono) */
  lame_set_num_channels(e->gfp,force_joint?2:in_ch);
  lame_set_in_samplerate(e->gfp,in_sr);
  lame_set_out_samplerate(e->gfp,out_sr);
  if(force_joint) lame_set_mode(e->gfp,JOINT_STEREO);
  lame_set_VBR(e->gfp,vbr_off);
  lame_set_brate(e->gfp,kbps);
  if(quality>=0) lame_set_quality(e->gfp,quality);  /* 0 = best (slowest); -1 = LAME default (3) */
  lame_set_bWriteVbrTag(e->gfp,0);
  lame_set_write_id3tag_automatic(e->gfp,0);
  if(lame_init_params(e->gfp)<0){lame_close(e->gfp);free(e);return 0;}
  return e;
}
EXPORT float* enc_buf(ENC*e,int ch,int n){
  if(n>e->cap){ e->cap=n; e->l=realloc(e->l,n*sizeof(float)); e->r=realloc(e->r,n*sizeof(float));
    e->outcap=(int)(1.25*n+7200); e->out=realloc(e->out,e->outcap); }
  if(e->outcap<7200){ e->outcap=7200; e->out=realloc(e->out,e->outcap); }
  return ch?e->r:e->l;
}
EXPORT unsigned char* enc_out(ENC*e){ return e->out; }
EXPORT int enc_encode(ENC*e,int n){ return lame_encode_buffer_ieee_float(e->gfp,e->l,e->r,n,e->out,e->outcap); }
EXPORT int enc_flush(ENC*e){ enc_buf(e,0,1); return lame_encode_flush(e->gfp,e->out,e->outcap); }
EXPORT void enc_free(ENC*e){ if(!e) return; lame_close(e->gfp); free(e->l); free(e->r); free(e->out); free(e); }
EXPORT int enc_quality(ENC*e){ return lame_get_quality(e->gfp); }
EXPORT int enc_mode(ENC*e){ return lame_get_mode(e->gfp); }

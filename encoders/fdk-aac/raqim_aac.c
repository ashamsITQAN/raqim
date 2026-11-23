/* Raqim: minimal FDK AAC (AAC-LC, raw access units) wrapper for WebAssembly */
#include <stdlib.h>
#include <string.h>
#include "aacenc_lib.h"
#define EXPORT __attribute__((visibility("default")))
typedef struct { HANDLE_AACENCODER h; INT_PCM *in; int cap; unsigned char *out; int outcap; int ch; AACENC_InfoStruct info; } AENC;
EXPORT AENC* aac_init(int sr,int ch,int kbps,int afterburner){
  AENC *e=calloc(1,sizeof(AENC)); if(!e) return 0;
  e->ch=ch;
  if(aacEncOpen(&e->h,0,ch)!=AACENC_OK){free(e);return 0;}
  int ok = aacEncoder_SetParam(e->h,AACENC_AOT,2)==AACENC_OK            /* AAC-LC */
        && aacEncoder_SetParam(e->h,AACENC_SAMPLERATE,sr)==AACENC_OK
        && aacEncoder_SetParam(e->h,AACENC_CHANNELMODE,ch==1?MODE_1:MODE_2)==AACENC_OK
        && aacEncoder_SetParam(e->h,AACENC_CHANNELORDER,1)==AACENC_OK
        && aacEncoder_SetParam(e->h,AACENC_BITRATEMODE,0)==AACENC_OK     /* CBR */
        && aacEncoder_SetParam(e->h,AACENC_BITRATE,kbps*1000)==AACENC_OK
        && aacEncoder_SetParam(e->h,AACENC_TRANSMUX,TT_MP4_RAW)==AACENC_OK
        && aacEncoder_SetParam(e->h,AACENC_AFTERBURNER,afterburner?1:0)==AACENC_OK;
  if(!ok || aacEncEncode(e->h,NULL,NULL,NULL,NULL)!=AACENC_OK || aacEncInfo(e->h,&e->info)!=AACENC_OK){aacEncClose(&e->h);free(e);return 0;}
  e->outcap=8192*ch; e->out=malloc(e->outcap);
  return e;
}
EXPORT INT_PCM* aac_buf(AENC*e,int frames){ int n=frames*e->ch; if(n>e->cap){e->cap=n;e->in=realloc(e->in,n*sizeof(INT_PCM));} return e->in; }
EXPORT int aac_frame_len(AENC*e){ return e->info.frameLength; }
EXPORT int aac_delay(AENC*e){ return e->info.nDelay; }
EXPORT int aac_asc_len(AENC*e){ return e->info.confSize; }
EXPORT unsigned char* aac_asc(AENC*e){ return e->info.confBuf; }
EXPORT unsigned char* aac_out(AENC*e){ return e->out; }
/* encode exactly one call: n interleaved frames (n may be 0 with flush=1). returns bytes of ONE access unit (0 = none yet), sets *consumed */
static int consumed_g;
EXPORT int aac_consumed(void){ return consumed_g; }
EXPORT int aac_encode(AENC*e,int offsetFrames,int nFrames,int flush){
  AACENC_BufDesc ib={0},ob={0}; AACENC_InArgs ia={0}; AACENC_OutArgs oa={0};
  void *ip=e->in+offsetFrames*e->ch; INT iid=IN_AUDIO_DATA, isz=nFrames*e->ch*sizeof(INT_PCM), iel=sizeof(INT_PCM);
  void *op=e->out; INT oid=OUT_BITSTREAM_DATA, osz=e->outcap, oel=1;
  ib.numBufs=1; ib.bufs=&ip; ib.bufferIdentifiers=&iid; ib.bufSizes=&isz; ib.bufElSizes=&iel;
  ob.numBufs=1; ob.bufs=&op; ob.bufferIdentifiers=&oid; ob.bufSizes=&osz; ob.bufElSizes=&oel;
  ia.numInSamples=flush?-1:nFrames*e->ch;
  AACENC_ERROR r=aacEncEncode(e->h,&ib,&ob,&ia,&oa);
  if(r==AACENC_ENCODE_EOF){consumed_g=0;return -2;}
  if(r!=AACENC_OK){consumed_g=0;return -1;}
  consumed_g=oa.numInSamples/e->ch;
  return oa.numOutBytes;
}
EXPORT void aac_free(AENC*e){ if(!e) return; aacEncClose(&e->h); free(e->in); free(e->out); free(e); }

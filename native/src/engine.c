/* Raqim native engine: WAV / MP3 in -> MP3 (LAME 3.100) or AAC-LC (FDK AAC) out.
 * Mirrors the browser engine exactly: same wrappers (raqim_lame.c, raqim_aac.c),
 * same settings, same sample conversion, same chunking.
 * Built twice for Windows: generic x86-64 and x86-64-v3 (AVX2); the launcher picks one at run time. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "engine.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#include "minimp3.h"

/* --- LAME wrapper (raqim_lame.c) --- */
typedef struct ENC ENC;
ENC* enc_init(int,int,int,int,int,int); float* enc_buf(ENC*,int,int); unsigned char* enc_out(ENC*);
int enc_encode(ENC*,int); int enc_flush(ENC*); void enc_free(ENC*);
/* --- FDK AAC wrapper (raqim_aac.c) --- */
typedef struct AENC AENC;
AENC* aac_init(int,int,int,int); short* aac_buf(AENC*,int); int aac_frame_len(AENC*); int aac_delay(AENC*);
int aac_asc_len(AENC*); unsigned char* aac_asc(AENC*); unsigned char* aac_out(AENC*);
int aac_encode(AENC*,int,int,int); int aac_consumed(void); void aac_free(AENC*);

/* encoder initialisation touches shared tables inside LAME: serialise it */
#ifdef _WIN32
#include <windows.h>
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT; static CRITICAL_SECTION g_cs;
static BOOL CALLBACK init_cs(PINIT_ONCE o, PVOID p, PVOID *c){ (void)o;(void)p;(void)c; InitializeCriticalSection(&g_cs); return TRUE; }
static void lock(void){ InitOnceExecuteOnce(&g_once, init_cs, NULL, NULL); EnterCriticalSection(&g_cs); }
static void unlock(void){ LeaveCriticalSection(&g_cs); }
#else
#include <pthread.h>
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static void lock(void){ pthread_mutex_lock(&g_mx); }
static void unlock(void){ pthread_mutex_unlock(&g_mx); }
#endif

/* ---------- growable output buffer ---------- */
static int out_put(RaqimOut *o, const void *d, size_t n){
  if(o->len+n > o->cap){ size_t c=o->cap?o->cap:1<<20; while(c<o->len+n) c*=2; void *p=realloc(o->data,c); if(!p) return -1; o->data=p; o->cap=c; }
  memcpy(o->data+o->len,d,n); o->len+=n; return 0;
}

/* ---------- pipe: float planar -> encoder (same rules as the JS Pipe / AacPipe) ---------- */
typedef struct {
  int fmt, kbps, quality, forceJoint;
  int nch, outCh, sr;
  ENC *lame; AENC *aac;
  /* aac */
  int frameLen, delay; long long inSamples; uint32_t *sizes; int nAU, capAU, maxAU; RaqimOut aus;
  RaqimOut *out; char *err;
} Pipe;

static const int OK_RATES[]={8000,11025,12000,16000,22050,24000,32000,44100,48000};
static const int AAC_RATES[]={8000,11025,12000,16000,22050,24000,32000,44100,48000,64000,88200,96000};
static int in_list(const int *l,int n,int v){ for(int i=0;i<n;i++) if(l[i]==v) return 1; return 0; }

static int pipe_open(Pipe *p, int sr, int nch){
  p->sr=sr; p->nch = nch<1?1:(nch>2?2:nch);
  if(p->fmt==RAQIM_FMT_MP3){
    p->outCh = p->forceJoint?2:p->nch;
    int outRate = in_list(OK_RATES,9,sr)? sr : ((sr%48000==0)?48000:44100);
    lock(); p->lame=enc_init(sr,p->nch,outRate,p->kbps,p->quality,p->forceJoint?1:0); unlock();
    if(!p->lame){ snprintf(p->err,256,"تعذّرت تهيئة مرمِّز LAME"); return -1; }
  } else {
    if(!in_list(AAC_RATES,12,sr)){ snprintf(p->err,256,"تردد المصدر (%d) غير مدعوم في AAC",sr); return -1; }
    lock(); p->aac=aac_init(sr,2,p->kbps,1); unlock();
    if(!p->aac){ snprintf(p->err,256,"تعذّرت تهيئة مرمِّز AAC بهذه الجودة"); return -1; }
    p->frameLen=aac_frame_len(p->aac); p->delay=aac_delay(p->aac);
  }
  return 0;
}
static int aac_take(Pipe *p, int k){
  if(k==-1){ snprintf(p->err,256,"خطأ في ترميز AAC"); return -1; }
  if(k>0){
    if(p->nAU==p->capAU){ p->capAU=p->capAU?p->capAU*2:4096; p->sizes=realloc(p->sizes,p->capAU*4); if(!p->sizes) return -1; }
    p->sizes[p->nAU++]=k; if(k>p->maxAU) p->maxAU=k;
    if(out_put(&p->aus,aac_out(p->aac),k)) return -1;
  }
  return 0;
}
static inline short to_i16(float a){ a=a>1?1:(a<-1?-1:a); double v=a<0?a*32768.0:a*32767.0; return (short)floor(v+0.5); }
static int pipe_push(Pipe *p, const float *L, const float *R, int n){
  if(!R) R=L;
  if(p->fmt==RAQIM_FMT_MP3){
    const int STEP=65536;
    for(int o=0;o<n;o+=STEP){
      int m=n-o<STEP?n-o:STEP;
      float *pl=enc_buf(p->lame,0,m), *pr=enc_buf(p->lame,1,m);
      memcpy(pl,L+o,m*sizeof(float));
      if(p->outCh==2) memcpy(pr,R+o,m*sizeof(float));
      int k=enc_encode(p->lame,m);
      if(k<0){ snprintf(p->err,256,"خطأ في الترميز (%d)",k); return -1; }
      if(k>0 && out_put(p->out,enc_out(p->lame),k)) return -1;
    }
  } else {
    const int STEP=32768;
    p->inSamples+=n;
    for(int o=0;o<n;o+=STEP){
      int m=n-o<STEP?n-o:STEP;
      short *b=aac_buf(p->aac,m);
      for(int i=0;i<m;i++){ b[2*i]=to_i16(L[o+i]); b[2*i+1]=to_i16(R[o+i]); }
      int off=0, guard=0;
      while(off<m){
        int k=aac_encode(p->aac,off,m-off,0);
        if(aac_take(p,k)) return -1;
        int c=aac_consumed(); off+=c;
        if(c==0&&k<=0){ if(++guard>4){ snprintf(p->err,256,"توقف مرمِّز AAC"); return -1; } } else guard=0;
      }
    }
  }
  return 0;
}
static void b64(const unsigned char *d,int n,char *o){ static const char*T="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; int j=0;
  for(int i=0;i<n;i+=3){ unsigned v=d[i]<<16|(i+1<n?d[i+1]<<8:0)|(i+2<n?d[i+2]:0); o[j++]=T[v>>18&63]; o[j++]=T[v>>12&63]; o[j++]=i+1<n?T[v>>6&63]:'='; o[j++]=i+2<n?T[v&63]:'='; } o[j]=0; }
static int pipe_finish(Pipe *p){
  if(p->fmt==RAQIM_FMT_MP3){
    int k=enc_flush(p->lame); if(k>0 && out_put(p->out,enc_out(p->lame),k)) return -1;
    enc_free(p->lame); p->lame=NULL; return 0;
  }
  for(int i=0;i<10000;i++){ int k=aac_encode(p->aac,0,0,1); if(k==-2) break; if(aac_take(p,k)) return -1; if(k==0) break; }
  /* response: [u32 LE header length][JSON header][raw AAC access units] */
  char asc[64]; b64(aac_asc(p->aac),aac_asc_len(p->aac),asc);
  size_t hcap=256+(size_t)p->nAU*11; char *h=malloc(hcap); if(!h) return -1;
  int hl=snprintf(h,hcap,"{\"sr\":%d,\"frameLen\":%d,\"delay\":%d,\"inSamples\":%lld,\"kbps\":%d,\"maxAU\":%d,\"total\":%zu,\"asc\":\"%s\",\"sizes\":[",
                  p->sr,p->frameLen,p->delay,p->inSamples,p->kbps,p->maxAU,p->aus.len,asc);
  for(int i=0;i<p->nAU;i++) hl+=snprintf(h+hl,hcap-hl,i?",%u":"%u",p->sizes[i]);
  hl+=snprintf(h+hl,hcap-hl,"]}");
  unsigned char len4[4]={hl&255,(hl>>8)&255,(hl>>16)&255,(hl>>24)&255};
  int r=out_put(p->out,len4,4)||out_put(p->out,h,hl)||out_put(p->out,p->aus.data,p->aus.len);
  free(h); aac_free(p->aac); p->aac=NULL; return r?-1:0;
}
static void pipe_close(Pipe *p){ if(p->lame) enc_free(p->lame); if(p->aac) aac_free(p->aac); free(p->sizes); free(p->aus.data); }

/* ---------- input stream ---------- */
typedef struct { RaqimRead rd; void *ctx; long long total, got; volatile double *progress; volatile int *cancel;
                 unsigned char buf[1<<16]; int pos, len, eof; } In;
static int in_fill(In *s){
  if(s->eof) return 0;
  if(s->pos>0 && s->pos<s->len){ memmove(s->buf,s->buf+s->pos,s->len-s->pos); s->len-=s->pos; s->pos=0; } else if(s->pos>=s->len){ s->len=s->pos=0; }
  int n=s->rd(s->ctx,s->buf+s->len,(int)sizeof(s->buf)-s->len);
  if(n<=0){ s->eof=1; return 0; }
  s->len+=n; s->got+=n; if(s->progress&&s->total>0) *s->progress=(double)s->got/s->total; return n;
}
static int in_need(In *s,int k){ while(s->len-s->pos<k){ if(s->len-s->pos>=(int)sizeof(s->buf)) return 0; if(!in_fill(s)) return 0; } return 1; }
static int in_read(In *s,void *d,int k){ int g=0; while(g<k){ if(s->pos>=s->len && !in_fill(s)) break; int c=s->len-s->pos; if(c>k-g) c=k-g; memcpy((char*)d+g,s->buf+s->pos,c); s->pos+=c; g+=c; } return g; }
static int in_skip(In *s,long long k){ char t[8192]; while(k>0){ int c=k>(long long)sizeof(t)?(int)sizeof(t):(int)k; int g=in_read(s,t,c); if(g<=0) return -1; k-=g; } return 0; }
static uint32_t le32(const unsigned char*b){ return b[0]|b[1]<<8|b[2]<<16|(uint32_t)b[3]<<24; }
static uint16_t le16(const unsigned char*b){ return b[0]|b[1]<<8; }

/* ---------- WAV (streamed) ---------- */
static int do_wav(In *s, Pipe *p){
  unsigned char h[12]; if(in_read(s,h,12)!=12) return -1;
  int af=0,ch=0,sr=0,ba=0,bps=0; long long dataLen=-1;
  for(;;){
    unsigned char c[8]; if(in_read(s,c,8)!=8){ snprintf(p->err,256,"ملف WAV غير مفهوم"); return -1; }
    uint32_t sz=le32(c+4);
    if(!memcmp(c,"fmt ",4)){
      unsigned char f[64]; int r=sz<64?sz:64; if(in_read(s,f,r)!=r) return -1; if(sz>64 && in_skip(s,sz-64)) return -1; if(sz&1) in_skip(s,1);
      af=le16(f); ch=le16(f+2); sr=le32(f+4); ba=le16(f+12); bps=le16(f+14);
      if(af==0xFFFE && sz>=26) af=le16(f+24);
    } else if(!memcmp(c,"data",4)){ dataLen=(sz==0||sz==0xFFFFFFFFu)?-1:sz; break; }
    else { if(in_skip(s,(long long)sz+(sz&1))) return -1; }
  }
  if(!((af==1&&(bps==8||bps==16||bps==24||bps==32))||(af==3&&bps==32))||ba<=0){ snprintf(p->err,256,"صيغة WAV غير مدعومة"); return -1; }
  if(pipe_open(p,sr,ch)) return -1;
  const int nch=ch<2?ch:2, bytes=bps/8, BLOCK=262144;
  unsigned char *raw=malloc((size_t)ba*BLOCK); float *L=malloc(sizeof(float)*BLOCK), *R=malloc(sizeof(float)*BLOCK);
  if(!raw||!L||!R){ free(raw);free(L);free(R); return -1; }
  long long left=dataLen; int rc=0;
  for(;;){
    if(s->cancel&&*s->cancel){ rc=-2; break; }
    long long want=(long long)ba*BLOCK; if(left>=0&&want>left) want=left;
    int got=in_read(s,raw,(int)want); int frames=got/ba; if(frames<=0) break;
    for(int i=0;i<frames;i++){
      const unsigned char *b=raw+(size_t)i*ba;
      for(int c=0;c<nch;c++){
        const unsigned char *o=b+c*bytes; float v;
        if(af==3){ float f; memcpy(&f,o,4); v=f; }
        else if(bytes==2) v=(int16_t)le16(o)/32768.0f;
        else if(bytes==3){ int32_t x=o[0]|o[1]<<8|((int8_t)o[2])*65536; v=x/8388608.0f; }
        else if(bytes==4) v=(int32_t)le32(o)/2147483648.0f;
        else v=(o[0]-128)/128.0f;
        (c?R:L)[i]=v;
      }
    }
    if(pipe_push(p,L,nch>1?R:NULL,frames)){ rc=-1; break; }
    if(left>=0){ left-=(long long)frames*ba; if(left<=0) break; }
    if(got<want) break;
  }
  free(raw);free(L);free(R);
  return rc;
}

/* ---------- MP3 (minimp3, float) ---------- */
static int do_mp3(In *s, Pipe *p){
  if(in_need(s,10) && !memcmp(s->buf+s->pos,"ID3",3)){
    const unsigned char *h=s->buf+s->pos; long long sz=10+((h[6]&127)<<21|(h[7]&127)<<14|(h[8]&127)<<7|(h[9]&127))+((h[5]&0x10)?10:0);
    if(in_skip(s,sz)) return -1;
  }
  static const int BATCH=65536;
  mp3dec_t dec; mp3dec_init(&dec);
  float pcm[MINIMP3_MAX_SAMPLES_PER_FRAME]; mp3dec_frame_info_t info;
  float *L=malloc(sizeof(float)*(BATCH+1152)), *R=malloc(sizeof(float)*(BATCH+1152)); int fill=0, opened=0, chs=1, rc=0;
  if(!L||!R){ free(L);free(R); return -1; }
  for(;;){
    if(s->cancel&&*s->cancel){ rc=-2; break; }
    in_need(s,16384);
    int avail=s->len-s->pos; if(avail<=0) break;
    int n=mp3dec_decode_frame(&dec,s->buf+s->pos,avail,pcm,&info);
    if(info.frame_bytes==0){ if(s->eof) break; if(!in_fill(s)&&s->len-s->pos==avail) break; continue; }
    s->pos+=info.frame_bytes;
    if(n<=0) continue;
    if(!opened){ chs=info.channels; if(pipe_open(p,info.hz,chs)){ rc=-1; break; } opened=1; }
    for(int i=0;i<n;i++){ L[fill+i]=pcm[i*info.channels]; R[fill+i]=info.channels>1?pcm[i*info.channels+1]:pcm[i*info.channels]; }
    fill+=n;
    if(fill>=BATCH){ if(pipe_push(p,L,chs>1?R:NULL,fill)){ rc=-1; break; } fill=0; }
  }
  if(!rc && opened && fill>0 && pipe_push(p,L,chs>1?R:NULL,fill)) rc=-1;
  if(!rc && !opened){ snprintf(p->err,256,"لم يُعثر على صوت MP3 في الملف"); rc=-1; }
  free(L);free(R);
  return rc;
}

/* ---------- entry point ---------- */
RAQIM_API int raqim_encode(const RaqimParams *prm, RaqimRead rd, void *ctx, long long total,
                           volatile double *progress, volatile int *cancel, RaqimOut *out, char *err256){
  Pipe p; memset(&p,0,sizeof p);
  p.fmt=prm->fmt; p.kbps=prm->kbps; p.quality=prm->quality; p.forceJoint=prm->forceJoint; p.out=out; p.err=err256; err256[0]=0;
  In *s=calloc(1,sizeof(In)); if(!s){ snprintf(err256,256,"نفدت الذاكرة"); return -1; }
  s->rd=rd; s->ctx=ctx; s->total=total; s->progress=progress; s->cancel=cancel;
  int rc;
  if(!in_need(s,12)){ snprintf(err256,256,"الملف فارغ"); free(s); return -1; }
  if(!memcmp(s->buf,"RIFF",4) && !memcmp(s->buf+8,"WAVE",4)) rc=do_wav(s,&p);
  else rc=do_mp3(s,&p);
  if(rc==0) rc=pipe_finish(&p);
  if(rc==-2) snprintf(err256,256,"cancelled");
  else if(rc && !err256[0]) snprintf(err256,256,"تعذّر التحويل");
  pipe_close(&p); free(s);
  if(progress) *progress=1.0;
  return rc;
}
RAQIM_API void raqim_free(RaqimOut *o){ free(o->data); o->data=NULL; o->len=o->cap=0; }
RAQIM_API const char* raqim_engine_name(void){
#if defined(__AVX2__)
  return "AVX2";
#else
  return "x86-64";
#endif
}

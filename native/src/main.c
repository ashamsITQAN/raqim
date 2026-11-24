#ifndef _WIN32
#define _GNU_SOURCE
#endif
/* Raqim desktop: serves raqim.html on 127.0.0.1 and runs conversions with the native engine.
 * Windows: opens the interface in an Edge app window (own profile) and exits when it closes.
 * Other systems (development/testing): prints the URL and serves until stopped. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <stdarg.h>
#ifndef _WIN32
#include <strings.h>
#endif
#include "engine.h"
#ifdef _WIN32
#define strncasecmp _strnicmp
#endif

#define RAQIM_PORT 47115
#ifndef VERSION
#define VERSION "dev"   /* set by build_windows.sh from raqim.html */
#endif

static const unsigned char HTML[] = {
#embed "raqim.html"
, 0 };

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <shellapi.h>
#  include <shlobj.h>
   typedef SOCKET sock_t;
#  define closesock closesocket
   static const unsigned char DLL_X64[] = {
#  embed "raqim_engine_x64.dll"
   };
   static const unsigned char DLL_AVX2[] = {
#  embed "raqim_engine_avx2.dll"
   };
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <pthread.h>
   typedef int sock_t;
#  define closesock close
#  define INVALID_SOCKET (-1)
#endif

static raqim_encode_fn ENCODE; static raqim_free_fn FREE; static raqim_name_fn NAME;
static char TOKEN[33];
/* liveness: the page pings while it is open; the program ends a little after the window is closed */
static volatile long long LAST_PING=0, BYE_AT=0; static volatile int ACTIVE_JOBS=0;
static long long now_ms(void);
static void logf_(const char *fmt,...);

/* ---------------- jobs (progress / cancel) ---------------- */
#define MAXJOBS 64
typedef struct { char id[40]; volatile double progress; volatile int cancel; int used; } Job;
static Job JOBS[MAXJOBS];
#ifdef _WIN32
static CRITICAL_SECTION JMX;
#define JLOCK() EnterCriticalSection(&JMX)
#define JUNLOCK() LeaveCriticalSection(&JMX)
#else
static pthread_mutex_t JMX = PTHREAD_MUTEX_INITIALIZER;
#define JLOCK() pthread_mutex_lock(&JMX)
#define JUNLOCK() pthread_mutex_unlock(&JMX)
#endif
static Job* job_get(const char *id, int create){
  Job *r=NULL; JLOCK();
  for(int i=0;i<MAXJOBS;i++) if(JOBS[i].used && !strcmp(JOBS[i].id,id)){ r=&JOBS[i]; break; }
  if(!r && create) for(int i=0;i<MAXJOBS;i++) if(!JOBS[i].used){ r=&JOBS[i]; memset(r,0,sizeof *r); r->used=1; snprintf(r->id,sizeof r->id,"%s",id); break; }
  JUNLOCK(); return r;
}
static void job_done(Job *j){ JLOCK(); j->used=0; JUNLOCK(); }

/* ---------------- tiny HTTP ---------------- */
static int send_all(sock_t s,const void *d,size_t n){ const char*p=d; while(n){ int k=send(s,p,n>1<<20?1<<20:(int)n,0); if(k<=0) return -1; p+=k; n-=k; } return 0; }
static void reply(sock_t s,int code,const char *ctype,const void *body,size_t n){
  char h[512]; const char *st=code==200?"OK":code==403?"Forbidden":code==404?"Not Found":code==400?"Bad Request":"Error";
  int hl=snprintf(h,sizeof h,"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",code,st,ctype,n);
  send_all(s,h,hl); if(n) send_all(s,body,n);
}
static const char* qparam(const char *q,const char *k,char *out,int outn){
  size_t kl=strlen(k); out[0]=0; if(!q) return out;
  for(const char *p=q;p&&*p;){ if(!strncmp(p,k,kl)&&p[kl]=='='){ p+=kl+1; int i=0; while(*p&&*p!='&'&&i<outn-1) out[i++]=*p++; out[i]=0; return out; } p=strchr(p,'&'); if(p) p++; }
  return out;
}
typedef struct { sock_t s; char *pre; int preLen, prePos; long long left; } Body;
static int body_read(void *ctx,unsigned char *buf,int max){
  Body *b=ctx; if(b->left<=0) return 0;
  if(max>b->left) max=(int)b->left;
  if(b->prePos<b->preLen){ int c=b->preLen-b->prePos; if(c>max) c=max; memcpy(buf,b->pre+b->prePos,c); b->prePos+=c; b->left-=c; return c; }
  int k=recv(b->s,(char*)buf,max,0); if(k<=0) return -1; b->left-=k; return k;
}

static void handle(sock_t s){
  char hb[16384]; int hl=0, hdrEnd=-1;
  while(hl<(int)sizeof hb-1){ int k=recv(s,hb+hl,sizeof hb-1-hl,0); if(k<=0){ closesock(s); return; } hl+=k; hb[hl]=0; char *e=strstr(hb,"\r\n\r\n"); if(e){ hdrEnd=(int)(e-hb)+4; break; } }
  if(hdrEnd<0){ closesock(s); return; }
  char method[8]={0}, path[1024]={0}; sscanf(hb,"%7s %1023s",method,path);
  char *q=strchr(path,'?'); if(q) *q++=0;
  long long clen=0; char token[64]={0};
  for(char *line=strstr(hb,"\r\n"); line && line<hb+hdrEnd; line=strstr(line+2,"\r\n")){
    char *l=line+2;
    if(!strncasecmp(l,"Content-Length:",15)) clen=atoll(l+15);
    if(!strncasecmp(l,"X-Raqim-Token:",14)){ sscanf(l+14," %63s",token); }
  }
  if(!strcmp(method,"GET") && (!strcmp(path,"/")||!strcmp(path,"/index.html"))){
    /* inject the session token so only this page can call the API */
    static const char marker[]="<script>";
    const char *html=(const char*)HTML; size_t n=sizeof HTML-1;
    const char *at=strstr(html,marker); size_t pre=at?(size_t)(at-html):0;
    char inj[256]; int il=snprintf(inj,sizeof inj,"<script>window.RAQIM_NATIVE={token:\"%s\",engine:\"%s\",version:\"%s\"};</script>\n",TOKEN,NAME(),VERSION);
    char h[256]; int hl2=snprintf(h,sizeof h,"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",n+il);
    send_all(s,h,hl2); send_all(s,html,pre); send_all(s,inj,il); send_all(s,html+pre,n-pre);
    closesock(s); return;
  }
  { char t[64]; qparam(q,"t",t,sizeof t); if(!token[0] && t[0]) snprintf(token,sizeof token,"%s",t); }
  if(!strncmp(path,"/api/",5) && strcmp(token,TOKEN)){ reply(s,403,"text/plain; charset=utf-8","forbidden",9); closesock(s); return; }
  char id[40]; qparam(q,"id",id,sizeof id);
  LAST_PING=now_ms();
  if(!strcmp(path,"/api/ping")){ reply(s,200,"application/json","{}",2); closesock(s); return; }
  if(!strcmp(path,"/api/bye")){ BYE_AT=now_ms(); reply(s,200,"application/json","{}",2); closesock(s); return; }
  if(!strcmp(path,"/api/progress")){
    Job *j=job_get(id,0); char b[64]; int n=snprintf(b,sizeof b,"{\"p\":%.4f}",j?j->progress:0.0);
    reply(s,200,"application/json",b,n); closesock(s); return;
  }
  if(!strcmp(path,"/api/cancel")){ Job *j=job_get(id,0); if(j) j->cancel=1; reply(s,200,"application/json","{}",2); closesock(s); return; }
  if(!strcmp(method,"POST") && !strcmp(path,"/api/encode")){
    char v[16]; RaqimParams prm;
    prm.fmt=!strcmp(qparam(q,"fmt",v,sizeof v),"m4a")?RAQIM_FMT_M4A:RAQIM_FMT_MP3;
    prm.kbps=atoi(qparam(q,"kbps",v,sizeof v)); prm.quality=atoi(qparam(q,"q",v,sizeof v)); prm.forceJoint=atoi(qparam(q,"fj",v,sizeof v));
    if(prm.kbps<8||prm.kbps>320){ reply(s,400,"text/plain; charset=utf-8","bad kbps",8); closesock(s); return; }
    __atomic_add_fetch(&ACTIVE_JOBS,1,__ATOMIC_SEQ_CST);
    Job *j=job_get(id[0]?id:"_",1);
    Body b={s,hb+hdrEnd,hl-hdrEnd,0,clen};
    RaqimOut out={0}; char err[256];
    int rc=ENCODE(&prm,body_read,&b,clen,j?&j->progress:NULL,j?&j->cancel:NULL,&out,err);
    if(rc==0) reply(s,200,"application/octet-stream",out.data,out.len);
    else { /* drain what is left of the upload so the browser sees the reply */
      unsigned char t[65536]; while(b.left>0 && body_read(&b,t,sizeof t)>0){}
      reply(s,rc==-2?409:500,"text/plain; charset=utf-8",err,strlen(err)); }
    FREE(&out); if(j) job_done(j); closesock(s);
    if(rc && rc!=-2) logf_("encode failed: %s",err);
    __atomic_sub_fetch(&ACTIVE_JOBS,1,__ATOMIC_SEQ_CST); LAST_PING=now_ms(); return;
  }
  if(!strcmp(path,"/api/info")){ char b[128]; int n=snprintf(b,sizeof b,"{\"engine\":\"%s\",\"version\":\"%s\"}",NAME(),VERSION); reply(s,200,"application/json",b,n); closesock(s); return; }
  reply(s,404,"text/plain","not found",9); closesock(s);
}

#ifdef _WIN32
static long long now_ms(void){ return (long long)GetTickCount64(); }
static wchar_t LOGPATH[MAX_PATH];
static void logf_(const char *fmt,...){
  if(!LOGPATH[0]) return; FILE *f=_wfopen(LOGPATH,L"a"); if(!f) return;
  SYSTEMTIME t; GetLocalTime(&t); fprintf(f,"%04d-%02d-%02d %02d:%02d:%02d  ",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
  va_list a; va_start(a,fmt); vfprintf(f,fmt,a); va_end(a); fputc('\n',f); fclose(f);
}
static LONG WINAPI on_crash(EXCEPTION_POINTERS *e){ logf_("CRASH code=0x%08lx at %p",e->ExceptionRecord->ExceptionCode,e->ExceptionRecord->ExceptionAddress); return EXCEPTION_CONTINUE_SEARCH; }
static DWORD WINAPI conn_thread(LPVOID p){ handle((sock_t)(uintptr_t)p); return 0; }
static DWORD WINAPI accept_thread(LPVOID p){ sock_t ls=(sock_t)(uintptr_t)p; for(;;){ sock_t c=accept(ls,NULL,NULL); if(c==INVALID_SOCKET) continue; HANDLE t=CreateThread(NULL,1<<20,conn_thread,(LPVOID)(uintptr_t)c,0,NULL); if(t) CloseHandle(t); else closesock(c); } return 0; }
#else
static long long now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000LL+t.tv_nsec/1000000; }
static void logf_(const char *fmt,...){ va_list a; va_start(a,fmt); vfprintf(stderr,fmt,a); va_end(a); fputc('\n',stderr); }
static void* conn_thread(void *p){ handle((sock_t)(intptr_t)p); return NULL; }
#endif

static void make_token(void){
  unsigned char r[16];
#ifdef _WIN32
  HCRYPTPROV h; if(CryptAcquireContextW(&h,NULL,NULL,PROV_RSA_FULL,CRYPT_VERIFYCONTEXT)){ CryptGenRandom(h,16,r); CryptReleaseContext(h,0); } else { srand((unsigned)time(NULL)^GetTickCount()); for(int i=0;i<16;i++) r[i]=rand(); }
#else
  FILE *f=fopen("/dev/urandom","rb"); if(!f||fread(r,1,16,f)!=16){ for(int i=0;i<16;i++) r[i]=rand(); } if(f) fclose(f);
#endif
  for(int i=0;i<16;i++) sprintf(TOKEN+2*i,"%02x",r[i]);
}

static sock_t listen_on(int port){
  sock_t ls=socket(AF_INET,SOCK_STREAM,0); if(ls==INVALID_SOCKET) return INVALID_SOCKET;
#ifdef _WIN32
  { BOOL one=TRUE; setsockopt(ls,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(const char*)&one,sizeof one); }
#else
  { int one=1; setsockopt(ls,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one); }
#endif
  struct sockaddr_in a; memset(&a,0,sizeof a); a.sin_family=AF_INET; a.sin_port=htons(port); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  if(bind(ls,(struct sockaddr*)&a,sizeof a)!=0 || listen(ls,64)!=0){ closesock(ls); return INVALID_SOCKET; }
  return ls;
}

#ifdef _WIN32
/* -------- Windows: pick engine DLL, open Edge app window -------- */
static int write_file(const wchar_t *path,const void *d,size_t n){
  HANDLE h=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL); if(h==INVALID_HANDLE_VALUE) return -1;
  DWORD w; BOOL ok=WriteFile(h,d,(DWORD)n,&w,NULL); CloseHandle(h); return ok&&w==n?0:-1;
}
static int file_size_is(const wchar_t *path,size_t n){ WIN32_FILE_ATTRIBUTE_DATA a; if(!GetFileAttributesExW(path,GetFileExInfoStandard,&a)) return 0; return a.nFileSizeHigh==0&&a.nFileSizeLow==n; }
static int load_engine(wchar_t *appdir){
  int avx2=__builtin_cpu_supports("avx2")&&__builtin_cpu_supports("fma")&&__builtin_cpu_supports("bmi2");
  const unsigned char *d=avx2?DLL_AVX2:DLL_X64; size_t n=avx2?sizeof DLL_AVX2:sizeof DLL_X64;
  wchar_t dir[MAX_PATH], path[MAX_PATH];
  swprintf(dir,MAX_PATH,L"%ls\\bin-" VERSION,appdir); CreateDirectoryW(dir,NULL);
  swprintf(path,MAX_PATH,L"%ls\\%ls",dir,avx2?L"raqim_engine_avx2.dll":L"raqim_engine_x64.dll");
  if(!file_size_is(path,n) && write_file(path,d,n)) return -1;
  HMODULE m=LoadLibraryW(path); if(!m) return -1;
  ENCODE=(raqim_encode_fn)GetProcAddress(m,"raqim_encode"); FREE=(raqim_free_fn)GetProcAddress(m,"raqim_free"); NAME=(raqim_name_fn)GetProcAddress(m,"raqim_engine_name");
  return ENCODE&&FREE&&NAME?0:-1;
}
static int find_browser(wchar_t *exe,DWORD cap){
  const wchar_t *keys[]={L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\msedge.exe",L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\chrome.exe"};
  for(int i=0;i<2;i++) for(int root=0;root<2;root++){
    DWORD sz=cap*sizeof(wchar_t);
    if(RegGetValueW(root?HKEY_CURRENT_USER:HKEY_LOCAL_MACHINE,keys[i],NULL,RRF_RT_REG_SZ,NULL,exe,&sz)==ERROR_SUCCESS && GetFileAttributesW(exe)!=INVALID_FILE_ATTRIBUTES) return 0;
  }
  const wchar_t *guesses[]={L"%ProgramFiles(x86)%\\Microsoft\\Edge\\Application\\msedge.exe",L"%ProgramFiles%\\Microsoft\\Edge\\Application\\msedge.exe"};
  for(int i=0;i<2;i++){ ExpandEnvironmentStringsW(guesses[i],exe,cap); if(GetFileAttributesW(exe)!=INVALID_FILE_ATTRIBUTES) return 0; }
  return -1;
}
static void open_window(const wchar_t *url,const wchar_t *appdir){
  wchar_t exe[MAX_PATH], cmd[4096];
  if(find_browser(exe,MAX_PATH)){
    logf_("no Edge/Chrome found: opening the default browser");
    ShellExecuteW(NULL,L"open",url,NULL,NULL,SW_SHOWNORMAL); return;
  }
  swprintf(cmd,4096,L"\"%ls\" --app=%ls --user-data-dir=\"%ls\\window\" --no-first-run --no-default-browser-check --window-size=1320,920",exe,url,appdir);
  STARTUPINFOW si; PROCESS_INFORMATION pi; memset(&si,0,sizeof si); si.cb=sizeof si;
  if(CreateProcessW(NULL,cmd,NULL,NULL,FALSE,0,NULL,NULL,&si,&pi)){ CloseHandle(pi.hProcess); CloseHandle(pi.hThread); logf_("window opened with %ls",exe); }
  else { logf_("CreateProcess failed (%lu): default browser",GetLastError()); ShellExecuteW(NULL,L"open",url,NULL,NULL,SW_SHOWNORMAL); }
}
static void launch_window(const wchar_t *url,const wchar_t *appdir){ /* for a second instance: don't wait */
  wchar_t exe[MAX_PATH], cmd[4096];
  if(!find_browser(exe,MAX_PATH)){ swprintf(cmd,4096,L"\"%ls\" --app=%ls --user-data-dir=\"%ls\\window\"",exe,url,appdir); STARTUPINFOW si; PROCESS_INFORMATION pi; memset(&si,0,sizeof si); si.cb=sizeof si; if(CreateProcessW(NULL,cmd,NULL,NULL,FALSE,0,NULL,NULL,&si,&pi)){ CloseHandle(pi.hProcess); CloseHandle(pi.hThread); return; } }
  ShellExecuteW(NULL,L"open",url,NULL,NULL,SW_SHOWNORMAL);
}
int WINAPI wWinMain(HINSTANCE hi,HINSTANCE hp,LPWSTR cl,int show){
  (void)hi;(void)hp;(void)show;
  wchar_t url[128], appdir[MAX_PATH], la[MAX_PATH], portfile[MAX_PATH];
  ExpandEnvironmentStringsW(L"%LOCALAPPDATA%",la,MAX_PATH);
  swprintf(appdir,MAX_PATH,L"%ls\\Raqim",la); CreateDirectoryW(appdir,NULL);
  swprintf(portfile,MAX_PATH,L"%ls\\port.txt",appdir);
  swprintf(LOGPATH,MAX_PATH,L"%ls\\raqim.log",appdir); SetUnhandledExceptionFilter(on_crash);
  /* one server per user: a second launch just opens another window on the running one */
  CreateMutexW(NULL,TRUE,L"Local\\RaqimDesktopServer");
  if(GetLastError()==ERROR_ALREADY_EXISTS){
    int port=RAQIM_PORT; FILE *f=_wfopen(portfile,L"r"); if(f){ if(fscanf(f,"%d",&port)!=1) port=RAQIM_PORT; fclose(f); }
    swprintf(url,128,L"http://127.0.0.1:%d/",port); launch_window(url,appdir); return 0;
  }
  WSADATA w; WSAStartup(MAKEWORD(2,2),&w); InitializeCriticalSection(&JMX);
  /* Windows (Hyper-V/WSL/Docker) may reserve port ranges: try a few fixed ports so the
     page keeps the same address (and its saved settings) between runs, then any free port */
  static const int PORTS[]={47115,38715,29115,51515,18115,8115,0};
  sock_t ls=INVALID_SOCKET; int port=0, lastErr=0;
  for(int i=0;i<7 && ls==INVALID_SOCKET;i++){ ls=listen_on(PORTS[i]); if(ls==INVALID_SOCKET) lastErr=WSAGetLastError(); else port=PORTS[i]; }
  if(ls==INVALID_SOCKET){ wchar_t m[256]; swprintf(m,256,L"Raqim: could not start the local server (error %d).",lastErr); MessageBoxW(NULL,m,L"Raqim",MB_OK|MB_ICONERROR); return 1; }
  if(port==0){ struct sockaddr_in a; int al=sizeof a; getsockname(ls,(struct sockaddr*)&a,&al); port=ntohs(a.sin_port); }
  { FILE *f=_wfopen(portfile,L"w"); if(f){ fprintf(f,"%d",port); fclose(f); } }
  swprintf(url,128,L"http://127.0.0.1:%d/",port);
  logf_("Raqim %s listening on port %d",VERSION,port);
  if(load_engine(appdir)){ logf_("engine load failed (%lu)",GetLastError()); MessageBoxW(NULL,L"Raqim: could not load the encoding engine.",L"Raqim",MB_OK|MB_ICONERROR); return 1; }
  logf_("engine: %s",NAME());
  make_token();
  HANDLE t=CreateThread(NULL,0,accept_thread,(LPVOID)(uintptr_t)ls,0,NULL); if(t) CloseHandle(t);
  if(cl && wcsstr(cl,L"--serve")){ Sleep(INFINITE); } /* server only (testing / own browser) */
  LAST_PING=now_ms(); open_window(url,appdir);
  /* stay alive while the page is open (it pings every 15 s) or a conversion is running;
     end 10 s after the window says goodbye, or 3 minutes after the last sign of life */
  for(;;){
    Sleep(2000); long long n=now_ms();
    if(ACTIVE_JOBS>0) continue;
    if(BYE_AT && BYE_AT>=LAST_PING-50 && n-BYE_AT>10000) break;
    if(n-LAST_PING>180000) break;
  }
  logf_("exit");
  return 0;
}
#else
/* -------- development build: serve on the given port (default 47115) -------- */
int main(int argc,char **argv){
  ENCODE=raqim_encode; FREE=raqim_free; NAME=raqim_engine_name;
  int port=argc>1?atoi(argv[1]):RAQIM_PORT;
  sock_t ls=listen_on(port); if(ls==INVALID_SOCKET){ perror("listen"); fprintf(stderr,"cannot listen on %d\n",port); return 1; }
  make_token(); printf("Raqim %s (%s) on http://127.0.0.1:%d/  token=%s\n",VERSION,NAME(),port,TOKEN); fflush(stdout);
  for(;;){ sock_t c=accept(ls,NULL,NULL); if(c<0) continue; pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at,1<<21); if(pthread_create(&th,&at,conn_thread,(void*)(intptr_t)c)==0) pthread_detach(th); else closesock(c); }
}
#endif

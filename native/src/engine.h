#ifndef RAQIM_ENGINE_H
#define RAQIM_ENGINE_H
#include <stddef.h>
#ifdef _WIN32
#  ifdef RAQIM_BUILD_DLL
#    define RAQIM_API __declspec(dllexport)
#  else
#    define RAQIM_API
#  endif
#else
#  define RAQIM_API __attribute__((visibility("default")))
#endif
enum { RAQIM_FMT_MP3 = 0, RAQIM_FMT_M4A = 1 };
typedef struct { int fmt, kbps, quality, forceJoint; } RaqimParams;
typedef struct { unsigned char *data; size_t len, cap; } RaqimOut;
/* read callback: returns bytes read, 0 at end, <0 on error */
typedef int (*RaqimRead)(void *ctx, unsigned char *buf, int max);
typedef int (*raqim_encode_fn)(const RaqimParams*, RaqimRead, void*, long long, volatile double*, volatile int*, RaqimOut*, char*);
typedef void (*raqim_free_fn)(RaqimOut*);
typedef const char* (*raqim_name_fn)(void);
RAQIM_API int raqim_encode(const RaqimParams *p, RaqimRead rd, void *ctx, long long total,
                           volatile double *progress, volatile int *cancel, RaqimOut *out, char *err256);
RAQIM_API void raqim_free(RaqimOut *o);
RAQIM_API const char* raqim_engine_name(void);
#endif

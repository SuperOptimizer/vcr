#pragma once


#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <blosc2.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <float.h>
#include <stdint.h>
#include <math.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>

#include "json.h"


#ifdef NDEBUG
#define ASSERT(expr, msg, ...) ((void)0)
#else
#define ASSERT(expr, msg, ...) do{if(!(expr)){fprintf(stderr,msg __VA_OPT__(,)#__VA_ARGS__); assert_fail_with_backtrace(#expr, __FILE__, __LINE__, __func__);}}while(0)
#endif


typedef enum log_level {
  LOG_INFO,
  LOG_WARN,
  LOG_ERROR,
  LOG_FATAL
} log_level;

#define LOG_INFO(...) log_msg(LOG_INFO, __FILE__, __FUNCTION__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...) log_msg(LOG_WARN, __FILE__, __FUNCTION__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) log_msg(LOG_ERROR, __FILE__, __FUNCTION__, __LINE__, __VA_ARGS__)
#define LOG_FATAL(...) log_msg(LOG_FATAL, __FILE__, __FUNCTION__, __LINE__, __VA_ARGS__)



typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;
typedef float f32;
typedef double f64;

#define overload __attribute__((overloadable))
#define purefunc __attribute__((pure))
#define constfunc __attribute__((const))

constexpr s32 CHUNK_LEN = 128;

typedef enum err {
  OK = 0,
  FAIL = -1
} err;

typedef u8 chunk[CHUNK_LEN][CHUNK_LEN][CHUNK_LEN];
typedef u8 slice[CHUNK_LEN][CHUNK_LEN];

typedef struct volume {
  s32 z, y, x;
  chunk* chunks;
} volume;

typedef struct image {
  s32 y, x;
  slice* slices;
} image;

typedef struct zarrinfo {
  s32 chunks[3];
  struct {
    s32 blocksize;
    s32 clevel;
    char cname[32];
    char id[32];
    s32 shuffle;
  } compressor;
  char dimension_separator;
  char dtype[32];
  s32 fill_value;
  void* filters;
  char order;
  s32 shape[3];
  s32 zarr_format;
} zarrinfo;


// util
void print_backtrace(void);
void log_msg(log_level level, const char* file, const char* func, int line, const char* fmt, ...);
void print_assert_details(const char* expr, const char* file, int line, const char* func);
void assert_fail_with_backtrace(const char* expr, const char* file, int line, const char* func);
bool path_exists(const char *path);
char* read_file(const char* filepath);

// chunk
static inline chunk* chunk_new() {return malloc(CHUNK_LEN*CHUNK_LEN*CHUNK_LEN);}
static inline void chunk_free(chunk* c){free(c);}

// zarr
zarrinfo zarr_parse_zarray(const char* json_string);
chunk* zarr_read_chunk(char* path, zarrinfo metadata);

// mesh structure for marching cubes output
typedef struct mesh {
    float* vertices;      // x,y,z per vertex (num_triangles * 9 floats)
    float* colors;        // r,g,b per vertex (num_triangles * 9 floats)
    int num_triangles;
} mesh;

// marching cubes
mesh generate_mesh_from_chunk(const chunk* volume_data, u8 iso_threshold);
void mesh_free(mesh* m);

// color types
typedef struct rgb {
    u8 r, g, b;
} rgb;

// colormap
rgb apply_viridis_colormap(u8 value);
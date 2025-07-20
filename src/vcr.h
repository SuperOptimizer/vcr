#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <string.h>

#include "json.h"

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

constexpr s32 CHUNK_SIZE = 128;

typedef enum err {
  OK = 0,
  FAIL = -1
} err;

typedef f32 chunk[CHUNK_SIZE][CHUNK_SIZE][CHUNK_SIZE];
typedef f32 slice[CHUNK_SIZE][CHUNK_SIZE];

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

// Function declarations
zarrinfo parse_zarr_json(const char* json_string);
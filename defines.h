#pragma once
#ifndef DEFINES_H
#define DEFINES_H

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <windows.h>s

typedef unsigned char uchar;
typedef struct kmem_cache_s kmem_cache_t;
typedef unsigned char bool;

#define BLOCK_SIZE (4096)
#define CACHE_L1_LINE_SIZE (64)
#define MAX_BUDDY_POWER (32)
#define true 1
#define false 0
#define DESIRED_OBJECT_COUNT (64)
#define BLOCK_NUMBER (200000)
#define THREAD_NUM (150)
#define ITERATIONS (15000)
#define USING_L1_ALIGNMENT (1)
#define shared_size (7)
#define SMALL_MEM_BUFFER_MAX_SIZE (13)


#endif
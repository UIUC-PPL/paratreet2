#ifndef PARATREET_COMMON_H_
#define PARATREET_COMMON_H_

#include <string>
#include <cinttypes> // For printing keys in hex
#include "Vector3D.h"
#include "SFC.h"

// Standalone (non-Charm) builds of the passive tree/cache core — the
// TreeCache unit test, or a host application bringing its own runtime
// (design/smp-cache-extraction.md phase 3). charmc defines __CHARMC__;
// without it, the utility structures already compile their serialization
// methods out (the same guard), and the few runtime calls in the
// Node/Particle/TreeCache chain fall back to plain C here. Charm builds
// are untouched (the block is skipped).
#ifndef __CHARMC__
#include <cassert>
#include <cstdio>
#include <cstdlib>
#define CkAssert(expr) assert(expr)
#define CkEnforce(expr) do { if (!(expr)) { fprintf(stderr, "CkEnforce failed: %s\n", #expr); abort(); } } while (0)
#define CkAbort(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); abort(); } while (0)
#define CkPrintf printf
#endif

// Floating point type
#ifndef USE_DOUBLE_FP
typedef float Real;
typedef float SSEReal;
#define REAL_MAX FLT_MAX
#else
typedef double Real;
typedef double SSEReal;
#define REAL_MAX DBL_MAX
#endif

// Simulation domain dimensions
#define NDIM 3
#define CMK_SSE 0

// Particle key
typedef SFC::Key Key;
#ifndef CHAR_BIT
#define CHAR_BIT 8
#endif
#define KEY_BITS (sizeof(Key)*CHAR_BIT)
#define BITS_PER_DIM (KEY_BITS/NDIM)
#define BOXES_PER_DIM (1<<(BITS_PER_DIM))

#endif // PARATREET_COMMON_H_

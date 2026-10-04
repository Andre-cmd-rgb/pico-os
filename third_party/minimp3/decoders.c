/*
 * The MP3 decoder, compiled once.
 *
 * This is the only file in PocketType that somebody else wrote. See
 * README.md for why, and codec/mp3.c for the part that is ours.
 */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD			/* no SSE or NEON on Xtensa */
#define MINIMP3_ONLY_MP3		/* MPEG layer III, not I or II */
#define MINIMP3_FLOAT_OUTPUT		/* all it worked out, not rounded to 16 bits */
#include "minimp3.h"

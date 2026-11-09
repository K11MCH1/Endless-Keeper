/* zstd (deps/zstd, BSD licence): built from src/ so that every build of the game compiles it; single-threaded */
#define KFX_ZSTD_SINGLE_THREAD 1
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../deps/zstd/zstd.c"

/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_attract.h
 *     Endless Keeper: the launcher's background - the game itself, hidden, playing battles and handing its frames over.
 * @par Purpose:
 *     The launcher (launcher/, EndlessKeeper.exe) starts the game with "-attract <name>": the name of a piece of shared
 *     memory it made. The game then runs an endless world without a window, sound, saves or input; a director stages
 *     battles by the keeper's dungeon (rival keepers, heroes, wild creatures; creatures of any level up to 100) and
 *     moves the camera slowly; every drawn frame is copied into the shared memory, where the launcher shows it behind
 *     its boxes. The game leaves when the launcher has gone.
 */
/******************************************************************************/
#ifndef WORLD_ATTRACT_H
#define WORLD_ATTRACT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The shared memory: this header, then two pictures of `capacity` bytes each (RGBA, top row first). The game writes the
   picture `frame` & 1 will name next, then adds one to `frame`: the launcher reads picture (frame - 1) & 1. */
#define ATTRACT_MAGIC 0x54414B45u /* "EKAT" */
struct AttractShared {
    uint32_t magic;
    uint32_t capacity;       /* bytes of each picture (the launcher's) */
    uint32_t launcher_pid;   /* the game leaves when this process has gone */
    volatile uint32_t frame; /* pictures written so far */
    uint32_t width[2], height[2];
    uint32_t fade;           /* 0 (clear) .. 255 (black): the director's cut between scenes */
    char caption[128];       /* what is going on */
    uint32_t reserved[16];
};

/** "-attract <name>": this run is the launcher's background. */
void world_attract_configure(const char *shared_name);
int world_attract_active(void);
/** Every world turn: the director. */
void world_attract_turn(void);
/** A drawn frame (RGBA; bottom_up: its first row is the picture's last). Any thread. */
void world_attract_frame(const unsigned char *rgba, int width, int height, int bottom_up);

#ifdef __cplusplus
}
#endif
#endif

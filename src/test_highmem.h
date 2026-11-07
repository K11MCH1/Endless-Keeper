/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file test_highmem.h
 *     Header file for test_highmem.c.
 */
/******************************************************************************/
#ifndef DK_TEST_HIGHMEM_H
#define DK_TEST_HIGHMEM_H

#ifdef __cplusplus
extern "C" {
#endif

/** Logs once how much of the space below 2 GB KFX_TEST_HIGHMEM reserved (nothing without it). */
void test_highmem_report(void);
/** Every game turn: the save tests (KFX_TEST_VERIFYSAVES, KFX_TEST_LOADSLOT). */
void test_hooks_turn(void);

#ifdef __cplusplus
}
#endif
#endif

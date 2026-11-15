/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_heresy.h
 *     Endless world: heresies, tomes that rewrite one rule of a dungeon at a price.
 * @par Purpose:
 *     A heroes' stronghold destroyed far from its destroyer's Heart (120 slabs or more) drops a heretical tome one time
 *     in four. The first keeper whose creature comes within two slabs takes it. "!heresy" lists a keeper's tomes,
 *     "!heresy take N" adopts tome N (two at most at once), "!heresy burn N" burns an adopted tome at the keeper's Temple
 *     (it needs one) and undoes it. The heresies built:
 *     - Open Graves: no creature wants a Lair (no anger for the lack), but sleeping heals at half speed;
 *     - Company Scrip: no wages, but everyone grows hungry twice as fast;
 *     - Night Court: double work by night, half work by day.
 *     Saved as the XTR2 record "HRS1".
 */
/******************************************************************************/
#ifndef DK_WORLD_HERESY_H
#define DK_WORLD_HERESY_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;
struct Coord3d;

enum WorldHeresy { WHe_None = 0, WHe_OpenGraves, WHe_CompanyScrip, WHe_NightCourt, WHe_OneKind, WHe_COUNT };
#define HERESY_ONE_KIND_LEVELS 5
#define HERESY_TOMES_PER_KEEPER 6
#define HERESY_ADOPTED_MAX 2
#define HERESY_FAR_SLABS 120

/** Whether a keeper has adopted this heresy. */
TbBool world_heresy_has(PlayerNumber plyr_idx, int heresy);
/** The rules it changes. */
TbBool world_heresy_no_lair(PlayerNumber plyr_idx);
int world_heresy_heal_pct(PlayerNumber plyr_idx);
GoldAmount world_heresy_pay(const struct Thing *creatng, GoldAmount pay);
int world_heresy_hunger_step(const struct Thing *creatng);
long world_heresy_work(const struct Thing *creatng, long value);
/** One Kind: the portal brings only the keeper's chosen kind (crmodel 0: nothing chosen this time, as given). */
ThingModel world_heresy_portal_kind(PlayerNumber plyr_idx, ThingModel crmodel);
/** One Kind: a creature that came through the portal is five levels up. */
void world_heresy_portal_arrived(struct Thing *creatng);
/** A heroes' stronghold's heart fell to keeper `by` at pos. */
void world_heresy_stronghold_destroyed(PlayerNumber by, const struct Coord3d *pos);
/** "!heresy ..." (also with '/'). */
TbBool world_heresy_command(const char *text);
/** The packet (simulating machine): op 1 list, 2 take tome n, 3 burn tome n. */
void world_heresy_packet(PlayerNumber plyr_idx, int op, int n);
/** Each world turn (server or single player): tomes found; the test hook. */
void world_heresy_turn(void);

void world_heresy_reset(void);
size_t world_heresy_save_size(void);
void world_heresy_save(uint8_t *buf);
TbBool world_heresy_load(const uint8_t *buf, size_t len);

/** "Start again" (or a new keeper in the seat): no tomes, no heresies. */
void world_heresy_seat_reset(PlayerNumber p);

#ifdef __cplusplus
}
#endif
#endif

/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file server_roles.h
 *     Endless Keeper: roles (Keeper Server's Permissions tab): what a player may do on a server, and the colour of
 *     their name in chat.
 */
/******************************************************************************/
#ifndef DK_SERVER_ROLES_H
#define DK_SERVER_ROLES_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
/* What a role may do: chat, and the admin's commands in game ("!kick Bob"). Keeper Server lists the same names. */
enum RolePermission {
    RolP_Chat = 0,
    RolP_Kick,
    RolP_Ban,
    RolP_Unban,
    RolP_Mute,
    RolP_Unmute,
    RolP_Give,
    RolP_Stats,
    RolP_Players,
    RolP_Say,
    RolP_Save,
    RolP_Reset,
    RolP_Restart,
    RolP_Stop,
    RolP_Cheats,
    RolP_Count
};

/** "chat", "kick", ... (NULL past the end). */
const char *role_permission_name(int perm);
/** The permission an in-game command needs (the command's own name), or -1 for a command that isn't a role's. */
int role_permission_by_name(const char *name);

/* The server */
/** Reads save/server_roles.txt (Keeper Server writes it) and save/server_role_members.txt again. */
void server_roles_reload(void);
/** Admin settings from save/server_roles.txt: "ingame" (in-game commands), "hierarchy" (a role acts only on lower
 *  roles), "announce" (kicks, bans and mutes from in game are told to everyone). */
TbBool server_roles_setting(const char *name);
/** May this user (a seated player) do this? Users with no seat have the Default role's permissions. */
TbBool server_roles_user_can(int user, int perm);
/** A seat's place among the roles: its top role's position (0 = the top role); lower is higher. */
int server_roles_seat_rank(PlayerNumber plyr_idx);
/** The seat's top role's name ("Default" for none). */
const char *server_roles_seat_top_name(PlayerNumber plyr_idx);
/** The seat's name colour as "#rrggbb" (its highest role with a colour), or "" for none. */
const char *server_roles_seat_color(PlayerNumber plyr_idx);
/** ,"roles":[...],"color":"#rrggbb" for the admin's player list. Returns the length written. */
size_t server_roles_seat_json(char *buf, size_t cap, PlayerNumber plyr_idx);
/** The admin's (Keeper Server's) "role ..." commands: add <role> <player>, remove <role> <player>, rename <old> <new>,
 *  reload. Only the admin connection gives roles. */
void server_roles_admin(const char *args);
/** Every server turn: the names and colours go to the clients that read them, when they change. */
void server_roles_turn(void);
/** A client said again what it reads (after a keyframe): it gets the table again. */
void server_roles_caps_changed(int user);

/* A client */
void roles_client_receive(const char *buf, size_t len);
void roles_client_reset(void);
/** The server's name and colour for this player (this client got the table, and has one for it). */
TbBool roles_client_player(PlayerNumber plyr_idx, const char **name, TbBool *has_color, uint32_t *rgb);

/******************************************************************************/
#ifdef __cplusplus
}
#endif
#endif

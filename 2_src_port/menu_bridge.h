/* menu_bridge.h - Shared integer settings, limits, colors and menu telemetry.
 * BR_SETTINGS is the schema used for declarations, defaults and validation.
 */
#ifndef MENU_BRIDGE_H
#define MENU_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

#define BR_MAGIC 0x57444D31u
#define BR_VERSION 18u

/* Named colour slots shared by the menu and the renderer.
 * 0..14 keep their historical order so existing CoreRR.cfg files load
 * unchanged. Slots 1-4 and 10-13 were never read by the renderer and stay
 * reserved. New slots are appended after 14, never inserted. */
typedef enum BR_ColorId {
    BR_COL_MARKER        = 0,
    BR_COL_LEGACY_1      = 1,
    BR_COL_LEGACY_2      = 2,
    BR_COL_LEGACY_3      = 3,
    BR_COL_LEGACY_4      = 4,
    BR_COL_DOWNED        = 5,
    BR_COL_NAME          = 6,
    BR_COL_WEAPON        = 7,
    BR_COL_DIST          = 8,
    BR_COL_ARMOR         = 9,
    BR_COL_LEGACY_10     = 10,
    BR_COL_LEGACY_11     = 11,
    BR_COL_LEGACY_12     = 12,
    BR_COL_LEGACY_13     = 13,
    BR_COL_AIM           = 14,
    BR_COL_HEALTH        = 15,
    BR_COL_CROUCH        = 16,
    BR_COL_SKEL          = 17,
    BR_COL_CHAM          = 18,
    BR_COL_BOX           = 19,
    BR_COL_RADAR_BG      = 20,
    BR_COL_RADAR_VEHICLE = 21,
    BR_COL_RADAR_LOOT    = 22,
    BR_COL_LOOT_NONE     = 23,
    BR_COL_LOOT_COMMON   = 24,
    BR_COL_LOOT_UNCOMMON = 25,
    BR_COL_LOOT_RARE     = 26,
    BR_COL_LOOT_EPIC     = 27,
    BR_COL_LOOT_LEGENDARY = 28,
    BR_COL_LOOT_EXOTIC   = 29,
    BR_COL_VEHICLE_MARKER = 30,
    BR_COL_VEHICLE_CHAM  = 31,
    BR_COL_SELF_STATUS   = 32,
    BR_COL_CONTAINER     = 33,
    BR_COL_DOOR          = 34,
    BR_COL_DROPPED_LOOT  = 35,
    BR_COL_VEHICLE_LOOT  = 36,
    BR_COL_RADAR_NODE    = 37,
    BR_COL_HIDE_OBJECT   = 38,
    BR_COLOR_COUNT       = 39
} BR_ColorId;

/* The self-test writes and reads color[BR_COL_WEAPON], and old CoreRR.cfg
 * files assume weapon sits at 7. Renumbering the enum must break the build
 * instead of silently moving the round-trip probe to another slot. */
typedef char br_weapon_slot_is_7[BR_COL_WEAPON == 7 ? 1 : -1];

/* Role colours stay hard-coded in rr_draw.c. This mirror only feeds the
 * read-only legend in the menu: never serialized, never written by col:. */
typedef enum BR_RoleColorId {
    BR_ROLE_COP = 0,
    BR_ROLE_ROBBER,
    BR_ROLE_HUBBER,
    BR_ROLE_CIVILIAN,
    BR_ROLE_CITIZEN,
    BR_ROLE_CLAN,
    BR_ROLE_POLICE,
    BR_ROLE_COLOR_COUNT
} BR_RoleColorId;

/* Reserved slots that exist only to keep old CoreRR.cfg files aligned. */
static int br_color_is_legacy(int id) {
    return (id >= BR_COL_LEGACY_1 && id <= BR_COL_LEGACY_4) ||
           (id >= BR_COL_LEGACY_10 && id <= BR_COL_LEGACY_13);
}

/* Shared schema for the WebView, defaults and on-disk settings.
 * Each entry is a field, default, minimum and maximum. */
#define BR_SETTINGS(X) \
    X(enabled, 1, 0, 1) \
    X(show_box, 1, 0, 1) \
    X(show_skeleton, 1, 0, 1) \
    X(show_name, 1, 0, 1) \
    X(show_armor, 1, 0, 1) \
    X(show_distance, 1, 0, 1) \
    X(show_weapon, 1, 0, 1) \
    X(show_head, 1, 0, 1) \
    X(show_self, 0, 0, 1) \
    X(show_clan, 1, 0, 1) \
    X(show_police, 1, 0, 1) \
    X(cham_on, 0, 0, 1) \
    X(max_distance, 30000, 0, 200000) \
    X(skel_distance, 15000, 0, 200000) \
    X(line_w, 1, 1, 4) \
    X(radar_on, 1, 0, 1) \
    X(radar_range, 150, 25, 500) \
    X(aa_on, 0, 0, 1) \
    X(aa_show_fov, 1, 0, 1) \
    X(aa_fov_px, 150, 30, 600) \
    X(aa_key, 0x12, 1, 254) \
    X(aa_smooth, 5, 1, 10) \
    X(aa_aim_cal, 100, 10, 2000) \
    X(aa_dist, 0, 0, 1000) \
    X(aa_target_clan, 0, 0, 1) \
    X(aa_target_police, 0, 0, 1) \
    X(aa_bone, 0, 0, 9) \
    X(aa_los_on, 0, 0, 1) \
    X(aa_closest_first, 0, 0, 1) \
    X(show_loot, 0, 0, 1) \
    X(show_loot_nodes, 0, 0, 1) \
    X(loot_tier_none, 1, 0, 1) \
    X(loot_tier_common, 1, 0, 1) \
    X(loot_tier_uncommon, 1, 0, 1) \
    X(loot_tier_rare, 1, 0, 1) \
    X(loot_tier_epic, 1, 0, 1) \
    X(loot_tier_legendary, 1, 0, 1) \
    X(loot_tier_exotic, 1, 0, 1) \
    X(vehicle_chams, 0, 0, 1) \
    X(show_containers, 0, 0, 1) \
    X(show_doors, 0, 0, 1) \
    X(show_dropped_loot, 0, 0, 1) \
    X(show_vehicle_loot, 0, 0, 1) \
    X(radar_world, 0, 0, 1) \
    X(show_crouch, 0, 0, 1) \
    X(show_health, 0, 0, 1) \
    X(show_vehicle_markers, 0, 0, 1) \
    X(show_vehicle_occupants, 0, 0, 1) \
    X(show_world_items, 0, 0, 1) \
    X(show_world_names, 1, 0, 1) \
    X(show_cop_counter, 0, 0, 1) \
    X(show_hide_objects, 0, 0, 1) \
    X(world_item_distance, 500, 50, 5000) \
    X(instant_loot, 0, 0, 1) \
    X(debug_slots, 0, 0, 1)   /* slot-chain diagnostic: slots_debug lines in status.log */

typedef struct BR_Config {
    unsigned int magic, version;
#define BR_DECLARE(field, initial, minimum, maximum) int field;
    BR_SETTINGS(BR_DECLARE)
#undef BR_DECLARE
    struct { unsigned char rgb[3]; } color[BR_COLOR_COUNT];
    /* Display-only copy of the fixed role colours; not part of the file. */
    struct { unsigned char rgb[3]; } role_color[BR_ROLE_COLOR_COUNT];
    /* UI-only accent colors; serialized as accent= and secondary= in CoreRR.cfg. */
    struct { unsigned char rgb[3]; } accent, secondary;

    int menu_open;
    int telem_targets;
    int telem_game_pid;
    int telem_cham_n;
    int telem_los_state; /* 0 off, 1 loading/stale, 2 ready */
    int telem_cop_count;  /* published by extras_refresh(), read by telem: */
    int telem_slot_total;
    int telem_slot_seats; /* slot-table rows: occupied seats + free seats    */

#ifdef RR_PORT_DIAGNOSTICS
    int show_vehicles;
#endif
} BR_Config;

/* Unrecognized values use the same no-rarity bucket as the loot renderer. */
static int br_loot_rarity_visible(const BR_Config* config, int rarity) {
    switch (rarity) {
        case 1: return config->loot_tier_common;
        case 2: return config->loot_tier_uncommon;
        case 3: return config->loot_tier_rare;
        case 4: return config->loot_tier_epic;
        case 5: return config->loot_tier_legendary;
        case 6: return config->loot_tier_exotic;
        default: return config->loot_tier_none;
    }
}

BR_Config* br_get(void);
int br_config_set(BR_Config* config, const char* key, int value);
int br_config_save(void);
int br_config_load(void);

int br_start_menu(void);
void br_stop_menu(void);
void br_set_menu_visible(int visible);
void br_request_toggle(void);

#ifdef __cplusplus
}
#endif
#endif

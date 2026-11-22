/* rr_extras.c - Cop counter, world item collections and instant loot.
 *
 * Two read features share one publication:
 *   - the roster's cop count, derived from BattleCharacterSlotsModel and the
 *     entity map, so it counts seats that hold a character (not the entity
 *     dictionary, which also carries non-player entities);
 *   - a snapshot of the five WorldEntitiesModel collections with a position and
 *     a description for each.
 *
 * The reads run in extras_refresh(), which the discovery worker calls. The
 * renderer and the menu copy extras_world_snapshot() / extras_cop_count(), so a
 * frame never waits on the driver and a torn-down world can never be walked
 * from the render thread.
 *
 * Instant loot is the one writer: a 250 ms loop owned by this module zeroes the
 * reveal timers of the container being viewed. There is a single rr_write() call
 * site (ex_float_write) and it is reachable only from extras_insta_tick(), after
 * the switch, pointer, class and value-range checks. With the switch off no pass
 * runs at all, so the driver sees no write packet.
 *
 * The reads are defensive on purpose: every pointer is class-checked before it
 * is dereferenced, both entity id sources must agree, and a coordinate outside
 * +/-EX_MAX_COORD rejects the whole item. A wrong offset therefore produces no
 * items (or a cop count of zero) instead of a crash.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "menu_bridge.h"
#include "rr_mem.h"
#include "rr_unity.h"
#include "rr_extras.h"
#include "rr_game.h"    /* game_scene_debug(): the validated views updater */

/* ---- publication ------------------------------------------------------- */
static SRWLOCK g_lock = SRWLOCK_INIT;
static EXTRA_WORLD_ITEM g_items[EXTRA_MAX_WORLD_ITEMS];
static int   g_item_count;
static int   g_cop_count;
static int   g_slot_total;
static DWORD g_stamp;          /* worker thread only */
static int   g_have_stamp;

/* Instant loot loop control, defined with the feature below. extras_reset()
 * stops the loop before the publication is dropped: the loop must not outlive
 * the driver connection it writes through. */
static void ex_inst_halt(void);
static void ex_inst_arm(uint64_t battle);

void extras_reset(void) {
    ex_inst_halt();
    AcquireSRWLockExclusive(&g_lock);
    g_item_count = 0;
    g_cop_count = 0;
    g_slot_total = 0;
    g_have_stamp = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

int extras_cop_count(void) {
    AcquireSRWLockShared(&g_lock);
    int value = g_cop_count;
    ReleaseSRWLockShared(&g_lock);
    return value;
}

int extras_slot_total(void) {
    AcquireSRWLockShared(&g_lock);
    int value = g_slot_total;
    ReleaseSRWLockShared(&g_lock);
    return value;
}

int extras_world_snapshot(EXTRA_WORLD_ITEM* out, int max) {
    if (!out || max <= 0) return 0;
    AcquireSRWLockShared(&g_lock);
    int n = g_item_count < max ? g_item_count : max;
    if (n > 0) memcpy(out, g_items, sizeof(out[0]) * (size_t)n);
    ReleaseSRWLockShared(&g_lock);
    return n;
}

/* ---- small read helpers ------------------------------------------------ */

/* Read one world position and reject torn-down worlds. */
static int ex_position(uint64_t addr, EXTRA_WORLD_ITEM* out) {
    float v[3];
    if (!rr_read(addr, v, sizeof(v))) return 0;
    for (int i = 0; i < 3; ++i)
        if (!isfinite(v[i]) || fabsf(v[i]) > EX_MAX_COORD) return 0;
    out->x = v[0]; out->y = v[1]; out->z = v[2];
    return 1;
}

/* A name candidate is an Il2CppString of 2..64 printable UTF-16 units. Reading
 * the characters here (instead of u_string) keeps it to one length read and one
 * character read, and validates the range before converting. */
static int ex_string_at(uint64_t str, char* out, size_t cap) {
    out[0] = 0;
    if (!PL(str) || cap < 8) return 0;
    int32_t len = 0;
    if (!rr_read(str + EX_OFF_STRING_LEN, &len, sizeof(len))) return 0;
    if (len < 2 || len > 64 || (size_t)len + 4 > cap) return 0;
    uint16_t chars[64];
    if (!rr_read(str + EX_OFF_STRING_CHARS, chars, (size_t)len * 2)) return 0;
    for (int i = 0; i < len; ++i)
        if (chars[i] < 0x20 || chars[i] > 0x2FF) return 0;
    size_t k = 0;
    for (int i = 0; i < len && k + 4 < cap; ++i) {
        unsigned c = chars[i];
        if (c < 0x80) out[k++] = (char)c;
        else if (c < 0x800) {
            out[k++] = (char)(0xC0 | (c >> 6));
            out[k++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[k++] = (char)(0xE0 | (c >> 12));
            out[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[k++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[k] = 0;
    return k > 0;
}
/* Type names are not descriptions; they come from class words that happen to
 * sit inside the object. */
static int ex_name_ok(const char* s) {
    size_t n = strlen(s);
    if (n < 2 || n > 63) return 0;
    if (strstr(s, "Models.") || strstr(s, "System.") || strstr(s, "UnityEngine")) return 0;
    return 1;
}

/* Winning scan offset per kind. The property layout does not change between
 * patches, so the steady state costs one pointer read per item. */
typedef struct { unsigned off; int used; } EX_NAME_HINT;
static EX_NAME_HINT g_name_hint[8];   /* worker thread only */

/* One field-body read per item instead of 60 pointer reads. Larger objects are
 * common, but a short instance must not fail the whole name. */
static size_t ex_body_read(uint64_t obj, uint8_t* body, size_t want) {
    static const size_t sizes[] = { 0x200, 0x100, 0x80, 0x40 };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        size_t n = sizes[i] < want ? sizes[i] : want;
        if (n >= 8 && rr_read(obj + U_OBJ_HEADER, body, n)) return n;
    }
    return 0;
}

static int ex_name_scan(uint64_t obj, int hint, char* out, size_t cap) {
    out[0] = 0;
    if (hint < 0 || hint >= (int)(sizeof(g_name_hint) / sizeof(g_name_hint[0]))) hint = -1;
    if (hint >= 0 && g_name_hint[hint].used &&
        ex_string_at(rr_u64(obj + g_name_hint[hint].off), out, cap) && ex_name_ok(out))
        return 1;
    uint8_t body[EX_NAME_SCAN_END];
    size_t got = ex_body_read(obj, body, sizeof(body));
    if (!got) return 0;
    for (unsigned off = U_OBJ_HEADER; off + 8 <= U_OBJ_HEADER + (unsigned)got; off += 8) {
        uint64_t p;
        memcpy(&p, body + (off - U_OBJ_HEADER), 8);
        if (!PL(p)) continue;
        if (!ex_string_at(p, out, cap) || !ex_name_ok(out)) continue;
        if (hint >= 0) { g_name_hint[hint].off = off; g_name_hint[hint].used = 1; }
        return 1;
    }
    out[0] = 0;
    return 0;
}
/* ---- dictionary walking ------------------------------------------------ */

typedef struct { uint64_t entries; int count; } EX_DICT;

/* A dictionary is walked only when the entry stride matches the layout this
 * build uses; u_dict() reports zero for an empty or unreadable dictionary,
 * which the callers treat as "nothing published", never as an error. */
static int ex_dict(uint64_t dict, EX_DICT* out) {
    int n = 0, stride = 0;
    uint64_t entries = 0;
    out->entries = 0;
    out->count = 0;
    if (!u_dict(dict, &n, &entries, &stride)) return 0;
    if (stride != (int)EX_OFF_DICT_STRIDE || n < 1 || n > EX_MAX_WORLD_DICT) return 0;
    out->entries = entries;
    out->count = n;
    return 1;
}

/* The whole entry array in one read: three reads per row would dominate. */
static uint8_t g_rows[EX_MAX_WORLD_DICT * EX_OFF_DICT_STRIDE];
static int ex_dict_rows(const EX_DICT* d, int cap_rows) {
    int n = d->count < cap_rows ? d->count : cap_rows;
    if (n <= 0) return 0;
    size_t bytes = (size_t)n * (size_t)EX_OFF_DICT_STRIDE;
    return rr_read(d->entries, g_rows, bytes) ? n : 0;
}

static int32_t ex_row_id(const uint8_t* row) {
    int32_t hash = 0, key = 0;
    memcpy(&hash, row + EX_OFF_DICT_HASH, 4);
    if (hash < 0) return -1;                 /* freed slot */
    memcpy(&key, row + EX_OFF_DICT_KEY, 4);
    return key;
}

static uint64_t ex_row_value(const uint8_t* row) {
    uint64_t value = 0;
    memcpy(&value, row + EX_OFF_DICT_VALUE, 8);
    return value;
}

/* ---- Feature 1: cop counter -------------------------------------------- */

/* Slot entity ids. Both id sources (the dictionary key and the slot's own
 * EntityId) must agree, the same rule cache_roster_identity() applies. */
static int ex_slots(uint64_t battle, int32_t* ids, int max) {
    if (!u_is(battle, "BattleModel")) return -1;
    uint64_t model = rr_u64(battle + EX_OFF_BATTLE_SLOTS_MODEL);
    if (!u_is(model, "BattleCharacterSlotsModel")) return -1;
    EX_DICT dict;
    if (!ex_dict(rr_u64(model + EX_OFF_SLOTS_ARRAY), &dict)) return 0;
    int n = ex_dict_rows(&dict, EX_MAX_SLOTS);
    if (n <= 0) return 0;
    int count = 0;
    for (int i = 0; i < n; ++i) {
        const uint8_t* row = g_rows + (size_t)i * EX_OFF_DICT_STRIDE;
        int32_t key = ex_row_id(row);
        if (key < 0) continue;
        uint64_t slot = ex_row_value(row);
        if (!u_is(slot, "BattleCharacterSlotModel")) continue;
        uint64_t data = rr_u64(slot + EX_OFF_SLOT_DATA);
        if (!u_is(data, "BattleCharacterSlotData")) continue;
        uint64_t ref = rr_u64(data + EX_OFF_SLOT_ENTITY_ID);
        int32_t id = -1;
        if (!PL(ref) || !rr_read(ref + EX_OFF_PRIM_VALUE, &id, sizeof(id))) continue;
        if (id <= 0 || id != key) continue;   /* empty seat, or the two disagree */
        if (count < max) ids[count] = id;
        ++count;
    }
    return count;
}

/* Replicated role name, the same chain fill_entry() follows in rr_game.c. */
static void ex_role_string(uint64_t character, char* out, size_t cap) {
    out[0] = 0;
    if (!PL(character) || cap < 2) return;
    uint64_t data = rr_u64(character + EX_OFF_CHAR_DATA);
    uint64_t common = PL(data) ? rr_u64(data + EX_OFF_DATA_COMMON) : 0;
    uint64_t ref = PL(common) ? rr_u64(common + EX_OFF_COMMON_ROLE) : 0;
    uint64_t desc = PL(ref) ? rr_u64(ref + EX_OFF_REF_VALUE) : 0;
    uint64_t str = PL(desc) ? rr_u64(desc + EX_OFF_DESC_NAME) : 0;
    if (PL(str)) u_string(str, out, cap);
    out[cap - 1] = 0;
}
typedef struct { int32_t id; int cop; } EX_ROLE;

/* Resolve the roles of the seated entity ids only: the entity map also holds
 * vehicles and other non-character entities, and their keys never match. */
static int ex_roles(uint64_t battle, const int32_t* ids, int id_count,
                    EX_ROLE* out, int cap) {
    uint64_t entities = rr_u64(battle + EX_OFF_BATTLE_ENTITIES);
    if (!u_is(entities, "BattleEntitiesModel")) return 0;
    EX_DICT dict;
    if (!ex_dict(rr_u64(entities + EX_OFF_BATTLE_MODELS), &dict)) return 0;
    int n = ex_dict_rows(&dict, EX_MAX_ENTITY_DICT);
    if (n <= 0) return 0;
    int count = 0;
    for (int i = 0; i < n && count < cap; ++i) {
        const uint8_t* row = g_rows + (size_t)i * EX_OFF_DICT_STRIDE;
        int32_t key = ex_row_id(row);
        if (key < 0) continue;
        int wanted = 0;
        for (int j = 0; j < id_count; ++j)
            if (ids[j] == key) { wanted = 1; break; }
        if (!wanted) continue;
        uint64_t entity = ex_row_value(row);
        if (!u_is(entity, "BattleCharacterEntityModel")) continue;
        uint64_t character = rr_u64(entity + EX_OFF_ENTITY_SERVER);
        if (!u_is(character, "BattleCharacterModel")) continue;
        int32_t character_id = -1;
        if (!rr_read(character + EX_OFF_CHAR_ID, &character_id, sizeof(character_id)) ||
            character_id != key) continue;
        char role[20];
        ex_role_string(character, role, sizeof(role));
        out[count].id = key;
        out[count].cop = strcmp(role, "cop") == 0;   /* rr_game.c police test */
        ++count;
    }
    return count;
}

static void ex_cop_scan(uint64_t battle, int* cops, int* slots) {
    *cops = 0;
    *slots = 0;
    static int32_t ids[EX_MAX_SLOTS];
    int n = ex_slots(battle, ids, EX_MAX_SLOTS);
    if (n <= 0) return;
    *slots = n > EX_MAX_SLOTS ? EX_MAX_SLOTS : n;
    static EX_ROLE roles[EX_MAX_SLOTS];
    int rn = ex_roles(battle, ids, *slots, roles, EX_MAX_SLOTS);
    for (int i = 0; i < *slots; ++i)
        for (int j = 0; j < rn; ++j)
            if (roles[j].id == ids[i]) {
                if (roles[j].cop) ++*cops;
                break;
            }
}
/* ---- Feature 2: world items -------------------------------------------- */

typedef struct {
    int kind;
    unsigned dict_off;
    unsigned pos_off;
    const char* klass;
    const char* label;
} EX_SOURCE;

static const EX_SOURCE g_sources[] = {
    {1, EX_OFF_WE_CONTAINERS,    EX_OFF_CONTAINER_POS,    "ContainerModel",   "Container"},
    {2, EX_OFF_WE_LOOTS,         EX_OFF_LOOT_POS,         "LootModel",        "Loot"},
    {3, EX_OFF_WE_VEHICLE_LOOTS, EX_OFF_VEHICLE_LOOT_POS, "VehicleLootModel", "Vehicle loot"},
    {4, EX_OFF_WE_DOORS,         EX_OFF_DOOR_POS,         "DoorModel",        "Door"},
};

/* DoorModel.State references a DoorStateModel whose _state reference resolves
 * to a DoorStateDescription; that description's name is the readable state.
 * Same chain cache_world_markers() uses for its "Door: <state>" label. */
static void ex_door_name(uint64_t door, char* out, size_t cap) {
    out[0] = 0;
    uint64_t state = rr_u64(door + EX_OFF_DOOR_STATE);
    if (!u_is(state, "DoorStateModel")) return;
    uint64_t data = rr_u64(state + RR_OFF_DOOR_STATE_DATA);
    if (!u_is(data, "ReferenceData`1")) return;
    uint64_t value = rr_u64(data + EX_OFF_REF_VALUE);
    if (!u_is(value, "DoorStateDescription")) return;
    char name[24] = {0};
    if (u_string(rr_u64(value + EX_OFF_DESC_NAME), name, sizeof(name)) && name[0])
        snprintf(out, cap, "Door: %s", name);
}

/* Containers, loot and vehicle loot keep their position in a small model; both
 * PositionModel and InteractPositionModel put X/Y/Z at 0x10/0x14/0x18. Doors
 * carry the three floats inline. */
static int ex_item_position(const EX_SOURCE* source, uint64_t obj, EXTRA_WORLD_ITEM* item) {
    if (source->kind == 4) return ex_position(obj + source->pos_off, item);
    uint64_t pos = rr_u64(obj + source->pos_off);
    if (!u_is(pos, "PositionModel") && !u_is(pos, "InteractPositionModel")) return 0;
    return ex_position(pos + EX_OFF_POS_X, item);
}

static int ex_world_items(uint64_t world, EXTRA_WORLD_ITEM* out, int count, int max) {
    for (int s = 0; s < (int)(sizeof(g_sources) / sizeof(g_sources[0])); ++s) {
        const EX_SOURCE* source = &g_sources[s];
        EX_DICT dict;
        if (!ex_dict(rr_u64(world + source->dict_off), &dict)) continue;
        int n = ex_dict_rows(&dict, EX_MAX_WORLD_DICT);
        for (int i = 0; i < n && count < max; ++i) {
            const uint8_t* row = g_rows + (size_t)i * EX_OFF_DICT_STRIDE;
            int32_t id = ex_row_id(row);
            if (id < 0) continue;
            uint64_t obj = ex_row_value(row);
            if (!u_is(obj, source->klass)) continue;
            EXTRA_WORLD_ITEM item;
            memset(&item, 0, sizeof(item));
            item.id = id;
            item.kind = source->kind;
            if (!ex_item_position(source, obj, &item)) continue;
            if (source->kind == 4) ex_door_name(obj, item.name, sizeof(item.name));
            if (!item.name[0] && !ex_name_scan(obj, s, item.name, sizeof(item.name)))
                snprintf(item.name, sizeof(item.name), "%s", source->label);
            out[count++] = item;
        }
    }
    return count;
}
/* Hiding objects (boxes, toilets, ...) hang their position and id off
 * LocationHideObjectDescription; the object keeps the occupant id. */
static int ex_hide_items(uint64_t world, EXTRA_WORLD_ITEM* out, int count, int max) {
    EX_DICT dict;
    if (!ex_dict(rr_u64(world + EX_OFF_WE_HIDE_OBJECTS), &dict)) return count;
    int n = ex_dict_rows(&dict, EX_MAX_WORLD_DICT);
    for (int i = 0; i < n && count < max; ++i) {
        const uint8_t* row = g_rows + (size_t)i * EX_OFF_DICT_STRIDE;
        int32_t id = ex_row_id(row);
        if (id < 0) continue;
        uint64_t obj = ex_row_value(row);
        if (!u_is(obj, "HideObjectModel")) continue;
        uint64_t desc = rr_u64(obj + EX_OFF_HIDE_DESC);
        if (!u_is(desc, "LocationHideObjectDescription")) continue;
        int32_t desc_id = -1;
        if (!rr_read(desc + EX_OFF_HIDE_DESC_ID, &desc_id, sizeof(desc_id)) || desc_id != id) continue;
        EXTRA_WORLD_ITEM item;
        memset(&item, 0, sizeof(item));
        item.id = id;
        item.kind = 5;
        if (!ex_position(desc + EX_OFF_HIDE_DESC_POS, &item)) continue;
        int32_t occupant = 0;
        if (rr_read(obj + EX_OFF_HIDE_ENTITY_ID, &occupant, sizeof(occupant))) item.extra = occupant;
        if (!ex_name_scan(obj, 4, item.name, sizeof(item.name)))
            snprintf(item.name, sizeof(item.name), "Hiding spot");
        out[count++] = item;
    }
    return count;
}

/* ---- refresh and publication ------------------------------------------- */

void extras_refresh(uint64_t battle, uint64_t world, int force) {
    BR_Config* cfg = br_get();
    /* Arm or disarm the instant loot loop before the throttle: the loop needs
     * the battle anchor, and this worker is the only place that has it. With
     * the switch off the anchor is 0 and no pass runs. */
    ex_inst_arm(cfg->instant_loot ? battle : 0);
    DWORD now = GetTickCount();
    if (!force && g_have_stamp && now - g_stamp < 400) return;

    int want_cops  = cfg->show_cop_counter != 0;
    int want_items = cfg->show_world_items != 0;
    int want_hide  = cfg->show_hide_objects != 0;

    /* Nothing consumes the result: drop the publication instead of spending
     * driver reads on it. */
    if (!want_cops && !want_items && !want_hide) {
        AcquireSRWLockExclusive(&g_lock);
        g_cop_count = 0; g_slot_total = 0; g_item_count = 0;
        ReleaseSRWLockExclusive(&g_lock);
        cfg->telem_cop_count = 0;
        cfg->telem_slot_total = 0;
        g_stamp = now; g_have_stamp = 1;
        return;
    }

    int cops = 0, slots = 0;
    static EXTRA_WORLD_ITEM staged[EXTRA_MAX_WORLD_ITEMS];
    int items = 0;
    int have_world = PL(world) && u_is(world, "WorldEntitiesModel");
    if (want_cops && PL(battle)) ex_cop_scan(battle, &cops, &slots);
    if (have_world && (want_items || want_hide)) {
        if (want_items) items = ex_world_items(world, staged, items, EXTRA_MAX_WORLD_ITEMS);
        if (want_hide)  items = ex_hide_items(world, staged, items, EXTRA_MAX_WORLD_ITEMS);
    }
    /* The world link can be torn down while the walk runs; a published item
     * list from a dead world would outlive the map it belongs to. */
    if (have_world && (!PL(world) || !u_is(world, "WorldEntitiesModel"))) items = 0;

    AcquireSRWLockExclusive(&g_lock);
    if (items > 0) memcpy(g_items, staged, sizeof(staged[0]) * (size_t)items);
    g_item_count = items;
    g_cop_count = cops;
    g_slot_total = slots;
    ReleaseSRWLockExclusive(&g_lock);

    /* The menu reads these through the existing telem: channel. */
    cfg->telem_cop_count = cops;
    cfg->telem_slot_total = slots;

    g_stamp = now;
    g_have_stamp = 1;
}

/* ---- Feature 3: instant loot (the only writer) -------------------------- */

/* Guarded so arm and halt cannot interleave. Both run rarely (a refresh that
 * sees the switch, and teardown), so the loop never contends on this lock. */
static SRWLOCK g_inst_lock = SRWLOCK_INIT;
static HANDLE   g_inst_thread = NULL;
static volatile LONG   g_inst_stop = 0;   /* 1 = the loop must return      */
static volatile LONG   g_inst_clean = 1;  /* 1 = no previous loop is alive */
static volatile LONG64 g_inst_battle = 0; /* battle anchor, 0 = disarmed   */

/* A field we are about to zero must read as a live duration first: finite,
 * non-negative and inside the bound the 19/11 table gives it. Anything else
 * means the offset does not describe what we think it does, and writing there
 * would corrupt unrelated state. */
static int ex_float_in(uint64_t addr, float max, float* out) {
    if (!PL(addr)) return 0;
    float v = 0.0f;
    if (!rr_read(addr, &v, sizeof(v))) return 0;
    if (!isfinite(v) || v < 0.0f || v > max) return 0;
    *out = v;
    return 1;
}

/* One float write. PL() keeps the address inside the target's user space and
 * the alignment check stops a garbage pointer from producing a misaligned field
 * write; rr_write() then fails closed on an unmapped page (the driver reports
 * STATUS_PARTIAL_COPY and this returns 0). */
static int ex_float_write(uint64_t addr, float value) {
    if (!PL(addr) || (addr & 3u) != 0) return 0;
    return rr_write(addr, &value, sizeof(value));
}

int extras_insta_tick(uint64_t battle, uint64_t updater) {
    /* One read of the switch decides whether this pass may write at all. */
    if (!br_get()->instant_loot) return 0;
    if (!PL(battle) || !PL(updater)) return 0;
    if (!u_is(updater, "BattleEntitiesUpdater")) return 0;
    if (rr_u64(updater + RR_OFF_VIEWS_BATTLE) != battle) return 0;

    /* Both links must be live: closing the container nulls the content link,
     * and that is the signal to stop writing. */
    uint64_t screen = rr_u64(updater + EX_OFF_UPDATER_TO_SCREEN);
    if (!PL(screen)) return 0;
    uint64_t inst = rr_u64(screen + EX_OFF_SCREEN_TO_INST);
    if (!PL(inst)) return 0;

    int wrote = 0;
    float time_cell = 0.0f;
    if (!ex_float_in(inst + EX_OFF_INST_TIME_CELL, EX_INST_TIME_CELL_MAX, &time_cell))
        return 0;   /* not the object we expect: touch nothing */
    if (time_cell > 0.0f && ex_float_write(inst + EX_OFF_INST_TIME_CELL, 0.0f)) ++wrote;

    uint64_t list = rr_u64(inst + EX_OFF_INST_LIST);
    if (!PL(list)) return wrote;
    int32_t count = 0;
    if (!rr_read(list + EX_OFF_LIST_COUNT, &count, sizeof(count))) return wrote;
    if (count <= 0 || count > EX_INST_MAX_ITEMS) return wrote;   /* sanity */
    uint64_t items = rr_u64(list + EX_OFF_LIST_ITEMS);
    if (!PL(items)) return wrote;

    for (int32_t i = 0; i < count; ++i) {
        uint64_t pair = items + (uint64_t)i * EX_OFF_LIST_STRIDE;
        uint64_t desc = rr_u64(pair + EX_OFF_PAIR_DESC);
        if (!PL(desc)) continue;
        float shine = 0.0f, icon = 0.0f;
        /* Both timers must be in range before either is written: a descriptor
         * that fails one bound is not trusted for the other. */
        if (!ex_float_in(desc + EX_OFF_DESC_SHINE, EX_INST_DURATION_MAX, &shine)) continue;
        if (!ex_float_in(desc + EX_OFF_DESC_ICON_REVEAL, EX_INST_DURATION_MAX, &icon)) continue;
        if (shine > 0.0f && ex_float_write(desc + EX_OFF_DESC_SHINE, 0.0f)) ++wrote;
        if (icon > 0.0f && ex_float_write(desc + EX_OFF_DESC_ICON_REVEAL, 0.0f)) ++wrote;
        /* desc + EX_OFF_DESC_GLOW_COLOR (GlowColor) is deliberately untouched. */
    }
    return wrote;
}

/* 250 ms loop. It never touches the target directly: each pass re-validates the
 * anchor and hands the tick a pointer the game's own discovery worker published
 * (g_views_updater, which views_updater_valid() already accepted). Sleeping in
 * 50 ms slices keeps a shutdown prompt. */
static DWORD WINAPI ex_inst_loop(LPVOID unused) {
    (void)unused;
    for (;;) {
        if (InterlockedCompareExchange(&g_inst_stop, 0, 0)) return 0;
        LONG64 battle = InterlockedCompareExchange64(&g_inst_battle, 0, 0);
        if (battle && br_get()->instant_loot) {
            uint64_t ub = 0, updater = 0;
            game_scene_debug(&ub, &updater, NULL);
            if (updater && ub == (uint64_t)battle)
                extras_insta_tick((uint64_t)battle, updater);
        }
        for (int i = 0; i < EX_INST_TICK_MS / 50; ++i) {
            if (InterlockedCompareExchange(&g_inst_stop, 0, 0)) return 0;
            Sleep(50);
        }
    }
}

/* Called from extras_refresh() with the battle anchor while the switch is on,
 * and with 0 while it is off. A 0 anchor only disarms: the loop exits on the
 * next stop request or idles until a new anchor arrives. */
static void ex_inst_arm(uint64_t battle) {
    AcquireSRWLockExclusive(&g_inst_lock);
    InterlockedExchange64(&g_inst_battle, (LONG64)battle);
    if (battle && !g_inst_thread && g_inst_clean) {
        InterlockedExchange(&g_inst_stop, 0);
        g_inst_thread = CreateThread(NULL, 0, ex_inst_loop, NULL, 0, NULL);
        InterlockedExchange(&g_inst_clean, g_inst_thread ? 0 : 1);
    }
    ReleaseSRWLockExclusive(&g_inst_lock);
}

/* Stop the loop and wait for it. A join that times out is never retried, so two
 * loops can never run at once; the flag keeps the next arm from starting one. */
static void ex_inst_halt(void) {
    AcquireSRWLockExclusive(&g_inst_lock);
    InterlockedExchange(&g_inst_stop, 1);
    InterlockedExchange64(&g_inst_battle, 0);
    HANDLE thread = g_inst_thread;
    g_inst_thread = NULL;
    ReleaseSRWLockExclusive(&g_inst_lock);
    if (!thread) {
        InterlockedExchange(&g_inst_clean, 1);
        return;
    }
    DWORD waited = WaitForSingleObject(thread, 3000);
    CloseHandle(thread);
    InterlockedExchange(&g_inst_clean, waited == WAIT_OBJECT_0 ? 1 : 0);
}






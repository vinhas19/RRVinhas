/* rr_extras.c - Cop counter, world item collections and instant loot.
 *
 * Two read features share one publication:
 *   - the roster's cop count, derived from BattleCharacterSlotsModel (the seat
 *     table) and the role each seated character replicates, read from the world
 *     roster the player ESP enumerates, so it counts seats that hold a cop (not
 *     the entity dictionary, which also carries non-player entities). The slot
 *     table's row count travels with it as the seat capacity, which is what the
 *     free-seat count in the overlay HUD is measured against;
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
    /* The telemetry copy is what the menu and the on-screen cop HUD read, so a
     * teardown clears it as well: after the game is gone the overlay must not
     * keep drawing the last battle's counter. */
    BR_Config* cfg = br_get();
    cfg->telem_cop_count = 0;
    cfg->telem_slot_total = 0;
    cfg->telem_slot_seats = 0;
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

/* Reference keyed row (Dictionary<TRef, TRef>): the key is 8 bytes wide at the
 * offset an int32 key occupies, so it must be read whole and class-checked
 * instead of through ex_row_id(). _slots is the only such table here. */
static uint64_t ex_row_ref(const uint8_t* row) {
    uint64_t key = 0;
    memcpy(&key, row + EX_OFF_SLOT_KEY, 8);
    return key;
}

/* ---- Feature 1: cop counter -------------------------------------------- */

/* ---- diagnostic: the slot chain, one status.log line per second ----------
 *
 * The cop counter fails closed: a stale offset publishes a count of zero, which
 * is indistinguishable from a lobby with no seats. With debug_slots on, every
 * refresh appends the raw contents of each link to status.log, so the first link
 * that reads as zero (or as a class that does not match) names the culprit.
 *
 * The values are read directly instead of through u_is()/u_dict(): the point is
 * to show what is actually there, including a class name that does not match.
 * Worker thread only, like the rest of the chain; the throttle is one line per
 * second so a several-minute session stays readable.
 */
/* status.log beside the exe: the debug_slots line and the --probe layout dump
 * both append to it. Returns NULL when the module path cannot be built or the
 * file cannot be opened, and the caller then writes nothing. */
static FILE* ex_status_open(void) {
    char path[MAX_PATH];
    DWORD length = GetModuleFileNameA(NULL, path, MAX_PATH);
    char* slash = length ? strrchr(path, '\\') : NULL;
    if (!slash) return NULL;
    *(slash + 1) = 0;
    strncat(path, "status.log", MAX_PATH - strlen(path) - 1);
    return fopen(path, "a");
}

static void ex_cls_name(uint64_t obj, char* out, size_t cap) {
    uint64_t klass = PL(obj) ? u_klass(obj) : 0;
    if (!klass || !u_class_name(klass, out, cap)) snprintf(out, cap, "?");
}

static void ex_slots_debug(uint64_t battle) {
    static DWORD last;                 /* worker thread only */
    DWORD now = GetTickCount();
    if (last && now - last < 1000) return;
    last = now;

    /* battle + 0x4D0 -> BattleCharacterSlotsModel */
    uint64_t model = PL(battle) ? rr_u64(battle + EX_OFF_BATTLE_SLOTS_MODEL) : 0;
    int is_slots = u_is(model, "BattleCharacterSlotsModel");

    /* model + 0x10 -> _slots dictionary; +0x18 entry array, +0x20 used count */
    uint64_t dict = PL(model) ? rr_u64(model + EX_OFF_SLOTS_ARRAY) : 0;
    int used = 0, cnt = 0, stride = 0;
    uint64_t entries = 0, uentries = 0;
    if (PL(dict)) {
        rr_read(dict + EX_OFF_DICT_USED, &used, sizeof(used));
        entries = rr_u64(dict + EX_OFF_DICT_ENTRIES);
        u_dict(dict, &cnt, &uentries, &stride);
    }

    /* First row whose hash is not negative: row 0 can be a freed slot, which
     * carries neither a key nor a value. _slots is keyed by a reference, so the
     * key is read whole (8 bytes) and class-checked -- first_key is only its low
     * half and must not be mistaken for an entity id. */
    int hash = 0, key = 0, row = -1, is_slot = 0, is_data = 0, key_is_desc = 0, eid = -1;
    uint64_t key_q = 0, slot = 0, data = 0, ref = 0, desc = 0;
    if (PL(entries)) {
        enum { EX_DBG_ROWS = 64 };
        uint8_t raw[EX_DBG_ROWS * EX_OFF_DICT_STRIDE];
        int rows = used >= 1 && used <= EX_DBG_ROWS ? used : EX_DBG_ROWS;
        if (rr_read(entries, raw, (size_t)rows * EX_OFF_DICT_STRIDE)) {
            for (int i = 0; i < rows; ++i) {
                const uint8_t* r = raw + (size_t)i * EX_OFF_DICT_STRIDE;
                memcpy(&hash, r + EX_OFF_DICT_HASH, 4);
                if (hash < 0) continue;
                row = i;
                key_q = ex_row_ref(r);
                key = (int32_t)(key_q & 0xFFFFFFFFu);
                slot = ex_row_value(r);
                break;
            }
        }
    }
    /* The seat table stores a BattleCharacterSlotDescription and a
     * BattleCharacterSlotModel in some order; the pairing is proven by the slot
     * model's own Description back-reference (the identity ex_slots() requires).
     * pair=0 is the order the SDK dump declares (description key first), pair=1
     * the reverse, -1 no accepted pairing. row + 0x10 of the model is _data_1 and
     * _data_1 + 0x20 is the EntityId reference whose boxed int sits at +0x18 --
     * the same hops as ex_slots() and rr_hiding.h. */
    int pair = -1;
    uint64_t pair_desc = 0, model_obj = 0;
    if (PL(key_q) && PL(slot)) {
        if (u_is(key_q, "BattleCharacterSlotDescription") &&
            u_is(slot, "BattleCharacterSlotModel") &&
            rr_u64(slot + EX_OFF_SLOT_DESC) == key_q) {
            pair = 0; pair_desc = key_q; model_obj = slot;
        } else if (u_is(slot, "BattleCharacterSlotDescription") &&
                   u_is(key_q, "BattleCharacterSlotModel") &&
                   rr_u64(key_q + EX_OFF_SLOT_DESC) == slot) {
            pair = 1; pair_desc = slot; model_obj = key_q;
        }
    }
    key_is_desc = u_is(key_q, "BattleCharacterSlotDescription");
    is_slot = u_is(slot, "BattleCharacterSlotModel");
    if (PL(model_obj)) {
        data = rr_u64(model_obj + EX_OFF_SLOT_DATA);
        desc = rr_u64(model_obj + EX_OFF_SLOT_DESC);
    }
    if (PL(data)) {
        is_data = u_is(data, "BattleCharacterSlotData");
        ref = rr_u64(data + EX_OFF_SLOT_ENTITY_ID);
    }
    if (PL(ref)) rr_read(ref + EX_OFF_PRIM_VALUE, &eid, sizeof(eid));

    char battle_cls[64], model_cls[64], key_cls[64], slot_cls[64], data_cls[64];
    ex_cls_name(battle, battle_cls, sizeof(battle_cls));
    ex_cls_name(model, model_cls, sizeof(model_cls));
    ex_cls_name(key_q, key_cls, sizeof(key_cls));
    ex_cls_name(slot, slot_cls, sizeof(slot_cls));
    ex_cls_name(data, data_cls, sizeof(data_cls));

    FILE* f = ex_status_open();
    if (!f) return;
    /* The fields the diagnostic asks for come first, in order; the raw addresses
     * and class names that follow say which hop produced them. _slots is keyed by
     * a BattleCharacterSlotDescription reference and valued by a
     * BattleCharacterSlotModel reference, so first_key_is_desc=1 and
     * desc_is_key=1 are the identities the reader requires; first_key is only the
     * low half of that key and is never compared with an entity id. */
    fprintf(f, "slots_debug: battle=0x%llX model=0x%llX is_slots=%d dict=0x%llX used=%d"
               " first_key=%d first_eid=%d first_is_slot=%d first_is_data=%d"
               " first_key_is_desc=%d pair=%d"
               " battle_cls=%s model_cls=%s key_cls=%s slot_cls=%s data_cls=%s"
               " row=%d hash=%d key_q=0x%llX slot=0x%llX data=0x%llX ref=0x%llX"
               " desc=0x%llX desc_is_key=%d entries=0x%llX cnt=%d stride=%d\n",
            (unsigned long long)battle, (unsigned long long)model, is_slots,
            (unsigned long long)dict, used, key, eid, is_slot, is_data, key_is_desc,
            pair,
            battle_cls, model_cls, key_cls, slot_cls, data_cls,
            row, hash, (unsigned long long)key_q, (unsigned long long)slot,
            (unsigned long long)data, (unsigned long long)ref,
            (unsigned long long)desc, desc != 0 && desc == pair_desc,
            (unsigned long long)entries, cnt, stride);
    fclose(f);
}

/* Slot entity ids, following the chain rr_hiding.h uses for the same table.
 *
 * _slots is Dictionary<BattleCharacterSlotDescription, BattleCharacterSlotModel>:
 * both halves of an entry are object references, so the key at +0x08 is the
 * slot's *description*, not its entity id. The id sits two hops further down, in
 * BattleCharacterSlotData.EntityId -- an IPrimitiveData<int> whose boxed int is
 * at +0x18. Comparing the dictionary key against the id, which this function
 * used to do, can never hold for a reference key: every row failed and a full
 * lobby published zero occupied seats. The pair is proven by identity instead
 * (the slot's own Description must be the key), exactly as rr_hiding.h does.
 *
 * `seats` (optional) reports the size of the slot table: `_slots` holds one row
 * per seat the battle created, and a seat without a player still has its row
 * (its EntityId references -1, which is why rr_hiding.h skips id < 0). That is
 * the only capacity the model exposes -- there is no _max/Capacity field in the
 * dump -- so the free-seat count is seats - occupied, and this dictionary read
 * is already paid for by the occupancy walk. */
static int ex_slots(uint64_t battle, int32_t* ids, int max, int* seats) {
    if (seats) *seats = 0;
    if (!u_is(battle, "BattleModel")) return -1;
    uint64_t model = rr_u64(battle + EX_OFF_BATTLE_SLOTS_MODEL);
    if (!u_is(model, "BattleCharacterSlotsModel")) return -1;
    EX_DICT dict;
    if (!ex_dict(rr_u64(model + EX_OFF_SLOTS_ARRAY), &dict)) return 0;
    if (seats) *seats = dict.count;
    int n = ex_dict_rows(&dict, EX_MAX_SLOTS);
    if (n <= 0) return 0;
    int count = 0;
    for (int i = 0; i < n && count < max; ++i) {
        const uint8_t* row = g_rows + (size_t)i * EX_OFF_DICT_STRIDE;
        int32_t hash = 0;
        memcpy(&hash, row + EX_OFF_DICT_HASH, 4);
        if (hash < 0) continue;                       /* freed entry          */
        /* The dump declares Dictionary<BattleCharacterSlotDescription,
         * BattleCharacterSlotModel>, so the key at +0x08 is the description and
         * the value at +0x10 the slot model. Do not trust the order alone: the
         * slot model carries its own Description back-reference, so the pairing
         * is accepted only when it holds. The reverse order is accepted the same
         * way, which keeps a runtime that stores the pair the other way round
         * readable instead of silently publishing zero. */
        uint64_t first = ex_row_ref(row), second = ex_row_value(row), slot = 0;
        if (u_is(first, "BattleCharacterSlotDescription") &&
            u_is(second, "BattleCharacterSlotModel") &&
            rr_u64(second + EX_OFF_SLOT_DESC) == first) slot = second;
        else if (u_is(second, "BattleCharacterSlotDescription") &&
                 u_is(first, "BattleCharacterSlotModel") &&
                 rr_u64(first + EX_OFF_SLOT_DESC) == second) slot = first;
        if (!slot) continue;
        uint64_t data = rr_u64(slot + EX_OFF_SLOT_DATA);
        if (!u_is(data, "BattleCharacterSlotData")) continue;
        uint64_t ref = rr_u64(data + EX_OFF_SLOT_ENTITY_ID);
        if (!u_is(ref, "BattleEntityIdData") && !u_is(ref, "IntData")) continue;
        int32_t id = -1;
        if (!rr_read(ref + EX_OFF_PRIM_VALUE, &id, sizeof(id))) continue;
        if (id < 0) continue;                 /* empty seat keeps its row      */
        int dup = 0;
        for (int j = 0; j < count; ++j)
            if (ids[j] == id) { dup = 1; break; }
        if (dup) continue;                    /* same seat twice = torn read   */
        if (count < max) ids[count] = id;
        ++count;
    }
    return count;
}

/* ---- roles: the chain the player ESP reads ------------------------------
 *
 * fill_entry() in rr_game.c resolves the role of every character the ESP
 * publishes through:
 *     BattleCharacterModel +0x40 Data
 *       -> BattleCharacterData +0x20 CommonData
 *         -> BattleCharacterCommonData +0x20 Role (ReferenceData`1)
 *           -> +0x18 description
 *             -> +0x10 Il2CppString
 * and the renderer colours a player blue when that string is exactly "cop"
 * (local_is_police in rr_game.c). ex_role_walk() *is* that chain, so the cop
 * counter and the ESP can never disagree about what a role is; the diagnostic
 * below reuses the same walk instead of keeping a second copy that could drift.
 */
typedef struct {
    uint64_t data, common, role_ref, ref_value, desc, str;
    char     role[20];
} EX_ROLE_TRACE;

static void ex_role_walk(uint64_t character, EX_ROLE_TRACE* t) {
    t->data = t->common = t->role_ref = t->ref_value = t->desc = t->str = 0;
    t->role[0] = 0;
    if (!PL(character)) return;
    t->data      = rr_u64(character + EX_OFF_CHAR_DATA);
    t->common    = PL(t->data)     ? rr_u64(t->data + EX_OFF_DATA_COMMON) : 0;
    t->role_ref  = PL(t->common)   ? rr_u64(t->common + EX_OFF_COMMON_ROLE) : 0;
    t->ref_value = PL(t->role_ref) ? rr_u64(t->role_ref + EX_OFF_REF_VALUE) : 0;
    t->desc      = t->ref_value;              /* ReferenceData`1 -> description */
    t->str       = PL(t->desc)     ? rr_u64(t->desc + EX_OFF_DESC_NAME) : 0;
    if (PL(t->str)) u_string(t->str, t->role, sizeof(t->role));
    t->role[sizeof(t->role) - 1] = 0;
}

typedef struct { int32_t id; int cop; } EX_ROLE;

static int ex_id_wanted(const int32_t* ids, int id_count, int32_t id) {
    for (int i = 0; i < id_count; ++i) if (ids[i] == id) return 1;
    return 0;
}

/* One entry per seated id; a second answer for the same id is ignored, so the
 * roster and the entity map can be merged without double counting. */
static int ex_role_add(EX_ROLE* out, int count, int cap, int32_t id, const char* role) {
    for (int i = 0; i < count; ++i) if (out[i].id == id) return count;
    if (count >= cap) return count;
    out[count].id = id;
    out[count].cop = strcmp(role, "cop") == 0;   /* rr_game.c police test */
    return count + 1;
}

/* The world roster: WorldEntitiesModel._battleCharacters, the
 * Dictionary<int, BattleCharacterModel> the ESP enumerates every frame. The id
 * is taken from the character's own Id field -- the value the published
 * snapshot carries and the seat table references -- not from the dictionary
 * key, so a roster keyed some other way still joins. A character whose role
 * string is empty is a template or an unspawned copy; fill_entry() drops those
 * too (it publishes nothing rather than a blank role). */
static int ex_roster_roles(uint64_t world, const int32_t* ids, int id_count,
                           EX_ROLE* out, int cap) {
    if (!u_is(world, "WorldEntitiesModel")) return 0;
    EX_DICT dict;
    if (!ex_dict(rr_u64(world + EX_OFF_WE_BATTLE_CHARS), &dict)) return 0;
    int n = ex_dict_rows(&dict, EX_MAX_WORLD_DICT);
    int count = 0;
    for (int i = 0; i < n && count < cap; ++i) {
        const uint8_t* row = g_rows + (size_t)i * EX_OFF_DICT_STRIDE;
        int32_t hash = 0;
        memcpy(&hash, row + EX_OFF_DICT_HASH, 4);
        if (hash < 0) continue;                       /* freed entry          */
        uint64_t character = ex_row_value(row);
        if (!u_is(character, "BattleCharacterModel")) continue;
        int32_t id = -1;
        if (!rr_read(character + EX_OFF_CHAR_ID, &id, sizeof(id))) continue;
        if (!ex_id_wanted(ids, id_count, id)) continue;
        EX_ROLE_TRACE t;
        ex_role_walk(character, &t);
        if (!t.role[0]) continue;
        count = ex_role_add(out, count, cap, id, t.role);
    }
    return count;
}

/* Fallback: BattleModel +0x90 BattleEntitiesModel -> +0x50 _models
 * (Dictionary<int, IEntityModel>) -> BattleCharacterEntityModel +0x28 server
 * character. Only used for seats the roster did not answer: that +0x28 hop is
 * from the 17/11 table and is the one link nothing else in the overlay reads,
 * while the roster above is the table the ESP proves every frame. `count` is
 * the number of roles already resolved, which the new answers are appended to.
 */
static int ex_entity_roles(uint64_t battle, const int32_t* ids, int id_count,
                           EX_ROLE* out, int count, int cap) {
    uint64_t entities = rr_u64(battle + EX_OFF_BATTLE_ENTITIES);
    if (!u_is(entities, "BattleEntitiesModel")) return count;
    EX_DICT dict;
    if (!ex_dict(rr_u64(entities + EX_OFF_BATTLE_MODELS), &dict)) return count;
    int n = ex_dict_rows(&dict, EX_MAX_ENTITY_DICT);
    if (n <= 0) return count;
    for (int i = 0; i < n && count < cap; ++i) {
        const uint8_t* row = g_rows + (size_t)i * EX_OFF_DICT_STRIDE;
        int32_t key = ex_row_id(row);
        if (key < 0) continue;
        if (!ex_id_wanted(ids, id_count, key)) continue;
        int known = 0;
        for (int j = 0; j < count; ++j) if (out[j].id == key) known = 1;
        if (known) continue;
        uint64_t entity = ex_row_value(row);
        if (!u_is(entity, "BattleCharacterEntityModel")) continue;
        uint64_t character = rr_u64(entity + EX_OFF_ENTITY_SERVER);
        if (!u_is(character, "BattleCharacterModel")) continue;
        int32_t character_id = -1;
        if (!rr_read(character + EX_OFF_CHAR_ID, &character_id, sizeof(character_id)) ||
            character_id != key) continue;
        EX_ROLE_TRACE t;
        ex_role_walk(character, &t);
        count = ex_role_add(out, count, cap, key, t.role);
    }
    return count;
}

/* Roles of the seated entity ids. The roster answers first because it is the
 * path the ESP already reads; the entity map then covers a seat whose character
 * the roster has not published yet (the frame a player spawns). A seat neither
 * path resolves is not a cop, which is the same answer as "not seated".
 */
static int ex_roles(uint64_t battle, uint64_t world, const int32_t* ids, int id_count,
                    EX_ROLE* out, int cap) {
    if (!ids || !out || id_count <= 0 || cap <= 0) return 0;
    int count = ex_roster_roles(world, ids, id_count, out, cap);
    if (count < id_count && PL(battle))
        count = ex_entity_roles(battle, ids, id_count, out, count, cap);
    return count;
}

static void ex_cop_scan(uint64_t battle, uint64_t world, int* cops, int* slots, int* seats) {
    *cops = 0;
    *slots = 0;
    *seats = 0;
    static int32_t ids[EX_MAX_SLOTS];
    int n = ex_slots(battle, ids, EX_MAX_SLOTS, seats);
    if (n <= 0) return;
    *slots = n > EX_MAX_SLOTS ? EX_MAX_SLOTS : n;
    static EX_ROLE roles[EX_MAX_SLOTS];
    int rn = ex_roles(battle, world, ids, *slots, roles, EX_MAX_SLOTS);
    for (int i = 0; i < *slots; ++i)
        for (int j = 0; j < rn; ++j)
            if (roles[j].id == ids[i]) {
                if (roles[j].cop) ++*cops;
                break;
            }
}

/* ---- diagnostic: the role chain, one status.log block per second --------
 *
 * The seat table can read perfectly and still count zero cops: the failure is
 * then the join from a seat's EntityId to a character whose replicated role
 * says "cop". With debug_slots on, the first three occupied seats are followed
 * through both joins -- the world roster the ESP reads and the entity-map chain
 * (BattleEntitiesModel._models -> +0x28 server character) -- and every hop is
 * printed with its class name plus the raw UTF-16 of the final string. The link
 * that is empty or class-checks out names itself here.
 */
static void ex_role_line(FILE* f, int32_t eid, const char* via, uint64_t character) {
    EX_ROLE_TRACE t;
    ex_role_walk(character, &t);
    char char_cls[64], desc_cls[64], str_cls[64];
    ex_cls_name(character, char_cls, sizeof(char_cls));
    ex_cls_name(t.desc, desc_cls, sizeof(desc_cls));
    ex_cls_name(t.str, str_cls, sizeof(str_cls));
    /* The UTF-16 the runtime holds, independently of the decoder: a role that
     * decodes to something unexpected still shows its code units. */
    int len = 0;
    uint16_t u16[16];
    memset(u16, 0, sizeof(u16));
    if (PL(t.str) && rr_read(t.str + U_STR_LEN, &len, sizeof(len)) &&
        len > 0 && len <= (int)(sizeof(u16) / sizeof(u16[0])))
        if (!rr_read(t.str + U_STR_CHARS, u16, (size_t)len * 2)) len = 0;
    char units[16 * 6 + 1];
    units[0] = 0;
    for (int i = 0; i < len; ++i) {
        char one[8];
        snprintf(one, sizeof(one), " %04X", u16[i]);
        strncat(units, one, sizeof(units) - strlen(units) - 1);
    }
    fprintf(f, "roles_debug: eid=%d via=%s char=0x%llX char_cls=%s data=0x%llX"
               " common=0x%llX role_ref=0x%llX ref_value=0x%llX desc_cls=%s"
               " str=0x%llX str_cls=%s u16len=%d u16=%s role=\"%s\" cop=%d\n",
            eid, via, (unsigned long long)character, char_cls,
            (unsigned long long)t.data, (unsigned long long)t.common,
            (unsigned long long)t.role_ref, (unsigned long long)t.ref_value, desc_cls,
            (unsigned long long)t.str, str_cls, len, units, t.role,
            strcmp(t.role, "cop") == 0);
}

static void ex_roles_debug(uint64_t battle, uint64_t world) {
    static DWORD last;                 /* worker thread only */
    DWORD now = GetTickCount();
    if (last && now - last < 1000) return;
    last = now;

    static int32_t ids[EX_MAX_SLOTS];
    int seats = 0;
    int occupied = ex_slots(battle, ids, EX_MAX_SLOTS, &seats);

    /* Every anchor both joins walk, printed raw: a missing class check can then
     * be told from an empty table. */
    uint64_t entities = PL(battle) ? rr_u64(battle + EX_OFF_BATTLE_ENTITIES) : 0;
    int is_entities = u_is(entities, "BattleEntitiesModel");
    uint64_t models = is_entities ? rr_u64(entities + EX_OFF_BATTLE_MODELS) : 0;
    int mcnt = 0, mstride = 0;
    uint64_t mentries = 0;
    if (PL(models)) u_dict(models, &mcnt, &mentries, &mstride);
    uint64_t roster_dict = PL(world) ? rr_u64(world + EX_OFF_WE_BATTLE_CHARS) : 0;
    int rcnt = 0, rstride = 0;
    uint64_t rentries = 0;
    if (PL(roster_dict)) u_dict(roster_dict, &rcnt, &rentries, &rstride);

    FILE* f = ex_status_open();
    if (!f) return;
    fprintf(f, "roles_debug: battle=0x%llX world=0x%llX is_world=%d"
               " roster_dict=0x%llX roster_cnt=%d roster_stride=%d"
               " entities=0x%llX is_entities=%d models=0x%llX models_cnt=%d"
               " models_stride=%d seats=%d occupied=%d\n",
            (unsigned long long)battle, (unsigned long long)world,
            u_is(world, "WorldEntitiesModel"), (unsigned long long)roster_dict,
            rcnt, rstride, (unsigned long long)entities, is_entities,
            (unsigned long long)models, mcnt, mstride, seats, occupied);

    enum { EX_DBG_SEATS = 3 };
    int want = occupied < EX_DBG_SEATS ? occupied : EX_DBG_SEATS;
    if (want <= 0) { fclose(f); return; }

    struct { int32_t id, server_id, roster_id; uint64_t entity, server, roster; }
        rows[EX_DBG_SEATS];
    memset(rows, 0, sizeof(rows));
    for (int k = 0; k < want; ++k) { rows[k].id = ids[k]; rows[k].server_id = -1;
                                     rows[k].roster_id = -1; }

    if (PL(mentries) && mcnt > 0) {
        int mn = mcnt < EX_MAX_ENTITY_DICT ? mcnt : EX_MAX_ENTITY_DICT;
        if (rr_read(mentries, g_rows, (size_t)mn * EX_OFF_DICT_STRIDE)) {
            for (int i = 0; i < mn; ++i) {
                const uint8_t* row = g_rows + (size_t)i * EX_OFF_DICT_STRIDE;
                int32_t key = ex_row_id(row);
                for (int k = 0; k < want; ++k) {
                    if (rows[k].id != key || rows[k].entity) continue;
                    rows[k].entity = ex_row_value(row);
                    rows[k].server = PL(rows[k].entity)
                        ? rr_u64(rows[k].entity + EX_OFF_ENTITY_SERVER) : 0;
                    if (!PL(rows[k].server) ||
                        !rr_read(rows[k].server + EX_OFF_CHAR_ID, &rows[k].server_id, 4))
                        rows[k].server_id = -1;
                }
            }
        }
    }
    if (PL(rentries) && rcnt > 0) {
        int rn = rcnt < EX_MAX_WORLD_DICT ? rcnt : EX_MAX_WORLD_DICT;
        if (rr_read(rentries, g_rows, (size_t)rn * EX_OFF_DICT_STRIDE)) {
            for (int i = 0; i < rn; ++i) {
                const uint8_t* row = g_rows + (size_t)i * EX_OFF_DICT_STRIDE;
                int32_t hash = 0;
                memcpy(&hash, row + EX_OFF_DICT_HASH, 4);
                if (hash < 0) continue;
                uint64_t character = ex_row_value(row);
                int32_t cid = -1;
                if (!PL(character) || !rr_read(character + EX_OFF_CHAR_ID, &cid, sizeof(cid)))
                    continue;
                for (int k = 0; k < want; ++k) {
                    if (rows[k].id != cid || rows[k].roster) continue;
                    rows[k].roster = character;
                    rows[k].roster_id = cid;
                }
            }
        }
    }

    for (int k = 0; k < want; ++k) {
        char entity_cls[64], server_cls[64], roster_cls[64];
        ex_cls_name(rows[k].entity, entity_cls, sizeof(entity_cls));
        ex_cls_name(rows[k].server, server_cls, sizeof(server_cls));
        ex_cls_name(rows[k].roster, roster_cls, sizeof(roster_cls));
        fprintf(f, "roles_debug: eid=%d entity=0x%llX is_entity=%d entity_cls=%s"
                   " server=0x%llX is_server=%d server_cls=%s server_id=%d"
                   " roster=0x%llX is_roster=%d roster_cls=%s roster_id=%d\n",
                rows[k].id, (unsigned long long)rows[k].entity,
                u_is(rows[k].entity, "BattleCharacterEntityModel"), entity_cls,
                (unsigned long long)rows[k].server,
                u_is(rows[k].server, "BattleCharacterModel"), server_cls,
                rows[k].server_id, (unsigned long long)rows[k].roster,
                u_is(rows[k].roster, "BattleCharacterModel"), roster_cls,
                rows[k].roster_id);
        ex_role_line(f, rows[k].id, "server", rows[k].server);
        ex_role_line(f, rows[k].id, "roster", rows[k].roster);
    }
    fclose(f);
}

/* ---- --probe: one-shot seat-table dump ---------------------------------
 *
 * The same read-only walk ex_slots() does, printed instead of published: the
 * raw 24-byte entries of the first rows, the class name at each candidate
 * offset, the value offsets u_dict_value_offset() can find for the three seat
 * classes, and the counts the production chain resolves. A runtime that lays
 * `_slots` out differently names itself here instead of publishing a count of
 * zero, which is what a seat table with no cops looks like.
 */
int extras_slots_probe(uint64_t battle_model, uint64_t world) {
    FILE* f = ex_status_open();
    if (!f) return -1;
    int is_model = u_is(battle_model, "BattleModel");
    uint64_t model = is_model ? rr_u64(battle_model + EX_OFF_BATTLE_SLOTS_MODEL) : 0;
    int is_slots = u_is(model, "BattleCharacterSlotsModel");
    uint64_t dict = is_slots ? rr_u64(model + EX_OFF_SLOTS_ARRAY) : 0;
    int cnt = 0, stride = 0;
    uint64_t entries = 0;
    if (PL(dict)) u_dict(dict, &cnt, &entries, &stride);
    fprintf(f, "slots_layout: battle_model=0x%llX is_model=%d model=0x%llX is_slots=%d"
               " dict=0x%llX cnt=%d stride=%d entries=0x%llX\n",
            (unsigned long long)battle_model, is_model,
            (unsigned long long)model, is_slots, (unsigned long long)dict,
            cnt, stride, (unsigned long long)entries);
    if (!PL(dict) || !PL(entries) || cnt <= 0) { fclose(f); return -1; }

    fprintf(f, "slots_layout: value_offset SlotModel=0x%X SlotDescription=0x%X SlotData=0x%X\n",
            u_dict_value_offset(dict, "BattleCharacterSlotModel", 16),
            u_dict_value_offset(dict, "BattleCharacterSlotDescription", 16),
            u_dict_value_offset(dict, "BattleCharacterSlotData", 16));

    enum { EX_PROBE_ROWS = 3 };
    uint8_t raw[EX_PROBE_ROWS * EX_OFF_DICT_STRIDE];
    int rows = cnt < EX_PROBE_ROWS ? cnt : EX_PROBE_ROWS;
    if (rr_read(entries, raw, (size_t)rows * EX_OFF_DICT_STRIDE)) {
        for (int i = 0; i < rows; ++i) {
            const uint8_t* r = raw + (size_t)i * EX_OFF_DICT_STRIDE;
            int32_t hash = 0, next = 0;
            uint32_t d[6];
            memcpy(&hash, r + EX_OFF_DICT_HASH, 4);
            memcpy(&next, r + 4, 4);
            for (int k = 0; k < 6; ++k) memcpy(&d[k], r + (size_t)k * 4, 4);
            uint64_t key = ex_row_ref(r), slot = ex_row_value(r);
            /* Prove the pairing instead of trusting the half order: the slot
             * model carries its own Description back-reference. */
            int pair = -1;
            if (PL(key) && PL(slot)) {
                if (u_is(key, "BattleCharacterSlotDescription") &&
                    u_is(slot, "BattleCharacterSlotModel") &&
                    rr_u64(slot + EX_OFF_SLOT_DESC) == key) pair = 0;
                else if (u_is(slot, "BattleCharacterSlotDescription") &&
                         u_is(key, "BattleCharacterSlotModel") &&
                         rr_u64(key + EX_OFF_SLOT_DESC) == slot) pair = 1;
            }
            uint64_t model_obj = pair == 1 ? key : slot;
            uint64_t data = PL(model_obj) ? rr_u64(model_obj + EX_OFF_SLOT_DATA) : 0;
            uint64_t ref = PL(data) ? rr_u64(data + EX_OFF_SLOT_ENTITY_ID) : 0;
            int eid = -1;
            if (PL(ref)) rr_read(ref + EX_OFF_PRIM_VALUE, &eid, sizeof(eid));
            char kc[64], vc[64], dc[64], rc[64];
            ex_cls_name(key, kc, sizeof(kc));
            ex_cls_name(slot, vc, sizeof(vc));
            ex_cls_name(data, dc, sizeof(dc));
            ex_cls_name(ref, rc, sizeof(rc));
            fprintf(f, "slots_layout: row=%d hash=%d next=%d dwords=%08X %08X %08X %08X %08X %08X"
                       " key=0x%llX (%s) slot=0x%llX (%s) pair=%d"
                       " data=0x%llX (%s) ref=0x%llX (%s) eid=%d\n",
                    i, hash, next, d[0], d[1], d[2], d[3], d[4], d[5],
                    (unsigned long long)key, kc, (unsigned long long)slot, vc, pair,
                    (unsigned long long)data, dc, (unsigned long long)ref, rc, eid);
        }
    }

    int cops = 0, slots = 0, seats = 0;
    ex_cop_scan(battle_model, world, &cops, &slots, &seats);
    fprintf(f, "slots_layout: cops=%d occupied=%d seats=%d free=%d\n",
            cops, slots, seats, seats > slots ? seats - slots : 0);
    fclose(f);
    /* Also emit the production lines the overlay would append (their 1 s
     * throttle is per process, so a one-shot dump writes exactly one block). */
    ex_slots_debug(battle_model);
    ex_roles_debug(battle_model, world);
    return slots;
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
     * the switch off the anchor is 0 and no pass runs.
     *
     * The anchor instant loot compares against is the *entities* model
     * (BattleEntitiesUpdater + RR_OFF_VIEWS_BATTLE), but callers hand this
     * function the BattleModel, because that is what ex_slots() needs (the seat
     * table hangs off its +0x4D0). Arming the BattleModel put two different
     * types on the two sides of that comparison, so the tick never passed its
     * back-pointer test, never read screen + 0x540, and instant loot stopped
     * writing while every offset was still correct. Resolve the entities model
     * here and leave the tick's comparison strict; both shapes are accepted so
     * the teardown calls and any caller that already has the entities model
     * keep working. */
    uint64_t anchor = 0;
    if (PL(battle)) {
        if (u_is(battle, "BattleEntitiesModel")) {
            anchor = battle;
        } else {
            uint64_t entities = rr_u64(battle + RR_OFF_BATTLE_ENTITIES);
            if (u_is(entities, "BattleEntitiesModel")) anchor = entities;
        }
    }
    ex_inst_arm(cfg->instant_loot ? anchor : 0);
    DWORD now = GetTickCount();
    if (!force && g_have_stamp && now - g_stamp < 400) return;

    /* Diagnostic hooks: run before the early return below, so a count of zero can
     * be read as "the chain is broken" or "no battle" instead of silently. The
     * role dump answers the next question -- the seat table read fine, so is the
     * failing link the join from a seat id to a character's role string? */
    if (cfg->debug_slots) {
        ex_slots_debug(battle);
        ex_roles_debug(battle, world);
    }

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
        cfg->telem_slot_seats = 0;
        g_stamp = now; g_have_stamp = 1;
        return;
    }

    int cops = 0, slots = 0, seats = 0;
    static EXTRA_WORLD_ITEM staged[EXTRA_MAX_WORLD_ITEMS];
    int items = 0;
    int have_world = PL(world) && u_is(world, "WorldEntitiesModel");
    if (want_cops && PL(battle)) ex_cop_scan(battle, world, &cops, &slots, &seats);
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

    /* The menu reads these through the existing telem: channel, and the overlay
     * draws the cop HUD from the same three fields. */
    cfg->telem_cop_count = cops;
    cfg->telem_slot_total = slots;
    cfg->telem_slot_seats = seats;

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

/* The same read without the range gate. Only the debug line uses it: a duration
 * that reads outside its bound is exactly the value the diagnostic has to show,
 * and leaving the caller's sentinel in place says the field did not read at all. */
static int ex_float_any(uint64_t addr, float* out) {
    float v = 0.0f;
    if (!PL(addr) || !rr_read(addr, &v, sizeof(v)) || !isfinite(v)) return 0;
    *out = v;
    return 1;
}

/* --- Offset sweeps for the two links a build can move -------------------
 * Reached only from ex_insta_debug(), i.e. only with debug_insta on and only
 * while the chain is broken: a null inst cannot say whether the updater ->
 * screen slot moved, the screen -> inst slot moved, or the back pointer did.
 * Both parents are then walked slot by slot and every candidate is named by
 * class, which turns "the offset moved" into one live run instead of one per
 * guess. Nothing here writes and nothing here is on the instant loot path. */

/* The filter is the class *name*, not u_is(): if the build renamed the view,
 * an exact match on the old name would reject the very object the sweep exists
 * to find. "Screen" is in the list so BattleScreenContent itself shows up in
 * the updater sweep, which is how a second content slot is spotted. */
static const char* const g_ex_sweep_names[] = { "Inventory", "Reveal", "View", "Screen" };
#define EX_SWEEP_NAMES ((int)(sizeof(g_ex_sweep_names) / sizeof(g_ex_sweep_names[0])))

/* A parent with more matching sub-models than this in one range would flood the
 * log; the first EX_SWEEP_MAX_LINES candidates of a sweep are printed, then a
 * line says the range was cut. */
enum { EX_SWEEP_MAX_LINES = 24 };

static DWORD g_insta_sweep_last = 0;   /* worker thread only, like the throttle */

/* Class name of obj and whether it matched one of the sweep names. ex_cls_name
 * leaves "?" when the name does not read, so garbage slots never match. */
static int ex_sweep_named(uint64_t obj, char* name, size_t cap) {
    ex_cls_name(obj, name, cap);
    for (int i = 0; i < EX_SWEEP_NAMES; ++i)
        if (strstr(name, g_ex_sweep_names[i])) return 1;
    return 0;
}

/* List<T>._size of a candidate list, -1 when it could not be reached. */
static int ex_sweep_count(uint64_t list) {
    int32_t count = 0;
    if (!PL(list) || !rr_read(list + EX_OFF_LIST_COUNT, &count, sizeof(count))) return -1;
    return count;
}

/* BattleEntitiesUpdater: every slot in EX_SWEEP_UPDATER_FROM..TO that names a
 * screen, and what the known screen -> inst offset yields on it. A slot other
 * than EX_OFF_UPDATER_TO_SCREEN whose probe is non-null is the content link
 * this build uses; is_screen=1 lists every BattleScreenContent the updater
 * carries. back is the back pointer at RR_OFF_VIEWS_BATTLE, printed so a
 * changed back pointer (not a moved offset) is visible as such. Returns the
 * number of candidates printed. */
static int ex_sweep_updater(FILE* f, uint64_t updater, uint64_t battle, uint64_t back) {
    int shown = 0;
    for (unsigned off = EX_SWEEP_UPDATER_FROM; off <= EX_SWEEP_UPDATER_TO; off += EX_SWEEP_STEP) {
        uint64_t obj = rr_u64(updater + off);
        char cls[96];
        if (!PL(obj) || !ex_sweep_named(obj, cls, sizeof(cls))) continue;
        uint64_t inst = rr_u64(obj + EX_OFF_SCREEN_TO_INST);
        char icls[96];
        ex_cls_name(inst, icls, sizeof(icls));
        fprintf(f, "insta_sweep_updater: off=0x%03X obj=0x%llX cls=\"%s\" is_screen=%d"
                   " probe=0x%X inst=0x%llX inst_cls=\"%s\" battle=0x%llX back=0x%llX\n",
                off, (unsigned long long)obj, cls, u_is(obj, "BattleScreenContent"),
                EX_OFF_SCREEN_TO_INST, (unsigned long long)inst, icls,
                (unsigned long long)battle, (unsigned long long)back);
        if (++shown >= EX_SWEEP_MAX_LINES) {
            fprintf(f, "insta_sweep_updater: range cut at 0x%03X (%d lines)\n",
                    off, EX_SWEEP_MAX_LINES);
            break;
        }
    }
    return shown;
}

/* BattleScreenContent: every slot in EX_SWEEP_SCREEN_FROM..TO that either names
 * a view or fits the reveal view's shape, with the cell timer and the list
 * count read at the offsets the walk already uses. A candidate is the reveal
 * view when cell sits in 0..EX_INST_TIME_CELL_MAX and count is the number of
 * items in the open container; the shape test survives a rename that drops
 * every word in g_ex_sweep_names. Returns the number of candidates printed. */
static int ex_sweep_screen(FILE* f, uint64_t screen) {
    int shown = 0;
    for (unsigned off = EX_SWEEP_SCREEN_FROM; off <= EX_SWEEP_SCREEN_TO; off += EX_SWEEP_STEP) {
        uint64_t obj = rr_u64(screen + off);
        if (!PL(obj)) continue;
        char cls[96];
        int by_name = ex_sweep_named(obj, cls, sizeof(cls));
        /* cell keeps -1 for "did not read" distinct from a zero, the same way
         * the insta_debug line does. */
        float cell = -1.0f;
        int cell_ok = ex_float_in(obj + EX_OFF_INST_TIME_CELL, EX_INST_TIME_CELL_MAX, &cell);
        if (!cell_ok) ex_float_any(obj + EX_OFF_INST_TIME_CELL, &cell);
        uint64_t list = rr_u64(obj + EX_OFF_INST_LIST);
        int count = ex_sweep_count(list);
        int by_shape = cell_ok && PL(list) && count > 0 && count <= EX_INST_MAX_ITEMS;
        if (!by_name && !by_shape) continue;
        fprintf(f, "insta_sweep_screen: off=0x%03X obj=0x%llX cls=\"%s\" match=%s"
                   " cell=%.4f list=0x%llX count=%d\n",
                off, (unsigned long long)obj, cls,
                by_name && by_shape ? "name+shape" : by_name ? "name" : "shape",
                cell, (unsigned long long)list, count);
        if (++shown >= EX_SWEEP_MAX_LINES) {
            fprintf(f, "insta_sweep_screen: range cut at %d lines\n", EX_SWEEP_MAX_LINES);
            break;
        }
    }
    return shown;
}

/* One insta_debug line per second while debug_insta is on, appended to
 * status.log beside the overlay like the slots_debug lines. A 0 pointer, a count
 * of -1 or a time_cell of -1 means that hop was never reached (or did not read),
 * never "zero"; writes counts the rr_write() calls that succeeded, so a live
 * timer together with writes=0 is a failing write path. The stamp is cleared
 * whenever a pass sees the switch off (extras_insta_tick), so the first line
 * after enabling it is never swallowed by the throttle. Instant loot loop
 * thread only in production; the self-test reaches it from the main thread.
 *
 * The tail of the line is what localises a broken pass: battle/back are the two
 * values the back-pointer test compares, gate names the first test that failed
 * (anchor, class, link, back, inst - or ok), and probe is what screen + its
 * slot reads even when that test failed, with probe_cls naming the object.
 *
 * While inst is null the two offset sweeps run too, on a slower period of their
 * own (EX_SWEEP_MIN_MS): the per-slot insta_sweep_updater / insta_sweep_screen
 * lines name every candidate the parent carries, and the closing insta_sweep:
 * line reports the candidate count even when it is zero, which is a result in
 * itself (nothing in range matches, so the type was renamed or the object hangs
 * off a different parent). Both are pure reads. */
static DWORD g_insta_dbg_last = 0;

static void ex_insta_debug(uint64_t battle, uint64_t updater, int is_updater,
                           uint64_t screen, uint64_t back, uint64_t inst,
                           float time_cell, uint64_t list, int count, int writes,
                           const char* gate, uint64_t probe) {
    DWORD now = GetTickCount();
    if (g_insta_dbg_last && now - g_insta_dbg_last < 1000) return;
    g_insta_dbg_last = now;

    /* Naming the probe object is what separates "the link holds the reveal view
     * and only the anchor is wrong" from "the link holds something else". */
    char pcls[96];
    pcls[0] = 0;
    if (PL(probe)) ex_cls_name(probe, pcls, sizeof(pcls));

    FILE* f = ex_status_open();
    if (!f) return;
    fprintf(f, "insta_debug: updater=0x%llX is_updater=%d screen=0x%llX inst=0x%llX"
               " time_cell=%.4f list=0x%llX count=%d writes=%d"
               " battle=0x%llX back=0x%llX gate=%s probe=0x%llX probe_cls=\"%s\"\n",
            (unsigned long long)updater, is_updater, (unsigned long long)screen,
            (unsigned long long)inst, time_cell, (unsigned long long)list,
            count, writes, (unsigned long long)battle, (unsigned long long)back,
            gate ? gate : "?", (unsigned long long)probe, pcls);
    /* A null inst is the one failure the line above cannot localise, so sweep
     * both parents while it is null. The sweep keeps its own slower period and
     * only re-arms when a pass sees a live inst, which keeps the log bounded
     * for a container that stays open. */
    if (!PL(inst)) {
        if (!g_insta_sweep_last || now - g_insta_sweep_last >= EX_SWEEP_MIN_MS) {
            g_insta_sweep_last = now;
            int nu = is_updater ? ex_sweep_updater(f, updater, battle, back) : 0;
            int ns = PL(screen) ? ex_sweep_screen(f, screen) : 0;
            fprintf(f, "insta_sweep: updater_hits=%d screen_hits=%d step=%u"
                       " ranges=0x%03X..0x%03X,0x%03X..0x%03X"
                       " updater=0x%llX battle=0x%llX back=0x%llX\n",
                    nu, ns, EX_SWEEP_STEP, EX_SWEEP_UPDATER_FROM, EX_SWEEP_UPDATER_TO,
                    EX_SWEEP_SCREEN_FROM, EX_SWEEP_SCREEN_TO,
                    (unsigned long long)updater, (unsigned long long)battle,
                    (unsigned long long)back);
        }
    } else {
        g_insta_sweep_last = 0;   /* a working chain re-arms the next sweep */
    }
    fclose(f);
}

/* The write pass over a validated InventoryRevealViewDescriptions. Reports what
 * it saw for the debug line: *time_cell is the cell-border duration read before
 * it was zeroed (-1 when it did not read, or the offending value when it failed
 * the bound), *plist the List<Pair> and *pcount its size (-1 when the list could
 * not be reached, 0 for an empty one). Returns the number of successful writes. */
static int ex_insta_walk(uint64_t inst, float* time_cell, uint64_t* plist, int* pcount) {
    int wrote = 0;
    *plist = 0;
    *pcount = -1;
    if (!ex_float_in(inst + EX_OFF_INST_TIME_CELL, EX_INST_TIME_CELL_MAX, time_cell)) {
        ex_float_any(inst + EX_OFF_INST_TIME_CELL, time_cell);   /* the tripwire value */
        return 0;   /* not the object we expect: touch nothing */
    }
    if (*time_cell > 0.0f && ex_float_write(inst + EX_OFF_INST_TIME_CELL, 0.0f)) ++wrote;

    uint64_t list = rr_u64(inst + EX_OFF_INST_LIST);
    if (!PL(list)) return wrote;
    *plist = list;
    int32_t count = 0;
    if (!rr_read(list + EX_OFF_LIST_COUNT, &count, sizeof(count))) return wrote;
    *pcount = count;
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

int extras_insta_tick(uint64_t battle, uint64_t updater) {
    /* One read of the switch decides whether this pass may write at all. */
    BR_Config* cfg = br_get();
    if (!cfg->instant_loot) return 0;

    /* State for the debug line, filled in as the chain is walked. Every value
     * starts at "not reached" so a line that stops early still says where. */
    uint64_t screen = 0, inst = 0, list = 0, back = 0, probe = 0;
    int is_updater = 0, count = -1, wrote = 0;
    float time_cell = -1.0f;
    /* Which test ended the chain, in walk order. Without it a line cannot tell
     * "the anchor is another type" (back) from "the link is null" (inst), and
     * those two need opposite fixes: the first is an arming bug, the second a
     * moved offset. */
    const char* gate = "anchor";   /* battle or updater not usable            */

    if (PL(battle) && PL(updater)) {
        gate = "class";
        is_updater = u_is(updater, "BattleEntitiesUpdater") ? 1 : 0;
        /* screen and the back pointer are read before the back-pointer test so
         * the line can tell a missing content link from an updater that belongs
         * to another battle, and so the sweep can report both. */
        if (is_updater) {
            screen = rr_u64(updater + EX_OFF_UPDATER_TO_SCREEN);
            back = rr_u64(updater + RR_OFF_VIEWS_BATTLE);
            /* The link is read even when the back-pointer test fails: probe is
             * what screen + its slot holds right now, so a broken pass can say
             * "the object is there, the anchor is wrong" instead of reporting
             * two nulls. Read only - the walk and every write stay behind the
             * whole chain. */
            if (PL(screen)) probe = rr_u64(screen + EX_OFF_SCREEN_TO_INST);
            if (!PL(screen)) {
                gate = "link";     /* updater carries no BattleScreenContent   */
            } else if (back != battle) {
                gate = "back";     /* anchor is not the object the updater
                                    * points back at; no link is trusted      */
            } else if (!PL(probe)) {
                gate = "inst";     /* EX_OFF_SCREEN_TO_INST moved or was nulled */
            } else {
                gate = "ok";
                inst = probe;
                wrote = ex_insta_walk(inst, &time_cell, &list, &count);
            }
        }
    }
    if (cfg->debug_insta)
        ex_insta_debug(battle, updater, is_updater, screen, back, inst,
                       time_cell, list, count, wrote, gate, probe);
    else {
        g_insta_dbg_last = 0;     /* the first pass after enabling always writes */
        g_insta_sweep_last = 0;   /* ...and the first broken pass sweeps at once */
    }
    return wrote;
}

/* One insta_loop line per second while debug_insta is on: the two halves the
 * tick joins, printed before the tick so the pair can be read side by side.
 * battle is the anchor the extras worker published (what the tick used to be
 * given), ub the battle game_scene_debug() reports for this pass (what the
 * tick is given now) and back what the updater points at, RR_OFF_VIEWS_BATTLE.
 * back == ub is the condition the write gate needs; back == battle is the one
 * it had when the anchor was a BattleModel and every pass died there. Its own
 * 1/s stamp, independent of the insta_debug throttle. */
static DWORD g_insta_loop_last = 0;

static void ex_insta_loop_debug(uint64_t battle, uint64_t ub, uint64_t updater) {
    DWORD now = GetTickCount();
    if (g_insta_loop_last && now - g_insta_loop_last < 1000) return;

    uint64_t back = 0;
    char ucls[96], bcls[96];
    ucls[0] = bcls[0] = 0;
    if (PL(updater)) ex_cls_name(updater, ucls, sizeof(ucls));
    if (PL(updater)) back = rr_u64(updater + RR_OFF_VIEWS_BATTLE);
    if (PL(back)) ex_cls_name(back, bcls, sizeof(bcls));

    FILE* f = ex_status_open();
    if (!f) return;
    g_insta_loop_last = now;
    fprintf(f, "insta_loop: battle=0x%llX ub=0x%llX same=%d updater=0x%llX"
               " updater_cls=\"%s\" back=0x%llX back_is_ub=%d back_is_battle=%d"
               " back_cls=\"%s\"\n",
            (unsigned long long)battle, (unsigned long long)ub,
            (battle && battle == ub) ? 1 : 0, (unsigned long long)updater, ucls,
            (unsigned long long)back, (back && back == ub) ? 1 : 0,
            (back && back == battle) ? 1 : 0, bcls);
    fclose(f);
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
            int debug = br_get()->debug_insta;
            if (debug) ex_insta_loop_debug((uint64_t)battle, ub, updater);
            else g_insta_loop_last = 0;   /* first line after enabling shows */
            /* The tick's own chain is the gate: it re-reads the class, the
             * content link, the back pointer and the timers, and writes only
             * once all of them pass. Handing it the battle this pass resolved
             * instead of the anchor published up to 400 ms ago is what keeps a
             * stale anchor from hiding the whole path: the old gate
             * (updater && ub == battle) then skipped the tick entirely, so an
             * anchor of the wrong type looked exactly like a dead offset. The
             * armed anchor is still what arms disarms the loop, and it is used
             * as the fallback pointer when no validated battle is resolved. */
            if (updater && (ub || debug))
                extras_insta_tick(ub ? ub : (uint64_t)battle, updater);
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
    LONG64 prev = g_inst_battle;
    InterlockedExchange64(&g_inst_battle, (LONG64)battle);
    if (battle && !g_inst_thread && g_inst_clean) {
        InterlockedExchange(&g_inst_stop, 0);
        g_inst_thread = CreateThread(NULL, 0, ex_inst_loop, NULL, 0, NULL);
        InterlockedExchange(&g_inst_clean, g_inst_thread ? 0 : 1);
    }
    ReleaseSRWLockExclusive(&g_inst_lock);
    /* One line per change, outside the lock: the anchor every pass compares its
     * back pointer against, named, so an anchor of the wrong type shows up as
     * *this* line instead of as every pass stopping at gate=back. */
    if ((uint64_t)prev != battle && br_get()->debug_insta) {
        char cls[96];
        cls[0] = 0;
        if (PL(battle)) ex_cls_name(battle, cls, sizeof(cls));
        FILE* f = ex_status_open();
        if (f) {
            fprintf(f, "insta_arm: anchor=0x%llX prev=0x%llX cls=\"%s\" armed=%d\n",
                    (unsigned long long)battle, (unsigned long long)prev, cls,
                    battle ? 1 : 0);
            fclose(f);
        }
    }
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






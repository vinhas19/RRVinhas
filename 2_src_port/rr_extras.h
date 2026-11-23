/* rr_extras.h - Extras: the roster's cop counter, the world item collections
 * (containers, loot, vehicle loot, doors, hiding objects) and instant loot.
 *
 * The two read features only use rr_read() through the driver. Their reads run
 * on the discovery worker through extras_refresh(); the result is published
 * under a lock and the renderer and the menu only ever copy that publication,
 * so no frame waits on the driver.
 *
 * Instant loot is the only writer in this module. Its loop runs at 250 ms while
 * the switch is on, and its three rr_write() calls are reachable only after the
 * pointer, class and value-range checks in extras_insta_tick().
 *
 * Field offsets are from the 17/11/2026 dump (GameAssembly 0x8FF3000,
 * timestamp 0x6AFC1686). Where robrun_sdk_offsets.h (dump-20261112) already
 * carries the same field, the generated constant is reused and the EX_ name is
 * an alias: the two can never drift apart, and a regenerated header moves both.
 */
#ifndef RR_EXTRAS_H
#define RR_EXTRAS_H

#include <stdint.h>
#include "robrun_offsets.h"
#include "robrun_sdk_offsets.h"
#include "rr_unity.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- BattleCharacterSlots ------------------------------------------------
 * src: 17/11/2026 dump, verify vs current build. BattleModel.BattleCharacter
 * SlotsModel and the slot chain are the same fields rr_hiding.h reads. */
#define EX_OFF_BATTLE_SLOTS_MODEL   RR_OFF_BATTLE_PLAYER_SLOTS  /* 0x4D0 BattleModel -> BattleCharacterSlotsModel */
#define EX_OFF_SLOTS_ARRAY          RR_OFF_SLOTS_DICTIONARY     /* 0x010 _slots */
#define EX_OFF_SLOT_DATA            RR_OFF_SLOT_DATA            /* 0x010 BattleCharacterSlotModel._data_1 */
#define EX_OFF_SLOT_DESC            RR_OFF_SLOT_DESC            /* 0x018 BattleCharacterSlotModel.Description */
#define EX_OFF_SLOT_ENTITY_ID       RR_OFF_SLOT_ENTITY_ID       /* 0x020 BattleCharacterSlotData.EntityId */
#define EX_OFF_SLOT_NICKNAME        RR_OFF_SLOT_NICKNAME        /* 0x030 BattleCharacterSlotData.Nickname */

/* --- Array and dictionary entry layout (rr_unity.h map) ------------------ */
#define EX_OFF_ARRAY_ITEMS          U_LIST_ITEMS    /* 0x010 backing array  */
#define EX_OFF_ARRAY_COUNT          U_ARR_LEN       /* 0x018 element count  */
#define EX_OFF_ARRAY_STRIDE         0x18u           /* hash, key, value     */
#define EX_OFF_DICT_ENTRIES         U_DICT_ENTRIES  /* 0x018 entry array    */
#define EX_OFF_DICT_USED            U_DICT_COUNT    /* 0x020 used slots     */
#define EX_OFF_DICT_STRIDE          0x18u
#define EX_OFF_DICT_HASH            0x00u           /* < 0 marks a freed slot */
#define EX_OFF_DICT_KEY             0x08u           /* int32 key of Dictionary<int, T> */
#define EX_OFF_DICT_VALUE           0x10u
/* The seat table's key is a reference, not an id: _slots is
 * Dictionary<BattleCharacterSlotDescription, BattleCharacterSlotModel>
 * (offsets/robrun_sdk_offsets.json), so the key occupies the same 0x08 offset
 * as an int32 key but is 8 bytes wide and must be class-checked. rr_hiding.h
 * reads the same table the same way. */
#define EX_OFF_SLOT_KEY             EX_OFF_DICT_KEY

/* --- WorldEntitiesModel collections -------------------------------------
 * src: 17/11/2026 dump; identical to the generated dump-20261112 values used
 * by cache_world_markers() in rr_game.c. */
/* The roster the player ESP enumerates: Dictionary<int, BattleCharacterModel>.
 * ex_roles() reads a seat's role from here, because this is the table the ESP
 * already resolves and colours players by. */
#define EX_OFF_WE_BATTLE_CHARS      RR_OFF_WEM_BATTLE_CHARS   /* 0x018 */
#define EX_OFF_WE_CONTAINERS        RR_OFF_WEM_CONTAINERS     /* 0x020 */
#define EX_OFF_WE_LOOTS             RR_OFF_WEM_DROPPED_LOOT   /* 0x038 */
#define EX_OFF_WE_VEHICLE_LOOTS     RR_OFF_WEM_VEHICLE_LOOT   /* 0x040 */
#define EX_OFF_WE_DOORS             RR_OFF_WEM_DOORS          /* 0x048 */
#define EX_OFF_WE_HIDE_OBJECTS      RR_OFF_WEM_HIDE_OBJECTS   /* 0x1B0 */

/* --- World item positions ------------------------------------------------
 * src: 17/11/2026 dump. Containers, loot and vehicle loot hold a position
 * model; doors hold the floats inline; hiding objects hang theirs off the
 * description. */
#define EX_OFF_CONTAINER_POS        RR_OFF_CONTAINER_POS      /* 0x028 ContainerModel.PositionModel */
#define EX_OFF_LOOT_POS             RR_OFF_DROPPED_LOOT_POS   /* 0x028 LootModel.InteractPositionModel */
#define EX_OFF_VEHICLE_LOOT_POS     RR_OFF_VEHICLE_LOOT_POS   /* 0x028 VehicleLootModel.InteractPositionModel */
#define EX_OFF_DOOR_STATE           RR_OFF_DOOR_STATE         /* 0x030 DoorModel.State */
#define EX_OFF_DOOR_POS             RR_OFF_DOOR_POS           /* 0x038 DoorModel.Position */
#define EX_OFF_POS_X                RR_OFF_POS_X              /* 0x010 PositionModel.X */
#define EX_OFF_POS_Y                RR_OFF_POS_Y              /* 0x014 */
#define EX_OFF_POS_Z                RR_OFF_POS_Z              /* 0x018 */

/* --- HideObjectModel ----------------------------------------------------
 * src: 17/11/2026 dump; matches RR_OFF_HIDE_* from dump-20261112. */
#define EX_OFF_HIDE_DESC            RR_OFF_HIDE_DESC          /* 0x018 Description */
#define EX_OFF_HIDE_ENTITY_ID       RR_OFF_HIDE_OCCUPANT      /* 0x010 _entityId */
#define EX_OFF_HIDE_DESC_POS        RR_OFF_HIDE_DESC_POS      /* 0x030 LocationHideObjectDescription.Position */
#define EX_OFF_HIDE_DESC_ID         RR_OFF_HIDE_DESC_ID       /* 0x020 LocationHideObjectDescription.EntityId */

/* --- Battle scene links used to resolve a slot's character --------------
 * src: 17/11/2026 dump; the RR_OFF_* values are from dump-20261112. */
#define EX_OFF_BATTLE_ENTITIES      RR_OFF_BATTLE_ENTITIES    /* 0x090 BattleModel -> BattleEntitiesModel */
#define EX_OFF_BATTLE_MODELS        RR_OFF_BATTLE_MODELS      /* 0x050 BattleEntitiesModel._models */
#define EX_OFF_ENTITY_SERVER        0x028u  /* BattleCharacterEntityModel server character */
#define EX_OFF_CHAR_ID              RR_OFF_CHAR_ID            /* 0x038 BattleCharacterModel.Id */
#define EX_OFF_CHAR_DATA            RR_OFF_CHAR_DATA          /* 0x040 BattleCharacterModel.Data */
#define EX_OFF_DATA_COMMON          RR_OFF_DATA_COMMON        /* 0x020 BattleCharacterData.CommonData */
#define EX_OFF_COMMON_ROLE          RR_OFF_COMMON_ROLE        /* 0x020 BattleCharacterCommonData.Role */
/* Reference<T> / description payload, identical to rr_game.c fill_entry(). */
#define EX_OFF_REF_VALUE            0x18u
#define EX_OFF_DESC_NAME            0x10u
#define EX_OFF_PRIM_VALUE           0x18u

/* --- Instant loot: the reveal timers of the container being viewed -------
 * src: 19/11/2026 source. NOT yet confirmed against the 22/11 build: every
 * field below is value-validated before it is written, so an offset that moved
 * produces no writes rather than corruption. The chain is
 * BattleEntitiesUpdater -> BattleScreenContent -> InventoryRevealViewDescriptions
 * -> List<Pair> -> inventory descriptor. */
#define EX_OFF_UPDATER_TO_SCREEN    0x068u  /* BattleEntitiesUpdater -> BattleScreenContent */
#define EX_OFF_SCREEN_TO_INST       0x540u  /* BattleScreenContent -> InventoryRevealViewDescriptions */
#define EX_OFF_INST_TIME_CELL       0x058u  /* inst -> TimeToOneCellBorder (float, s) */
#define EX_OFF_INST_LIST            0x018u  /* inst -> List<Pair> */
#define EX_OFF_LIST_ITEMS           0x010u  /* List<T>._items (backing array) */
#define EX_OFF_LIST_COUNT           0x018u  /* List<T>._size */
#define EX_OFF_LIST_STRIDE          0x018u  /* sizeof(Pair) in the backing array */
#define EX_OFF_PAIR_DESC            0x010u  /* Pair -> inventory descriptor */
#define EX_OFF_DESC_SHINE           0x040u  /* desc -> shine duration (float, s) */
#define EX_OFF_DESC_ICON_REVEAL     0x068u  /* desc -> icon reveal duration (float, s) */
/* desc + 0x58 is GlowColor (float RGBA). Writing it corrupts the renderer, so
 * the name exists only to keep the constant documented and unreferenced. */
#define EX_OFF_DESC_GLOW_COLOR      0x058u  /* DO NOT WRITE */

/* Write guards. A live duration outside its bound means the offset does not
 * describe what we think it does, so the pass skips instead of writing. */
#define EX_INST_TIME_CELL_MAX       5.0f    /* sane TimeToOneCellBorder range  */
#define EX_INST_DURATION_MAX        10.0f   /* sane shine / icon range         */
#define EX_INST_MAX_ITEMS           500     /* list sanity cap                 */
#define EX_INST_TICK_MS             250     /* loop period while a container is open */

/* --- IL2CPP object layout (rr_unity.h map) ------------------------------ */
#define EX_OFF_OBJ_CLASS            0x00u           /* Il2CppObject.klass    */
#define EX_OFF_OBJ_NAME_ASCII       U_CLASS_NAME    /* 0x010 Il2CppClass.name */
#define EX_OFF_STRING_LEN           U_STR_LEN       /* 0x010 Il2CppString length (UTF-16 units) */
#define EX_OFF_STRING_CHARS         U_STR_CHARS     /* 0x014 UTF-16 characters */

/* Position and name bounds. A coordinate outside +/-EX_MAX_COORD is garbage
 * left by a torn-down world, not a marker worth drawing. */
#define EX_MAX_COORD                50000.0f
#define EX_MAX_WORLD_DICT           2048
#define EX_MAX_ENTITY_DICT          512
#define EX_MAX_SLOTS                256
#define EX_NAME_SCAN_END            0x200u   /* field offsets scanned for a name */
#define EXTRA_MAX_WORLD_ITEMS       512

/* One published world item. kind: 1 container, 2 loot, 3 vehicle loot,
 * 4 door, 5 hiding object. extra carries the hiding object's entity id; doors
 * keep their state in the name ("Door: Open") because DoorModel.State is a
 * description reference, not a value. */
typedef struct {
    int32_t  id;
    int32_t  kind;
    float    x, y, z;
    char     name[64];
    int32_t  extra;
} EXTRA_WORLD_ITEM;

/* Re-read the target and publish. Worker thread only: this is the function
 * that performs target reads. battle/world are the pointers the caller
 * validated; anything stale produces no items instead of a crash. force
 * bypasses the internal 400 ms throttle. */
void extras_refresh(uint64_t battle, uint64_t world, int force);

/* Published copies. No target reads, safe from any thread. */
int extras_cop_count(void);
int extras_slot_total(void);
int extras_world_snapshot(EXTRA_WORLD_ITEM* out, int max);

/* Instant loot: one validated pass over the container being viewed. This is the
 * only function in the module that writes to the target, and the 250 ms loop is
 * its only caller in production code. Returns the number of floats written: 0
 * when the switch is off, when either pointer is null (no container open), when
 * a pointer fails its class check, or when a duration reads outside its bound.
 * The self-test calls it with null and bogus pointers to prove those paths
 * write nothing. */
int extras_insta_tick(uint64_t battle, uint64_t updater);

/* Drop the publication (detach, world change) and stop the instant loot loop. */
void extras_reset(void);

/* Read-only seat-table dump for --probe. Walks the same chain ex_slots() uses
 * and appends the raw entries, the class name at every candidate offset, the
 * role chain of the first three occupied seats and the resolved counts to
 * status.log beside the exe. Returns the number of occupied seats, or -1 when
 * the table cannot be read. Makes no writes and publishes nothing. */
int extras_slots_probe(uint64_t battle_model, uint64_t world);

#ifdef __cplusplus
}
#endif

#endif /* RR_EXTRAS_H */

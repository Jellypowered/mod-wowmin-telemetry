/*
 * Copyright (C) 2026 WoWMin contributors, released under the MIT license.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "AbstractFollower.h"
#include "Battleground.h"
#include "Chat.h"
#include "ConfigValueCache.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Group.h"
#include "GroupMgr.h"
#include "InstanceScript.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MovementGenerator.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "StringConvert.h"
#include "TypeContainerVisitor.h"
#include "Unit.h"
#include "Vehicle.h"
#include "WorldSession.h"
#include "WorldStatePackets.h"

#ifdef MOD_PLAYERBOTS
#include "Action.h"
#include "AiObjectContext.h"
#include "BattleGroundTactics.h"
#include "Engine.h"
#include "LastMovementValue.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#endif

using namespace Acore::ChatCommands;

namespace
{
enum class WowMinTelemetryConfig
{
    Enabled,
    StreamEnabled,
    StreamHost,
    StreamPort,
    StreamIntervalMs,

    NumConfigs,
};

class WowMinTelemetryConfigData : public ConfigValueCache<WowMinTelemetryConfig>
{
public:
    WowMinTelemetryConfigData() : ConfigValueCache(WowMinTelemetryConfig::NumConfigs) {}

    void BuildConfigCache() override
    {
        SetConfigValue<bool>(WowMinTelemetryConfig::Enabled, "WowMinTelemetry.Enable", false);
        SetConfigValue<bool>(WowMinTelemetryConfig::StreamEnabled, "WowMinTelemetry.Stream.Enable", false);
        SetConfigValue<std::string>(WowMinTelemetryConfig::StreamHost, "WowMinTelemetry.Stream.Host", "127.0.0.1");
        SetConfigValue<uint32>(WowMinTelemetryConfig::StreamPort, "WowMinTelemetry.Stream.Port", 7979);
        SetConfigValue<uint32>(WowMinTelemetryConfig::StreamIntervalMs, "WowMinTelemetry.Stream.IntervalMs", 100);
    }
};

WowMinTelemetryConfigData telemetryConfig;
std::mutex telemetryStateMutex;
std::unordered_map<uint64, uint64> sessionStartedAt;
std::unordered_map<uint64, uint32> mapUpdateTimers;
#ifdef MOD_PLAYERBOTS
std::unordered_map<uint64, std::unordered_map<uint64, uint32>> battlegroundRoles;
#endif
constexpr std::size_t MAX_DEATH_HISTORY = 50;
constexpr std::size_t MAX_EVENT_HISTORY = 1000;
enum PlayerStateFlags : uint32
{
    PLAYER_STATE_TAXI = 1 << 0,
    PLAYER_STATE_MOUNTED = 1 << 1,
    PLAYER_STATE_SAPPED = 1 << 2,
    PLAYER_STATE_STUNNED = 1 << 3,
    PLAYER_STATE_SPIRIT_FORM = 1 << 4,
    PLAYER_STATE_FLAG_CARRIER = 1 << 5,
    PLAYER_STATE_FLYING = 1 << 6,
    PLAYER_STATE_SITTING = 1 << 7,
};
constexpr uint32 SPELL_WARSONG_FLAG = 23333;
constexpr uint32 SPELL_SILVERWING_FLAG = 23335;
constexpr uint32 SPELL_NETHERSTORM_FLAG = 34976;
uint64 nextDeathEventId = 1;
uint64 nextSessionEventId = 1;

uint64 GetSessionKey(uint32 mapId, uint32 instanceId) { return uint64(mapId) << 32 | instanceId; }

uint64 GetSessionStartedAt(uint32 mapId, uint32 instanceId)
{
    if (!instanceId)
        return 0;

    std::lock_guard<std::mutex> lock(telemetryStateMutex);
    auto const iterator = sessionStartedAt.find(GetSessionKey(mapId, instanceId));
    return iterator != sessionStartedAt.end() ? iterator->second : 0;
}

#ifdef MOD_PLAYERBOTS
int32 GetCachedBattlegroundRole(uint32 mapId, uint32 instanceId, ObjectGuid playerGuid)
{
    std::lock_guard<std::mutex> lock(telemetryStateMutex);
    auto const sessionIterator = battlegroundRoles.find(GetSessionKey(mapId, instanceId));
    if (sessionIterator == battlegroundRoles.end())
        return 0;

    auto const roleIterator = sessionIterator->second.find(playerGuid.GetCounter());
    return roleIterator != sessionIterator->second.end() ? roleIterator->second : 0;
}
#endif

uint8 GetGroupRole(Group const* group, ObjectGuid playerGuid)
{
    if (!group)
        return 0;

    for (Group::MemberSlot const& member : group->GetMemberSlots())
        if (member.guid == playerGuid)
            return member.roles;

    return 0;
}

std::string EscapeTelemetryField(std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (char character : value)
    {
        switch (character)
        {
            case '%':
                escaped += "%25";
                break;
            case '|':
                escaped += "%7C";
                break;
            case '\r':
                escaped += "%0D";
                break;
            case '\n':
                escaped += "%0A";
                break;
            default:
                escaped += character;
                break;
        }
    }
    return escaped;
}

uint32 GetPlayerStateFlags(Player* player)
{
    uint32 stateFlags = 0;
    if (player->IsInFlight())
        stateFlags |= PLAYER_STATE_TAXI;
    if (player->IsMounted())
        stateFlags |= PLAYER_STATE_MOUNTED;
    if (player->HasAuraWithMechanic(1ULL << MECHANIC_SAPPED))
        stateFlags |= PLAYER_STATE_SAPPED;
    if (player->HasUnitState(UNIT_STATE_STUNNED))
        stateFlags |= PLAYER_STATE_STUNNED;
    if (player->HasSpiritOfRedemptionAura())
        stateFlags |= PLAYER_STATE_SPIRIT_FORM;
    if (player->IsFlying())
        stateFlags |= PLAYER_STATE_FLYING;
    // On the ground or in a chair (eating, drinking, resting).
    if (player->IsSitState())
        stateFlags |= PLAYER_STATE_SITTING;
    if (player->HasAura(SPELL_WARSONG_FLAG) || player->HasAura(SPELL_SILVERWING_FLAG) ||
        player->HasAura(SPELL_NETHERSTORM_FLAG))
        stateFlags |= PLAYER_STATE_FLAG_CARRIER;
    return stateFlags;
}

int32 GetUnitWmoGroupId(Map* map, Unit* unit)
{
    uint32 mogpFlags = 0;
    int32 adtId = 0;
    int32 rootId = 0;
    int32 wmoGroupId = -1;
    if (!map->GetAreaInfo(unit->GetPhaseMask(), unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ(),
                          mogpFlags, adtId, rootId, wmoGroupId))
        return -1;
    return wmoGroupId;
}

// ── Position stream ──────────────────────────────────────────
//
// wowmin subscribes to a session with "wowmin stream <mapId> <instanceId>" and
// renews it every few seconds. While subscribed, the session's map update
// sends a UDP snapshot of its players to the configured destination (never one
// chosen by the command) every WowMinTelemetry.Stream.IntervalMs.
//
// Datagram, little-endian:
//   header (28 bytes): "WMS1", u8 version, u8 part, u8 partCount, u8 reserved,
//     u32 mapId, u32 instanceId, u32 sequence, u32 serverTimeMs,
//     u16 playerCount, u16 reserved
//   player (30 bytes + name): u32 guid, f32 x, f32 y, f32 z, f32 orientation,
//     u8 healthPct, u8 powerPct, i8 powerType, u8 flags (1 alive, 2 in combat,
//     4 waiting to resurrect), u8 stateFlags (1 on a taxi, 2 mounted, 4 sapped,
//     8 stunned, 16 spirit form, 32 carrying a flag, 64 flying, 128 sitting), u8
//     nameLength, i32 wmoGroupId, name bytes
// A snapshot larger than one datagram is split into parts sharing a sequence.
//
// Movement paths go in separate datagrams, sent alongside a snapshot when a
// player's server-driven path changes and re-sent every
// STREAM_PATH_REFRESH_MS while it lasts. Each path entry stands alone.
//   header (24 bytes): "WMP1", u8 version, u8 reserved x3, u32 mapId,
//     u32 instanceId, u32 serverTimeMs, u16 pathCount, u16 reserved
//   path (12 bytes + points): u32 guid, u32 splineId (0: no longer moving on
//     a path), u8 pointCount, u8 flags (1 truncated: the middle of the path
//     was dropped; 2 the guid is a creature's, not a player's), u16 reserved,
//     then pointCount x (f32 x, f32 y, f32 z): the waypoints still ahead,
//     ending at the destination
//
// Creatures go in their own datagrams too: each one whenever it moves, fights,
// or changes, and every STREAM_CREATURE_REFRESH_MS otherwise. A creature not
// re-sent for a few seconds has despawned. Triggers, totems, and critters are
// left out. Names, levels, and ranks come from the world database by entry.
//   header (24 bytes): "WMC1", then as for paths, with u16 creatureCount
//   creature (36 bytes): u32 guid, u32 entry, f32 x, f32 y, f32 z,
//     f32 orientation, u8 healthPct, u8 flags (1 alive, 2 in combat, 4 hostile
//     to players, 8 boss, 16 controlled by a player, e.g. a pet, 32 its target
//     is a player, 64 scenery: see IsScenery; its path isn't sent), u16
//     reserved, u32 targetGuid (0: none), i32 wmoGroupId
//
// With mod-playerbots, what each bot is deciding goes in its own datagrams
// too: whenever it changes (at most every STREAM_BOT_MIN_INTERVAL_MS) and
// every STREAM_BOT_REFRESH_MS otherwise. The module only listens to the bot's
// engines and never changes what they do.
//   header (24 bytes): "WMB1", then as for paths, with u16 botCount
//   bot (36 bytes + strategies + actions): u32 guid, u8 state (0 combat,
//     1 out of combat, 2 dead), u8 motionType (the core's
//     MovementGeneratorType; 255: none), u8 movePriority (0 idle, 1 wander,
//     2 normal, 3 combat, 4 forced), u8 flags (1 it has a move target, 2 its
//     motion target is a player, 4 its selected target is a player),
//     f32 moveX, f32 moveY, f32 moveZ (where it last chose to move),
//     u32 moveAgeMs, u32 motionTargetGuid (whom it follows or chases; 0: none),
//     u32 targetGuid (its selection; 0: none), u16 strategiesLength,
//     u8 actionCount, u8 reserved, the strategies of its current state
//     (comma-separated), then actionCount x (u8 nameLength, u8 reserved,
//     u16 repeats, f32 relevance, u32 ageMs, name bytes), newest first
//
// How each player and creature looks goes in its own datagrams too, whenever
// it changes and every STREAM_APPEARANCE_REFRESH_MS otherwise, so wowmin can
// draw their models with their gear.
//   header (24 bytes): "WMA1", then as for paths, with u16 unitCount
//   unit (108 bytes): u32 guid, u8 flags (1 a creature, not a player, 2 helm
//     hidden, 4 cloak hidden), u8 race, u8 gender, u8 class, u8 skin, u8 face,
//     u8 hairStyle, u8 hairColor, u8 facialStyle, u8 sheathState (0 none,
//     1 melee drawn, 2 ranged drawn), u16 scale (object scale x 1000),
//     u32 displayId,
//     u32 nativeDisplayId, u32 mountDisplayId, then 19 x u32 item display IDs
//     by equipment slot (0: empty), then u8 mainHandType (InventoryType),
//     u8 mainHandSubclass (weapon subclass), u8 offHandType (InventoryType),
//     u8 rangedSubclass, so the right attack animation can be chosen. For a
//     creature only the display IDs, the sheath state, the scale, and the
//     weapon slots matter.
//
// Which auras each unit wears that have spell art, so wowmin can draw them:
// sent when a unit's list changes and every STREAM_APPEARANCE_REFRESH_MS while
// it isn't empty; a unit whose last such aura goes is sent once with none.
// Only auras the client is told about (with an aura slot), not passive, and
// whose spell has a visual count, at most STREAM_MAX_UNIT_AURAS a unit.
//   header (24 bytes): "WMU1", then as for paths, with u16 unitCount
//   unit (8 bytes + auras): u32 guid, u8 flags (1 a creature, not a player),
//     u8 auraCount, u16 reserved, then auraCount x u32 spell ID
//
// Players riding vehicles (Ulduar's demolishers, a passenger seat on a
// mammoth) go in their own datagrams, with every snapshot while any are, so
// wowmin can seat or hide them; a player missing from them is on foot.
//   header (24 bytes): "WMV1", then as for paths, with u16 passengerCount
//   passenger (12 bytes): u32 guid, u32 vehicleGuid, u8 flags (1 the vehicle
//     is a creature, not a player, 2 the seat hides its passenger, 4 the
//     seat drives the vehicle), i8 seat index, u16 reserved
//
// Game objects with a model (battleground banners and flags, doors, chests)
// go in their own datagrams too, whenever one appears or changes and every
// STREAM_GAMEOBJECT_REFRESH_MS otherwise; one that despawns is sent once
// marked gone. Objects without a model and spell foci are left out.
//   header (24 bytes): "WMG1", then as for paths, with u16 objectCount
//   object (48 bytes): u32 guid, u32 entry, u32 displayId, f32 x, f32 y,
//     f32 z, f32 rotation x, y, z, w (a quaternion), u16 scale (x 1000), u8
//     type (GameobjectTypes), u8 state (GOState: 0 active, e.g. a door open,
//     1 ready, 2 alternative), u8 flags (1 gone), u8 reserved x3
//
// The harmful auras on each unit (for wowmin's nameplates) go in their own
// datagrams: when a unit's list changes and every STREAM_DEBUFF_REFRESH_MS
// while it isn't empty; a unit whose last one goes is sent once with none.
// At most STREAM_MAX_DEBUFFS a unit, from those with an aura slot.
//   header (24 bytes): "WMD1", then as for paths, with u16 unitCount
//   unit (8 bytes + debuffs): u32 guid, u8 flags (1 a creature), u8
//     debuffCount, u16 reserved, then debuffCount x (u32 spellId, u32
//     remainingMs (0: lasts until removed), u32 durationMs, u8 stacks, u8
//     flags (1 cast by a player), u16 reserved)
//
// The raid markers (skull, cross, ...) the session's groups have set go in
// their own datagram when they change and every STREAM_MARKER_REFRESH_MS
// while any are set; once all are cleared it is sent once empty.
//   header (24 bytes): "WMR1", then as for paths, with u16 markerCount
//   marker (8 bytes): u8 icon (0 star .. 7 skull), u8 flags (1 the target is
//     a creature), u16 reserved, u32 guid
//
// Combat events (casts, notable auras, auto-attack swings, damage, and heals)
// are gathered from the core's hooks as they happen and sent with the next
// snapshot, at most STREAM_MAX_EVENTS per snapshot; the header's reserved u16
// (offset 22) counts the events dropped beyond that. Only subscribed sessions
// gather any.
//   header (24 bytes): "WME1", then as for paths, with u16 eventCount
//   event (44 bytes): u8 kind (1 cast started, 2 cast fired, 3 cast
//     cancelled, 4 aura applied or refreshed, 5 aura removed, 6 auto-attack
//     swing, 7 damage, 8 heal), u8 flags (1 the source is a creature, 2 the
//     target is a creature, 4 area spell, 8 channelled, 16 has a ground
//     point, 32 periodic, 64 helpful, 128 triggered by another spell),
//     u8 schoolMask, u8 auraCategory (0 none, 1 crowd control, 2 shield or
//     immunity, 3 defensive, 4 offensive), u32 sourceGuid (0: none),
//     u32 targetGuid (0: none), u32 spellId (0: none, e.g. a white hit),
//     u32 value (cast started: cast time in ms; cast fired: channel duration
//     in ms; aura applied: duration in ms, 0 if permanent; damage and heal:
//     the final amount), f32 missileSpeed (yards per second; 0: instant),
//     f32 radius (yards; 0: none), f32 x, f32 y, f32 z (the ground point),
//     u16 ageMs (how long before the header's server time it happened),
//     u16 extraFlags (1 the spell leaves a lasting area on the ground, such as
//     Consecration; its cast-fired event then carries the area's duration in
//     value, as a channel's does)

constexpr uint8 STREAM_VERSION = 1;
constexpr uint64 STREAM_SUBSCRIPTION_TTL_MS = 10000;
constexpr std::size_t STREAM_MAX_SUBSCRIPTIONS = 64;
constexpr std::size_t STREAM_MAX_DATAGRAM = 1200;
constexpr std::size_t STREAM_MAX_NAME = 48;
constexpr uint32 STREAM_MIN_INTERVAL_MS = 20;
constexpr std::size_t STREAM_MAX_PATH_POINTS = 32;
constexpr uint64 STREAM_PATH_REFRESH_MS = 1000;
constexpr uint64 STREAM_CREATURE_REFRESH_MS = 1000;
// A creature that has moved this far since its last update is re-sent, and
// its WMO group looked up again.
constexpr float STREAM_CREATURE_MOVE_YARDS = 0.5f;
constexpr uint64 STREAM_BOT_REFRESH_MS = 1000;
constexpr uint64 STREAM_BOT_MIN_INTERVAL_MS = 250;
constexpr std::size_t STREAM_MAX_STRATEGIES = 400;
constexpr std::size_t STREAM_MAX_ACTION_NAME = 48;
// Actions kept per bot; a run of the same action counts as one.
constexpr std::size_t BOT_ACTION_HISTORY = 6;
constexpr uint64 STREAM_APPEARANCE_REFRESH_MS = 5000;
constexpr uint64 STREAM_GAMEOBJECT_REFRESH_MS = 10000;
constexpr uint64 STREAM_DEBUFF_REFRESH_MS = 3000;
constexpr std::size_t STREAM_MAX_DEBUFFS = 6;
constexpr uint64 STREAM_MARKER_REFRESH_MS = 2000;
// A group's copied raid markers are dropped once no stream has asked for
// them for this long.
constexpr uint64 STREAM_GROUP_MARKERS_FORGET_MS = 10000;
constexpr std::size_t STREAM_MAX_EVENTS = 256;
constexpr std::size_t STREAM_MAX_UNIT_AURAS = 32;

enum StreamEventKind : uint8
{
    STREAM_EVENT_CAST_START = 1,
    STREAM_EVENT_CAST_GO = 2,
    STREAM_EVENT_CAST_CANCEL = 3,
    STREAM_EVENT_AURA_APPLY = 4,
    STREAM_EVENT_AURA_REMOVE = 5,
    STREAM_EVENT_SWING = 6,
    STREAM_EVENT_DAMAGE = 7,
    STREAM_EVENT_HEAL = 8,
};

enum StreamEventFlag : uint8
{
    STREAM_EVENT_SOURCE_CREATURE = 1,
    STREAM_EVENT_TARGET_CREATURE = 2,
    STREAM_EVENT_AREA = 4,
    STREAM_EVENT_CHANNELED = 8,
    STREAM_EVENT_GROUND = 16,
    STREAM_EVENT_PERIODIC = 32,
    STREAM_EVENT_HELPFUL = 64,
    STREAM_EVENT_TRIGGERED = 128,
};

enum StreamEventExtraFlag : uint16
{
    STREAM_EVENT_PERSISTENT_AREA = 1,
};

enum StreamAuraCategory : uint8
{
    STREAM_AURA_NONE = 0,
    STREAM_AURA_CROWD_CONTROL = 1,
    STREAM_AURA_SHIELD = 2,
    STREAM_AURA_DEFENSIVE = 3,
    STREAM_AURA_OFFENSIVE = 4,
};

struct StreamCombatEvent
{
    uint8 kind = 0;
    uint8 flags = 0;
    uint8 schoolMask = 0;
    uint8 auraCategory = STREAM_AURA_NONE;
    uint32 sourceGuid = 0;
    uint32 targetGuid = 0;
    uint32 spellId = 0;
    uint32 value = 0;
    float missileSpeed = 0.0f;
    float radius = 0.0f;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint16 extraFlags = 0;
    uint64 atMs = 0;
};

// What a unit wears: item display IDs by equipment slot, and the weapon types
// that choose its attack animation.
struct StreamGear
{
    std::array<uint32, EQUIPMENT_SLOT_END> items{};
    uint8 mainHandType = 0;
    uint8 mainHandSubclass = 0;
    uint8 offHandType = 0;
    uint8 rangedSubclass = 0;
};

// Folds a value into a change-detection signature (boost's hash_combine
// mix: the golden-ratio constant and two shifts).
constexpr std::size_t SIGNATURE_MIX = 0x9e3779b97f4a7c15ULL;

template <class T>
void MixSignature(std::size_t& signature, T const& value)
{
    signature ^= std::hash<T>{}(value) + SIGNATURE_MIX + (signature << 6) + (signature >> 2);
}

// Raid markers, copied for the groups the streams show. A group's markers are
// set from the world thread (a player's /mark) and from map threads (bots mark
// targets in their AI update), so a map thread must not read the group's own
// while another map updates. Streams ask for their groups' markers here; the
// world update copies them after every map has finished its update.
using GroupMarkers = std::array<ObjectGuid, TARGETICONCOUNT>;
struct CopiedGroupMarkers
{
    GroupMarkers markers{};
    uint64 askedAtMs = 0;
};
std::mutex groupMarkersMutex;
std::unordered_map<ObjectGuid::LowType, CopiedGroupMarkers> groupMarkers;

// Map thread: the groups' markers as last copied (none before the first
// copy), each group's in the order given.
std::vector<GroupMarkers> GetGroupMarkers(std::vector<ObjectGuid::LowType> const& groupIds, uint64 nowMs)
{
    std::vector<GroupMarkers> markers;
    markers.reserve(groupIds.size());
    std::lock_guard<std::mutex> lock(groupMarkersMutex);
    for (ObjectGuid::LowType groupId : groupIds)
    {
        CopiedGroupMarkers& copied = groupMarkers[groupId];
        copied.askedAtMs = nowMs;
        markers.push_back(copied.markers);
    }
    return markers;
}

// World thread, after the maps' update: copies the asked-for groups' markers,
// and forgets groups that have gone or that no stream has asked for lately.
void CopyGroupMarkers(uint64 nowMs)
{
    std::lock_guard<std::mutex> lock(groupMarkersMutex);
    std::erase_if(groupMarkers,
                  [&](auto& entry)
                  {
                      Group* group = sGroupMgr->GetGroupByGUID(entry.first);
                      if (!group || nowMs - entry.second.askedAtMs > STREAM_GROUP_MARKERS_FORGET_MS)
                          return true;
                      for (uint8 icon = 0; icon < TARGETICONCOUNT; ++icon)
                          entry.second.markers[icon] = group->GetTargetIcon(icon);
                      return false;
                  });
}

// What was last sent for each path and creature, keyed by guid. Only the
// session's map thread uses it.
struct StreamSessionState
{
    struct SentPath
    {
        uint32 splineId = 0;
        uint64 sentAtMs = 0;
    };
    struct SentCreature
    {
        uint64 sentAtMs = 0;
        uint8 healthPct = 0;
        uint8 flags = 0;
        uint32 targetGuid = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        int32 wmoGroupId = -1;
    };
    std::unordered_map<uint32, SentPath> playerPaths;
    std::unordered_map<uint32, SentPath> creaturePaths;
    struct SentSignature
    {
        uint64 sentAtMs = 0;
        std::size_t signature = 0;
    };
    std::unordered_map<uint32, SentCreature> creatures;
    std::unordered_map<uint32, SentSignature> bots;
    struct SentAppearance
    {
        uint64 sentAtMs = 0;
        std::size_t signature = 0;
        // The item entries the unit showed when its gear was last read.
        std::optional<std::array<uint32, EQUIPMENT_SLOT_END>> gearEntries;
        StreamGear gear;
    };
    // Keyed by guid, with bit 32 set for creatures.
    std::unordered_map<uint64, SentAppearance> appearances;
    // Signature 0 means the unit was last sent (or seen) with no art auras.
    std::unordered_map<uint64, SentSignature> auras;
    std::unordered_map<uint32, SentSignature> gameObjects;
    std::unordered_map<uint64, SentSignature> debuffs;
    SentSignature markers;
    // Filled by the combat hooks and emptied by each snapshot. The hooks
    // normally run on the session's map thread, but take no chances.
    std::mutex eventMutex;
    std::vector<StreamCombatEvent> events;
    uint32 droppedEvents = 0;
};

struct StreamSubscription
{
    uint64 expiresAtMs;
    uint32 elapsedMs;
    uint32 sequence;
    std::shared_ptr<StreamSessionState> state;
};

std::mutex streamStateMutex;
std::unordered_map<uint64, StreamSubscription> streamSubscriptions;
// Lets the combat hooks, which run for every unit, skip the lock while nobody
// is watching. Updated with streamSubscriptions, under streamStateMutex.
std::atomic<bool> streamAnySubscribed = false;

struct StreamPlayer
{
    uint32 guid;
    float x;
    float y;
    float z;
    float orientation;
    uint8 healthPct;
    uint8 powerPct;
    int8 powerType;
    uint8 flags;
    uint8 stateFlags;
    int32 wmoGroupId;
    std::string name;
};

struct StreamPath
{
    uint32 guid;
    uint32 splineId;
    uint8 flags;
    std::vector<G3D::Vector3> points;
};

struct StreamCreature
{
    uint32 guid;
    uint32 entry;
    float x;
    float y;
    float z;
    float orientation;
    uint8 healthPct;
    uint8 flags;
    uint32 targetGuid;
    int32 wmoGroupId;
};

struct StreamAppearance
{
    uint32 guid = 0;
    uint8 flags = 0;
    uint8 race = 0;
    uint8 gender = 0;
    uint8 unitClass = 0;
    uint8 skin = 0;
    uint8 face = 0;
    uint8 hairStyle = 0;
    uint8 hairColor = 0;
    uint8 facialStyle = 0;
    uint8 sheathState = 0;
    uint16 scale = 1000;
    uint32 displayId = 0;
    uint32 nativeDisplayId = 0;
    uint32 mountDisplayId = 0;
    StreamGear gear;
};

struct StreamDebuff
{
    uint32 spellId = 0;
    uint32 remainingMs = 0;
    uint32 durationMs = 0;
    uint8 stacks = 0;
    bool fromPlayer = false;
};

struct StreamUnitDebuffs
{
    uint32 guid = 0;
    bool creature = false;
    std::vector<StreamDebuff> debuffs;
};

struct StreamMarker
{
    uint8 icon = 0;
    bool creature = false;
    uint32 guid = 0;
};

struct StreamGameObject
{
    uint32 guid = 0;
    uint32 entry = 0;
    uint32 displayId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
    uint16 scale = 1000;
    uint8 type = 0;
    uint8 state = 0;
    bool gone = false;
};

struct StreamPassenger
{
    uint32 guid = 0;
    uint32 vehicleGuid = 0;
    uint8 flags = 0;
    int8 seat = 0;
};

struct StreamUnitAuras
{
    uint32 guid = 0;
    bool creature = false;
    std::vector<uint32> spellIds;
};

struct StreamBotAction
{
    std::string name;
    float relevance;
    uint32 ageMs;
    uint16 repeats;
};

struct StreamBot
{
    uint32 guid = 0;
    uint8 state = 0;
    uint8 motionType = 255;
    uint8 movePriority = 0;
    uint8 flags = 0;
    float moveX = 0.0f;
    float moveY = 0.0f;
    float moveZ = 0.0f;
    uint32 moveAgeMs = 0;
    uint32 motionTargetGuid = 0;
    uint32 targetGuid = 0;
    std::string strategies;
    std::vector<StreamBotAction> actions;
};

void AppendU8(std::vector<uint8>& out, uint8 value) { out.push_back(value); }

void AppendU16(std::vector<uint8>& out, uint16 value)
{
    out.push_back(uint8(value));
    out.push_back(uint8(value >> 8));
}

void AppendU32(std::vector<uint8>& out, uint32 value)
{
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(uint8(value >> shift));
}

void AppendF32(std::vector<uint8>& out, float value)
{
    uint32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    AppendU32(out, bits);
}

std::vector<std::vector<uint8>> BuildStreamDatagrams(uint32 mapId, uint32 instanceId, uint32 sequence,
                                                     uint32 serverTimeMs, std::vector<StreamPlayer> const& players)
{
    std::vector<std::vector<uint8>> datagrams;
    auto startDatagram = [&]()
    {
        std::vector<uint8>& datagram = datagrams.emplace_back();
        datagram.reserve(STREAM_MAX_DATAGRAM);
        datagram.insert(datagram.end(), {'W', 'M', 'S', '1'});
        AppendU8(datagram, STREAM_VERSION);
        AppendU8(datagram, 0);  // part, patched below
        AppendU8(datagram, 0);  // partCount, patched below
        AppendU8(datagram, 0);
        AppendU32(datagram, mapId);
        AppendU32(datagram, instanceId);
        AppendU32(datagram, sequence);
        AppendU32(datagram, serverTimeMs);
        AppendU16(datagram, 0);  // playerCount, patched below
        AppendU16(datagram, 0);
    };
    startDatagram();
    uint16 count = 0;
    auto finishDatagram = [&]()
    {
        datagrams.back()[24] = uint8(count);
        datagrams.back()[25] = uint8(count >> 8);
        count = 0;
    };

    for (StreamPlayer const& player : players)
    {
        std::size_t const nameLength = std::min(player.name.size(), STREAM_MAX_NAME);
        if (count && datagrams.back().size() + 30 + nameLength > STREAM_MAX_DATAGRAM)
        {
            finishDatagram();
            startDatagram();
        }
        std::vector<uint8>& datagram = datagrams.back();
        AppendU32(datagram, player.guid);
        AppendF32(datagram, player.x);
        AppendF32(datagram, player.y);
        AppendF32(datagram, player.z);
        AppendF32(datagram, player.orientation);
        AppendU8(datagram, player.healthPct);
        AppendU8(datagram, player.powerPct);
        AppendU8(datagram, uint8(player.powerType));
        AppendU8(datagram, player.flags);
        AppendU8(datagram, player.stateFlags);
        AppendU8(datagram, uint8(nameLength));
        AppendU32(datagram, uint32(player.wmoGroupId));
        datagram.insert(datagram.end(), player.name.begin(), player.name.begin() + nameLength);
        ++count;
    }
    finishDatagram();

    std::size_t const partCount = std::min<std::size_t>(datagrams.size(), 255);
    datagrams.resize(partCount);
    for (std::size_t part = 0; part < partCount; ++part)
    {
        datagrams[part][5] = uint8(part);
        datagrams[part][6] = uint8(partCount);
    }
    return datagrams;
}

// Packs self-contained entries into as few datagrams as fit, each with the
// 24-byte header that path and creature datagrams share.
std::vector<std::vector<uint8>> PackEntries(std::string_view magic, uint32 mapId, uint32 instanceId,
                                            uint32 serverTimeMs, std::vector<std::vector<uint8>> const& entries)
{
    std::vector<std::vector<uint8>> datagrams;
    uint16 count = 0;
    auto finishDatagram = [&]()
    {
        datagrams.back()[20] = uint8(count);
        datagrams.back()[21] = uint8(count >> 8);
        count = 0;
    };
    for (std::vector<uint8> const& entry : entries)
    {
        if (datagrams.empty() || datagrams.back().size() + entry.size() > STREAM_MAX_DATAGRAM)
        {
            if (!datagrams.empty())
                finishDatagram();
            std::vector<uint8>& datagram = datagrams.emplace_back();
            datagram.reserve(STREAM_MAX_DATAGRAM);
            datagram.insert(datagram.end(), magic.begin(), magic.end());
            AppendU8(datagram, STREAM_VERSION);
            AppendU8(datagram, 0);
            AppendU16(datagram, 0);
            AppendU32(datagram, mapId);
            AppendU32(datagram, instanceId);
            AppendU32(datagram, serverTimeMs);
            AppendU16(datagram, 0);  // entry count, patched by finishDatagram
            AppendU16(datagram, 0);
        }
        datagrams.back().insert(datagrams.back().end(), entry.begin(), entry.end());
        ++count;
    }
    if (!datagrams.empty())
        finishDatagram();
    return datagrams;
}

std::vector<std::vector<uint8>> BuildPathDatagrams(uint32 mapId, uint32 instanceId, uint32 serverTimeMs,
                                                   std::vector<StreamPath> const& paths)
{
    std::vector<std::vector<uint8>> entries;
    entries.reserve(paths.size());
    for (StreamPath const& path : paths)
    {
        std::size_t const pointCount = std::min(path.points.size(), STREAM_MAX_PATH_POINTS);
        std::vector<uint8>& entry = entries.emplace_back();
        entry.reserve(12 + pointCount * 12);
        AppendU32(entry, path.guid);
        AppendU32(entry, path.splineId);
        AppendU8(entry, uint8(pointCount));
        AppendU8(entry, path.flags);
        AppendU16(entry, 0);
        for (std::size_t index = 0; index < pointCount; ++index)
        {
            AppendF32(entry, path.points[index].x);
            AppendF32(entry, path.points[index].y);
            AppendF32(entry, path.points[index].z);
        }
    }
    return PackEntries("WMP1", mapId, instanceId, serverTimeMs, entries);
}

std::vector<std::vector<uint8>> BuildCreatureDatagrams(uint32 mapId, uint32 instanceId, uint32 serverTimeMs,
                                                       std::vector<StreamCreature> const& creatures)
{
    std::vector<std::vector<uint8>> entries;
    entries.reserve(creatures.size());
    for (StreamCreature const& creature : creatures)
    {
        std::vector<uint8>& entry = entries.emplace_back();
        entry.reserve(36);
        AppendU32(entry, creature.guid);
        AppendU32(entry, creature.entry);
        AppendF32(entry, creature.x);
        AppendF32(entry, creature.y);
        AppendF32(entry, creature.z);
        AppendF32(entry, creature.orientation);
        AppendU8(entry, creature.healthPct);
        AppendU8(entry, creature.flags);
        AppendU16(entry, 0);
        AppendU32(entry, creature.targetGuid);
        AppendU32(entry, uint32(creature.wmoGroupId));
    }
    return PackEntries("WMC1", mapId, instanceId, serverTimeMs, entries);
}

std::vector<std::vector<uint8>> BuildBotDatagrams(uint32 mapId, uint32 instanceId, uint32 serverTimeMs,
                                                  std::vector<StreamBot> const& bots)
{
    std::vector<std::vector<uint8>> entries;
    entries.reserve(bots.size());
    for (StreamBot const& bot : bots)
    {
        std::size_t const strategiesLength = std::min(bot.strategies.size(), STREAM_MAX_STRATEGIES);
        std::size_t const actionCount = std::min(bot.actions.size(), BOT_ACTION_HISTORY);
        std::vector<uint8>& entry = entries.emplace_back();
        entry.reserve(36 + strategiesLength + actionCount * (12 + STREAM_MAX_ACTION_NAME));
        AppendU32(entry, bot.guid);
        AppendU8(entry, bot.state);
        AppendU8(entry, bot.motionType);
        AppendU8(entry, bot.movePriority);
        AppendU8(entry, bot.flags);
        AppendF32(entry, bot.moveX);
        AppendF32(entry, bot.moveY);
        AppendF32(entry, bot.moveZ);
        AppendU32(entry, bot.moveAgeMs);
        AppendU32(entry, bot.motionTargetGuid);
        AppendU32(entry, bot.targetGuid);
        AppendU16(entry, uint16(strategiesLength));
        AppendU8(entry, uint8(actionCount));
        AppendU8(entry, 0);
        entry.insert(entry.end(), bot.strategies.begin(), bot.strategies.begin() + strategiesLength);
        for (std::size_t index = 0; index < actionCount; ++index)
        {
            StreamBotAction const& action = bot.actions[index];
            std::size_t const nameLength = std::min(action.name.size(), STREAM_MAX_ACTION_NAME);
            AppendU8(entry, uint8(nameLength));
            AppendU8(entry, 0);
            AppendU16(entry, action.repeats);
            AppendF32(entry, action.relevance);
            AppendU32(entry, action.ageMs);
            entry.insert(entry.end(), action.name.begin(), action.name.begin() + nameLength);
        }
    }
    return PackEntries("WMB1", mapId, instanceId, serverTimeMs, entries);
}

std::vector<std::vector<uint8>> BuildAppearanceDatagrams(uint32 mapId, uint32 instanceId, uint32 serverTimeMs,
                                                         std::vector<StreamAppearance> const& appearances)
{
    std::vector<std::vector<uint8>> entries;
    entries.reserve(appearances.size());
    for (StreamAppearance const& appearance : appearances)
    {
        std::vector<uint8>& entry = entries.emplace_back();
        entry.reserve(32 + appearance.gear.items.size() * 4);
        AppendU32(entry, appearance.guid);
        AppendU8(entry, appearance.flags);
        AppendU8(entry, appearance.race);
        AppendU8(entry, appearance.gender);
        AppendU8(entry, appearance.unitClass);
        AppendU8(entry, appearance.skin);
        AppendU8(entry, appearance.face);
        AppendU8(entry, appearance.hairStyle);
        AppendU8(entry, appearance.hairColor);
        AppendU8(entry, appearance.facialStyle);
        AppendU8(entry, appearance.sheathState);
        AppendU16(entry, appearance.scale);
        AppendU32(entry, appearance.displayId);
        AppendU32(entry, appearance.nativeDisplayId);
        AppendU32(entry, appearance.mountDisplayId);
        for (uint32 item : appearance.gear.items)
            AppendU32(entry, item);
        AppendU8(entry, appearance.gear.mainHandType);
        AppendU8(entry, appearance.gear.mainHandSubclass);
        AppendU8(entry, appearance.gear.offHandType);
        AppendU8(entry, appearance.gear.rangedSubclass);
    }
    return PackEntries("WMA1", mapId, instanceId, serverTimeMs, entries);
}

std::vector<std::vector<uint8>> BuildDebuffDatagrams(uint32 mapId, uint32 instanceId, uint32 serverTimeMs,
                                                     std::vector<StreamUnitDebuffs> const& units)
{
    std::vector<std::vector<uint8>> entries;
    entries.reserve(units.size());
    for (StreamUnitDebuffs const& unit : units)
    {
        std::vector<uint8>& entry = entries.emplace_back();
        entry.reserve(8 + unit.debuffs.size() * 16);
        AppendU32(entry, unit.guid);
        AppendU8(entry, unit.creature ? 1 : 0);
        AppendU8(entry, uint8(unit.debuffs.size()));
        AppendU16(entry, 0);
        for (StreamDebuff const& debuff : unit.debuffs)
        {
            AppendU32(entry, debuff.spellId);
            AppendU32(entry, debuff.remainingMs);
            AppendU32(entry, debuff.durationMs);
            AppendU8(entry, debuff.stacks);
            AppendU8(entry, debuff.fromPlayer ? 1 : 0);
            AppendU16(entry, 0);
        }
    }
    return PackEntries("WMD1", mapId, instanceId, serverTimeMs, entries);
}

std::vector<std::vector<uint8>> BuildMarkerDatagrams(uint32 mapId, uint32 instanceId, uint32 serverTimeMs,
                                                     std::vector<StreamMarker> const& markers)
{
    std::vector<std::vector<uint8>> entries;
    entries.reserve(markers.size());
    for (StreamMarker const& marker : markers)
    {
        std::vector<uint8>& entry = entries.emplace_back();
        AppendU8(entry, marker.icon);
        AppendU8(entry, marker.creature ? 1 : 0);
        AppendU16(entry, 0);
        AppendU32(entry, marker.guid);
    }
    std::vector<std::vector<uint8>> datagrams = PackEntries("WMR1", mapId, instanceId, serverTimeMs, entries);
    // All markers cleared: one empty datagram says so.
    if (datagrams.empty())
    {
        std::vector<uint8>& datagram = datagrams.emplace_back();
        datagram.insert(datagram.end(), {'W', 'M', 'R', '1'});
        AppendU8(datagram, STREAM_VERSION);
        AppendU8(datagram, 0);
        AppendU16(datagram, 0);
        AppendU32(datagram, mapId);
        AppendU32(datagram, instanceId);
        AppendU32(datagram, serverTimeMs);
        AppendU16(datagram, 0);
        AppendU16(datagram, 0);
    }
    return datagrams;
}

std::vector<std::vector<uint8>> BuildGameObjectDatagrams(uint32 mapId, uint32 instanceId, uint32 serverTimeMs,
                                                         std::vector<StreamGameObject> const& objects)
{
    std::vector<std::vector<uint8>> entries;
    entries.reserve(objects.size());
    for (StreamGameObject const& object : objects)
    {
        std::vector<uint8>& entry = entries.emplace_back();
        entry.reserve(48);
        AppendU32(entry, object.guid);
        AppendU32(entry, object.entry);
        AppendU32(entry, object.displayId);
        AppendF32(entry, object.x);
        AppendF32(entry, object.y);
        AppendF32(entry, object.z);
        for (float component : object.rotation)
            AppendF32(entry, component);
        AppendU16(entry, object.scale);
        AppendU8(entry, object.type);
        AppendU8(entry, object.state);
        AppendU8(entry, object.gone ? 1 : 0);
        AppendU8(entry, 0);
        AppendU16(entry, 0);
    }
    return PackEntries("WMG1", mapId, instanceId, serverTimeMs, entries);
}

std::vector<std::vector<uint8>> BuildPassengerDatagrams(uint32 mapId, uint32 instanceId, uint32 serverTimeMs,
                                                        std::vector<StreamPassenger> const& passengers)
{
    std::vector<std::vector<uint8>> entries;
    entries.reserve(passengers.size());
    for (StreamPassenger const& passenger : passengers)
    {
        std::vector<uint8>& entry = entries.emplace_back();
        entry.reserve(12);
        AppendU32(entry, passenger.guid);
        AppendU32(entry, passenger.vehicleGuid);
        AppendU8(entry, passenger.flags);
        AppendU8(entry, uint8(passenger.seat));
        AppendU16(entry, 0);
    }
    return PackEntries("WMV1", mapId, instanceId, serverTimeMs, entries);
}

std::vector<std::vector<uint8>> BuildUnitAuraDatagrams(uint32 mapId, uint32 instanceId, uint32 serverTimeMs,
                                                       std::vector<StreamUnitAuras> const& units)
{
    std::vector<std::vector<uint8>> entries;
    entries.reserve(units.size());
    for (StreamUnitAuras const& unit : units)
    {
        std::vector<uint8>& entry = entries.emplace_back();
        entry.reserve(8 + unit.spellIds.size() * 4);
        AppendU32(entry, unit.guid);
        AppendU8(entry, unit.creature ? 1 : 0);
        AppendU8(entry, uint8(unit.spellIds.size()));
        AppendU16(entry, 0);
        for (uint32 spellId : unit.spellIds)
            AppendU32(entry, spellId);
    }
    return PackEntries("WMU1", mapId, instanceId, serverTimeMs, entries);
}

std::vector<std::vector<uint8>> BuildCombatEventDatagrams(uint32 mapId, uint32 instanceId, uint64 serverTimeMs,
                                                          std::vector<StreamCombatEvent> const& events,
                                                          uint32 droppedEvents)
{
    std::vector<std::vector<uint8>> entries;
    entries.reserve(events.size());
    for (StreamCombatEvent const& event : events)
    {
        std::vector<uint8>& entry = entries.emplace_back();
        entry.reserve(44);
        AppendU8(entry, event.kind);
        AppendU8(entry, event.flags);
        AppendU8(entry, event.schoolMask);
        AppendU8(entry, event.auraCategory);
        AppendU32(entry, event.sourceGuid);
        AppendU32(entry, event.targetGuid);
        AppendU32(entry, event.spellId);
        AppendU32(entry, event.value);
        AppendF32(entry, event.missileSpeed);
        AppendF32(entry, event.radius);
        AppendF32(entry, event.x);
        AppendF32(entry, event.y);
        AppendF32(entry, event.z);
        AppendU16(entry, uint16(std::min<uint64>(serverTimeMs - std::min(event.atMs, serverTimeMs), 65535)));
        AppendU16(entry, event.extraFlags);
    }
    std::vector<std::vector<uint8>> datagrams = PackEntries("WME1", mapId, instanceId, uint32(serverTimeMs), entries);
    if (!datagrams.empty())
    {
        uint16 const dropped = uint16(std::min<uint32>(droppedEvents, 65535));
        datagrams.front()[22] = uint8(dropped);
        datagrams.front()[23] = uint8(dropped >> 8);
    }
    return datagrams;
}

ItemTemplate const* GetItemTemplate(uint32 itemEntry)
{
    return itemEntry ? sObjectMgr->GetItemTemplate(itemEntry) : nullptr;
}

#ifdef MOD_PLAYERBOTS
// The latest actions a bot has carried out, oldest first. Recorders on the
// bot's engines append to it, normally from the bot's map thread (the one that
// streams it), but playerbots can run actions from the world thread too.
struct BotActionLog
{
    std::mutex mutex;
    struct Entry
    {
        std::string name;
        float relevance;
        uint64 atMs;
        uint16 repeats;
    };
    std::deque<Entry> entries;
    uint32 revision = 0;
};

// Listens to one bot engine without changing what it does. The engine owns
// and deletes it, which expires `alive`.
class BotActionRecorder : public ActionExecutionListener
{
public:
    BotActionRecorder(std::shared_ptr<BotActionLog> log, std::shared_ptr<bool> alive)
        : _log(std::move(log)), _alive(std::move(alive))
    {
    }

    bool Before(Action* /*action*/, Event /*event*/) override { return true; }
    bool AllowExecution(Action* /*action*/, Event /*event*/) override { return true; }
    bool OverrideResult(Action* /*action*/, bool executed, Event /*event*/) override { return executed; }

    void After(Action* action, bool executed, Event /*event*/) override
    {
        if (!executed || !action)
            return;
        std::string const name = action->getName();
        uint64 const nowMs = GameTime::GetGameTimeMS().count();
        std::lock_guard<std::mutex> lock(_log->mutex);
        ++_log->revision;
        if (!_log->entries.empty() && _log->entries.back().name == name)
        {
            BotActionLog::Entry& last = _log->entries.back();
            last.relevance = action->getRelevance();
            last.atMs = nowMs;
            if (last.repeats < UINT16_MAX)
                ++last.repeats;
            return;
        }
        _log->entries.push_back({name, action->getRelevance(), nowMs, 1});
        if (_log->entries.size() > BOT_ACTION_HISTORY)
            _log->entries.pop_front();
    }

private:
    std::shared_ptr<BotActionLog> _log;
    std::shared_ptr<bool> _alive;
};

// Each streamed bot's action log, and the engines recording into it. Bots on
// different maps update on different threads, hence the mutex.
struct BotActionTracking
{
    std::shared_ptr<BotActionLog> log = std::make_shared<BotActionLog>();
    std::vector<std::pair<Engine const*, std::weak_ptr<bool>>> engines;
};
std::mutex botActionTrackingMutex;
std::unordered_map<uint32, BotActionTracking> botActionTracking;

// Returns the bot's action log, first adding a recorder to `engine` (its
// current one) if it has none. A bot's engines change with combat, so each
// gets one as it comes into use.
std::shared_ptr<BotActionLog> TrackBotActions(uint32 guid, Engine* engine)
{
    std::lock_guard<std::mutex> lock(botActionTrackingMutex);
    BotActionTracking& tracking = botActionTracking[guid];
    std::erase_if(tracking.engines, [](auto const& attached) { return attached.second.expired(); });
    bool const recorded = std::any_of(tracking.engines.begin(), tracking.engines.end(),
                                      [&](auto const& attached) { return attached.first == engine; });
    if (engine && !recorded)
    {
        auto alive = std::make_shared<bool>(true);
        engine->AddActionExecutionListener(new BotActionRecorder(tracking.log, alive));
        tracking.engines.emplace_back(engine, alive);
    }
    return tracking.log;
}

// Forgets a bot's action log when it logs out; its engines, and the
// recorders on them, go with it.
void ForgetBotActions(uint32 guid)
{
    std::lock_guard<std::mutex> lock(botActionTrackingMutex);
    botActionTracking.erase(guid);
}
#endif

// Collects the creatures in a map's object store (spawns, summons, and pets).
class StreamGameObjectCollector
{
public:
    explicit StreamGameObjectCollector(std::vector<GameObject*>& objects) : _objects(objects) {}

    void Visit(std::unordered_map<ObjectGuid, GameObject*>& objectMap)
    {
        for (auto const& [guid, object] : objectMap)
            _objects.push_back(object);
    }

    template <class T>
    void Visit(std::unordered_map<ObjectGuid, T*>&)
    {
    }

private:
    std::vector<GameObject*>& _objects;
};

class StreamCreatureCollector
{
public:
    explicit StreamCreatureCollector(std::vector<Creature*>& creatures) : _creatures(creatures) {}

    void Visit(std::unordered_map<ObjectGuid, Creature*>& creatureMap)
    {
        for (auto const& [guid, creature] : creatureMap)
            _creatures.push_back(creature);
    }

    template <class T>
    void Visit(std::unordered_map<ObjectGuid, T*>&)
    {
    }

private:
    std::vector<Creature*>& _creatures;
};

// The waypoints still ahead of a unit moving under server control (bots, and
// players while feared, charmed, or scripted), ending at its destination; empty
// when it is standing still, steering itself, or on a flight path. Long paths
// keep their first points and the destination, and set `truncated`.
std::vector<G3D::Vector3> GetRemainingPath(Unit* unit, bool& truncated)
{
    truncated = false;
    Movement::MoveSpline const* moveSpline = unit->movespline;
    if (!moveSpline || !moveSpline->Initialized() || moveSpline->Finalized() || unit->IsInFlight())
        return {};
    Movement::MoveSpline::MySpline const& spline = moveSpline->_Spline();
    int32 const first = moveSpline->_currentSplineIdx() + 1;
    int32 const last = spline.last();
    if (first > last)
        return {};

    std::vector<G3D::Vector3> points;
    int32 const count = last - first + 1;
    int32 const kept = std::min<int32>(count, int32(STREAM_MAX_PATH_POINTS));
    truncated = kept < count;
    points.reserve(kept);
    for (int32 index = first; index < first + kept - (truncated ? 1 : 0); ++index)
        points.push_back(spline.getPoint(index));
    if (truncated)
        points.push_back(spline.getPoint(last));
    return points;
}

// One non-blocking UDP socket shared by every map thread. Sends are a single
// kernel copy each, so a mutex is cheaper than handing off to another thread.
class StreamSender
{
public:
    void Send(std::vector<std::vector<uint8>> const& datagrams, std::string_view host, uint32 port)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_socket || host != _host || port != _port)
            Open(host, port);
        if (!_socket)
            return;
        for (std::vector<uint8> const& datagram : datagrams)
        {
            boost::system::error_code error;
            _socket->send_to(boost::asio::buffer(datagram), _endpoint, 0, error);
        }
    }

private:
    void Open(std::string_view host, uint32 port)
    {
        _host = host;
        _port = port;
        _socket.reset();
        boost::system::error_code error;
        boost::asio::ip::address const address = boost::asio::ip::make_address(std::string(host), error);
        if (error || !port || port > 65535)
        {
            LOG_ERROR("module", "WoWMin telemetry stream: invalid destination {}:{}", host, port);
            return;
        }
        _endpoint = boost::asio::ip::udp::endpoint(address, uint16(port));
        _socket.emplace(_context);
        _socket->open(_endpoint.protocol(), error);
        if (!error)
            _socket->non_blocking(true, error);
        if (error)
        {
            LOG_ERROR("module", "WoWMin telemetry stream: could not open UDP socket: {}", error.message());
            _socket.reset();
        }
    }

    std::mutex _mutex;
    boost::asio::io_context _context;
    std::optional<boost::asio::ip::udp::socket> _socket;
    boost::asio::ip::udp::endpoint _endpoint;
    std::string _host;
    uint32 _port = 0;
};

StreamSender streamSender;

struct TelemetryWorldState
{
    int32 id;
    int32 value;
};

struct TelemetryBattleground
{
    uint32 mapId;
    uint32 instanceId;
    uint32 battlegroundTypeId;
    uint32 status;
    uint32 elapsedMs;
    int32 remainingMs;
    uint32 winner;
    uint32 allianceScore;
    uint32 hordeScore;
    uint32 alliancePlayers;
    uint32 hordePlayers;
    uint32 allianceAlive;
    uint32 hordeAlive;
    uint32 nextResurrectMs;
    int32 allianceStrategy;
    int32 hordeStrategy;
    std::vector<TelemetryWorldState> worldStates;
};

std::unordered_map<uint64, TelemetryBattleground> battlegroundStates;

struct TelemetryBoss
{
    uint32 id;
    uint32 state;
    std::string name;
};

struct TelemetryInstance
{
    uint32 mapId;
    uint32 instanceId;
    uint32 completedEncounterMask;
    std::vector<TelemetryBoss> bosses;
};

struct TelemetryDeath
{
    uint32 mapId;
    uint32 instanceId;
    uint64 eventId;
    uint64 occurredAt;
    uint32 victimGuid;
    std::string victimName;
    uint32 killerGuid;
    uint32 killerType;
    std::string killerName;
};

struct TelemetryEvent
{
    uint32 mapId;
    uint32 instanceId;
    uint64 eventId;
    uint64 occurredAt;
    std::string type;
    uint32 actorGuid;
    std::string actorName;
    uint32 targetGuid;
    uint32 targetType;
    std::string targetName;
    uint32 valueId;
    std::string valueName;
    uint32 amount;
};

std::unordered_map<uint64, TelemetryInstance> instanceStates;
std::unordered_map<uint64, std::deque<TelemetryDeath>> deathHistory;
std::unordered_map<uint64, std::deque<TelemetryEvent>> eventHistory;

void AddTelemetryEvent(Player* actor, std::string type, Unit const* target, uint32 valueId, std::string valueName,
                       uint32 amount)
{
    if (!actor || !actor->GetInstanceId())
        return;

    TelemetryEvent event{actor->GetMapId(),
                         actor->GetInstanceId(),
                         0,
                         static_cast<uint64>(GameTime::GetGameTime().count()),
                         std::move(type),
                         actor->GetGUID().GetCounter(),
                         actor->GetName(),
                         target ? target->GetGUID().GetCounter() : 0,
                         target ? target->GetTypeId() : TYPEID_OBJECT,
                         target ? target->GetName() : "",
                         valueId,
                         std::move(valueName),
                         amount};
    std::lock_guard<std::mutex> lock(telemetryStateMutex);
    event.eventId = nextSessionEventId++;
    std::deque<TelemetryEvent>& events = eventHistory[GetSessionKey(event.mapId, event.instanceId)];
    events.push_back(std::move(event));
    if (events.size() > MAX_EVENT_HISTORY)
        events.pop_front();
}

struct TelemetryPlayer
{
    std::string name;
    uint32 mapId;
    uint32 instanceId;
    float positionX;
    float positionY;
    float positionZ;
    float orientation;
    uint32 level;
    uint32 race;
    uint32 playerClass;
    uint32 accountId;
    bool isBot;
    bool alive;
    bool inCombat;
    uint32 mapType;
    uint32 difficulty;
    uint64 sessionStartedAt;
    uint32 groupId;
    bool isRaidGroup;
    uint32 subgroup;
    uint32 teamId;
    float healthPct;
    int32 powerType;
    float powerPct;
    uint32 targetGuid;
    uint32 targetType;
    std::string targetName;
    uint32 roleMask;
    bool waitingForResurrect;
    int32 battlegroundRole;
    int32 wmoGroupId;
    uint32 gender;
    uint32 stateFlags;
};

class wowmin_telemetry_commandscript : public CommandScript
{
public:
    wowmin_telemetry_commandscript() : CommandScript("wowmin_telemetry_commandscript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable wowminCommandTable = {
            {"telemetry", HandleTelemetryCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"stream", HandleStreamCommand, SEC_ADMINISTRATOR, Console::Yes},
        };

        static ChatCommandTable commandTable = {
            {"wowmin", wowminCommandTable},
        };

        return commandTable;
    }

    // Subscribes (or renews) a session's position stream for the next
    // STREAM_SUBSCRIPTION_TTL_MS. Replies "WSTREAM|<version>|<port>|<intervalMs>|<ttlMs>"
    // or "WSTREAM|disabled".
    static bool HandleStreamCommand(ChatHandler* handler, char const* args)
    {
        if (!telemetryConfig.GetConfigValue<bool>(WowMinTelemetryConfig::Enabled) ||
            !telemetryConfig.GetConfigValue<bool>(WowMinTelemetryConfig::StreamEnabled))
        {
            handler->SendSysMessage("WSTREAM|disabled");
            return true;
        }

        std::istringstream arguments(args ? args : "");
        std::string mapArgument;
        std::string instanceArgument;
        std::string unexpectedArgument;
        arguments >> mapArgument >> instanceArgument;
        auto const mapId = Acore::StringTo<uint32>(mapArgument);
        auto const instanceId = Acore::StringTo<uint32>(instanceArgument);
        if (!mapId || !instanceId || !*instanceId || arguments >> unexpectedArgument)
        {
            handler->SendSysMessage("Usage: wowmin stream <mapId> <instanceId>");
            return false;
        }

        uint64 const expiresAtMs = GameTime::GetGameTimeMS().count() + STREAM_SUBSCRIPTION_TTL_MS;
        {
            std::lock_guard<std::mutex> lock(streamStateMutex);
            uint64 const sessionKey = GetSessionKey(*mapId, *instanceId);
            auto const iterator = streamSubscriptions.find(sessionKey);
            if (iterator != streamSubscriptions.end())
                iterator->second.expiresAtMs = expiresAtMs;
            else if (streamSubscriptions.size() < STREAM_MAX_SUBSCRIPTIONS)
            {
                streamSubscriptions.emplace(
                    sessionKey, StreamSubscription{expiresAtMs, 0, 0, std::make_shared<StreamSessionState>()});
                streamAnySubscribed = true;
            }
            else
            {
                handler->SendSysMessage("WSTREAM|full");
                return true;
            }
        }
        handler->PSendSysMessage("WSTREAM|{}|{}|{}|{}", STREAM_VERSION,
                                 telemetryConfig.GetConfigValue<uint32>(WowMinTelemetryConfig::StreamPort),
                                 std::max(STREAM_MIN_INTERVAL_MS, telemetryConfig.GetConfigValue<uint32>(
                                                                      WowMinTelemetryConfig::StreamIntervalMs)),
                                 STREAM_SUBSCRIPTION_TTL_MS);
        return true;
    }

    static bool HandleTelemetryCommand(ChatHandler* handler, char const* args)
    {
        if (!telemetryConfig.GetConfigValue<bool>(WowMinTelemetryConfig::Enabled))
        {
            handler->SendSysMessage(
                "WoWMin telemetry is disabled. Set "
                "WowMinTelemetry.Enable = 1 and reload config.");
            return true;
        }

        std::istringstream arguments(args ? args : "");
        std::string mapArgument;
        std::string instanceArgument;
        std::string unexpectedArgument;
        bool const filterMap = bool(arguments >> mapArgument);
        bool const filterInstance = bool(arguments >> instanceArgument);
        auto const mapId = filterMap ? Acore::StringTo<uint32>(mapArgument) : std::optional<uint32>(0);
        auto const instanceId = filterInstance ? Acore::StringTo<uint32>(instanceArgument) : std::optional<uint32>(0);
        if (!mapId || !instanceId || arguments >> unexpectedArgument)
        {
            handler->SendSysMessage("Usage: wowmin telemetry [mapId] [instanceId]");
            return false;
        }

        std::vector<TelemetryPlayer> players;
        std::unordered_set<uint64> activeSessionKeys;
        {
            std::shared_lock<std::shared_mutex> playerLock(*HashMapHolder<Player>::GetLock());
            for (auto const& playerEntry : ObjectAccessor::GetPlayers())
            {
                Player* player = playerEntry.second;
                if (!player || !player->IsInWorld())
                    continue;
                if (filterMap && player->GetMapId() != *mapId)
                    continue;
                if (filterInstance && player->GetInstanceId() != *instanceId)
                    continue;

                WorldSession* session = player->GetSession();
                if (!session)
                    continue;

                Map* map = player->GetMap();
                Group* group = player->GetGroup();
                Unit* target = player->GetSelectedUnit();
                Powers const powerType = player->getPowerType();
                int32 battlegroundRole = -1;
                int32 const wmoGroupId = GetUnitWmoGroupId(map, player);
#ifdef MOD_PLAYERBOTS
                if (session->IsHeadless() && map->IsBattlegroundOrArena())
                    battlegroundRole =
                        GetCachedBattlegroundRole(player->GetMapId(), player->GetInstanceId(), player->GetGUID());
#endif
                if (player->GetInstanceId())
                    activeSessionKeys.insert(GetSessionKey(player->GetMapId(), player->GetInstanceId()));
                uint32 const stateFlags = GetPlayerStateFlags(player);
                players.push_back({player->GetName(),
                                   player->GetMapId(),
                                   player->GetInstanceId(),
                                   player->GetPositionX(),
                                   player->GetPositionY(),
                                   player->GetPositionZ(),
                                   player->GetOrientation(),
                                   player->GetLevel(),
                                   player->getRace(),
                                   player->getClass(),
                                   session->GetAccountId(),
                                   session->IsHeadless(),
                                   player->IsAlive(),
                                   player->IsInCombat(),
                                   map->GetEntry()->map_type,
                                   map->GetDifficulty(),
                                   GetSessionStartedAt(player->GetMapId(), player->GetInstanceId()),
                                   group ? group->GetGUID().GetCounter() : 0,
                                   group && group->isRaidGroup(),
                                   group ? group->GetMemberGroup(player->GetGUID()) : 0,
                                   player->GetTeamId(),
                                   player->GetHealthPct(),
                                   powerType,
                                   player->GetPowerPct(powerType),
                                   target ? target->GetGUID().GetCounter() : 0,
                                   target ? target->GetTypeId() : TYPEID_OBJECT,
                                   target ? target->GetName() : "",
                                   GetGroupRole(group, player->GetGUID()),
                                   player->HasAura(SPELL_WAITING_FOR_RESURRECT),
                                   battlegroundRole,
                                   wmoGroupId,
                                   player->getGender(),
                                   stateFlags});
            }
        }

        std::vector<TelemetryBattleground> battlegrounds;
        {
            std::lock_guard<std::mutex> lock(telemetryStateMutex);
            for (auto const& [sessionKey, battleground] : battlegroundStates)
                if ((!filterMap || battleground.mapId == *mapId) &&
                    (!filterInstance || battleground.instanceId == *instanceId))
                    activeSessionKeys.insert(sessionKey);
            for (uint64 sessionKey : activeSessionKeys)
                if (auto const iterator = battlegroundStates.find(sessionKey); iterator != battlegroundStates.end())
                    battlegrounds.push_back(iterator->second);
        }

        std::vector<TelemetryInstance> instances;
        std::vector<TelemetryDeath> deaths;
        std::vector<TelemetryEvent> events;
        {
            std::lock_guard<std::mutex> lock(telemetryStateMutex);
            for (auto const& [sessionKey, instance] : instanceStates)
                if ((!filterMap || instance.mapId == *mapId) && (!filterInstance || instance.instanceId == *instanceId))
                    activeSessionKeys.insert(sessionKey);
            for (uint64 sessionKey : activeSessionKeys)
            {
                if (auto const iterator = instanceStates.find(sessionKey); iterator != instanceStates.end())
                    instances.push_back(iterator->second);
                if (auto const iterator = deathHistory.find(sessionKey); iterator != deathHistory.end())
                    deaths.insert(deaths.end(), iterator->second.begin(), iterator->second.end());
                if (auto const iterator = eventHistory.find(sessionKey); iterator != eventHistory.end())
                    events.insert(events.end(), iterator->second.begin(), iterator->second.end());
            }
        }

        handler->SendSysMessage("WMAP_VERSION|7");
        for (TelemetryPlayer const& player : players)
        {
            handler->PSendSysMessage(
                "WMAP|{}|{}|{}|{:.3f}|{:.3f}|{:.3f}|{:.3f}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{:.1f}|{}|"
                "{:.1f}|{}|{}|{}|{}|{}|{}|{}|{}|{}",
                EscapeTelemetryField(player.name), player.mapId, player.instanceId, player.positionX, player.positionY,
                player.positionZ, player.orientation, player.level, player.race, player.playerClass, player.accountId,
                player.isBot ? 1 : 0, player.alive ? 1 : 0, player.inCombat ? 1 : 0, player.mapType, player.difficulty,
                player.sessionStartedAt, player.groupId, player.isRaidGroup ? 1 : 0, player.subgroup, player.teamId,
                player.healthPct, player.powerType, player.powerPct, player.targetGuid, player.targetType,
                EscapeTelemetryField(player.targetName), player.roleMask, player.waitingForResurrect ? 1 : 0,
                player.battlegroundRole, player.wmoGroupId, player.gender, player.stateFlags);
        }

        std::size_t worldStateCount = 0;
        for (TelemetryBattleground const& battleground : battlegrounds)
        {
            handler->PSendSysMessage(
                "WBG|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}", battleground.mapId, battleground.instanceId,
                battleground.battlegroundTypeId, battleground.status, battleground.elapsedMs, battleground.remainingMs,
                battleground.winner, battleground.allianceScore, battleground.hordeScore, battleground.alliancePlayers,
                battleground.hordePlayers, battleground.allianceAlive, battleground.hordeAlive,
                battleground.nextResurrectMs, battleground.allianceStrategy, battleground.hordeStrategy);
            for (TelemetryWorldState const& worldState : battleground.worldStates)
            {
                handler->PSendSysMessage("WOBJ|{}|{}|{}|{}", battleground.mapId, battleground.instanceId, worldState.id,
                                         worldState.value);
                ++worldStateCount;
            }
        }
        std::size_t bossCount = 0;
        for (TelemetryInstance const& instance : instances)
        {
            handler->PSendSysMessage("WINS|{}|{}|{}", instance.mapId, instance.instanceId,
                                     instance.completedEncounterMask);
            for (TelemetryBoss const& boss : instance.bosses)
            {
                handler->PSendSysMessage("WBOSS|{}|{}|{}|{}|{}", instance.mapId, instance.instanceId, boss.id,
                                         boss.state, EscapeTelemetryField(boss.name));
                ++bossCount;
            }
        }
        for (TelemetryDeath const& death : deaths)
            handler->PSendSysMessage("WDEATH|{}|{}|{}|{}|{}|{}|{}|{}|{}", death.mapId, death.instanceId, death.eventId,
                                     death.occurredAt, death.victimGuid, EscapeTelemetryField(death.victimName),
                                     death.killerGuid, death.killerType, EscapeTelemetryField(death.killerName));
        for (TelemetryEvent const& event : events)
            handler->PSendSysMessage("WEVENT|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}", event.mapId, event.instanceId,
                                     event.eventId, event.occurredAt, EscapeTelemetryField(event.type), event.actorGuid,
                                     EscapeTelemetryField(event.actorName), event.targetGuid, event.targetType,
                                     EscapeTelemetryField(event.targetName), event.valueId,
                                     EscapeTelemetryField(event.valueName), event.amount);
        handler->PSendSysMessage("WMAP_END|{}|{}|{}|{}|{}|{}|{}", players.size(), battlegrounds.size(), worldStateCount,
                                 instances.size(), bossCount, deaths.size(), events.size());
        return true;
    }
};

class wowmin_telemetry_mapscript : public AllMapScript
{
public:
    wowmin_telemetry_mapscript()
        : AllMapScript("wowmin_telemetry_mapscript",
                       {ALLMAPHOOK_ON_CREATE_MAP, ALLMAPHOOK_ON_DESTROY_MAP, ALLMAPHOOK_ON_MAP_UPDATE})
    {
    }

    void OnCreateMap(Map* map) override
    {
        if (!map->GetInstanceId())
            return;

        std::lock_guard<std::mutex> lock(telemetryStateMutex);
        uint64 const sessionKey = GetSessionKey(map->GetId(), map->GetInstanceId());
        sessionStartedAt[sessionKey] = GameTime::GetGameTime().count();
        mapUpdateTimers[sessionKey] = BATTLEGROUND_UPDATE_INTERVAL;
    }

    void OnDestroyMap(Map* map) override
    {
        if (!map->GetInstanceId())
            return;

        std::lock_guard<std::mutex> lock(telemetryStateMutex);
        uint64 const sessionKey = GetSessionKey(map->GetId(), map->GetInstanceId());
        sessionStartedAt.erase(sessionKey);
        mapUpdateTimers.erase(sessionKey);
        battlegroundStates.erase(sessionKey);
#ifdef MOD_PLAYERBOTS
        battlegroundRoles.erase(sessionKey);
#endif
        instanceStates.erase(sessionKey);
        deathHistory.erase(sessionKey);
        eventHistory.erase(sessionKey);

        std::lock_guard<std::mutex> streamLock(streamStateMutex);
        streamSubscriptions.erase(sessionKey);
        streamAnySubscribed = !streamSubscriptions.empty();
    }

    void OnMapUpdate(Map* map, uint32 diff) override
    {
        if (!map->GetInstanceId())
            return;

        uint64 const sessionKey = GetSessionKey(map->GetId(), map->GetInstanceId());
        if (telemetryConfig.GetConfigValue<bool>(WowMinTelemetryConfig::Enabled) &&
            telemetryConfig.GetConfigValue<bool>(WowMinTelemetryConfig::StreamEnabled))
            StreamPositions(map, sessionKey, diff);
        {
            std::lock_guard<std::mutex> lock(telemetryStateMutex);
            uint32& updateTimer = mapUpdateTimers[sessionKey];
            updateTimer += diff;
            if (updateTimer < BATTLEGROUND_UPDATE_INTERVAL)
                return;
            updateTimer %= BATTLEGROUND_UPDATE_INTERVAL;
        }

        if (BattlegroundMap* battlegroundMap = map->ToBattlegroundMap())
            CaptureBattleground(battlegroundMap, sessionKey);
        if (map->IsDungeon())
            CaptureInstance(map, sessionKey);
    }

private:
    // Runs on the map's own update thread, so its players can be read directly.
    static void StreamPositions(Map* map, uint64 sessionKey, uint32 diff)
    {
        uint32 const intervalMs = std::max(
            STREAM_MIN_INTERVAL_MS, telemetryConfig.GetConfigValue<uint32>(WowMinTelemetryConfig::StreamIntervalMs));
        uint64 const nowMs = GameTime::GetGameTimeMS().count();
        uint32 sequence = 0;
        std::shared_ptr<StreamSessionState> state;
        {
            std::lock_guard<std::mutex> lock(streamStateMutex);
            auto const iterator = streamSubscriptions.find(sessionKey);
            if (iterator == streamSubscriptions.end())
                return;
            if (iterator->second.expiresAtMs < nowMs)
            {
                streamSubscriptions.erase(iterator);
                streamAnySubscribed = !streamSubscriptions.empty();
                return;
            }
            iterator->second.elapsedMs += diff;
            if (iterator->second.elapsedMs < intervalMs)
                return;
            // Drop missed intervals rather than sending a burst after a slow tick.
            iterator->second.elapsedMs = 0;
            sequence = ++iterator->second.sequence;
            state = iterator->second.state;
        }

        std::vector<StreamPlayer> players;
        std::vector<StreamPath> paths;
        std::vector<StreamBot> bots;
        std::vector<StreamAppearance> appearances;
        std::vector<StreamUnitAuras> unitAuras;
        std::vector<StreamUnitDebuffs> debuffs;
        std::vector<StreamPassenger> passengers;
        for (auto const& playerReference : map->GetPlayers())
        {
            Player* player = playerReference.GetSource();
            if (!player || !player->IsInWorld())
                continue;
            Powers const powerType = player->getPowerType();
            uint8 flags = 0;
            if (player->IsAlive())
                flags |= 1;
            if (player->IsInCombat())
                flags |= 2;
            if (player->HasAura(SPELL_WAITING_FOR_RESURRECT))
                flags |= 4;
            players.push_back({player->GetGUID().GetCounter(), player->GetPositionX(), player->GetPositionY(),
                               player->GetPositionZ(), player->GetOrientation(),
                               uint8(std::clamp(player->GetHealthPct(), 0.0f, 100.0f)),
                               uint8(std::clamp(player->GetPowerPct(powerType), 0.0f, 100.0f)), int8(powerType), flags,
                               uint8(GetPlayerStateFlags(player)), GetUnitWmoGroupId(map, player), player->GetName()});
            CollectPathChange(player, player->GetGUID().GetCounter(), false, state->playerPaths, nowMs, paths);
            CollectPassenger(player, passengers);
            CollectAppearance(player, *state, nowMs, appearances);
            CollectAuras(player, *state, nowMs, unitAuras);
            CollectDebuffs(player, *state, nowMs, debuffs);
#ifdef MOD_PLAYERBOTS
            if (player->GetSession() && player->GetSession()->IsHeadless())
                CollectBot(map, player, *state, nowMs, bots);
#endif
        }
        // Forget players who have left, so a return is sent as new.
        for (auto iterator = state->playerPaths.begin(); iterator != state->playerPaths.end();)
        {
            bool const present = std::any_of(players.begin(), players.end(), [&](StreamPlayer const& player)
                                             { return player.guid == iterator->first; });
            iterator = present ? std::next(iterator) : state->playerPaths.erase(iterator);
        }
        std::erase_if(state->bots,
                      [&](auto const& entry)
                      {
                          return std::none_of(players.begin(), players.end(),
                                              [&](StreamPlayer const& player) { return player.guid == entry.first; });
                      });
        std::vector<StreamCreature> creatures;
        CollectCreatures(map, *state, nowMs, creatures, paths, appearances, unitAuras, debuffs);
        std::vector<StreamMarker> markers;
        bool const sendMarkers = CollectMarkers(map, *state, nowMs, markers);
        std::vector<StreamGameObject> gameObjects;
        CollectGameObjects(map, *state, nowMs, gameObjects);
        // Forget units that have gone, so a return is sent as new.
        auto gone = [&](auto const& entry)
        {
            if (entry.first >> 32)
                return !state->creatures.contains(uint32(entry.first));
            return std::none_of(players.begin(), players.end(),
                                [&](StreamPlayer const& player) { return player.guid == uint32(entry.first); });
        };
        std::erase_if(state->appearances, gone);
        std::erase_if(state->auras, gone);
        std::erase_if(state->debuffs, gone);

        std::string_view const host = telemetryConfig.GetConfigValue(WowMinTelemetryConfig::StreamHost);
        uint32 const port = telemetryConfig.GetConfigValue<uint32>(WowMinTelemetryConfig::StreamPort);
        streamSender.Send(BuildStreamDatagrams(map->GetId(), map->GetInstanceId(), sequence, uint32(nowMs), players),
                          host, port);
        if (!creatures.empty())
            streamSender.Send(BuildCreatureDatagrams(map->GetId(), map->GetInstanceId(), uint32(nowMs), creatures),
                              host, port);
        if (!paths.empty())
            streamSender.Send(BuildPathDatagrams(map->GetId(), map->GetInstanceId(), uint32(nowMs), paths), host, port);
        if (!bots.empty())
            streamSender.Send(BuildBotDatagrams(map->GetId(), map->GetInstanceId(), uint32(nowMs), bots), host, port);
        if (!appearances.empty())
            streamSender.Send(BuildAppearanceDatagrams(map->GetId(), map->GetInstanceId(), uint32(nowMs), appearances),
                              host, port);
        if (!unitAuras.empty())
            streamSender.Send(BuildUnitAuraDatagrams(map->GetId(), map->GetInstanceId(), uint32(nowMs), unitAuras),
                              host, port);
        if (!passengers.empty())
            streamSender.Send(BuildPassengerDatagrams(map->GetId(), map->GetInstanceId(), uint32(nowMs), passengers),
                              host, port);
        if (!gameObjects.empty())
            streamSender.Send(BuildGameObjectDatagrams(map->GetId(), map->GetInstanceId(), uint32(nowMs), gameObjects),
                              host, port);
        if (!debuffs.empty())
            streamSender.Send(BuildDebuffDatagrams(map->GetId(), map->GetInstanceId(), uint32(nowMs), debuffs), host,
                              port);
        if (sendMarkers)
            streamSender.Send(BuildMarkerDatagrams(map->GetId(), map->GetInstanceId(), uint32(nowMs), markers), host,
                              port);
        std::vector<StreamCombatEvent> events;
        uint32 droppedEvents = 0;
        {
            std::lock_guard<std::mutex> lock(state->eventMutex);
            events.swap(state->events);
            std::swap(droppedEvents, state->droppedEvents);
        }
        if (!events.empty())
            streamSender.Send(
                BuildCombatEventDatagrams(map->GetId(), map->GetInstanceId(), nowMs, events, droppedEvents), host,
                port);
    }

    // What the unit wears: the visible entry for players, so transmogrification
    // shows; creatures only have weapon slots.
    // The item entries a unit shows: a player's visible items, a creature's
    // virtual weapons. Plain update fields, so they are read every tick and
    // a gear change is seen without equip hooks.
    static std::array<uint32, EQUIPMENT_SLOT_END> ShownItemEntries(Unit* unit)
    {
        std::array<uint32, EQUIPMENT_SLOT_END> entries{};
        for (uint8 slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (unit->IsPlayer())
                entries[slot] = unit->GetUInt32Value(PLAYER_VISIBLE_ITEM_1_ENTRYID + slot * 2);
            else if (slot >= EQUIPMENT_SLOT_MAINHAND && slot <= EQUIPMENT_SLOT_RANGED)
                entries[slot] = unit->GetUInt32Value(UNIT_VIRTUAL_ITEM_SLOT_ID + slot - EQUIPMENT_SLOT_MAINHAND);
        }
        return entries;
    }

    static StreamGear ReadGear(std::array<uint32, EQUIPMENT_SLOT_END> const& entries)
    {
        StreamGear gear;
        for (uint8 slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            ItemTemplate const* itemTemplate = GetItemTemplate(entries[slot]);
            if (!itemTemplate)
                continue;
            gear.items[slot] = itemTemplate->DisplayInfoID;
            if (slot == EQUIPMENT_SLOT_MAINHAND)
            {
                gear.mainHandType = uint8(itemTemplate->InventoryType);
                gear.mainHandSubclass = uint8(itemTemplate->SubClass);
            }
            else if (slot == EQUIPMENT_SLOT_OFFHAND)
                gear.offHandType = uint8(itemTemplate->InventoryType);
            else if (slot == EQUIPMENT_SLOT_RANGED)
                gear.rangedSubclass = uint8(itemTemplate->SubClass);
        }
        return gear;
    }

    // Adds how the unit looks to `appearances` when that has changed, or is
    // due a refresh. Display, sheath, and scale are read every tick (they
    // change in combat); gear comes from the session's cache.
    static void CollectAppearance(Unit* unit, StreamSessionState& state, uint64 nowMs,
                                  std::vector<StreamAppearance>& appearances)
    {
        StreamAppearance appearance;
        appearance.guid = unit->GetGUID().GetCounter();
        appearance.displayId = unit->GetDisplayId();
        appearance.nativeDisplayId = unit->GetNativeDisplayId();
        appearance.mountDisplayId = unit->GetMountID();
        appearance.sheathState = uint8(unit->GetSheath());
        appearance.scale = uint16(std::clamp(unit->GetObjectScale() * 1000.0f, 0.0f, 65535.0f));
        appearance.race = unit->getRace();
        appearance.gender = unit->getGender();
        appearance.unitClass = unit->getClass();
        if (Player* player = unit->ToPlayer())
        {
            appearance.skin = player->GetByteValue(PLAYER_BYTES, PLAYER_BYTES_OFFSET_SKIN_ID);
            appearance.face = player->GetByteValue(PLAYER_BYTES, PLAYER_BYTES_OFFSET_FACE_ID);
            appearance.hairStyle = player->GetByteValue(PLAYER_BYTES, PLAYER_BYTES_OFFSET_HAIR_STYLE_ID);
            appearance.hairColor = player->GetByteValue(PLAYER_BYTES, PLAYER_BYTES_OFFSET_HAIR_COLOR_ID);
            appearance.facialStyle = player->GetByteValue(PLAYER_BYTES_2, PLAYER_BYTES_2_OFFSET_FACIAL_STYLE);
            if (player->HasPlayerFlag(PLAYER_FLAGS_HIDE_HELM))
                appearance.flags |= 2;
            if (player->HasPlayerFlag(PLAYER_FLAGS_HIDE_CLOAK))
                appearance.flags |= 4;
        }
        else
            appearance.flags |= 1;
        uint64 const key = appearance.guid | (unit->IsPlayer() ? 0 : uint64(1) << 32);
        auto [iterator, inserted] = state.appearances.try_emplace(key);
        StreamSessionState::SentAppearance& sent = iterator->second;
        std::array<uint32, EQUIPMENT_SLOT_END> const entries = ShownItemEntries(unit);
        if (sent.gearEntries != entries)
        {
            sent.gear = ReadGear(entries);
            sent.gearEntries = entries;
        }
        appearance.gear = sent.gear;

        std::size_t signature = 0;
        auto mix = [&](uint64 value) { MixSignature(signature, value); };
        mix(appearance.flags);
        mix(appearance.race | appearance.gender << 8 | appearance.unitClass << 16 | appearance.sheathState << 24);
        mix(appearance.skin | appearance.face << 8 | appearance.hairStyle << 16 | appearance.hairColor << 24);
        mix(appearance.facialStyle | appearance.scale << 8);
        mix(appearance.displayId);
        mix(appearance.nativeDisplayId);
        mix(appearance.mountDisplayId);
        for (uint32 item : appearance.gear.items)
            mix(item);
        mix(appearance.gear.mainHandType | appearance.gear.mainHandSubclass << 8 | appearance.gear.offHandType << 16 |
            appearance.gear.rangedSubclass << 24);

        if (!inserted && sent.signature == signature && nowMs - sent.sentAtMs < STREAM_APPEARANCE_REFRESH_MS)
            return;
        sent.signature = signature;
        sent.sentAtMs = nowMs;
        appearances.push_back(appearance);
    }

    // Adds the unit's art auras to `unitAuras` when they have changed, or are
    // due a refresh; a unit with none is sent only as its last one goes.
    // Adds each spawned game object with a model that appeared, changed, or is
    // due a refresh to `objects`, and one marked gone for each that went.
    static void CollectGameObjects(Map* map, StreamSessionState& state, uint64 nowMs,
                                   std::vector<StreamGameObject>& objects)
    {
        std::vector<GameObject*> present;
        StreamGameObjectCollector collector(present);
        TypeContainerVisitor<StreamGameObjectCollector, MapStoredObjectTypesContainer> visitor(collector);
        visitor.Visit(map->GetObjectsStore());

        std::unordered_set<uint32> seen;
        for (GameObject* object : present)
        {
            if (!object->IsInWorld() || !object->isSpawned() || !object->GetDisplayId())
                continue;
            GameobjectTypes const type = object->GetGoType();
            // Spell foci are invisible markers; traps can have a model (the glow
            // under an Arathi Basin banner), so only those without one go.
            if (type == GAMEOBJECT_TYPE_SPELL_FOCUS)
                continue;
            uint32 const guid = object->GetGUID().GetCounter();
            seen.insert(guid);
            G3D::Quat const rotation = object->GetWorldRotation();
            StreamGameObject entry{guid,
                                   object->GetEntry(),
                                   object->GetDisplayId(),
                                   object->GetPositionX(),
                                   object->GetPositionY(),
                                   object->GetPositionZ(),
                                   {rotation.x, rotation.y, rotation.z, rotation.w},
                                   uint16(std::clamp(object->GetObjectScale() * 1000.0f, 0.0f, 65535.0f)),
                                   uint8(type),
                                   uint8(object->GetGoState()),
                                   false};
            std::size_t signature = std::hash<uint32>{}(entry.displayId);
            auto mix = [&](auto value) { MixSignature(signature, value); };
            mix(entry.state);
            mix(entry.scale);
            mix(int32(entry.x * 10.0f));
            mix(int32(entry.y * 10.0f));
            mix(int32(entry.z * 10.0f));
            mix(int32(rotation.w * 1000.0f));
            mix(int32(rotation.z * 1000.0f));
            auto [iterator, inserted] = state.gameObjects.try_emplace(guid);
            StreamSessionState::SentSignature& sent = iterator->second;
            if (!inserted && sent.signature == signature && nowMs - sent.sentAtMs < STREAM_GAMEOBJECT_REFRESH_MS)
                continue;
            sent = {nowMs, signature};
            objects.push_back(entry);
        }
        // Gone since the last tick: sent once, then forgotten.
        for (auto iterator = state.gameObjects.begin(); iterator != state.gameObjects.end();)
        {
            if (seen.contains(iterator->first))
            {
                ++iterator;
                continue;
            }
            StreamGameObject gone;
            gone.guid = iterator->first;
            gone.gone = true;
            objects.push_back(gone);
            iterator = state.gameObjects.erase(iterator);
        }
    }

    // Adds the player to `passengers` if they ride a vehicle.
    static void CollectPassenger(Player* player, std::vector<StreamPassenger>& passengers)
    {
        Vehicle* vehicle = player->GetVehicle();
        Unit* base = vehicle ? vehicle->GetBase() : nullptr;
        if (!base)
            return;
        uint8 flags = base->IsPlayer() ? 0 : 1;
        if (VehicleSeatEntry const* seat = vehicle->GetSeatForPassenger(player))
        {
            if (seat->m_flags & VEHICLE_SEAT_FLAG_HIDE_PASSENGER)
                flags |= 2;
            if (seat->m_flags & VEHICLE_SEAT_FLAG_CAN_CONTROL)
                flags |= 4;
        }
        passengers.push_back(
            {player->GetGUID().GetCounter(), base->GetGUID().GetCounter(), flags, player->GetTransSeat()});
    }

    // Adds the unit's harmful auras to `units` when the list changed (spells or
    // stacks), or is due a refresh while it isn't empty.
    static void CollectDebuffs(Unit* unit, StreamSessionState& state, uint64 nowMs,
                               std::vector<StreamUnitDebuffs>& units)
    {
        StreamUnitDebuffs entry{unit->GetGUID().GetCounter(), !unit->IsPlayer(), {}};
        std::size_t signature = 0;
        for (auto const& [spellId, application] : unit->GetAppliedAuras())
        {
            if (entry.debuffs.size() >= STREAM_MAX_DEBUFFS)
                break;
            if (!application || application->GetSlot() >= MAX_AURAS || application->IsPositive() ||
                !application->GetBase())
                continue;
            Aura const* aura = application->GetBase();
            if (aura->GetSpellInfo()->IsPassive())
                continue;
            int32 const remaining = aura->GetDuration();
            int32 const duration = aura->GetMaxDuration();
            entry.debuffs.push_back({aura->GetId(), uint32(std::max(0, remaining)), uint32(std::max(0, duration)),
                                     aura->GetStackAmount(), aura->GetCasterGUID().IsPlayer()});
            MixSignature(signature, aura->GetId());
            MixSignature(signature, uint32(aura->GetStackAmount()));
        }
        if (!entry.debuffs.empty() && !signature)
            signature = 1;
        uint64 const key = entry.guid | (entry.creature ? uint64(1) << 32 : 0);
        auto [iterator, inserted] = state.debuffs.try_emplace(key);
        StreamSessionState::SentSignature& sent = iterator->second;
        bool const changed = sent.signature != signature;
        bool const due = signature && nowMs - sent.sentAtMs >= STREAM_DEBUFF_REFRESH_MS;
        if ((inserted && !signature) || (!changed && !due))
            return;
        sent = {nowMs, signature};
        units.push_back(std::move(entry));
    }

    // Fills `markers` with the raid markers the session's groups have set;
    // true when they should be sent (changed, or due a refresh while any are).
    static bool CollectMarkers(Map* map, StreamSessionState& state, uint64 nowMs, std::vector<StreamMarker>& markers)
    {
        std::vector<ObjectGuid::LowType> groupIds;
        for (auto const& playerReference : map->GetPlayers())
            if (Player* player = playerReference.GetSource())
                if (Group* group = player->GetGroup())
                    if (std::find(groupIds.begin(), groupIds.end(), group->GetGUID().GetCounter()) == groupIds.end())
                        groupIds.push_back(group->GetGUID().GetCounter());
        std::size_t signature = 0;
        for (GroupMarkers const& groupMarkers : GetGroupMarkers(groupIds, nowMs))
        {
            for (uint8 icon = 0; icon < TARGETICONCOUNT; ++icon)
            {
                ObjectGuid const target = groupMarkers[icon];
                if (!target)
                    continue;
                bool const duplicate =
                    std::any_of(markers.begin(), markers.end(), [&](StreamMarker const& marker)
                                { return marker.icon == icon && marker.guid == target.GetCounter(); });
                if (duplicate)
                    continue;
                markers.push_back({icon, !target.IsPlayer(), target.GetCounter()});
                MixSignature(signature, target.GetRawValue() + icon);
            }
        }
        if (!markers.empty() && !signature)
            signature = 1;
        StreamSessionState::SentSignature& sent = state.markers;
        bool const changed = sent.signature != signature;
        bool const due = signature && nowMs - sent.sentAtMs >= STREAM_MARKER_REFRESH_MS;
        if (!changed && !due)
            return false;
        sent = {nowMs, signature};
        return true;
    }

    static void CollectAuras(Unit* unit, StreamSessionState& state, uint64 nowMs,
                             std::vector<StreamUnitAuras>& unitAuras)
    {
        StreamUnitAuras auras{unit->GetGUID().GetCounter(), !unit->IsPlayer(), {}};
        for (auto const& [spellId, application] : unit->GetAppliedAuras())
        {
            if (auras.spellIds.size() >= STREAM_MAX_UNIT_AURAS)
                break;
            if (!application || application->GetSlot() >= MAX_AURAS || !application->GetBase())
                continue;
            SpellInfo const* spellInfo = application->GetBase()->GetSpellInfo();
            if (!spellInfo || spellInfo->IsPassive() || !spellInfo->SpellVisual[0])
                continue;
            // The same spell from several casters shows once.
            if (std::find(auras.spellIds.begin(), auras.spellIds.end(), spellInfo->Id) == auras.spellIds.end())
                auras.spellIds.push_back(spellInfo->Id);
        }
        std::sort(auras.spellIds.begin(), auras.spellIds.end());
        std::size_t signature = 0;
        for (uint32 spellId : auras.spellIds)
            MixSignature(signature, spellId);
        if (!auras.spellIds.empty() && !signature)
            signature = 1;

        uint64 const key = auras.guid | (auras.creature ? uint64(1) << 32 : 0);
        auto [iterator, inserted] = state.auras.try_emplace(key);
        StreamSessionState::SentSignature& sent = iterator->second;
        bool const changed = sent.signature != signature;
        bool const due = signature && nowMs - sent.sentAtMs >= STREAM_APPEARANCE_REFRESH_MS;
        // A unit first seen with none needs no message.
        if ((inserted && !signature) || (!changed && !due))
            return;
        sent.signature = signature;
        sent.sentAtMs = nowMs;
        unitAuras.push_back(std::move(auras));
    }

#ifdef MOD_PLAYERBOTS
    // Adds what the bot is deciding to `bots` when that has changed, or is due
    // a refresh.
    static void CollectBot(Map* map, Player* player, StreamSessionState& state, uint64 nowMs,
                           std::vector<StreamBot>& bots)
    {
        PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(player);
        AiObjectContext* context = botAI ? botAI->GetAiObjectContext() : nullptr;
        if (!context)
            return;
        uint32 const guid = player->GetGUID().GetCounter();
        std::shared_ptr<BotActionLog> const log = TrackBotActions(guid, botAI->GetCurrentEngine());
        auto [iterator, inserted] = state.bots.try_emplace(guid);
        StreamSessionState::SentSignature& sent = iterator->second;
        if (!inserted && nowMs - sent.sentAtMs < STREAM_BOT_MIN_INTERVAL_MS)
            return;

        StreamBot bot;
        bot.guid = guid;
        BotState const botState = botAI->GetState();
        bot.state = uint8(botState);
        MotionMaster* motionMaster = player->GetMotionMaster();
        if (motionMaster && !motionMaster->empty())
        {
            bot.motionType = uint8(motionMaster->GetCurrentMovementGeneratorType());
            if (auto const* follower = dynamic_cast<AbstractFollower const*>(motionMaster->top()))
                if (Unit* target = follower->GetTarget())
                {
                    bot.motionTargetGuid = target->GetGUID().GetCounter();
                    if (target->IsPlayer())
                        bot.flags |= 2;
                }
        }
        ObjectGuid const target = player->GetTarget();
        bot.targetGuid = target ? target.GetCounter() : 0;
        if (target.IsPlayer())
            bot.flags |= 4;
        uint32 moveSetAt = 0;
        if (Value<LastMovement&>* lastMovement = context->GetValue<LastMovement&>("last movement"))
        {
            LastMovement const& lastMove = lastMovement->Get();
            bot.movePriority = uint8(lastMove.priority);
            if (lastMove.msTime && lastMove.lastMoveToMapId == map->GetId() &&
                (lastMove.lastMoveToX != 0.0f || lastMove.lastMoveToY != 0.0f))
            {
                bot.flags |= 1;
                bot.moveX = lastMove.lastMoveToX;
                bot.moveY = lastMove.lastMoveToY;
                bot.moveZ = lastMove.lastMoveToZ;
                bot.moveAgeMs = getMSTimeDiff(lastMove.msTime, getMSTime());
                moveSetAt = lastMove.msTime;
            }
        }
        for (std::string const& strategy : botAI->GetStrategies(botState))
        {
            if (!bot.strategies.empty())
                bot.strategies += ',';
            bot.strategies += strategy;
        }
        uint32 revision = 0;
        if (log)
        {
            std::lock_guard<std::mutex> lock(log->mutex);
            revision = log->revision;
            for (auto entry = log->entries.rbegin(); entry != log->entries.rend(); ++entry)
                bot.actions.push_back(
                    {entry->name, entry->relevance,
                     uint32(std::min<uint64>(nowMs > entry->atMs ? nowMs - entry->atMs : 0, UINT32_MAX)),
                     entry->repeats});
        }

        std::size_t signature = std::hash<std::string>{}(bot.strategies);
        auto mix = [&](uint64 value) { MixSignature(signature, value); };
        mix(bot.state);
        mix(bot.motionType);
        mix(bot.movePriority);
        mix(bot.flags);
        mix(bot.motionTargetGuid);
        mix(bot.targetGuid);
        mix(moveSetAt);
        mix(revision);
        if (!inserted && signature == sent.signature && nowMs - sent.sentAtMs < STREAM_BOT_REFRESH_MS)
            return;
        sent = {nowMs, signature};
        bots.push_back(std::move(bot));
    }
#endif

    // Adds the unit's path to `paths` when it has changed since it was last
    // sent, or is due a refresh.
    static void CollectPathChange(Unit* unit, uint32 guid, bool creature,
                                  std::unordered_map<uint32, StreamSessionState::SentPath>& sentPaths, uint64 nowMs,
                                  std::vector<StreamPath>& paths)
    {
        bool truncated = false;
        std::vector<G3D::Vector3> points = GetRemainingPath(unit, truncated);
        uint32 const splineId = points.empty() ? 0 : unit->movespline->GetId();
        StreamSessionState::SentPath& sent = sentPaths[guid];
        bool const changed = splineId != sent.splineId;
        if (!changed && (!splineId || nowMs - sent.sentAtMs < STREAM_PATH_REFRESH_MS))
            return;
        sent = {splineId, nowMs};
        uint8 const flags = uint8((truncated ? 1 : 0) | (creature ? 2 : 0));
        paths.push_back({guid, splineId, flags, std::move(points)});
    }

    // Scenery: a creature players can't select (Arathi Basin's workers), or,
    // outside a fight, one they can neither attack nor talk to. wowmin draws
    // it without a ring, facing, or path, so its path isn't sent.
    static bool IsScenery(Creature* creature)
    {
        if (creature->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
            return true;
        if (creature->IsInCombat() || creature->GetNpcFlags() != UNIT_NPC_FLAG_NONE)
            return false;
        return creature->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) || creature->HasUnitFlag(UNIT_FLAG_IMMUNE_TO_PC);
    }

    // Adds each creature that moved, changed, or is due a refresh to
    // `creatures`, and its path to `paths` when that changed.
    static void CollectCreatures(Map* map, StreamSessionState& state, uint64 nowMs,
                                 std::vector<StreamCreature>& creatures, std::vector<StreamPath>& paths,
                                 std::vector<StreamAppearance>& appearances, std::vector<StreamUnitAuras>& unitAuras,
                                 std::vector<StreamUnitDebuffs>& debuffs)
    {
        std::vector<Creature*> present;
        StreamCreatureCollector collector(present);
        TypeContainerVisitor<StreamCreatureCollector, MapStoredObjectTypesContainer> visitor(collector);
        visitor.Visit(map->GetObjectsStore());

        std::unordered_set<uint32> seen;
        for (Creature* creature : present)
        {
            if (!creature->IsInWorld() || creature->IsDuringRemoveFromWorld() || creature->IsTrigger() ||
                creature->IsTotem() || creature->IsCritter())
                continue;
            uint32 const guid = creature->GetGUID().GetCounter();
            seen.insert(guid);

            ObjectGuid const target = creature->GetTarget();
            uint8 flags = 0;
            if (creature->IsAlive())
                flags |= 1;
            if (creature->IsInCombat())
                flags |= 2;
            if (creature->IsHostileToPlayers())
                flags |= 4;
            if (creature->IsDungeonBoss() || creature->isWorldBoss())
                flags |= 8;
            if (creature->IsControlledByPlayer() || creature->GetCharmerOrOwnerGUID().IsPlayer())
                flags |= 16;
            if (target.IsPlayer())
                flags |= 32;
            bool const scenery = IsScenery(creature);
            if (scenery)
                flags |= 64;
            uint32 const targetGuid = target ? target.GetCounter() : 0;
            uint8 const healthPct = uint8(std::clamp(creature->GetHealthPct(), 0.0f, 100.0f));

            auto [iterator, inserted] = state.creatures.try_emplace(guid);
            StreamSessionState::SentCreature& sent = iterator->second;
            float const moved = std::hypot(creature->GetPositionX() - sent.x, creature->GetPositionY() - sent.y,
                                           creature->GetPositionZ() - sent.z);
            bool const moving = moved >= STREAM_CREATURE_MOVE_YARDS ||
                                (creature->movespline->Initialized() && !creature->movespline->Finalized());
            bool const changed = inserted || moving || flags != sent.flags || healthPct != sent.healthPct ||
                                 targetGuid != sent.targetGuid;
            if (changed || nowMs - sent.sentAtMs >= STREAM_CREATURE_REFRESH_MS)
            {
                if (inserted || moved >= STREAM_CREATURE_MOVE_YARDS)
                {
                    sent.wmoGroupId = GetUnitWmoGroupId(map, creature);
                    sent.x = creature->GetPositionX();
                    sent.y = creature->GetPositionY();
                    sent.z = creature->GetPositionZ();
                }
                sent.sentAtMs = nowMs;
                sent.flags = flags;
                sent.healthPct = healthPct;
                sent.targetGuid = targetGuid;
                creatures.push_back({guid, creature->GetEntry(), creature->GetPositionX(), creature->GetPositionY(),
                                     creature->GetPositionZ(), creature->GetOrientation(), healthPct, flags, targetGuid,
                                     sent.wmoGroupId});
            }
            if (!scenery)
                CollectPathChange(creature, guid, true, state.creaturePaths, nowMs, paths);
            CollectAppearance(creature, state, nowMs, appearances);
            CollectAuras(creature, state, nowMs, unitAuras);
            CollectDebuffs(creature, state, nowMs, debuffs);
        }
        // Forget despawned creatures, so a respawn is sent as new.
        std::erase_if(state.creatures, [&](auto const& entry) { return !seen.contains(entry.first); });
        std::erase_if(state.creaturePaths, [&](auto const& entry) { return !seen.contains(entry.first); });
    }

public:
    // Also called when a battleground ends (wowmin_telemetry_battlegroundscript).
    static void CaptureBattleground(BattlegroundMap* map, uint64 sessionKey)
    {
        Battleground* battleground = map->GetBG();
        if (!battleground)
            return;

        WorldPackets::WorldState::InitWorldStates packet;
        battleground->FillInitialWorldStates(packet);
        TelemetryBattleground snapshot{
            map->GetId(),
            map->GetInstanceId(),
            battleground->GetBgTypeID(true),
            battleground->GetStatus(),
            battleground->GetStartTime(),
            battleground->GetStatus() == STATUS_WAIT_JOIN    ? std::max(0, battleground->GetStartDelayTime())
            : battleground->GetStatus() == STATUS_WAIT_LEAVE ? static_cast<int32>(battleground->GetEndTime())
                                                             : 0,
            battleground->GetWinner(),
            battleground->GetTeamScore(TEAM_ALLIANCE),
            battleground->GetTeamScore(TEAM_HORDE),
            battleground->GetPlayersCountByTeam(TEAM_ALLIANCE),
            battleground->GetPlayersCountByTeam(TEAM_HORDE),
            battleground->GetAlivePlayersCountByTeam(TEAM_ALLIANCE),
            battleground->GetAlivePlayersCountByTeam(TEAM_HORDE),
            battleground->GetStatus() == STATUS_IN_PROGRESS
                ? RESURRECTION_INTERVAL - std::min(RESURRECTION_INTERVAL, battleground->GetLastResurrectTime())
                : 0,
            -1,
            -1,
            {}};
#ifdef MOD_PLAYERBOTS
        BattlegroundTypeId strategyType = battleground->GetBgTypeID();
        if (battleground->GetBgTypeID(true) == BATTLEGROUND_WS)
            strategyType = BATTLEGROUND_WS;
        if (battleground->GetStatus() == STATUS_IN_PROGRESS &&
            (strategyType == BATTLEGROUND_WS || strategyType == BATTLEGROUND_AB || strategyType == BATTLEGROUND_AV ||
             strategyType == BATTLEGROUND_EY))
        {
            snapshot.allianceStrategy = BGTactics::GetBotStrategyForTeam(battleground, TEAM_ALLIANCE);
            snapshot.hordeStrategy = BGTactics::GetBotStrategyForTeam(battleground, TEAM_HORDE);
        }
#endif
        snapshot.worldStates.reserve(packet.Worldstates.size());
        for (WorldPackets::WorldState::InitWorldStates::WorldStateInfo const& worldState : packet.Worldstates)
            snapshot.worldStates.push_back({worldState.VariableID, worldState.Value});

#ifdef MOD_PLAYERBOTS
        std::unordered_map<uint64, uint32> roleSnapshot;
        for (auto const& playerReference : map->GetPlayers())
        {
            Player* player = playerReference.GetSource();
            if (!player || !player->GetSession() || !player->GetSession()->IsHeadless())
                continue;

            PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(player);
            AiObjectContext* context = botAI ? botAI->GetAiObjectContext() : nullptr;
            Value<uint32>* role = context ? context->GetValue<uint32>("bg role") : nullptr;
            if (role)
                roleSnapshot[player->GetGUID().GetCounter()] = role->Get();
        }
#endif

        std::lock_guard<std::mutex> lock(telemetryStateMutex);
        battlegroundStates[sessionKey] = std::move(snapshot);
#ifdef MOD_PLAYERBOTS
        battlegroundRoles[sessionKey] = std::move(roleSnapshot);
#endif
    }

private:
    static void CaptureInstance(Map* map, uint64 sessionKey)
    {
        InstanceMap* instanceMap = map->ToInstanceMap();
        InstanceScript* instance = instanceMap ? instanceMap->GetInstanceScript() : nullptr;
        if (!instance)
            return;

        Difficulty difficulty = IsSharedDifficultyMap(map->GetId())
                                    ? Difficulty(static_cast<uint32>(map->GetDifficulty()) % 2)
                                    : map->GetDifficulty();
        DungeonEncounterList const* encounters = sObjectMgr->GetDungeonEncounterList(map->GetId(), difficulty);
        if (!encounters)
            for (uint32 candidate = 0; candidate < MAX_DIFFICULTY && !encounters; ++candidate)
                encounters = sObjectMgr->GetDungeonEncounterList(map->GetId(), Difficulty(candidate));

        TelemetryInstance snapshot{map->GetId(), map->GetInstanceId(), instance->GetCompletedEncounterMask(), {}};
        uint32 const encounterCount = instance->GetEncounterCount();
        snapshot.bosses.reserve(encounterCount);
        for (uint32 bossId = 0; bossId < encounterCount; ++bossId)
            snapshot.bosses.push_back(
                {bossId, instance->GetBossState(bossId), "Encounter " + std::to_string(bossId + 1)});
        if (encounters)
            for (DungeonEncounter const* encounter : *encounters)
                if (encounter->dbcEntry->encounterIndex < snapshot.bosses.size())
                    snapshot.bosses[encounter->dbcEntry->encounterIndex].name =
                        encounter->dbcEntry->encounterName[LOCALE_enUS];

        std::lock_guard<std::mutex> lock(telemetryStateMutex);
        instanceStates[sessionKey] = std::move(snapshot);
    }
};

// ── Combat events ────────────────────────────────────────────

// Queues a combat event for the stream of the session the unit is in, if
// anyone is watching it.
void RecordCombatEvent(Unit const* unit, StreamCombatEvent const& event)
{
    if (!streamAnySubscribed || !unit || !unit->IsInWorld())
        return;
    Map* map = unit->GetMap();
    if (!map || !map->GetInstanceId())
        return;
    std::shared_ptr<StreamSessionState> state;
    {
        std::lock_guard<std::mutex> lock(streamStateMutex);
        auto const iterator = streamSubscriptions.find(GetSessionKey(map->GetId(), map->GetInstanceId()));
        if (iterator == streamSubscriptions.end())
            return;
        state = iterator->second.state;
    }
    std::lock_guard<std::mutex> lock(state->eventMutex);
    if (state->events.size() >= STREAM_MAX_EVENTS)
    {
        ++state->droppedEvents;
        return;
    }
    state->events.push_back(event);
    state->events.back().atMs = GameTime::GetGameTimeMS().count();
}

void SetEventSource(StreamCombatEvent& event, ObjectGuid guid)
{
    event.sourceGuid = guid.GetCounter();
    if (guid && !guid.IsPlayer())
        event.flags |= STREAM_EVENT_SOURCE_CREATURE;
}

void SetEventTarget(StreamCombatEvent& event, ObjectGuid guid)
{
    event.targetGuid = guid.GetCounter();
    if (guid && !guid.IsPlayer())
        event.flags |= STREAM_EVENT_TARGET_CREATURE;
}

StreamCombatEvent MakeUnitEvent(uint8 kind, Unit const* source, Unit const* target)
{
    StreamCombatEvent event;
    event.kind = kind;
    if (source)
        SetEventSource(event, source->GetGUID());
    if (target)
        SetEventTarget(event, target->GetGUID());
    return event;
}

// Which notable kind of aura a spell applies, if any: crowd control, shields
// and immunities, and big defensive and offensive cooldowns. Read from the
// spell alone, so an aura is classed the same way when it goes as when it
// came. Passive auras never count, and permanent ones only as crowd control.
uint8 ClassifyAura(SpellInfo const* spellInfo)
{
    if (!spellInfo || spellInfo->IsPassive())
        return STREAM_AURA_NONE;
    bool const permanent = spellInfo->GetMaxDuration() < 0;
    uint8 category = STREAM_AURA_NONE;
    // Lower values win: crowd control, then shield, defensive, offensive.
    auto consider = [&](uint8 candidate)
    {
        if (category == STREAM_AURA_NONE || candidate < category)
            category = candidate;
    };
    for (SpellEffectInfo const& effect : spellInfo->GetEffects())
    {
        if (!effect.IsAura())
            continue;
        int32 const amount = effect.CalcValue();
        switch (effect.ApplyAuraName)
        {
            case SPELL_AURA_MOD_STUN:
            case SPELL_AURA_MOD_FEAR:
            case SPELL_AURA_MOD_CONFUSE:
            case SPELL_AURA_MOD_CHARM:
            case SPELL_AURA_MOD_POSSESS:
            case SPELL_AURA_MOD_ROOT:
            case SPELL_AURA_MOD_SILENCE:
            case SPELL_AURA_MOD_PACIFY:
            case SPELL_AURA_MOD_PACIFY_SILENCE:
                consider(STREAM_AURA_CROWD_CONTROL);
                break;
            case SPELL_AURA_SCHOOL_ABSORB:
            case SPELL_AURA_SCHOOL_IMMUNITY:
            case SPELL_AURA_DAMAGE_IMMUNITY:
                if (!permanent)
                    consider(STREAM_AURA_SHIELD);
                break;
            case SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN:
                if (!permanent && amount <= -20)
                    consider(STREAM_AURA_DEFENSIVE);
                break;
            case SPELL_AURA_MOD_DAMAGE_PERCENT_DONE:
                if (!permanent && amount >= 10)
                    consider(STREAM_AURA_OFFENSIVE);
                break;
            case SPELL_AURA_MOD_MELEE_HASTE:
            case SPELL_AURA_MOD_MELEE_RANGED_HASTE:
            case SPELL_AURA_MOD_CASTING_SPEED_NOT_STACK:
            case SPELL_AURA_HASTE_SPELLS:
                if (!permanent && amount >= 20)
                    consider(STREAM_AURA_OFFENSIVE);
                break;
            default:
                break;
        }
    }
    return category;
}

// A spell's school, its missile speed, and the reach of its area effects.
void SetEventSpell(StreamCombatEvent& event, SpellInfo const* spellInfo, WorldObject* caster)
{
    event.spellId = spellInfo->Id;
    event.schoolMask = uint8(spellInfo->SchoolMask);
    if (spellInfo->IsPositive())
        event.flags |= STREAM_EVENT_HELPFUL;
    if (spellInfo->IsAffectingArea())
        event.flags |= STREAM_EVENT_AREA;
    if (spellInfo->IsChanneled())
        event.flags |= STREAM_EVENT_CHANNELED;
    event.missileSpeed = spellInfo->Speed;
    for (SpellEffectInfo const& effect : spellInfo->GetEffects())
        if (effect.HasRadius())
            event.radius = std::max(event.radius, effect.CalcRadius(caster));
}

StreamCombatEvent MakeCastEvent(uint8 kind, Spell* spell, Unit* caster, SpellInfo const* spellInfo)
{
    StreamCombatEvent event = MakeUnitEvent(kind, caster, spell->m_targets.GetUnitTarget());
    SetEventSpell(event, spellInfo, caster);
    // The cast's own school, which the core sets for some spells: a wand's
    // Shoot takes the wand's damage school.
    event.schoolMask = uint8(spell->GetSpellSchoolMask());
    if (spell->IsTriggered())
        event.flags |= STREAM_EVENT_TRIGGERED;
    if (spell->m_targets.HasDst())
    {
        if (WorldLocation const* point = spell->m_targets.GetDstPos())
        {
            event.flags |= STREAM_EVENT_GROUND;
            event.x = point->GetPositionX();
            event.y = point->GetPositionY();
            event.z = point->GetPositionZ();
        }
    }
    return event;
}

// The spell behind the damage or heal the core is about to deal. Its hooks
// say which spell it is but not the final amount; the hooks that follow on
// the same thread give the amount but not the spell, so they are paired here.
// Units are kept by GUID: a hook's units are only valid during that call.
struct PendingSpellAmount
{
    ObjectGuid source;
    ObjectGuid target;
    SpellInfo const* spellInfo = nullptr;
    bool periodic = false;
};

ObjectGuid GuidOf(Unit const* unit) { return unit ? unit->GetGUID() : ObjectGuid::Empty; }

thread_local PendingSpellAmount pendingDamage;
thread_local PendingSpellAmount pendingHeal;

// Auto-attack swings are seen once per damage school; one is enough.
struct LastSwing
{
    ObjectGuid attacker;
    ObjectGuid target;
    uint64 atMs = 0;
};

thread_local LastSwing lastSwing;

class wowmin_telemetry_spellscript : public AllSpellScript
{
public:
    wowmin_telemetry_spellscript()
        : AllSpellScript("wowmin_telemetry_spellscript",
                         {ALLSPELLHOOK_ON_PREPARE, ALLSPELLHOOK_ON_CAST, ALLSPELLHOOK_ON_CAST_CANCEL})
    {
    }

    // Instant spells have already fired by the time this runs.
    void OnSpellPrepare(Spell* spell, Unit* caster, SpellInfo const* spellInfo) override
    {
        if (!streamAnySubscribed || !spell || !caster || !spellInfo || spellInfo->IsPassive() ||
            spellInfo->IsChanneled() || spell->GetCastTime() <= 0)
            return;
        StreamCombatEvent event = MakeCastEvent(STREAM_EVENT_CAST_START, spell, caster, spellInfo);
        event.value = uint32(spell->GetCastTime());
        RecordCombatEvent(caster, event);
    }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* spellInfo, bool /*skipCheck*/) override
    {
        if (!streamAnySubscribed || !spell || !caster || !spellInfo || spellInfo->IsPassive())
            return;
        StreamCombatEvent event = MakeCastEvent(STREAM_EVENT_CAST_GO, spell, caster, spellInfo);
        if (spellInfo->HasEffect(SPELL_EFFECT_PERSISTENT_AREA_AURA))
            event.extraFlags |= STREAM_EVENT_PERSISTENT_AREA;
        if (spellInfo->IsChanneled() || (event.extraFlags & STREAM_EVENT_PERSISTENT_AREA))
            event.value = uint32(std::max(0, spellInfo->GetMaxDuration()));
        RecordCombatEvent(caster, event);
    }

    void OnSpellCastCancel(Spell* spell, Unit* caster, SpellInfo const* spellInfo, bool /*bySelf*/) override
    {
        if (!streamAnySubscribed || !spell || !caster || !spellInfo ||
            (spell->GetCastTime() <= 0 && !spellInfo->IsChanneled()))
            return;
        RecordCombatEvent(caster, MakeCastEvent(STREAM_EVENT_CAST_CANCEL, spell, caster, spellInfo));
    }
};

class wowmin_telemetry_combatscript : public UnitScript
{
public:
    // DealDamage has no hook flag: it runs for every unit script.
    wowmin_telemetry_combatscript()
        : UnitScript("wowmin_telemetry_combatscript", true,
                     {UNITHOOK_MODIFY_MELEE_DAMAGE, UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN,
                      UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK, UNITHOOK_MODIFY_HEAL_RECEIVED, UNITHOOK_ON_HEAL,
                      UNITHOOK_ON_AURA_APPLY, UNITHOOK_ON_AURA_REMOVE})
    {
    }

    // Runs for every auto-attack swing, before the hit is rolled, so misses
    // count too.
    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& /*damage*/) override
    {
        if (!streamAnySubscribed || !target || !attacker)
            return;
        uint64 const nowMs = GameTime::GetGameTimeMS().count();
        if (lastSwing.attacker == attacker->GetGUID() && lastSwing.target == target->GetGUID() &&
            lastSwing.atMs == nowMs)
            return;
        lastSwing = {attacker->GetGUID(), target->GetGUID(), nowMs};
        StreamCombatEvent event = MakeUnitEvent(STREAM_EVENT_SWING, attacker, target);
        event.schoolMask = SPELL_SCHOOL_MASK_NORMAL;
        RecordCombatEvent(attacker, event);
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& /*damage*/, SpellInfo const* spellInfo) override
    {
        if (streamAnySubscribed)
            pendingDamage = {GuidOf(attacker), GuidOf(target), spellInfo, false};
    }

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& /*damage*/,
                                       SpellInfo const* spellInfo) override
    {
        if (streamAnySubscribed)
            pendingDamage = {GuidOf(attacker), GuidOf(target), spellInfo, true};
    }

    // The final amount, after armour, resistances, and absorbs.
    uint32 DealDamage(Unit* attacker, Unit* victim, uint32 damage, DamageEffectType damageType) override
    {
        if (!streamAnySubscribed || !victim || !damage ||
            (damageType != DIRECT_DAMAGE && damageType != SPELL_DIRECT_DAMAGE && damageType != DOT))
            return damage;
        StreamCombatEvent event = MakeUnitEvent(STREAM_EVENT_DAMAGE, attacker, victim);
        event.value = damage;
        if (damageType == DOT)
            event.flags |= STREAM_EVENT_PERIODIC;
        if (damageType == DIRECT_DAMAGE)
            event.schoolMask = SPELL_SCHOOL_MASK_NORMAL;
        else if (pendingDamage.spellInfo && pendingDamage.source == GuidOf(attacker) &&
                 pendingDamage.target == victim->GetGUID())
        {
            event.spellId = pendingDamage.spellInfo->Id;
            event.schoolMask = uint8(pendingDamage.spellInfo->SchoolMask);
            if (pendingDamage.periodic)
                event.flags |= STREAM_EVENT_PERIODIC;
        }
        pendingDamage = {};
        RecordCombatEvent(victim, event);
        return damage;
    }

    // The core passes healer and target in either order here, depending on
    // the path, so OnHeal matches them either way round.
    void ModifyHealReceived(Unit* target, Unit* healer, uint32& /*heal*/, SpellInfo const* spellInfo) override
    {
        if (!streamAnySubscribed)
            return;
        bool const periodic = pendingDamage.periodic && pendingDamage.spellInfo == spellInfo;
        pendingHeal = {GuidOf(healer), GuidOf(target), spellInfo, periodic};
        pendingDamage = {};
    }

    // The amount actually healed, without overhealing.
    void OnHeal(Unit* healer, Unit* receiver, uint32& gain) override
    {
        if (!streamAnySubscribed || !receiver || !gain)
            return;
        StreamCombatEvent event = MakeUnitEvent(STREAM_EVENT_HEAL, healer, receiver);
        event.value = gain;
        event.flags |= STREAM_EVENT_HELPFUL;
        ObjectGuid const healerGuid = GuidOf(healer);
        ObjectGuid const receiverGuid = receiver->GetGUID();
        bool const matches = (pendingHeal.source == healerGuid && pendingHeal.target == receiverGuid) ||
                             (pendingHeal.source == receiverGuid && pendingHeal.target == healerGuid);
        if (pendingHeal.spellInfo && matches)
        {
            event.spellId = pendingHeal.spellInfo->Id;
            event.schoolMask = uint8(pendingHeal.spellInfo->SchoolMask);
            if (pendingHeal.periodic)
                event.flags |= STREAM_EVENT_PERIODIC;
        }
        pendingHeal = {};
        RecordCombatEvent(receiver, event);
    }

    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        if (!streamAnySubscribed || !unit || !aura)
            return;
        RecordAura(STREAM_EVENT_AURA_APPLY, unit, aura);
    }

    void OnAuraRemove(Unit* unit, AuraApplication* application, AuraRemoveMode /*mode*/) override
    {
        if (!streamAnySubscribed || !unit || !application || !application->GetBase())
            return;
        RecordAura(STREAM_EVENT_AURA_REMOVE, unit, application->GetBase());
    }

private:
    static void RecordAura(uint8 kind, Unit* unit, Aura const* aura)
    {
        SpellInfo const* spellInfo = aura->GetSpellInfo();
        uint8 const category = ClassifyAura(spellInfo);
        if (category == STREAM_AURA_NONE)
            return;
        StreamCombatEvent event;
        event.kind = kind;
        SetEventSource(event, aura->GetCasterGUID());
        SetEventTarget(event, unit->GetGUID());
        event.spellId = spellInfo->Id;
        event.schoolMask = uint8(spellInfo->SchoolMask);
        event.auraCategory = category;
        if (spellInfo->IsPositive())
            event.flags |= STREAM_EVENT_HELPFUL;
        if (kind == STREAM_EVENT_AURA_APPLY)
            event.value = uint32(std::max(0, aura->GetDuration()));
        RecordCombatEvent(unit, event);
    }
};

class wowmin_telemetry_battlegroundscript : public AllBattlegroundScript
{
public:
    wowmin_telemetry_battlegroundscript()
        : AllBattlegroundScript("wowmin_telemetry_battlegroundscript", {ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_END})
    {
    }

    void OnBattlegroundEnd(Battleground* battleground, TeamId /*winnerTeam*/) override
    {
        if (!battleground || !battleground->GetInstanceID())
            return;

        wowmin_telemetry_mapscript::CaptureBattleground(
            battleground->GetBgMap(), GetSessionKey(battleground->GetMapId(), battleground->GetInstanceID()));
    }
};

class wowmin_telemetry_unitscript : public UnitScript
{
public:
    wowmin_telemetry_unitscript() : UnitScript("wowmin_telemetry_unitscript", true, {UNITHOOK_ON_UNIT_DEATH}) {}

    void OnUnitDeath(Unit* unit, Unit* killer) override
    {
        if (!unit)
            return;

        Player* killerPlayer = killer ? killer->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        if (killerPlayer && killerPlayer != unit && killerPlayer->GetInstanceId() &&
            killerPlayer->GetMap() == unit->GetMap())
            AddTelemetryEvent(killerPlayer, "kill", unit, unit->GetEntry(), unit->GetName(), 1);

        Player* victim = unit->ToPlayer();
        if (!victim || !victim->GetInstanceId())
            return;

        Map* map = victim->GetMap();
        if (!map)
            return;

        std::lock_guard<std::mutex> lock(telemetryStateMutex);
        std::deque<TelemetryDeath>& deaths = deathHistory[GetSessionKey(map->GetId(), map->GetInstanceId())];
        deaths.push_back({map->GetId(), map->GetInstanceId(), nextDeathEventId++,
                          static_cast<uint64>(GameTime::GetGameTime().count()), victim->GetGUID().GetCounter(),
                          victim->GetName(), killer ? killer->GetGUID().GetCounter() : 0,
                          killer ? killer->GetTypeId() : TYPEID_OBJECT, killer ? killer->GetName() : ""});
        if (deaths.size() > MAX_DEATH_HISTORY)
            deaths.pop_front();
    }
};

class wowmin_telemetry_playerscript : public PlayerScript
{
public:
    wowmin_telemetry_playerscript()
        : PlayerScript("wowmin_telemetry_playerscript", {PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_LOOT_ITEM,
                                                         PLAYERHOOK_ON_STORE_NEW_ITEM, PLAYERHOOK_ON_LOGOUT})
    {
    }

    void OnPlayerLogout(Player* player) override
    {
#ifdef MOD_PLAYERBOTS
        if (player)
            ForgetBotActions(player->GetGUID().GetCounter());
#else
        (void)player;
#endif
    }

    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
    {
        if (player && player->GetLevel() > oldLevel)
            AddTelemetryEvent(player, "level", nullptr, player->GetLevel(), "", oldLevel);
    }

    void OnPlayerStoreNewItem(Player* player, Item* item, uint32 /*count*/) override
    {
        if (!player || !item || !player->GetInstanceId())
            return;

        uint32 itemEntry = item->GetEntry();
        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(itemEntry);
        pendingLootItems.push_back({player->GetGUID().GetCounter(), reinterpret_cast<std::uintptr_t>(item), itemEntry,
                                    itemTemplate ? itemTemplate->Name1 : ""});
        if (pendingLootItems.size() > MAX_PENDING_LOOT_ITEMS)
            pendingLootItems.pop_front();
    }

    void OnPlayerLootItem(Player* player, Item* item, uint32 count, ObjectGuid /*lootGuid*/) override
    {
        if (!player || !item || !player->GetInstanceId())
            return;

        uint64 playerGuid = player->GetGUID().GetCounter();
        std::uintptr_t itemAddress = reinterpret_cast<std::uintptr_t>(item);
        for (std::size_t index = pendingLootItems.size(); index > 0; --index)
        {
            PendingLootItem const& pendingItem = pendingLootItems[index - 1];
            if (pendingItem.playerGuid != playerGuid || pendingItem.itemAddress != itemAddress)
                continue;

            uint32 itemEntry = pendingItem.itemEntry;
            std::string itemName = pendingItem.itemName;
            pendingLootItems.erase(pendingLootItems.begin() + index - 1);
            AddTelemetryEvent(player, "loot", nullptr, itemEntry, std::move(itemName), count);
            return;
        }
    }

private:
    struct PendingLootItem
    {
        uint64 playerGuid;
        std::uintptr_t itemAddress;
        uint32 itemEntry;
        std::string itemName;
    };

    static constexpr std::size_t MAX_PENDING_LOOT_ITEMS = 64;
    inline static thread_local std::deque<PendingLootItem> pendingLootItems;
};

class wowmin_telemetry_worldscript : public WorldScript
{
public:
    wowmin_telemetry_worldscript()
        : WorldScript("wowmin_telemetry_worldscript", {WORLDHOOK_ON_BEFORE_CONFIG_LOAD, WORLDHOOK_ON_UPDATE})
    {
    }

    void OnBeforeConfigLoad(bool reload) override { telemetryConfig.Initialize(reload); }

    // Runs after every map has finished its update (World::Update), so no map
    // thread is setting raid markers while they are copied.
    void OnUpdate(uint32 /*diff*/) override
    {
        if (streamAnySubscribed)
            CopyGroupMarkers(GameTime::GetGameTimeMS().count());
    }
};
}  // namespace

void AddSC_wowmin_telemetry()
{
    new wowmin_telemetry_commandscript();
    new wowmin_telemetry_mapscript();
    new wowmin_telemetry_battlegroundscript();
    new wowmin_telemetry_unitscript();
    new wowmin_telemetry_combatscript();
    new wowmin_telemetry_spellscript();
    new wowmin_telemetry_playerscript();
    new wowmin_telemetry_worldscript();
}

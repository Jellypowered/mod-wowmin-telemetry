/*
 * Copyright (C) 2026 WoWMin contributors, released under the MIT license.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
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

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>

#include "AbstractFollower.h"
#include "Battleground.h"
#include "Chat.h"
#include "Creature.h"
#include "ConfigValueCache.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MovementGenerator.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "TypeContainerVisitor.h"
#include "Unit.h"
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
//     4 waiting to resurrect), u8 stateFlags, u8 nameLength, i32 wmoGroupId,
//     name bytes
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
//     is a player), u16 reserved, u32 targetGuid (0: none), i32 wmoGroupId
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
// Gear is read when a unit is first seen and after a player equips or
// unequips something; this re-read catches anything those hooks miss.
constexpr uint64 STREAM_GEAR_REFRESH_MS = 60000;

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

// Bumped when a player equips or unequips something, so streams re-read that
// player's gear. Equip hooks can run outside the map thread.
std::mutex gearRevisionMutex;
std::unordered_map<uint32, uint32> gearRevisions;

uint32 GetGearRevision(uint32 guid)
{
    std::lock_guard<std::mutex> lock(gearRevisionMutex);
    auto const iterator = gearRevisions.find(guid);
    return iterator == gearRevisions.end() ? 0 : iterator->second;
}

void BumpGearRevision(uint32 guid)
{
    std::lock_guard<std::mutex> lock(gearRevisionMutex);
    ++gearRevisions[guid];
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
        bool gearRead = false;
        uint64 gearReadAtMs = 0;
        uint32 gearRevision = 0;
        StreamGear gear;
    };
    // Keyed by guid, with bit 32 set for creatures.
    std::unordered_map<uint64, SentAppearance> appearances;
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
        AppendU8(datagram, 0); // part, patched below
        AppendU8(datagram, 0); // partCount, patched below
        AppendU8(datagram, 0);
        AppendU32(datagram, mapId);
        AppendU32(datagram, instanceId);
        AppendU32(datagram, sequence);
        AppendU32(datagram, serverTimeMs);
        AppendU16(datagram, 0); // playerCount, patched below
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
            AppendU16(datagram, 0); // entry count, patched by finishDatagram
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
        // Forget bots whose engines have all gone, i.e. that logged out.
        std::erase_if(botActionTracking,
                      [&](auto const& entry)
                      {
                          return entry.first != guid &&
                                 std::all_of(entry.second.engines.begin(), entry.second.engines.end(),
                                             [](auto const& attached) { return attached.second.expired(); });
                      });
    }
    return tracking.log;
}
#endif

// Collects the creatures in a map's object store (spawns, summons, and pets).
class StreamCreatureCollector
{
public:
    explicit StreamCreatureCollector(std::vector<Creature*>& creatures) : _creatures(creatures) {}

    void Visit(std::unordered_map<ObjectGuid, Creature*>& creatureMap)
    {
        for (auto const& [guid, creature] : creatureMap)
            _creatures.push_back(creature);
    }

    template<class T>
    void Visit(std::unordered_map<ObjectGuid, T*>&) {}

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
                streamSubscriptions.emplace(
                    sessionKey, StreamSubscription{expiresAtMs, 0, 0, std::make_shared<StreamSessionState>()});
            else
            {
                handler->SendSysMessage("WSTREAM|full");
                return true;
            }
        }
        handler->PSendSysMessage("WSTREAM|{}|{}|{}|{}", STREAM_VERSION,
                                 telemetryConfig.GetConfigValue<uint32>(WowMinTelemetryConfig::StreamPort),
                                 std::max(STREAM_MIN_INTERVAL_MS,
                                          telemetryConfig.GetConfigValue<uint32>(WowMinTelemetryConfig::StreamIntervalMs)),
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
            for (uint64 sessionKey : activeSessionKeys)
                if (auto const iterator = battlegroundStates.find(sessionKey); iterator != battlegroundStates.end())
                    battlegrounds.push_back(iterator->second);
        }

        std::vector<TelemetryInstance> instances;
        std::vector<TelemetryDeath> deaths;
        std::vector<TelemetryEvent> events;
        {
            std::lock_guard<std::mutex> lock(telemetryStateMutex);
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
            players.push_back({player->GetGUID().GetCounter(),
                               player->GetPositionX(),
                               player->GetPositionY(),
                               player->GetPositionZ(),
                               player->GetOrientation(),
                               uint8(std::clamp(player->GetHealthPct(), 0.0f, 100.0f)),
                               uint8(std::clamp(player->GetPowerPct(powerType), 0.0f, 100.0f)),
                               int8(powerType),
                               flags,
                               uint8(GetPlayerStateFlags(player)),
                               GetUnitWmoGroupId(map, player),
                               player->GetName()});
            CollectPathChange(player, player->GetGUID().GetCounter(), false, state->playerPaths, nowMs, paths);
            CollectAppearance(player, *state, nowMs, appearances);
#ifdef MOD_PLAYERBOTS
            if (player->GetSession() && player->GetSession()->IsHeadless())
                CollectBot(map, player, *state, nowMs, bots);
#endif
        }
        // Forget players who have left, so a return is sent as new.
        for (auto iterator = state->playerPaths.begin(); iterator != state->playerPaths.end();)
        {
            bool const present = std::any_of(players.begin(), players.end(),
                [&](StreamPlayer const& player) { return player.guid == iterator->first; });
            iterator = present ? std::next(iterator) : state->playerPaths.erase(iterator);
        }
        std::erase_if(state->bots,
                      [&](auto const& entry)
                      {
                          return std::none_of(players.begin(), players.end(),
                                              [&](StreamPlayer const& player) { return player.guid == entry.first; });
                      });
        std::vector<StreamCreature> creatures;
        CollectCreatures(map, *state, nowMs, creatures, paths, appearances);
        // Forget units that have gone, so a return is sent as new.
        std::erase_if(state->appearances,
                      [&](auto const& entry)
                      {
                          if (entry.first >> 32)
                              return !state->creatures.contains(uint32(entry.first));
                          return std::none_of(players.begin(), players.end(), [&](StreamPlayer const& player)
                                              { return player.guid == uint32(entry.first); });
                      });

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
    }

    // What the unit wears: the visible entry for players, so transmogrification
    // shows; creatures only have weapon slots.
    static StreamGear ReadGear(Unit* unit)
    {
        StreamGear gear;
        for (uint8 slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            uint32 itemEntry = 0;
            if (unit->IsPlayer())
                itemEntry = unit->GetUInt32Value(PLAYER_VISIBLE_ITEM_1_ENTRYID + slot * 2);
            else if (slot >= EQUIPMENT_SLOT_MAINHAND && slot <= EQUIPMENT_SLOT_RANGED)
                itemEntry = unit->GetUInt32Value(UNIT_VIRTUAL_ITEM_SLOT_ID + slot - EQUIPMENT_SLOT_MAINHAND);
            ItemTemplate const* itemTemplate = GetItemTemplate(itemEntry);
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
        uint32 const gearRevision = unit->IsPlayer() ? GetGearRevision(appearance.guid) : 0;
        if (!sent.gearRead || gearRevision != sent.gearRevision || nowMs - sent.gearReadAtMs >= STREAM_GEAR_REFRESH_MS)
        {
            sent.gear = ReadGear(unit);
            sent.gearRead = true;
            sent.gearReadAtMs = nowMs;
            sent.gearRevision = gearRevision;
        }
        appearance.gear = sent.gear;

        std::size_t signature = 0;
        auto mix = [&](uint64 value)
        { signature ^= std::hash<uint64>{}(value) + 0x9e3779b97f4a7c15ULL + (signature << 6) + (signature >> 2); };
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
                bot.actions.push_back({entry->name, entry->relevance,
                                       uint32(std::min<uint64>(nowMs > entry->atMs ? nowMs - entry->atMs : 0,
                                                               UINT32_MAX)),
                                       entry->repeats});
        }

        std::size_t signature = std::hash<std::string>{}(bot.strategies);
        auto mix = [&](uint64 value)
        { signature ^= std::hash<uint64>{}(value) + 0x9e3779b97f4a7c15ULL + (signature << 6) + (signature >> 2); };
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

    // Adds each creature that moved, changed, or is due a refresh to
    // `creatures`, and its path to `paths` when that changed.
    static void CollectCreatures(Map* map, StreamSessionState& state, uint64 nowMs,
                                 std::vector<StreamCreature>& creatures, std::vector<StreamPath>& paths,
                                 std::vector<StreamAppearance>& appearances)
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
            CollectPathChange(creature, guid, true, state.creaturePaths, nowMs, paths);
            CollectAppearance(creature, state, nowMs, appearances);
        }
        // Forget despawned creatures, so a respawn is sent as new.
        std::erase_if(state.creatures, [&](auto const& entry) { return !seen.contains(entry.first); });
        std::erase_if(state.creaturePaths, [&](auto const& entry) { return !seen.contains(entry.first); });
    }

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
        : PlayerScript("wowmin_telemetry_playerscript",
                       {PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_LOOT_ITEM, PLAYERHOOK_ON_STORE_NEW_ITEM,
                        PLAYERHOOK_ON_EQUIP, PLAYERHOOK_ON_UNEQUIP_ITEM})
    {
    }

    // Streams re-read a player's gear after either of these.
    void OnPlayerEquip(Player* player, Item* /*item*/, uint8 /*bag*/, uint8 /*slot*/, bool /*update*/) override
    {
        if (player)
            BumpGearRevision(player->GetGUID().GetCounter());
    }

    void OnPlayerUnequip(Player* player, Item* /*item*/) override
    {
        if (player)
            BumpGearRevision(player->GetGUID().GetCounter());
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
    wowmin_telemetry_worldscript() : WorldScript("wowmin_telemetry_worldscript", {WORLDHOOK_ON_BEFORE_CONFIG_LOAD}) {}

    void OnBeforeConfigLoad(bool reload) override { telemetryConfig.Initialize(reload); }
};
}  // namespace

void AddSC_wowmin_telemetry()
{
    new wowmin_telemetry_commandscript();
    new wowmin_telemetry_mapscript();
    new wowmin_telemetry_unitscript();
    new wowmin_telemetry_playerscript();
    new wowmin_telemetry_worldscript();
}

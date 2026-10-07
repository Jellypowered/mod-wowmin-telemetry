/*
 * Copyright (C) 2026 WoWMin contributors, released under the MIT license.
 */

#include <algorithm>
#include <cstdint>
#include <deque>
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

#include "Battleground.h"
#include "Chat.h"
#include "ConfigValueCache.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Item.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "Unit.h"
#include "WorldSession.h"
#include "WorldStatePackets.h"

#ifdef MOD_PLAYERBOTS
#include "AiObjectContext.h"
#include "BattleGroundTactics.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#endif

using namespace Acore::ChatCommands;

namespace
{
enum class WowMinTelemetryConfig
{
    Enabled,

    NumConfigs,
};

class WowMinTelemetryConfigData : public ConfigValueCache<WowMinTelemetryConfig>
{
public:
    WowMinTelemetryConfigData() : ConfigValueCache(WowMinTelemetryConfig::NumConfigs) {}

    void BuildConfigCache() override
    {
        SetConfigValue<bool>(WowMinTelemetryConfig::Enabled, "WowMinTelemetry.Enable", false);
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
        };

        static ChatCommandTable commandTable = {
            {"wowmin", wowminCommandTable},
        };

        return commandTable;
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
                uint32 mogpFlags = 0;
                int32 adtId = 0;
                int32 rootId = 0;
                int32 wmoGroupId = -1;
                if (!map->GetAreaInfo(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(),
                                      player->GetPositionZ(), mogpFlags, adtId, rootId, wmoGroupId))
                    wmoGroupId = -1;
#ifdef MOD_PLAYERBOTS
                if (session->IsHeadless() && map->IsBattlegroundOrArena())
                    battlegroundRole =
                        GetCachedBattlegroundRole(player->GetMapId(), player->GetInstanceId(), player->GetGUID());
#endif
                if (player->GetInstanceId())
                    activeSessionKeys.insert(GetSessionKey(player->GetMapId(), player->GetInstanceId()));
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
    }

    void OnMapUpdate(Map* map, uint32 diff) override
    {
        if (!map->GetInstanceId())
            return;

        uint64 const sessionKey = GetSessionKey(map->GetId(), map->GetInstanceId());
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
                       {PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_LOOT_ITEM, PLAYERHOOK_ON_STORE_NEW_ITEM})
    {
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

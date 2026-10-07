# mod-wowmin-telemetry

A small AzerothCore module that exposes live, in-memory player telemetry to
[WoWMin](https://github.com/Jellypowered/wowmin) through the existing authenticated
worldserver command transport.

The module is intentionally independent of `mod-playerbots`. It reports core player and
session state for real players and headless sessions, allowing WoWMin to monitor world,
instance, raid, arena, and battleground activity without relying on stale character saves.

## Command

```text
wowmin telemetry [mapId] [instanceId]
```

The command requires `SEC_ADMINISTRATOR` and permits console/SOAP execution. Optional filters
reduce the snapshot to a map or one runtime instance.

The response protocol is line-oriented and versioned:

```text
WMAP_VERSION|7
WMAP|name|mapId|instanceId|x|y|z|orientation|level|race|class|accountId|isBot|alive|inCombat|mapType|difficulty|sessionStartedAt|groupId|isRaidGroup|subgroup|teamId|healthPct|powerType|powerPct|targetGuid|targetType|targetName|roleMask|waitingForResurrect|battlegroundRole|wmoGroupId|gender|stateFlags
WBG|mapId|instanceId|battlegroundTypeId|status|elapsedMs|remainingMs|winner|allianceScore|hordeScore|alliancePlayers|hordePlayers|allianceAlive|hordeAlive|nextResurrectMs|allianceStrategy|hordeStrategy
WOBJ|mapId|instanceId|worldStateId|value
WINS|mapId|instanceId|completedEncounterMask
WBOSS|mapId|instanceId|bossId|state|name
WDEATH|mapId|instanceId|eventId|occurredAt|victimGuid|victimName|killerGuid|killerType|killerName
WEVENT|mapId|instanceId|eventId|occurredAt|type|actorGuid|actorName|targetGuid|targetType|targetName|valueId|valueName|amount
WMAP_END|playerCount|battlegroundCount|worldStateCount|instanceCount|bossCount|deathCount|eventCount
```

Version 7 adds authoritative battleground flag-carrier state (`32`) while a player holds the Warsong,
Silverwing, or Netherstorm flag aura. The bit clears with the aura when the flag is dropped, captured,
returned, or lost on death. Version 6 adds gender plus a compact player-state bitmask: taxi flight (`1`),
mounted (`2`), sapped (`4`), stunned (`8`), and Spirit of Redemption form (`16`). The existing `alive`, `inCombat`, and
`waitingForResurrect` fields remain authoritative for death, combat, and resurrection indicators.
Version 5 adds bounded, server-owned event IDs for participant kills, looted items, and level-ups.
Kill credit follows the player owner of pets and controlled units; loot events include item ID, name,
and count. Version 4 adds scripted boss states, the completed encounter mask, a bounded 50-entry participant
death history per runtime session, and each participant's current WMO group for floor selection.
Boss names use `DungeonEncounter` data when its encounter index matches a scripted boss slot and fall
back to a stable numbered label. Version 3 battleground records retain their original meanings.
String fields percent-escape protocol delimiters. `isBot` identifies headless `WorldSession`
instances through the core API.

The module remains usable without `mod-playerbots`. When both modules are compiled, the optional
integration reports the read-only server-selected team strategy and per-bot `bg role`; unavailable
values are `-1`. WoWMin never changes either value. Version 2 fields retain their original meanings,
including the core LFG role mask in `roleMask`.

## Concurrency

The command acquires the core player-map shared lock only while copying immutable player rows. It
releases the lock before formatting and sending output. Runtime maps publish battleground and
instance-script snapshots once per second from their map update hook; the command copies those
snapshots and bounded death/event history under a separate mutex and never calls map APIs from the command
thread.

## Installation

1. Place this repository at `azerothcore-wotlk/modules/mod-wowmin-telemetry`.
2. Reconfigure and rebuild AzerothCore so the new module is discovered.
3. Copy `mod-wowmin-telemetry.conf.dist` to the worldserver configuration directory as
   `mod-wowmin-telemetry.conf`.
4. Set `WowMinTelemetry.Enable = 1`.
5. Restart the worldserver.
6. Test from an administrator console or SOAP client with `wowmin telemetry`.

The module is disabled by default. No database changes are required.

## WoWMin integration

WoWMin should request `wowmin telemetry`, validate the `WMAP_VERSION` line, and verify that the
`WMAP_END` player, battleground, world-state, instance, boss, death, and event counts match parsed
rows. Database coordinates remain a fallback when the command is disabled or unavailable. WoWMin
retains parsers for legacy responses and protocol versions 1–6 so the service and module can be deployed in
either order.

## License

MIT. See [LICENSE](LICENSE).

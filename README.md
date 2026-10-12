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

`WBG.winner` uses the core `PvPTeamId`: Horde (`0`), Alliance (`1`), or neutral/draw (`2`). Final
battleground snapshots are captured by the battleground-end hook and remain queryable until map destruction.
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

## Live stream (optional)

For WoWMin's 3D Instance Watch the module can also stream one runtime instance
over UDP, ten times a second by default. It is off unless
`WowMinTelemetry.Stream.Enable = 1`.

```text
wowmin stream <mapId> <instanceId>
```

The command (also `SEC_ADMINISTRATOR`, console/SOAP) subscribes the instance for 10 seconds and
replies `WSTREAM|<version>|<port>|<intervalMs>|<ttlMs>`, `WSTREAM|disabled`, or `WSTREAM|full`
(at most 64 instances at once). WoWMin renews every few seconds while a page watches; the stream
stops by itself when renewals stop. Datagrams go only to `WowMinTelemetry.Stream.Host` and
`.Port`; the command cannot redirect them. They are not encrypted, so keep the destination on
localhost or a trusted network.

Every datagram starts with a four-byte magic and a version byte (1). The layouts, little-endian,
are documented beside the code in `src/WowMinTelemetry.cpp`:

| Magic | Carries | Sent |
|---|---|---|
| `WMS1` | Players: position, facing, health, power, state, WMO group, name | Every interval, split into parts when large |
| `WMP1` | Movement paths (waypoints still ahead) of players, bots, and mobs | When a path changes, and every second |
| `WMC1` | Creatures: entry, position, health, hostility, boss, target | When they move or change, and every second |
| `WMB1` | Playerbot decisions: state, move target, strategies, recent actions (with `mod-playerbots`) | When they change, at most 4 a second, and every second |
| `WMA1` | Appearance: race, look, display IDs, mount, sheath state, 19 item displays | On first sight and changes, and every 5 seconds |
| `WME1` | Combat events: casts, notable auras, swings, damage, heals | With the next snapshot after they happen |
| `WMU1` | Auras with spell art on each unit | When a unit's list changes, and every 5 seconds |
| `WMV1` | Vehicle passengers and their seats | With every snapshot while any ride |
| `WMG1` | Game objects with a model: banners, flags, doors, chests | When one appears or changes, and every 10 seconds |
| `WMD1` | Harmful auras on each unit, for nameplates | When a unit's list changes, and every 3 seconds |
| `WMR1` | Raid markers set by the instance's groups | When they change, and every 2 seconds |

Only subscribed instances gather any of this: an unsubscribed instance costs one subscription lookup per
map update, and the combat and aura hooks return at once while nothing is subscribed. Continents
(instance ID 0) are not streamed.

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
5. For WoWMin's 3D Instance Watch, also set `WowMinTelemetry.Stream.Enable = 1` and point
   `WowMinTelemetry.Stream.Host`/`.Port` at the machine WoWMin runs on (its `WOWMIN_STREAM_PORT`).
6. Restart the worldserver.
7. Test from an administrator console or SOAP client with `wowmin telemetry`.

The module is disabled by default. No database changes are required.

## WoWMin integration

WoWMin should request `wowmin telemetry`, validate the `WMAP_VERSION` line, and verify that the
`WMAP_END` player, battleground, world-state, instance, boss, death, and event counts match parsed
rows. Database coordinates remain a fallback when the command is disabled or unavailable. WoWMin
retains parsers for legacy responses and protocol versions 1–6 so the service and module can be deployed in
either order. With a module that has no `wowmin stream` command, or the stream disabled, WoWMin's
Instance Watch carries on with the 1 second telemetry.

## License

MIT. See [LICENSE](LICENSE).

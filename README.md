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

The response protocol is line-oriented and versioned. Long layouts below use `\` continuations for display only;
actual wire records occupy one line, with no continuation character:

```text
WMAP_VERSION|7
WMAP|name|mapId|instanceId|x|y|z|orientation|level|race|class|accountId|isBot|alive|inCombat|\
mapType|difficulty|sessionStartedAt|groupId|isRaidGroup|subgroup|teamId|healthPct|powerType|powerPct|\
targetGuid|targetType|targetName|roleMask|waitingForResurrect|battlegroundRole|wmoGroupId|gender|stateFlags
WBG|mapId|instanceId|battlegroundTypeId|status|elapsedMs|remainingMs|winner|\
allianceScore|hordeScore|alliancePlayers|hordePlayers|allianceAlive|hordeAlive|nextResurrectMs|\
allianceStrategy|hordeStrategy
WOBJ|mapId|instanceId|worldStateId|value
WINS|mapId|instanceId|completedEncounterMask
WBOSS|mapId|instanceId|bossId|state|name
WDEATH|mapId|instanceId|eventId|occurredAt|victimGuid|victimName|killerGuid|killerType|killerName
WEVENT|mapId|instanceId|eventId|occurredAt|type|actorGuid|actorName|\
targetGuid|targetType|targetName|valueId|valueName|amount
WMAP_END|playerCount|battlegroundCount|worldStateCount|instanceCount|bossCount|deathCount|eventCount
```

`WBG.winner` uses the core `PvPTeamId`: Horde (`0`), Alliance (`1`), or neutral/draw (`2`). Final
battleground snapshots are captured by the battleground-end hook and remain queryable until map destruction.
Version 7 adds authoritative battleground flag-carrier state (`32`) while a player holds the Warsong,
Silverwing, or Netherstorm flag aura. The bit clears with the aura when the flag is dropped, captured,
returned, or lost on death. Version 6 adds gender plus a compact player-state bitmask: taxi flight (`1`),
mounted (`2`), sapped (`4`), stunned (`8`), and Spirit of Redemption form (`16`); flying in the air (`64`) and
sitting (`128`) were added later within version 7, so readers should ignore bits they don't know. The existing
`alive`, `inCombat`, and `waitingForResurrect` fields remain authoritative for death, combat, and resurrection
indicators.
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
values are `-1`. WoWMin never changes either value. Version 2 fields retain their original meanings.
Text `roleMask` retains its original meaning: the core-assigned group role mask, or `0` when ungrouped. It is never
replaced with an inferred role; `WMAP_VERSION|7` and its field layout remain unchanged.

### Building with `mod-playerbots`

When `mod-playerbots` is present (`MOD_PLAYERBOTS` is defined), the module compiles against parts of
its internal API, not a stable interface, so a change to any of these breaks the build:

- `PlayerbotAI` through `sPlayerbotsMgr.GetPlayerbotAI`: `GetState`, `GetStrategies`,
  `GetAiObjectContext`, `GetCurrentEngine`
- `PlayerbotAI::IsTank` and `PlayerbotAI::IsHeal` for stream-only role fallback
- `Engine::AddActionExecutionListener`, `Engine::removeActionExecutionListener`, and the
  `ActionExecutionListener` interface (`Engine.h`)
- the `"last movement"` value (`LastMovement`: `priority`, `msTime`, `lastMoveToMapId`,
  `lastMoveToX`, `lastMoveToY`, `lastMoveToZ`) and the `"bg role"` value (`AiObjectContext.h`,
  `LastMovementValue.h`)
- `BGTactics::GetBotStrategyForTeam` (`BattleGroundTactics.h`)

Checked against `mod-playerbots` as of October 2026.

## Live stream (optional)

For WoWMin's 3D Instance Watch the module can also stream one runtime instance
over UDP, ten times a second by default. It is off unless
`WowMinTelemetry.Stream.Enable = 1`.

```text
wowmin stream <mapId> <instanceId>
```

The command (also `SEC_ADMINISTRATOR`, console/SOAP) subscribes the instance for 10 seconds and
replies `WSTREAM|<version>|<port>|<intervalMs>|<ttlMs>`, `WSTREAM|disabled`, or `WSTREAM|full`
(at most 64 instances at once). Expired slots are reclaimed even if the requested instance never existed or no longer
updates, both during world updates and before admitting requests. WoWMin renews every few seconds while a page watches;
the stream stops by itself when renewals stop. Datagrams go only to `WowMinTelemetry.Stream.Host` and
`.Port`; the command cannot redirect them. They are not encrypted, so keep the destination on
localhost or a trusted network. Reloading with telemetry or streaming disabled immediately retires active UDP
subscriptions, queued combat events, and per-stream caches. Re-enabling requires fresh subscriptions; already collected
text history is unaffected. Failed destination validation/socket opening is retried at most once every five seconds
while watched; changing host or port bypasses the cooldown. No alternate destination is substituted.

Every datagram starts with a four-byte magic and a version byte (currently 3). Stream version 3 keeps the v2 byte
layouts but makes non-player unit IDs unique across creature, pet, and vehicle GUID types. Every unit-reference field
uses the same ID: player low GUIDs remain unchanged; non-player IDs contain the low byte of `HighGuid` in bits 24–31
and its 24-bit counter in bits 0–23 (creature `0x30`, pet `0x40`, vehicle `0x50` in the upper byte). The existing
player/creature flags distinguish the player and non-player namespaces. Game-object records retain their own low-GUID
namespace. These are runtime stream identities, not database spawn IDs. Text telemetry is unchanged.

Deploy the v3-capable WoWMin reader before this module; it accepts v1, v2, and v3 for every packet family. Older readers
reject v3 rather than mixing incompatible IDs. Live-view caches may retain old IDs until their normal expiry after a
module upgrade. Stream version 2 adds the core LFG role mask to each `WMS1` player record so roles remain available
with WoWMin's character-database snapshot fallback. Unlike text telemetry, stream roles may use mod-playerbots
classification for grouped players with no assigned combat role. Ungrouped players remain `0`. Stream roles and the
group IDs used internally for raid markers come from world-thread snapshots. Initial stream roles are `0` until
refreshed; raid markers can need a further snapshot.
All layouts are little-endian and documented beside the code in `src/WowMinTelemetry.cpp`:

| Magic | Carries | Sent |
|---|---|---|
| `WMS1` | Player position, state, role, WMO group, and name | Every interval; split when large |
| `WMP1` | Movement paths (waypoints still ahead) of players, bots, and mobs | When a path changes, and every second |
| `WMC1` | Creatures: entry, position, health, hostility, boss, target | When they move or change, and every second |
| `WMB1` | Playerbot state, movement, strategies, recent actions | On change (at most 4 Hz); refresh every second |
| `WMA1` | Appearance, mount, sheath state, 19 item displays | On sight/change; refresh every 5 seconds |
| `WME1` | Combat events: casts, notable auras, swings, damage, heals | With the next snapshot after they happen |
| `WMU1` | Auras with spell art on each unit | When a unit's list changes, and every 5 seconds |
| `WMV1` | Vehicle passengers and their seats | With every snapshot while any ride |
| `WMG1` | Modeled game objects: banners, flags, doors, chests | On sight/change; refresh every 10 seconds |
| `WMD1` | Harmful auras on each unit, for nameplates | When a unit's list changes, and every 3 seconds |
| `WMR1` | Raid markers set by the instance's groups | When they change, and every 2 seconds |

Only subscribed instances gather any of this: an unsubscribed instance costs one subscription lookup per
map update. Combat and aura hooks check the unit's own active subscription before inspecting spells or building events,
even while other maps are watched. Continents (instance ID 0) are not streamed.

## Concurrency

Map threads build owned datagram batches; the world update drains them through one nonblocking UDP socket after all
map updates join. The shared queue lock covers only admission and queue swaps, not address parsing, socket operations,
or sends. The queue holds at most 2,048 datagrams; a batch that cannot fit is dropped whole as best-effort traffic.
Config reload discards queued batches before changing settings, preventing sends to an obsolete destination. No extra
sender thread is created.

CLI/SOAP text commands run in the world update after map tasks join; administrator chat commands run in the world
session phase before map tasks start. They read live player fields under the player-map shared lock, including map
difficulty and WMO lookups. That lock protects the player registry, not all mutable player fields; safety also relies
on the world/map phase ordering. The lock is released before output formatting and sending. Map callbacks separately
publish battleground/instance snapshots once per second; text commands copy those and bounded history under a mutex.

For streams, map threads request group data by player GUID and read owned snapshots under a per-stream mutex. Only the
world thread resolves groups, traverses member roles, and performs optional role inference, after map tasks join. No
live group pointers cross threads. Snapshot requests/data are bounded by the stream's participants and retire with its
subscription. Logout removes the player's requests/data; other departures refresh on the next snapshot. Bot action
listeners detach on the world thread after map updates join when no active stream observes the bot, or during
disable/logout cleanup. A live engine owns each attached listener; lifetime tokens prevent access
after engine destruction. Retiring tracking drops its action history; watching again attaches fresh listeners.

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

The module is disabled by default. With `WowMinTelemetry.Enable = 0`, text map/battleground snapshots and
kill/death/loot/level collection stop; lifecycle bookkeeping and cleanup remain active. Already collected bounded
history is retained until map destruction. Text collection requires only telemetry enablement, not UDP subscriptions
or `WowMinTelemetry.Stream.Enable`. No database changes are required.

## WoWMin integration

WoWMin should request `wowmin telemetry`, validate the `WMAP_VERSION` line, and verify that the
`WMAP_END` player, battleground, world-state, instance, boss, death, and event counts match parsed
rows. Database coordinates remain a fallback when the command is disabled or unavailable. WoWMin
retains parsers for legacy responses and protocol versions 1–6 so the service and module can be deployed in
either order. With a module that has no `wowmin stream` command, or the stream disabled, WoWMin's
Instance Watch carries on with the 1 second telemetry.

## License

MIT. See [LICENSE](LICENSE).

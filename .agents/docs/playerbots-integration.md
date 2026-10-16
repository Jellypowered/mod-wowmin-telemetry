# Optional mod-playerbots integration

Read with `cpp-guidelines.md` and `telemetry.md` for any code under `#ifdef MOD_PLAYERBOTS` or any change to playerbots
headers, values, engines, strategies, roles, or battleground tactics.

## Compatibility boundary

- The module must compile and operate without mod-playerbots. Guard every playerbots include, symbol, member, and
  behavior with `MOD_PLAYERBOTS`; do not leak its types into unconditional structures or signatures.
- Playerbots APIs are internal and branch-specific. Verify every signature and ownership assumption against the
  checked-out `../mod-playerbots` source. Update README's compatibility list when that surface changes.
- If the module requires a playerbots or parent-core change, keep it in a companion change, state the required
  revision, and do not silently patch a sibling repository.

## Observational behavior

- Integration is read-only. It may observe bot state, strategies, values, movement decisions, and action execution, but
  must not add/remove strategies, execute actions, alter movement, assign roles, or change gameplay decisions.
- Calls that can also receive a human player must be verified for that case. `PlayerbotAI::IsTank`/`IsHeal`, manager
  lookups, and context access do not all have identical preconditions.
- Human players, random bots, and other headless sessions are distinct cases. Do not use `IsHeadless()` alone as
  proof that a `PlayerbotAI` or named AI value exists.

## Threading and lifetime

- Playerbot AI updates on map threads, with different maps in parallel. Access the bot's current engine/context only
  on an established safe path for that bot.
- Operations that mutate groups, guilds, sessions, login state, or other world-owned data belong on the world thread,
  normally through `PlayerbotWorldThreadProcessor`.
- Shared listener/log registries need synchronization. The engine owns action listeners; tracking code must tolerate
  engine replacement and destruction without dereferencing stale engine pointers.
- Never retain bot, target, engine-owned action, or movement-object pointers beyond their valid call/update lifetime.

## Cost

Optional integration still runs for every streamed bot at the configured interval. Bound strategy/action strings,
history, listener registrations, and cache entries. Remove per-bot state on logout and when a stream no longer tracks
that bot.

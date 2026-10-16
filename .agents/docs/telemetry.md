# Telemetry architecture

Read this document before changing commands, packets, hooks, configuration, retained state, subscription behavior, or
thread synchronization in `src/WowMinTelemetry.cpp`.

## Execution and ownership

- `AllMapScript::OnMapUpdate` and map-owned unit traversal run on map threads. Snapshot object fields there and transfer
  only owned values.
- CLI/SOAP commands run through the world update. Verify ordering against the checked-out `World.cpp` and `MapMgr.cpp`
  before assuming maps are quiescent.
- Combat, spell, player, and battleground hooks follow their local call thread. Trace each hook before sharing data.
- `WorldScript::OnUpdate` is the appropriate place for world-owned state after map updates have joined.
- State shared across maps or with the world thread must have explicit synchronization. Establish and preserve a
  documented lock order; never keep a shared-state lock across network I/O or large formatting.
- Group, guild, session, and playerbots state is not assumed thread-safe. Prefer a world-thread-produced synchronized
  snapshot when map-thread telemetry needs it.

## Enablement and bounded state

- `WowMinTelemetry.Enable = 0` should make collection cheap, not merely hide the commands.
- `WowMinTelemetry.Stream.Enable = 0` must stop stream collection and retire stream-only state.
- Every retained map, history, queue, and subscription needs a bound and a cleanup path for expiry, map destruction,
  logout, reload/disable, invalid identifiers, and shutdown as applicable.
- Expired subscriptions must be reclaimed even when the requested map never existed or no longer updates.
- Invalid destination configuration is validated once or rate-limited; never emit an error per packet or map tick.

## Text command protocol

- `WMAP_VERSION` covers field count and meaning, not only syntax. A semantic change to an existing field must preserve
  backward behavior or advance the version with a matching consumer change.
- Percent-escape every string field that can contain `|`, CR, LF, or `%` before formatting a line.
- Snapshot under the correct owner/lock, then release locks before formatting and sending command output.
- Keep history IDs monotonic under their protecting lock and keep histories bounded.

## UDP stream protocol

- `STREAM_VERSION` is shared by every emitted magic (`WMS1`, `WMP1`, `WMC1`, and the other packet families). A version
  change therefore affects every parser, even when only one record layout changes.
- For each wire change, update together: version support, fixed entry size, field order, offsets, reserved bytes, packet
  split calculation, count widths, code comments, README, and WoWMin parser/tests.
- Keep datagrams within `STREAM_MAX_DATAGRAM`. Cap variable-length data before narrowing to `uint8`/`uint16`, and define
  intentional truncation behavior.
- Validate part/count overflow and empty-clear messages. Do not silently drop records merely because an encoded count is
  narrower than a container size.
- Commands may select only a server session, never a host or port. UDP remains server-configured and best-effort.
- Build owned datagrams before entering sender synchronization. A slow or failed destination must not stall map
  updates or produce unbounded logs.

## Performance review

For each new field or packet, identify collection frequency, maps/units scanned, allocations, cache/refresh policy,
upper bounds, and disabled cost. Change detection reduces network traffic but does not make the underlying scan free.

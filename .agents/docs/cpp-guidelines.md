# C++ guidelines

Apply the parent AzerothCore C++ standards plus the module-specific rules below. Run the checks in `build.md` before
claiming completion.

## Style and APIs

- C++20, Allman braces, four spaces, no tabs, and no braces around a single-line statement.
- Write `auto const&`, `Type const*`, `Player* player`, and `static float const value = 1.0f`.
- Use UpperCamelCase for functions, lowerCamelCase for locals and parameters, and `_lowerCamelCase` for private members.
- Prefer `constexpr`, `enum`, or `enum class` to magic values and preprocessor constants.
- Use fmt-style `{}` formatting and AzerothCore `LOG_*` macros; never use printf formatting, `printf`, `sLog->`, or
  `TC_LOG_*`.
- Use `Acore::` APIs and helpers from `Random.h`; do not introduce `Trinity::`, `std::rand`, `rand()`, or `<random>`.
- Use typed object and flag helpers rather than raw type or flag fields.
- `ObjectGuid::GetCounter()` is allowed only when a documented telemetry protocol serializes a numeric low GUID. Do not
  use it for logs, retained identity outside that contract, or comparisons where ObjectGuid fits.
- New source files use this module's MIT header. Preserve compatible upstream notices when adapting code.

## Configuration and database access

- Declare distributed options in `conf/mod-wowmin-telemetry.conf.dist` under `WowMinTelemetry.*`, with defaults, units,
  enablement dependencies, and security implications.
- Load options into `WowMinTelemetryConfigData` at startup/reload. Hot paths may read that cache, not `sConfigMgr`
  directly.
- Validate destination and range-sensitive settings once, avoid repeated error logging, and make disabled defaults
  cheap.
- Use prepared statements for database work, async reads on map/update paths, and transactions for multi-statement
  writes.
  The current module performs no database queries; adding one requires explicit design review.

## Lifetime and thread safety

- Never retain raw `Player*`, `Creature*`, `Unit*`, `Map*`, `Item*`, or `WorldSession*` beyond the current call/update.
  Retain an ObjectGuid or owned scalar/string snapshot and resolve again when needed.
- Check lookups and conversions before dereference, and reject objects from another map.
- Shared mutable state needs a mutex, atomic, thread-local ownership, or a documented single-thread owner.
- Minimize lock scope. Bounded copying of protected containers into owned snapshots is allowed under their lock.
  Release the lock before expensive processing, network I/O, large formatting, or invoking callbacks.
- Hooks run frequently. Gate collection on telemetry enablement before allocation, locking, scans, or spell inspection.
  Gate stream-only collection on a relevant subscription; text telemetry histories must not depend on UDP subscriptions.

## Comments

Comments explain protocol contracts, thread ownership, non-obvious core behavior, or safety constraints. Do not narrate
change history. Prefer one or two lines; detailed wire-format documentation is a valid exception.

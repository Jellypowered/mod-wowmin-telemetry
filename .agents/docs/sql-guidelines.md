# SQL guidelines

The module currently ships no SQL and requires no database changes. The `.gitkeep` files under `data/sql/db-auth`,
`db-characters`, and `db-world` only preserve module-loader directories.

Before adding SQL:

- Confirm the behavior cannot be implemented safely from existing in-memory core data or configuration.
- Inspect the checked-out AzerothCore module SQL loader and a current local module with the same database target; do not
  assume parent-core `pending_db_*` placement applies inside this nested repository.
- Keep files inside this module's matching `data/sql/db-*` tree. Never place a module update in the parent core's base,
  archive, merged-update, or pending-update directories unless the user explicitly requests a separate core change.
- Choose base/update placement from the verified local module-loader convention and document upgrade behavior.
- Make updates idempotent: scope `DELETE`/`UPDATE` precisely, pair inserted fixed rows with a matching delete where
  appropriate, use backticks consistently, avoid double semicolons and duplicate blank lines, and use InnoDB for tables.
- Use one concern per file and preserve existing installations as well as fresh installs.

Run the parent SQL codestyle tool only when its expected repository/branch layout supports the module path. If it cannot
run—for example because it hardcodes `origin/master`—report that limitation and review the SQL manually rather than
claiming it passed.

# setup/

Everything needed to take a bare Linux host to a running stack:

```sh
make setup     # install, configure, generate
make           # build
make run       # start
```

`make setup` is idempotent. Re-running reports what was already correct instead
of redoing it, so it is also how a half-configured machine gets repaired.

## Files

| Path | Purpose |
| --- | --- |
| `worlds.conf` | **The world table.** One row per world. Everything below is generated from it. |
| `setup.sh` | The orchestrator run by `make setup`. |
| `check.sh` | The read-only preflight run by `make setup-check`. |
| `lib/common.sh` | Logging, root handling, service and package detection. |
| `lib/packages.sh` | Build and runtime dependencies (apt, dnf, pacman). |
| `lib/services.sh` | Starting PostgreSQL and Redis. |
| `lib/postgres.sh` | The `postgres` role password, `~/.pgpass`, and the world databases. |
| `lib/worlds.sh` | Reads `worlds.conf`; generates the world configs and the key rotation script, and checks the copies packaged beside the binaries. |
| `lib/certs.sh` | The login TLS certificate and the launcher's public-key pin. |
| `lib/layout.sh` | Runtime directories and `world.dat`. |

## What `make setup` does

1. **Validates `worlds.conf`** — duplicate names, ports or databases; capacities
   above `MAX_PLAYERS`; more worlds than `MAX_WORLDS`. All refused before
   anything is written.
2. **Installs dependencies** — compiler, `libhiredis`, `libpq`, `libsodium`,
   `libsqlite3`, `libssl`, `openssl`, PostgreSQL, Redis.
3. **Starts PostgreSQL and Redis**, and enables them at boot where systemd runs.
4. **Configures PostgreSQL auth** — generates a password for the `postgres` role
   and records it in `~/.pgpass` (mode 600), so nothing has to carry it in an
   environment variable that `/proc` exposes to every process of this user.
5. **Creates one database per world.** Not the tables: `character_database_init()`
   owns that schema and runs its own migrations at startup.
6. **Creates the runtime directories** the servers open but do not create.
7. **Generates** `world_server/world_config/*.conf`,
   and `common/server_keys/generate_daily_server_keys.sh` from `worlds.conf`.
8. **Checks** that the `worlds.conf` packaged beside each binary matches `setup/worlds.conf`.
9. **Generates the login TLS certificate** and installs the matching public-key
   pin into the client tree.
10. **Generates `world.dat`** by building the client's world generator natively,
    writing one file into both trees so the two copies cannot differ.

Root is needed for steps 2–5 only. The password is requested once, up front. If
it is unavailable, those steps report a blocker and the rest still runs.

## Adding a world

1. Add a row to `worlds.conf`. Row order sets `world_id` — appending is safe,
   reordering renumbers every world.
2. Add the matching entries to `common/src/world_database_config.c`
   (`get_database_for_world`, `get_database_for_world_id`, `get_world_name_by_id`).
   `make setup` will tell you precisely which are missing if you forget.
3. Add the world to the `worlds=(...)` array in `scripts/start_servers.sh`.
4. `make setup && make`

Raise `MAX_WORLDS` in `realm_server/include/world_database_manager.h` first if
you are going past ten.

## Changing a world's address

Edit `host` in `worlds.conf` and re-run `make setup`. That keeps the world's
generated `.conf` in agreement with the roster every service reads, and a
disagreement between them shows up as a world clients cannot reach.

A world's own address is added to its realm-handshake allowlist automatically,
so a realm on the same machine keeps working. A realm on a **different** machine
needs an explicit `realm_allow` line in that world's `.conf`.

## Editing generated files by hand

`make setup` will not overwrite a world `.conf` whose five positional values
already match `worlds.conf`, so anything you add below them survives. A file
that *disagrees* is left alone and reported; `make setup-worlds` overwrites it.

`generate_daily_server_keys.sh` is regenerated whole. Put changes in
`worlds.conf`.

## Ports

Each row gives a world one client port. The realm link gets a **second** port,
`port + 1000` by default, or whatever an optional eighth column names. Setup
refuses a `worlds.conf` where any two ports collide — client or realm — because
that failure otherwise appears as a bind error at startup with no indication of
which two rows disagreed.

## Environment

| Variable | Effect |
| --- | --- |
| `MMO_CLIENT_ROOT` | Path to the client tree. Default `../mmo_client`. |
| `FORCE_CONFIGS=1` | Overwrite world `.conf` files that disagree with `worlds.conf`. |
| `SKIP_PACKAGES=1` | Do not touch the package manager. |
| `SKIP_WORLD_DAT=1` | Do not generate `world.dat` (minutes, ~115 MB). |

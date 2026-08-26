# Multiverse MMO Server

This repository contains the Linux server stack for Multiverse MMO. The stack consists of:

- A TLS login server on port `7776`. It handles registration, login, patch notes, and session creation.
- A realm server on port `7777`. It handles character selection and reports available worlds.
- Ten world server processes. They handle movement, combat, NPCs, parties, quests, shops, loot, and player state.

Redis stores sessions, game tickets, and server authentication keys. SQLite stores user accounts. PostgreSQL stores characters and world-specific player data.

## Quick start

On any Linux host, including WSL:

```sh
sudo apt install -y make git      # only if you do not have them
cd mmo_server
make setup                        # dependencies, services, databases, configs, certs
make                              # build
make run                          # start the stack; Ctrl+C stops it
```

`make setup` asks for your sudo password once, up front, and does everything the
manual steps below used to. It is idempotent — re-run it any time to repair a
half-configured machine. `make setup-check` verifies without changing anything
and is the first thing to run when something will not start.

See [setup/README.md](setup/README.md) for what it does step by step, and how to
add a world or change a world's address.

The manual path below remains accurate if you would rather do it yourself, or
need to understand what `make setup` did.

---

## WSL setup on Windows

WSL with Ubuntu is the recommended development environment for the server. The Windows client can connect to services running in WSL through `localhost` on a standard local setup.

### 1. Install WSL and Ubuntu

Open PowerShell as Administrator and run:

```powershell
wsl --install -d Ubuntu
```

Restart Windows if requested, then open Ubuntu and complete its first-run user setup.

For better filesystem performance, keep the repositories in the WSL filesystem, such as `~/mmo/dev`, instead of under `/mnt/c`.

### 2. Install the server dependencies

Run these commands inside Ubuntu:

```sh
sudo apt update
sudo apt install -y \
  build-essential \
  libhiredis-dev \
  libpq-dev \
  libsodium-dev \
  libsqlite3-dev \
  libssl-dev \
  openssl \
  postgresql \
  postgresql-client \
  redis-server
```

### 3. Start PostgreSQL and Redis

```sh
sudo service postgresql start
sudo service redis-server start
```

Confirm that both services respond:

```sh
pg_isready
redis-cli PING
```

Redis should print `PONG`.

Set a password for the PostgreSQL `postgres` user. The server supervisor will request this password when it starts.

```sh
sudo -u postgres psql -c "ALTER USER postgres WITH PASSWORD 'choose-a-password';"
```

Replace `choose-a-password` with a local development password.

### 4. Create the login database directory and the TLS certificates

`make setup` does all of this, including cross-installing the pins into the
client tree. By hand, from the `mmo_server` root:

```sh
mkdir -p database/databases/users_data
for svc in login realm world; do
  mkdir -p ${svc}_server/certs
  openssl req -x509 -newkey rsa:2048 \
    -keyout ${svc}_server/certs/server.key \
    -out    ${svc}_server/certs/server.crt \
    -days 3650 -nodes -subj "/CN=mmo-${svc}"
  chmod 600 ${svc}_server/certs/server.key
done
```

Three certificates, because three services terminate TLS. Every certificate and
private key is a local file and is ignored by Git.

Generating them is only half of it: each end of a link has to be told which
public key to expect. See [TLS and pinning](#tls-and-pinning) below for the four
pin files and how to produce them — `make setup` writes all four.

### 5. Create the PostgreSQL databases

```sh
make setup-postgres
```

This creates one PostgreSQL database per world listed in `setup/worlds.conf`. It
is safe to run again; existing databases are left alone.

It does **not** create tables. `character_database_init()` issues the
`CREATE TABLE` and the migrations for `characters`, `character_items` and
`character_currencies` when a world server first starts, and it is the only
definition of that schema.

### 6. Build the server

```sh
make -j"$(nproc)"
```

Run the strict build before committing server changes:

```sh
make strict
```

This target removes existing build artifacts, rebuilds every server with
`-Wall -Wextra -Werror`, and fails when the compiler reports a warning.

The build creates:

```text
login_server/bin/login_server
realm_server/bin/realm_server
world_server/bin/world_server
world_server/bin/data/
```

## Run the full stack

Make sure PostgreSQL and Redis are running, then start the stack from the repository root:

```sh
make run
```

Enter the PostgreSQL password when prompted. The supervisor starts the login server, all ten world servers, and the realm server. Keep this terminal open while the stack is running.

Press `Ctrl+C` for a graceful shutdown. From another WSL terminal, you can also run:

```sh
make stop
```

### Windows client, WSL server

This is the supported development split: the servers run under WSL, the client
is the MSYS2 Windows build, and the two talk over loopback.

WSL2 forwards `127.0.0.1` from Windows into the WSL VM for any socket bound to
`0.0.0.0`, which all three services are. **No address configuration is needed** —
the client's compiled defaults already work:

```ini
login_ip=127.0.0.1
login_port=7776
game_ip=127.0.0.1
game_port=7777
```

These values can be placed in `mmo_client/Launcher/bin/server.conf` if you need
to override them (a server on a different machine, or WSL1, where you need the
VM's address from `ip addr show eth0`).

What *does* have to cross the boundary is the pin files. Check the two trees out
beside each other under the Windows filesystem — `C:\...\mmo\dev\mmo_server`
and `C:\...\mmo\dev\mmo_client`, reached from WSL as `/mnt/c/...` — and
`make setup` writes the client pins directly into the client tree. Otherwise set
`MMO_CLIENT_ROOT`.

Check the whole path from Windows without launching the game:

```powershell
# TCP reachability
foreach ($p in 7776,7777) {
  $c = New-Object Net.Sockets.TcpClient
  try { $c.Connect('127.0.0.1', $p); "port $p CONNECTED"; $c.Close() }
  catch { "port $p FAILED: $($_.Exception.Message)" }
}

# TLS handshake against the realm, saving the certificate it presented
$c = New-Object Net.Sockets.TcpClient
$c.Connect('127.0.0.1', 7777)
$cb = [Net.Security.RemoteCertificateValidationCallback]{ $true }
$s = New-Object Net.Security.SslStream($c.GetStream(), $false, $cb)
$s.AuthenticateAsClient('mmo-realm')
"TLS $($s.SslProtocol)"
[IO.File]::WriteAllBytes("$env:TEMP\realm.cer", $s.RemoteCertificate.GetRawCertData())
```

Then, back in WSL, confirm the key it presented is the one the client pins:

```sh
SEEN=$(openssl x509 -inform DER -in /mnt/c/Users/$USER/AppData/Local/Temp/realm.cer \
       -pubkey -noout | openssl pkey -pubin -outform der \
       | openssl dgst -sha256 -binary | openssl base64)
grep -qxF "sha256/$SEEN" ../mmo_client/Game/certs/realm_pins.txt \
  && echo "MATCH — the game will accept this realm" \
  || echo "MISMATCH — regenerate the pin with make setup"
```

A `MISMATCH` after regenerating the realm certificate is the expected failure:
the client tree still names the old key. Re-run `make setup`, then rebuild the
client so it ships the new pin file.

## Native Linux setup

Ubuntu and Debian use the same package, database, certificate, build, and run commands shown in the WSL section. On another distribution, install the equivalent packages for:

- GCC, Make, and POSIX threads
- SQLite 3 development headers
- hiredis development headers and Redis
- PostgreSQL client development headers and PostgreSQL
- OpenSSL development headers and command-line tools
- libsodium development headers

Then follow steps 3 through 6 above.

## Run individual services

The complete stack is the normal development mode. These targets are available for focused debugging:

```sh
make run-login
make run-realm
make run-world
```

`make run-world` starts only the Armeia world. Individual services still require their relevant PostgreSQL, Redis, certificate, configuration, and data files.

## Configuration and ports

The default local stack uses these files:

- `realm_server/realm_config/realm_1.conf` sets the realm name and port.
- `setup/worlds.conf` is the world table, and the only place the roster lives.
  Every service reads it: the realm's world list and world identifiers, each
  world's database and realm port, and every world's display name. `make setup`
  generates every `world_server/world_config/*.conf` and the key rotation script
  from it — edit it there rather than in the generated files.
- `world_server/world_config/*.conf` sets each world's name, region, address, capacity, and hardcore flag. Generated; hand edits below the positional block are preserved.
- `world_server/data/world.dat` contains the collision grid used for server-side movement validation. A world server will not start without it: with no collision grid it cannot tell open ground from a wall, so movement would go unvalidated.
- `world_server/data/zones.json` defines the regions used for zone-change notifications.
- `login_server/server_files/patch_notes.txt` contains the launcher patch notes.

Default listening ports are:

| Service | Port |
| --- | ---: |
| Login | 7776 |
| Realm | 7777 |
| Armeia | 7778 |
| Bosteuis | 7779 |
| Cardinal | 7780 |
| Derive | 7781 |
| Exodus | 7782 |
| Karmel | 7785 |
| Longevity | 7786 |
| Nervow | 7787 |
| Jatrus | 7788 |
| Prototype | 7790 |

Each world also listens for the realm on its client port **+ 1000** (8778 for
Armeia, and so on) — see [The realm listener](#the-realm-listener). Those ports
belong on a private interface; do not open them to players.

For remote clients, set reachable addresses in `setup/worlds.conf` and re-run
`make setup`. Also configure the client launcher and allow the client ports —
not the realm ports — through the host firewall.

Redis defaults to `127.0.0.1:6379`. The server code accepts `REDIS_HOST` and `REDIS_PORT` environment variables when Redis runs elsewhere.

### Connection pools and worker counts

Every blocking query runs on a worker thread, and every worker needs a database
connection. The two numbers are linked: running more workers than connections
does not make the database faster, it makes the surplus wait inside the pool's
acquire, which gives up after five seconds and fails the query. So the worker
counts are **derived** from the pool sizes rather than set independently, and
raising a pool raises its workers with it.

| Variable | Default | Sizes |
| --- | ---: | --- |
| `MMO_DB_POOL_SIZE` | 4 | The world server's character-database pool. |
| `MMO_WORLD_POOL_SIZE` | 4 | The realm server's pool **per world** in the roster. |
| `MMO_REDIS_POOL_SIZE` | 8 | The Redis pool shared by all three services. |

The derived worker counts, which `MMO_WORLD_WORKERS` and `MMO_REALM_WORKERS`
override when you want them decoupled:

| Service | Workers |
| --- | --- |
| World | `MMO_DB_POOL_SIZE` × 2 — a world worker spends part of its time in Redis and socket writes, so a small multiple keeps the connections busy without a crowd waiting on them. |
| Realm | `MMO_WORLD_POOL_SIZE` — every realm packet is a query against one world's pool. |

Raise these together with PostgreSQL's own `max_connections`, which is shared
across every process pointed at the same server: the realm opens
`MMO_WORLD_POOL_SIZE` connections *per world in the roster*, so with twelve
worlds the default is already 48 from the realm alone.

### Health and metrics

Every service can expose two HTTP routes for monitoring:

| Route | Response |
| --- | --- |
| `GET /healthz` | `200 ok` while healthy, `503` with a one-line reason when not. For liveness and readiness probes. |
| `GET /metrics` | Prometheus text exposition. |

Disabled unless `MMO_METRICS_PORT` is set. When it is, each service adds its own
offset to it, so one variable configures the whole stack:

| Service | Port |
| --- | --- |
| Login | base + 0 |
| Realm | base + 1 |
| World *N* | base + *N* (its `worlds.conf` row order) |

```sh
MMO_METRICS_PORT=9500 bash scripts/start_servers.sh
curl localhost:9501/healthz     # the realm
curl localhost:9502/metrics     # Armeia, world 1
```

The endpoint binds `127.0.0.1` by default and authenticates nobody.
`MMO_METRICS_BIND` moves it to another interface — do that only behind a
firewall, because population, tick health and pool saturation are exactly what
an attacker would use to pick a moment.

What `/healthz` reports as unhealthy:

- **Login** — Redis unreachable, so no session can be created.
- **Realm** — Redis unreachable, or every configured world offline.
- **World** — the gameplay thread has stopped, the tick is averaging over its
  50ms budget, or the database pool is saturated with threads queued behind it.

A **full** world is healthy: it is working correctly, and the realm already
steers players elsewhere on capacity.

The world's `/metrics` covers population, per-phase tick time (mean and worst
over the last ten-second window), NPC occupancy, chat and worker queue depth,
open connections, database pool saturation, and open shop sessions.

### Following one player across three services

A login crosses the login server, the realm and a world. Each writes its own
log file, and lining up three timestamped files to follow one player is not a
diagnosis method.

Every session gets a **correlation id**, minted by the login server when the
session is created. It travels with the session in Redis, into the world
ticket, and onto every log line the three services write while handling that
player:

```text
[14:22:07.104] [INFO ] [a3f9c1e05b2d7480] auth.c:198: [LOGIN] SUCCESS
[14:22:07.318] [INFO ] [a3f9c1e05b2d7480] world_list.c:288: world ticket issued
[14:22:07.512] [INFO ] [a3f9c1e05b2d7480] net_loop.c:305: entered the world
```

So one player's whole login is:

```sh
grep a3f9c1e05b2d7480 logs/*.log | sort
```

Lines not handling a specific player carry no id and are formatted exactly as
before, so existing log greps keep working.

### TLS and pinning

Three of the four links in this system are encrypted. The fourth is not, on
purpose.

| Link | Transport | Who checks whom |
| --- | --- | --- |
| launcher → login | TLS 1.2+ | launcher pins the login server |
| game → realm | TLS 1.2+ | game pins the realm |
| realm ↔ world | TLS 1.2+, mutual | realm pins the world **and** the world pins the realm |
| game → world | plaintext | — |

The game↔world hop carries positions, damage numbers and chat. Watching it
yields roughly what standing next to the player would, and it is the one link
running at 20Hz for every player at once. Everything worth capturing —
credentials, the session key, world-entry tickets, the shared server auth key,
and every character create/delete — travels on one of the other three.

Nothing here has a certificate from a public CA, so there is no chain to verify
and chain verification would decide nothing. Identity is a **pin**: SHA-256 over
the certificate's DER SubjectPublicKeyInfo, provisioned ahead of time and
checked after every handshake. A peer whose key is not pinned is refused
*before* any protocol byte is written — which is the point, since the first
thing written on two of these links is a credential.

Six certificate files and four pin files:

```
login_server/certs/server.{crt,key}     mmo_client/Launcher/certs/login_pins.txt
realm_server/certs/server.{crt,key}     mmo_client/Game/certs/realm_pins.txt
world_server/certs/server.{crt,key}     realm_server/certs/world_pins.txt
                                        world_server/certs/realm_pins.txt
```

`make setup` generates and installs all of them. Set `MMO_CLIENT_ROOT` if the
client tree is not checked out beside this one, or setup will print the two
client pins for you to install by hand.

Produce a pin line for a certificate with:

```sh
openssl x509 -in server.crt -pubkey -noout \
  | openssl pkey -pubin -outform der \
  | openssl dgst -sha256 -binary | openssl base64
```

and write the result as `sha256/<base64>` into the pin file.

It must be the **SubjectPublicKeyInfo** digest, which is what that pipeline
produces. OpenSSL's `X509_pubkey_digest()` hashes the public key bit string
without the algorithm identifier around it and yields different bytes for the
same certificate; every pinning site in both trees once called it, so every pin
check compared a digest of one thing against a file naming another and refused
every peer. `tests/cert_pin_digest_test.c` pins a fixed certificate against a
digest produced by the command line above, and `tests/check_pinning_sync.sh`
keeps the computation identical in both trees.

A pin file may hold more than one line. That is how a key is rotated: publish a
client carrying both the current and the next pin, roll the server, then drop
the old line. A pin file that fails to parse is a **startup failure**, not a
skipped line, and an empty pin set accepts nobody rather than everybody.

Every service refuses to start without its certificate and its pin file. There
is no plaintext fallback anywhere: a silent downgrade would leave an operator
believing a link was encrypted while the session key went out in the clear.

Startup on a healthy stack looks like this:

```
[TLS] Server context ready (TLS 1.2+)                       # realm, client link
[TLS] Client context ready (TLS 1.2+, presenting a certificate)
Realm↔world TLS ready (1 world key pinned)
[TLS] Mutual server context ready (TLS 1.2+, client certificate required)   # world
Realm link TLS ready (1 realm key pinned)
```

A refusal names the key it saw, so a mismatch is diagnosable without a packet
capture:

```
[TLS] Armeia presented public key 0614…a10a, which is not pinned — refusing.
```

### Securing Redis

Redis holds every session key, every single-use login token, every world-entry
ticket and every server-to-server authentication key. **Anything that can reach
port 6379 owns every account.** Redis ships with no password and, in many
distributions, no bind restriction.

Two things are required for any deployment that is not a single developer
machine:

1. **Bind it to an interface players cannot reach.** In `redis.conf`:

   ```
   bind 127.0.0.1
   protected-mode yes
   ```

2. **Require authentication.** Either the simple form:

   ```
   requirepass <a long random string>
   ```

   or, on Redis 6+, an ACL user restricted to the key prefixes this system
   uses (`session:*`, `ticket:*`, `auth_token:*`, `server_auth_key:*`).

Then give every service the credential:

| Variable | Meaning |
| --- | --- |
| `MMO_REDIS_PASSWORD` | Password sent with `AUTH`. Unset means no authentication, and every service warns loudly at startup. |
| `MMO_REDIS_USER` | ACL user name (Redis 6+). Unset uses the one-argument `AUTH` against `requirepass`. |

`scripts/start_servers.sh` passes both to the twelve child processes and exports
`REDISCLI_AUTH` for its own `redis-cli` calls, so:

```sh
MMO_REDIS_PASSWORD='...' bash scripts/start_servers.sh
```

is all that is needed. The same variable works for
`common/server_keys/generate_daily_server_keys.sh`, via `REDISCLI_AUTH`.

Reconnections re-authenticate: a connection rebuilt after a Redis restart is a
new connection and arrives unauthenticated, which is the usual way an
AUTH-protected deployment appears to work until the first restart.

### Server-to-server authentication key

The realm authenticates to each world with a shared key held in Redis under
`server_auth_key:global` and `server_auth_key:<WorldName>`. **The realm refuses
to start when no global key is provisioned.** There is no built-in default: a
credential compiled into the source is a credential everybody has.

`scripts/start_servers.sh` provisions one automatically when Redis has none.
Provision or rotate manually with:

```sh
bash common/server_keys/generate_daily_server_keys.sh
```

Keys carry a 24-hour TTL, so this belongs in cron.

### The realm listener

The realm-to-world heartbeat link has **its own listening port**, separate from
the one players connect to. It used to share the player listener and be told
apart by peeking at the first byte of every accepted connection, which put a
privileged server-to-server handshake on the port the whole internet talks to
and made every player pay a syscall pair to prove they were not a realm.

By default a world's realm port is its client port **+ 1000** — Armeia listens
for players on 7778 and for its realm on 8778. An optional eighth column in
`setup/worlds.conf` overrides it per world. Both the world and the realm read
that same file, so the two cannot disagree.

Firewall the realm ports off your public interface, or bind them to a private
one with `realm_bind`. That is a stronger boundary than any of the settings
below, because the socket then does not exist where players can reach it.

Per-world settings, all in `world_server/world_config/*.conf`:

| Setting | Default | Meaning |
| --- | --- | --- |
| `realm_port` | client port + 1000, or `worlds.conf` column 8 | Port the realm link listens on. Change it in `worlds.conf` instead unless you have a reason not to — the realm reads that. |
| `realm_bind` | every interface | Address the realm listener binds to. Set it to the private interface when the realm is on a separate network from your players. |
| `realm_allow` | `127.0.0.0/8`, `::1`, and the world's own configured address | Repeatable. One address or CIDR block per line that may open the handshake. |
| `realm_max_handlers` | `4` | Ceiling on realm handler threads running at once. |

The default covers any deployment where the realm and its worlds share a host,
including one where `worlds.conf` names a routable address so remote clients can
reach the worlds — the realm's own connection then arrives from the host's
interface address rather than `127.0.0.1`, which is why the world's own
configured address is in the default set.

**A realm on a different machine from its worlds must be listed explicitly**, or
every world refuses its handshake and the world list shows them all offline.
Each world prints the allowlist in effect in its startup banner; check there
first if worlds appear offline.

### Session and ticket address binding

The realm link is TLS and the world link is plaintext (see
[TLS and pinning](#tls-and-pinning)). Encryption is not the whole story either
way: a session key also leaks from places that are not the wire — a log, a crash
dump, the client's own memory — so both credentials are additionally bound to
the address the issuing server observed:

- The login server records the client address in the session hash; the realm
  refuses a session key presented from a different address.
- The realm records the client address in the world-entry ticket; the world
  refuses a ticket redeemed from a different address.

A deployment that terminates client connections behind a NAT or proxy which
presents *different* source addresses to the login, realm, and world services
will see every login refused. Put the three services behind the same ingress
path, or terminate TLS and forward the original address.

## Keeping the two trees paired

`mmo_server` and `mmo_client` are separate repositories that share a wire
format, a world file format, and a set of content rules. Nothing in git links
them, so the pairing is recorded in a `PAIRED_RELEASE` file that both trees
carry and `make test` checks:

```sh
make test        # runs every cross-tree check, pairing included
```

**Check out both trees as siblings**, or set `MMO_CLIENT_ROOT`:

```text
somewhere/
├── mmo_server/
└── mmo_client/
```

The cross-tree checks are:

| Check | What it compares |
| --- | --- |
| `check_tree_pairing.sh` | The two `PAIRED_RELEASE` files. |
| `check_protocol_version.sh` | That a change to `protocol.h` bumped `PROTOCOL_VERSION`. |
| `check_protocol_sync.sh` | That both copies of `protocol.h` declare the same wire format. |
| `check_world_format.sh` | That `world.dat` matches the layout both readers expect. |
| `check_single_definition.sh` | That no shared constant is defined twice. |
| `check_client_content.sh` | That the client's content references resolve server-side. |

A check that cannot find the client tree **skips** — except
`check_tree_pairing.sh`, which fails when `MMO_CLIENT_ROOT` names a tree that
is not there. That distinction is deliberate: a server-only checkout is
legitimate, but a CI job that was *told* where the client is and could not read
it must not go green having compared nothing.

**When changing a shared surface**, bump `release` in *both* `PAIRED_RELEASE`
files in the same change. That is `protocol.h` (which also bumps
`PROTOCOL_VERSION`), the world file format, or anything the checks above read.

## Database notes

The login server creates the SQLite user database and its `users` table on first run at:

```text
database/databases/users_data/users.db
```

World names map to PostgreSQL databases such as `armeia_db`, `bosteuis_db`, and
`jatrus_db`. Connection settings come from `setup/worlds.conf`, which holds one
`key = value` line per setting and applies them to every world — only the
database name differs per world:

| Setting | Environment override | Default |
| --- | --- | --- |
| `pg_host` | `MMO_PG_HOST` | `localhost` |
| `pg_port` | `MMO_PG_PORT` | libpq's |
| `pg_user` | `MMO_PG_USER` | `postgres` |
| `pg_password` | `MMO_PG_PASSWORD` | none — prefer `~/.pgpass` |
| `pg_sslmode` | `MMO_PG_SSLMODE` | libpq's `prefer`, which accepts an unencrypted connection |
| `pg_options` | `MMO_PG_OPTIONS` | none |

**The two defaults worth changing.** `postgres` is the cluster superuser: a SQL
injection anywhere, or one compromised world process, reaches every other
world's database and the cluster itself. And with no `pg_sslmode`, libpq will
happily send the password and every character's data unencrypted.

`make setup` creates an `mmo_app` role with `SELECT`/`INSERT`/`UPDATE`/`DELETE`
on the world databases and nothing else, and writes its password to
`~/.pgpass`. It does **not** switch the servers over automatically, because the
servers create their own schema on first start and that needs table ownership.
To switch:

```sh
# once per world database, as postgres
psql -d armeia_db -c "ALTER TABLE characters          OWNER TO mmo_app;"
psql -d armeia_db -c "ALTER TABLE character_items     OWNER TO mmo_app;"
psql -d armeia_db -c "ALTER TABLE character_currencies OWNER TO mmo_app;"
```

then add to `setup/worlds.conf`:

```
pg_user = mmo_app
pg_sslmode = require      # whenever PostgreSQL is not on this host
```

Every service warns at startup while either default is still in place.

#### Item-instance identifiers

The world server seeds its item-instance allocator from
`MAX(instance_id)` in `character_items` at startup, and refuses to start when
that query fails. Any database that ran a build predating that seeding may hold
rows written by a counter that had restarted into a range already in use. Audit
and repair each world database once, with its world server stopped:

```sh
sudo -u postgres psql -d prototype_db -f scripts/audit_item_instance_ids.sql
# only if the audit reports inverted rows:
sudo -u postgres psql -d prototype_db -f scripts/repair_item_instance_ids.sql
```

Useful database targets include:

```sh
make show-dbs
make reset-world WORLD=armeia
make reset-all-characters
```

The reset targets delete character data. `make clean-db` deletes the SQLite account database and drops every world database.

## Other build targets

```sh
make
make clean
make rebuild
make help
```

`make clean` removes compiled binaries and object files. It does not delete databases. Run `make` again after changing world JSON data so the updated files are copied into `world_server/bin/data`.

## Project layout

```text
common/         Shared protocol, session, Redis, and database mapping code
database/       SQLite and PostgreSQL access code
login_server/   TLS account and session service
realm_server/   Character selection and world discovery service
world_server/   Gameplay simulation, collision data, zones, and other world data
scripts/        Full-stack start and stop supervisors
tests/          Development and integration tests
```

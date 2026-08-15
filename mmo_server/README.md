# Multiverse MMO Server

This repository contains the Linux server stack for Multiverse MMO. The stack consists of:

- A TLS login server on port `7776`. It handles registration, login, patch notes, and session creation.
- A realm server on port `7777`. It handles character selection and reports available worlds.
- Ten world server processes. They handle movement, combat, NPCs, parties, quests, shops, loot, and player state.

Redis stores sessions, game tickets, and server authentication keys. SQLite stores user accounts. PostgreSQL stores characters and world-specific player data.

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

### 4. Create the login database directory and TLS certificate

From the `mmo_server` root, run:

```sh
mkdir -p database/databases/users_data login_server/certs
openssl req -x509 -newkey rsa:2048 \
  -keyout login_server/certs/server.key \
  -out login_server/certs/server.crt \
  -days 3650 \
  -nodes \
  -subj "/CN=mmo-login"
chmod 600 login_server/certs/server.key
```

The certificate and private key are local files and are ignored by Git.

### 5. Create the PostgreSQL databases

```sh
make setup-all
```

This creates one PostgreSQL database and a `characters` table for each configured world. The command is safe to run again because it preserves existing databases and tables.

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

The Windows client defaults to the matching local endpoints:

```ini
login_ip=127.0.0.1
login_port=7776
game_ip=127.0.0.1
game_port=7777
```

These values can be placed in `mmo_client/Launcher/bin/server.conf`.

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
- `realm_server/worlds/worlds.txt` lists the worlds shown to clients.
- `world_server/world_config/*.conf` sets each world's name, region, address, capacity, and hardcore flag.
- `world_server/data/world.dat` contains the collision grid used for server-side movement validation.
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

For remote clients, update both `realm_server/worlds/worlds.txt` and the world configuration files with reachable addresses. Also configure the client launcher and allow the required ports through the host firewall.

Redis defaults to `127.0.0.1:6379`. The server code accepts `REDIS_HOST` and `REDIS_PORT` environment variables when Redis runs elsewhere.

## Database notes

The login server creates the SQLite user database and its `users` table on first run at:

```text
database/databases/users_data/users.db
```

World names map to PostgreSQL databases such as `armeia_db`, `bosteuis_db`, and `jatrus_db`. The connection code uses the PostgreSQL `postgres` account on `localhost` and reads its password from `PGPASSWORD`.

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

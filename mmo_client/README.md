# Multiverse MMO Client

This repository contains the Windows client for Multiverse MMO. It builds two C applications:

- `Launcher.exe` handles account registration, login, patch notes, server configuration, and game startup.
- `Game.exe` renders the game with OpenGL and GLFW, connects to the realm and world services, and provides character selection, combat, inventory, quests, shops, dialogue, audio, and settings.

The client expects compatible MMO server services to be running. By default, it connects to `127.0.0.1:7776` for login and realm traffic, then uses `127.0.0.1:7777` for game traffic.

## Prerequisites

Build the client on Windows from an MSYS2 MinGW 64-bit shell. The Makefile uses Windows commands and libraries, plus fixed `/mingw64` paths for OpenSSL.

Install the following software:

- Windows
- [MSYS2](https://www.msys2.org/)
- Git, if you plan to clone the repository from the command line

Open the **MSYS2 MinGW 64-bit** shell and install the build tools and OpenSSL:

```sh
pacman -Syu
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-make mingw-w64-x86_64-openssl
```

If `pacman -Syu` asks you to close the terminal, reopen the MinGW 64-bit shell, run `pacman -Syu` again, then install the packages.

GLFW headers and static libraries are included under `Game/include/GLFW` and `Game/lib`. No separate GLFW package is required.

## Build

From the repository root, run:

```sh
mingw32-make
```

You can also use the Windows wrapper:

```bat
make.bat
```

The build creates:

```text
Launcher/bin/Launcher.exe
Game/bin/Game.exe
```

Available build targets are:

```sh
mingw32-make launcher
mingw32-make game
mingw32-make clean
mingw32-make clean-launcher
mingw32-make clean-game
```

The launcher links OpenSSL statically. If the linker cannot find OpenSSL, confirm that these files exist in the MinGW environment:

```sh
ls /mingw64/lib/libssl.a /mingw64/lib/libcrypto.a
```

## Server configuration

The launcher reads `Launcher/bin/server.conf` at startup. Create that file when the server does not use the local defaults:

```ini
login_ip=127.0.0.1
login_port=7776
game_ip=127.0.0.1
game_port=7777
```

Replace the addresses and ports with those exposed by your server. If the file is absent, the launcher uses the values shown above.

## Run

Start the required server services first. Then run the client from the repository root:

```sh
mingw32-make run
```

You can also build first and launch the executable directly:

```sh
./Launcher/bin/Launcher.exe
```

Keep the repository root as the current working directory. The game loads fonts, sprites, maps, JSON data, audio, and settings through paths under `Game/`.

Register or sign in through the launcher, then select **Start Game**. The launcher requests a session from the server and passes it to the game process. Starting `Game.exe` by itself does not create an authenticated session.

## Default controls

- `W`, `A`, `S`, and `D` move the character.
- `Space` performs the basic attack.
- `1` through `5` activate abilities.
- `I` opens the inventory.
- `C` opens the character screen.
- `J` opens the quest log.
- `P` leaves the current party.
- `Enter` opens chat.
- `F11` toggles fullscreen mode.

To change gameplay bindings, create `Game/data/keybinds.cfg` and assign supported key names to the binding names defined in `Game/src/core/keybinds.c`. Restart the game after editing the file. Display and audio settings are stored in `Game/data/settings.cfg`.

## Project layout

```text
common/          Shared client protocol and version definitions
Launcher/        Native Windows launcher source and headers
Game/src/        Game source, grouped by system
Game/include/    Game headers and vendored headers
Game/Sprites/    Fonts, sprites, and other visual resources
Game/assets/     UI atlas data
Game/data/       Maps, gameplay data, and local settings
Game/lib/        Vendored GLFW libraries
```

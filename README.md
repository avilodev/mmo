# Multiverse MMO

Client and server for Multiverse MMO, in one repository.

| Directory | What it is |
|---|---|
| `mmo_client/` | Windows client: `Launcher.exe` and `Game.exe` (C, OpenGL, GLFW). See [mmo_client/README.md](mmo_client/README.md). |
| `mmo_server/` | Login, realm and world servers (C, PostgreSQL, Redis). See [mmo_server/README.md](mmo_server/README.md). |
| `Next_steps/` | Design and refactor plans (e.g. the 3D refactor). |
| `Story/` | World bible, story and content drafts. |

Until 2026-09-26 the client and server were separate repositories
(`avilodev/mmo_client`, `avilodev/mmo_server`). Their full history is kept
here under the two directories.

## After cloning

Point git at the tracked pre-commit hook once (it rejects trailing whitespace):

```sh
git config core.hooksPath .githooks
```

Build and test each tree from its own directory (`make`, `make test`), as its
README describes. The cross-tree checks in `mmo_server/tests/` find the client
beside the server automatically.

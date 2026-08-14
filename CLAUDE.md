# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

Qt 6 Widgets, C++20, no dependencies beyond Qt. There is no test suite and no formatter config.

```sh
# macOS (Homebrew Qt) — the configured build dir already points at /usr/local/opt/qt
cmake -S . -B build -DCMAKE_PREFIX_PATH=$(brew --prefix qt)
cmake --build build

# uConsole / Debian trixie
sudo apt install build-essential cmake qt6-base-dev qt6-connectivity-dev qt6-svg-dev \
    libqt6sql6-sqlite
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j4
```

The macOS build is a bundle (Bluetooth needs a usage description in `etc/Info.plist.in`), so the
binary is `./build/Corelet.app/Contents/MacOS/Corelet`. The uConsole build is a plain binary at
`./build/corelet`.

Sources are listed explicitly in `CMakeLists.txt` — a new `.cpp` must be added there or it silently
won't compile. `compile_commands.json` is a symlink into `build/`.

## Do not visually verify UI work

After implementing a UI feature, stop at "it compiles". Do not launch the app, render a widget to
PNG, build a screenshot harness, or otherwise inspect the result — the maintainer reviews the visual
outcome and gives feedback. Report what changed and hand it over.

## Running the app locally — read this first

On macOS `QStandardPaths::AppDataLocation` and `QSettings` resolve through Foundation, **not**
`$HOME`. Launching with `HOME=/scratch` still writes to the real
`~/Library/Application Support/singular0/corelet/history/<public-key>.sqlite3` and
`~/Library/Preferences/com.singular0.corelet.plist`. That device database is the *only* copy of
received messages (see below), so a run against a stub daemon destroys real user data.

Combined with the rule above, there is essentially no reason to run the binary unprompted. If the
maintainer asks for a run, back up the affected device database first.

```sh
./build/corelet --host 10.0.0.4 --port 5099   # skips the connect dialog
./build/corelet --ble MeshCore-3f2a          # advertised name, or an adapter handle
```

## Architecture

Three layers under `src/`, strictly one-directional (`ui` → `model` → `protocol`):

- **`protocol/`** — wire constants, frame codec, transports, and `CompanionClient`, the state
  machine that owns the link.
- **`model/`** — `Channel`/`Message` value types, the SQLite `History`, and two `QAbstractListModel`s
  (`ChannelModel` for the sidebar, `ChatModel` for the open conversation).
- **`ui/`** — `MainWindow` wires client signals to models, plus `ConnectDialog`, two item delegates,
  the dark `theme`, and SVG icons in a `.qrc`.

This is a client only. All radio work, crypto and channel state live in `umeshcored`
(`../umeshcore`) or the device firmware.

### Constraints that shape the protocol layer

Violating any of these produces bugs that only show up against a real device:

- **Replies are untagged.** Nothing in a response identifies the command it answers, so exactly one
  command is in flight at a time and the rest queue in `CompanionClient::pump`. Never send two
  commands concurrently and match replies by shape.
- **Pushes (code `>= 0x80`) are interleaved** with replies and are routed in `handlePush` before the
  queue is consulted. `PUSH_MSG_WAITING` is what triggers message collection.
- **`SYNC_NEXT_MESSAGE` pops** from the daemon's inbox. The daemon is not storage; whatever the app
  collects it must persist to the device database or the message is gone. This includes direct messages,
  which have no view yet but are still stored under channel `-1`.
- **A channel's slot number is only its current wire address**, never its persistent identity or
  row. `GET_CHANNEL` answers for all 8 slots and unused ones have an all-zero key; slots are sparse,
  and rows shift on re-enumeration. Persistent state uses a fingerprint of the channel key.
- **The wire says nothing about channel kind.** `ChannelType` is deduced in
  `model::Channel::classify` from the key, mirroring how the daemon builds them (constant public
  key, SHA-256 of the name including `#` for hashtags). The offline cache stores the resolved type,
  never the key.
- **Sender names are unauthenticated.** Channel messages are `"Name: body"` text; anyone can claim
  any name. Never treat `Message::sender` as identity.
- **Our own sends never come back over the air** — they are echoed locally in
  `MainWindow::onSendResult` when the daemon acknowledges.
- **Only framing differs between links.** `Transport` hides it: TCP length-prefixes and de-frames
  incrementally, BLE is one Nordic UART write/notification per frame. Everything above `Transport`
  is link-agnostic; don't add transport branches to `CompanionClient`.
- The enums in `protocol/protocol.h` are wire numbers mirroring the daemon's
  `src/companion/frames.h`. Never renumber; append only.

### Persistent state

- `history/<public-key>.sqlite3` under `QStandardPaths::AppDataLocation` — one SQLite database per
  device, capped at `History::MaxPerChannel` (500) messages per channel. Rows are indexed by a
  SHA-256 fingerprint of the channel key; the device identity is carried by the filename.
- `QSettings` (org `singular0`, app `corelet`) holds global `geometry`, `splitter` and the
  `connection/*` target. Device content lives below `devices/<public-key>/`; channel cache entries
  below that are keyed by channel-key fingerprint and the selected channel is stored by the same
  fingerprint. The secret channel keys stay in the daemon.

## Git

Commit straight to `main` unless asked for a branch. Don't create a feature branch, and don't ask
for permission to commit to the default branch.

## Conventions

- `QT_NO_KEYWORDS` is defined: use `Q_SIGNALS` / `Q_SLOTS` / `Q_EMIT`, never the lowercase macros.
- Warnings are `-Wall -Wextra -Wpedantic`; keep new code clean.
- Files are named in `snake_case` (`chat_model.cpp`, not `chatmodel.cpp`) throughout.
- Members are trailing-underscore, 4-space indent, ~100 columns.
- User-visible strings use ASCII `...`, never the Unicode ellipsis `…`.
- Comments here explain *why* a thing is the way it is — a wire quirk, a uConsole constraint. Match
  that register rather than narrating what the code does.

### UI performance

The uConsole panel is 1280x480 — wide and very short. The chat is a `QListView` with a custom
delegate specifically so only visible rows lay out and paint; a `QTextBrowser` of HTML would
re-layout the whole conversation per message, which is visible on a CM4. Keep per-paint work out of
delegates (e.g. `ChannelModel` pre-flattens the sidebar preview string). Vertical space is the
scarce resource: padding is deliberately tighter than desktop defaults, and sidebar rows are one
line.

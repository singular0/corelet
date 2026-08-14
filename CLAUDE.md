# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

`README.md` is written for end users: what the app does, how to install a released package, the
command-line options, license. Keep maintainer detail — packaging internals, CI, protocol
constraints, UI rationale — here rather than there, and don't let the two drift into duplicating
each other.

## Build

Qt 6 Widgets, C++20, no dependencies beyond Qt. No formatter config. Tests are two CTest binaries
built under `BUILD_TESTING` (`tests/history_test.cpp`, `tests/chat_model_test.cpp`); run them with
`ctest --test-dir build`. `libqt6sql6-sqlite` is a Build-Depend as well as a runtime one because
the history test opens a real QSQLITE database and `dh_auto_test` fails without the driver plugin.

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

`debian/` packages the same tree for arm64 (uConsole) and amd64. `scripts/build-deb.sh` with no
argument runs `dpkg-buildpackage` natively; `deps` installs Build-Depends first; `arm64|amd64|all`
sets up a `debian:trixie` container and runs that same native path inside it, which is what works
from macOS. Output goes to `dist/`, each `.deb` beside the `-dbgsym` package holding its symbols;
install one as `sudo apt install ./dist/corelet_0.1.0_arm64.deb` so apt resolves the Qt runtime.
A foreign architecture runs under Docker's qemu binfmt handler, so an arm64 build on an x86 host
takes about half an hour against four minutes native — fine for a one-off, which is the only thing
it is for.

CI (`.github/workflows/debian.yml`) builds each architecture on its own runner rather than
cross-compiling or emulating: debhelper skips `dh_auto_test` when the host architecture differs
from the build one, so a cross-built package would ship untested. Don't add a cross path.

The package is source format `3.0 (native)`, so its version is `debian/changelog`'s and must be
kept level with `project(... VERSION ...)` — the script warns when they drift. Build-Depends in
`debian/control` are the single source of dependency truth: `apt-get build-dep` reads them, so no
list of Qt packages is repeated in the script, the workflow or the README.

Two version numbers with different jobs. `project(... VERSION ...)` and `debian/changelog` version
the *packages* — the `.deb`'s version, `CFBundleShortVersionString`, the DMG filename, the man page
— and are hand-bumped, level with each other. What the binary itself reports for `--version` and in
its title bar is the tag it was built against: `cmake/version.cmake` runs `git describe --tags
--dirty --match 'v[0-9]*'`, strips the `v` and writes `build/version.h`. That runs on every build
rather than at configure time, because tagging a commit changes nothing CMake would otherwise
notice; the header is rewritten only when the string changes, so an unchanged build compiles
nothing. A checkout with no tag to find — a source tarball, a shallow clone, the container `.deb`
build that drops `.git` — reports `0.0.0`, which is no release and reads as the unknown build it
is; it deliberately does not borrow `project(... VERSION ...)`, since a binary that cannot name the
tag it came from should not claim one. That is why both packaging workflows check out with
`fetch-depth: 0`: on a default shallow clone every release binary would report `0.0.0`.

`scripts/build-dmg.sh` builds the macOS disk image into `build/dmg/` — never into `build/`, which
has to keep linking against Homebrew Qt for development — runs `macdeployqt`, ad-hoc signs and
packages with `hdiutil`. Its version comes from `CMakeLists.txt` alone, so nothing can drift.
After `macdeployqt` it drops Qt's virtual-keyboard input context and the unusable QtPdf image
plugin, then sweeps `Frameworks/` for anything no remaining binary links, which is what keeps the
QML runtime out of a Widgets app: dropping the keyboard plugin orphans QtQuick and QtQml, which in
turn orphans QtQmlModels. Only `Frameworks/` is swept — plugins are opened by name at runtime, so
nothing links them and the same test would call every one of them garbage.

Three checks then run, and all three are load-bearing:

- **Plugins that must be present.** macdeployqt guesses plugins from linkage and says nothing when
  it guesses wrong. `libqcocoa` and `libqsqlite` are loaded by name, so a missing one is not a link
  error: no cocoa means the app starts with no window, no sqlite means history dies at runtime with
  "Driver not loaded".
- **No reference outside the bundle** — a framework macdeployqt did not copy stays an absolute path
  into the build machine's Homebrew prefix, which only fails on somebody else's Mac.
- **No dangling reference** (`check_no_dangling`). Don't remove it: over-pruning surfaces only as a
  dyld failure on a user's machine, never as a build error.

The image is `hdiutil`-built from a staging folder holding the app beside a symlink to
`/Applications` — the drag-to-install layout with no dependency past what macOS ships.

`.github/workflows/macos.yml` builds arm64 on `macos-15` and x86-64 on `macos-15-intel`; there is no
universal binary because a Homebrew Qt is thin, and no `create-dmg` because it positions icons over
AppleScript, which needs a GUI session a runner does not have. Both runners are macOS 15, so the
DMGs want macOS 15 or newer — the bundled Qt is a Homebrew bottle built for that release.
`macos-15-intel` is the last x86-64 image GitHub will offer and goes away in August 2027.

Signing runs after `macdeployqt` (which rewrites load commands) and inside-out (a nested binary
signed after its container breaks the container's seal); don't add `--options runtime`, which only
means something under notarization. Ad-hoc is the floor rather than a nicety — Apple Silicon
refuses to execute an unsigned Mach-O — but it does not satisfy Gatekeeper, and since macOS 15 the
Control-click bypass is gone, so users clear quarantine by hand (the README says how). Two
consequences: macOS keys the Bluetooth permission to the bundle's code identity, which an ad-hoc
signature changes on every build, so the prompt can reappear or stick as denied; and a Homebrew
cask would not help, because casks quarantine by default. Both dissolve with a paid Developer ID,
which turns this into `codesign --options runtime -s "Developer ID Application: ..."`,
`xcrun notarytool submit --wait` and `xcrun stapler staple`.

Pushing a version tag is the whole release process — bump `CMakeLists.txt` and `debian/changelog`
first, then `git tag v0.2.0 && git push origin v0.2.0`. The release publishes all four packages
plus a `SHA256SUMS` file, which is the only verification a download has since nothing is notarized
or signed against a key a stranger can check. Both `-dbgsym` packages stay out of the release —
they are for debugging a build you already have and would double the asset list — but CI still
uploads them as build artifacts on every push.

`debian.yml` and `macos.yml` build on push and PR and are also `workflow_call`-able;
`.github/workflows/release.yml` is the only thing triggered by a tag and calls both, so a release
is built by the same jobs CI already runs and a tag never builds twice. Neither packaging workflow
should get a tag trigger back. Releases are `v` + semver only: the `tags:` glob is the closest
GitHub's filter syntax gets, and the `gate` job re-checks with semver's regex and *skips* the run
on a non-match — an unrecognised tag is not a failure. A tag whose version is ahead of
`debian/changelog` or `project(... VERSION ...)` does fail, because neither package takes its
version from the tag and the release would carry the previous version's files.

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
- **`ui/`** — `MainWindow` wires client signals to models, plus `ConnectDialog`, `NodePane`, the
  add/share channel dialogs, two item delegates, the dark `theme`, and SVG icons in a `.qrc`
  (Lucide, ISC — some derived from Feather and also MIT; see `src/ui/icons/LICENSE` and
  `debian/copyright`, which are what the README's license section points at).

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
- **BLE is the Nordic UART service** — `6e400001-...`, writing `...0002` and subscribing `...0003`.
  A device handle is whatever the local adapter gives it, an address under BlueZ and an opaque
  per-host UUID under CoreBluetooth, so the advertised name is stored alongside it and used to
  re-find the device when the handle goes stale.
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
scarce resource: padding is deliberately tighter than desktop defaults, and a sidebar row is two
tight lines rather than the three a desktop client would spend.

### UI behaviour that is deliberate

- Messages anchor to the bottom, and new ones auto-scroll **only when the view is already at the
  bottom** (`MainWindow::appendToView`) — yanking the view while the user reads back is worse than
  a missed jump. An accent divider marks the first unseen message when returning to a channel or
  when traffic arrives while the view is scrolled back.
- A sidebar row is a channel-type icon plus two lines: the name, then the newest message with its
  stamp and an unread pill. `ChannelDelegate::activityStamp` gives the clock time alone for
  anything inside 24 hours and prepends `d MMM` beyond that — a channel list is scanned rather
  than read, and a bare time on a week-old row means nothing. Mesh timestamps can sit slightly in
  the future when a sender's clock runs fast, which still counts as just now.

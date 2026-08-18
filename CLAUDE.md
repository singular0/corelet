# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

`README.md` is written for end users: what the app does, how to install a released package, the
command-line options, license. Keep maintainer detail — packaging internals, CI, protocol
constraints, UI rationale — here rather than there, and don't let the two drift into duplicating
each other.

## Build

Qt 6 Widgets, C++20, no dependencies beyond Qt. No formatter config. Tests are two CTest binaries
built under `BUILD_TESTING` (`tests/history_test.cpp`, `tests/chat_model_test.cpp`) plus
`tests/version_test.cmake`, which drives the version resolver over throwaway repositories rather
than compiling anything; run them with `ctest --test-dir build`. `libqt6sql6-sqlite` is a Build-Depend as well as a runtime one because
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
install one as `sudo apt install ./dist/corelet_*_arm64.deb` so apt resolves the Qt runtime.
A foreign architecture runs under Docker's qemu binfmt handler, so an arm64 build on an x86 host
takes about half an hour against four minutes native — fine for a one-off, which is the only thing
it is for.

CI (`.github/workflows/debian.yml`) builds each architecture on its own runner rather than
cross-compiling or emulating: debhelper skips `dh_auto_test` when the host architecture differs
from the build one, so a cross-built package would ship untested. Don't add a cross path.

The package is source format `3.0 (native)`. `scripts/build-deb.sh` resolves Git once, copies the
source into a temporary tree, and prepends a generated `debian/changelog` stanza there; the
checked-in changelog is release history, not a version input. The staged tree carries the frozen
version manifest into CMake, so package metadata and the binary cannot observe different commits.
Build-Depends in `debian/control` are the single source of dependency truth: `apt-get build-dep`
reads them, so no list of Qt packages is repeated in the script, workflow or README.

Git is the only source of the current version. `cmake/version.cmake` accepts valid `v` + SemVer
release tags and produces one canonical identity plus the syntax-constrained Debian and Apple
forms. An exact `v1.2.3` reports `1.2.3`; later commits report `1.2.3-N-gHASH`; a Git checkout with
no reachable release tag reports `0.0.0-HASH`; and a source tree with no repository in it at all —
a downloaded tarball, or the staging tree the Debian package builds from — reports `0.0.0`.
Staged or unstaged tracked changes add `-dirty`; untracked build products do not. The
resolver generates the header, man page and Info.plist on every build and only rewrites changed
files, so tagging an already configured checkout updates its identity without forcing an otherwise
unchanged rebuild.

A tree that *does* have a `.git` and still yields nothing is a hard error rather than a `0.0.0`.
That distinction is what v0.1.0 was missing: Git refuses a repository owned by another user, which
is what a container job is — the workspace belongs to the runner account and the build runs as
root — so the Debian jobs resolved nothing, said so to nobody, and shipped debs stamped `0.0.0`
underneath a changelog that said `0.1.0`, beside correctly named DMGs. The resolver now passes
`-c safe.directory` for the source directory it was pointed at (`safe.directory` is read only from
protected scopes, so an environment variable would not do, and it must be the physical path, not a
route through a symlink), which fixes CI, the local container path and any `sudo` build in one
place. `release.yml` then re-checks the built filenames against the tag before publishing, because
every job resolving the version separately is exactly how they came to disagree.

Debian native versions cannot contain hyphens, so prereleases use `~` and development components
use `+`/`.` there. Apple's standard bundle fields are numeric: `CFBundleShortVersionString` uses
the tag's numeric core (or `0.0.0`) and `CFBundleVersion` uses the Git commit count (or `0`); the
full canonical string is also stored as `CoreletVersion`. Both packaging workflows check out with
`fetch-depth: 0`, otherwise a post-tag CI build would fall back to `0.0.0-HASH`.

`scripts/build-dmg.sh` freezes the Git manifest and builds the macOS disk image into `build/dmg/`
— never into `build/`, which has to keep linking against Homebrew Qt for development — runs
`macdeployqt`, ad-hoc signs and packages with `hdiutil`. The manifest versions the binary, bundle,
man page, DMG filename and volume label together.
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

Pushing a version tag is the whole release process: `git tag v0.2.0 && git push origin v0.2.0`.
The release publishes all four packages plus a `SHA256SUMS` file, which is the only verification a
download has since nothing is notarized
or signed against a key a stranger can check. Both `-dbgsym` packages stay out of the release —
they are for debugging a build you already have and would double the asset list — but CI still
uploads them as build artifacts on every run.

`debian.yml` and `macos.yml` build on PR, on `workflow_dispatch` and on `workflow_call`;
`.github/workflows/release.yml` is the only thing triggered by a tag and calls both, so a release
is built by the same jobs CI already runs and a tag never builds twice. Neither packaging workflow
should get a tag trigger back. Neither should get a push trigger back either: commits land straight
on `main`, so a push build would only report after the fact — dispatch the workflow by hand when
you want the packaging exercised before tagging. Releases are `v` + semver only: the `tags:` glob
is the closest GitHub's filter syntax gets, and the `gate` job re-checks with semver's regex and
*skips* the run on a non-match — an unrecognised tag is not a failure. It also checks that the
shared resolver sees the pushed tag exactly before starting the package jobs.

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
./build/corelet --socket /run/coreletd/companion.sock   # skips the connect dialog
./build/corelet --host 10.0.0.4 --port 5099
./build/corelet --ble MeshCore-3f2a                    # advertised name, or an adapter handle
```

## Architecture

Three layers under `src/`, strictly one-directional (`ui` → `model` → `protocol`):

- **`protocol/`** — wire constants, frame codec, transports, and `CompanionClient`, the state
  machine that owns the link.
- **`model/`** — `Channel`/`Contact`/`Message` value types, the SQLite `History`, and three
  `QAbstractListModel`s (`ChannelModel` for the sidebar, `ChatModel` for the open conversation,
  `ContactModel` for the address book).
- **`ui/`** — `MainWindow` wires client signals to models, plus `ConnectDialog`, `NodePane`, the
  add/share channel and contacts dialogs, three item delegates, the dark `theme`, and SVG icons in
  a `.qrc` (Lucide, ISC — some derived from Feather and also MIT; see `src/ui/icons/LICENSE` and
  `debian/copyright`, which are what the README's license section points at). Anything two lists
  have to say the same way is shared rather than copied: `row_format.h` for a last-heard stamp and
  a hop count, `Avatar` for the disc a name is drawn as.

This is a client only. All radio work, crypto and channel state live in `coreletd`
(`../coreletd`) or the device firmware.

### Constraints that shape the protocol layer

Violating any of these produces bugs that only show up against a real device:

- **Replies are untagged.** Nothing in a response identifies the command it answers, so exactly one
  command is in flight at a time and the rest queue in `CompanionClient::pump`. Never send two
  commands concurrently and match replies by shape.
- **Pushes (code `>= 0x80`) are interleaved** with replies and are routed in `handlePush` before the
  queue is consulted. `PUSH_MSG_WAITING` is what triggers message collection.
- **`SYNC_NEXT_MESSAGE` pops** from the daemon's inbox. The daemon is not storage; whatever the app
  collects it must persist to the device database or the message is gone. This includes direct messages,
  which have no view yet but are still stored under channel `-1`. Because the pop is destructive, the
  drain is gated on storage: `CompanionClient::setStorageAvailable` must be true before the first
  sync, `MainWindow::preflightStorage` sets it from a real open of the device database on
  `deviceInfoChanged`, and any `History` failure turns it back off. Both message signals are
  delivered directly, which is what makes the client's post-emit `requestSync()` a "the previous
  one committed" check rather than a hope. Nothing collected is lost by stopping — an uncollected
  message stays in the daemon's inbox — so never make the drain unconditional again.
- **A channel's slot number is only its current wire address**, never its persistent identity or
  row. `GET_CHANNEL` answers for all 8 slots and unused ones have an all-zero key; slots are sparse,
  and rows shift on re-enumeration. Persistent state uses a fingerprint of the channel key.
- **A contact push says only *who*.** `PUSH_ADVERT`, `PUSH_NEW_ADVERT` and `PUSH_PATH_UPDATED`
  carry the node's public key and nothing else, so what changed about it is a
  `GET_CONTACT_BY_KEY` of its own. `CompanionClient` allows one outstanding fetch per key: a busy
  mesh repeats an advert by several routes, and an advert that also moved the path pushes twice.
  Nothing about contacts is cached on this side, unlike channel names, which are kept so the
  sidebar has something to draw while offline — the daemon's store is the authority, and the
  address book is a window somebody opens rather than the view the app lives in.
- **The wire says nothing about channel kind.** `ChannelType` is deduced in
  `model::Channel::classify` from the key, mirroring how the daemon builds them (constant public
  key, SHA-256 of the name including `#` for hashtags). The offline cache stores the resolved type,
  never the key.
- **Sender names are unauthenticated.** Channel messages are `"Name: body"` text; anyone can claim
  any name. Never treat `Message::sender` as identity.
- **Text limits are byte budgets, never character counts.** `protocol/text_limits.h` derives them
  from the 184-byte mesh payload: a channel name is 32 encoded bytes, and a message body is what the
  184 leaves once the envelope, the 5-byte text header and the node's own `"Name: "` prefix are
  taken — which is why the budget depends on the name `SELF_INFO` reported and cannot be a constant.
  A Cyrillic letter costs two bytes and an emoji four, so a `QString` character count over-promises
  by up to four times, and the send comes back from the node as an unexplained `NOT_FOUND`.
  `CompanionClient` refuses an over-long name or body rather than letting `Writer::padded` cut one
  through the middle of a character; the UI reads the same rules, and neither end truncates what
  somebody typed.
- **Our own sends never come back over the air** — they are echoed locally in
  `MainWindow::onSendResult` when the daemon acknowledges.
- **Only framing differs between links.** `Transport` hides it: the two stream links length-prefix
  and de-frame incrementally, BLE is one Nordic UART write/notification per frame. Everything above
  `Transport` is link-agnostic; don't add transport branches to `CompanionClient`.
- **The Unix socket is the daemon's default endpoint**, TCP its opt-in, and the two carry the same
  length-prefixed stream. `StreamTransport` holds all of it — the `FrameReader`, the send path and
  the one-`closed()`-per-`open()` guard — and `TcpTransport`/`UnixTransport` supply only the three
  verbs `QTcpSocket` and `QLocalSocket` spell differently, since those classes share no base past
  `QIODevice`. A socket path must be absolute: Qt resolves a bare name against the temporary
  directory, so `ConnectTarget::isValid` rejects a relative one rather than letting it connect
  somewhere the user never named. With nothing remembered, `ConnectDialog::lastTarget` opens on the
  socket when one exists at the daemon's default path and on loopback TCP otherwise.
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
  Every operation returns a `HistoryResult` and a read returns it beside the rows, because an
  unreadable database and an empty one otherwise look identical and the difference is somebody's
  whole message history.
- WAL with `synchronous = NORMAL`. This was weighed against `FULL` and kept: in WAL mode `NORMAL`
  already survives the app being killed, crashing or torn down mid-write, because the WAL sits in
  the OS page cache and the kernel outlives the process, and WAL cannot corrupt the database the way
  a rollback journal at `NORMAL` can. Only a battery pull or a kernel panic within seconds of a
  commit loses anything, and `FULL` buys that back at one SD-card fsync per received message, on the
  write path of a backlog drain. Don't switch it without a measurement on the CM4. `PRAGMA
  user_version` is set on open purely as a *write*, so a read-only file or a full card fails the
  preflight rather than the first message.
- A message the app cannot place — no device identity yet, or a channel whose key is not in hand —
  goes to `History::orphanDeviceId()` / `History::orphanChannel(slot)` rather than being dropped:
  the node has already discarded its copy. Both are near-all-zero and so unreachable by a real
  32-byte public key or a real SHA-256 fingerprint, and the orphan conversation keeps the wire slot
  in its last byte, which is the only clue left if the key turns up later. Nothing reads these back
  yet (see the DM view, same situation).
- `QSettings` (org `singular0`, app `corelet`) holds global `geometry`, `splitter` and the
  `connection/*` target. Device content lives below `devices/<public-key>/`; channel cache entries
  below that are keyed by channel-key fingerprint and the selected channel is stored by the same
  fingerprint. The secret channel keys stay in the daemon.

## Git

Commit straight to `main` unless asked for a branch. Don't create a feature branch, and don't ask
for permission to commit to the default branch.

## Backlog

The backlog lives in GitHub issues (`gh issue list`) and nowhere else. There used to be a `TODO.md`
and a `FEATURES.md`; they were deleted once their contents became issues, because a checklist in the
tree and a tracker are the same list maintained twice and they drift within a week. Don't reinstate
either, and don't leave `TODO` comments in the code as a way of remembering work — open an issue and
reference its number if the code needs the pointer at all.

Issues carry three kinds of label, and every issue gets a priority and at least one area:

- `priority: P0` through `P4` — P0 is "the app is incomplete without this", P4 is someday.
- `area: protocol` / `area: ui` / `area: persistence` — where the work lands.
- `bug`, `enhancement`, `tech-debt` — the last one is structural work on code that already exists
  and behaves, as distinct from a defect.

Write an issue the way comments here are written: what is missing or wrong, *why* it matters, the
`file.cpp:line` references that anchor it, then a task checklist. Say what it depends on with an
issue number — the ordering between them is the part that is expensive to rediscover.

Keep it current as work lands. A commit that finishes an issue closes it (`Closes #12` in the
message); a commit that only moves it along ticks the boxes it completed. When you notice something
worth doing that is outside the current task, open an issue rather than widening the change.

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
- Every field with a byte budget carries a `ByteLimit` (`ui/byte_limit.h`), which is what
  `QLineEdit::setMaxLength` would be if it counted encoded bytes instead of characters — the whole
  reason it exists, since the same 32-byte field holds 32 Latin letters, 16 Cyrillic ones or 8
  emoji. Nothing about it is visible, deliberately: a field that has stopped taking characters is
  the signal a maximum length has always given, and a byte count on screen only invites the question
  of why an emoji costs four of them. Overflow comes back out from in front of the cursor rather
  than off the end of the field, so typing into a full field does nothing at all instead of pushing
  a character out somewhere the user is not looking, and `clampToUtf8Bytes` cuts between grapheme
  clusters — half a surrogate pair is not a character, and an accent without its letter is a
  different one. It is for a budget, not for any bounded field: a channel key is an exact size and a
  port a numeric range, and neither gets one.

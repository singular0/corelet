# Corelet

A MeshCore companion app for the [ClockworkPi uConsole](https://www.clockworkpi.com/uconsole),
talking to [`umeshcored`](../umeshcore) over the companion protocol on loopback, or straight to a
MeshCore device over Bluetooth LE.

Qt 6 Widgets, C++20, no dependencies beyond Qt itself. It is a client only: all radio work,
crypto and state live in the daemon or the device's firmware, so this process is a link and a
window.

## Status

| Area | State |
|------|-------|
| Channel list, per-channel history, send/receive | Working |
| TCP to a `umeshcored` | Working |
| Bluetooth LE to a device | Working |
| Message history across restarts | Working (the app owns it — see below) |
| Auto-reconnect, offline read-only mode | Working |
| Direct messages | Captured and stored, but not shown or sendable |
| Contacts, adverts, telemetry, settings | Not implemented |
| Adding or editing channels | Not implemented — edit the daemon's `channels` file |

Persisted state is isolated first by the node's public key and then by a fingerprint of each
channel key. Changing devices cannot expose another device's channels or messages, and moving a
channel to a different slot keeps its history with the channel.

## Building

On the uConsole (Raspberry Pi OS / Debian trixie):

```sh
sudo apt install build-essential cmake qt6-base-dev qt6-connectivity-dev qt6-svg-dev \
    libqt6sql6-sqlite
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j4
```

`qt6-connectivity-dev` is what brings in QtBluetooth; BLE also needs `bluez` running, which it is
by default. `qt6-svg-dev` is for the sidebar's channel-type icons, and `libqt6sql6-sqlite` supplies
the SQLite driver used for message history.

On macOS with Homebrew Qt, point CMake at it:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=$(brew --prefix qt)
cmake --build build
```

The Mac build is a bundle — macOS refuses Bluetooth to a process with no usage description, and
only a bundle has anywhere to declare one — so it runs from
`./build/Corelet.app/Contents/MacOS/Corelet`. The uConsole build is a plain binary named `corelet`.

## Debian packages

`debian/` builds a `corelet` package for the uConsole (arm64) and for an x86 desktop (amd64).
Nothing in it is architecture-specific; the difference is only where it runs.

On the uConsole, or any Debian machine you want a package for, build natively:

```sh
./scripts/build-deb.sh deps    # Build-Depends, read out of debian/control
./scripts/build-deb.sh
```

Off the target — from a Mac, or to build the uConsole's package on a desktop — name the
architecture and the script sets up a `debian:trixie` container and runs that same native build
inside it:

```sh
./scripts/build-deb.sh amd64
./scripts/build-deb.sh arm64      # the uConsole
./scripts/build-deb.sh all
```

A foreign architecture runs under the qemu binfmt handler Docker ships, so an arm64 build on an
x86 host takes about half an hour rather than four minutes — fine for a one-off, which is why CI
does it differently (below). Packages land in `dist/`, next to the `-dbgsym` package holding their
debug symbols; install one with `sudo apt install ./dist/corelet_0.1.0_arm64.deb`, which pulls the
Qt runtime with it.

There is no cross-compilation path on purpose. It would be faster than qemu, but debhelper skips
`dh_auto_test` whenever the host architecture differs from the build one, so a cross-built package
ships untested, and it would be a second build path to keep working for one small app.

`.github/workflows/deb.yml` builds both packages on each push, each on a runner of its own
architecture — `ubuntu-latest` and `ubuntu-24.04-arm`, both inside a `debian:trixie` container — so
neither emulation nor a cross toolchain is involved and both packages run their tests. GitHub's
arm64 runners are free for public repositories; on a private one without them, drop the container
job back to `./scripts/build-deb.sh arm64` under `docker/setup-qemu-action` and accept the wait.
Each job uploads its `.deb` as a build artifact.

The package version is `debian/changelog`'s, and the format is native, so a release is a changelog
entry with the same version as `project(... VERSION ...)` in `CMakeLists.txt`. The script warns if
the two have drifted.

## macOS disk image

The bundle CMake builds links straight into the Homebrew Qt prefix, so it only runs on the machine
that built it. `scripts/build-dmg.sh` produces one that doesn't:

```sh
./scripts/build-dmg.sh deps    # brew install cmake qt
./scripts/build-dmg.sh
```

It builds RelWithDebInfo into a tree of its own — the development `build/` keeps pointing at
Homebrew Qt, which is what you want while working on it — runs the tests, copies the bundle to a
staging folder, and lets `macdeployqt` pull the frameworks and plugins inside and rewrite the load
commands.

Then it takes most of that back out. macdeployqt copies plugin directories wholesale, and Qt's only
input-context plugin is a virtual keyboard that links the entire QML runtime; nothing here types
into an on-screen keyboard, and this app is Widgets specifically so it never loads QtQuick. Dropping
that one plugin leaves QtQuick and QtQml unreferenced, dropping those orphans QtQmlModels, and
dropping the QML stack orphans ICU, which on macOS nothing else uses — so the script sweeps
`Frameworks/` repeatedly for anything no remaining binary links, which takes the bundle from 94 MB
to 37 MB and leaves exactly the seven Qt frameworks `target_link_libraries` names. Only
`Frameworks/` is swept: plugins are opened by name at runtime, so nothing links them and the same
test would call every one of them garbage.

Three checks then run, in the order that makes the sweep safe to have done at all:

- **Plugins that have to be there.** macdeployqt picks plugins by guessing from the binary's
  linkage and says nothing when it guesses wrong. Both are loaded by name, so a missing one is not
  a link error: without `libqcocoa` the app starts with no window, and without `libqsqlite` history
  fails at runtime with "Driver not loaded".
- **No reference outside the bundle.** A framework macdeployqt did not know to copy stays an
  absolute path into the build machine's Homebrew prefix — the failure that shows up only on
  somebody else's Mac.
- **No reference the bundle cannot resolve.** The converse, and what keeps the sweep honest:
  over-pruning cannot show up as a build error, only as a dyld failure at launch on a user's
  machine.

The disk image is built by `hdiutil` from a staging folder holding the app beside a symlink to
`/Applications`, which is the drag-to-install layout with no dependency past what macOS ships.
`create-dmg` would give a nicer window, but it positions icons over AppleScript, which needs a
logged-in GUI session and so fails on a CI runner.

There is no universal binary. A Homebrew Qt is thin, so one would mean `lipo`-ing a tree of
frameworks by hand or switching to the official Qt installer; `.github/workflows/dmg.yml` builds
each architecture on a runner of that architecture instead, exactly as the Debian packages are
built. Both runners are macOS 15 so the two share a deployment target — which also means the DMGs
want macOS 15 or newer, since the bundled Qt is a Homebrew bottle built for that release.
`macos-15-intel` is the last x86-64 image GitHub will offer, and it goes away in August 2027. On a
tag the workflow opens a draft release with both DMGs attached.

### Ad-hoc signing

The bundle is signed, but with an ad-hoc signature rather than a Developer ID. That is the floor,
not a nicety: Apple Silicon refuses to execute a Mach-O carrying no signature at all. Signing runs
after `macdeployqt`, which rewrites load commands and would invalidate a seal applied first, and
inside-out, because signing a nested binary after its container breaks the container's signature.
The hardened runtime is deliberately not enabled — it is what notarization requires, and on an
ad-hoc signature it only adds restrictions.

What ad-hoc does not do is satisfy Gatekeeper. A downloaded DMG is quarantined, and since macOS 15
the Control-click → Open bypass no longer works, so the app has to be allowed once from
**System Settings → Privacy & Security → Open Anyway**, or:

```sh
xattr -dr com.apple.quarantine /Applications/Corelet.app
```

Two consequences worth knowing. macOS keys the Bluetooth permission to the bundle's code identity,
and an ad-hoc signature is a fresh identity on every build, so the BLE prompt can reappear after an
update or stick in a denied state — deleting the entry under Privacy & Security → Bluetooth clears
it. And a Homebrew cask would not help: casks quarantine by default. Both go away with a paid
Developer ID, which turns the extra steps into `codesign --options runtime -s "Developer ID
Application: ..."`, `xcrun notarytool submit --wait` and `xcrun stapler staple`.

## Running

Started with no arguments, the app asks what to connect to: a host and port, or a device picked
from a Bluetooth scan. It remembers the answer, and the node pane shows the current target; use
its connection button to disconnect or choose another target.

Naming a target on the command line skips the dialog:

```sh
./build/corelet --host 10.0.0.4 --port 5099
./build/corelet --ble MeshCore-3f2a      # advertised name, or an address
```

Over TCP the daemon must be running and its `companion_port` must match. That protocol has **no
authentication**, so the default is loopback; only point it at another host across a network you
control.

Over BLE the device must be in range and not already connected to another app — MeshCore firmware
serves one companion at a time. macOS asks for Bluetooth permission the first time; on Linux the
user needs to be able to talk to BlueZ, which the default `bluetooth` group grants.

`etc/corelet.desktop` is installed by `cmake --install` and by the Debian package, so the app shows
up in the uConsole's launcher. `corelet(1)` documents the options.

## How it talks to the daemon

Worth knowing before changing anything in `src/protocol/`:

- **Replies are untagged.** Nothing in a response says which command it answers, so exactly one
  command is in flight at a time and the rest queue behind it (`CompanionClient::pump`). Sending
  two commands and matching replies by shape would work until two commands shared a reply code.
- **Pushes are interleaved.** Codes `>= 0x80` are unsolicited and are routed before the queue is
  consulted. `PUSH_MSG_WAITING` is what drives collection of new messages.
- **`SYNC_NEXT_MESSAGE` pops.** Collecting a message removes it from the daemon's inbox, so the
  daemon is not storage — the app appends everything it collects to the device's SQLite database
  under `QStandardPaths::AppDataLocation/history/`. That database *is* your history; the daemon has
  no copy.
- **This includes direct messages.** v1 has no DM view, but running this app still drains DMs from
  the shared inbox. They are written to the device's database under channel `-1` rather than
  dropped, and the status bar reports when one arrives.
- **All 8 channel slots always answer.** `GET_CHANNEL` returns an entry for every slot; unused ones
  have an all-zero key, which is how the app tells configured channels apart. Slots are sparse and
  are only wire addresses. Persistent selection, cache entries and message history use a SHA-256
  fingerprint of the channel key instead, nested under the node's public key.
- **Nothing on the wire says what kind of channel a slot holds.** `GET_CHANNEL` answers with a name
  and a key and no more, so the sidebar's public / hashtag / private icon is deduced from the key
  exactly as the daemon builds them: the well-known public key is a constant, a hashtag key is
  SHA-256 over the name with the `#` included, and anything else arrived out of band. The offline
  channel cache stores the answer rather than the key, which stays in the daemon's state directory.
- **Channel messages carry the sender in the text.** They are sent as `"Name: body"` because a
  channel message has no per-sender key. The app splits that back out for display, which means
  **the sender name is unauthenticated** and anyone on the channel can claim any name.
- **Our own messages never come back.** A sent channel message is echoed into the local history
  when the daemon acknowledges it, or it would never appear.
- **Only the framing differs between links.** TCP is a byte stream, so commands carry the `<`
  length prefix and replies have to be de-framed incrementally. BLE is message-oriented: one GATT
  write is one command, one notification is one reply, and the prefix is absent. Everything above
  `proto::Transport` — the queue, the handshake, the sync drain — is the same either way.
- **BLE is the Nordic UART service.** `6e400001-…`, writing to `…0002` and subscribing to `…0003`.
  A device is identified by whatever handle the local adapter gives it, which is an address under
  BlueZ and an opaque per-host UUID under CoreBluetooth, so the advertised name is stored alongside
  it and used to re-find the device when the handle goes stale.

## Layout

```
src/
  protocol/   companion codes, frame codec, the TCP and BLE transports,
              the client and its command queue
  model/      channel + message types, persistent history, the two list models
  ui/         main window, connect dialog, the two list delegates, theme,
              icons/ (Lucide SVGs, ISC — see its LICENSE)
```

## Notes on the UI

The uConsole panel is 1280x480 — wide and very short. Padding is deliberately tighter than a
desktop default, the sidebar is narrow, and the chat is a `QListView` with a custom delegate so
only visible rows are laid out and painted. A `QTextBrowser` full of HTML would re-layout the whole
conversation on every message, which is noticeable on a CM4.

Messages anchor to the bottom of the pane. New ones only auto-scroll when you are already at the
bottom, so reading back is not interrupted by traffic. An accent divider marks the first unseen
message when returning to a channel, or when new traffic arrives while its view is scrolled back.

A sidebar row is one line: a channel-type icon, the name, when the channel last carried traffic,
and an unread pill. The stamp is the clock time for today, `dd/MM` for this year and `dd/MM/yy`
for anything older — short enough that the column stays narrow. There is no message preview: on a
480-row panel a second line halves how many channels fit, and the icon plus the time answer the
question a sidebar is actually scanned for.

## License

GPL-3.0-or-later, matching the daemon.

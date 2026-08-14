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

`etc/corelet.desktop` is installed by `cmake --install`, so the app shows up in the
uConsole's launcher.

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

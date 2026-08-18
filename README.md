# Corelet

A lightweight, cross-platform desktop companion app for [MeshCore](https://meshcore.co.uk) — a
chat window for your mesh node.

It reaches your node over Bluetooth LE straight to the device, or over a Unix socket or TCP to a
companion daemon. Either way it lists the node's channels and the people you talk to, shows their
history and sends and receives messages.

Corelet is a client and nothing more. All radio work, encryption and channel state live in the
device firmware or the daemon.

Built with C++ and Qt 6 and packaged for Debian and macOS.

## Features

Unchecked boxes are not implemented yet — use another client or the daemon for those.

**Messaging**

- [x] A sidebar of channels and direct conversations, each with its newest message, unread count
      and an icon for what it is.
- [x] Send and receive channel messages.
- [x] Send and receive direct messages. Start one from the **+** menu and pick who from your
      contacts.
- [x] Day markers and an unread marker, so a busy conversation opens on what is new.
- [x] History kept on disk per node and surviving restarts. Collecting a message removes it from
      the node's inbox, so **Corelet's database is the only copy of your messages.**
- [ ] Delivery confirmation for a direct message. The tick means your node accepted it, not that it
      arrived.

**Channels**

- [x] Create a private channel and share its key.
- [x] Join a private channel from a key someone sent you.
- [x] Join the public channel, or any hashtag channel.
- [x] Remove a channel from the node.

**Your node**

- [x] A node pane with its name, what you are connected to, and battery level.
- [x] A node info panel with the public key, position and radio settings.
- [x] A contacts window: everyone the node has heard from, with their public key, when their last
      advert arrived and how far it travelled — and the place you pick somebody to message.
- [ ] Telemetry.
- [ ] Changing device settings.

**Connection**

- [x] Bluetooth LE to a device, or a Unix socket or TCP to a daemon, remembered after the first
      launch.
- [x] Reconnects on its own when the link drops.
- [x] Stays readable from a local cache while the node is unreachable.

## Background

I got a [ClockworkPi uConsole](https://www.clockworkpi.com/uconsole), wanted a MeshCore client for
it, and didn't much like the ones that already existed — mostly a question of weight and of how
they use a screen this shape.

So this is the one I wanted. It is also an experiment: Corelet was written end to end with agentic
LLM tooling, and **not one line of its code was typed by hand.** Every design decision, review and
correction was mine; none of the typing was.

## Install

Pre-built packages for each release are on the
[Releases page](https://github.com/singular0/corelet/releases).

### Debian, Raspberry Pi OS and the uConsole

Take `arm64` for the uConsole or a Raspberry Pi, `amd64` for an x86 desktop. The packages are built
for Debian 13 (trixie) and want that or newer, since they use the system's Qt 6.

```sh
sudo apt install ./corelet_*_arm64.deb
```

Install the *file path*, not the bare name — that leading `./` is what lets apt pull the Qt runtime
in as a dependency.

Corelet then appears in the application launcher. Bluetooth needs your user to be able to talk to
BlueZ, which membership of the default `bluetooth` group grants.

### macOS

Take `arm64` for Apple Silicon or `x86_64` for an Intel Mac, open the disk image and drag Corelet
to Applications. **macOS 15 (Sequoia) or newer** is required.

#### The app is ad-hoc signed, so the first launch takes an extra step

Corelet is signed, but with an ad-hoc signature rather than a paid Apple Developer ID, and it is
not notarized. Gatekeeper will refuse to open it the first time. Two ways past it:

- Try to open the app, then go to **System Settings → Privacy & Security**, find the message about
  Corelet being blocked, and click **Open Anyway**.
- Or clear the download quarantine flag yourself:

  ```sh
  xattr -dr com.apple.quarantine /Applications/Corelet.app
  ```

One knock-on effect worth knowing: macOS ties the Bluetooth permission to an app's code identity,
and an ad-hoc signature is a new identity on every build. After updating, the Bluetooth prompt may
come back, or the permission may appear stuck as denied. Deleting Corelet's entry under
**Privacy & Security → Bluetooth** clears that and lets it ask again.

## Running

Started with no arguments, Corelet asks what to connect to — the daemon's socket, a host and port,
or a device picked from a Bluetooth scan — and remembers the answer. The connection button in the
node pane disconnects or points it somewhere else.

Naming a target on the command line skips that dialog:

```sh
corelet --ble MeshCore-3f2a          # advertised name, or an address
corelet --socket /run/coreletd/companion.sock
corelet --host 10.0.0.4 --port 5099
```

| Option | Meaning |
|--------|---------|
| `-b`, `--ble <device>` | Reach a MeshCore device over Bluetooth LE, by advertised name or by the address this machine knows it as. |
| `-s`, `--socket <path>` | Unix socket the daemon listens on, matching its `companion_socket`. Must be absolute. Default `/run/coreletd/companion.sock`. |
| `-H`, `--host <host>` | Host running the MeshCore daemon. Default `127.0.0.1`. |
| `-p`, `--port <port>` | Companion port on that host, matching the daemon's `companion_port`. Default `5000`. |
| `-h`, `--help` | Usage summary. |
| `-v`, `--version` | Version. |

Name one target, not several: `--socket`, `--host`/`--port` and `--ble` are alternatives.

`man corelet` has the same reference on Linux.

Three things to expect. Over BLE, the device has to be in range and not already connected to
something else — MeshCore firmware serves one companion app at a time. Over a Unix socket, access
is decided by the socket's group, so `permission denied` means your user is not in it
(`sudo adduser "$USER" coreletd`, then log out and back in). And note that the companion protocol
has **no authentication whatsoever**: the socket's permissions are the only access control there
is, the daemon listens on loopback when it is put on TCP instead, and you should only point Corelet
across a network you control.

## Troubleshooting

If emoji appear as squares on Debian, Raspberry Pi OS or the uConsole, install the emoji font:

```sh
sudo apt install fonts-noto-color-emoji
```

## Build from source

Qt 6 and a C++20 compiler, no other dependencies.

**Debian / Raspberry Pi OS:**

```sh
sudo apt install build-essential cmake qt6-base-dev qt6-connectivity-dev qt6-svg-dev \
    libqt6sql6-sqlite
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j4
```

The binary is `./build/corelet`; `sudo cmake --install build` adds the launcher entry and man page.

**macOS**, with Qt from Homebrew (`brew install cmake qt`):

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=$(brew --prefix qt)
cmake --build build
```

This one is an app bundle — macOS refuses Bluetooth access to anything that can't declare why it
wants it — so it runs from `./build/Corelet.app/Contents/MacOS/Corelet`. It links against your
Homebrew Qt and so only runs on the machine that built it; `scripts/build-dmg.sh` makes a
self-contained disk image instead.

`ctest --test-dir build` runs the tests. Packaging scripts for both platforms live in `scripts/`,
and `CLAUDE.md` documents how they and the CI workflows fit together.

## License

Corelet is free software under the **GPL-3.0-or-later**. There is no warranty; see the license
for the exact terms.

The icons used are [Lucide](https://lucide.dev), used under the ISC license — some of them
derive from Feather and carry its MIT terms too. `src/ui/icons/LICENSE` and `debian/copyright` have
the details.

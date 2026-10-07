# Zurbax

A desktop operating system written from scratch in C. The Linux kernel is
the only outside code.

There's no glibc, no X11 or Wayland, no GTK or Qt, no GRUB and no
distribution underneath. Zurbax has its own pieces for everything:
- the C runtime,
- graphics engine and font,
- display server and desktop,
- toolkit,
- init system,
- network stack with TLS,
- sound server and decoders,
- installer and disk tools,
- every app.

The look takes its cues from elementary OS: a calm top bar, a dock, rounded
windows with header bars and readable text. The focus is on games and on
making the desktop your own.

## What it does

**Boot.** The firmware's own logo (the ASUS logo on an ASUS PC) stays on
screen from power-on to the desktop, with a spinner under it. The kernel
has the whole system inside it, and the firmware starts it directly, with no
boot loader in between.

**Desktop.** A top bar with an applications menu, clock, quick settings
(Wi-Fi networks, sound, brightness, night light, Do Not Disturb) and power
menu. A dock with running indicators that hides out of the way, plus
notifications with sounds, a lock screen and screenshots (Print Screen).
Game mode, an FPS counter and keyboard layouts (US, UK, Romanian, German,
French, Spanish) are built in.

**Game controllers.** Xbox, PlayStation, Switch Pro, Steam and other USB
controllers work the moment they're plugged in, up to four at once. The
Guide button opens the applications menu, and the D-pad, A and B move
around it. Every app can be used with a controller: the D-pad and left
stick act as the arrow keys, A as Enter and B as Escape. The Arcade's games
have proper controller layouts, and Settings → Gaming shows a live picture
of each controller to test its buttons and sticks.

**First start.** The Welcome assistant covers keyboard, time zone, an
account and a look. Every step can be skipped. When started from the USB
stick, it offers to try Zurbax or install it.

**Installing.** The Installer either erases a disk or installs into its
free space next to Windows, and adds a "Zurbax" entry to the firmware's
boot menu. It takes about a minute.

**Apps**

| App | What it is |
|-----|------------|
| Drift | Web browser with its own HTML and CSS engine: tabs, bookmarks, history, search, forms, find, zoom |
| Harbor | App store: install, update and remove apps from the built-in source or signed web sources |
| Mail | Email for Gmail, Outlook, iCloud, Yahoo or any IMAP account: HTML mail, attachments, search, drafts, undo |
| Dashboard | Your day at a glance: weather, calendar, recent files, screen time |
| Camera | Photos from the webcam, with looks and a timer |
| Help | A searchable guide to Zurbax |
| Arcade | Blocks, Snake, 2048, Mines and Breakout, with best scores; keyboard, mouse or controller |
| Music | Albums, songs and artists from your Music folder (FLAC, Ogg Vorbis, WAV) |
| Weather | Live forecasts with an hourly strip, the week and details |
| Photos | Viewer for PNG, JPEG, GIF and BMP |
| Vault | Files, with thumbnails |
| Forge | Terminal running the Flint shell, with `sudo` |
| Writer | Text editor |
| Notes, Calendar, Clock, Calculator | The everyday tools |
| Screen Time | Daily app usage, limits, break reminders and downtime |
| Pulse | System monitor |
| Screenshot | Capture the screen, a window or an area |
| Settings | Appearance, wallpaper, display, sound, network, keyboard, mouse, power, users, about |
| Welcome, Installer | Setup and installation |

**In Harbor** (installed when you want them): Chess, with a computer
opponent from Easy to Expert; Sudoku, with fresh puzzles that each have one
solution; and Paint, for sketching and marking up pictures.

**Mail** blocks pictures from the web until you ask for them, since loading
them tells the sender you opened the message. It shows HTML mail with
Drift's engine but never runs scripts or loads outside style sheets. It
won't send your password over a connection that isn't encrypted. Gmail
needs an app password (Mail explains how when you type a Gmail address).
`tools/mailtest.py` runs a pretend mail server for trying it out.

**Under the hood**
- **TLS 1.3** with certificate checks against Mozilla's root list.
- **HTTP** downloads, and the `fetch` command.
- **Wi-Fi** with WPA2-Personal and open networks: znet runs the WPA2
  handshake itself over the kernel's nl80211 interface, and images carry the
  firmware for common Intel, MediaTek and Realtek laptop chips.
- **DHCP networking** on wired and wireless connections.
- **A sound mixer** that drives the sound card directly.

## Building

```sh
make            # everything: libraries, system programs, apps
make test       # the test suites (runtime, graphics, toolkit, TLS/crypto, audio)
make image      # the kernel with the system inside, and build/zurbax.img
make run        # boot build/zurbax.img in QEMU with UEFI firmware
```

A C compiler and `make` are enough for the system itself. Building the
kernel also needs the usual kernel build tools (`bc`, `flex`, `bison`,
`libelf`, `libssl` headers). `kernel/build.sh` downloads Linux on first use.

To try it on a real PC, write `build/zurbax.img` to a USB stick and boot
from it in UEFI mode. On ASUS machines, press F8 or Esc for the boot menu.
GitHub Actions builds the image on every push (see the `zurbax-image`
artifact).

## Layout

| Path | What it is |
|------|------------|
| `libzr/` | The runtime: syscalls, memory, strings, files, time zones, processes, events, JSON, SHA-256 |
| `libzg/` | Graphics: rasterizer, gradients, blur, PNG/JPEG/GIF/BMP, SVG icons, the font |
| `libzui/` | The toolkit: widgets, layout, text editing, dialogs, sound client |
| `libzn/` | Networking: crypto, X.509, TLS 1.3, DNS, HTTP |
| `libza/` | Audio files: WAV, FLAC, Ogg Vorbis, tags and cover art |
| `libzd/` | Disks: GPT, FAT32, ext4, cpio |
| `system/` | zinit (process 1), zdisplay (display server and desktop), znet, zaudio, zinstall, harborctl, the Flint shell, sudo, fetch, play |
| `apps/` | The apps |
| `store/` | Apps offered in Harbor, packaged by `mkimage repo` |
| `kernel/` | Kernel configuration and build script |
| `tools/` | Image builder, QEMU test drivers (with a simulated access point and a virtual controller), development helpers |
| `data/` | Icons, wallpaper, trusted root certificates, Harbor's trusted keys |
| `tests/` | Test suites and fixtures |

## Not there yet

- **WPA3-only and enterprise Wi-Fi.** WPA2-Personal and open networks work
  (including routers in WPA2/WPA3 mixed mode); networks that require WPA3
  (SAE) or 802.1X logins don't yet.
- **JavaScript.** Drift shows pages as their HTML and CSS describe them;
  sites that only work through scripts (web apps, most video sites) don't.
  WebP pictures don't show yet.
- **MP3 and AAC playback.** These need the formats' large fixed tables;
  FLAC, Ogg Vorbis and WAV play.
- **3D graphics.** The desktop is drawn in software and works on any GPU
  through the firmware's framebuffer. Hardware-accelerated 3D for big
  games would need GPU drivers in user space, which Zurbax doesn't have.
- **Windows programs.** `.exe` files don't run.

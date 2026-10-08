# ZOS 2.0 "Golden Rain"

A desktop operating system written from scratch in C. The Linux kernel is
the only outside code.

**ZOS 2.0, code name Golden Rain, is the release for playing:** game
controllers everywhere, Glide (a 3D game drawn by ZOS's own renderer),
controller vibration, F11 full screen, and the Golden Rain wallpaper. It
also brings Wi-Fi and Mail. (Inside the source, programs, paths and
settings still carry the project's first name, `zurbax`.)

There's no glibc, no X server or libwayland, no GTK or Qt, no GRUB and no
distribution underneath. ZOS has its own pieces for everything:
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
menu. The dock is the Arc: apps sit in round glass bubbles along a gentle
curve at the bottom of the screen, higher in the middle like the sun over
the horizon, strung on a golden thread. Running apps wear a golden ring with
a pip per window, the app in front glows under a raindrop, and a starting
app has a golden arc running round it; the dock steps aside for windows
that need the room. The lock screen shows the wallpaper as it is, the date
over a large clock, waiting notifications on frosted glass, and you with a
password field at the bottom; it softly blurs the picture while you type,
and unlocking lifts it all away. Plus notifications with sounds and
screenshots (Print Screen).
Game mode, an FPS counter and keyboard layouts (US, UK, Romanian, German,
French, Spanish) are built in. Wallpapers: the Lake at Dusk photo (the default), Golden
Rain, and five painted landscapes, all drawn to fit any screen.

**Game controllers.** Xbox, PlayStation, Switch Pro, Steam and other USB
controllers work the moment they're plugged in, up to four at once. The
Guide button opens the applications menu, and the D-pad, A and B move
around it. Every app can be used with a controller: the D-pad and left
stick act as the arrow keys, A as Enter and B as Escape. Games can make
controllers vibrate, the top bar shows when one is connected, and a notice
gives its battery level. The Arcade's games have proper controller
layouts, and Settings → Gaming shows a live picture of each controller to
test its buttons, sticks and vibration. F11 makes any window full screen,
which also turns on game mode.

**Linux programs.** zdisplay also speaks Wayland, the window protocol
standard Linux programs use (its own implementation of it, not libwayland's).
Wayland programs, and X11 programs through Xwayland, open as ordinary ZOS
windows: they move, resize, maximize, go full screen, show their menus and
cursors and sit in the dock like ZOS apps, and they type with the keyboard
layout chosen in Settings. Games can lock the pointer and read raw mouse
motion for mouse-look. A full-screen game that keeps a smaller picture is
shown centred at its own aspect ratio. Programs that draw with the GPU share
their buffers with zdisplay (linux-dmabuf), and a full-screen game alone on
screen goes straight to the display from its own buffer, with no copy.

**Steam.** The Steam app starts Valve's Steam client, and through it Proton
for Windows games, full screen in Big Picture or as a desktop window. Steam
is a standard Linux program, so it runs from the Linux layer, a separate
root file system built from upstream sources by `layer/build.sh`: glibc
2.42 and GCC 15.2's runtime (64- and 32-bit), Mesa 26.2 with AMD's
(radeonsi, RADV) and Intel's (iris, crocus, ANV, HASVK) OpenGL and Vulkan
drivers plus zink, virgl and softpipe, libglvnd, the Vulkan loader, X11's
libraries, Xwayland, PulseAudio playing through ZOS's sound service, fonts,
a GNU userland and Valve's launcher. It ships as one compressed image
(`steam.sfs`) on the install stick and ISO; zinit mounts it at
`/data/layers/steam`, from the stick in a live session and from the disk
once installed (the installer copies it). `layerrun` starts programs inside
the layer through a user namespace, with no privileges, along with a
full-screen Xwayland, `zos-xwm` (a window manager for that screen: full
screen for games and Big Picture, focus, centred dialogs), a D-Bus session
bus and the sound server. The Steam app checks that everything is in place
and names the GPU games will use. Valve's client starts from the layer,
updates itself, builds its runtime container inside layerrun's namespace,
and shows Big Picture full screen on ZOS's display, taking clicks and
typing.

**Graphics drivers.** The kernel carries AMD's and Intel's GPU drivers with
their firmware (Radeon RX 400 series and newer, Ryzen graphics, Steam Deck;
Intel 6th generation and newer, Arc), so screens run at their native
resolution and refresh rate and GPUs save power; machines without one of
these use the firmware's framebuffer.

**First start.** The Welcome assistant covers keyboard, time zone, an
account and a look. Every step can be skipped. When started from the USB
stick, it offers to try ZOS or install it.

**Installing.** The Installer either erases a disk or installs into its
free space next to Windows, and adds a "ZOS" entry to the firmware's
boot menu. It takes about a minute.

**Apps**

| App | What it is |
|-----|------------|
| Drift | Web browser with its own HTML and CSS engine: tabs, bookmarks, history, search, forms, find, zoom |
| Harbor | App store: install, update and remove apps from the built-in source or signed web sources |
| Mail | Email for Gmail, Outlook, iCloud, Yahoo or any IMAP account: HTML mail, attachments, search, drafts, undo |
| Dashboard | Your day at a glance: weather, calendar, recent files, screen time |
| Camera | Photos from the webcam, with looks and a timer |
| Help | A searchable guide to ZOS, with what's new in 2.0 |
| Arcade | Glide, a 3D flying game, plus Blocks, Snake, 2048, Mines and Breakout, with best scores; keyboard, mouse or controller |
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

**Glide** is the Arcade's 3D game: fly a paper glider through golden rings
over an endless low-poly land of lakes, hills and forests at sunset. It's
drawn by ZOS's own 3D renderer on every processor core, and lowers its
resolution by itself when a machine can't keep up.

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
make test       # the test suites (runtime, graphics, 3D, toolkit, TLS/crypto, audio, Wi-Fi, disks)
make image      # the kernel with the system inside, build/zos.iso and build/zos.img
make run        # boot build/zos.img in QEMU with UEFI firmware
layer/build.sh  # the Linux layer for Steam (hours), build/layer/steam.sfs
```

A C compiler and `make` are enough for the system itself. Building the
kernel also needs the usual kernel build tools (`bc`, `flex`, `bison`,
`libelf`, `libssl` headers). `kernel/build.sh` downloads Linux on first use,
and `make image` fetches the Wi-Fi and GPU firmware from linux-firmware.
`make image` puts `build/layer/steam.sfs` on the stick and ISO when it has
been built. The layer's build compiles its own GCC first and everything
else with it, as cross builds against the layer; it needs the tools listed
in the `layer` job of `.github/workflows/build.yml` (LLVM 18 and Clang for
Mesa's OpenCL compiler, meson, cmake, autotools, mksquashfs) and about
10 GB of space. Its sources are pinned with their SHA-256 sums in
`layer/sources.txt`.

To try it on a real PC, burn `build/zos.iso` to a DVD, or write it to a
USB stick with balenaEtcher, Rufus or `dd`, and boot from it in UEFI mode.
On ASUS machines, press F8 or Esc for the boot menu. The ISO is a hybrid:
a UEFI El Torito image for discs, and a GPT for USB sticks, sharing one
copy of the kernel. `build/zos.img` is a plain USB stick image. GitHub
Actions builds both on every push (see the `zos-2.0-golden-rain` artifact).
The Installer finds the system on whichever one ZOS started from.

## Layout

| Path | What it is |
|------|------------|
| `libzr/` | The runtime: syscalls, memory, strings, files, time zones, processes, events, JSON, SHA-256 |
| `libzg/` | Graphics: rasterizer, gradients, blur, PNG/JPEG/GIF/BMP, SVG icons, the font, a software 3D renderer |
| `libzui/` | The toolkit: widgets, layout, text editing, dialogs, sound client |
| `libzn/` | Networking: crypto, X.509, TLS 1.3, DNS, HTTP |
| `libza/` | Audio files: WAV, FLAC, Ogg Vorbis, tags and cover art |
| `libzd/` | Disks: GPT, FAT32, ext4, ISO 9660 with El Torito, cpio |
| `system/` | zinit (process 1), zdisplay (display server and desktop), znet, zaudio, zinstall, harborctl, layerrun, the Flint shell, sudo, fetch, play |
| `apps/` | The apps |
| `store/` | Apps offered in Harbor, packaged by `mkimage repo` |
| `kernel/` | Kernel configuration, build script and the list of GPU firmware |
| `layer/` | The Linux layer for Steam: upstream sources (pinned) and how to build them |
| `tools/` | Image builder, QEMU test drivers (with a simulated access point and a virtual controller), the Wayland protocol table generator (`wlgen.py`), development helpers |
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
- **Steam on real hardware.** Tested so far without a GPU: in QEMU, zinit
  mounts the layer from the stick and layerrun, Mesa (software OpenGL) and
  Xwayland run in it as the desktop's user; on a headless ZOS desktop,
  Steam starts and reaches its sign-in screen. Signing in, Proton games and
  GPU acceleration (RADV, radeonsi, ANV, iris, and Xwayland's glamor) need
  a real AMD or Intel GPU and haven't been tried on one yet. Building the
  layer takes a few hours (CI caches it); images built without it work, and
  the Steam app says what's missing. Controllers plugged in while Steam
  runs are seen through the layer's libudev, which ZOS feeds with device
  events.
- **Hardware 3D in ZOS's own apps.** The desktop and Glide are drawn in
  software, so they work on any GPU.
- **NVIDIA graphics.** NVIDIA's driver is a kernel module, and the ZOS
  kernel has no module loader; on laptops with NVIDIA and AMD/Intel
  graphics, the AMD or Intel GPU drives the screen.
- **Windows programs outside Steam.** `.exe` files don't run on their own.

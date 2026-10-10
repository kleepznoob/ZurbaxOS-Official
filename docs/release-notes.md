Zurbax 2.0: a desktop operating system written from
scratch in C, on the Linux kernel, made for playing.

![The Zurbax desktop](https://raw.githubusercontent.com/kleepznoob/I-REALLY-WANNA-IMPRESS-ME-PARENTS-BY-MAKING-MONEY/claude/zurbax-os/docs/screenshots/desktop.jpg)

## Which file to download

| File | Use it for |
|------|------------|
| `zurbax.iso` | A DVD, or a USB stick (it's a hybrid image, so the same file works for both) |
| `zurbax.img` | A USB stick, written as a plain disk image |
| `SHA256SUMS` | Checking that the download arrived intact (`sha256sum -c SHA256SUMS`) |

## Starting it

1. Write `zurbax.iso` to a USB stick with balenaEtcher, Rufus
   (in DD mode) or `dd`, or burn it to a DVD.
2. Restart the PC and open its boot menu (F8 or Esc on ASUS machines, F12
   on many others), then pick the stick. Zurbax starts in UEFI mode; turn off
   Secure Boot if the firmware refuses it.
3. The Welcome assistant offers to try Zurbax from the stick or install it.
   The Installer can erase a disk or install into its free space next to
   Windows, and adds a "Zurbax" entry to the firmware's boot menu.

## What's in it

- The whole desktop: top bar, the Arc dock, applications menu, lock screen,
  notifications, quick settings and wallpapers.
- Apps: Drift (web browser), Harbor (app store), Mail, Dashboard, Music,
  Photos, Camera, Vault (files), Forge (terminal), Writer, Notes, Calendar,
  Clock, Calculator, Weather, Screen Time, Pulse, Screenshot, Settings and
  Help.
- Games: the Arcade, with Glide (a 3D flying game) plus Blocks, Snake,
  2048, Mines and Breakout. Xbox, PlayStation, Switch Pro and Steam
  controllers work as soon as they're plugged in.
- Steam: the Linux layer (glibc, Mesa with AMD and Intel's OpenGL and
  Vulkan drivers, Xwayland, PulseAudio) is on the image, and the Steam app
  starts Valve's client full screen in Big Picture.
- Wi-Fi (WPA2-Personal and open networks) and wired networking, with
  firmware for common Intel, MediaTek and Realtek chips and for AMD and
  Intel graphics.

## Known limits

- NVIDIA graphics aren't supported; on laptops with two GPUs, the AMD or
  Intel one drives the screen.
- Steam has been tested up to its sign-in screen without a GPU. Signing
  in, Proton games and GPU acceleration haven't been tried on real AMD or
  Intel hardware yet.
- Wi-Fi networks that require WPA3 or enterprise logins, JavaScript in
  Drift, and MP3 or AAC playback aren't there yet.

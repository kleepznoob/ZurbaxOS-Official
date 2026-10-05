# Zurbax

An operating system built from scratch — kernel aside — in C.

Zurbax is a desktop Linux system with its own runtime, graphics engine,
display server, toolkit, shell and applications. Nothing from existing
desktop stacks is used: no glibc, no Wayland or X11, no GTK or Qt, no
GNOME or KDE. The only outside component is the Linux kernel itself.

The look is calm and modern — a clean top bar, a dock, rounded panels and
unobtrusive animation — taking cues from the tidy, native feel of
elementary OS rather than copying any existing system.

## Layout

| Path | What it is |
|------|------------|
| `kernel/` | Config and build script for the Linux kernel (drivers built in, direct EFI boot) |
| `libzr/`  | The runtime: syscalls, memory, strings, files, time, processes, events |
| `libzg/`  | The graphics engine: rasterizer, images, text (planned next) |
| `libzui/` | The application toolkit (planned) |
| `system/` | Display server, shell, init, services (planned) |
| `apps/`   | Settings, files, browser, terminal and the rest (planned) |
| `tests/`  | Test suites (`make test`) |

## Building

```sh
make test          # build the libraries and run the test suite
kernel/build.sh     # build the Zurbax kernel
```

A C compiler is all that is needed; the code is freestanding and links no
system libraries.

## Status

Early. The runtime (`libzr`) is complete and tested, and the kernel builds
into a bootable image. Graphics, the shell and the apps come next.

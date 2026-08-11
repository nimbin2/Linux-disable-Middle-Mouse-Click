# Linux-disable-Middle-Mouse-Click

Disable **TrackPoint middle-click** while keeping **press-and-scroll** working.

ThinkPad/Lenovo TrackPoint devices use the middle button + TrackPoint to implement
click-and-scroll. If you also use middle-click to open links/tabs, this can be
annoying because accidental middle-clicks happen while scrolling.

This tiny C program intercepts raw evdev events, removes the *plain* middle-click,
but still emits the correct events when you actually use middle-button scrolling.

> **100% vibecode** – the code is intentionally small, direct, and easy to tweak.

---

## Features / Options

| Option | What it does |
|--------|--------------|
| `-d /dev/input/eventX` (repeatable) | Filter specific device node(s). |
| `--auto` | Auto-detect TrackPoint devices via udev (requires `ID_INPUT_POINTINGSTICK=1` **and** the device must actually expose `BTN_MIDDLE`). |
| `--match REGEX` | Optional extra filter for `--auto`: case-insensitive POSIX regex matched against udev `ID_MODEL` (useful if auto finds more than you want). |
| `-h` / `--help` | Show help. |

If you pass both `-d` and `--auto`, the program filters the **union** of both sets.

---

## Getting the right device names

The kernel creates input nodes under `/dev/input/` (`event0`, `event1`, …).
You need the exact node that belongs to the TrackPoint you want to filter.

### 1) Using `libinput`

```bash
sudo libinput debug-events | grep -i trackpoint

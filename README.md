# middle‑disable – Disable TrackPoint middle‑click, keep scrolling

ThinkPad/Lenovo TrackPoint keyboards use the middle button *and* the
TrackPoint itself to implement click‑and‑scroll.  When you also use the
middle button for opening browser tabs, the scrolling‑behaviour becomes
annoying.  This tiny C program intercepts the raw input events, drops the
plain middle‑click while still emitting the proper *press‑and‑scroll*
events.

> **100 % vibecode** – the source is small, self‑contained and completely
> free to modify.

---

## Features

| Option | What it does |
|--------|--------------|
| `-d /dev/input/eventX` (repeatable) | Filter a specific device. |
| `--auto` | Scan the system and automatically filter *all* TrackPoint devices (`ID_INPUT_POINTINGSTICK=1`). |
| `-h / --help` | Show a usage summary. |

If you give *both* `-d` and `--auto`, the program will filter the union of
the explicit devices and the auto‑detected ones.

---

## Getting the right device names

The kernel creates a node for every input device under **/dev/input/**,
named `event0`, `event1`, …  You need the exact node that belongs to the
TrackPoint you want to filter.

### 1. Using `udevadm`

```bash
# replace event13 with the number you suspect
udevadm info -a -n /dev/input/event13 | grep -i "trackpoint"
```

If the output contains a line like `ATTRS{idInputPointingStick}=="1"` you
have the right one.

### 2. Using `libinput`

```bash
$ libinput debug-events
-event13  DEVICE_ADDED  TPPS/2 Elan TrackPoint   seat0 default group10 …
-event15  DEVICE_ADDED  Lenovo TrackPoint Keyboard II Mouse …
```

The part after `-event` is the *eventX* you need (e.g. `event13`).

### 3. By‑id /path shortcuts

The `/dev/input/by-id/` and `/dev/input/by-path/` symlinks are stable across boots,
e.g.:

```bash
ls -l /dev/input/by-id/*TrackPoint*mouse
lrwxrwxrwx 1 root root 13 Sep  1 12:34 usb-Lenovo_TrackPoint_Keyboard_II-if01-event-mouse -> ../event15
```

You can feed those symlinks straight to the program (`-d /dev/input/by-id/...`).

---

## Building

You need the **libudev** development headers (package `libudev-dev` on Debian/Ubuntu,
`systemd-devel` on Fedora, etc.).

```bash
gcc -Wall -O2 -D_GNU_SOURCE -o middle-disable middle-disable.c -ludev
```

The binary must be run **as root** (or with the `CAP_SYS_ADMIN` capability)
because it needs to:

* open `/dev/uinput` and create a virtual device,
* grab the real `/dev/input/eventX` devices so the kernel stops sending them to X/Wayland.

---

## Running

### 1. Manual start (for testing)

```bash
sudo ./middle-disable --auto
# or
sudo ./middle-disable -d /dev/input/event13 -d /dev/input/event15
```

Press **Ctrl‑C** to stop – the program cleans up the uinput nodes and releases the grabs.

### 2. Systemd service (run at boot)

Create `/etc/systemd/system/middle-disable.service`:

```ini
[Unit]
Description=Disable TrackPoint middle click (keep scrolling)
After=systemd-udevd.service
Wants=systemd-udevd.service

[Service]
Type=simple
ExecStart=/usr/local/sbin/middle-disable --auto
# change the path if you installed it elsewhere
Restart=on-failure
CapabilityBoundingSet=CAP_SYS_ADMIN
AmbientCapabilities=CAP_SYS_ADMIN
User=root

[Install]
WantedBy=multi-user.target
```

Enable and start it:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now middle-disable.service
```

Check the log:

```bash
journalctl -u middle-disable -f
```

---

## How it works (short explanation)

* The program **grabs** the physical input device (`EVIOCGRAB`).  
  This tells the kernel “don’t deliver the events downstream”.
* It creates a **virtual** device with `uinput` that mimics the original
  capabilities (buttons, relative axes).
* For the built‑in TrackPoint (`internal`), the logic is:
  * When BTN_MIDDLE goes down, **don’t** forward it immediately.
  * If a wheel (`REL_WHEEL`, `REL_HWHEEL`, …) shows up while the button is held,
    emit a *synthetic* BTN_MIDDLE press – this is what the desktop thinks
    of as “click‑and‑scroll”.
  * When the button is released, forward the BTN_MIDDLE release **only** if we
    previously sent the synthetic press.
* For external keyboards (`external`), any BTN_MIDDLE events are simply **dropped**.
* All other events (movement, left/right click, etc.) are forwarded unchanged.

That is exactly the behaviour you had in the original code, now with a clean
command‑line interface and automatic detection.

---

## License

Public domain / “free as vibecode”.  Do whatever you want with it.

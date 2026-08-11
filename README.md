# Linux-disable-Middle-Mouse-Click

Disable TrackPoint middle-click while keeping press-and-scroll working.
ThinkPad/Lenovo TrackPoint devices use the middle button + TrackPoint to implement click-and-scroll. If you also use middle-click to open links/tabs, it’s easy to accidentally middle-click while scrolling. This small C program intercepts raw evdev events, removes the plain middle-click, but still emits the correct events when you actually scroll with the middle button held.  

100% vibecode: this repo/program was generated/iterated with ChatGPT. It’s intentionally small and hackable.

---

## Features

| Option | What it does |
|--------|--------------|
| `-d /dev/input/eventX` (repeatable) | Filter a specific device. |
| `--auto` | Auto-detect TrackPoint devices via udev (matches `ID_INPUT_POINTINGSTICK=1` and also requires the device to expose `BTN_MIDDLE`). |
| `--match REGEX` | Optional extra filter for `--auto`: case-insensitive POSIX regex matched against udev `ID_MODEL` (useful if auto finds too many devices). |
| `-h / --help` | Show a usage summary. |

If you give *both* `-d` and `--auto`, the program will filter the union of the explicit devices and the auto‑detected ones.

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

### 3. SysV init (sysvinit / sysv-rc / Devuan, etc.)

Create `/etc/init.d/middle-disable`:

```sh
#!/bin/sh
### BEGIN INIT INFO
# Provides:          middle-disable
# Required-Start:    $local_fs $remote_fs udev
# Required-Stop:     $local_fs $remote_fs
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: Disable TrackPoint middle click (keep scrolling)
### END INIT INFO

DAEMON=/usr/local/sbin/middle-disable
NAME=middle-disable
PIDFILE=/run/$NAME.pid

# Choose ONE:
ARGS="--auto"
# ARGS="-d /dev/input/event13 -d /dev/input/event15"

case "$1" in
  start)
    echo "Starting $NAME..."
    start-stop-daemon --start --quiet --background \
      --make-pidfile --pidfile "$PIDFILE" \
      --exec "$DAEMON" -- $ARGS
    ;;
  stop)
    echo "Stopping $NAME..."
    start-stop-daemon --stop --quiet --pidfile "$PIDFILE" --retry 5
    rm -f "$PIDFILE"
    ;;
  restart)
    "$0" stop
    sleep 1
    "$0" start
    ;;
  status)
    if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
      echo "$NAME is running (pid $(cat "$PIDFILE"))."
      exit 0
    else
      echo "$NAME is not running."
      exit 3
    fi
    ;;
  *)
    echo "Usage: $0 {start|stop|restart|status}"
    exit 2
    ;;
esac

exit 0
```

Enable it (Debian/Devuan style):

```bash
sudo chmod +x /etc/init.d/middle-disable
sudo update-rc.d middle-disable defaults
sudo service middle-disable start
```

On some distros you may use chkconfig instead of update-rc.d.

### 4. OpenRC

Create `/etc/init.d/middle-disable`:  
```bash
#!/sbin/openrc-run

name="middle-disable"
description="Disable TrackPoint middle click (keep scrolling)"

command="/usr/local/sbin/middle-disable"

# Choose ONE:
command_args="--auto"
# command_args="-d /dev/input/event13 -d /dev/input/event15"

command_background="yes"
pidfile="/run/${RC_SVCNAME}.pid"

depend() {
  need localmount
  after udev
}
```

Enable and start:

```bash
sudo chmod +x /etc/init.d/middle-disable
sudo rc-update add middle-disable default
sudo rc-service middle-disable start
```

---

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

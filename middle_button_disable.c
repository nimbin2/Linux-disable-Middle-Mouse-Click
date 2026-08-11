/*=====================================================================
 *  middle‑disable.c – disable TrackPoint middle‑click, keep scrolling
 *
 *  (c) 2024 100 % vibecode – feel free to copy, hack and share
 *
 *  Build (needs libudev, POSIX regex is in libc):
 *      gcc -Wall -O2 -D_GNU_SOURCE -o middle-disable middle-disable.c -ludev
 *
 *  New command‑line interface
 *      -d /dev/input/eventX          (repeatable)
 *      --auto                       auto‑detect all TrackPoints
 *      --match <regex>              optional additional filter on the device
 *                                   model name (case‑insensitive POSIX regex)
 *      -h / --help                  Show help.
 *
 *  The README (at the end of this file) explains how to discover the
 *  right eventX names, how to install a systemd service, etc.
 *====================================================================*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE               /* must be defined before any header */
#endif

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <getopt.h>
#include <libudev.h>
#include <regex.h>

/* -----------------------------------------------------------------
 *  Constants & helper macros
 * ----------------------------------------------------------------- */
#define MAX_DEVICES 32            /* plenty for any laptop */

static inline int test_bit(unsigned int nr, const unsigned long *addr)
{
    return (addr[nr / (8 * sizeof(unsigned long))] >>
            (nr % (8 * sizeof(unsigned long)))) & 1;
}

/* -----------------------------------------------------------------
 *  Device description (one entry per filtered device)
 * ----------------------------------------------------------------- */
struct device {
    const char *path;          /* /dev/input/eventX */
    const char *name;          /* name shown in /dev/input */
    int  fd;                   /* fd of the real grabbed device */
    int  ufd;                  /* fd of the uinput virtual device */
    int  middle_down;          /* BTN_MIDDLE is currently pressed */
    int  middle_sent;          /* we already emitted a synthetic press */
};

/* -----------------------------------------------------------------
 *  Global stop flag (SIGINT / SIGTERM)
 * ----------------------------------------------------------------- */
static volatile sig_atomic_t running = 1;
static void stop_handler(int sig) { (void)sig; running = 0; }

/* -----------------------------------------------------------------
 *  uinput helpers (identical to your original implementation)
 * ----------------------------------------------------------------- */
static int emit_event(struct device *dev, const struct input_event *ev)
{
    ssize_t n = write(dev->ufd, ev, sizeof(*ev));
    return (n == sizeof(*ev)) ? 0 : -1;
}
static int emit_key(struct device *dev,
                    const struct input_event *src,
                    int code, int value)
{
    struct input_event ev = *src;
    ev.type  = EV_KEY;
    ev.code  = code;
    ev.value = value;
    return emit_event(dev, &ev);
}

/* -----------------------------------------------------------------
 *  Create the uinput virtual device (mirrors the capabilities we need)
 * ----------------------------------------------------------------- */
static int create_uinput(struct device *dev)
{
    dev->ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (dev->ufd < 0) { perror("/dev/uinput"); return -1; }

    if (ioctl(dev->ufd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(dev->ufd, UI_SET_EVBIT, EV_REL) < 0 ||
        ioctl(dev->ufd, UI_SET_EVBIT, EV_SYN) < 0)
        return -1;

    if (ioctl(dev->ufd, UI_SET_KEYBIT, BTN_LEFT)   < 0 ||
        ioctl(dev->ufd, UI_SET_KEYBIT, BTN_RIGHT)  < 0 ||
        ioctl(dev->ufd, UI_SET_KEYBIT, BTN_MIDDLE) < 0)
        return -1;

    if (ioctl(dev->ufd, UI_SET_RELBIT, REL_X)                < 0 ||
        ioctl(dev->ufd, UI_SET_RELBIT, REL_Y)                < 0 ||
        ioctl(dev->ufd, UI_SET_RELBIT, REL_WHEEL)            < 0 ||
        ioctl(dev->ufd, UI_SET_RELBIT, REL_HWHEEL)           < 0 ||
        ioctl(dev->ufd, UI_SET_RELBIT, REL_WHEEL_HI_RES)     < 0 ||
        ioctl(dev->ufd, UI_SET_RELBIT, REL_HWHEEL_HI_RES)    < 0)
        return -1;

    struct uinput_setup setup = {
        .id = {
            .bustype = BUS_USB,
            .vendor  = 0x17ef,
            .product = 0x60ee,
            .version = 1
        }
    };
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "%s", dev->name);

    if (ioctl(dev->ufd, UI_DEV_SETUP, &setup) < 0) return -1;
    if (ioctl(dev->ufd, UI_DEV_CREATE, 0)    < 0) return -1;

    usleep(100000);   /* give the kernel a moment to create the node */
    return 0;
}

/* -----------------------------------------------------------------
 *  Event processing – internal (built‑in) vs external keyboards
 * ----------------------------------------------------------------- */
static void process_internal(struct device *dev,
                             const struct input_event *ev)
{
    if (ev->type == EV_KEY && ev->code == BTN_MIDDLE) {
        if (ev->value == 1) { dev->middle_down = 1; return; }
        if (ev->value == 0) {
            if (dev->middle_sent) emit_key(dev, ev, BTN_MIDDLE, 0);
            dev->middle_down = 0;
            dev->middle_sent = 0;
            return;
        }
        return;                     /* ignore repeats */
    }

    if (dev->middle_down && !dev->middle_sent &&
        ev->type == EV_REL &&
        (ev->code == REL_WHEEL ||
         ev->code == REL_HWHEEL ||
         ev->code == REL_WHEEL_HI_RES ||
         ev->code == REL_HWHEEL_HI_RES))
    {
        emit_key(dev, ev, BTN_MIDDLE, 1);
        dev->middle_sent = 1;
    }

    emit_event(dev, ev);
}
static void process_external(struct device *dev,
                             const struct input_event *ev)
{
    if (ev->type == EV_KEY && ev->code == BTN_MIDDLE)
        return;                     /* drop middle‑click completely */
    emit_event(dev, ev);
}

/* -----------------------------------------------------------------
 *  Clean‑up helper – destroy uinput nodes and release the grabs
 * ----------------------------------------------------------------- */
static void cleanup(struct device *devices, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (devices[i].ufd >= 0) {
            ioctl(devices[i].ufd, UI_DEV_DESTROY);
            close(devices[i].ufd);
        }
        if (devices[i].fd >= 0) {
            ioctl(devices[i].fd, EVIOCGRAB, 0);
            close(devices[i].fd);
        }
    }
}

/* -----------------------------------------------------------------
 *  Does the given /dev/input/eventX actually expose BTN_MIDDLE ?
 *  Returns 1 if the key is present, 0 otherwise.
 * ----------------------------------------------------------------- */
static int has_middle_button(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    unsigned long bits[(KEY_MAX / (8 * sizeof(unsigned long))) + 1];
    memset(bits, 0, sizeof(bits));

    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) {
        close(fd);
        return 0;
    }
    close(fd);
    return test_bit(BTN_MIDDLE, bits);
}

/* -----------------------------------------------------------------
 *  libudev auto‑detect helper
 *
 *  Returns a NULL‑terminated malloc‑ed array of *device paths* that:
 *      • have ID_INPUT_POINTINGSTICK=1
 *      • really expose BTN_MIDDLE (checked above)
 *      • optionally match the user‑supplied regex against ID_MODEL
 *
 *  The caller must free each string and the array itself.
 * ----------------------------------------------------------------- */
static char **discover_trackpoints(const char *name_regex)
{
    struct udev *udev = udev_new();
    if (!udev) {
        fprintf(stderr, "udev_new() failed\n");
        return NULL;
    }

    struct udev_enumerate *e = udev_enumerate_new(udev);
    if (!e) { udev_unref(udev); return NULL; }

    udev_enumerate_add_match_subsystem(e, "input");
    udev_enumerate_add_match_property(e, "ID_INPUT_POINTINGSTICK", "1");
    udev_enumerate_scan_devices(e);

    regex_t regex;
    int have_regex = (name_regex != NULL);
    if (have_regex) {
        int rc = regcomp(&regex, name_regex, REG_NOSUB | REG_ICASE);
        if (rc != 0) {
            char errbuf[128];
            regerror(rc, &regex, errbuf, sizeof(errbuf));
            fprintf(stderr, "Invalid regex \"%s\": %s\n", name_regex, errbuf);
            udev_enumerate_unref(e);
            udev_unref(udev);
            return NULL;
        }
    }

    char **list = calloc(MAX_DEVICES + 1, sizeof(char *));
    if (!list) { perror("calloc"); if (have_regex) regfree(&regex);
                  udev_enumerate_unref(e); udev_unref(udev); return NULL; }

    size_t cnt = 0;
    struct udev_list_entry *entry;
    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(e)) {
        const char *syspath = udev_list_entry_get_name(entry);
        struct udev_device *dev = udev_device_new_from_syspath(udev, syspath);
        const char *devnode = udev_device_get_devnode(dev);
        const char *model   = udev_device_get_property_value(dev, "ID_MODEL");

        if (!devnode) { udev_device_unref(dev); continue; }

        /* 1️⃣ the device must really have BTN_MIDDLE */
        if (!has_middle_button(devnode)) {
            udev_device_unref(dev);
            continue;
        }

        /* 2️⃣ optional name regex */
        if (have_regex && model && regexec(&regex, model, 0, NULL, 0) != 0) {
            udev_device_unref(dev);
            continue;
        }

        if (cnt < MAX_DEVICES)
            list[cnt++] = strdup(devnode);
        udev_device_unref(dev);
    }

    list[cnt] = NULL;                     /* terminator */

    if (have_regex) regfree(&regex);
    udev_enumerate_unref(e);
    udev_unref(udev);
    return list;
}

/* -----------------------------------------------------------------
 *  Usage / help text
 * ----------------------------------------------------------------- */
static void usage(const char *progname)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  -d, --device /dev/input/eventX   Add a specific device (repeatable).\n"
        "  --auto                           Auto‑detect all TrackPoint devices.\n"
        "  --match REGEX                    Optional extra filter on the device's\n"
        "                                   ID_MODEL string (case‑insensitive POSIX regex).\n"
        "  -h, --help                       Show this help and exit.\n"
        "\n"
        "How to find the correct eventX:\n"
        "  $ udevadm info -a -n /dev/input/event13 | grep -i \"trackpoint\"\n"
        "  $ libinput debug-events | grep -i \"trackpoint\"\n"
        "\n"
        "Compile (needs libudev, POSIX regex is in libc):\n"
        "  gcc -Wall -O2 -D_GNU_SOURCE -o middle-disable middle-disable.c -ludev\n"
        "\n"
        "Run as root (or with CAP_SYS_ADMIN).\n"
        "\n"
        "The source is 100 %% vibecode – feel free to tweak it!\n",
        progname);
}

/* -----------------------------------------------------------------
 *  main()
 * ----------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    /* ----- command line parsing ----- */
    static const struct option long_opts[] = {
        {"device", required_argument, NULL, 'd'},
        {"auto",   no_argument,       NULL,  1 },
        {"match",  required_argument, NULL,  2 },
        {"help",   no_argument,       NULL, 'h'},
        {0,0,0,0}
    };

    const char *explicit_paths[MAX_DEVICES];
    size_t      explicit_cnt = 0;
    int         do_auto = 0;
    const char *match_regex = NULL;

    for (int c; (c = getopt_long(argc, argv, "d:h", long_opts, NULL)) != -1; ) {
        switch (c) {
        case 'd':
            if (explicit_cnt >= MAX_DEVICES) {
                fprintf(stderr, "Too many -d arguments (max %d)\n", MAX_DEVICES);
                return EXIT_FAILURE;
            }
            explicit_paths[explicit_cnt++] = optarg;
            break;
        case 1:   /* --auto */
            do_auto = 1;
            break;
        case 2:   /* --match */
            match_regex = optarg;
            break;
        case 'h':
        default:
            usage(argv[0]);
            return (c == 'h') ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    /* ----- build the final list of device paths ----- */
    char **auto_list = NULL;
    const char *paths[MAX_DEVICES];
    size_t total_devices = 0;

    if (do_auto) {
        auto_list = discover_trackpoints(match_regex);
        if (!auto_list) {
            fprintf(stderr, "Auto‑detect failed.\n");
            return EXIT_FAILURE;
        }
        for (size_t i = 0; auto_list[i] && total_devices < MAX_DEVICES; ++i)
            paths[total_devices++] = auto_list[i];
    }

    for (size_t i = 0; i < explicit_cnt && total_devices < MAX_DEVICES; ++i)
        paths[total_devices++] = explicit_paths[i];

    if (total_devices == 0) {
        fprintf(stderr, "No devices selected – use -d or --auto.\n");
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    /* ---------- install signal handlers ---------- */
    signal(SIGINT,  stop_handler);
    signal(SIGTERM, stop_handler);

    /* ---------- open, grab, and create uinput for each device ---------- */
    struct device devices[MAX_DEVICES];

    for (size_t i = 0; i < total_devices; ++i) {
        devices[i].path = paths[i];
        devices[i].name = "TrackPoint (filtered)";
        devices[i].fd   = -1;
        devices[i].ufd  = -1;
        devices[i].middle_down = 0;
        devices[i].middle_sent = 0;

        devices[i].fd = open(devices[i].path, O_RDONLY | O_NONBLOCK);
        if (devices[i].fd < 0) {
            perror(devices[i].path);
            cleanup(devices, i);
            goto out_free;
        }

        if (ioctl(devices[i].fd, EVIOCGRAB, 1) < 0) {
            perror("EVIOCGRAB");
            cleanup(devices, i + 1);
            goto out_free;
        }

        if (create_uinput(&devices[i]) < 0) {
            fprintf(stderr, "Failed to create uinput for %s\n", devices[i].path);
            cleanup(devices, i + 1);
            goto out_free;
        }

        fprintf(stderr, "Filtering %s → uinput created\n", devices[i].path);
    }

    /* ---------- main poll loop ---------- */
    struct pollfd fds[MAX_DEVICES];
    while (running) {
        for (size_t i = 0; i < total_devices; ++i) {
            fds[i].fd      = devices[i].fd;
            fds[i].events  = POLLIN;
            fds[i].revents = 0;
        }

        int ret = poll(fds, total_devices, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        for (size_t i = 0; i < total_devices; ++i) {
            if (!(fds[i].revents & POLLIN)) continue;

            struct input_event evbuf[32];
            ssize_t nbytes = read(devices[i].fd, evbuf, sizeof(evbuf));
            if (nbytes < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                perror("read");
                running = 0;
                break;
            }

            size_t evcnt = (size_t)nbytes / sizeof(struct input_event);
            for (size_t j = 0; j < evcnt; ++j) {
                if (i == 0)
                    process_internal(&devices[i], &evbuf[j]);
                else
                    process_external(&devices[i], &evbuf[j]);
            }
        }
    }

    cleanup(devices, total_devices);

out_free:
    if (auto_list) {
        for (size_t i = 0; auto_list[i]; ++i)
            free((void *)auto_list[i]);
        free(auto_list);
    }
    return EXIT_SUCCESS;
}

/*=====================================================================
 *  README (excerpt) – how to use the program
 *=====================================================================

# middle‑disable – Disable TrackPoint middle‑click, keep scrolling

ThinkPad/Lenovo TrackPoint keyboards use the middle button together with the
pointing stick to implement click‑and‑scroll.  When you also use the middle
button to open hyperlinks this becomes painful.  This program removes the plain
middle‑click while preserving the scrolling behaviour.

## New command‑line interface

| Option | Meaning |
|--------|---------|
| `-d /dev/input/eventX` (repeatable) | Explicitly add a single device. |
| `--auto` | Auto‑detect all TrackPoint devices (the ones that expose `BTN_MIDDLE`). |
| `--match "<regex>"` | Optional extra filter on the udev `ID_MODEL` property. Example: `--match "Lenovo"` keeps only Lenovo keyboards. |
| `-h` / `--help` | Show this help. |

### Why `--auto` used to list *too many* devices

The original auto‑detect only looked at `ID_INPUT_POINTINGSTICK=1`.  Some USB
hubs or touch‑screens also set that flag, so they were mistakenly added.

The **new version** adds two safeguards:

1. **Capability check** – the device must actually have the `BTN_MIDDLE` key
   (checked via `EVIOCGBIT`).  
2. **Optional name regex** – you can further narrow the list with `--match`.

If you run `--auto` without a regex you’ll now see only the genuine
TrackPoints (internal + external keyboards).  If you still get extras,
add a regex that matches the model you want.

## Finding your device name (`eventX`)

```bash
# Show the udev tree for a known node
udevadm info -a -n /dev/input/event13 | grep -i "trackpoint"

# Or use libinput’s live debug output
libinput debug-events | grep -i "trackpoint"
*/

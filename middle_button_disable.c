/*=====================================================================
 *  middle-disable.c – disable TrackPoint middle-click, keep scrolling
 *
 *  (c) 2024 100 % vibecode – feel free to copy, hack and share
 *
 *  Build (needs libudev, POSIX regex is in libc):
 *      gcc -Wall -O2 -D_GNU_SOURCE -o middle_button_disable middle-disable.c -ludev
 *
 *  CLI:
 *      -d /dev/input/eventX          (repeatable)
 *      --auto                       auto-detect TrackPoints (+ hotplug)
 *      --match <regex>              optional filter on ID_MODEL (case-insensitive POSIX regex)
 *      --vendor <hex>               optional filter on ID_VENDOR_ID (only if property exists)
 *      --product <hex>              optional filter on ID_MODEL_ID (only if property exists)
 *      -h / --help                  show help
 *
 *  Important fix:
 *    We MUST ignore the uinput devices we create ourselves, otherwise udev
 *    hotplug will see them as new pointingsticks and we recurse forever
 *    (event numbers skyrocketing like event256, event257, ...).
 *    We exclude /sys/devices/virtual/\* (uinput) devices and also give our
 *    uinput a distinctive name prefix.
 *====================================================================*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <regex.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <libudev.h>

/* -----------------------------------------------------------------
 *  Constants & helper macros
 * ----------------------------------------------------------------- */
#define MAX_DEVICES 32
#define UINPUT_NAME_PREFIX "middle-disable: "

static inline int test_bit(unsigned int nr, const unsigned long *addr)
{
    return (addr[nr / (8 * sizeof(unsigned long))] >>
            (nr % (8 * sizeof(unsigned long)))) & 1;
}

/* -----------------------------------------------------------------
 *  Device description
 * ----------------------------------------------------------------- */
struct device {
    char *path;                 /* strdup()ed, e.g. /dev/input/eventX */
    char *name;                 /* strdup()ed source device name (for logs) */
    int  fd;                    /* fd of the real grabbed device */
    int  ufd;                   /* fd of the uinput virtual device */
    int  middle_down;           /* BTN_MIDDLE currently pressed */
    int  middle_sent;           /* we already emitted a synthetic press */
};

/* -----------------------------------------------------------------
 *  Filters (applied to --auto and hotplug)
 * ----------------------------------------------------------------- */
struct filters {
    const char *vendor_id;      /* hex, e.g. "17ef" (only if udev property exists) */
    const char *product_id;     /* hex, e.g. "60ee" (only if udev property exists) */
    int have_regex;
    regex_t regex;
};

/* -----------------------------------------------------------------
 *  Global stop flag (SIGINT / SIGTERM)
 * ----------------------------------------------------------------- */
static volatile sig_atomic_t running = 1;
static void stop_handler(int sig) { (void)sig; running = 0; }

/* -----------------------------------------------------------------
 *  uinput emit helpers
 * ----------------------------------------------------------------- */
static int emit_event(struct device *dev, const struct input_event *ev)
{
    ssize_t n = write(dev->ufd, ev, sizeof(*ev));
    return (n == sizeof(*ev)) ? 0 : -1;
}

static int emit_key(struct device *dev, const struct input_event *src,
                    int code, int value)
{
    struct input_event ev = *src;
    ev.type  = EV_KEY;
    ev.code  = code;
    ev.value = value;
    return emit_event(dev, &ev);
}

/* -----------------------------------------------------------------
 *  Create uinput device
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

    if (ioctl(dev->ufd, UI_SET_RELBIT, REL_X)             < 0 ||
        ioctl(dev->ufd, UI_SET_RELBIT, REL_Y)             < 0 ||
        ioctl(dev->ufd, UI_SET_RELBIT, REL_WHEEL)         < 0 ||
        ioctl(dev->ufd, UI_SET_RELBIT, REL_HWHEEL)        < 0 ||
        ioctl(dev->ufd, UI_SET_RELBIT, REL_WHEEL_HI_RES)  < 0 ||
        ioctl(dev->ufd, UI_SET_RELBIT, REL_HWHEEL_HI_RES) < 0)
        return -1;

    /* Mark as virtual so we can reliably exclude it from --auto/hotplug */
    struct uinput_setup setup = {
        .id = {
            .bustype = BUS_VIRTUAL,
            .vendor  = 0xfeed,
            .product = 0x0001,
            .version = 1
        }
    };

    const char *srcname = dev->name ? dev->name : "TrackPoint";
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "%s%s", UINPUT_NAME_PREFIX, srcname);

    if (ioctl(dev->ufd, UI_DEV_SETUP, &setup) < 0) return -1;
    if (ioctl(dev->ufd, UI_DEV_CREATE, 0)    < 0) return -1;

    usleep(100000);
    return 0;
}

/* -----------------------------------------------------------------
 *  Event processing: Disable plain middle-click, keep scroll behavior
 * ----------------------------------------------------------------- */
static void process_trackpoint(struct device *dev, const struct input_event *ev)
{
    if (ev->type == EV_KEY && ev->code == BTN_MIDDLE) {
        if (ev->value == 1) { dev->middle_down = 1; return; }
        if (ev->value == 0) {
            if (dev->middle_sent) emit_key(dev, ev, BTN_MIDDLE, 0);
            dev->middle_down = 0;
            dev->middle_sent = 0;
            return;
        }
        return; /* ignore repeats */
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

/* -----------------------------------------------------------------
 *  Capability check: does /dev/input/eventX expose BTN_MIDDLE?
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
 *  Read device name from the real device (best-effort)
 * ----------------------------------------------------------------- */
static char *read_device_name(int fd)
{
    char buf[256];
    memset(buf, 0, sizeof(buf));
    if (ioctl(fd, EVIOCGNAME(sizeof(buf)), buf) < 0 || buf[0] == '\0')
        return strdup("TrackPoint");
    return strdup(buf);
}

/* -----------------------------------------------------------------
 *  Detect whether a udev "input" device is virtual (uinput)
 *  We use syspath: uinput lives under /sys/devices/virtual/...
 * ----------------------------------------------------------------- */
static int is_virtual_input_udev_device(struct udev_device *d)
{
    const char *syspath = udev_device_get_syspath(d);
    if (!syspath) return 0;
    return (strstr(syspath, "/devices/virtual/") != NULL);
}

/* -----------------------------------------------------------------
 *  "only if it has it" hex option matching
 * ----------------------------------------------------------------- */
static int hex_match_opt(const char *opt, const char *udev_val)
{
    if (!opt || !*opt) return 1;
    if (!udev_val || !*udev_val) return 1; /* only filter if property exists */

    while (!strncasecmp(opt, "0x", 2)) opt += 2;
    return strcasecmp(opt, udev_val) == 0;
}

/* -----------------------------------------------------------------
 *  Does a udev input device match our TrackPoint filters?
 *  (and not be one of our own uinput devices)
 * ----------------------------------------------------------------- */
static int device_matches_filters(struct udev_device *d, const struct filters *f)
{
    /* Prevent infinite recursion: never match uinput/virtual devices */
    if (is_virtual_input_udev_device(d))
        return 0;

    const char *tpflag = udev_device_get_property_value(d, "ID_INPUT_POINTINGSTICK");
    if (!tpflag || strcmp(tpflag, "1") != 0) return 0;

    const char *model = udev_device_get_property_value(d, "ID_MODEL");
    const char *vid   = udev_device_get_property_value(d, "ID_VENDOR_ID");
    const char *pid   = udev_device_get_property_value(d, "ID_MODEL_ID");

    if (!hex_match_opt(f->vendor_id, vid)) return 0;
    if (!hex_match_opt(f->product_id, pid)) return 0;

    if (f->have_regex) {
        if (!model) return 0;
        if (regexec((regex_t *)&f->regex, model, 0, NULL, 0) != 0)
            return 0;
    }

    return 1;
}

/* -----------------------------------------------------------------
 *  Find device by path
 * ----------------------------------------------------------------- */
static ssize_t find_device_idx(struct device *devices, size_t count, const char *path)
{
    for (size_t i = 0; i < count; ++i) {
        if (devices[i].path && strcmp(devices[i].path, path) == 0)
            return (ssize_t)i;
    }
    return -1;
}

/* -----------------------------------------------------------------
 *  Remove device (close fds, destroy uinput, free strings, compact array)
 * ----------------------------------------------------------------- */
static void remove_device(struct device *devices, size_t *count, size_t idx)
{
    if (idx >= *count) return;

    if (devices[idx].ufd >= 0) {
        ioctl(devices[idx].ufd, UI_DEV_DESTROY);
        close(devices[idx].ufd);
    }
    if (devices[idx].fd >= 0) {
        ioctl(devices[idx].fd, EVIOCGRAB, 0);
        close(devices[idx].fd);
    }

    free(devices[idx].path);
    free(devices[idx].name);

    if (idx != *count - 1)
        devices[idx] = devices[*count - 1];

    (*count)--;
}

/* -----------------------------------------------------------------
 *  Add device by devnode path (grabs it and creates a uinput clone)
 * ----------------------------------------------------------------- */
static int add_device(struct device *devices, size_t *count, const char *path)
{
    if (*count >= MAX_DEVICES) {
        fprintf(stderr, "Too many devices (max %d)\n", MAX_DEVICES);
        return -1;
    }

    if (find_device_idx(devices, *count, path) >= 0)
        return 0; /* already added */

    struct device dev;
    memset(&dev, 0, sizeof(dev));
    dev.path = strdup(path);
    dev.name = NULL;
    dev.fd = -1;
    dev.ufd = -1;

    /* udev "add" can race /dev node creation; retry ENOENT a bit */
    for (int attempt = 0; attempt < 20; ++attempt) {
        dev.fd = open(path, O_RDONLY | O_NONBLOCK);
        if (dev.fd >= 0) break;
        if (errno == ENOENT) usleep(50000);
        else break;
    }
    if (dev.fd < 0) {
        perror(path);
        free(dev.path);
        return -1;
    }

    dev.name = read_device_name(dev.fd);

    if (ioctl(dev.fd, EVIOCGRAB, 1) < 0) {
        perror("EVIOCGRAB");
        close(dev.fd);
        free(dev.path);
        free(dev.name);
        return -1;
    }

    if (create_uinput(&dev) < 0) {
        fprintf(stderr, "Failed to create uinput for %s\n", path);
        ioctl(dev.fd, EVIOCGRAB, 0);
        close(dev.fd);
        free(dev.path);
        free(dev.name);
        return -1;
    }

    devices[*count] = dev;
    (*count)++;

    fprintf(stderr, "Filtering %s (%s) → uinput created\n",
            path, dev.name ? dev.name : "?");
    return 0;
}

/* -----------------------------------------------------------------
 *  Cleanup all devices
 * ----------------------------------------------------------------- */
static void cleanup_all(struct device *devices, size_t *count)
{
    while (*count > 0)
        remove_device(devices, count, *count - 1);
}

/* -----------------------------------------------------------------
 *  Initial enumeration for --auto
 * ----------------------------------------------------------------- */
static void enumerate_existing_trackpoints(struct udev *udev,
                                           const struct filters *f,
                                           struct device *devices,
                                           size_t *count)
{
    struct udev_enumerate *e = udev_enumerate_new(udev);
    if (!e) return;

    udev_enumerate_add_match_subsystem(e, "input");
    udev_enumerate_add_match_property(e, "ID_INPUT_POINTINGSTICK", "1");
    udev_enumerate_scan_devices(e);

    struct udev_list_entry *entry;
    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(e)) {
        const char *syspath = udev_list_entry_get_name(entry);
        struct udev_device *d = udev_device_new_from_syspath(udev, syspath);
        if (!d) continue;

        const char *devnode = udev_device_get_devnode(d);
        if (!devnode || strncmp(devnode, "/dev/input/event", 16) != 0) {
            udev_device_unref(d);
            continue;
        }

        if (!device_matches_filters(d, f)) {
            udev_device_unref(d);
            continue;
        }

        if (!has_middle_button(devnode)) {
            udev_device_unref(d);
            continue;
        }

        add_device(devices, count, devnode);
        udev_device_unref(d);
    }

    udev_enumerate_unref(e);
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
        "  --auto                           Auto-detect TrackPoints and keep\n"
        "                                   monitoring for hotplug add/remove.\n"
        "  --match REGEX                    Optional filter on udev ID_MODEL\n"
        "                                   (case-insensitive POSIX regex).\n"
        "  --vendor HEX                     Optional filter on udev ID_VENDOR_ID\n"
        "                                   (applies only if property exists).\n"
        "  --product HEX                    Optional filter on udev ID_MODEL_ID\n"
        "                                   (applies only if property exists).\n"
        "  -h, --help                       Show this help and exit.\n"
        "\n"
        "Compile:\n"
        "  gcc -Wall -O2 -D_GNU_SOURCE -o middle_button_disable middle-disable.c -ludev\n"
        "\n"
        "Run as root (or with suitable privileges for /dev/uinput + EVIOCGRAB).\n",
        progname);
}

/* -----------------------------------------------------------------
 *  main()
 * ----------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    static const struct option long_opts[] = {
        {"device",  required_argument, NULL, 'd'},
        {"auto",    no_argument,       NULL,  1 },
        {"match",   required_argument, NULL,  2 },
        {"vendor",  required_argument, NULL,  3 },
        {"product", required_argument, NULL,  4 },
        {"help",    no_argument,       NULL, 'h'},
        {0,0,0,0}
    };

    const char *explicit_paths[MAX_DEVICES];
    size_t explicit_cnt = 0;

    int do_auto = 0;
    const char *match_regex = NULL;

    struct filters filt;
    memset(&filt, 0, sizeof(filt));

    for (int c; (c = getopt_long(argc, argv, "d:h", long_opts, NULL)) != -1; ) {
        switch (c) {
        case 'd':
            if (explicit_cnt >= MAX_DEVICES) {
                fprintf(stderr, "Too many -d arguments (max %d)\n", MAX_DEVICES);
                return EXIT_FAILURE;
            }
            explicit_paths[explicit_cnt++] = optarg;
            break;
        case 1: /* --auto */
            do_auto = 1;
            break;
        case 2: /* --match */
            match_regex = optarg;
            break;
        case 3: /* --vendor */
            filt.vendor_id = optarg;
            break;
        case 4: /* --product */
            filt.product_id = optarg;
            break;
        case 'h':
        default:
            usage(argv[0]);
            return (c == 'h') ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    if (!do_auto && explicit_cnt == 0) {
        fprintf(stderr, "No devices selected – use -d or --auto.\n");
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (match_regex) {
        int rc = regcomp(&filt.regex, match_regex, REG_NOSUB | REG_ICASE);
        if (rc != 0) {
            char errbuf[128];
            regerror(rc, &filt.regex, errbuf, sizeof(errbuf));
            fprintf(stderr, "Invalid regex \"%s\": %s\n", match_regex, errbuf);
            return EXIT_FAILURE;
        }
        filt.have_regex = 1;
    }

    signal(SIGINT,  stop_handler);
    signal(SIGTERM, stop_handler);

    struct device devices[MAX_DEVICES];
    memset(devices, 0, sizeof(devices));
    for (size_t i = 0; i < MAX_DEVICES; ++i) {
        devices[i].fd = -1;
        devices[i].ufd = -1;
    }
    size_t total_devices = 0;

    /* udev monitor (only if --auto) */
    struct udev *udev = NULL;
    struct udev_monitor *mon = NULL;
    int mon_fd = -1;

    if (do_auto) {
        udev = udev_new();
        if (!udev) {
            fprintf(stderr, "udev_new() failed\n");
            if (filt.have_regex) regfree(&filt.regex);
            return EXIT_FAILURE;
        }

        mon = udev_monitor_new_from_netlink(udev, "udev");
        if (!mon) {
            fprintf(stderr, "udev_monitor_new_from_netlink() failed\n");
            udev_unref(udev);
            if (filt.have_regex) regfree(&filt.regex);
            return EXIT_FAILURE;
        }

        udev_monitor_filter_add_match_subsystem_devtype(mon, "input", NULL);
        udev_monitor_enable_receiving(mon);
        mon_fd = udev_monitor_get_fd(mon);

        /* Add existing TrackPoints at startup */
        enumerate_existing_trackpoints(udev, &filt, devices, &total_devices);
    }

    /* Add explicitly provided devices (no udev filtering here) */
    for (size_t i = 0; i < explicit_cnt; ++i)
        add_device(devices, &total_devices, explicit_paths[i]);

    /* ---------------- main loop ---------------- */
    struct pollfd fds[MAX_DEVICES + 1];

    while (running) {
        nfds_t nfds = 0;

        if (mon_fd >= 0) {
            fds[nfds].fd = mon_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }

        for (size_t i = 0; i < total_devices; ++i) {
            fds[nfds].fd = devices[i].fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }

        int ret = poll(fds, nfds, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        /* Handle udev hotplug */
        if (mon_fd >= 0 && (fds[0].revents & POLLIN)) {
            struct udev_device *ud;
            while ((ud = udev_monitor_receive_device(mon)) != NULL) {
                const char *action  = udev_device_get_action(ud);
                const char *devnode = udev_device_get_devnode(ud);

                if (action && devnode &&
                    strncmp(devnode, "/dev/input/event", 16) == 0)
                {
                    if (strcmp(action, "add") == 0) {
                        if (device_matches_filters(ud, &filt) &&
                            has_middle_button(devnode))
                        {
                            add_device(devices, &total_devices, devnode);
                        }
                    } else if (strcmp(action, "remove") == 0) {
                        ssize_t idx = find_device_idx(devices, total_devices, devnode);
                        if (idx >= 0) {
                            fprintf(stderr, "Device removed: %s\n", devnode);
                            remove_device(devices, &total_devices, (size_t)idx);
                        }
                    }
                }

                udev_device_unref(ud);
            }
        }

        /* Handle input events */
        size_t base = (mon_fd >= 0) ? 1 : 0;
        for (size_t i = 0; i < total_devices; ++i) {
            if (!(fds[base + i].revents & POLLIN)) continue;

            struct input_event evbuf[32];
            ssize_t nbytes = read(devices[i].fd, evbuf, sizeof(evbuf));
            if (nbytes < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;

                /* If the device disappears without us seeing a udev remove */
                if (errno == ENODEV || errno == ENXIO || errno == EIO) {
                    fprintf(stderr, "Device vanished: %s\n", devices[i].path);
                    remove_device(devices, &total_devices, i);
                    i--; /* array compacted; re-check this index */
                    continue;
                }

                perror("read");
                running = 0;
                break;
            }

            size_t evcnt = (size_t)nbytes / sizeof(struct input_event);
            for (size_t j = 0; j < evcnt; ++j)
                process_trackpoint(&devices[i], &evbuf[j]);
        }
    }

    cleanup_all(devices, &total_devices);

    if (mon) udev_monitor_unref(mon);
    if (udev) udev_unref(udev);

    if (filt.have_regex) regfree(&filt.regex);

    return EXIT_SUCCESS;
}

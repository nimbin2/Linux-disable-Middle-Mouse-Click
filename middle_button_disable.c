/*=====================================================================
 *  middle_button_disable.c
 *
 *  Disables the TrackPoint middle button (no BTN_MIDDLE ever reaches
 *  userspace, so no primary-selection paste) while keeping middle-button
 *  scrolling alive.
 *
 *  How:
 *    - Grab the real TrackPoint device (EVIOCGRAB) and re-emit its events
 *      through a uinput clone.
 *    - BTN_MIDDLE is swallowed and only tracked internally.
 *    - While the physical middle button is held, REL_X/REL_Y motion is
 *      translated into wheel events instead of pointer motion.
 *
 *  Wheel events are emitted as high-resolution (REL_WHEEL_HI_RES,
 *  120 units = one detent) *and* legacy (REL_WHEEL) events. libinput
 *  ignores legacy events on devices that advertise hi-res axes, so
 *  emitting only REL_WHEEL results in no scrolling at all.
 *
 *  Build:
 *      gcc -Wall -Wextra -O2 -D_GNU_SOURCE \
 *          -o middle_button_disable middle_button_disable.c -ludev
 *
 *  Run (needs access to /dev/input/event* and /dev/uinput):
 *      sudo ./middle_button_disable --auto
 *====================================================================*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
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

/* Older headers may lack the hi-res wheel codes. */
#ifndef REL_WHEEL_HI_RES
#define REL_WHEEL_HI_RES 0x0b
#endif
#ifndef REL_HWHEEL_HI_RES
#define REL_HWHEEL_HI_RES 0x0c
#endif

/* -----------------------------------------------------------------
 *  Constants
 * ----------------------------------------------------------------- */
#define MAX_DEVICES         32
#define UINPUT_NAME_PREFIX  "middle-disable: "
#define HI_RES_PER_DETENT   120     /* kernel ABI: 120 units == one click */
#define DEFAULT_THRESHOLD   8       /* raw REL units per detent */

/* -----------------------------------------------------------------
 *  Runtime options
 * ----------------------------------------------------------------- */
struct options {
    int threshold;      /* raw REL units per wheel detent (higher = slower) */
    int natural;        /* invert scroll direction */
    int hscroll;        /* horizontal scrolling enabled */
};

static struct options opts = {
    .threshold = DEFAULT_THRESHOLD,
    .natural   = 0,
    .hscroll   = 1,
};

/* -----------------------------------------------------------------
 *  Per-device state
 * ----------------------------------------------------------------- */
struct device {
    char *path;                 /* /dev/input/eventX */
    char *name;                 /* source device name */
    int   fd;                   /* real device */
    int   ufd;                  /* uinput clone */

    int   middle_down;          /* physical BTN_MIDDLE is held */
    int   native_wheel;         /* device emitted wheel itself while held */

    /* Scroll accumulators. sub_* holds raw motion scaled by
     * HI_RES_PER_DETENT; det_* holds hi-res units not yet converted
     * into a legacy detent. */
    long  sub_v, sub_h;
    long  det_v, det_h;
};

/* -----------------------------------------------------------------
 *  udev filters for --auto
 * ----------------------------------------------------------------- */
struct filters {
    const char *vendor_id;      /* ID_VENDOR_ID, checked only if present */
    const char *product_id;     /* ID_MODEL_ID,  checked only if present */
    int         have_regex;
    regex_t     regex;          /* matched against ID_MODEL */
};

/* -----------------------------------------------------------------
 *  Signals
 * ----------------------------------------------------------------- */
static volatile sig_atomic_t running = 1;
static void stop_handler(int sig) { (void)sig; running = 0; }

static inline int test_bit(unsigned int nr, const unsigned long *addr)
{
    return (addr[nr / (8 * sizeof(unsigned long))] >>
            (nr % (8 * sizeof(unsigned long)))) & 1;
}

/* -----------------------------------------------------------------
 *  uinput output
 * ----------------------------------------------------------------- */
static void emit_event(struct device *dev, const struct input_event *ev)
{
    ssize_t n = write(dev->ufd, ev, sizeof(*ev));
    (void)n;    /* uinput writes are all-or-nothing; nothing to retry */
}

static void emit_rel(struct device *dev, const struct input_event *src,
                     int code, int value)
{
    struct input_event ev = *src;
    ev.type  = EV_REL;
    ev.code  = code;
    ev.value = value;
    emit_event(dev, &ev);
}

/* -----------------------------------------------------------------
 *  Scroll conversion
 *
 *  raw   : motion delta on this axis, sign already normalised so that
 *          positive == wheel up / wheel right
 *  sub   : leftover raw motion, scaled by HI_RES_PER_DETENT
 *  det   : leftover hi-res units toward the next legacy detent
 * ----------------------------------------------------------------- */
static void scroll_axis(struct device *dev, const struct input_event *src,
                        int raw, long *sub, long *det,
                        int code_hi, int code_lo)
{
    *sub += (long)raw * HI_RES_PER_DETENT;

    long hi = *sub / opts.threshold;    /* truncates toward zero: symmetric */
    if (hi == 0)
        return;
    *sub -= hi * opts.threshold;

    emit_rel(dev, src, code_hi, (int)hi);

    *det += hi;
    long steps = *det / HI_RES_PER_DETENT;
    if (steps != 0) {
        *det -= steps * HI_RES_PER_DETENT;
        emit_rel(dev, src, code_lo, (int)steps);
    }
}

static void reset_scroll(struct device *dev)
{
    dev->sub_v = dev->sub_h = 0;
    dev->det_v = dev->det_h = 0;
    dev->native_wheel = 0;
}

/* -----------------------------------------------------------------
 *  Core event filter
 * ----------------------------------------------------------------- */
static void process_event(struct device *dev, const struct input_event *ev)
{
    /* Middle button: never forwarded, only tracked. */
    if (ev->type == EV_KEY && ev->code == BTN_MIDDLE) {
        if (ev->value == 1) {
            dev->middle_down = 1;
            reset_scroll(dev);
        } else if (ev->value == 0) {
            dev->middle_down = 0;
            reset_scroll(dev);
        }
        return;
    }

    if (!dev->middle_down) {
        emit_event(dev, ev);
        return;
    }

    /* ---- middle button held: scroll mode ---- */

    if (ev->type == EV_REL) {
        switch (ev->code) {
        case REL_WHEEL:
        case REL_HWHEEL:
        case REL_WHEEL_HI_RES:
        case REL_HWHEEL_HI_RES:
            /* Device scrolls on its own; don't emulate on top of it. */
            dev->native_wheel = 1;
            emit_event(dev, ev);
            return;

        case REL_Y:
            if (dev->native_wheel)
                return;
            /* REL_Y is positive downward, wheel up is positive. */
            scroll_axis(dev, ev,
                        opts.natural ? ev->value : -ev->value,
                        &dev->sub_v, &dev->det_v,
                        REL_WHEEL_HI_RES, REL_WHEEL);
            return;

        case REL_X:
            if (dev->native_wheel || !opts.hscroll)
                return;
            scroll_axis(dev, ev,
                        opts.natural ? -ev->value : ev->value,
                        &dev->sub_h, &dev->det_h,
                        REL_HWHEEL_HI_RES, REL_HWHEEL);
            return;

        default:
            return;     /* swallow other motion while scrolling */
        }
    }

    /* Buttons, SYN, everything else passes through unchanged. */
    emit_event(dev, ev);
}

/* -----------------------------------------------------------------
 *  uinput clone creation
 * ----------------------------------------------------------------- */
static int create_uinput(struct device *dev)
{
    struct input_id id;
    struct uinput_setup setup;

    dev->ufd = open("/dev/uinput", O_WRONLY);
    if (dev->ufd < 0) {
        perror("/dev/uinput");
        return -1;
    }

    if (ioctl(dev->ufd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(dev->ufd, UI_SET_EVBIT, EV_REL) < 0 ||
        ioctl(dev->ufd, UI_SET_EVBIT, EV_SYN) < 0)
        goto fail;

    if (ioctl(dev->ufd, UI_SET_KEYBIT, BTN_LEFT)   < 0 ||
        ioctl(dev->ufd, UI_SET_KEYBIT, BTN_RIGHT)  < 0)
        goto fail;

    /* BTN_MIDDLE is deliberately NOT declared: the clone has no middle
     * button at all, so nothing downstream can synthesise a paste. */

    if (ioctl(dev->ufd, UI_SET_RELBIT, REL_X)             < 0 ||
        ioctl(dev->ufd, UI_SET_RELBIT, REL_Y)             < 0 ||
        ioctl(dev->ufd, UI_SET_RELBIT, REL_WHEEL)         < 0 ||
        ioctl(dev->ufd, UI_SET_RELBIT, REL_HWHEEL)        < 0 ||
        ioctl(dev->ufd, UI_SET_RELBIT, REL_WHEEL_HI_RES)  < 0 ||
        ioctl(dev->ufd, UI_SET_RELBIT, REL_HWHEEL_HI_RES) < 0)
        goto fail;

    /* Advertise as a pointing stick so libinput applies TrackPoint
     * acceleration instead of generic mouse acceleration. */
#ifdef INPUT_PROP_POINTING_STICK
    ioctl(dev->ufd, UI_SET_PROPBIT, INPUT_PROP_POINTING_STICK);
#endif
#ifdef INPUT_PROP_POINTER
    ioctl(dev->ufd, UI_SET_PROPBIT, INPUT_PROP_POINTER);
#endif

    memset(&setup, 0, sizeof(setup));
    memset(&id, 0, sizeof(id));

    /* Inherit vendor/product from the source, but stay on BUS_VIRTUAL so
     * the clone lands under /sys/devices/virtual/ and our own hotplug
     * filter can exclude it. */
    if (ioctl(dev->fd, EVIOCGID, &id) == 0) {
        setup.id.vendor  = id.vendor;
        setup.id.product = id.product;
        setup.id.version = id.version;
    } else {
        setup.id.vendor  = 0xfeed;
        setup.id.product = 0x0001;
        setup.id.version = 1;
    }
    setup.id.bustype = BUS_VIRTUAL;

    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "%s%s",
             UINPUT_NAME_PREFIX, dev->name ? dev->name : "TrackPoint");

    if (ioctl(dev->ufd, UI_DEV_SETUP, &setup) < 0) goto fail;
    if (ioctl(dev->ufd, UI_DEV_CREATE, 0)     < 0) goto fail;

    usleep(100000);     /* let udev settle before events start flowing */
    return 0;

fail:
    perror("uinput setup");
    close(dev->ufd);
    dev->ufd = -1;
    return -1;
}

/* -----------------------------------------------------------------
 *  Device helpers
 * ----------------------------------------------------------------- */
static int has_middle_button(const char *path)
{
    unsigned long bits[(KEY_MAX / (8 * sizeof(unsigned long))) + 1];
    int fd = open(path, O_RDONLY);
    int ok;

    if (fd < 0)
        return 0;

    memset(bits, 0, sizeof(bits));
    ok = (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) >= 0) &&
         test_bit(BTN_MIDDLE, bits);
    close(fd);
    return ok;
}

static char *read_device_name(int fd)
{
    char buf[256];

    memset(buf, 0, sizeof(buf));
    if (ioctl(fd, EVIOCGNAME(sizeof(buf)), buf) < 0 || buf[0] == '\0')
        return strdup("TrackPoint");
    return strdup(buf);
}

static ssize_t find_device_idx(struct device *devs, size_t count, const char *path)
{
    for (size_t i = 0; i < count; ++i)
        if (devs[i].path && strcmp(devs[i].path, path) == 0)
            return (ssize_t)i;
    return -1;
}

static void remove_device(struct device *devs, size_t *count, size_t idx)
{
    if (idx >= *count)
        return;

    if (devs[idx].ufd >= 0) {
        ioctl(devs[idx].ufd, UI_DEV_DESTROY);
        close(devs[idx].ufd);
    }
    if (devs[idx].fd >= 0) {
        ioctl(devs[idx].fd, EVIOCGRAB, 0);
        close(devs[idx].fd);
    }

    free(devs[idx].path);
    free(devs[idx].name);

    if (idx != *count - 1)
        devs[idx] = devs[*count - 1];
    memset(&devs[*count - 1], 0, sizeof(struct device));
    devs[*count - 1].fd = -1;
    devs[*count - 1].ufd = -1;

    (*count)--;
}

static void cleanup_all(struct device *devs, size_t *count)
{
    while (*count > 0)
        remove_device(devs, count, *count - 1);
}

static int add_device(struct device *devs, size_t *count, const char *path)
{
    struct device dev;

    if (*count >= MAX_DEVICES) {
        fprintf(stderr, "Too many devices (max %d)\n", MAX_DEVICES);
        return -1;
    }
    if (find_device_idx(devs, *count, path) >= 0)
        return 0;

    memset(&dev, 0, sizeof(dev));
    dev.fd  = -1;
    dev.ufd = -1;
    dev.path = strdup(path);
    if (!dev.path)
        return -1;

    /* Hotplugged nodes can appear slightly before they are openable. */
    for (int attempt = 0; attempt < 20; ++attempt) {
        dev.fd = open(path, O_RDONLY);
        if (dev.fd >= 0)
            break;
        if (errno != ENOENT && errno != EACCES)
            break;
        usleep(50000);
    }
    if (dev.fd < 0) {
        perror(path);
        free(dev.path);
        return -1;
    }

    dev.name = read_device_name(dev.fd);

    if (ioctl(dev.fd, EVIOCGRAB, 1) < 0) {
        perror("EVIOCGRAB");
        goto fail;
    }

    if (create_uinput(&dev) < 0) {
        fprintf(stderr, "Failed to create uinput clone for %s\n", path);
        ioctl(dev.fd, EVIOCGRAB, 0);
        goto fail;
    }

    devs[*count] = dev;
    (*count)++;

    fprintf(stderr, "Filtering %s (%s)\n", path, dev.name);
    return 0;

fail:
    close(dev.fd);
    free(dev.path);
    free(dev.name);
    return -1;
}

/* -----------------------------------------------------------------
 *  udev matching
 * ----------------------------------------------------------------- */
static int is_virtual(struct udev_device *d)
{
    const char *syspath = udev_device_get_syspath(d);
    return syspath && strstr(syspath, "/devices/virtual/") != NULL;
}

static int hex_match_opt(const char *opt, const char *udev_val)
{
    if (!opt || !*opt)          return 1;
    if (!udev_val || !*udev_val) return 1;

    while (!strncasecmp(opt, "0x", 2))
        opt += 2;
    return strcasecmp(opt, udev_val) == 0;
}

static int device_matches(struct udev_device *d, const struct filters *f)
{
    const char *flag, *model, *vid, *pid;

    if (is_virtual(d))
        return 0;

    flag = udev_device_get_property_value(d, "ID_INPUT_POINTINGSTICK");
    if (!flag || strcmp(flag, "1") != 0)
        return 0;

    model = udev_device_get_property_value(d, "ID_MODEL");
    vid   = udev_device_get_property_value(d, "ID_VENDOR_ID");
    pid   = udev_device_get_property_value(d, "ID_MODEL_ID");

    if (!hex_match_opt(f->vendor_id, vid))  return 0;
    if (!hex_match_opt(f->product_id, pid)) return 0;

    if (f->have_regex) {
        if (!model)
            return 0;
        if (regexec((regex_t *)&f->regex, model, 0, NULL, 0) != 0)
            return 0;
    }
    return 1;
}

static void enumerate_trackpoints(struct udev *udev, const struct filters *f,
                                  struct device *devs, size_t *count)
{
    struct udev_enumerate *e = udev_enumerate_new(udev);
    struct udev_list_entry *entry;

    if (!e)
        return;

    udev_enumerate_add_match_subsystem(e, "input");
    udev_enumerate_add_match_property(e, "ID_INPUT_POINTINGSTICK", "1");
    udev_enumerate_scan_devices(e);

    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(e)) {
        struct udev_device *d =
            udev_device_new_from_syspath(udev, udev_list_entry_get_name(entry));
        const char *devnode;

        if (!d)
            continue;

        devnode = udev_device_get_devnode(d);
        if (devnode &&
            strncmp(devnode, "/dev/input/event", 16) == 0 &&
            device_matches(d, f) &&
            has_middle_button(devnode))
            add_device(devs, count, devnode);

        udev_device_unref(d);
    }

    udev_enumerate_unref(e);
}

/* -----------------------------------------------------------------
 *  CLI
 * ----------------------------------------------------------------- */
static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Device selection:\n"
        "  -d, --device PATH    Filter a specific /dev/input/eventX (repeatable).\n"
        "      --auto           Auto-detect TrackPoints and follow hotplug.\n"
        "      --match REGEX    Restrict --auto by udev ID_MODEL (case-insensitive).\n"
        "      --vendor HEX     Restrict --auto by udev ID_VENDOR_ID.\n"
        "      --product HEX    Restrict --auto by udev ID_MODEL_ID.\n"
        "\n"
        "Scrolling:\n"
        "  -t, --threshold N    Motion units per wheel detent (default %d,\n"
        "                       higher = slower scrolling).\n"
        "  -n, --natural        Invert scroll direction.\n"
        "      --no-hscroll     Disable horizontal scrolling.\n"
        "\n"
        "  -h, --help           Show this help.\n"
        "\n"
        "Build:\n"
        "  gcc -Wall -Wextra -O2 -D_GNU_SOURCE -o %s %s.c -ludev\n",
        prog, DEFAULT_THRESHOLD, prog, prog);
}

int main(int argc, char *argv[])
{
    static const struct option long_opts[] = {
        {"device",     required_argument, NULL, 'd'},
        {"threshold",  required_argument, NULL, 't'},
        {"natural",    no_argument,       NULL, 'n'},
        {"auto",       no_argument,       NULL,  1 },
        {"match",      required_argument, NULL,  2 },
        {"vendor",     required_argument, NULL,  3 },
        {"product",    required_argument, NULL,  4 },
        {"no-hscroll", no_argument,       NULL,  5 },
        {"help",       no_argument,       NULL, 'h'},
        {0, 0, 0, 0}
    };

    const char *explicit_paths[MAX_DEVICES];
    size_t explicit_cnt = 0;
    const char *match_regex = NULL;
    int do_auto = 0;

    struct filters filt;
    struct device devs[MAX_DEVICES];
    size_t total = 0;

    struct udev *udev = NULL;
    struct udev_monitor *mon = NULL;
    int mon_fd = -1;

    struct pollfd fds[MAX_DEVICES + 1];
    size_t devidx[MAX_DEVICES + 1];

    memset(&filt, 0, sizeof(filt));

    for (int c; (c = getopt_long(argc, argv, "d:t:nh", long_opts, NULL)) != -1; ) {
        switch (c) {
        case 'd':
            if (explicit_cnt >= MAX_DEVICES) {
                fprintf(stderr, "Too many -d arguments (max %d)\n", MAX_DEVICES);
                return EXIT_FAILURE;
            }
            explicit_paths[explicit_cnt++] = optarg;
            break;
        case 't': {
            int v = atoi(optarg);
            if (v < 1 || v > 1000) {
                fprintf(stderr, "--threshold must be between 1 and 1000\n");
                return EXIT_FAILURE;
            }
            opts.threshold = v;
            break;
        }
        case 'n': opts.natural = 1;      break;
        case  1:  do_auto = 1;           break;
        case  2:  match_regex = optarg;  break;
        case  3:  filt.vendor_id = optarg;  break;
        case  4:  filt.product_id = optarg; break;
        case  5:  opts.hscroll = 0;      break;
        case 'h':
        default:
            usage(argv[0]);
            return (c == 'h') ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    if (!do_auto && explicit_cnt == 0) {
        fprintf(stderr, "No devices selected - use -d or --auto.\n\n");
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (match_regex) {
        int rc = regcomp(&filt.regex, match_regex, REG_NOSUB | REG_ICASE);
        if (rc != 0) {
            char err[128];
            regerror(rc, &filt.regex, err, sizeof(err));
            fprintf(stderr, "Invalid regex \"%s\": %s\n", match_regex, err);
            return EXIT_FAILURE;
        }
        filt.have_regex = 1;
    }

    signal(SIGINT,  stop_handler);
    signal(SIGTERM, stop_handler);

    memset(devs, 0, sizeof(devs));
    for (size_t i = 0; i < MAX_DEVICES; ++i) {
        devs[i].fd  = -1;
        devs[i].ufd = -1;
    }

    if (do_auto) {
        udev = udev_new();
        if (!udev) {
            fprintf(stderr, "udev_new() failed\n");
            goto done;
        }

        mon = udev_monitor_new_from_netlink(udev, "udev");
        if (!mon) {
            fprintf(stderr, "udev_monitor_new_from_netlink() failed\n");
            goto done;
        }

        udev_monitor_filter_add_match_subsystem_devtype(mon, "input", NULL);
        udev_monitor_enable_receiving(mon);
        mon_fd = udev_monitor_get_fd(mon);

        enumerate_trackpoints(udev, &filt, devs, &total);
    }

    for (size_t i = 0; i < explicit_cnt; ++i)
        add_device(devs, &total, explicit_paths[i]);

    if (total == 0)
        fprintf(stderr, "Warning: no matching device found yet.\n");

    while (running) {
        nfds_t nfds = 0;
        size_t base;
        int dead[MAX_DEVICES];

        if (mon_fd >= 0) {
            fds[nfds].fd      = mon_fd;
            fds[nfds].events  = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }
        base = nfds;

        for (size_t i = 0; i < total; ++i) {
            fds[nfds].fd      = devs[i].fd;
            fds[nfds].events  = POLLIN;
            fds[nfds].revents = 0;
            devidx[nfds]      = i;
            nfds++;
        }

        if (poll(fds, nfds, -1) < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        /* --- hotplug --- */
        if (mon_fd >= 0 && (fds[0].revents & POLLIN)) {
            struct udev_device *ud;
            while ((ud = udev_monitor_receive_device(mon)) != NULL) {
                const char *action  = udev_device_get_action(ud);
                const char *devnode = udev_device_get_devnode(ud);

                if (action && devnode &&
                    strncmp(devnode, "/dev/input/event", 16) == 0) {
                    if (strcmp(action, "add") == 0) {
                        if (device_matches(ud, &filt) && has_middle_button(devnode))
                            add_device(devs, &total, devnode);
                    } else if (strcmp(action, "remove") == 0) {
                        ssize_t idx = find_device_idx(devs, total, devnode);
                        if (idx >= 0) {
                            fprintf(stderr, "Device removed: %s\n", devnode);
                            remove_device(devs, &total, (size_t)idx);
                        }
                    }
                }
                udev_device_unref(ud);
            }
            /* The device list may have changed; rebuild the poll set. */
            continue;
        }

        /* --- input events --- */
        memset(dead, 0, sizeof(dead));

        for (nfds_t k = base; k < nfds; ++k) {
            struct input_event evbuf[64];
            size_t i = devidx[k];
            ssize_t nbytes;

            if (fds[k].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                dead[i] = 1;
                continue;
            }
            if (!(fds[k].revents & POLLIN))
                continue;

            nbytes = read(devs[i].fd, evbuf, sizeof(evbuf));
            if (nbytes < 0) {
                if (errno == EAGAIN || errno == EINTR)
                    continue;
                fprintf(stderr, "%s: %s\n", devs[i].path, strerror(errno));
                dead[i] = 1;
                continue;
            }

            for (size_t j = 0; j < (size_t)nbytes / sizeof(struct input_event); ++j)
                process_event(&devs[i], &evbuf[j]);
        }

        /* Remove in descending order: remove_device() swaps in the last entry. */
        for (size_t i = total; i-- > 0; ) {
            if (dead[i]) {
                fprintf(stderr, "Dropping %s\n", devs[i].path);
                remove_device(devs, &total, i);
            }
        }
    }

done:
    cleanup_all(devs, &total);
    if (mon)  udev_monitor_unref(mon);
    if (udev) udev_unref(udev);
    if (filt.have_regex) regfree(&filt.regex);

    return EXIT_SUCCESS;
}

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define INTERNAL "/dev/input/event13"
#define EXTERNAL "/dev/input/event15"

struct device {
    const char *path;
    const char *name;
    int fd;
    int ufd;
    int middle_down;
    int middle_sent;
};

static volatile sig_atomic_t running = 1;

static void stop_handler(int sig)
{
    (void)sig;
    running = 0;
}

static int emit_event(struct device *dev, const struct input_event *ev)
{
    ssize_t n = write(dev->ufd, ev, sizeof(*ev));

    if (n != sizeof(*ev))
        return -1;

    return 0;
}

static int emit_key(struct device *dev,
                    const struct input_event *src,
                    int code,
                    int value)
{
    struct input_event ev = *src;

    ev.type = EV_KEY;
    ev.code = code;
    ev.value = value;

    return emit_event(dev, &ev);
}

static int create_uinput(struct device *dev)
{
    dev->ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);

    if (dev->ufd < 0) {
        perror("/dev/uinput");
        return -1;
    }

    /*
     * We only need the event types/codes that these TrackPoint
     * devices actually use.
     */
    if (ioctl(dev->ufd, UI_SET_EVBIT, EV_KEY) < 0)
        return -1;

    if (ioctl(dev->ufd, UI_SET_EVBIT, EV_REL) < 0)
        return -1;

    if (ioctl(dev->ufd, UI_SET_EVBIT, EV_SYN) < 0)
        return -1;

    /*
     * Keyboard buttons.
     */
    if (ioctl(dev->ufd, UI_SET_KEYBIT, BTN_LEFT) < 0)
        return -1;

    if (ioctl(dev->ufd, UI_SET_KEYBIT, BTN_RIGHT) < 0)
        return -1;

    if (ioctl(dev->ufd, UI_SET_KEYBIT, BTN_MIDDLE) < 0)
        return -1;

    /*
     * TrackPoint movement and wheel.
     */
    if (ioctl(dev->ufd, UI_SET_RELBIT, REL_X) < 0)
        return -1;

    if (ioctl(dev->ufd, UI_SET_RELBIT, REL_Y) < 0)
        return -1;

    if (ioctl(dev->ufd, UI_SET_RELBIT, REL_WHEEL) < 0)
        return -1;

    if (ioctl(dev->ufd, UI_SET_RELBIT, REL_HWHEEL) < 0)
        return -1;

    if (ioctl(dev->ufd, UI_SET_RELBIT, REL_WHEEL_HI_RES) < 0)
        return -1;

    if (ioctl(dev->ufd, UI_SET_RELBIT, REL_HWHEEL_HI_RES) < 0)
        return -1;

    struct uinput_setup setup = {
        .id = {
            .bustype = BUS_USB,
            .vendor = 0x17ef,
            .product = 0x60ee,
            .version = 1
        }
    };

    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "%s", dev->name);

    if (ioctl(dev->ufd, UI_DEV_SETUP, &setup) < 0)
        return -1;

    if (ioctl(dev->ufd, UI_DEV_CREATE) < 0)
        return -1;

    /*
     * Give the input subsystem time to create the device.
     */
    usleep(100000);

    return 0;
}

static void process_internal(struct device *dev,
                             const struct input_event *ev)
{
    /*
     * Middle button is held.
     *
     * Don't forward it yet. We don't know whether this is going
     * to be a middle click or TrackPoint scrolling.
     */
    if (ev->type == EV_KEY && ev->code == BTN_MIDDLE) {
        if (ev->value == 1) {
            dev->middle_down = 1;
            return;
        }

        if (ev->value == 0) {
            if (dev->middle_sent)
                emit_key(dev, ev, BTN_MIDDLE, 0);

            dev->middle_down = 0;
            dev->middle_sent = 0;
            return;
        }

        /*
         * Repeat events aren't useful for BTN_MIDDLE here.
         */
        return;
    }

    /*
     * A wheel event while the physical middle button is held
     * means TrackPoint scrolling.
     */
    if (dev->middle_down &&
        !dev->middle_sent &&
        ev->type == EV_REL &&
        (ev->code == REL_WHEEL ||
         ev->code == REL_HWHEEL ||
         ev->code == REL_WHEEL_HI_RES ||
         ev->code == REL_HWHEEL_HI_RES)) {

        emit_key(dev, ev, BTN_MIDDLE, 1);
        dev->middle_sent = 1;
    }

    /*
     * Everything except the delayed middle-button event is
     * forwarded normally.
     */
    emit_event(dev, ev);
}

static void process_external(struct device *dev,
                             const struct input_event *ev)
{
    /*
     * The external TrackPoint's middle button is completely
     * disabled.
     */
    if (ev->type == EV_KEY && ev->code == BTN_MIDDLE)
        return;

    emit_event(dev, ev);
}

static void cleanup(struct device *devices, size_t count)
{
    for (size_t i = 0; i < count; i++) {
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

int main(void)
{
    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);

    struct device devices[] = {
        {
            .path = INTERNAL,
            .name = "ThinkPad TrackPoint (filtered)",
            .fd = -1,
            .ufd = -1,
            .middle_down = 0,
            .middle_sent = 0
        },
        {
            .path = EXTERNAL,
            .name = "Lenovo TrackPoint Keyboard II (filtered)",
            .fd = -1,
            .ufd = -1,
            .middle_down = 0,
            .middle_sent = 0
        }
    };

    const size_t count = sizeof(devices) / sizeof(devices[0]);

    for (size_t i = 0; i < count; i++) {
        devices[i].fd = open(devices[i].path, O_RDONLY | O_NONBLOCK);

        if (devices[i].fd < 0) {
            perror(devices[i].path);
            cleanup(devices, i);
            return 1;
        }

        if (ioctl(devices[i].fd, EVIOCGRAB, 1) < 0) {
            perror("EVIOCGRAB");
            cleanup(devices, i + 1);
            return 1;
        }

        if (create_uinput(&devices[i]) < 0) {
            fprintf(stderr, "Failed to create uinput device for %s\n",
                    devices[i].path);
            cleanup(devices, i + 1);
            return 1;
        }

        fprintf(stderr, "Filtering %s\n", devices[i].path);
    }

    struct pollfd fds[2];

    while (running) {
        for (size_t i = 0; i < count; i++) {
            fds[i].fd = devices[i].fd;
            fds[i].events = POLLIN;
            fds[i].revents = 0;
        }

        int ret = poll(fds, count, -1);

        if (ret < 0) {
            if (errno == EINTR)
                continue;

            perror("poll");
            break;
        }

        for (size_t i = 0; i < count; i++) {
            if (!(fds[i].revents & POLLIN))
                continue;

            struct input_event events[32];

            ssize_t bytes = read(
                devices[i].fd,
                events,
                sizeof(events)
            );

            if (bytes < 0) {
                if (errno == EAGAIN || errno == EINTR)
                    continue;

                perror("read");
                running = 0;
                break;
            }

            size_t event_count =
                (size_t)bytes / sizeof(struct input_event);

            for (size_t j = 0; j < event_count; j++) {
                if (i == 0)
                    process_internal(&devices[i], &events[j]);
                else
                    process_external(&devices[i], &events[j]);
            }
        }
    }

    cleanup(devices, count);

    return 0;
}

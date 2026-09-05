// three-finger-drag — macOS-style three-finger drag for Linux touchpads.
//
// Reads the touchpads' raw multitouch events, and when three fingers move
// together, synthesises a left-button drag on a virtual pointer device.
// Every capable touchpad is watched at once — some devices (clones of Apple's
// Magic Trackpad among them) expose several event nodes that all look like the
// touchpad, and only the kernel knows which one carries the touches. Watching
// them all costs nothing and means plugging in a second touchpad just works.
// Hotplug is handled with inotify on /dev/input.
//
// Dependencies: none. Just the kernel uapi headers and libc.
//
// Why synthesise the motion at all? Because when three fingers are down,
// libinput classifies the contact as a swipe gesture and emits no pointer
// motion whatsoever. Pressing a button is therefore not enough — we have to
// move the cursor ourselves, from the average position of the fingers on the pad.
//
// We open the touchpad read-only and never call EVIOCGRAB, so the real device
// keeps behaving exactly as it always did. If this program dies, nothing is lost
// but the feature.
//
// SPDX-License-Identifier: MIT

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/inotify.h>
#include <sys/ioctl.h>

#include <linux/input.h>
#include <linux/uinput.h>

#define PROG "three-finger-drag"

#define MAX_SLOTS 16
#define MAX_PADS 8
#define FINGERS_REQUIRED 3

// A 1 mm finger movement moves the cursor this far before --sensitivity is
// applied. X11 layers its own pointer acceleration on top of what we emit.
#define BASE_PX_PER_MM 6.0

#define BITS_PER_LONG (int)(8 * sizeof(long))
#define NLONGS(x) (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define test_bit(bit, arr) (((arr)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1)

// ---------------------------------------------------------------- options ---

static struct {
    const char *device;      // explicit device path, or NULL to autodetect
    double sensitivity;      // cursor speed multiplier
    double threshold_mm;     // movement needed before a drag starts
    int    grace_ms;         // hold the button after ALL fingers lift
    int    settle_ms;        // hold the button after dropping to 1-2 fingers
    int    max_drag_ms;      // safety net: never hold the button longer than this
    bool   debug;
} opt = {
    .device       = NULL,
    .sensitivity  = 1.0,
    .threshold_mm = 1.5,
    .grace_ms     = 500,
    .settle_ms    = 40,
    .max_drag_ms  = 30000,
    .debug        = false,
};

static long elapsed_ms(void);

#define dbg(...) do { if (opt.debug) { \
    fprintf(stderr, "[" PROG " %7ld] ", elapsed_ms()); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define warn(...) do { fprintf(stderr, "[" PROG "] " __VA_ARGS__); fputc('\n', stderr); } while (0)

// ---------------------------------------------------------------- signals ---

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

// ------------------------------------------------------------------ clock ---

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long g_start_ms;

// Milliseconds since startup, for readable debug traces.
static long elapsed_ms(void) { return now_ms() - g_start_ms; }

// -------------------------------------------------------- touchpad device ---

struct touchpad {
    int    fd;
    char   path[320];
    char   name[256];
    int    slot_count;
    double res_x;   // units per mm
    double res_y;
};

// A touchpad we can use reports three-finger contact and per-slot coordinates.
static bool device_is_usable(int fd, char *name, size_t name_sz)
{
    unsigned long keys[NLONGS(KEY_MAX + 1)];
    unsigned long abs[NLONGS(ABS_MAX + 1)];

    memset(keys, 0, sizeof keys);
    memset(abs,  0, sizeof abs);

    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0) return false;
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs),  abs)  < 0) return false;

    if (!test_bit(BTN_TOOL_TRIPLETAP, keys)) return false;
    if (!test_bit(ABS_MT_SLOT, abs))         return false;
    if (!test_bit(ABS_MT_POSITION_X, abs))   return false;
    if (!test_bit(ABS_MT_POSITION_Y, abs))   return false;

    if (ioctl(fd, EVIOCGNAME(name_sz), name) < 0)
        snprintf(name, name_sz, "(unnamed)");

    return true;
}

static double axis_resolution(int fd, unsigned axis, const char *label)
{
    struct input_absinfo info;
    if (ioctl(fd, EVIOCGABS(axis), &info) < 0) return 0.0;

    if (info.resolution > 0)
        return (double)info.resolution;

    // Some touchpads report no resolution. Assume a 100 mm axis, which is close
    // enough for a sensitivity knob the user tunes by feel anyway.
    double span = (double)(info.maximum - info.minimum);
    if (span <= 0) return 0.0;
    dbg("%s reports no resolution; assuming a 100 mm axis", label);
    return span / 100.0;
}

static bool touchpad_try_open(struct touchpad *tp, const char *path)
{
    memset(tp, 0, sizeof *tp);
    tp->fd = -1;

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    if (!device_is_usable(fd, tp->name, sizeof tp->name)) {
        close(fd);
        return false;
    }

    tp->fd = fd;
    snprintf(tp->path, sizeof tp->path, "%s", path);

    struct input_absinfo slot_info;
    tp->slot_count = MAX_SLOTS;
    if (ioctl(tp->fd, EVIOCGABS(ABS_MT_SLOT), &slot_info) == 0) {
        int n = slot_info.maximum + 1;
        if (n > 0 && n < MAX_SLOTS) tp->slot_count = n;
    }

    tp->res_x = axis_resolution(tp->fd, ABS_MT_POSITION_X, "ABS_MT_POSITION_X");
    tp->res_y = axis_resolution(tp->fd, ABS_MT_POSITION_Y, "ABS_MT_POSITION_Y");
    if (tp->res_x <= 0) tp->res_x = 1.0;
    if (tp->res_y <= 0) tp->res_y = 1.0;

    dbg("using %s (%s), %d slots, %.1f x %.1f units/mm",
        tp->path, tp->name, tp->slot_count, tp->res_x, tp->res_y);
    return true;
}

static void touchpad_close(struct touchpad *tp)
{
    if (tp->fd >= 0) close(tp->fd);
    tp->fd = -1;
}

// -------------------------------------------------------- virtual pointer ---

struct vpointer {
    int  fd;
    bool button_down;
    // With several touchpads sharing one virtual pointer, the button belongs
    // to whichever tracker pressed it. Nobody else may start or end a drag.
    const void *owner;
};

static bool vpointer_create(struct vpointer *vp)
{
    memset(vp, 0, sizeof *vp);

    vp->fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (vp->fd < 0) {
        warn("cannot open /dev/uinput: %s", strerror(errno));
        warn("this program needs write access to /dev/uinput (run as root)");
        return false;
    }

    if (ioctl(vp->fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(vp->fd, UI_SET_KEYBIT, BTN_LEFT) < 0 ||
        ioctl(vp->fd, UI_SET_EVBIT, EV_REL) < 0 ||
        ioctl(vp->fd, UI_SET_RELBIT, REL_X) < 0 ||
        ioctl(vp->fd, UI_SET_RELBIT, REL_Y) < 0) {
        warn("cannot configure the virtual pointer: %s", strerror(errno));
        close(vp->fd);
        vp->fd = -1;
        return false;
    }

    struct uinput_setup setup;
    memset(&setup, 0, sizeof setup);
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor  = 0x1d6b;   // Linux Foundation, as good a choice as any
    setup.id.product = 0x0003;
    setup.id.version = 1;
    snprintf(setup.name, sizeof setup.name, "three-finger-drag virtual pointer");

    if (ioctl(vp->fd, UI_DEV_SETUP, &setup) < 0 || ioctl(vp->fd, UI_DEV_CREATE) < 0) {
        warn("cannot create the virtual pointer: %s", strerror(errno));
        close(vp->fd);
        vp->fd = -1;
        return false;
    }

    dbg("virtual pointer created");
    return true;
}

static void vpointer_emit(struct vpointer *vp, unsigned type, unsigned code, int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type  = (unsigned short)type;
    ev.code  = (unsigned short)code;
    ev.value = value;

    if (write(vp->fd, &ev, sizeof ev) != (ssize_t)sizeof ev)
        warn("failed to write to the virtual pointer: %s", strerror(errno));
}

static void vpointer_sync(struct vpointer *vp) { vpointer_emit(vp, EV_SYN, SYN_REPORT, 0); }

static void vpointer_button(struct vpointer *vp, bool down)
{
    if (vp->button_down == down) return;
    vpointer_emit(vp, EV_KEY, BTN_LEFT, down ? 1 : 0);
    vpointer_sync(vp);
    vp->button_down = down;
    dbg("button %s", down ? "DOWN" : "UP");
}

static void vpointer_move(struct vpointer *vp, int dx, int dy)
{
    if (dx == 0 && dy == 0) return;
    if (dx) vpointer_emit(vp, EV_REL, REL_X, dx);
    if (dy) vpointer_emit(vp, EV_REL, REL_Y, dy);
    vpointer_sync(vp);
}

static void vpointer_destroy(struct vpointer *vp)
{
    if (vp->fd < 0) return;
    // Release before teardown. A stuck mouse button is the worst thing this
    // program could possibly leave behind.
    vpointer_button(vp, false);
    ioctl(vp->fd, UI_DEV_DESTROY);
    close(vp->fd);
    vp->fd = -1;
}

// ------------------------------------------------------------------ state ---

enum state {
    ST_IDLE,      // fewer than three fingers, nothing happening
    ST_PENDING,   // three fingers down, waiting to see real movement
    ST_DRAGGING,  // button held, tracking finger movement
    ST_LINGER,    // fingers left the pad; button still held, deciding what next
};

static const char *state_name(enum state s)
{
    switch (s) {
    case ST_IDLE:     return "IDLE";
    case ST_PENDING:  return "PENDING";
    case ST_DRAGGING: return "DRAGGING";
    case ST_LINGER:   return "LINGER";
    }
    return "?";
}

struct slot { bool active; int x, y; };

struct tracker {
    const char *dev_name;   // borrowed from the touchpad, for debug traces
    struct slot slots[MAX_SLOTS];
    int  current_slot;

    enum state state;
    double anchor_x, anchor_y;   // where the three fingers first landed
    double last_x, last_y;       // previous centroid, for deltas
    double acc_x, acc_y;         // sub-pixel remainder

    long drag_started_ms;
    long linger_deadline_ms;
    int  linger_fingers;
};

static int count_fingers(const struct tracker *t, int slot_count)
{
    int n = 0;
    for (int i = 0; i < slot_count; i++)
        if (t->slots[i].active) n++;
    return n;
}

static void centroid(const struct tracker *t, int slot_count, double *cx, double *cy)
{
    double sx = 0, sy = 0;
    int n = 0;
    for (int i = 0; i < slot_count; i++) {
        if (!t->slots[i].active) continue;
        sx += t->slots[i].x;
        sy += t->slots[i].y;
        n++;
    }
    if (n == 0) { *cx = *cy = 0; return; }
    *cx = sx / n;
    *cy = sy / n;
}

static void transition(struct tracker *t, enum state next)
{
    if (t->state == next) return;
    dbg("%s: %s -> %s", t->dev_name ? t->dev_name : "?",
        state_name(t->state), state_name(next));
    t->state = next;
}

static void end_drag(struct tracker *t, struct vpointer *vp)
{
    if (vp->owner == t) {
        vpointer_button(vp, false);
        vp->owner = NULL;
    }
    t->acc_x = t->acc_y = 0;
    transition(t, ST_IDLE);
}

// Called on every SYN_REPORT: the finger picture is now consistent.
static void evaluate(struct tracker *t, struct touchpad *tp, struct vpointer *vp)
{
    int n = count_fingers(t, tp->slot_count);
    long now = now_ms();

    double cx, cy;
    centroid(t, tp->slot_count, &cx, &cy);

    // Safety net: never hold the button indefinitely, whatever the fingers do.
    if (vp->owner == t && vp->button_down && t->drag_started_ms &&
        now - t->drag_started_ms > opt.max_drag_ms) {
        warn("drag exceeded %d ms; releasing the button as a safety measure", opt.max_drag_ms);
        end_drag(t, vp);
        return;
    }

    switch (t->state) {
    case ST_IDLE:
        if (n >= FINGERS_REQUIRED) {
            t->anchor_x = cx;
            t->anchor_y = cy;
            transition(t, ST_PENDING);
        }
        break;

    case ST_PENDING: {
        if (n < FINGERS_REQUIRED) { transition(t, ST_IDLE); break; }

        double dx_mm = (cx - t->anchor_x) / tp->res_x;
        double dy_mm = (cy - t->anchor_y) / tp->res_y;
        if (dx_mm * dx_mm + dy_mm * dy_mm >= opt.threshold_mm * opt.threshold_mm) {
            // Another touchpad is mid-drag; two drags on one pointer would
            // fight over the button. Stay pending until it lets go.
            if (vp->button_down && vp->owner != t) break;
            vp->owner = t;
            t->last_x = cx;
            t->last_y = cy;
            t->acc_x = t->acc_y = 0;
            t->drag_started_ms = now;
            vpointer_button(vp, true);
            transition(t, ST_DRAGGING);
        }
        break;
    }

    case ST_DRAGGING:
        if (n >= FINGERS_REQUIRED) {
            double dx_px = (cx - t->last_x) / tp->res_x * BASE_PX_PER_MM * opt.sensitivity;
            double dy_px = (cy - t->last_y) / tp->res_y * BASE_PX_PER_MM * opt.sensitivity;

            t->acc_x += dx_px;
            t->acc_y += dy_px;

            int out_x = (int)t->acc_x;
            int out_y = (int)t->acc_y;
            t->acc_x -= out_x;
            t->acc_y -= out_y;

            vpointer_move(vp, out_x, out_y);

            t->last_x = cx;
            t->last_y = cy;
        } else {
            t->linger_fingers = n;
            t->linger_deadline_ms = now + (n == 0 ? opt.grace_ms : opt.settle_ms);
            transition(t, ST_LINGER);
        }
        break;

    case ST_LINGER:
        if (n >= FINGERS_REQUIRED) {
            // Fingers came back in time. Re-baseline so the cursor does not jump.
            t->last_x = cx;
            t->last_y = cy;
            transition(t, ST_DRAGGING);
        } else if (n != t->linger_fingers) {
            // The finger count changed while lingering (usually 2 -> 1 -> 0 as
            // the hand lifts off). Re-arm with the deadline for the new count.
            t->linger_fingers = n;
            t->linger_deadline_ms = now + (n == 0 ? opt.grace_ms : opt.settle_ms);
        }
        break;
    }
}

// Called when poll() times out, so deadlines fire even with no new events.
static void check_deadline(struct tracker *t, struct vpointer *vp)
{
    if (t->state != ST_LINGER) return;
    if (now_ms() < t->linger_deadline_ms) return;

    dbg("linger expired with %d finger(s) after %ld ms of drag; ending",
        t->linger_fingers, now_ms() - t->drag_started_ms);
    end_drag(t, vp);
}

static int poll_timeout(const struct tracker *t)
{
    if (t->state != ST_LINGER) return -1;
    long remaining = t->linger_deadline_ms - now_ms();
    return remaining <= 0 ? 0 : (int)remaining;
}

// Re-baselining on a fresh device: forget every finger we thought was down.
static void tracker_reset(struct tracker *t)
{
    memset(t->slots, 0, sizeof t->slots);
    t->current_slot = 0;
    t->state = ST_IDLE;
    t->acc_x = t->acc_y = 0;
}

static void handle_event(struct tracker *t, struct touchpad *tp,
                         struct vpointer *vp, const struct input_event *ev)
{
    switch (ev->type) {
    case EV_ABS:
        switch (ev->code) {
        case ABS_MT_SLOT:
            if (ev->value >= 0 && ev->value < tp->slot_count) t->current_slot = ev->value;
            break;
        case ABS_MT_TRACKING_ID:
            t->slots[t->current_slot].active = (ev->value != -1);
            break;
        case ABS_MT_POSITION_X:
            t->slots[t->current_slot].x = ev->value;
            break;
        case ABS_MT_POSITION_Y:
            t->slots[t->current_slot].y = ev->value;
            break;
        default: break;
        }
        break;

    case EV_SYN:
        if (ev->code == SYN_REPORT) evaluate(t, tp, vp);
        break;

    default: break;
    }
}

// ------------------------------------------------------------- pad roster ---

// Every capable touchpad gets its own tracker; they share the virtual pointer.
struct pad {
    struct touchpad tp;
    struct tracker  tk;
};

static struct pad g_pads[MAX_PADS];   // tp.fd < 0 means the slot is free

static int pads_active(void)
{
    int n = 0;
    for (int i = 0; i < MAX_PADS; i++)
        if (g_pads[i].tp.fd >= 0) n++;
    return n;
}

static void pad_add(const char *path)
{
    for (int i = 0; i < MAX_PADS; i++)
        if (g_pads[i].tp.fd >= 0 && strcmp(g_pads[i].tp.path, path) == 0)
            return;   // already watching it

    struct pad *p = NULL;
    for (int i = 0; i < MAX_PADS; i++)
        if (g_pads[i].tp.fd < 0) { p = &g_pads[i]; break; }
    if (!p) { warn("more than %d touchpads; ignoring %s", MAX_PADS, path); return; }

    if (!touchpad_try_open(&p->tp, path)) return;

    tracker_reset(&p->tk);
    p->tk.dev_name = p->tp.name;
}

static void pad_remove(struct pad *p, struct vpointer *vp)
{
    end_drag(&p->tk, vp);
    dbg("stopped watching %s (%s)", p->tp.path, p->tp.name);
    touchpad_close(&p->tp);
}

static void pads_rescan(void)
{
    if (opt.device) {
        pad_add(opt.device);
        return;
    }

    DIR *dir = opendir("/dev/input");
    if (!dir) { warn("cannot scan /dev/input: %s", strerror(errno)); return; }

    struct dirent *ent;
    while ((ent = readdir(dir))) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;

        char path[320];
        snprintf(path, sizeof path, "/dev/input/%s", ent->d_name);
        pad_add(path);
    }
    closedir(dir);
}

// ------------------------------------------------------------------- main ---

static void usage(FILE *out)
{
    fprintf(out,
        "Usage: " PROG " [options]\n"
        "\n"
        "macOS-style three-finger drag for Linux touchpads.\n"
        "\n"
        "Options:\n"
        "  -d, --device PATH       watch only this event device (default: every capable touchpad)\n"
        "  -s, --sensitivity N     cursor speed multiplier (default: %.2f)\n"
        "  -t, --threshold N       mm of movement before a drag starts (default: %.2f)\n"
        "  -g, --grace-ms N        keep dragging for N ms after all fingers lift (default: %d)\n"
        "      --settle-ms N       keep dragging for N ms after dropping to 1-2 fingers (default: %d)\n"
        "      --max-drag-ms N     force-release the button after N ms (default: %d)\n"
        "  -v, --debug             log state transitions to stderr and stay in the foreground\n"
        "  -h, --help              show this help\n",
        opt.sensitivity, opt.threshold_mm, opt.grace_ms, opt.settle_ms, opt.max_drag_ms);
}

static bool parse_args(int argc, char **argv)
{
    enum { OPT_SETTLE = 1000, OPT_MAXDRAG };

    static const struct option longopts[] = {
        { "device",       required_argument, NULL, 'd' },
        { "sensitivity",  required_argument, NULL, 's' },
        { "threshold",    required_argument, NULL, 't' },
        { "grace-ms",     required_argument, NULL, 'g' },
        { "settle-ms",    required_argument, NULL, OPT_SETTLE },
        { "max-drag-ms",  required_argument, NULL, OPT_MAXDRAG },
        { "debug",        no_argument,       NULL, 'v' },
        { "help",         no_argument,       NULL, 'h' },
        { 0, 0, 0, 0 },
    };

    int c;
    while ((c = getopt_long(argc, argv, "d:s:t:g:vh", longopts, NULL)) != -1) {
        switch (c) {
        case 'd': opt.device = optarg; break;
        case 's': opt.sensitivity = atof(optarg); break;
        case 't': opt.threshold_mm = atof(optarg); break;
        case 'g': opt.grace_ms = atoi(optarg); break;
        case OPT_SETTLE:  opt.settle_ms = atoi(optarg); break;
        case OPT_MAXDRAG: opt.max_drag_ms = atoi(optarg); break;
        case 'v': opt.debug = true; break;
        case 'h': usage(stdout); exit(EXIT_SUCCESS);
        default:  usage(stderr); return false;
        }
    }

    if (opt.sensitivity <= 0)  { warn("sensitivity must be positive"); return false; }
    if (opt.threshold_mm < 0)  { warn("threshold cannot be negative"); return false; }
    if (opt.grace_ms < 0 || opt.settle_ms < 0) { warn("timeouts cannot be negative"); return false; }
    if (opt.max_drag_ms <= 0)  { warn("max drag time must be positive"); return false; }

    return true;
}

static int min_timeout(int a, int b)
{
    if (a < 0) return b;
    if (b < 0) return a;
    return a < b ? a : b;
}

int main(int argc, char **argv)
{
    g_start_ms = now_ms();

    if (!parse_args(argc, argv)) return EXIT_FAILURE;

    install_signal_handlers();

    struct vpointer vp;
    if (!vpointer_create(&vp)) return EXIT_FAILURE;

    for (int i = 0; i < MAX_PADS; i++) g_pads[i].tp.fd = -1;

    // Watch /dev/input so touchpads plugged in later are picked up at once.
    // Without inotify we fall back to a rescan every couple of seconds.
    int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd >= 0 && inotify_add_watch(ifd, "/dev/input",
                                      IN_CREATE | IN_DELETE | IN_ATTRIB) < 0) {
        close(ifd);
        ifd = -1;
    }
    if (ifd < 0) dbg("inotify unavailable (%s); using periodic rescans", strerror(errno));

    pads_rescan();
    bool warned_no_pad = false;
    if (pads_active() == 0) {
        if (opt.device)
            warn("%s is missing or not a multitouch touchpad reporting 3 fingers; waiting for it", opt.device);
        else
            warn("no touchpad reporting three-finger contact was found; waiting for one");
        warned_no_pad = true;
    }

    int exit_code = EXIT_SUCCESS;

    while (!g_stop) {
        struct pollfd pfds[1 + MAX_PADS];
        struct pad   *pmap[1 + MAX_PADS];
        int nfds = 0;

        if (ifd >= 0) {
            pfds[nfds].fd = ifd;
            pfds[nfds].events = POLLIN;
            pmap[nfds] = NULL;
            nfds++;
        }

        int timeout = -1;
        for (int i = 0; i < MAX_PADS; i++) {
            if (g_pads[i].tp.fd < 0) continue;
            pfds[nfds].fd = g_pads[i].tp.fd;
            pfds[nfds].events = POLLIN;
            pmap[nfds] = &g_pads[i];
            nfds++;
            timeout = min_timeout(timeout, poll_timeout(&g_pads[i].tk));
        }

        // No inotify, or nothing to watch: rescan on a timer instead.
        if (ifd < 0 || pads_active() == 0)
            timeout = min_timeout(timeout, 2000);

        int ready = poll(pfds, (nfds_t)nfds, timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            warn("poll failed: %s", strerror(errno));
            exit_code = EXIT_FAILURE;
            break;
        }

        for (int i = 0; i < MAX_PADS; i++)
            if (g_pads[i].tp.fd >= 0) check_deadline(&g_pads[i].tk, &vp);

        bool rescan = false;

        for (int k = 0; k < nfds && !g_stop; k++) {
            if (pfds[k].revents == 0) continue;

            if (pmap[k] == NULL) {   // inotify: something changed under /dev/input
                char buf[4096];
                while (read(ifd, buf, sizeof buf) > 0) {}
                rescan = true;
                continue;
            }

            struct pad *p = pmap[k];

            if (pfds[k].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                dbg("%s went away", p->tp.name);
                pad_remove(p, &vp);
                continue;
            }
            if (!(pfds[k].revents & POLLIN)) continue;

            struct input_event evs[64];
            ssize_t got = read(p->tp.fd, evs, sizeof evs);
            if (got < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                dbg("read from %s failed (%s)", p->tp.name, strerror(errno));
                pad_remove(p, &vp);
                continue;
            }
            if (got == 0 || got % (ssize_t)sizeof(struct input_event) != 0) continue;

            size_t count = (size_t)got / sizeof(struct input_event);
            for (size_t i = 0; i < count && !g_stop; i++)
                handle_event(&p->tk, &p->tp, &vp, &evs[i]);

            check_deadline(&p->tk, &vp);
        }

        if (rescan || (ready == 0 && (ifd < 0 || pads_active() == 0)))
            pads_rescan();

        if (pads_active() > 0) {
            warned_no_pad = false;
        } else if (!warned_no_pad) {
            warn("all touchpads are gone; waiting for one to appear");
            warned_no_pad = true;
        }
    }

    dbg("shutting down");
    for (int i = 0; i < MAX_PADS; i++)
        if (g_pads[i].tp.fd >= 0) pad_remove(&g_pads[i], &vp);
    if (ifd >= 0) close(ifd);
    vpointer_destroy(&vp);
    return exit_code;
}

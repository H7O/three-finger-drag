# three-finger-drag

macOS-style three-finger drag for Linux touchpads, in ~650 lines of C with **no dependencies**.

Put three fingers on the touchpad and move: the cursor drags. Lift them and put them
back within a moment, and the drag carries on. It is the MacBook gesture, on Linux.

```
  /dev/input/eventN            this daemon               /dev/uinput
  (real touchpad)     ──read──▶  state machine  ──write──▶  virtual pointer
   MT slots, TRIPLETAP           centroid delta            REL_X/REL_Y + BTN_LEFT
```

## Do you need this?

**Probably not, if your libinput is 1.28 or newer.** Three-finger drag has been built
into libinput since March 2025 — check with `libinput --version`, and if you have it,
use the native feature instead of this.

This exists for everyone still on an older stack. Linux Mint 22.3 and Ubuntu 24.04 LTS
ship libinput **1.25**, and will for years. Replacing the input stack on a laptop to get
one gesture is a bad trade; this is the alternative.

## What makes it different

Other tools solve this by shelling out to `libinput debug-events` and parsing its
human-readable output, then driving the cursor through xdotool. That works, but it
depends on a debug command's text format staying stable, and it drags in a language
runtime and an X11 automation tool.

This talks to the kernel directly. It needs **libc and the Linux uapi headers** — nothing
else. No Rust toolchain, no xdotool, no libinput-tools, no config file format, no daemon
framework. `gcc three-finger-drag.c -o three-finger-drag` is the whole build.

## The interesting problem

When three fingers are down, libinput classifies the contact as a *swipe gesture* and
emits **no pointer motion at all**. So pressing a button is not enough — nothing would
move. The daemon has to synthesise the cursor movement itself, from the centroid of the
fingers still on the pad, converted through the touchpad's real `units/mm` resolution so
the speed matches your normal pointer.

The other subtlety: when a finger lifts, the centroid jumps. The tracker re-baselines on
every change in finger count, so the cursor never leaps.

The touchpad is opened **read-only and never grabbed** (`EVIOCGRAB` is not used). The real
device keeps behaving exactly as it always did; this daemon is a passive second reader.
If it crashes, you lose the feature and nothing else.

## Behaviour

| Situation | What happens |
|---|---|
| 3 fingers down, movement passes threshold | button down, drag begins |
| dragging, drop to 1–2 fingers | drag ends after `--settle-ms` (default 40 ms) |
| dragging, all fingers lift | button held for `--grace-ms` (default 400 ms) |
| 3 fingers return within the grace window | drag continues, uninterrupted |
| grace expires | button released, drag ends |
| anything held longer than `--max-drag-ms` | force-released as a safety net |

The grace window is what makes it feel like a Mac: you can lift, reposition, and carry on.
Measured on real hands, re-applying three fingers takes about 350 ms — hence the 400 ms default.

**A deliberate deviation from libinput's native behaviour:** libinput keeps dragging when
you drop to two fingers. This daemon ends the drag instead, because it runs *alongside*
libinput rather than inside it, and cannot stop libinput from starting two-finger scrolling
under the drag. Ending cleanly beats fighting the scroll.

## Requirements

- A touchpad reporting `BTN_TOOL_TRIPLETAP` and multitouch slots (any modern one does)
- `uinput` — built into most kernels; `modprobe uinput` if not
- Tested on **X11**. Wayland is untested: compositors may ignore synthetic pointer devices.

## Build and install

```bash
make
sudo make install     # /usr/local/bin + /etc/systemd/system
sudo make enable      # start now and on every boot
```

Try it first without installing anything:

```bash
sudo ./three-finger-drag --debug
```

That runs in the foreground and logs every state transition with timestamps.

## Options

```
-d, --device PATH      touchpad event device (default: autodetect)
-s, --sensitivity N    cursor speed multiplier (default: 1.0)
-t, --threshold N      mm of movement before a drag starts (default: 1.5)
-g, --grace-ms N       keep dragging N ms after all fingers lift (default: 400)
    --settle-ms N      keep dragging N ms after dropping to 1-2 fingers (default: 40)
    --max-drag-ms N    force-release the button after N ms (default: 30000)
-v, --debug            log state transitions and stay in the foreground
```

To tune, edit `ExecStart=` in the unit file and `systemctl daemon-reload`.

## Turning off tap-and-drag

Once this works you probably want the old double-tap-and-drag gone. On Cinnamon:

```bash
gsettings set org.cinnamon.desktop.peripherals.touchpad tap-and-drag false
```

GNOME uses `org.gnome.desktop.peripherals.touchpad`. Elsewhere, `xinput set-prop`
on `libinput Tapping Drag Enabled`.

## Uninstall

```bash
sudo systemctl disable --now three-finger-drag
sudo make uninstall
gsettings set org.cinnamon.desktop.peripherals.touchpad tap-and-drag true
```

Nothing else is touched. No packages, no PPAs, no libraries replaced.

## Security

The service runs as root, because reading `/dev/input/*` and writing `/dev/uinput`
requires it. The systemd unit is written so that root means as little as possible:
empty capability bounding set, `DevicePolicy=closed` with only the two devices it needs,
no network, read-only filesystem, syscall filtering.

It reads **only the touchpad** — the device is selected by requiring three-finger
multitouch capability, and keyboards do not have it. Resident footprint is about 250 KB.

The alternative approach — adding your user to the `input` group — is not recommended:
that grants every process you run the ability to read every input device, keyboard included.

## Scope, honestly

Developed and tested on one machine: an ELAN I²C touchpad under Linux Mint 22.3
(Cinnamon 6.6.9, X11, kernel 7.0). The code targets the generic kernel multitouch
protocol rather than anything vendor-specific, so it should work on any touchpad with
three-finger reporting — but "should" is not "has been". Reports from other hardware,
desktops, and distributions are welcome.

## Design notes

[`docs/PLAN.md`](docs/PLAN.md) records how this approach was chosen — what was measured on
the target machine, why libinput 1.25 ruled out the native feature, why the desktop's own
gesture system could not express a drag, and why the existing third-party tool was passed over.

## License

MIT.

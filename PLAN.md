# Three-Finger Drag for the Touchpad

Goal: drag-and-drop by moving three fingers on the touchpad, the way a MacBook
trackpad does it — then switch off double-tap-and-drag once it works.

---

## 1. What this machine actually is

Measured, not assumed:

| Thing | Value | Why it matters |
|---|---|---|
| OS | Linux Mint 22.3 Zena (Ubuntu 24.04 base) | |
| Desktop | Cinnamon 6.6.9 | |
| Session | **X11** | Synthetic input is straightforward; Wayland would be far harder |
| libinput | **1.25.0** | Native 3-finger drag needs **1.28+** — too old |
| X input driver | xf86-input-libinput 1.4.0 | Would also need ≥1.5 to expose the setting |
| Touchpad | `ELAN2841:00 04F3:31BD`, I²C | |
| Event node | `/dev/input/event5` | Stable link: `/dev/input/by-path/platform-AMDI0010:00-event-mouse` |
| Multitouch | `BTN_TOOL_TRIPLETAP`, `QUADTAP`, `QUINTTAP`, MT-B slots | **Hardware reports 3+ fingers natively** |
| `/dev/uinput` | present, `crw------- root root` | Injection possible; needs root or a udev rule |
| `uinput` driver | **built into the kernel** | No module to load |
| User groups | `d adm cdrom sudo dip video plugdev users lpadmin sambashare render` | **not** in `input` |
| Current setting | `tap-and-drag = true` | This is the double-tap-drag to retire at the end |

## 2. Why the easy answers don't work

**Cinnamon's built-in gestures can't do it.** The `org.cinnamon.gestures` schema
exists but is `enabled=false`, and every binding is *discrete* — fired once at
gesture end (`swipe-left-3 → WORKSPACE_PREVIOUS`, `::end`). Drag is continuous:
button down, track motion, button up. There is no action type for that. Dead end,
regardless of the toggle.

**libinput has the feature natively — but not the version we have.** Three-finger
drag landed in **libinput 1.28.0** (March 2025) and was refined in 1.31.0. Mint
22.3 ships **1.25.0**. Getting it natively means replacing libinput *and* the X
driver system-wide — that is the entire input stack of a laptop, with the failure
mode "no keyboard and no mouse at the login screen." Not worth it for this.

**The known third-party tool is thin.** `marsqing/libinput-three-finger-drag`
(Rust, 133 stars, 24 commits) works by *forking `libinput debug-events` and parsing
its text output*, then driving X11 through xdotool. It needs `libinput-tools` and
`xdotool` installed, and it depends on the stability of a debug command's human-readable
output. That is the fragile, obscure-dependency shape we agreed to avoid.

## 3. The plan: a small C daemon, no dependencies

Everything needed is already in the kernel. The daemon is ~400 lines of C against
`<linux/input.h>` and `<linux/uinput.h>` plus libc. **No libraries, no packages,
nothing to trust but the kernel ABI and our own code.**

### How it works

```
  /dev/input/event5            our daemon              /dev/uinput
  (real touchpad)     ──read──▶  state machine  ──write──▶  virtual mouse
   MT slots, TRIPLETAP           centroid delta            REL_X/REL_Y + BTN_LEFT
                                                                 │
                                                                 ▼
                                                        X11 core pointer
```

We open the touchpad **read-only and non-exclusively** — no `EVIOCGRAB`. The real
touchpad keeps working exactly as it does now; we are a passive second reader.

**The one non-obvious constraint:** when three fingers are down, libinput classifies
it as a swipe gesture and emits **no pointer motion at all**. So pressing a button
isn't enough — the daemon must synthesise the cursor movement itself, from the
average movement of the fingers still on the pad. That is the core of the work.

### State machine (mirrors libinput's documented semantics, which were modelled on macOS)

| Transition | Behaviour |
|---|---|
| 3 fingers down, movement passes threshold | `BTN_LEFT` **down**, drag begins |
| dragging, finger lifts → 2 remain | drag **continues**, motion follows the 2 (no scrolling) |
| dragging, → 1 remains | drag **ends**, `BTN_LEFT` up, normal pointer resumes |
| dragging, all lifted | grace window (~300 ms), see below |
| 3 fingers return within grace | drag **continues** uninterrupted |
| grace expires, or 1–2 fingers return | drag **ends** |

The grace window is what makes it feel like a Mac — you can reposition mid-drag.

### Implementation notes worth getting right

- **Find the device by name, not by number.** Scan `/dev/input/event*`, match
  `EVIOCGNAME` against `Touchpad`. Node numbering is not guaranteed across reboots.
- **Re-baseline the centroid when the finger count changes.** Otherwise lifting one
  of three fingers moves the average and the cursor jumps.
- **Scale using real units.** `EVIOCGABS` gives `resolution` in units/mm; convert to
  a sensible pointer delta and expose one `--sensitivity` flag rather than a magic number.
- **Never leave the button stuck down.** This is the one genuinely nasty failure mode.
  Handlers for `SIGTERM`/`SIGINT`/`SIGHUP` must emit `BTN_LEFT` up before exiting,
  plus a maximum-drag safety timeout that force-releases.
- **`--debug` foreground mode** printing state transitions, for testing before it
  ever runs as a service.

### Files

```
/home/d/code/mouse/
├── PLAN.md                       ← this file
├── three-finger-drag.c
├── Makefile                      ← gcc, -O2 -Wall -Wextra, no -l flags
└── systemd/three-finger-drag.service
```

## 4. Permissions

Reading `/dev/input/event5` and writing `/dev/uinput` both need privilege. Two routes:

- **Add the user to the `input` group.** Simpler — but it grants *every* process
  running as you the ability to read *every* input device, keyboard included. That is
  a keylogging surface opened permanently for a touchpad feature.
- **Run as a root systemd system service.** ✅ **Recommended.** Privilege stays inside
  one small binary instead of widening the whole user account, and systemd can fence it in:

```ini
NoNewPrivileges=true      ProtectSystem=strict     ProtectHome=read-only
PrivateNetwork=true       PrivateTmp=true          RestrictAddressFamilies=AF_UNIX
DeviceAllow=/dev/uinput rw
DeviceAllow=char-input r
```

The daemon reads only the touchpad — never the keyboard.

## 5. Steps

1. Write `three-finger-drag.c` — device discovery, evdev reader, state machine, uinput writer.
2. Write the `Makefile`.
3. Build and run in the foreground with `--debug`. **Nothing installed yet.**
4. Test by hand (§6). Iterate on sensitivity and the grace window until it feels right.
5. Install to `/usr/local/bin/`, add the systemd unit, `enable --now`.
6. Reboot once to confirm it survives and picks the device up cleanly.
7. **Only then**, turn off double-tap-drag:
   `gsettings set org.cinnamon.desktop.peripherals.touchpad tap-and-drag false`

Step 7 is deliberately last — the old method stays available until the new one is proven.

## 6. Verification

Functional:
- Drag a window by its title bar, across the whole screen, in one gesture
- Select a paragraph of text by dragging
- Drag a file between two Nemo windows
- Lift one finger mid-drag → drag survives; lift to one finger → drag ends cleanly
- Lift all three and re-apply quickly → drag resumes

Non-regression — these must be untouched:
- One-finger pointer movement
- Two-finger scrolling (natural scrolling stays on)
- Tap to click
- Three- and four-finger swipes (Cinnamon gestures are off, so these should stay inert)

Safety:
- `systemctl stop three-finger-drag` mid-drag → button must release, not stick
- Kill `-9` the daemon mid-drag → confirm the virtual device disappearing releases the button
- Check `journalctl -u three-finger-drag` is quiet in normal use

## 7. Rollback

```bash
sudo systemctl disable --now three-finger-drag
sudo rm /usr/local/bin/three-finger-drag /etc/systemd/system/three-finger-drag.service
gsettings set org.cinnamon.desktop.peripherals.touchpad tap-and-drag true
```

Nothing outside those paths is modified. No system package is touched, no PPA added,
no library replaced — so there is nothing that can break the input stack. Worst case
the daemon doesn't run and the touchpad behaves exactly as it does today.

## 8. Known risks

| Risk | Mitigation |
|---|---|
| Stuck mouse button | Signal handlers + max-drag timeout; test explicitly (§6) |
| Cursor jump when finger count changes | Re-baseline centroid on every count change |
| Drag feels too fast/slow | `--sensitivity` flag, tuned in §5 step 4 before install |
| Kernel names the device differently after an update | Match by name at startup, not by node number |
| Future Mint gets libinput ≥1.28 natively | Then delete all of this and tick a checkbox — rollback in §7 |

## 9. References

- [Three-finger drag — libinput 1.31.0 documentation](https://wayland.freedesktop.org/libinput/doc/latest/drag-3fg.html)
- [Libinput 1.28 Released With Three-Finger Drag — Phoronix](https://www.phoronix.com/news/Libinput-1.28-Released)
- [Enable 3/4 Finger Dragging via Libinput — UbuntuHandbook](https://ubuntuhandbook.org/index.php/2025/03/enable-3-finger-dragging-ubuntu/)
- [marsqing/libinput-three-finger-drag](https://github.com/marsqing/libinput-three-finger-drag) — the third-party option, rejected above
- [libinput — ArchWiki](https://wiki.archlinux.org/title/Libinput)

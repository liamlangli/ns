# Profiling and the live profiler

`ns` collects two kinds of profile from the same recorder:

- a **whole-run report** written to `bin/ns.profile` at exit
  (`ns profile [path]`, `ns build --profile`, `--profile` on any command), and
- a **live stream** of one message per frame, published to the GUI viewer over
  loopback TCP while the program runs (`ns profiler --live [path | target]`).

The recorder is `src/ns_profile.c`; the live transport is
`src/ns_profile_live.c`; the viewer is the ns application in `nscode/profile`
(`live.ns` owns the session and the ring, `main.ns` draws it).

## Whole-run reports

`ns profile hot.ns` runs the program with collection enabled and, at exit,
writes `bin/ns.profile` (text aggregates plus a folded flamechart) and a compact
timeline blob beside it (`ns.profile.tl`, or `.tl.zst` when `compress` is
available). It then prints a colored hot-path summary and opens the viewer.

`ns profiler [file]` opens the viewer on an existing report without running
anything.

## Live capture

```bash
ns profiler --live
```

From a project directory this opens the viewer, which binds a loopback port,
starts `ns profile --live-port <n>` in the project, and renders frames as they
arrive. `ns profiler --live <path|target>` selects a directory, a file, or a
`[[targets]]` name. Run, Restart, Stop, Pause/Resume, Replay, the follow
(`Live`) toggle and the `Frame` / `Window` scope toggle all live in the viewer
window; the CLI hands over `NS_PROFILE_LIVE=1`, `NS_PROFILE_PROJECT`,
`NS_PROFILE_ENTRY` and `NS_PROFILE_NS` and then exits.

A target can also be pointed at a viewer by hand:

```bash
ns profile --live-port 9613 --live-host 127.0.0.1 src/main.ns
```

A run whose port has no listener warns once and stays an ordinary local
profile. A live run still writes `bin/ns.profile` when it ends, so a session
leaves the usual report behind.

### Frames

A frame ends when the **outermost native callback returns**: `view`'s
`on_frame`, ui event callbacks, and anything else that re-enters the VM through
the callback bridge in `src/ns_vm_lib.c`. Nested callbacks belong to the frame
that contains them. A program with no callbacks at all (a plain script) reaches
a boundary only through the per-frame event cap, and publishes what it has when
it exits.

### The 128-frame ring

Live capture cannot keep a whole-run timeline: a program under the profiler may
run for hours. Both ends therefore keep a ring of the last
`NS_PROFILE_LIVE_FRAMES` (128) frames.

In the runtime, retained samples move out of the linear timeline array into
128 frame slots (`ns_profile.frames`). Closing a frame publishes it, advances
the head, and recycles the slot that ages out - reusing its array, so
steady-state capture stops allocating. Two limits bound it further: a single
frame is cut at `NS_PROFILE_LIVE_FRAME_EVENTS` samples, and the window retires
frames early when it holds more than `NS_PROFILE_LIVE_RING_EVENTS` samples in
total (`frames_retired` counts those). Aggregates - the per-symbol table and the
flame tree - stay cumulative for the whole run, so the summary and the report
still describe everything that happened; only retained *samples* are a moving
window, and the exit report covers the window the ring still holds.

The viewer keeps its own 128-slot ring of the encoded frame payloads in one
byte ring, and decodes only what is on screen. The frame strip draws every
retained frame as a bar against a 16.7 ms line: click one to pin it, `Live` to
follow the newest again, and `Window` to merge every retained frame into a
single timeline and flamegraph.

## Wire format

Little-endian throughout. The viewer listens; the target connects, so the
target is always the client. `include/ns_profile_live.h` is the reference.

```text
stream  := u32 magic 'NSLP' , message*
message := u32 type , u32 payload_len , payload
```

Target to viewer:

| Type | Name | Payload |
| --- | --- | --- |
| 1 | `HELLO` | `u32 version, u32 pid, u32 frame_capacity, u16 title_len, title` |
| 2 | `SYMBOLS` | `u32 first_id, u32 count`, then `u8 kind, u16 lib_len, u16 name_len, lib, name` per symbol |
| 3 | `THREADS` | `u32 first_id, u32 count`, then `u16 len, name` per lane |
| 4 | `FRAME` | `u32 index, i32 start_us, i32 end_us, u32 event_count`, then events |
| 5 | `BYE` | `u32 reason` (0 normal exit, 1 stopped by the viewer) |

Viewer to target: `20 PAUSE`, `21 RESUME`, `22 QUIT`, `23 REPLAY`, each an
8-byte header with no payload. `PAUSE` stops publishing while capture keeps
filling the ring; `REPLAY` resends every frame the ring still holds, oldest
first; `QUIT` makes the target exit, writing its report on the way out.

Symbol and lane tables are incremental: ids are dense and stable for a session,
and only entries the viewer has not seen are sent before the frame that uses
them. An event is the same 20-byte record the `.tl` timeline blob uses, so one
reader decodes both a file and a live frame:

```text
u8 kind, u8 depth, u16 thread, u16 sym, u16 pad,
i32 start_us, i32 elapsed_us, i32 self_us
```

Frames are sorted and coalesced before they are sent, so the viewer draws them
without further work.

### Backpressure

The target's socket is nonblocking and its writer queues into memory.
Profiling must never stall the program it measures, so a viewer that falls
behind by more than `NS_PROFILE_LIVE_OUT_MAX` (8 MB) loses whole frames rather
than delaying the target; the gap shows up as a jump in the frame index, and
the target warns about dropped batches when it exits.

## Tests

- `test/ns_profile_test.c` - recorder aggregates, timeline retention, and the
  frame ring (window size, retirement, flatten-for-report).
- `test/ns_profile_test.sh` - CLI behaviour, report contents, and an end-to-end
  live session against a stand-in viewer written in ns.
- `nscode/profile/test/live_test.ns` - the viewer's stream decoding, symbol and
  lane tables, ring retention, batched frames, and partial messages.

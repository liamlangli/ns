#pragma once

#include "ns_profile.h"

// Live profile streaming: `ns profile --live-port <n>` connects the profiled
// process to the `ns profiler` viewer over loopback TCP and publishes one
// message per frame instead of a single report at exit.
//
// The viewer listens and launches the target, so the target is always the
// connecting side. Capture switches to the 128-frame ring (see ns_profile.h),
// which bounds memory for a program that runs for hours and lets a late or
// reconnecting viewer ask for the whole retained window with REPLAY.
//
// Wire format, little-endian throughout:
//
//   stream  := u32 magic 'NSLP' , message*
//   message := u32 type , u32 payload_len , payload
//
// target -> viewer
//   1 HELLO   u32 version, u32 pid, u32 frame_capacity, u16 title_len, title
//   2 SYMBOLS u32 first_id, u32 count, [u8 kind, u16 lib_len, u16 name_len,
//             lib, name]*  - incremental; ids index the viewer symbol table
//   3 THREADS u32 first_id, u32 count, [u16 len, name]*  - incremental lanes
//   4 FRAME   u32 index, i32 start_us, i32 end_us, u32 event_count, event*
//   5 BYE     u32 reason (0 normal exit, 1 stopped by the viewer)
//
// viewer -> target
//   20 PAUSE  stop publishing frames; capture keeps filling the ring
//   21 RESUME publish again, starting with the next closed frame
//   22 QUIT   exit the profiled process (writes its report first)
//   23 REPLAY resend every frame the ring still holds, oldest first
//
// An event is the 20-byte record the `.tl` timeline blob uses, so the viewer
// decodes a file and a live frame with the same reader.

#define NS_PROFILE_LIVE_MAGIC 0x504C534Eu /* 'NSLP' */
#define NS_PROFILE_LIVE_VERSION 1

#define NS_PROFILE_LIVE_MSG_HELLO 1
#define NS_PROFILE_LIVE_MSG_SYMBOLS 2
#define NS_PROFILE_LIVE_MSG_THREADS 3
#define NS_PROFILE_LIVE_MSG_FRAME 4
#define NS_PROFILE_LIVE_MSG_BYE 5

#define NS_PROFILE_LIVE_CMD_PAUSE 20
#define NS_PROFILE_LIVE_CMD_RESUME 21
#define NS_PROFILE_LIVE_CMD_QUIT 22
#define NS_PROFILE_LIVE_CMD_REPLAY 23

#define NS_PROFILE_LIVE_BYE_EXIT 0
#define NS_PROFILE_LIVE_BYE_STOPPED 1

// Bytes allowed to queue for a slow viewer before pending frames are dropped.
// Profiling must not stall the program it measures.
#define NS_PROFILE_LIVE_OUT_MAX (8 * 1024 * 1024)

// Connect to `host`:`port` (`host` may be null for 127.0.0.1), send HELLO with
// `title`, enable the frame ring, and install the per-frame streamer. Returns
// false when the viewer cannot be reached; capture then stays local.
ns_bool ns_profile_live_connect(const char *host, i32 port, const char *title);

// True once connect succeeded and the socket is still usable.
ns_bool ns_profile_live_active(void);

// Flush what is queued, send BYE, and close. Safe to call when not connected.
void ns_profile_live_close(u32 reason);

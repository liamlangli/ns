# Hand skeleton input

`use view` exposes application-independent hand tracking. Generated visionOS
apps include the ARKit backend and a generic hand-tracking permission purpose.
Desktop native modules and the browser runtime expose the same ABI and return
`VIEW_HANDS_UNSUPPORTED`; they never synthesize real hand input.

Call `view_hands_start()` after entering an immersive space. It schedules
permission and provider startup on the main thread and returns a status:
`VIEW_HANDS_STOPPED` (0), `VIEW_HANDS_STARTING` (1), `VIEW_HANDS_RUNNING` (2),
`VIEW_HANDS_FAILED` (-1, denied or provider failure), or
`VIEW_HANDS_UNSUPPORTED` (-2). Calling start while starting/running is idempotent.
To retry after a failure, call stop and then start. Applications own the retry
UI and decide what lost tracking means for their interaction.

Call `view_hands_snapshot()` once per application frame before reading either
hand. It returns the provider status and freezes both hands for subsequent
reads. `view_hand_tracked(hand)` reports anchor validity, while
`view_hand_joint_tracked(hand, joint)` reports each joint's validity. Samples
older than 0.20 seconds, removed anchors, and non-running providers are untracked.
`view_hands_stop()` invalidates the readings immediately and schedules session
shutdown. Generation tokens prevent delayed callbacks from reviving a stopped
or replaced session. All samples remain in memory.

`VIEW_HAND_LEFT` and `VIEW_HAND_RIGHT` identify the hands. The 27
`VIEW_HAND_JOINT_*` constants cover the wrist, four thumb joints, five joints
per other finger, and two forearm joints; `VIEW_HAND_JOINT_COUNT` is 27.
Read scalar positions with
`view_hand_joint_position(hand, joint, VIEW_HAND_AXIS_X/Y/Z)`. Positions use
metres in the immersive world's right-handed origin: +X right, +Y up, +Z back.
Invalid indices and untracked joints return zero; always check validity rather
than interpreting zero as a measured position.

```ns
use view

fn read_index_tip() {
    let status = view_hands_snapshot()
    if status != VIEW_HANDS_RUNNING { return }
    if view_hand_joint_tracked(VIEW_HAND_LEFT, VIEW_HAND_JOINT_INDEX_FINGER_TIP) {
        let x = view_hand_joint_position(VIEW_HAND_LEFT, VIEW_HAND_JOINT_INDEX_FINGER_TIP, VIEW_HAND_AXIS_X)
        // Use the measured position in application logic.
    }
}
```

The native backend publishes measurements only. Gesture names, finger openness
thresholds, palm direction conventions, debounce, simulation and game actions
belong in the application's Nano Script code. `ns project` and `make install`
package the backend automatically; no application-local native adapter is needed.

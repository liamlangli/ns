// GPU device management.
//
// The Metal backend is opt-in on macOS while it still depends on external
// mapper headers. Other platforms will use a Vulkan backend later; until then
// this keeps the statically linked standard library resolving gpu_request_device.
#if !defined(NS_DARWIN) || !NS_ENABLE_METAL

#include "view.h"

ns_bool gpu_request_device(view *v) {
    ns_unused(v);
    // No GPU backend wired up on this platform yet.
    return false;
}

#endif

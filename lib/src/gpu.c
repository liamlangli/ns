// GPU device management.
//
// macOS uses the Metal backend implemented in gpu.metal.m. Other platforms
// (Linux/Windows) will use a Vulkan backend; until that lands this provides a
// linkable stub so the statically linked standard library always resolves
// gpu_request_device.
#ifndef NS_DARWIN

#include "view.h"

ns_bool gpu_request_device(view *v) {
    ns_unused(v);
    // No GPU backend wired up on this platform yet.
    return false;
}

#endif // NS_DARWIN

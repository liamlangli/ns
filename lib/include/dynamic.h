#ifndef NS_DYNAMIC_H
#define NS_DYNAMIC_H

#include <stdint.h>

// Native Box3D adapter used by lib/dynamic.ns. The public Nano Script module
// keeps its data-oriented structs and state-image queries; this interface is
// intentionally scalar/array-only so it stays inside Nano Script's FFI ABI.

uint64_t dynamic_native_world_create(int capacity, float gravity_x, float gravity_y, float gravity_z);
void dynamic_native_world_release(uint64_t handle);
void dynamic_native_world_clear(uint64_t handle);

int dynamic_native_body_add(uint64_t handle, int index,
                            float position_x, float position_y, float position_z,
                            float orientation_x, float orientation_y, float orientation_z, float orientation_w,
                            float velocity_x, float velocity_y, float velocity_z,
                            float angular_x, float angular_y, float angular_z,
                            float inverse_mass, float inverse_inertia_x, float inverse_inertia_y, float inverse_inertia_z,
                            float margin, float restitution, float friction,
                            float damping, float angular_damping);

void dynamic_native_body_set(uint64_t handle, int index,
                             float position_x, float position_y, float position_z,
                             float orientation_x, float orientation_y, float orientation_z, float orientation_w,
                             float velocity_x, float velocity_y, float velocity_z,
                             float angular_x, float angular_y, float angular_z,
                             float inverse_mass, float inverse_inertia_x, float inverse_inertia_y, float inverse_inertia_z,
                             float margin, float restitution, float friction,
                             float damping, float angular_damping);

int dynamic_native_body_set_hull(uint64_t handle, int index, const float *vertices, int count);

void dynamic_native_world_step(uint64_t handle, float time_step, int sub_step_count,
                               float gravity_x, float gravity_y, float gravity_z,
                               int bounds_enabled,
                               float bounds_min_x, float bounds_min_y, float bounds_min_z,
                               float bounds_max_x, float bounds_max_y, float bounds_max_z,
                               float bounds_restitution);

void dynamic_native_world_sync(uint64_t handle, float *state, int capacity, int row_count, int count);

#endif

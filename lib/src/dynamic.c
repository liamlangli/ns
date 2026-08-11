#include "dynamic.h"

#include <box3d/box3d.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    NS_DYNAMIC_MAX_CONTACTS = 4,
    NS_DYNAMIC_ROW_POSITION = 1,
    NS_DYNAMIC_ROW_ORIENTATION = 2,
    NS_DYNAMIC_ROW_VELOCITY = 3,
    NS_DYNAMIC_ROW_ANGULAR = 4,
    NS_DYNAMIC_ROW_MATERIAL = 5,
    NS_DYNAMIC_ROW_INERTIA = 6,
    NS_DYNAMIC_ROW_PAIR = 7,
    NS_DYNAMIC_ROW_CONTACT = 11,
    NS_DYNAMIC_ROW_POINT = 15,
};

typedef struct dynamic_native_body {
    b3BodyId body_id;
    b3ShapeId shape_id;
    float inverse_mass;
    b3Vec3 inverse_inertia;
    float margin;
    float restitution;
    float friction;
    float damping;
    float angular_damping;
} dynamic_native_body;

typedef struct dynamic_native_world {
    b3WorldId world_id;
    int capacity;
    int count;
    dynamic_native_body *bodies;
    b3BodyId bounds[6];
    bool bounds_enabled;
    b3Vec3 bounds_min;
    b3Vec3 bounds_max;
    float bounds_restitution;
} dynamic_native_world;

static dynamic_native_world *dynamic_world_from_handle(uint64_t handle) {
    return (dynamic_native_world *)(uintptr_t)handle;
}

static float dynamic_multiply_restitution(float a, uint64_t material_a, float b, uint64_t material_b) {
    (void)material_a;
    (void)material_b;
    return a * b;
}

static b3Quat dynamic_quat(float x, float y, float z, float w) {
    float length_squared = x * x + y * y + z * z + w * w;
    if (length_squared <= 1.0e-12f) {
        return b3Quat_identity;
    }
    float inverse_length = 1.0f / sqrtf(length_squared);
    return (b3Quat){{x * inverse_length, y * inverse_length, z * inverse_length}, w * inverse_length};
}

static b3Matrix3 dynamic_inertia(float inverse_x, float inverse_y, float inverse_z) {
    float x = inverse_x > 0.0f ? 1.0f / inverse_x : 0.0f;
    float y = inverse_y > 0.0f ? 1.0f / inverse_y : 0.0f;
    float z = inverse_z > 0.0f ? 1.0f / inverse_z : 0.0f;
    return (b3Matrix3){{x, 0.0f, 0.0f}, {0.0f, y, 0.0f}, {0.0f, 0.0f, z}};
}

static void dynamic_apply_mass(dynamic_native_body *body) {
    if (body->inverse_mass <= 0.0f) {
        b3Body_SetType(body->body_id, b3_staticBody);
        return;
    }

    b3Body_SetType(body->body_id, b3_dynamicBody);
    b3MassData mass_data = {
        .mass = 1.0f / body->inverse_mass,
        .center = b3Vec3_zero,
        .inertia = dynamic_inertia(body->inverse_inertia.x, body->inverse_inertia.y, body->inverse_inertia.z),
    };
    b3Body_SetMassData(body->body_id, mass_data);
}

static b3ShapeDef dynamic_shape_def(const dynamic_native_body *body) {
    b3ShapeDef def = b3DefaultShapeDef();
    def.density = 1.0f;
    def.updateBodyMass = false;
    def.baseMaterial.friction = body->friction;
    def.baseMaterial.restitution = body->restitution;
    return def;
}

static void dynamic_destroy_bounds(dynamic_native_world *world) {
    for (int i = 0; i < 6; ++i) {
        if (B3_IS_NON_NULL(world->bounds[i]) && b3Body_IsValid(world->bounds[i])) {
            b3DestroyBody(world->bounds[i]);
        }
        world->bounds[i] = b3_nullBodyId;
    }
}

static void dynamic_create_bound(dynamic_native_world *world, int index, b3Vec3 center, b3Vec3 half_extents) {
    b3BodyDef body_def = b3DefaultBodyDef();
    body_def.position = center;
    world->bounds[index] = b3CreateBody(world->world_id, &body_def);

    b3BoxHull box = b3MakeBoxHull(half_extents.x, half_extents.y, half_extents.z);
    b3ShapeDef shape_def = b3DefaultShapeDef();
    shape_def.baseMaterial.friction = 0.5f;
    shape_def.baseMaterial.restitution = world->bounds_restitution;
    b3CreateHullShape(world->bounds[index], &shape_def, &box.base);
}

static void dynamic_update_bounds(dynamic_native_world *world, bool enabled, b3Vec3 min, b3Vec3 max,
                                  float restitution) {
    bool unchanged = world->bounds_enabled == enabled &&
                     world->bounds_min.x == min.x && world->bounds_min.y == min.y && world->bounds_min.z == min.z &&
                     world->bounds_max.x == max.x && world->bounds_max.y == max.y && world->bounds_max.z == max.z &&
                     world->bounds_restitution == restitution;
    if (unchanged) {
        return;
    }

    dynamic_destroy_bounds(world);
    world->bounds_enabled = enabled;
    world->bounds_min = min;
    world->bounds_max = max;
    world->bounds_restitution = restitution;
    if (!enabled) {
        return;
    }

    b3Vec3 extent = {max.x - min.x, max.y - min.y, max.z - min.z};
    if (extent.x <= 0.0f || extent.y <= 0.0f || extent.z <= 0.0f) {
        world->bounds_enabled = false;
        return;
    }
    float thickness = fmaxf(1.0f, fmaxf(extent.x, fmaxf(extent.y, extent.z)));
    b3Vec3 middle = {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f, (min.z + max.z) * 0.5f};
    b3Vec3 span = {extent.x * 0.5f + thickness, extent.y * 0.5f + thickness, extent.z * 0.5f + thickness};

    dynamic_create_bound(world, 0, (b3Vec3){min.x - thickness, middle.y, middle.z},
                         (b3Vec3){thickness, span.y, span.z});
    dynamic_create_bound(world, 1, (b3Vec3){max.x + thickness, middle.y, middle.z},
                         (b3Vec3){thickness, span.y, span.z});
    dynamic_create_bound(world, 2, (b3Vec3){middle.x, min.y - thickness, middle.z},
                         (b3Vec3){span.x, thickness, span.z});
    dynamic_create_bound(world, 3, (b3Vec3){middle.x, max.y + thickness, middle.z},
                         (b3Vec3){span.x, thickness, span.z});
    dynamic_create_bound(world, 4, (b3Vec3){middle.x, middle.y, min.z - thickness},
                         (b3Vec3){span.x, span.y, thickness});
    dynamic_create_bound(world, 5, (b3Vec3){middle.x, middle.y, max.z + thickness},
                         (b3Vec3){span.x, span.y, thickness});
}

uint64_t dynamic_native_world_create(int capacity, float gravity_x, float gravity_y, float gravity_z) {
    if (capacity < 1) {
        capacity = 1;
    }
    dynamic_native_world *world = calloc(1, sizeof(*world));
    if (world == NULL) {
        return 0;
    }
    world->bodies = calloc((size_t)capacity, sizeof(*world->bodies));
    if (world->bodies == NULL) {
        free(world);
        return 0;
    }
    world->capacity = capacity;

    b3WorldDef def = b3DefaultWorldDef();
    def.gravity = (b3Vec3){gravity_x, gravity_y, gravity_z};
    def.capacity.dynamicBodyCount = capacity;
    def.capacity.dynamicShapeCount = capacity;
    def.restitutionCallback = dynamic_multiply_restitution;
    world->world_id = b3CreateWorld(&def);
    if (B3_IS_NULL(world->world_id)) {
        free(world->bodies);
        free(world);
        return 0;
    }
    return (uint64_t)(uintptr_t)world;
}

void dynamic_native_world_release(uint64_t handle) {
    dynamic_native_world *world = dynamic_world_from_handle(handle);
    if (world == NULL) {
        return;
    }
    if (B3_IS_NON_NULL(world->world_id) && b3World_IsValid(world->world_id)) {
        b3DestroyWorld(world->world_id);
    }
    free(world->bodies);
    free(world);
}

void dynamic_native_world_clear(uint64_t handle) {
    dynamic_native_world *world = dynamic_world_from_handle(handle);
    if (world == NULL) {
        return;
    }
    for (int i = 0; i < world->count; ++i) {
        if (B3_IS_NON_NULL(world->bodies[i].body_id) && b3Body_IsValid(world->bodies[i].body_id)) {
            b3DestroyBody(world->bodies[i].body_id);
        }
    }
    memset(world->bodies, 0, (size_t)world->capacity * sizeof(*world->bodies));
    world->count = 0;
}

static void dynamic_body_set_values(dynamic_native_body *body,
                                    float position_x, float position_y, float position_z,
                                    float orientation_x, float orientation_y, float orientation_z, float orientation_w,
                                    float velocity_x, float velocity_y, float velocity_z,
                                    float angular_x, float angular_y, float angular_z,
                                    float inverse_mass, float inverse_inertia_x, float inverse_inertia_y, float inverse_inertia_z,
                                    float margin, float restitution, float friction,
                                    float damping, float angular_damping) {
    body->inverse_mass = inverse_mass;
    body->inverse_inertia = (b3Vec3){inverse_inertia_x, inverse_inertia_y, inverse_inertia_z};
    body->margin = margin;
    body->restitution = restitution;
    body->friction = friction;
    body->damping = damping;
    body->angular_damping = angular_damping;

    b3Body_SetTransform(body->body_id, (b3Pos){position_x, position_y, position_z},
                        dynamic_quat(orientation_x, orientation_y, orientation_z, orientation_w));
    b3Body_SetLinearVelocity(body->body_id, (b3Vec3){velocity_x, velocity_y, velocity_z});
    b3Body_SetAngularVelocity(body->body_id, (b3Vec3){angular_x, angular_y, angular_z});
    b3Body_SetLinearDamping(body->body_id, damping);
    b3Body_SetAngularDamping(body->body_id, angular_damping);
    dynamic_apply_mass(body);

    if (B3_IS_NON_NULL(body->shape_id) && b3Shape_IsValid(body->shape_id)) {
        b3Shape_SetFriction(body->shape_id, friction);
        b3Shape_SetRestitution(body->shape_id, restitution);
    }
}

int dynamic_native_body_add(uint64_t handle, int index,
                            float position_x, float position_y, float position_z,
                            float orientation_x, float orientation_y, float orientation_z, float orientation_w,
                            float velocity_x, float velocity_y, float velocity_z,
                            float angular_x, float angular_y, float angular_z,
                            float inverse_mass, float inverse_inertia_x, float inverse_inertia_y, float inverse_inertia_z,
                            float margin, float restitution, float friction,
                            float damping, float angular_damping) {
    dynamic_native_world *world = dynamic_world_from_handle(handle);
    if (world == NULL || index < 0 || index >= world->capacity || index != world->count) {
        return 0;
    }

    b3BodyDef def = b3DefaultBodyDef();
    def.type = inverse_mass > 0.0f ? b3_dynamicBody : b3_staticBody;
    def.position = (b3Pos){position_x, position_y, position_z};
    def.rotation = dynamic_quat(orientation_x, orientation_y, orientation_z, orientation_w);
    def.linearVelocity = (b3Vec3){velocity_x, velocity_y, velocity_z};
    def.angularVelocity = (b3Vec3){angular_x, angular_y, angular_z};
    def.linearDamping = damping;
    def.angularDamping = angular_damping;
    def.userData = (void *)(intptr_t)(index + 1);
    def.enableSleep = false;

    dynamic_native_body *body = &world->bodies[index];
    body->body_id = b3CreateBody(world->world_id, &def);
    if (B3_IS_NULL(body->body_id)) {
        return 0;
    }
    body->shape_id = b3_nullShapeId;
    body->inverse_mass = inverse_mass;
    body->inverse_inertia = (b3Vec3){inverse_inertia_x, inverse_inertia_y, inverse_inertia_z};
    body->margin = margin;
    body->restitution = restitution;
    body->friction = friction;
    body->damping = damping;
    body->angular_damping = angular_damping;
    dynamic_apply_mass(body);
    world->count += 1;

    if (margin > 0.0f) {
        b3Sphere sphere = {.center = b3Vec3_zero, .radius = margin};
        b3ShapeDef shape_def = dynamic_shape_def(body);
        body->shape_id = b3CreateSphereShape(body->body_id, &shape_def, &sphere);
        dynamic_apply_mass(body);
    }
    return 1;
}

void dynamic_native_body_set(uint64_t handle, int index,
                             float position_x, float position_y, float position_z,
                             float orientation_x, float orientation_y, float orientation_z, float orientation_w,
                             float velocity_x, float velocity_y, float velocity_z,
                             float angular_x, float angular_y, float angular_z,
                             float inverse_mass, float inverse_inertia_x, float inverse_inertia_y, float inverse_inertia_z,
                             float margin, float restitution, float friction,
                             float damping, float angular_damping) {
    dynamic_native_world *world = dynamic_world_from_handle(handle);
    if (world == NULL || index < 0 || index >= world->count) {
        return;
    }
    dynamic_body_set_values(&world->bodies[index], position_x, position_y, position_z,
                            orientation_x, orientation_y, orientation_z, orientation_w,
                            velocity_x, velocity_y, velocity_z, angular_x, angular_y, angular_z,
                            inverse_mass, inverse_inertia_x, inverse_inertia_y, inverse_inertia_z,
                            margin, restitution, friction, damping, angular_damping);
}

int dynamic_native_body_set_hull(uint64_t handle, int index, const float *vertices, int count) {
    dynamic_native_world *world = dynamic_world_from_handle(handle);
    if (world == NULL || vertices == NULL || index < 0 || index >= world->count || count < 1) {
        return 0;
    }
    dynamic_native_body *body = &world->bodies[index];
    if (B3_IS_NON_NULL(body->shape_id) && b3Shape_IsValid(body->shape_id)) {
        b3DestroyShape(body->shape_id, false);
        body->shape_id = b3_nullShapeId;
    }

    b3ShapeDef def = dynamic_shape_def(body);
    if (count == 1 && body->margin > 0.0f) {
        b3Sphere sphere = {
            .center = {vertices[0], vertices[1], vertices[2]},
            .radius = body->margin,
        };
        body->shape_id = b3CreateSphereShape(body->body_id, &def, &sphere);
    } else if (count == 2 && body->margin > 0.0f) {
        b3Capsule capsule = {
            .center1 = {vertices[0], vertices[1], vertices[2]},
            .center2 = {vertices[3], vertices[4], vertices[5]},
            .radius = body->margin,
        };
        body->shape_id = b3CreateCapsuleShape(body->body_id, &def, &capsule);
    } else if (count >= 4) {
        b3Vec3 points[64];
        if (count > 64) {
            count = 64;
        }
        for (int i = 0; i < count; ++i) {
            points[i] = (b3Vec3){vertices[i * 3], vertices[i * 3 + 1], vertices[i * 3 + 2]};
        }
        b3HullData *hull = b3CreateHull(points, count, count);
        if (hull != NULL) {
            body->shape_id = b3CreateHullShape(body->body_id, &def, hull);
            b3DestroyHull(hull);
        }
    }

    dynamic_apply_mass(body);
    return B3_IS_NON_NULL(body->shape_id) ? 1 : 0;
}

void dynamic_native_world_step(uint64_t handle, float time_step, int sub_step_count,
                               float gravity_x, float gravity_y, float gravity_z,
                               int bounds_enabled,
                               float bounds_min_x, float bounds_min_y, float bounds_min_z,
                               float bounds_max_x, float bounds_max_y, float bounds_max_z,
                               float bounds_restitution) {
    dynamic_native_world *world = dynamic_world_from_handle(handle);
    if (world == NULL || time_step <= 0.0f) {
        return;
    }
    if (sub_step_count < 1) {
        sub_step_count = 1;
    }
    b3World_SetGravity(world->world_id, (b3Vec3){gravity_x, gravity_y, gravity_z});
    dynamic_update_bounds(world, bounds_enabled != 0,
                          (b3Vec3){bounds_min_x, bounds_min_y, bounds_min_z},
                          (b3Vec3){bounds_max_x, bounds_max_y, bounds_max_z},
                          bounds_restitution);
    b3World_Step(world->world_id, time_step, sub_step_count);
}

static int dynamic_state_index(int capacity, int column, int row) {
    return (row * capacity + column) * 4;
}

static void dynamic_state_write(float *state, int capacity, int column, int row,
                                float x, float y, float z, float w) {
    int base = dynamic_state_index(capacity, column, row);
    state[base] = x;
    state[base + 1] = y;
    state[base + 2] = z;
    state[base + 3] = w;
}

static bool dynamic_same_body(b3BodyId a, b3BodyId b) {
    return B3_ID_EQUALS(a, b);
}

static int dynamic_body_index(b3BodyId body_id) {
    return (int)(intptr_t)b3Body_GetUserData(body_id) - 1;
}

static void dynamic_sync_contacts(dynamic_native_world *world, float *state, int capacity, int index) {
    dynamic_native_body *body = &world->bodies[index];
    b3ContactData contacts[NS_DYNAMIC_MAX_CONTACTS];
    int contact_count = b3Body_GetContactData(body->body_id, contacts, NS_DYNAMIC_MAX_CONTACTS);
    int slot = 0;
    for (int i = 0; i < contact_count && slot < NS_DYNAMIC_MAX_CONTACTS; ++i) {
        b3BodyId body_a = b3Shape_GetBody(contacts[i].shapeIdA);
        b3BodyId body_b = b3Shape_GetBody(contacts[i].shapeIdB);
        bool is_a = dynamic_same_body(body->body_id, body_a);
        b3BodyId other = is_a ? body_b : body_a;
        int partner = dynamic_body_index(other);
        if (partner < 0 || partner >= world->count) {
            continue;
        }

        for (int manifold_index = 0;
             manifold_index < contacts[i].manifoldCount && slot < NS_DYNAMIC_MAX_CONTACTS;
             ++manifold_index) {
            const b3Manifold *manifold = &contacts[i].manifolds[manifold_index];
            if (manifold->pointCount == 0) {
                continue;
            }
            const b3ManifoldPoint *point = &manifold->points[0];
            b3Vec3 normal = manifold->normal;
            if (is_a) {
                normal = (b3Vec3){-normal.x, -normal.y, -normal.z};
            }
            b3Pos center = is_a ? b3Body_GetWorldCenterOfMass(body_a) : b3Body_GetWorldCenterOfMass(body_b);
            b3Vec3 anchor = is_a ? point->anchorA : point->anchorB;
            float depth = fmaxf(-point->separation, 1.0e-6f);
            dynamic_state_write(state, capacity, index, NS_DYNAMIC_ROW_PAIR + slot,
                                (float)(partner + 1), point->normalImpulse, 0.0f, 0.0f);
            dynamic_state_write(state, capacity, index, NS_DYNAMIC_ROW_CONTACT + slot,
                                normal.x, normal.y, normal.z, depth);
            dynamic_state_write(state, capacity, index, NS_DYNAMIC_ROW_POINT + slot,
                                (float)center.x + anchor.x, (float)center.y + anchor.y,
                                (float)center.z + anchor.z, 0.0f);
            slot += 1;
        }
    }
}

void dynamic_native_world_sync(uint64_t handle, float *state, int capacity, int row_count, int count) {
    dynamic_native_world *world = dynamic_world_from_handle(handle);
    if (world == NULL || state == NULL || capacity < world->capacity || row_count <= NS_DYNAMIC_ROW_POINT) {
        return;
    }
    if (count > world->count) {
        count = world->count;
    }

    for (int index = 0; index < count; ++index) {
        for (int slot = 0; slot < NS_DYNAMIC_MAX_CONTACTS; ++slot) {
            dynamic_state_write(state, capacity, index, NS_DYNAMIC_ROW_PAIR + slot, 0.0f, 0.0f, 0.0f, 0.0f);
            dynamic_state_write(state, capacity, index, NS_DYNAMIC_ROW_CONTACT + slot, 0.0f, 0.0f, 0.0f, 0.0f);
            dynamic_state_write(state, capacity, index, NS_DYNAMIC_ROW_POINT + slot, 0.0f, 0.0f, 0.0f, 0.0f);
        }

        dynamic_native_body *body = &world->bodies[index];
        b3Pos position = b3Body_GetPosition(body->body_id);
        b3Quat orientation = b3Body_GetRotation(body->body_id);
        b3Vec3 velocity = b3Body_GetLinearVelocity(body->body_id);
        b3Vec3 angular = b3Body_GetAngularVelocity(body->body_id);

        int position_base = dynamic_state_index(capacity, index, NS_DYNAMIC_ROW_POSITION);
        float bound = state[position_base + 3];
        int inertia_base = dynamic_state_index(capacity, index, NS_DYNAMIC_ROW_INERTIA);
        float hull_count = state[inertia_base + 3];
        dynamic_state_write(state, capacity, index, NS_DYNAMIC_ROW_POSITION,
                            (float)position.x, (float)position.y, (float)position.z, bound);
        dynamic_state_write(state, capacity, index, NS_DYNAMIC_ROW_ORIENTATION,
                            orientation.v.x, orientation.v.y, orientation.v.z, orientation.s);
        dynamic_state_write(state, capacity, index, NS_DYNAMIC_ROW_VELOCITY,
                            velocity.x, velocity.y, velocity.z, body->inverse_mass);
        dynamic_state_write(state, capacity, index, NS_DYNAMIC_ROW_ANGULAR,
                            angular.x, angular.y, angular.z, body->margin);
        dynamic_state_write(state, capacity, index, NS_DYNAMIC_ROW_MATERIAL,
                            body->restitution, body->friction, body->damping, body->angular_damping);
        dynamic_state_write(state, capacity, index, NS_DYNAMIC_ROW_INERTIA,
                            body->inverse_inertia.x, body->inverse_inertia.y, body->inverse_inertia.z, hull_count);
        dynamic_sync_contacts(world, state, capacity, index);
    }
}

#include "ns_type.h"

#include <errno.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

typedef struct io_image {
    i32 width;
    i32 height;
    i32 channels;
    u8 *data;
} io_image;

typedef struct io_glb {
    i32 json_size;
    i32 data_size;
    u8 *json;
    u8 *data;
    u8 *file;
} io_glb;

typedef struct io_glb_mesh {
    i32 vertex_count;
    i32 index_count;
    i32 joint_count;
    i32 image_width;
    i32 image_height;
    i32 image_channels;
    f32 *positions;
    f32 *normals;
    f32 *texcoords;
    i32 *joints;
    f32 *weights;
    f32 *joint_positions;
    u32 *indices;
    u8 *image;
} io_glb_mesh;

typedef struct io_json_span {
    const char *begin;
    const char *end;
} io_json_span;

void io_glb_destroy(io_glb *glb);
void io_glb_mesh_destroy(io_glb_mesh *mesh);

enum {
    IO_GLB_MAGIC = 0x46546c67,
    IO_GLB_VERSION = 2,
    IO_GLB_JSON = 0x4e4f534a,
    IO_GLB_BIN = 0x004e4942,
    IO_GLTF_BYTE = 5120,
    IO_GLTF_UNSIGNED_BYTE = 5121,
    IO_GLTF_SHORT = 5122,
    IO_GLTF_UNSIGNED_SHORT = 5123,
    IO_GLTF_UNSIGNED_INT = 5125,
    IO_GLTF_FLOAT = 5126
};

static u32 io_u32_read(const u8 *p) {
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static void io_u32_write(u8 *p, u32 value) {
    p[0] = (u8)value;
    p[1] = (u8)(value >> 8);
    p[2] = (u8)(value >> 16);
    p[3] = (u8)(value >> 24);
}

static const char *io_json_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static const char *io_json_string_end(const char *p, const char *end) {
    if (p >= end || *p != '"') return NULL;
    for (p++; p < end; p++) {
        if (*p == '\\') {
            if (++p >= end) return NULL;
        } else if (*p == '"') {
            return p + 1;
        }
    }
    return NULL;
}

static const char *io_json_value_end(const char *p, const char *end) {
    p = io_json_ws(p, end);
    if (p >= end) return NULL;
    if (*p == '"') return io_json_string_end(p, end);
    if (*p == '{' || *p == '[') {
        const char open = *p;
        const char close = open == '{' ? '}' : ']';
        i32 depth = 1;
        for (p++; p < end; p++) {
            if (*p == '"') {
                p = io_json_string_end(p, end);
                if (!p) return NULL;
                p--;
            } else if (*p == open) {
                depth++;
            } else if (*p == close && --depth == 0) {
                return p + 1;
            }
        }
        return NULL;
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
    return p;
}

static ns_bool io_json_key_equals(const char *begin, const char *end, const char *key) {
    const size_t len = (size_t)(end - begin);
    return strlen(key) == len && memcmp(begin, key, len) == 0;
}

static io_json_span io_json_prop(io_json_span object, const char *key) {
    io_json_span missing = {0};
    const char *p = io_json_ws(object.begin, object.end);
    if (p >= object.end || *p != '{') return missing;
    p++;
    while ((p = io_json_ws(p, object.end)) < object.end && *p != '}') {
        const char *key_end = io_json_string_end(p, object.end);
        if (!key_end) return missing;
        const char *colon = io_json_ws(key_end, object.end);
        if (colon >= object.end || *colon != ':') return missing;
        const char *value = io_json_ws(colon + 1, object.end);
        const char *value_end = io_json_value_end(value, object.end);
        if (!value_end) return missing;
        if (io_json_key_equals(p + 1, key_end - 1, key)) {
            return (io_json_span){value, value_end};
        }
        p = io_json_ws(value_end, object.end);
        if (p < object.end && *p == ',') p++;
    }
    return missing;
}

static io_json_span io_json_item(io_json_span array, i32 index) {
    io_json_span missing = {0};
    const char *p = io_json_ws(array.begin, array.end);
    if (index < 0 || p >= array.end || *p != '[') return missing;
    p++;
    for (i32 at = 0; ; at++) {
        p = io_json_ws(p, array.end);
        if (p >= array.end || *p == ']') return missing;
        const char *value_end = io_json_value_end(p, array.end);
        if (!value_end) return missing;
        if (at == index) return (io_json_span){p, value_end};
        p = io_json_ws(value_end, array.end);
        if (p < array.end && *p == ',') p++;
    }
}

static i32 io_json_int(io_json_span value, i32 fallback) {
    if (!value.begin) return fallback;
    errno = 0;
    char *end = NULL;
    long result = strtol(value.begin, &end, 10);
    if (errno || end == value.begin || end > value.end || result < INT32_MIN || result > INT32_MAX) return fallback;
    return (i32)result;
}

static ns_bool io_json_bool(io_json_span value) {
    return value.begin && value.end - value.begin == 4 && memcmp(value.begin, "true", 4) == 0;
}

static ns_bool io_json_string_is(io_json_span value, const char *text) {
    const size_t len = strlen(text);
    return value.begin && value.end - value.begin == (ptrdiff_t)len + 2 && value.begin[0] == '"' &&
           memcmp(value.begin + 1, text, len) == 0 && value.end[-1] == '"';
}

static ns_bool io_file_read_all(const char *path, u8 **out, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    ns_bool ok = fseek(file, 0, SEEK_END) == 0;
    long end = ok ? ftell(file) : -1;
    ok = ok && end >= 0 && fseek(file, 0, SEEK_SET) == 0;
    u8 *bytes = ok && end > 0 ? (u8 *)malloc((size_t)end) : NULL;
    ok = ok && end > 0 && bytes && fread(bytes, 1, (size_t)end, file) == (size_t)end;
    fclose(file);
    if (!ok) {
        free(bytes);
        return false;
    }
    *out = bytes;
    *size = (size_t)end;
    return true;
}

static ns_bool io_file_write_all(const char *path, const u8 *bytes, size_t size) {
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    ns_bool ok = fwrite(bytes, 1, size, file) == size;
    if (fclose(file) != 0) ok = false;
    if (!ok) remove(path);
    return ok;
}

io_glb *io_glb_read(const char *path) {
    u8 *file = NULL;
    size_t size = 0;
    if (!io_file_read_all(path, &file, &size)) {
        ns_error("io", "Failed to load GLB from file: %s\n", path);
        return NULL;
    }
    if (size < 20 || io_u32_read(file) != IO_GLB_MAGIC || io_u32_read(file + 4) != IO_GLB_VERSION ||
        io_u32_read(file + 8) != size) {
        ns_error("io", "Invalid GLB header: %s\n", path);
        free(file);
        return NULL;
    }

    io_glb *glb = (io_glb *)malloc(sizeof(io_glb));
    if (!glb) {
        free(file);
        return NULL;
    }
    memset(glb, 0, sizeof(*glb));
    glb->file = file;
    size_t offset = 12;
    while (offset + 8 <= size) {
        const u32 chunk_size = io_u32_read(file + offset);
        const u32 chunk_type = io_u32_read(file + offset + 4);
        offset += 8;
        if ((size_t)chunk_size > size - offset) break;
        if (chunk_type == IO_GLB_JSON && !glb->json) {
            glb->json = file + offset;
            glb->json_size = (i32)chunk_size;
        } else if (chunk_type == IO_GLB_BIN && !glb->data) {
            glb->data = file + offset;
            glb->data_size = (i32)chunk_size;
        }
        offset += chunk_size;
    }
    if (!glb->json || offset != size) {
        ns_error("io", "Invalid GLB chunks: %s\n", path);
        io_glb_destroy(glb);
        return NULL;
    }
    return glb;
}

io_glb *io_glb_create(const u8 *json, i32 json_size, const u8 *data, i32 data_size) {
    if (!json || json_size <= 0 || data_size < 0 || (data_size > 0 && !data)) return NULL;
    io_glb *glb = (io_glb *)malloc(sizeof(io_glb));
    if (!glb) return NULL;
    memset(glb, 0, sizeof(*glb));
    glb->json = (u8 *)malloc((size_t)json_size + 1);
    glb->data = data_size > 0 ? (u8 *)malloc((size_t)data_size) : NULL;
    if (!glb->json || (data_size > 0 && !glb->data)) {
        io_glb_destroy(glb);
        return NULL;
    }
    memcpy(glb->json, json, (size_t)json_size);
    glb->json[json_size] = 0;
    if (data_size > 0) memcpy(glb->data, data, (size_t)data_size);
    glb->json_size = json_size;
    glb->data_size = data_size;
    return glb;
}

i32 io_glb_valid(const io_glb *glb) {
    return glb && glb->json && glb->json_size > 0;
}

i32 io_glb_json_size(const io_glb *glb) {
    return glb ? glb->json_size : 0;
}

i32 io_glb_data_size(const io_glb *glb) {
    return glb ? glb->data_size : 0;
}

i32 io_glb_copy_json(const io_glb *glb, u8 *values, i32 capacity) {
    if (!glb || !values || capacity < glb->json_size) return 0;
    memcpy(values, glb->json, (size_t)glb->json_size);
    return glb->json_size;
}

i32 io_glb_copy_data(const io_glb *glb, u8 *values, i32 capacity) {
    if (!glb || !values || capacity < glb->data_size) return 0;
    if (glb->data_size > 0) memcpy(values, glb->data, (size_t)glb->data_size);
    return glb->data_size;
}

i32 io_glb_write(const char *path, const io_glb *glb) {
    if (!glb || !glb->json || glb->json_size <= 0 || glb->data_size < 0 ||
        (glb->data_size > 0 && !glb->data)) return 0;
    const u32 json_size = ((u32)glb->json_size + 3u) & ~3u;
    const u32 data_size = ((u32)glb->data_size + 3u) & ~3u;
    const u32 total = 12u + 8u + json_size + (glb->data_size > 0 ? 8u + data_size : 0u);
    u8 *file = (u8 *)malloc(total);
    if (!file) return 0;
    memset(file, 0, total);
    io_u32_write(file, IO_GLB_MAGIC);
    io_u32_write(file + 4, IO_GLB_VERSION);
    io_u32_write(file + 8, total);
    io_u32_write(file + 12, json_size);
    io_u32_write(file + 16, IO_GLB_JSON);
    memcpy(file + 20, glb->json, (size_t)glb->json_size);
    memset(file + 20 + glb->json_size, 0x20, json_size - (u32)glb->json_size);
    if (glb->data_size > 0) {
        const u32 at = 20u + json_size;
        io_u32_write(file + at, data_size);
        io_u32_write(file + at + 4, IO_GLB_BIN);
        memcpy(file + at + 8, glb->data, (size_t)glb->data_size);
    }
    const ns_bool ok = io_file_write_all(path, file, total);
    free(file);
    if (!ok) ns_error("io", "Failed to save GLB to file: %s\n", path);
    return ok;
}

void io_glb_destroy(io_glb *glb) {
    if (!glb) return;
    if (glb->file) {
        free(glb->file);
    } else {
        free(glb->json);
        free(glb->data);
    }
    free(glb);
}

static i32 io_gltf_type_components(io_json_span type) {
    if (io_json_string_is(type, "SCALAR")) return 1;
    if (io_json_string_is(type, "VEC2")) return 2;
    if (io_json_string_is(type, "VEC3")) return 3;
    if (io_json_string_is(type, "VEC4")) return 4;
    if (io_json_string_is(type, "MAT2")) return 4;
    if (io_json_string_is(type, "MAT3")) return 9;
    if (io_json_string_is(type, "MAT4")) return 16;
    return 0;
}

static i32 io_gltf_component_size(i32 type) {
    if (type == IO_GLTF_BYTE || type == IO_GLTF_UNSIGNED_BYTE) return 1;
    if (type == IO_GLTF_SHORT || type == IO_GLTF_UNSIGNED_SHORT) return 2;
    if (type == IO_GLTF_UNSIGNED_INT || type == IO_GLTF_FLOAT) return 4;
    return 0;
}

typedef struct io_gltf_accessor {
    const u8 *data;
    i32 count;
    i32 components;
    i32 component_type;
    i32 stride;
    ns_bool normalized;
} io_gltf_accessor;

static ns_bool io_gltf_accessor_read(const io_glb *glb, io_json_span root, i32 index, io_gltf_accessor *out) {
    io_json_span accessor = io_json_item(io_json_prop(root, "accessors"), index);
    if (!accessor.begin || io_json_prop(accessor, "sparse").begin) return false;
    const i32 view_index = io_json_int(io_json_prop(accessor, "bufferView"), -1);
    io_json_span view = io_json_item(io_json_prop(root, "bufferViews"), view_index);
    if (!view.begin || io_json_int(io_json_prop(view, "buffer"), 0) != 0) return false;
    const i32 count = io_json_int(io_json_prop(accessor, "count"), -1);
    const i32 components = io_gltf_type_components(io_json_prop(accessor, "type"));
    const i32 component_type = io_json_int(io_json_prop(accessor, "componentType"), -1);
    const i32 component_size = io_gltf_component_size(component_type);
    const i32 view_offset = io_json_int(io_json_prop(view, "byteOffset"), 0);
    const i32 accessor_offset = io_json_int(io_json_prop(accessor, "byteOffset"), 0);
    const i32 element_size = component_size * components;
    const i32 stride = io_json_int(io_json_prop(view, "byteStride"), element_size);
    const i64 begin = (i64)view_offset + accessor_offset;
    const i64 last = count > 0 ? begin + (i64)(count - 1) * stride + element_size : begin;
    if (count < 0 || components == 0 || component_size == 0 || stride < element_size || begin < 0 || last > glb->data_size) return false;
    *out = (io_gltf_accessor){glb->data + begin, count, components, component_type, stride,
                              io_json_bool(io_json_prop(accessor, "normalized"))};
    return true;
}

static f32 io_gltf_float(const u8 *p, i32 type, ns_bool normalized) {
    if (type == IO_GLTF_FLOAT) {
        f32 value;
        memcpy(&value, p, sizeof(value));
        return value;
    }
    if (type == IO_GLTF_BYTE) {
        const i8 value = *(const i8 *)p;
        return normalized ? fmaxf(-1.0f, (f32)value / 127.0f) : (f32)value;
    }
    if (type == IO_GLTF_UNSIGNED_BYTE) {
        const u8 value = *p;
        return normalized ? (f32)value / 255.0f : (f32)value;
    }
    if (type == IO_GLTF_SHORT) {
        i16 value;
        memcpy(&value, p, sizeof(value));
        return normalized ? fmaxf(-1.0f, (f32)value / 32767.0f) : (f32)value;
    }
    if (type == IO_GLTF_UNSIGNED_SHORT) {
        u16 value;
        memcpy(&value, p, sizeof(value));
        return normalized ? (f32)value / 65535.0f : (f32)value;
    }
    if (type == IO_GLTF_UNSIGNED_INT) {
        u32 value;
        memcpy(&value, p, sizeof(value));
        return normalized ? (f32)((f64)value / 4294967295.0) : (f32)value;
    }
    return 0.0f;
}

static u32 io_gltf_uint(const u8 *p, i32 type) {
    if (type == IO_GLTF_UNSIGNED_BYTE) return *p;
    if (type == IO_GLTF_UNSIGNED_SHORT) {
        u16 value;
        memcpy(&value, p, sizeof(value));
        return value;
    }
    if (type == IO_GLTF_UNSIGNED_INT) {
        u32 value;
        memcpy(&value, p, sizeof(value));
        return value;
    }
    return 0;
}

static f32 *io_gltf_floats(io_gltf_accessor accessor, i32 wanted, i32 count) {
    f32 *values = (f32 *)malloc(sizeof(f32) * (size_t)count * wanted);
    if (!values) return NULL;
    const i32 component_size = io_gltf_component_size(accessor.component_type);
    for (i32 row = 0; row < count; row++) {
        for (i32 component = 0; component < wanted; component++) {
            values[row * wanted + component] = component < accessor.components
                ? io_gltf_float(accessor.data + (i64)row * accessor.stride + component * component_size,
                                accessor.component_type, accessor.normalized)
                : 0.0f;
        }
    }
    return values;
}

static i32 *io_gltf_joints(io_gltf_accessor accessor, i32 count) {
    i32 *values = (i32 *)malloc(sizeof(i32) * (size_t)count * 4);
    if (!values) return NULL;
    const i32 component_size = io_gltf_component_size(accessor.component_type);
    for (i32 row = 0; row < count; row++) {
        for (i32 component = 0; component < 4; component++) {
            values[row * 4 + component] = component < accessor.components
                ? (i32)io_gltf_uint(accessor.data + (i64)row * accessor.stride + component * component_size,
                                    accessor.component_type)
                : 0;
        }
    }
    return values;
}

// A glTF inverse-bind matrix maps a mesh-space point into joint space. The
// bind-pose joint origin is therefore the point p for which A*p + t = 0.
static ns_bool io_gltf_inverse_bind_origin(const u8 *data, i32 component_type,
                                           ns_bool normalized, f32 out[3]) {
    const i32 size = io_gltf_component_size(component_type);
    f32 m[16];
    for (i32 i = 0; i < 16; i++) m[i] = io_gltf_float(data + i * size, component_type, normalized);
    const f32 a00 = m[0], a01 = m[4], a02 = m[8];
    const f32 a10 = m[1], a11 = m[5], a12 = m[9];
    const f32 a20 = m[2], a21 = m[6], a22 = m[10];
    const f32 det = a00 * (a11 * a22 - a12 * a21) -
                    a01 * (a10 * a22 - a12 * a20) +
                    a02 * (a10 * a21 - a11 * a20);
    if (fabsf(det) < 1e-12f) return false;
    const f32 inv_det = 1.0f / det;
    const f32 i00 = (a11 * a22 - a12 * a21) * inv_det;
    const f32 i01 = (a02 * a21 - a01 * a22) * inv_det;
    const f32 i02 = (a01 * a12 - a02 * a11) * inv_det;
    const f32 i10 = (a12 * a20 - a10 * a22) * inv_det;
    const f32 i11 = (a00 * a22 - a02 * a20) * inv_det;
    const f32 i12 = (a02 * a10 - a00 * a12) * inv_det;
    const f32 i20 = (a10 * a21 - a11 * a20) * inv_det;
    const f32 i21 = (a01 * a20 - a00 * a21) * inv_det;
    const f32 i22 = (a00 * a11 - a01 * a10) * inv_det;
    out[0] = 0.0f - (i00 * m[12] + i01 * m[13] + i02 * m[14]);
    out[1] = 0.0f - (i10 * m[12] + i11 * m[13] + i12 * m[14]);
    out[2] = 0.0f - (i20 * m[12] + i21 * m[13] + i22 * m[14]);
    return true;
}

static void io_gltf_skin_joints(io_glb_mesh *mesh, const io_glb *glb,
                                io_json_span root, i32 mesh_index) {
    io_json_span nodes = io_json_prop(root, "nodes");
    i32 skin_index = -1;
    for (i32 index = 0;; index++) {
        io_json_span node = io_json_item(nodes, index);
        if (!node.begin) break;
        if (io_json_int(io_json_prop(node, "mesh"), -1) == mesh_index) {
            skin_index = io_json_int(io_json_prop(node, "skin"), -1);
            if (skin_index >= 0) break;
        }
    }
    if (skin_index < 0) return;
    io_json_span skin = io_json_item(io_json_prop(root, "skins"), skin_index);
    const i32 inverse_bind_index = io_json_int(io_json_prop(skin, "inverseBindMatrices"), -1);
    io_gltf_accessor inverse_bind = {0};
    if (!io_gltf_accessor_read(glb, root, inverse_bind_index, &inverse_bind) ||
        inverse_bind.components != 16 || inverse_bind.count <= 0) return;
    f32 *positions = (f32 *)malloc(sizeof(f32) * (size_t)inverse_bind.count * 3);
    if (!positions) return;
    for (i32 joint = 0; joint < inverse_bind.count; joint++) {
        if (!io_gltf_inverse_bind_origin(inverse_bind.data + (i64)joint * inverse_bind.stride,
                                         inverse_bind.component_type, inverse_bind.normalized,
                                         positions + joint * 3)) {
            free(positions);
            return;
        }
    }
    mesh->joint_count = inverse_bind.count;
    mesh->joint_positions = positions;
}

static ns_bool io_gltf_image(io_glb_mesh *mesh, const io_glb *glb, io_json_span root, io_json_span primitive) {
    const i32 material_index = io_json_int(io_json_prop(primitive, "material"), -1);
    io_json_span material = io_json_item(io_json_prop(root, "materials"), material_index);
    io_json_span pbr = io_json_prop(material, "pbrMetallicRoughness");
    io_json_span texture_info = io_json_prop(pbr, "baseColorTexture");
    const i32 texture_index = io_json_int(io_json_prop(texture_info, "index"), -1);
    io_json_span texture = io_json_item(io_json_prop(root, "textures"), texture_index);
    const i32 image_index = io_json_int(io_json_prop(texture, "source"), -1);
    io_json_span image = io_json_item(io_json_prop(root, "images"), image_index);
    const i32 view_index = io_json_int(io_json_prop(image, "bufferView"), -1);
    io_json_span view = io_json_item(io_json_prop(root, "bufferViews"), view_index);
    const i32 offset = io_json_int(io_json_prop(view, "byteOffset"), 0);
    const i32 size = io_json_int(io_json_prop(view, "byteLength"), -1);
    if (!image.begin || !view.begin || offset < 0 || size <= 0 || (i64)offset + size > glb->data_size) return true;
    int source_channels = 0;
    u8 *decoded = stbi_load_from_memory(glb->data + offset, size, &mesh->image_width, &mesh->image_height,
                                        &source_channels, 4);
    if (!decoded) return false;
    mesh->image = decoded;
    mesh->image_channels = 4;
    return true;
}

io_glb_mesh *io_glb_mesh_create(void) {
    io_glb_mesh *mesh = (io_glb_mesh *)malloc(sizeof(io_glb_mesh));
    if (mesh) memset(mesh, 0, sizeof(*mesh));
    return mesh;
}

io_glb_mesh *io_glb_mesh_read(const io_glb *glb, i32 mesh_index, i32 primitive_index) {
    if (!glb || !glb->json || !glb->data || glb->json_size <= 0 || glb->data_size <= 0) return NULL;
    io_json_span root = {(const char *)glb->json, (const char *)glb->json + glb->json_size};
    io_json_span mesh_json = io_json_item(io_json_prop(root, "meshes"), mesh_index);
    io_json_span primitive = io_json_item(io_json_prop(mesh_json, "primitives"), primitive_index);
    io_json_span attributes = io_json_prop(primitive, "attributes");
    if (!primitive.begin || io_json_int(io_json_prop(primitive, "mode"), 4) != 4) return NULL;

    io_gltf_accessor position = {0};
    const i32 position_index = io_json_int(io_json_prop(attributes, "POSITION"), -1);
    if (!io_gltf_accessor_read(glb, root, position_index, &position) || position.components != 3) return NULL;
    io_glb_mesh *mesh = io_glb_mesh_create();
    if (!mesh) return NULL;
    mesh->vertex_count = position.count;
    mesh->positions = io_gltf_floats(position, 3, position.count);

    io_gltf_accessor attribute = {0};
    const i32 normal_index = io_json_int(io_json_prop(attributes, "NORMAL"), -1);
    if (normal_index >= 0 && io_gltf_accessor_read(glb, root, normal_index, &attribute) && attribute.count == position.count)
        mesh->normals = io_gltf_floats(attribute, 3, position.count);
    const i32 texcoord_index = io_json_int(io_json_prop(attributes, "TEXCOORD_0"), -1);
    if (texcoord_index >= 0 && io_gltf_accessor_read(glb, root, texcoord_index, &attribute) && attribute.count == position.count)
        mesh->texcoords = io_gltf_floats(attribute, 2, position.count);
    const i32 joints_index = io_json_int(io_json_prop(attributes, "JOINTS_0"), -1);
    if (joints_index >= 0 && io_gltf_accessor_read(glb, root, joints_index, &attribute) && attribute.count == position.count)
        mesh->joints = io_gltf_joints(attribute, position.count);
    const i32 weights_index = io_json_int(io_json_prop(attributes, "WEIGHTS_0"), -1);
    if (weights_index >= 0 && io_gltf_accessor_read(glb, root, weights_index, &attribute) && attribute.count == position.count)
        mesh->weights = io_gltf_floats(attribute, 4, position.count);
    io_gltf_skin_joints(mesh, glb, root, mesh_index);

    const i32 indices_index = io_json_int(io_json_prop(primitive, "indices"), -1);
    io_gltf_accessor indices = {0};
    if (indices_index >= 0 && io_gltf_accessor_read(glb, root, indices_index, &indices) && indices.components == 1) {
        mesh->index_count = indices.count;
        mesh->indices = (u32 *)malloc(sizeof(u32) * (size_t)indices.count);
        if (mesh->indices) {
            for (i32 i = 0; i < indices.count; i++)
                mesh->indices[i] = io_gltf_uint(indices.data + (i64)i * indices.stride, indices.component_type);
        }
    } else {
        mesh->index_count = position.count;
        mesh->indices = (u32 *)malloc(sizeof(u32) * (size_t)position.count);
        if (mesh->indices) for (i32 i = 0; i < position.count; i++) mesh->indices[i] = (u32)i;
    }
    if (!mesh->positions || !mesh->indices || !io_gltf_image(mesh, glb, root, primitive)) {
        io_glb_mesh_destroy(mesh);
        return NULL;
    }
    return mesh;
}

i32 io_glb_mesh_valid(const io_glb_mesh *mesh) {
    return mesh && mesh->positions && mesh->indices && mesh->vertex_count > 0 && mesh->index_count > 0;
}

i32 io_glb_mesh_vertex_count(const io_glb_mesh *mesh) { return mesh ? mesh->vertex_count : 0; }
i32 io_glb_mesh_index_count(const io_glb_mesh *mesh) { return mesh ? mesh->index_count : 0; }
i32 io_glb_mesh_joint_count(const io_glb_mesh *mesh) { return mesh ? mesh->joint_count : 0; }
i32 io_glb_mesh_image_width(const io_glb_mesh *mesh) { return mesh ? mesh->image_width : 0; }
i32 io_glb_mesh_image_height(const io_glb_mesh *mesh) { return mesh ? mesh->image_height : 0; }
i32 io_glb_mesh_image_channels(const io_glb_mesh *mesh) { return mesh ? mesh->image_channels : 0; }

static i32 io_glb_mesh_copy(void *dst, i32 capacity, const void *src, i32 count, size_t element_size) {
    if (!dst || capacity < count || !src || count <= 0) return 0;
    memcpy(dst, src, (size_t)count * element_size);
    return count;
}

i32 io_glb_mesh_copy_positions(const io_glb_mesh *mesh, f32 *values, i32 capacity) {
    return mesh ? io_glb_mesh_copy(values, capacity, mesh->positions, mesh->vertex_count * 3, sizeof(f32)) : 0;
}

i32 io_glb_mesh_copy_normals(const io_glb_mesh *mesh, f32 *values, i32 capacity) {
    return mesh ? io_glb_mesh_copy(values, capacity, mesh->normals, mesh->vertex_count * 3, sizeof(f32)) : 0;
}

i32 io_glb_mesh_copy_texcoords(const io_glb_mesh *mesh, f32 *values, i32 capacity) {
    return mesh ? io_glb_mesh_copy(values, capacity, mesh->texcoords, mesh->vertex_count * 2, sizeof(f32)) : 0;
}

i32 io_glb_mesh_copy_joints(const io_glb_mesh *mesh, i32 *values, i32 capacity) {
    return mesh ? io_glb_mesh_copy(values, capacity, mesh->joints, mesh->vertex_count * 4, sizeof(i32)) : 0;
}

i32 io_glb_mesh_copy_weights(const io_glb_mesh *mesh, f32 *values, i32 capacity) {
    return mesh ? io_glb_mesh_copy(values, capacity, mesh->weights, mesh->vertex_count * 4, sizeof(f32)) : 0;
}

i32 io_glb_mesh_copy_joint_positions(const io_glb_mesh *mesh, f32 *values, i32 capacity) {
    return mesh ? io_glb_mesh_copy(values, capacity, mesh->joint_positions, mesh->joint_count * 3, sizeof(f32)) : 0;
}

i32 io_glb_mesh_copy_indices(const io_glb_mesh *mesh, u32 *values, i32 capacity) {
    return mesh ? io_glb_mesh_copy(values, capacity, mesh->indices, mesh->index_count, sizeof(u32)) : 0;
}

i32 io_glb_mesh_copy_colors(const io_glb_mesh *mesh, f32 *values, i32 capacity) {
    const i32 count = mesh ? mesh->vertex_count * 3 : 0;
    if (!mesh || !values || capacity < count || !mesh->texcoords || !mesh->image ||
        mesh->image_width <= 0 || mesh->image_height <= 0 || mesh->image_channels < 3) return 0;
    for (i32 vertex = 0; vertex < mesh->vertex_count; vertex++) {
        const f32 u = fminf(1.0f, fmaxf(0.0f, mesh->texcoords[vertex * 2]));
        const f32 v = fminf(1.0f, fmaxf(0.0f, mesh->texcoords[vertex * 2 + 1]));
        const i32 x = (i32)(u * (mesh->image_width - 1) + 0.5f);
        const i32 y = (i32)(v * (mesh->image_height - 1) + 0.5f);
        const i64 source = ((i64)y * mesh->image_width + x) * mesh->image_channels;
        values[vertex * 3] = (f32)mesh->image[source] / 255.0f;
        values[vertex * 3 + 1] = (f32)mesh->image[source + 1] / 255.0f;
        values[vertex * 3 + 2] = (f32)mesh->image[source + 2] / 255.0f;
    }
    return count;
}

i32 io_glb_mesh_copy_image(const io_glb_mesh *mesh, u8 *values, i32 capacity) {
    if (!mesh || mesh->image_width <= 0 || mesh->image_height <= 0 || mesh->image_channels <= 0) return 0;
    const i64 count = (i64)mesh->image_width * mesh->image_height * mesh->image_channels;
    if (count > INT32_MAX) return 0;
    return io_glb_mesh_copy(values, capacity, mesh->image, (i32)count, sizeof(u8));
}

void io_glb_mesh_destroy(io_glb_mesh *mesh) {
    if (!mesh) return;
    free(mesh->positions);
    free(mesh->normals);
    free(mesh->texcoords);
    free(mesh->joints);
    free(mesh->weights);
    free(mesh->joint_positions);
    free(mesh->indices);
    stbi_image_free(mesh->image);
    free(mesh);
}

// `path` arrives as a C string: the ns runtime passes `str` arguments to ref
// functions as a char* (see the FFI string marshalling in ns_vm_lib.c), not as
// an ns_str struct by value.
io_image* io_load_image(const char *path) {
    io_image *img = (io_image*)ns_malloc(sizeof(io_image));
    if (img == NULL) {
        ns_error("io", "Failed to allocate memory for image\n");
        return NULL;
    }

    img->width = 0;
    img->height = 0;
    img->channels = 0;
    img->data = NULL;
    img->data = stbi_load(path, &img->width, &img->height, &img->channels, 0);
    if (img->data == NULL) {
        ns_error("io", "Failed to load image from file: %s\n", path);
        return img;
    }

    return img;
}

void io_image_destroy(io_image *img) {
    if (img == NULL) return;
    stbi_image_free(img->data);
    ns_free(img);
}

i32 io_save_image(const char *path, const io_image *img) {
    if (img == NULL) {
        ns_error("io", "Image is NULL\n");
        return 0;
    }

    i32 result = stbi_write_png(path, img->width, img->height, img->channels, img->data, img->width * img->channels);
    if (result == 0) {
        ns_error("io", "Failed to save image to file: %s\n", path);
    }
    return result;
}

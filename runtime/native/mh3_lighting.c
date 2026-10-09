/* Typed reconstruction of RMHP08's NW4R LightObj and LightSetting setup.
 * Evidence: Ghidra 8007a57c..8007b104 / 8007a944 and the game's DOL constants.
 * No GX entry points, CPU context emulation, FIFO bytes or GPU waits. */
#include "mh3_lighting.h"
#include <math.h>
#include <string.h>

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void write32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static float read_float(const uint8_t *p) {
    uint32_t bits = read32(p);
    float value;
    memcpy(&value, &bits, 4);
    return value;
}
static void write_float(uint8_t *p, float value) {
    uint32_t bits;
    memcpy(&bits, &value, 4);
    write32(p, bits);
}
static void spot_flags(mh3_light *light) {
    light->flags = (light->flags & ~(uint32_t)MH3_LIGHT_SPECULAR) | MH3_LIGHT_SPOT;
}
void mh3_light_set_position(mh3_light *light, float x, float y, float z) {
    light->position = (mh3_vec3){{x, y, z}};
    light->flags &= ~(uint32_t)MH3_LIGHT_SPECULAR;
}
void mh3_light_set_direction(mh3_light *light, float x, float y, float z, int specular) {
    light->direction = (mh3_vec3){{-x, -y, -z}};
    if (specular)
        light->flags = (light->flags & ~(uint32_t)MH3_LIGHT_SPOT) | MH3_LIGHT_SPECULAR | MH3_LIGHT_SPECULAR_DIRECTION;
    else
        spot_flags(light);
}
void mh3_light_set_angular(mh3_light *light, float a, float b, float c) {
    light->angular[0] = a;
    light->angular[1] = b;
    light->angular[2] = c;
    spot_flags(light);
}
void mh3_light_set_distance(mh3_light *light, float a, float b, float c) {
    light->distance[0] = a;
    light->distance[1] = b;
    light->distance[2] = c;
    spot_flags(light);
}
void mh3_light_set_spot(mh3_light *light, float cutoff, uint32_t shape) {
    float a = 1.0f, b = 0.0f, c = 0.0f;
    if (cutoff <= 0.0f || cutoff > 90.0f)
        shape = 0;
    if (shape >= 1 && shape <= 6) {
        /* The DOL uses binary32 pi, rounds its product, then divides by 180. */
        float cosine = cosf((3.1415927410125732f * cutoff) / 180.0f);
        float remaining = 1.0f - cosine;
        float reciprocal;
        switch (shape) {
        case 1:
            a = -1000.0f * cosine;
            b = 1000.0f;
            break;
        case 2:
            b = 1.0f / remaining;
            a = -cosine * b;
            break;
        case 3:
            a = 0.0f;
            c = 1.0f / remaining;
            b = -cosine * c;
            break;
        case 4:
            reciprocal = 1.0f / (remaining * remaining);
            a = reciprocal * cosine * (cosine - 2.0f);
            b = 2.0f * reciprocal;
            c = -reciprocal;
            break;
        case 5:
            reciprocal = 1.0f / (remaining * remaining);
            c = -4.0f * reciprocal;
            b = 4.0f * (1.0f + cosine) * reciprocal;
            a = c * cosine;
            break;
        case 6:
            reciprocal = 1.0f / (remaining * remaining);
            a = 1.0f - reciprocal * 2.0f * cosine * cosine;
            b = 4.0f * cosine * reciprocal;
            c = -2.0f * reciprocal;
            break;
        }
    }
    mh3_light_set_angular(light, a, b, c);
}
void mh3_light_set_distance_attenuation(mh3_light *light, float distance, float brightness, uint32_t shape) {
    float a = 1.0f, b = 0.0f, c = 0.0f;
    if (distance < 0.0f || brightness <= 0.0f || brightness >= 1.0f)
        shape = 0;
    switch (shape) {
    case 1:
        b = (1.0f - brightness) / (brightness * distance);
        break;
    case 2:
        b = (0.5f * (1.0f - brightness)) / (brightness * distance);
        c = (0.5f * (1.0f - brightness)) / (distance * (brightness * distance));
        break;
    case 3:
        c = (1.0f - brightness) / (distance * (brightness * distance));
        break;
    }
    mh3_light_set_distance(light, a, b, c);
}
void mh3_light_set_shininess(mh3_light *light, float shininess) {
    float half = shininess * 0.5f;
    light->angular[0] = 0.0f;
    light->angular[1] = 0.0f;
    light->angular[2] = 1.0f;
    light->distance[0] = half;
    light->distance[1] = 0.0f;
    light->distance[2] = 1.0f - half;
    light->flags = (light->flags & ~(uint32_t)MH3_LIGHT_SPOT) | MH3_LIGHT_SPECULAR;
}
void mh3_light_default(size_t index, mh3_light *out) {
    memset(out, 0, sizeof *out);
    if (index >= 3)
        return;
    out->color = (mh3_rgba8){255, 255, 255, 255};
    mh3_light_set_position(out, 4000000.0f, 4000000.0f, 4000000.0f);
    if (index == 0) {
        mh3_light_set_direction(out, 0.0f, -1.0f, 0.0f, 0);
        mh3_light_set_spot(out, 90.0f, 0);
        mh3_light_set_distance_attenuation(out, 10.0f, 0.5f, 0);
    } else if (index == 2) {
        mh3_light_set_direction(out, 0.0f, -1.0f, 0.0f, 1);
        mh3_light_set_shininess(out, 16.0f);
    }
    out->flags |= MH3_LIGHT_ENABLED;
}
static mh3_vec3 transform(const mh3_matrix34 *m, mh3_vec3 v, int point) {
    mh3_vec3 out;
    size_t row;
    for (row = 0; row < 3; row++) {
        float a = m->v[row][0] * v.v[0];
        float b = m->v[row][1] * v.v[1];
        float c = m->v[row][2] * v.v[2];
        out.v[row] = (a + b) + c;
        if (point)
            out.v[row] += m->v[row][3];
    }
    return out;
}
void mh3_light_apply_view(mh3_light *light, const mh3_matrix34 *view) {
    mh3_vec3 ray = {{-light->direction.v[0], -light->direction.v[1], -light->direction.v[2]}};
    ray = transform(view, ray, 0);
    if (light->flags & MH3_LIGHT_SPECULAR_DIRECTION) {
        mh3_vec3 half = {{-ray.v[0], -ray.v[1], 1.0f - ray.v[2]}};
        float magnitude = half.v[0] * half.v[0] + half.v[1] * half.v[1] + half.v[2] * half.v[2];
        float reciprocal = magnitude != 0.0f ? 1.0f / sqrtf(magnitude) : 0.0f;
        size_t i;
        for (i = 0; i < 3; i++) {
            light->direction.v[i] = half.v[i] * reciprocal;
            light->position.v[i] = -1.0e18f * ray.v[i];
        }
    } else {
        light->position = transform(view, light->position, 1);
        light->direction = (mh3_vec3){{-ray.v[0], -ray.v[1], -ray.v[2]}};
    }
}
mh3_vec3 mh3_light_ray_direction(const mh3_light *light) {
    mh3_vec3 ray = {{-light->direction.v[0], -light->direction.v[1], -light->direction.v[2]}};
    return ray;
}
mh3_res_status mh3_light_store_vector(mh3_res_mut_span record, const mh3_vec3 *vector) {
    size_t i;
    if (!record.data || !vector)
        return MH3_RES_INVALID;
    if (record.size < 12)
        return MH3_RES_RANGE;
    for (i = 0; i < 3; i++)
        write_float(record.data + i * 4, vector->v[i]);
    return MH3_RES_OK;
}
mh3_res_status mh3_light_clear_record(mh3_res_mut_span record) {
    if (!record.data)
        return MH3_RES_INVALID;
    if (record.size < MH3_LIGHT_RECORD_BYTES)
        return MH3_RES_RANGE;
    memset(record.data, 0, MH3_LIGHT_RECORD_BYTES);
    return MH3_RES_OK;
}
mh3_res_status mh3_light_copy_record(mh3_res_mut_span destination, mh3_res_span source) {
    if (!destination.data || !source.data)
        return MH3_RES_INVALID;
    if (destination.size < MH3_LIGHT_RECORD_BYTES || source.size < MH3_LIGHT_RECORD_BYTES)
        return MH3_RES_RANGE;
    memmove(destination.data, source.data, MH3_LIGHT_RECORD_BYTES);
    return MH3_RES_OK;
}
mh3_res_status mh3_light_read(mh3_res_span record, mh3_light *out) {
    size_t i;
    if (!record.data || !out)
        return MH3_RES_INVALID;
    if (record.size < MH3_LIGHT_RECORD_BYTES)
        return MH3_RES_RANGE;
    out->flags = read32(record.data);
    memcpy(&out->color, record.data + 16, 4);
    for (i = 0; i < 3; i++) {
        out->angular[i] = read_float(record.data + 20 + i * 4);
        out->distance[i] = read_float(record.data + 32 + i * 4);
        out->position.v[i] = read_float(record.data + 44 + i * 4);
        out->direction.v[i] = read_float(record.data + 56 + i * 4);
    }
    return MH3_RES_OK;
}
mh3_res_status mh3_light_store(mh3_res_mut_span record, const mh3_light *light) {
    size_t i;
    if (!record.data || !light)
        return MH3_RES_INVALID;
    if (record.size < MH3_LIGHT_RECORD_BYTES)
        return MH3_RES_RANGE;
    write32(record.data, light->flags);
    memcpy(record.data + 16, &light->color, 4);
    for (i = 0; i < 3; i++) {
        write_float(record.data + 20 + i * 4, light->angular[i]);
        write_float(record.data + 32 + i * 4, light->distance[i]);
        write_float(record.data + 44 + i * 4, light->position.v[i]);
        write_float(record.data + 56 + i * 4, light->direction.v[i]);
    }
    return MH3_RES_OK;
}
mh3_res_status mh3_light_setting_store_defaults(mh3_res_mut_span descriptor, mh3_res_mut_span lights, mh3_res_mut_span ambient, mh3_res_mut_span sets, uint16_t light_count, uint16_t set_count,
                                                uint32_t light_address, uint32_t ambient_address, uint32_t set_address) {
    size_t i;
    mh3_light light;
    if (!descriptor.data || !lights.data || !ambient.data || !sets.data)
        return MH3_RES_INVALID;
    if (!light_count || light_count > MH3_LIGHT_MAX || !set_count || set_count > MH3_LIGHT_SET_MAX)
        return MH3_RES_RANGE;
    if (descriptor.size < 16 || lights.size < (size_t)light_count * MH3_LIGHT_RECORD_BYTES || ambient.size < (size_t)light_count * 4 || sets.size < (size_t)set_count * MH3_LIGHT_SET_RECORD_BYTES)
        return MH3_RES_RANGE;
    descriptor.data[0] = (uint8_t)(light_count >> 8);
    descriptor.data[1] = (uint8_t)light_count;
    descriptor.data[2] = (uint8_t)(set_count >> 8);
    descriptor.data[3] = (uint8_t)set_count;
    write32(descriptor.data + 4, light_address);
    write32(descriptor.data + 8, ambient_address);
    write32(descriptor.data + 12, set_address);
    for (i = 0; i < set_count; i++)
        memset(sets.data + i * MH3_LIGHT_SET_RECORD_BYTES, 255, 9);
    sets.data[0] = 0;
    sets.data[1] = 2;
    for (i = 0; i < light_count; i++) {
        mh3_light_default(i, &light);
        memset(lights.data + i * MH3_LIGHT_RECORD_BYTES, 0, MH3_LIGHT_RECORD_BYTES);
        mh3_light_store((mh3_res_mut_span){lights.data + i * MH3_LIGHT_RECORD_BYTES, MH3_LIGHT_RECORD_BYTES}, &light);
    }
    memset(ambient.data, 255, (size_t)light_count * 4);
    return MH3_RES_OK;
}

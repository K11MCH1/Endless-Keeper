/******************************************************************************/
/** @file save_encode.cpp
 *     Encodes a struct into a record using its field table.
 */
/******************************************************************************/
#include "save_schema.h"
#include "save_internal.h"

#include <array>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <memory>

#include <algorithm>
/******************************************************************************/
/** Raw memory that is all zero reads the same in either byte order, so it needs no flag. */
static void note_raw_bytes(struct SaveBuffer *out, const uint8_t *bytes, uint32_t length)
{
    for (uint32_t index = 0; index < length; index++)
    {
        if (bytes[index] != 0)
        {
            out->has_raw_bytes = true;
            return;
        }
    }
}

static enum SaveResult encode_struct(struct SaveBuffer *out, const struct SaveStructDesc *desc, const uint8_t *src,
    enum SaveMode mode, struct SaveError *err, int depth);

/** The bytes of a union that its member in use doesn't cover. They are stored as they are in memory, and only
 *  when something is in them, so the bytes of a member that was picked wrongly are not lost. */
static enum SaveResult encode_member_tail(struct SaveBuffer *out, const struct SaveFieldDesc *field,
    const struct SaveStructDesc *member_table, const uint8_t *src, struct SaveError *err)
{
    const uint32_t region_end = field->offset + field->mem_size;
    uint32_t tail_start = save_struct_extent(member_table);
    tail_start = std::max(tail_start, field->offset);
    tail_start = std::min(tail_start, region_end);
    uint32_t tail_length = region_end - tail_start;
    bool has_bytes = false;
    for (uint32_t index = 0; index < tail_length; index++)
        has_bytes = has_bytes || (src[tail_start + index] != 0);
    if (has_bytes && (tail_length > 0xFFFF))
        return save_fail(err, SVR_Unsupported, "%s: the bytes of a union past its member don't fit", field->name);
    if (!has_bytes)
        tail_length = 0;
    int failed = save_buf_u16(out, static_cast<uint16_t>(tail_length));
    if (tail_length > 0)
    {
        failed |= save_buf_u16(out, static_cast<uint16_t>(tail_start - field->offset));
        failed |= save_buf_append(out, src + tail_start, tail_length);
        out->has_raw_bytes = true;
    }
    return failed ? save_fail(err, SVR_NoMemory, "out of memory") : SVR_Ok;
}

static enum SaveResult encode_union(struct SaveBuffer *out, const struct SaveStructDesc *desc, const struct SaveFieldDesc *field,
    const uint8_t *src, enum SaveMode mode, struct SaveError *err, int depth)
{
    const struct SaveUnionDesc *union_desc = field->uni;
    int pick = -1;
    enum SaveResult result = save_union_pick(desc, field, src, &pick, err);
    if (result != SVR_Ok)
        return result;
    if ((pick >= 0) && (union_desc->members[pick].sub == nullptr) && (union_desc->members[pick].raw_size == 0))
        pick = -1;
    if (save_buf_u8(out, static_cast<uint8_t>(pick + 1)) != 0)
        return save_fail(err, SVR_NoMemory, "out of memory");
    if (pick < 0)
        return SVR_Ok;
    if (union_desc->members[pick].sub != nullptr)
    {
        result = encode_struct(out, union_desc->members[pick].sub, src, mode, err, depth + 1);
        if (result != SVR_Ok)
            return result;
        return encode_member_tail(out, field, union_desc->members[pick].sub, src, err);
    }
    note_raw_bytes(out, src + field->offset, union_desc->members[pick].raw_size);
    if (save_buf_append(out, src + field->offset, union_desc->members[pick].raw_size) != 0)
        return save_fail(err, SVR_NoMemory, "out of memory");
    return SVR_Ok;
}

/* Zero elements (singleplayer pass, 2026-11-28): most of the game's big arrays of structs are unused slots, all zero
 * bytes in memory (8,192 creature controls of 11 KB, 32,767 things). An all-zero element is always written the same
 * way, so its bytes are worked out once per struct and mode (per thread: a save may be built on its own thread) and
 * copied for every such element. The record is byte for byte what the field walk writes (KFX_SAVE_ENCCHECK=1 builds
 * every record both ways and compares them). KFX_SAVE_ZEROFAST=0: the field walk for every element, as before. */
struct ZeroImage {
    const struct SaveStructDesc *desc;
    int mode;
    bool ok;
    std::vector<uint8_t> bytes;
};
static thread_local std::vector<std::unique_ptr<ZeroImage>> *zero_images; /* (heap entries: nested structs add theirs meanwhile) */
static bool zero_fast_on(void)
{
    static int on = -1;
    if (on < 0)
    {
        const char *v = getenv("KFX_SAVE_ZEROFAST");
        on = ((v == nullptr) || (strcmp(v, "0") != 0)) ? 1 : 0;
    }
    return on > 0;
}
static thread_local bool zero_walk_forced; /* (KFX_SAVE_ENCCHECK: the second build walks every field) */

static bool bytes_all_zero(const uint8_t *p, size_t n)
{
    size_t i = 0;
    for (; (i < n) && ((reinterpret_cast<uintptr_t>(p + i) & 7) != 0); i++)
        if (p[i] != 0)
            return false;
    for (; i + 32 <= n; i += 32)
    {
        uint64_t a, b, c, d;
        memcpy(&a, p + i, 8); memcpy(&b, p + i + 8, 8); memcpy(&c, p + i + 16, 8); memcpy(&d, p + i + 24, 8);
        if ((a | b | c | d) != 0)
            return false;
    }
    for (; i < n; i++)
        if (p[i] != 0)
            return false;
    return true;
}

static const ZeroImage *zero_image_of(const struct SaveStructDesc *desc, uint32_t elem, enum SaveMode mode, int depth)
{
    if (zero_images == nullptr)
        zero_images = new std::vector<std::unique_ptr<ZeroImage>>();
    for (const auto &zp : *zero_images)
        if ((zp->desc == desc) && (zp->mode == static_cast<int>(mode)))
            return zp->ok ? zp.get() : nullptr;
    auto holder = std::make_unique<ZeroImage>();
    ZeroImage &z = *holder;
    z.desc = desc;
    z.mode = static_cast<int>(mode);
    z.ok = false;
    std::vector<uint8_t> zeros(std::max<uint32_t>(elem, desc->size), 0);
    struct SaveBuffer tmp;
    memset(&tmp, 0, sizeof(tmp));
    struct SaveError e;
    if ((encode_struct(&tmp, desc, zeros.data(), mode, &e, depth) == SVR_Ok) && !tmp.has_raw_bytes)
    {
        z.bytes.assign(tmp.data, tmp.data + tmp.len);
        z.ok = true;
    }
    save_buf_free(&tmp);
    const ZeroImage *made = z.ok ? holder.get() : nullptr;
    zero_images->push_back(std::move(holder));
    return made;
}

static enum SaveResult encode_struct(struct SaveBuffer *out, const struct SaveStructDesc *desc, const uint8_t *src,
    enum SaveMode mode, struct SaveError *err, int depth)
{
    if (depth > SAVE_MAX_DEPTH)
        return save_fail(err, SVR_Unsupported, "%s: structs nest too deeply", desc->name);
    for (uint32_t field_index = 0; field_index < desc->field_count; field_index++)
    {
        const struct SaveFieldDesc *field = &desc->fields[field_index];
        if (!save_field_included(field, mode))
            continue;
        const uint32_t count = save_field_count(field);
        const uint32_t elem = field->mem_size / count;
        const uint8_t *bytes = src + field->offset;
        if (field->flags & SVF_RAW)
            note_raw_bytes(out, bytes, field->mem_size);
        if (field->stored_type == SV_UNION)
        {
            const enum SaveResult result = encode_union(out, desc, field, src, mode, err, depth);
            if (result != SVR_Ok)
                return result;
            continue;
        }
        if (field->stored_type == SV_STRUCT)
        {
            const ZeroImage *zi = ((count >= 4) && zero_fast_on() && !zero_walk_forced) ? zero_image_of(field->sub, elem, mode, depth + 1) : nullptr;
            for (uint32_t index = 0; index < count; index++)
            {
                const uint8_t *eb = bytes + (static_cast<size_t>(index) * elem);
                if ((zi != nullptr) && bytes_all_zero(eb, elem))
                {
                    if (save_buf_append(out, zi->bytes.data(), static_cast<uint32_t>(zi->bytes.size())) != 0)
                        return save_fail(err, SVR_NoMemory, "out of memory");
                    continue;
                }
                const enum SaveResult result = encode_struct(out, field->sub, eb, mode, err, depth + 1);
                if (result != SVR_Ok)
                    return result;
            }
            continue;
        }
        if (save_fast_copy_ok(field->stored_type, elem))
        {
            if (save_buf_append(out, bytes, count * elem) != 0)
                return save_fail(err, SVR_NoMemory, "out of memory");
            continue;
        }
        const uint32_t stored_size = save_stored_size(field->stored_type);
        for (uint32_t index = 0; index < count; index++)
        {
            struct SaveValue value;
            std::array<uint8_t, 8> tmp;
            if (save_value_load_mem(bytes + (static_cast<size_t>(index) * elem), elem, field->stored_type, &value) != 0)
                return save_fail(err, SVR_Unsupported, "%s.%s: can't read a %u byte member as %s", desc->name, field->name,
                    static_cast<unsigned>(elem), save_stored_name(field->stored_type));
            if (save_value_store_le(tmp.data(), field->stored_type, &value, (field->flags & SVF_CLAMP) != 0) < 0)
                return save_fail(err, SVR_Unsupported, "%s.%s: value doesn't fit %s", desc->name, field->name,
                    save_stored_name(field->stored_type));
            if (save_buf_append(out, tmp.data(), stored_size) != 0)
                return save_fail(err, SVR_NoMemory, "out of memory");
        }
    }
    return SVR_Ok;
}

enum SaveResult save_encode_record(struct SaveBuffer *out, const struct SaveStructDesc *desc, const void *src,
    enum SaveMode mode, struct SaveError *err)
{
    return encode_struct(out, desc, static_cast<const uint8_t *>(src), mode, err, 0);
}

enum SaveResult save_encode_record_walk(struct SaveBuffer *out, const struct SaveStructDesc *desc, const void *src,
    enum SaveMode mode, struct SaveError *err)
{
    zero_walk_forced = true;
    const enum SaveResult result = encode_struct(out, desc, static_cast<const uint8_t *>(src), mode, err, 0);
    zero_walk_forced = false;
    return result;
}

/******************************************************************************/
/** @file save_ksc.cpp
 *     KSC: a record coded by the shape its schema gives it (research prototype, 2026-11-28).
 * @par
 *     A Save's GAME record is 124 MB, 99% zero bytes, because KeeperFX's tables are sized for 104 players, 32767
 *     things and 8192 creature controls; zlib level 1 made it 0.9 MB in about 150 ms. KSC walks the record with the
 *     file's own schema (the SCHM chunk, so it follows every build's fields and needs no table of its own):
 *       - every leaf of the schema tree (a struct's field) gets its own stream, and so do union codes, union tails
 *         and raw union members: like values sit together (all things' models, all map blocks' columns);
 *       - in an array of structs, an element whose bytes are all zero is left out and marked in a presence bitmap;
 *       - a struct without unions is copied field by field with a flat plan (offset, length, stream), a field's
 *         values gathered from all the array's elements at once;
 *       - streams of 2-, 4- and 8-byte integers are split into byte planes (low bytes, then the next ...);
 *       - the streams, back to back, are one zstd frame.
 *     Decoding walks the same schema and takes each value from its stream. Measured on three real saves
 *     (2026-11-28): GAME 105-144 KB against zstd's 165-214 KB and zlib's 824-884 KB.
 * @par Body
 *     "KSC1", u8 root name length, root name, u32 record length, u32 stream count, per stream a varint length and,
 *     when the length is not 0, a u8 transform (bit 1: byte planes), then one zstd frame holding the streams.
 */
/******************************************************************************/
#include "save_ksc.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "../../../../deps/zstd/zstd.h"

/******************************************************************************/
namespace {

enum KscType { KT_U8 = 1, KT_I8, KT_U16, KT_I16, KT_U32, KT_I32, KT_U64, KT_I64, KT_F32, KT_F64, KT_BOOL, KT_STR,
    KT_STRUCT, KT_UNION };
const uint8_t ksc_type_size[15] = {0, 1, 1, 2, 2, 4, 4, 8, 8, 4, 8, 1, 1, 0, 0};
enum { SK_DATA, SK_PRES, SK_UCODE, SK_TAILH, SK_TAILB, SK_RAW, SK_N };
const int KSC_MAX_DEPTH = 24;
const uint32_t KSC_MAX_STREAMS = 1u << 20;

struct KMember { std::string_view name; std::string_view sub; uint32_t raw; int sub_i; };
struct KField {
    std::string_view name; uint8_t type; uint64_t count; std::string_view sub; int sub_i;
    std::vector<KMember> mem;
};
struct KStruct { std::string_view name; std::vector<KField> f; int fixed; uint64_t fixed_len; uint64_t zero_len; int base; };
struct KLeaf { uint32_t off, len; int stream; };

struct KSchema {
    std::vector<KStruct> s;
    std::vector<std::vector<KLeaf>> plan;
    int nslots = 0;
};

struct Rd {
    const uint8_t *p, *end;
    bool bad;
    bool take(void *o, size_t n) { if ((size_t)(end - p) < n) { bad = true; return false; } memcpy(o, p, n); p += n; return true; }
    std::string_view str()
    {
        uint16_t l = 0;
        if (!take(&l, 2) || ((size_t)(end - p) < l)) { bad = true; return std::string_view(); }
        std::string_view v(reinterpret_cast<const char *>(p), l);
        p += l;
        return v;
    }
};

int find_struct(const KSchema &sc, std::string_view n)
{
    for (size_t i = 0; i < sc.s.size(); i++)
        if (sc.s[i].name == n)
            return (int)i;
    return -1;
}

/** Fixed length (no union inside) and the length of an all-zero element; false when the schema loops or is too deep. */
bool settle(KSchema &sc, int si, int depth)
{
    KStruct &s = sc.s[si];
    if (s.fixed >= 0)
        return true;
    if (depth > KSC_MAX_DEPTH)
        return false;
    uint64_t len = 0, zl = 0;
    int fixed = 1;
    for (const KField &f : s.f)
    {
        if (f.type == KT_UNION) { fixed = 0; zl += 1; continue; }
        if (f.type == KT_STRUCT)
        {
            if (!settle(sc, f.sub_i, depth + 1))
                return false;
            const KStruct &sub = sc.s[f.sub_i];
            if (!sub.fixed) fixed = 0;
            len += f.count * sub.fixed_len;
            zl += f.count * sub.zero_len;
        } else
        {
            len += f.count * ksc_type_size[f.type];
            zl += f.count * ksc_type_size[f.type];
        }
        if ((zl > SAVE_MAX_RAW_LEN) || (len > SAVE_MAX_RAW_LEN))
            return false;
    }
    s.fixed = fixed;
    s.fixed_len = fixed ? len : 0;
    s.zero_len = zl;
    return true;
}

bool plan_add(KSchema &sc, std::vector<KLeaf> &pl, int si, uint32_t &off, int depth)
{
    if ((depth > KSC_MAX_DEPTH) || (pl.size() > 4096))
        return false;
    const KStruct &s = sc.s[si];
    for (size_t i = 0; i < s.f.size(); i++)
    {
        const KField &f = s.f[i];
        if (f.type == KT_STRUCT)
        {
            for (uint64_t e = 0; e < f.count; e++)
                if (!plan_add(sc, pl, f.sub_i, off, depth + 1))
                    return false;
            continue;
        }
        const uint32_t len = (uint32_t)(f.count * ksc_type_size[f.type]);
        const int st = s.base + (int)i * SK_N + SK_DATA;
        if (!pl.empty() && (pl.back().stream == st) && (pl.back().off + pl.back().len == off))
            pl.back().len += len; /* (the elements of one array field side by side) */
        else
            pl.push_back(KLeaf{off, len, st});
        off += len;
    }
    return true;
}

bool parse_schema(const uint8_t *data, uint32_t len, KSchema &sc)
{
    Rd r{data, data + len, false};
    uint32_t n = 0;
    r.take(&n, 4);
    if (r.bad || (n > len / 3))
        return false;
    sc.s.resize(n);
    for (uint32_t i = 0; (i < n) && !r.bad; i++)
    {
        KStruct &s = sc.s[i];
        s.name = r.str();
        uint16_t nf = 0;
        r.take(&nf, 2);
        s.fixed = -1;
        s.f.resize(nf);
        for (uint16_t k = 0; (k < nf) && !r.bad; k++)
        {
            KField &f = s.f[k];
            f.name = r.str();
            uint8_t flags = 0, ndim = 0;
            r.take(&f.type, 1); r.take(&flags, 1); r.take(&ndim, 1);
            if ((f.type < KT_U8) || (f.type > KT_UNION) || (ndim > 3)) { r.bad = true; break; }
            f.count = 1;
            for (int d = 0; d < ndim; d++)
            {
                uint32_t dim = 0;
                r.take(&dim, 4);
                f.count *= dim;
                if (f.count > SAVE_MAX_RAW_LEN) { r.bad = true; break; }
            }
            f.sub = r.str();
            if (f.type == KT_UNION)
            {
                uint16_t m = 0;
                r.take(&m, 2);
                if ((m > 254) || (ndim != 0)) { r.bad = true; break; }
                f.mem.resize(m);
                for (KMember &mb : f.mem) { mb.name = r.str(); mb.sub = r.str(); r.take(&mb.raw, 4); }
            }
        }
    }
    if (r.bad)
        return false;
    int slots = 0;
    for (KStruct &s : sc.s)
    {
        s.base = slots;
        slots += (int)s.f.size() * SK_N;
        if ((uint32_t)slots > KSC_MAX_STREAMS)
            return false;
        for (KField &f : s.f)
        {
            f.sub_i = f.sub.empty() ? -1 : find_struct(sc, f.sub);
            if ((f.type == KT_STRUCT) && (f.sub_i < 0))
                return false;
            for (KMember &m : f.mem)
            {
                m.sub_i = m.sub.empty() ? -1 : find_struct(sc, m.sub);
                if (!m.sub.empty() && (m.sub_i < 0))
                    return false;
            }
        }
    }
    sc.nslots = slots;
    for (size_t i = 0; i < sc.s.size(); i++)
        if (!settle(sc, (int)i, 0))
            return false;
    for (size_t i = 0; i < sc.s.size(); i++)
        for (const KField &f : sc.s[i].f)
            for (const KMember &m : f.mem)
                if ((m.sub_i >= 0) && !settle(sc, m.sub_i, 0))
                    return false;
    sc.plan.resize(sc.s.size());
    for (size_t i = 0; i < sc.s.size(); i++)
    {
        if (!sc.s[i].fixed)
            continue;
        uint32_t off = 0;
        if (!plan_add(sc, sc.plan[i], (int)i, off, 0))
            sc.plan[i].clear(); /* (too big to flatten: walked field by field) */
    }
    return true;
}

struct Stream {
    std::vector<uint8_t> b;
    size_t rpos = 0;
    uint8_t bits = 0;
    uint8_t esize = 1;
    const uint8_t *in = nullptr; /* decode: the stream's bytes */
    size_t in_len = 0;
};

struct Coder {
    const KSchema &sc;
    std::vector<Stream> st;
    const uint8_t *end;        /* encode: end of the record */
    uint8_t *out_begin, *out_end; /* decode: the record being made */
    bool bad = false;
    explicit Coder(const KSchema &s) : sc(s), st((size_t)s.nslots), end(nullptr), out_begin(nullptr), out_end(nullptr)
    {
        for (const KStruct &ks : sc.s)
            for (size_t i = 0; i < ks.f.size(); i++)
            {
                const KField &f = ks.f[i];
                const uint8_t es = ((f.type >= KT_U16) && (f.type <= KT_I64)) ? ksc_type_size[f.type] : 1;
                st[(size_t)(ks.base + (int)i * SK_N + SK_DATA)].esize = es;
                st[(size_t)(ks.base + (int)i * SK_N + SK_TAILH)].esize = 2;
            }
    }
    Stream &slot(int si, size_t fi, int kind) { return st[(size_t)(sc.s[si].base + (int)fi * SK_N + kind)]; }
};

inline bool all_zero(const uint8_t *p, size_t n)
{
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
    {
        uint64_t a, b, c, d;
        memcpy(&a, p + i, 8); memcpy(&b, p + i + 8, 8); memcpy(&c, p + i + 16, 8); memcpy(&d, p + i + 24, 8);
        if (a | b | c | d)
            return false;
    }
    for (; i < n; i++)
        if (p[i])
            return false;
    return true;
}

inline void put(Stream &s, const uint8_t *p, size_t n) { s.b.insert(s.b.end(), p, p + n); }
inline void pres_put(Stream &s, bool bit)
{
    if ((s.bits == 0) || (s.bits == 8)) { s.b.push_back(0); s.bits = 0; }
    if (bit) s.b.back() |= (uint8_t)(1u << s.bits);
    s.bits++;
}

/* ----- encode ----- */
/** Bytes one encoded struct takes at p (0: it runs past the record). */
size_t walk(Coder &c, int si, const uint8_t *p, int depth);
size_t walk_field(Coder &c, const KField &f, const uint8_t *p, int depth)
{
    if (f.type == KT_STRUCT)
    {
        const KStruct &sub = c.sc.s[f.sub_i];
        if (sub.fixed) return (size_t)(f.count * sub.fixed_len);
        size_t o = 0;
        for (uint64_t e = 0; (e < f.count) && !c.bad; e++) o += walk(c, f.sub_i, p + o, depth + 1);
        return o;
    }
    if (f.type == KT_UNION)
    {
        if (p >= c.end) { c.bad = true; return 0; }
        const uint8_t code = p[0];
        size_t o = 1;
        if (code == 0) return o;
        if (code > f.mem.size()) { c.bad = true; return 0; }
        const KMember &m = f.mem[code - 1];
        if (m.sub_i >= 0)
        {
            o += walk(c, m.sub_i, p + o, depth + 1);
            if ((size_t)(c.end - p) < o + 2) { c.bad = true; return 0; }
            uint16_t tl; memcpy(&tl, p + o, 2); o += 2;
            if (tl) o += 2 + tl;
        } else
            o += m.raw;
        return o;
    }
    return (size_t)(f.count * ksc_type_size[f.type]);
}
size_t walk(Coder &c, int si, const uint8_t *p, int depth)
{
    const KStruct &s = c.sc.s[si];
    if (s.fixed) return (size_t)s.fixed_len;
    if (depth > KSC_MAX_DEPTH) { c.bad = true; return 0; }
    size_t o = 0;
    for (const KField &f : s.f) { o += walk_field(c, f, p + o, depth); if (c.bad) return 0; }
    return o;
}

void gather(Coder &c, int si, const uint8_t *base, size_t stride, const uint32_t *idx, size_t n)
{
    for (const KLeaf &lf : c.sc.plan[si])
    {
        Stream &s = c.st[(size_t)lf.stream];
        const size_t at = s.b.size();
        s.b.resize(at + n * lf.len);
        uint8_t *d = s.b.data() + at;
        const uint8_t *src = base + lf.off;
        for (size_t e = 0; e < n; e++)
            memcpy(d + e * lf.len, src + (idx ? idx[e] : e) * stride, lf.len);
    }
}

void enc_struct(Coder &c, int si, const uint8_t *p, int depth);
size_t enc_field(Coder &c, int si, size_t fi, const uint8_t *p, int depth)
{
    const KField &f = c.sc.s[si].f[fi];
    if (f.type == KT_STRUCT)
    {
        const KStruct &sub = c.sc.s[f.sub_i];
        if (f.count == 1)
        {
            const size_t l = walk(c, f.sub_i, p, depth + 1);
            enc_struct(c, f.sub_i, p, depth + 1);
            return l;
        }
        Stream &pres = c.slot(si, fi, SK_PRES);
        if (sub.fixed && !c.sc.plan[f.sub_i].empty())
        {
            /* the elements that aren't all zero, then field by field */
            std::vector<uint32_t> idx;
            for (uint64_t e = 0; e < f.count; e++)
            {
                const bool nz = !all_zero(p + e * sub.fixed_len, (size_t)sub.fixed_len);
                pres_put(pres, nz);
                if (nz) idx.push_back((uint32_t)e);
            }
            gather(c, f.sub_i, p, (size_t)sub.fixed_len, idx.data(), idx.size());
            return (size_t)(f.count * sub.fixed_len);
        }
        size_t o = 0;
        for (uint64_t e = 0; (e < f.count) && !c.bad; e++)
        {
            const size_t l = walk(c, f.sub_i, p + o, depth + 1);
            const bool nz = !all_zero(p + o, l);
            pres_put(pres, nz);
            if (nz) enc_struct(c, f.sub_i, p + o, depth + 1);
            o += l;
        }
        return o;
    }
    if (f.type == KT_UNION)
    {
        const uint8_t code = p[0];
        put(c.slot(si, fi, SK_UCODE), p, 1);
        size_t o = 1;
        if (code == 0) return o;
        const KMember &m = f.mem[code - 1];
        if (m.sub_i >= 0)
        {
            const size_t l = walk(c, m.sub_i, p + o, depth + 1);
            enc_struct(c, m.sub_i, p + o, depth + 1);
            o += l;
            uint16_t tl; memcpy(&tl, p + o, 2);
            put(c.slot(si, fi, SK_TAILH), p + o, 2); o += 2;
            if (tl)
            {
                put(c.slot(si, fi, SK_TAILH), p + o, 2); o += 2;
                put(c.slot(si, fi, SK_TAILB), p + o, tl); o += tl;
            }
        } else
        {
            put(c.slot(si, fi, SK_RAW), p + o, m.raw);
            o += m.raw;
        }
        return o;
    }
    const size_t n = (size_t)(f.count * ksc_type_size[f.type]);
    put(c.slot(si, fi, SK_DATA), p, n);
    return n;
}
void enc_struct(Coder &c, int si, const uint8_t *p, int depth)
{
    const KStruct &s = c.sc.s[si];
    if (s.fixed && !c.sc.plan[si].empty())
    {
        gather(c, si, p, 0, nullptr, 1);
        return;
    }
    size_t o = 0;
    for (size_t i = 0; (i < s.f.size()) && !c.bad; i++) o += enc_field(c, si, i, p + o, depth);
}

void planes(uint8_t *p, size_t n, int es, bool undo, std::vector<uint8_t> &tmp)
{
    const size_t m = n / es;
    tmp.resize(n);
    uint8_t *t = tmp.data();
    if (!undo)
        for (size_t i = 0; i < m; i++) for (int b = 0; b < es; b++) t[b * m + i] = p[i * es + b];
    else
        for (size_t i = 0; i < m; i++) for (int b = 0; b < es; b++) t[i * es + b] = p[b * m + i];
    memcpy(p, t, m * es);
}

void put_varint(std::vector<uint8_t> &o, uint64_t v)
{
    while (v >= 0x80) { o.push_back((uint8_t)(v | 0x80)); v >>= 7; }
    o.push_back((uint8_t)v);
}
bool get_varint(Rd &r, uint64_t &v)
{
    v = 0;
    for (int sh = 0; sh < 64; sh += 7)
    {
        uint8_t b;
        if (!r.take(&b, 1)) return false;
        v |= (uint64_t)(b & 0x7F) << sh;
        if (!(b & 0x80)) return true;
    }
    r.bad = true;
    return false;
}

/* ----- decode ----- */
inline void take(Coder &c, Stream &s, uint8_t *dst, size_t n)
{
    if ((s.in_len - s.rpos < n) || (dst < c.out_begin) || ((size_t)(c.out_end - dst) < n)) { c.bad = true; return; }
    memcpy(dst, s.in + s.rpos, n);
    s.rpos += n;
}
inline bool pres_get(Coder &c, Stream &s)
{
    const size_t byte = s.rpos >> 3;
    if (byte >= s.in_len) { c.bad = true; return false; }
    const int bit = (int)(s.rpos & 7);
    s.rpos++;
    return ((s.in[byte] >> bit) & 1) != 0;
}
bool room(Coder &c, uint8_t *o, uint64_t n)
{
    if ((o < c.out_begin) || ((uint64_t)(c.out_end - o) < n)) { c.bad = true; return false; }
    return true;
}
void scatter(Coder &c, int si, uint8_t *base, size_t stride, const uint32_t *idx, size_t n)
{
    for (const KLeaf &lf : c.sc.plan[si])
    {
        Stream &s = c.st[(size_t)lf.stream];
        if (s.in_len - s.rpos < n * lf.len) { c.bad = true; return; }
        const uint8_t *src = s.in + s.rpos;
        uint8_t *d = base + lf.off;
        for (size_t e = 0; e < n; e++)
            memcpy(d + (idx ? idx[e] : e) * stride, src + e * lf.len, lf.len);
        s.rpos += n * lf.len;
    }
}
size_t dec_struct(Coder &c, int si, uint8_t *o, int depth);
size_t dec_field(Coder &c, int si, size_t fi, uint8_t *o, int depth)
{
    const KField &f = c.sc.s[si].f[fi];
    if (f.type == KT_STRUCT)
    {
        if (f.count == 1) return dec_struct(c, f.sub_i, o, depth + 1);
        const KStruct &sub = c.sc.s[f.sub_i];
        Stream &pres = c.slot(si, fi, SK_PRES);
        if (sub.fixed && !c.sc.plan[f.sub_i].empty())
        {
            if (!room(c, o, f.count * sub.fixed_len)) return 0;
            std::vector<uint32_t> idx;
            for (uint64_t e = 0; (e < f.count) && !c.bad; e++)
            {
                if (pres_get(c, pres)) idx.push_back((uint32_t)e);
                else memset(o + e * sub.fixed_len, 0, (size_t)sub.fixed_len);
            }
            if (!c.bad) scatter(c, f.sub_i, o, (size_t)sub.fixed_len, idx.data(), idx.size());
            return (size_t)(f.count * sub.fixed_len);
        }
        size_t off = 0;
        for (uint64_t e = 0; (e < f.count) && !c.bad; e++)
        {
            if (pres_get(c, pres)) off += dec_struct(c, f.sub_i, o + off, depth + 1);
            else if (room(c, o + off, sub.zero_len)) { memset(o + off, 0, (size_t)sub.zero_len); off += (size_t)sub.zero_len; }
        }
        return off;
    }
    if (f.type == KT_UNION)
    {
        uint8_t code = 0;
        if (!room(c, o, 1)) return 0;
        take(c, c.slot(si, fi, SK_UCODE), o, 1);
        code = o[0];
        size_t off = 1;
        if ((code == 0) || c.bad) return off;
        if (code > f.mem.size()) { c.bad = true; return off; }
        const KMember &m = f.mem[code - 1];
        if (m.sub_i >= 0)
        {
            off += dec_struct(c, m.sub_i, o + off, depth + 1);
            Stream &th = c.slot(si, fi, SK_TAILH);
            take(c, th, o + off, 2);
            if (c.bad) return off;
            uint16_t tl; memcpy(&tl, o + off, 2); off += 2;
            if (tl)
            {
                take(c, th, o + off, 2); off += 2;
                take(c, c.slot(si, fi, SK_TAILB), o + off, tl); off += tl;
            }
        } else
        {
            take(c, c.slot(si, fi, SK_RAW), o + off, m.raw);
            off += m.raw;
        }
        return off;
    }
    const size_t n = (size_t)(f.count * ksc_type_size[f.type]);
    take(c, c.slot(si, fi, SK_DATA), o, n);
    return n;
}
size_t dec_struct(Coder &c, int si, uint8_t *o, int depth)
{
    const KStruct &s = c.sc.s[si];
    if (depth > KSC_MAX_DEPTH) { c.bad = true; return 0; }
    if (s.fixed && !c.sc.plan[si].empty())
    {
        if (room(c, o, s.fixed_len)) scatter(c, si, o, 0, nullptr, 1);
        return (size_t)s.fixed_len;
    }
    size_t off = 0;
    for (size_t i = 0; (i < s.f.size()) && !c.bad; i++) off += dec_field(c, si, i, o + off, depth);
    return off;
}

} // namespace

/******************************************************************************/
extern "C" enum SaveResult save_ksc_encode(const uint8_t *schema, uint32_t schema_len, const char *root,
    const uint8_t *rec, uint32_t rec_len, struct SaveBuffer *out, struct SaveError *err)
{
    KSchema sc;
    if (!parse_schema(schema, schema_len, sc))
        return save_fail(err, SVR_Damaged, "KSC: the schema can't be read");
    const int ri = find_struct(sc, root);
    if (ri < 0)
        return save_fail(err, SVR_Damaged, "KSC: no struct %s in the schema", root);
    Coder c(sc);
    c.end = rec + rec_len;
    const size_t used = walk(c, ri, rec, 0);
    if (c.bad || (used != rec_len))
        return save_fail(err, SVR_Damaged, "KSC: the record doesn't match its schema");
    enc_struct(c, ri, rec, 0);
    if (c.bad)
        return save_fail(err, SVR_Damaged, "KSC: the record doesn't match its schema");
    std::vector<uint8_t> head, cat, tmp;
    const char magic[4] = {'K', 'S', 'C', '1'};
    head.insert(head.end(), magic, magic + 4);
    const size_t rn = strlen(root) < 255 ? strlen(root) : 255;
    head.push_back((uint8_t)rn);
    head.insert(head.end(), root, root + rn);
    for (int sh = 0; sh < 32; sh += 8) head.push_back((uint8_t)(rec_len >> sh));
    for (int sh = 0; sh < 32; sh += 8) head.push_back((uint8_t)((uint32_t)sc.nslots >> sh));
    size_t total = 0;
    for (const Stream &s : c.st) total += s.b.size();
    cat.reserve(total);
    for (Stream &s : c.st)
    {
        put_varint(head, s.b.size());
        if (s.b.empty())
            continue;
        uint8_t x = 0;
        if ((s.esize > 1) && (s.b.size() >= 16) && (s.b.size() % s.esize == 0) && !all_zero(s.b.data(), s.b.size()))
        {
            planes(s.b.data(), s.b.size(), s.esize, false, tmp);
            x |= 2;
        }
        head.push_back(x);
        cat.insert(cat.end(), s.b.begin(), s.b.end());
        std::vector<uint8_t>().swap(s.b);
    }
    const size_t zcap = ZSTD_compressBound(cat.size());
    std::unique_ptr<uint8_t, decltype(&free)> z(static_cast<uint8_t *>(malloc(zcap)), &free); /* (no zero fill) */
    if (!z)
        return save_fail(err, SVR_NoMemory, "out of memory");
    const size_t zl = ZSTD_compress(z.get(), zcap, cat.data(), cat.size(), 1);
    if (ZSTD_isError(zl))
        return save_fail(err, SVR_NoMemory, "KSC: zstd failed");
    if ((head.size() + zl > UINT32_MAX) || (save_buf_append(out, head.data(), (uint32_t)head.size()) != 0) ||
        (save_buf_append(out, z.get(), (uint32_t)zl) != 0))
        return save_fail(err, SVR_NoMemory, "out of memory");
    return SVR_Ok;
}

extern "C" enum SaveResult save_ksc_decode(const uint8_t *schema, uint32_t schema_len, const uint8_t *in,
    uint32_t in_len, uint8_t *rec, uint32_t rec_len, struct SaveError *err)
{
    KSchema sc;
    if (!parse_schema(schema, schema_len, sc))
        return save_fail(err, SVR_Damaged, "KSC: the schema can't be read");
    Rd r{in, in + in_len, false};
    char magic[4] = {0};
    uint8_t rn = 0;
    r.take(magic, 4);
    r.take(&rn, 1);
    if (r.bad || (memcmp(magic, "KSC1", 4) != 0) || ((size_t)(r.end - r.p) < rn))
        return save_fail(err, SVR_Damaged, "KSC: not a KSC body");
    const std::string_view root(reinterpret_cast<const char *>(r.p), rn);
    r.p += rn;
    uint32_t rl = 0, ns = 0;
    r.take(&rl, 4);
    r.take(&ns, 4);
    const int ri = find_struct(sc, root);
    if (r.bad || (ri < 0) || (rl != rec_len) || (ns != (uint32_t)sc.nslots))
        return save_fail(err, SVR_Damaged, "KSC: the body doesn't match the schema");
    Coder c(sc);
    std::vector<uint64_t> lens(ns);
    std::vector<uint8_t> xf(ns, 0);
    uint64_t total = 0;
    for (uint32_t i = 0; (i < ns) && !r.bad; i++)
    {
        get_varint(r, lens[i]);
        if (lens[i] && !r.bad) r.take(&xf[i], 1);
        total += lens[i];
        if (total > SAVE_MAX_RAW_LEN) r.bad = true;
    }
    if (r.bad)
        return save_fail(err, SVR_Damaged, "KSC: the stream table is damaged");
    std::unique_ptr<uint8_t, decltype(&free)> catp(static_cast<uint8_t *>(malloc((size_t)total + 1)), &free); /* (no zero fill) */
    std::vector<uint8_t> tmp;
    if (!catp)
        return save_fail(err, SVR_NoMemory, "out of memory");
    uint8_t *const cat = catp.get();
    const size_t got = ZSTD_decompress(cat, (size_t)total, r.p, (size_t)(r.end - r.p));
    if (ZSTD_isError(got) || (got != total))
        return save_fail(err, SVR_Damaged, "KSC: the streams can't be decompressed");
    size_t at = 0;
    for (uint32_t i = 0; i < ns; i++)
    {
        Stream &s = c.st[i];
        s.in = cat + at;
        s.in_len = (size_t)lens[i];
        if ((xf[i] & 2) && (s.esize > 1) && (s.in_len % s.esize == 0))
            planes(cat + at, s.in_len, s.esize, true, tmp);
        at += s.in_len;
    }
    c.out_begin = rec;
    c.out_end = rec + rec_len;
    const size_t made = dec_struct(c, ri, rec, 0);
    if (c.bad || (made != rec_len))
        return save_fail(err, SVR_Damaged, "KSC: the record can't be rebuilt");
    return SVR_Ok;
}

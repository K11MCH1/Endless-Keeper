// A small JSON reader (Keeper Server, and the launcher for GitHub release lists).
#pragma once
#include <string>
#include <vector>
#include <utility>
#include <cstdlib>

struct JVal {
    enum Type { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;

    const JVal *get(const char *k) const
    {
        for (const auto &p : o)
            if (p.first == k)
                return &p.second;
        return nullptr;
    }
    double num(const char *k, double d = 0) const
    {
        const JVal *v = get(k);
        return (v != nullptr && v->t == Num) ? v->n : d;
    }
    long long inum(const char *k, long long d = 0) const
    {
        const JVal *v = get(k);
        return (v != nullptr && v->t == Num) ? (long long)v->n : d;
    }
    std::string str(const char *k, const char *d = "") const
    {
        const JVal *v = get(k);
        return (v != nullptr && v->t == Str) ? v->s : std::string(d);
    }
    bool boolean(const char *k, bool d = false) const
    {
        const JVal *v = get(k);
        return (v != nullptr && v->t == Bool) ? v->b : d;
    }
};

class JParser {
public:
    JParser(const char *p, const char *e) : p_(p), e_(e) {}
    bool parse(JVal &out)
    {
        ok_ = true;
        value(out, 0);
        ws();
        return ok_;
    }

private:
    const char *p_, *e_;
    bool ok_ = true;

    void ws()
    {
        while (p_ < e_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n'))
            p_++;
    }
    bool lit(const char *s)
    {
        const char *q = p_;
        while (*s != '\0')
        {
            if (q >= e_ || *q != *s)
                return false;
            q++;
            s++;
        }
        p_ = q;
        return true;
    }
    void string(std::string &out)
    {
        p_++; // the opening quote
        while (p_ < e_ && *p_ != '"')
        {
            char c = *p_++;
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (p_ >= e_)
                break;
            c = *p_++;
            switch (c)
            {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u':
            {
                unsigned v = 0;
                for (int i = 0; i < 4 && p_ < e_; i++)
                {
                    const char h = *p_++;
                    v <<= 4;
                    if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
                }
                /* as UTF-8 (a surrogate half, of a character beyond the 16 bits, becomes a space) */
                if (v < 0x80) out += (v >= 0x20) ? (char)v : ' ';
                else if (v < 0x800) { out += (char)(0xC0 | (v >> 6)); out += (char)(0x80 | (v & 0x3F)); }
                else if (v >= 0xD800 && v <= 0xDFFF) out += ' ';
                else { out += (char)(0xE0 | (v >> 12)); out += (char)(0x80 | ((v >> 6) & 0x3F)); out += (char)(0x80 | (v & 0x3F)); }
                break;
            }
            default: out += c; break;
            }
        }
        if (p_ < e_)
            p_++; // the closing quote
        else
            ok_ = false;
    }
    void value(JVal &v, int depth)
    {
        ws();
        if (p_ >= e_ || depth > 32)
        {
            ok_ = false;
            return;
        }
        const char c = *p_;
        if (c == '{')
        {
            v.t = JVal::Obj;
            p_++;
            ws();
            if (p_ < e_ && *p_ == '}')
            {
                p_++;
                return;
            }
            while (ok_ && p_ < e_)
            {
                ws();
                if (p_ >= e_ || *p_ != '"')
                {
                    ok_ = false;
                    return;
                }
                std::string key;
                string(key);
                ws();
                if (p_ >= e_ || *p_ != ':')
                {
                    ok_ = false;
                    return;
                }
                p_++;
                v.o.emplace_back(std::move(key), JVal());
                value(v.o.back().second, depth + 1);
                ws();
                if (p_ < e_ && *p_ == ',')
                {
                    p_++;
                    continue;
                }
                if (p_ < e_ && *p_ == '}')
                {
                    p_++;
                    return;
                }
                ok_ = false;
            }
            return;
        }
        if (c == '[')
        {
            v.t = JVal::Arr;
            p_++;
            ws();
            if (p_ < e_ && *p_ == ']')
            {
                p_++;
                return;
            }
            while (ok_ && p_ < e_)
            {
                v.a.emplace_back();
                value(v.a.back(), depth + 1);
                ws();
                if (p_ < e_ && *p_ == ',')
                {
                    p_++;
                    continue;
                }
                if (p_ < e_ && *p_ == ']')
                {
                    p_++;
                    return;
                }
                ok_ = false;
            }
            return;
        }
        if (c == '"')
        {
            v.t = JVal::Str;
            string(v.s);
            return;
        }
        if (lit("true"))
        {
            v.t = JVal::Bool;
            v.b = true;
            return;
        }
        if (lit("false"))
        {
            v.t = JVal::Bool;
            return;
        }
        if (lit("null"))
            return;
        char *end = nullptr;
        std::string num(p_, (size_t)((e_ - p_) < 64 ? (e_ - p_) : 64));
        v.n = std::strtod(num.c_str(), &end);
        if (end == num.c_str())
        {
            ok_ = false;
            return;
        }
        v.t = JVal::Num;
        p_ += (end - num.c_str());
    }
};

inline bool json_parse(const std::string &text, JVal &out)
{
    JParser jp(text.data(), text.data() + text.size());
    return jp.parse(out) && ((out.t == JVal::Obj) || (out.t == JVal::Arr));
}

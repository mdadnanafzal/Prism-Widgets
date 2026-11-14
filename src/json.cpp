// Minimal JSON parser/writer (wide strings). Tolerates // comments and trailing commas.
#include "common.h"

const JVal* JVal::get(const wchar_t* key) const {
    if (type != Obj) return nullptr;
    for (auto& kv : o) if (kv.first == key) return &kv.second;
    return nullptr;
}
JVal& JVal::set(const wchar_t* key) {
    if (type != Obj) { type = Obj; o.clear(); }
    for (auto& kv : o) if (kv.first == key) return kv.second;
    o.emplace_back(key, JVal());
    return o.back().second;
}
double JVal::num(const wchar_t* key, double def) const {
    const JVal* v = get(key);
    if (!v) return def;
    if (v->type == Num) return v->n;
    if (v->type == Str && !v->s.empty()) return _wtof(v->s.c_str());
    return def;
}
bool JVal::boolean(const wchar_t* key, bool def) const {
    const JVal* v = get(key);
    if (!v) return def;
    if (v->type == Bool) return v->b;
    if (v->type == Num) return v->n != 0;
    return def;
}
std::wstring JVal::str(const wchar_t* key, const wchar_t* def) const {
    const JVal* v = get(key);
    if (v && v->type == Str) return v->s;
    if (v && v->type == Num) { wchar_t b[64]; swprintf(b, 64, L"%g", v->n); return b; }
    return def ? def : L"";
}

namespace {
struct Parser {
    const wchar_t* p; const wchar_t* end; std::wstring err;
    void ws() {
        for (;;) {
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
            if (p + 1 < end && p[0] == '/' && p[1] == '/') { while (p < end && *p != '\n') p++; continue; }
            if (p + 1 < end && p[0] == '/' && p[1] == '*') {
                p += 2; while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++; p += 2; continue;
            }
            break;
        }
    }
    bool fail(const wchar_t* m) { if (err.empty()) err = m; return false; }
    bool str(std::wstring& out) {
        if (*p != '"') return fail(L"expected string");
        p++;
        while (p < end && *p != '"') {
            if (*p == '\\') {
                p++; if (p >= end) break;
                switch (*p) {
                case 'n': out += L'\n'; break; case 't': out += L'\t'; break;
                case 'r': out += L'\r'; break; case 'b': out += L'\b'; break;
                case 'f': out += L'\f'; break;
                case 'u': {
                    if (end - p < 5) return fail(L"bad escape");
                    wchar_t h[5] = { p[1], p[2], p[3], p[4], 0 };
                    out += (wchar_t)wcstoul(h, nullptr, 16); p += 4; break;
                }
                default: out += *p;
                }
                p++;
            } else out += *p++;
        }
        if (p >= end) return fail(L"unterminated string");
        p++;
        return true;
    }
    bool val(JVal& v, int depth) {
        if (depth > 64) return fail(L"too deep");
        ws(); if (p >= end) return fail(L"unexpected end");
        if (*p == '{') {
            v.type = JVal::Obj; p++;
            for (;;) {
                ws(); if (p < end && *p == '}') { p++; return true; }
                std::wstring k; if (!str(k)) return false;
                ws(); if (p >= end || *p != ':') return fail(L"expected ':'"); p++;
                JVal c; if (!val(c, depth + 1)) return false;
                v.o.emplace_back(std::move(k), std::move(c));
                ws(); if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == '}') { p++; return true; }
                return fail(L"expected ',' or '}'");
            }
        }
        if (*p == '[') {
            v.type = JVal::Arr; p++;
            for (;;) {
                ws(); if (p < end && *p == ']') { p++; return true; }
                JVal c; if (!val(c, depth + 1)) return false;
                v.a.push_back(std::move(c));
                ws(); if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == ']') { p++; return true; }
                return fail(L"expected ',' or ']'");
            }
        }
        if (*p == '"') { v.type = JVal::Str; return str(v.s); }
        if (end - p >= 4 && !wcsncmp(p, L"true", 4)) { v.type = JVal::Bool; v.b = true; p += 4; return true; }
        if (end - p >= 5 && !wcsncmp(p, L"false", 5)) { v.type = JVal::Bool; v.b = false; p += 5; return true; }
        if (end - p >= 4 && !wcsncmp(p, L"null", 4)) { v.type = JVal::Null; p += 4; return true; }
        wchar_t* e = nullptr;
        double d = wcstod(p, &e);
        if (e == p) return fail(L"unexpected character");
        v.type = JVal::Num; v.n = d; p = e;
        return true;
    }
};
void esc(std::wstring& o, const std::wstring& s) {
    o += L'"';
    for (wchar_t c : s) {
        switch (c) {
        case '"': o += L"\\\""; break; case '\\': o += L"\\\\"; break;
        case '\n': o += L"\\n"; break; case '\r': o += L"\\r"; break; case '\t': o += L"\\t"; break;
        default:
            if (c < 0x20) { wchar_t b[8]; swprintf(b, 8, L"\\u%04x", c); o += b; }
            else o += c;
        }
    }
    o += L'"';
}
void write(std::wstring& o, const JVal& v, int ind) {
    std::wstring pad(ind * 2, L' '), pad2((ind + 1) * 2, L' ');
    switch (v.type) {
    case JVal::Null: o += L"null"; break;
    case JVal::Bool: o += v.b ? L"true" : L"false"; break;
    case JVal::Num: {
        wchar_t b[64];
        if (std::fabs(v.n - std::round(v.n)) < 1e-9 && std::fabs(v.n) < 1e15) swprintf(b, 64, L"%lld", (long long)std::llround(v.n));
        else swprintf(b, 64, L"%.6g", v.n);
        o += b; break;
    }
    case JVal::Str: esc(o, v.s); break;
    case JVal::Arr:
        if (v.a.empty()) { o += L"[]"; break; }
        o += L"[\n";
        for (size_t i = 0; i < v.a.size(); i++) { o += pad2; write(o, v.a[i], ind + 1); if (i + 1 < v.a.size()) o += L","; o += L"\n"; }
        o += pad + L"]"; break;
    case JVal::Obj:
        if (v.o.empty()) { o += L"{}"; break; }
        o += L"{\n";
        for (size_t i = 0; i < v.o.size(); i++) {
            o += pad2; esc(o, v.o[i].first); o += L": "; write(o, v.o[i].second, ind + 1);
            if (i + 1 < v.o.size()) o += L","; o += L"\n";
        }
        o += pad + L"}"; break;
    }
}
} // namespace

bool JsonParse(const std::wstring& text, JVal& out, std::wstring* err) {
    Parser ps{ text.c_str(), text.c_str() + text.size(), L"" };
    out = JVal();
    bool ok = ps.val(out, 0);
    if (!ok && err) {
        int line = 1; for (const wchar_t* q = text.c_str(); q < ps.p && q < ps.end; q++) if (*q == '\n') line++;
        *err = ps.err + L" (line " + std::to_wstring(line) + L")";
    }
    return ok;
}
std::wstring JsonWrite(const JVal& v, int indent) { std::wstring o; write(o, v, indent); return o; }

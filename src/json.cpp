#include "json.h"

JVal::~JVal() { for (int i = 0; i < arr.n; i++) delete arr.p[i]; }

JVal* JVal::Get(const wchar_t* key) const
{
    if (t != JObj) return nullptr;
    for (int i = 0; i < keys.n; i++) if (keys.p[i].Eq(key)) return arr.p[i];
    return nullptr;
}
const Str* JVal::GetStr(const wchar_t* key) const
{
    JVal* v = Get(key);
    return v && v->t == JStr ? &v->s : nullptr;
}
Str JVal::Str_(const wchar_t* key, const wchar_t* def) const
{
    const Str* s = GetStr(key);
    return s ? *s : Str(def);
}
double JVal::Num(const wchar_t* key, double def) const
{
    JVal* v = Get(key);
    return v && v->t == JNum ? v->num : def;
}
bool JVal::Bool(const wchar_t* key, bool def) const
{
    JVal* v = Get(key);
    return v && v->t == JBool ? v->b : def;
}
JVal* JVal::Set(const wchar_t* key, JVal* v)
{
    for (int i = 0; i < keys.n; i++)
        if (keys.p[i].Eq(key)) { delete arr.p[i]; arr.p[i] = v; return this; }
    keys.Push(Str(key));
    arr.Push(v);
    return this;
}
JVal* JVal::SetStr(const wchar_t* key, const Str& v) { return Set(key, JNewStr(v)); }
JVal* JVal::SetNum(const wchar_t* key, double v) { return Set(key, JNewNum(v)); }
JVal* JVal::SetBool(const wchar_t* key, bool v) { return Set(key, JNewBool(v)); }
JVal* JVal::SetNull(const wchar_t* key) { return Set(key, new JVal(JNull)); }
void JVal::RemoveKey(const wchar_t* key)
{
    for (int i = 0; i < keys.n; i++)
        if (keys.p[i].Eq(key)) { delete arr.p[i]; arr.RemoveAt(i); keys.RemoveAt(i); return; }
}

JVal* JNewObj() { return new JVal(JObj); }
JVal* JNewArr() { return new JVal(JArr); }
JVal* JNewStr(const Str& s) { JVal* v = new JVal(JStr); v->s = s; return v; }
JVal* JNewNum(double x) { JVal* v = new JVal(JNum); v->num = x; return v; }
JVal* JNewBool(bool x) { JVal* v = new JVal(JBool); v->b = x; return v; }

// ---- 解析 ----
struct JParser
{
    const wchar_t* s;
    int n, i = 0;
    bool fail = false;

    void Ws() { while (i < n && (s[i] == L' ' || s[i] == L'\t' || s[i] == L'\r' || s[i] == L'\n' || s[i] == 0xFEFF)) i++; }
    static int Hex(wchar_t c) { return c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : c >= L'A' && c <= L'F' ? c - L'A' + 10 : -1; }

    bool ParseString(Str& out)
    {
        if (i >= n || s[i] != L'"') return false;
        i++;
        while (i < n && s[i] != L'"')
        {
            wchar_t c = s[i++];
            if (c != L'\\') { out.Ch(c); continue; }
            if (i >= n) return false;
            wchar_t e = s[i++];
            switch (e)
            {
            case L'"': out.Ch(L'"'); break;
            case L'\\': out.Ch(L'\\'); break;
            case L'/': out.Ch(L'/'); break;
            case L'b': out.Ch(L'\b'); break;
            case L'f': out.Ch(L'\f'); break;
            case L'n': out.Ch(L'\n'); break;
            case L'r': out.Ch(L'\r'); break;
            case L't': out.Ch(L'\t'); break;
            case L'u':
            {
                if (i + 4 > n) return false;
                int v = 0;
                for (int k = 0; k < 4; k++) { int h = Hex(s[i + k]); if (h < 0) return false; v = v * 16 + h; }
                i += 4;
                out.Ch((wchar_t)v);
                break;
            }
            default: return false;
            }
        }
        if (i >= n) return false;
        i++;
        return true;
    }

    JVal* Value(int depth)
    {
        if (depth > 64) { fail = true; return nullptr; }
        Ws();
        if (i >= n) { fail = true; return nullptr; }
        wchar_t c = s[i];
        if (c == L'{')
        {
            i++;
            JVal* o = JNewObj();
            Ws();
            if (i < n && s[i] == L'}') { i++; return o; }
            while (true)
            {
                Ws();
                Str k;
                if (!ParseString(k)) { fail = true; delete o; return nullptr; }
                Ws();
                if (i >= n || s[i] != L':') { fail = true; delete o; return nullptr; }
                i++;
                JVal* v = Value(depth + 1);
                if (fail) { delete o; return nullptr; }
                o->keys.Push(Move(k));
                o->arr.Push(v);
                Ws();
                if (i < n && s[i] == L',') { i++; continue; }
                if (i < n && s[i] == L'}') { i++; return o; }
                fail = true; delete o; return nullptr;
            }
        }
        if (c == L'[')
        {
            i++;
            JVal* a = JNewArr();
            Ws();
            if (i < n && s[i] == L']') { i++; return a; }
            while (true)
            {
                JVal* v = Value(depth + 1);
                if (fail) { delete a; return nullptr; }
                a->arr.Push(v);
                Ws();
                if (i < n && s[i] == L',') { i++; continue; }
                if (i < n && s[i] == L']') { i++; return a; }
                fail = true; delete a; return nullptr;
            }
        }
        if (c == L'"')
        {
            JVal* v = new JVal(JStr);
            if (!ParseString(v->s)) { fail = true; delete v; return nullptr; }
            return v;
        }
        if (n - i >= 4 && s[i] == L't' && s[i + 1] == L'r' && s[i + 2] == L'u' && s[i + 3] == L'e') { i += 4; return JNewBool(true); }
        if (n - i >= 5 && s[i] == L'f' && s[i + 1] == L'a' && s[i + 2] == L'l' && s[i + 3] == L's' && s[i + 4] == L'e') { i += 5; return JNewBool(false); }
        if (n - i >= 4 && s[i] == L'n' && s[i + 1] == L'u' && s[i + 2] == L'l' && s[i + 3] == L'l') { i += 4; return new JVal(JNull); }
        double d;
        int used;
        if (ParseNum(s + i, n - i, &d, &used)) { i += used; return JNewNum(d); }
        fail = true;
        return nullptr;
    }
};

JVal* JParse(const Str& text)
{
    JParser p;
    p.s = text.c();
    p.n = text.n;
    JVal* v = p.Value(0);
    if (p.fail) return nullptr;
    p.Ws();
    if (p.i != p.n) { delete v; return nullptr; }
    return v;
}

// ---- 写出 ----
void JEscape(Str& out, const Str& s)
{
    out.Ch(L'"');
    for (int i = 0; i < s.n; i++)
    {
        wchar_t c = s.p[i];
        switch (c)
        {
        case L'"': out.Append(L"\\\""); break;
        case L'\\': out.Append(L"\\\\"); break;
        case L'\n': out.Append(L"\\n"); break;
        case L'\r': out.Append(L"\\r"); break;
        case L'\t': out.Append(L"\\t"); break;
        default:
            if (c < 0x20)
            {
                static const wchar_t* hex = L"0123456789abcdef";
                out.Append(L"\\u00");
                out.Ch(hex[c >> 4]);
                out.Ch(hex[c & 15]);
            }
            else out.Ch(c);
        }
    }
    out.Ch(L'"');
}

static bool IsInline(const JVal* o)
{
    if (o->arr.n > 3) return false;
    for (int i = 0; i < o->arr.n; i++) if (o->arr.p[i]->t == JObj || o->arr.p[i]->t == JArr) return false;
    return true;
}

static void Pad(Str& out, int depth) { for (int i = 0; i < depth * 2; i++) out.Ch(L' '); }

static void WriteVal(Str& out, const JVal* v, int depth)
{
    switch (v->t)
    {
    case JNull: out.Append(L"null"); break;
    case JBool: out.Append(v->b ? L"true" : L"false"); break;
    case JNum: out.Num(v->num, 6); break;
    case JStr: JEscape(out, v->s); break;
    case JObj:
        if (v->arr.n == 0) { out.Append(L"{}"); break; }
        if (IsInline(v))
        {
            out.Append(L"{ ");
            for (int i = 0; i < v->arr.n; i++)
            {
                if (i) out.Append(L", ");
                JEscape(out, v->keys.p[i]);
                out.Append(L": ");
                WriteVal(out, v->arr.p[i], depth + 1);
            }
            out.Append(L" }");
            break;
        }
        out.Append(L"{\r\n");
        for (int i = 0; i < v->arr.n; i++)
        {
            Pad(out, depth + 1);
            JEscape(out, v->keys.p[i]);
            out.Append(L": ");
            WriteVal(out, v->arr.p[i], depth + 1);
            out.Append(i < v->arr.n - 1 ? L",\r\n" : L"\r\n");
        }
        Pad(out, depth);
        out.Ch(L'}');
        break;
    case JArr:
        if (v->arr.n == 0) { out.Append(L"[]"); break; }
        out.Append(L"[\r\n");
        for (int i = 0; i < v->arr.n; i++)
        {
            Pad(out, depth + 1);
            WriteVal(out, v->arr.p[i], depth + 1);
            out.Append(i < v->arr.n - 1 ? L",\r\n" : L"\r\n");
        }
        Pad(out, depth);
        out.Ch(L']');
        break;
    }
}

Str JWrite(const JVal* v)
{
    Str out;
    WriteVal(out, v, 0);
    out.Append(L"\r\n");
    return out;
}

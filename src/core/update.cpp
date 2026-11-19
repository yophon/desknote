#include "update.h"
#include "json.h"

// 按点分开逐段比数字，前面的 v 和后面的 -beta 之类不管
int CompareVersions(const Str& a, const Str& b)
{
    int i = 0, j = 0;
    if (i < a.n && (a[i] == L'v' || a[i] == L'V')) i++;
    if (j < b.n && (b[j] == L'v' || b[j] == L'V')) j++;
    for (int part = 0; part < 4; part++)
    {
        i64 x = 0, y = 0;
        while (i < a.n && a[i] >= L'0' && a[i] <= L'9') x = x * 10 + (a[i++] - L'0');
        while (j < b.n && b[j] >= L'0' && b[j] <= L'9') y = y * 10 + (b[j++] - L'0');
        if (x != y) return x < y ? -1 : 1;
        if (i < a.n && a[i] == L'.') i++;
        if (j < b.n && b[j] == L'.') j++;
    }
    return 0;
}

bool ParseLatestRelease(const Str& json, Str* tag, Str* url)
{
    JVal* v = JParse(json);
    bool ok = false;
    if (v && v->t == JObj && !v->Bool(L"draft", false) && !v->Bool(L"prerelease", false))
    {
        *tag = v->Str_(L"tag_name", L"");
        *url = v->Str_(L"html_url", L"");
        ok = tag->n > 0 && url->StartsWith(L"http");
    }
    delete v;
    return ok;
}

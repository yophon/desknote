#include "links.h"

// 链接：http(s):// 或 www. 开头，到空白或中文标点为止
static bool IsUrlStop(wchar_t c)
{
    if (c <= L' ') return true;
    const wchar_t* stops = L"<>\"'，。；：、！？（）【】《》「」";
    for (const wchar_t* p = stops; *p; p++) if (*p == c) return true;
    return false;
}
void FindLinks(const Str& text, Vec<LinkSpan>& out)
{
    int i = 0;
    while (i < text.n)
    {
        bool http = (text.n - i >= 7 && (text.Sub(i, 7).EqI(L"http://"))) || (text.n - i >= 8 && text.Sub(i, 8).EqI(L"https://"));
        bool www = !http && text.n - i >= 4 && text.Sub(i, 4).EqI(L"www.");
        if (!http && !www) { i++; continue; }
        int j = i;
        while (j < text.n && !IsUrlStop(text.p[j])) j++;
        while (j > i && Str(L".,;:!?)]}").FindCh(text.p[j - 1]) >= 0) j--;
        if (j - i >= 8)
        {
            LinkSpan s;
            s.start = i;
            s.len = j - i;
            s.url = www ? L"http://" + text.Sub(i, j - i) : text.Sub(i, j - i);
            out.Push(s);
        }
        i = Max(j, i + 1);
    }
}

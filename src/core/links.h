// 条目文字里的链接（点了用浏览器打开）
#pragma once
#include "rt.h"

struct LinkSpan { int start, len; Str url; };
void FindLinks(const Str& text, Vec<LinkSpan>& out);

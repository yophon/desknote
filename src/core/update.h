// 检查新版本：GitHub Releases 的 latest 接口（仓库公开，不用登录）
#pragma once
#include "rt.h"

#define UPDATE_URL L"https://api.github.com/repos/yophon/desknote/releases/latest"

int CompareVersions(const Str& a, const Str& b);              // "v2.1.0" 和 "2.0.10" 这样的：<0、0、>0
bool ParseLatestRelease(const Str& json, Str* tag, Str* url); // 取 tag_name、html_url

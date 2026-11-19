// JSON：解析成树；写出格式和 WPF 版一致（2 空格缩进，字段不超过 3 个的简单对象写成一行，CRLF）
#pragma once
#include "rt.h"

enum JType { JNull, JBool, JNum, JStr, JArr, JObj };

struct JVal
{
    JType t = JNull;
    bool b = false;
    double num = 0;
    Str s;
    Vec<JVal*> arr;
    Vec<Str> keys;   // 对象：keys[i] 对应 arr[i]

    JVal() {}
    explicit JVal(JType type) : t(type) {}
    ~JVal();
    JVal(const JVal&) = delete;
    JVal& operator=(const JVal&) = delete;

    // 读
    JVal* Get(const wchar_t* key) const;
    const Str* GetStr(const wchar_t* key) const;
    Str Str_(const wchar_t* key, const wchar_t* def) const;
    double Num(const wchar_t* key, double def) const;
    bool Bool(const wchar_t* key, bool def) const;
    int Count() const { return arr.n; }
    JVal* At(int i) const { return arr.p[i]; }

    // 写（返回 this 便于连写）
    JVal* Set(const wchar_t* key, JVal* v);
    JVal* SetStr(const wchar_t* key, const Str& v);
    JVal* SetNum(const wchar_t* key, double v);
    JVal* SetBool(const wchar_t* key, bool v);
    JVal* SetNull(const wchar_t* key);
    JVal* Add(JVal* v) { arr.Push(v); return this; }
    void RemoveKey(const wchar_t* key);
};

JVal* JNewObj();
JVal* JNewArr();
JVal* JNewStr(const Str& s);
JVal* JNewNum(double v);
JVal* JNewBool(bool v);

JVal* JParse(const Str& text);       // 失败返回 nullptr
Str JWrite(const JVal* v);           // 末尾带 CRLF
void JEscape(Str& out, const Str& s);

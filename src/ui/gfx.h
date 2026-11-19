// 绘图接口（跨平台）：界面代码只通过它画东西、排文字。
// 单位都是 DIP（96 DPI 下的像素）；颜色都是 0xAARRGGBB；文字里的位置是 Str 的下标。
// Painter、TextLayout 由各平台定义：Windows 在 win/gfx_d2d.cpp（Direct2D + DirectWrite 软件渲染）。
#pragma once
#include "rt.h"

struct RectF
{
    float left, top, right, bottom;
    RectF() : left(0), top(0), right(0), bottom(0) {}
    RectF(float l, float t, float r, float b) : left(l), top(t), right(r), bottom(b) {}
    float W() const { return right - left; }
    float H() const { return bottom - top; }
};

// 0xRRGGBB 加上不透明度
inline u32 Argb(u32 rgb, float a) { return ((u32)(Clamp(a, 0.f, 1.f) * 255 + 0.5f) << 24) | (rgb & 0xFFFFFF); }

struct Painter;    // 一次绘制
struct TextLayout; // 排好版的一段文字

enum FontKind
{
    FontUi,   // 界面字体（Windows：Segoe UI Variable，中文回退到微软雅黑）
    FontNote, // 便签正文（Windows：微软雅黑 UI）
};

struct TextFmt
{
    int font = FontUi;
    float size = 14;
    int weight = 400;
    bool wrap = true;      // false：只排一行，超出最大宽度的部分显示省略号
    float lineH = 0;       // > 0：固定行高
    float baseline = 0.8f; // 固定行高时基线的位置（占行高的比例）
};

namespace Gfx
{
    // 绘图资源用时创建，空闲几秒后整体释放（省内存）
    void Touch();
    void ReleaseIfIdle();
    void ReleaseAll();

    void Clear(Painter* p, u32 argb);
    void PushClip(Painter* p, RectF r);      // 之后只画在 r 里（不抗锯齿的直边）
    void PopClip(Painter* p);
    void SetOffset(Painter* p, float dx, float dy); // 之后画的东西整体平移（不累加，传 0,0 复原）
    void GrayscaleText(Painter* p);          // 文字用灰度抗锯齿（透明背景上必须这样）

    // ---- 文字排版 ----
    TextLayout* Layout(const Str& s, const TextFmt& f, float maxW);
    void Free(TextLayout* l);
    void SetMaxWidth(TextLayout* l, float w);
    void SetAlign(TextLayout* l, int align);                   // Ui::Left / Center / Right
    void SetUnderline(TextLayout* l, int start, int len);
    void SetStrike(TextLayout* l, int start, int len);
    void SetSize(TextLayout* l, int start, int len, float size);
    float Width(TextLayout* l);   // 含行尾空格
    float Height(TextLayout* l);
    void Draw(Painter* p, TextLayout* l, float x, float y, u32 argb, bool clip = false); // clip：超出最大宽度的不画
    // 命中测试：坐标相对排版的左上角
    struct CaretPos { float x, y, h; };
    CaretPos Caret(TextLayout* l, int pos);         // pos 前面的光标
    // 点在哪个位置（落在字的后半边算后一个位置）；inside：点在字上；charAt：点中的那个字的位置
    int HitPoint(TextLayout* l, float x, float y, bool* inside = nullptr, int* charAt = nullptr);
    void RangeRects(TextLayout* l, int start, int len, Vec<RectF>& out); // 一段文字占的矩形（每行一个）
    struct LineInfo { int length, newlineLength; };
    void Lines(TextLayout* l, Vec<LineInfo>& out);
    void Clusters(TextLayout* l, Vec<int>& lengths); // 每个字形簇（emoji、组合字符算一个）占几个下标
}

namespace Ui
{
    enum { Left, Center, Right };
    void Fill(Painter* rt, RectF r, float radius, u32 argb);
    void FillCorners(Painter* rt, RectF r, float tl, float tr, float br, float bl, u32 argb); // 每个角半径不同
    void Stroke(Painter* rt, RectF r, float radius, u32 argb, float width = 1);
    void Circle(Painter* rt, float cx, float cy, float radius, u32 argb);
    void Ring(Painter* rt, float cx, float cy, float radius, u32 argb, float width);
    void Line(Painter* rt, float x0, float y0, float x1, float y1, u32 argb, float width = 1);
    void Shadow(Painter* rt, RectF r, float radius, int layers, float alpha, float dy);
    // 界面字体的单行文字，在 r 里垂直居中，放不下显示省略号；返回文字宽度
    float Text(Painter* rt, const Str& s, RectF r, float size, int weight, u32 argb, int align = Left);
    // 界面字体的多行文字，返回高度（rt 为空只量不画）
    float TextWrap(Painter* rt, const Str& s, float x, float y, float w, float size, int weight, u32 argb);
    float Measure(const Str& s, float size, int weight);
    float MeasureWrap(const Str& s, float w, float size, int weight);
    // 图标字体里的一个图标，以 (cx, cy) 为中心（Windows：Segoe Fluent Icons，没有就用 Segoe MDL2 Assets）
    void Icon(Painter* rt, wchar_t glyph, float cx, float cy, float size, u32 argb);
}

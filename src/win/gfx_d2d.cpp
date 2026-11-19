// 绘图接口（ui/gfx.h）的 Windows 实现：Direct2D 画图形，DirectWrite 排文字。
// 工厂用时创建，空闲几秒后整体释放（省内存）；画刷每次现做现放（软件渲染，开销很小）
#include "gfx_d2d.h"

template <class T> static void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

// =====================================================================
// 工厂
// =====================================================================
namespace Gfx
{
    static ID2D1Factory* d2d;
    static IDWriteFactory* dw;
    static u64 lastUse;
    ID2D1Factory* D2D()
    {
        if (!d2d) D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, (void**)&d2d);
        lastUse = TickMs();
        return d2d;
    }
    IDWriteFactory* DW()
    {
        // 独立工厂：字体缓存归它自己，释放工厂时一起释放（共享工厂的缓存会一直留在进程里）
        if (!dw) DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory), (IUnknown**)&dw);
        lastUse = TickMs();
        return dw;
    }
    void Touch() { lastUse = TickMs(); }
    void ReleaseAll()
    {
        Rel(dw);
        Rel(d2d);
        HeapCompact(GetProcessHeap(), 0);
    }
    void ReleaseIfIdle() { if ((d2d || dw) && TickMs() - lastUse > 3000) ReleaseAll(); }
    D2D1_COLOR_F Color(u32 argb)
    {
        D2D1_COLOR_F c = { ((argb >> 16) & 255) / 255.f, ((argb >> 8) & 255) / 255.f, (argb & 255) / 255.f, (argb >> 24) / 255.f };
        return c;
    }

    static ID2D1SolidColorBrush* Brush(Painter* p, u32 argb)
    {
        ID2D1SolidColorBrush* b = nullptr;
        D2D1_COLOR_F c = Color(argb);
        p->rt->CreateSolidColorBrush(&c, nullptr, &b);
        return b;
    }

    void Clear(Painter* p, u32 argb) { D2D1_COLOR_F c = Color(argb); p->rt->Clear(&c); }
    void PushClip(Painter* p, RectF r) { p->rt->PushAxisAlignedClip(D2D1::RectF(r.left, r.top, r.right, r.bottom), D2D1_ANTIALIAS_MODE_ALIASED); }
    void PopClip(Painter* p) { p->rt->PopAxisAlignedClip(); }
    void SetOffset(Painter* p, float dx, float dy) { p->rt->SetTransform(D2D1::Matrix3x2F::Translation(dx, dy)); }
    void GrayscaleText(Painter* p) { p->rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); }

    // =================================================================
    // 文字排版：TextLayout 就是 IDWriteTextLayout
    // =================================================================
    static IDWriteTextLayout* L(TextLayout* l) { return (IDWriteTextLayout*)l; }

    static bool HasFamily(const wchar_t* name)
    {
        IDWriteFontCollection* fc = nullptr;
        BOOL exists = FALSE;
        if (SUCCEEDED(DW()->GetSystemFontCollection(&fc, FALSE)) && fc)
        {
            UINT32 idx;
            fc->FindFamilyName(name, &idx, &exists);
            fc->Release();
        }
        return exists != FALSE;
    }
    // 界面字体：Segoe UI Variable（Win11 的界面字体），中文由系统回退到微软雅黑；没有就用 Segoe UI
    static const wchar_t* UiFamily()
    {
        static int has = -1;
        if (has < 0) has = HasFamily(L"Segoe UI Variable Text") ? 1 : 0;
        return has ? L"Segoe UI Variable Text" : L"Segoe UI";
    }

    TextLayout* Layout(const Str& s, const TextFmt& fm, float maxW)
    {
        const wchar_t* family = fm.font == FontNote ? L"Microsoft YaHei UI" : UiFamily();
        IDWriteTextFormat* f = nullptr;
        DW()->CreateTextFormat(family, nullptr, (DWRITE_FONT_WEIGHT)fm.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                               fm.size, L"zh-cn", &f);
        if (!f) return nullptr;
        if (!fm.wrap)
        {
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            IDWriteInlineObject* sign = nullptr;
            DW()->CreateEllipsisTrimmingSign(f, &sign);
            DWRITE_TRIMMING trim = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            f->SetTrimming(&trim, sign);
            Rel(sign);
        }
        IDWriteTextLayout* l = nullptr;
        DW()->CreateTextLayout(s.c(), s.n, f, Max(maxW, 1.f), 100000.f, &l);
        f->Release();
        if (l && fm.lineH > 0) l->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, fm.lineH, fm.lineH * fm.baseline);
        return (TextLayout*)l;
    }
    void Free(TextLayout* l) { if (l) L(l)->Release(); }
    void SetMaxWidth(TextLayout* l, float w) { L(l)->SetMaxWidth(w); }
    void SetAlign(TextLayout* l, int align)
    {
        L(l)->SetTextAlignment(align == Ui::Center ? DWRITE_TEXT_ALIGNMENT_CENTER : align == Ui::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    void SetUnderline(TextLayout* l, int start, int len) { L(l)->SetUnderline(TRUE, { (UINT32)start, (UINT32)len }); }
    void SetStrike(TextLayout* l, int start, int len) { L(l)->SetStrikethrough(TRUE, { (UINT32)start, (UINT32)len }); }
    void SetSize(TextLayout* l, int start, int len, float size) { L(l)->SetFontSize(size, { (UINT32)start, (UINT32)len }); }
    float Width(TextLayout* l) { DWRITE_TEXT_METRICS m = {}; L(l)->GetMetrics(&m); return m.widthIncludingTrailingWhitespace; }
    float Height(TextLayout* l) { DWRITE_TEXT_METRICS m = {}; L(l)->GetMetrics(&m); return m.height; }
    void Draw(Painter* p, TextLayout* l, float x, float y, u32 argb, bool clip)
    {
        ID2D1SolidColorBrush* b = Brush(p, argb);
        if (!b) return;
        p->rt->DrawTextLayout(D2D1::Point2F(x, y), L(l), b, clip ? D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT | D2D1_DRAW_TEXT_OPTIONS_CLIP : D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        b->Release();
    }
    CaretPos Caret(TextLayout* l, int pos)
    {
        DWRITE_HIT_TEST_METRICS m = {};
        float px = 0, py = 0;
        L(l)->HitTestTextPosition((UINT32)pos, FALSE, &px, &py, &m);
        CaretPos c = { px, py, m.height };
        return c;
    }
    int HitPoint(TextLayout* l, float x, float y, bool* insideOut, int* charAt)
    {
        BOOL trailing = FALSE, inside = FALSE;
        DWRITE_HIT_TEST_METRICS m = {};
        L(l)->HitTestPoint(x, y, &trailing, &inside, &m);
        if (insideOut) *insideOut = inside != FALSE;
        if (charAt) *charAt = (int)m.textPosition;
        return (int)(m.textPosition + (trailing ? m.length : 0));
    }
    void RangeRects(TextLayout* l, int start, int len, Vec<RectF>& out)
    {
        UINT32 cnt = 0;
        L(l)->HitTestTextRange(start, len, 0, 0, nullptr, 0, &cnt);
        if (!cnt) return;
        DWRITE_HIT_TEST_METRICS* ms = (DWRITE_HIT_TEST_METRICS*)MemAlloc(sizeof(DWRITE_HIT_TEST_METRICS) * cnt);
        L(l)->HitTestTextRange(start, len, 0, 0, ms, cnt, &cnt);
        for (UINT32 i = 0; i < cnt; i++) out.Push(RectF(ms[i].left, ms[i].top, ms[i].left + ms[i].width, ms[i].top + ms[i].height));
        MemFree(ms);
    }
    void Lines(TextLayout* l, Vec<LineInfo>& out)
    {
        UINT32 lc = 0;
        L(l)->GetLineMetrics(nullptr, 0, &lc);
        if (!lc) return;
        DWRITE_LINE_METRICS* lm = (DWRITE_LINE_METRICS*)MemAlloc(sizeof(DWRITE_LINE_METRICS) * lc);
        L(l)->GetLineMetrics(lm, lc, &lc);
        for (UINT32 i = 0; i < lc; i++) { LineInfo li = { (int)lm[i].length, (int)lm[i].newlineLength }; out.Push(li); }
        MemFree(lm);
    }
    void Clusters(TextLayout* l, Vec<int>& lengths)
    {
        UINT32 cnt = 0;
        L(l)->GetClusterMetrics(nullptr, 0, &cnt);
        if (!cnt) return;
        DWRITE_CLUSTER_METRICS* cm = (DWRITE_CLUSTER_METRICS*)MemAlloc(sizeof(DWRITE_CLUSTER_METRICS) * cnt);
        L(l)->GetClusterMetrics(cm, cnt, &cnt);
        for (UINT32 i = 0; i < cnt; i++) lengths.Push((int)cm[i].length);
        MemFree(cm);
    }
}

// =====================================================================
// 图形和界面文字
// =====================================================================
namespace Ui
{
    using Gfx::Brush;

    void Fill(Painter* p, RectF r, float radius, u32 argb)
    {
        ID2D1SolidColorBrush* b = Brush(p, argb);
        if (!b) return;
        D2D1_RECT_F d = { r.left, r.top, r.right, r.bottom };
        if (radius > 0) p->rt->FillRoundedRectangle(D2D1::RoundedRect(d, radius, radius), b);
        else p->rt->FillRectangle(&d, b);
        b->Release();
    }

    void FillCorners(Painter* p, RectF r, float tl, float tr, float br, float bl, u32 argb)
    {
        ID2D1PathGeometry* g = nullptr;
        ID2D1GeometrySink* s = nullptr;
        Gfx::D2D()->CreatePathGeometry(&g);
        if (!g) return;
        g->Open(&s);
        s->BeginFigure(D2D1::Point2F(r.left + tl, r.top), D2D1_FIGURE_BEGIN_FILLED);
        s->AddLine(D2D1::Point2F(r.right - tr, r.top));
        if (tr > 0) s->AddArc(D2D1::ArcSegment(D2D1::Point2F(r.right, r.top + tr), D2D1::SizeF(tr, tr), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        s->AddLine(D2D1::Point2F(r.right, r.bottom - br));
        if (br > 0) s->AddArc(D2D1::ArcSegment(D2D1::Point2F(r.right - br, r.bottom), D2D1::SizeF(br, br), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        s->AddLine(D2D1::Point2F(r.left + bl, r.bottom));
        if (bl > 0) s->AddArc(D2D1::ArcSegment(D2D1::Point2F(r.left, r.bottom - bl), D2D1::SizeF(bl, bl), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        s->AddLine(D2D1::Point2F(r.left, r.top + tl));
        if (tl > 0) s->AddArc(D2D1::ArcSegment(D2D1::Point2F(r.left + tl, r.top), D2D1::SizeF(tl, tl), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
        s->Close();
        ID2D1SolidColorBrush* b = Brush(p, argb);
        if (b) { p->rt->FillGeometry(g, b); b->Release(); }
        s->Release();
        g->Release();
    }

    void Stroke(Painter* p, RectF r, float radius, u32 argb, float width)
    {
        ID2D1SolidColorBrush* b = Brush(p, argb);
        if (!b) return;
        float h = width / 2;
        D2D1_RECT_F in = { r.left + h, r.top + h, r.right - h, r.bottom - h };
        if (radius > 0) p->rt->DrawRoundedRectangle(D2D1::RoundedRect(in, Max(0.f, radius - h), Max(0.f, radius - h)), b, width);
        else p->rt->DrawRectangle(&in, b, width);
        b->Release();
    }

    void Circle(Painter* p, float cx, float cy, float radius, u32 argb)
    {
        ID2D1SolidColorBrush* b = Brush(p, argb);
        if (!b) return;
        p->rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), b);
        b->Release();
    }

    void Ring(Painter* p, float cx, float cy, float radius, u32 argb, float width)
    {
        ID2D1SolidColorBrush* b = Brush(p, argb);
        if (!b) return;
        p->rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), b, width);
        b->Release();
    }

    void Line(Painter* p, float x0, float y0, float x1, float y1, u32 argb, float width)
    {
        ID2D1SolidColorBrush* b = Brush(p, argb);
        if (!b) return;
        p->rt->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), b, width);
        b->Release();
    }

    // 用一圈圈逐渐放大的半透明圆角矩形叠出柔和阴影（软件渲染没有模糊特效可用）
    void Shadow(Painter* p, RectF r, float radius, int layers, float alpha, float dy)
    {
        ID2D1SolidColorBrush* b = Brush(p, 0xFF000000);
        if (!b) return;
        b->SetOpacity(alpha);
        for (int k = layers; k >= 1; k--)
        {
            D2D1_RECT_F e = { r.left - k, r.top - k + dy, r.right + k, r.bottom + k + dy };
            p->rt->FillRoundedRectangle(D2D1::RoundedRect(e, radius + k, radius + k), b);
        }
        b->Release();
    }

    // 界面文字：行高 = 字号 × 1.43（取整），基线在 0.76 处
    static TextLayout* UiLayout(const Str& s, float size, int weight, bool wrap, float w)
    {
        TextFmt f;
        f.font = FontUi;
        f.size = size;
        f.weight = weight;
        f.wrap = wrap;
        f.lineH = (float)(int)(size * 1.43f + 0.5f);
        f.baseline = 0.76f;
        return Gfx::Layout(s, f, w);
    }

    float Text(Painter* p, const Str& s, RectF r, float size, int weight, u32 argb, int align)
    {
        if (!s.n) return 0;
        TextLayout* l = UiLayout(s, size, weight, false, r.right - r.left);
        if (!l) return 0;
        float w = Gfx::Width(l), h = Gfx::Height(l);
        if (align != Left) Gfx::SetAlign(l, align);
        Gfx::Draw(p, l, r.left, r.top + ((r.bottom - r.top) - h) / 2, argb);
        Gfx::Free(l);
        return Min(w, r.right - r.left);
    }

    float TextWrap(Painter* p, const Str& s, float x, float y, float w, float size, int weight, u32 argb)
    {
        if (!s.n) return 0;
        TextLayout* l = UiLayout(s, size, weight, true, w);
        if (!l) return 0;
        float h = Gfx::Height(l);
        if (p) Gfx::Draw(p, l, x, y, argb);
        Gfx::Free(l);
        return h;
    }

    float Measure(const Str& s, float size, int weight)
    {
        if (!s.n) return 0;
        TextLayout* l = UiLayout(s, size, weight, false, 100000.f);
        if (!l) return 0;
        float w = Gfx::Width(l);
        Gfx::Free(l);
        return w;
    }

    float MeasureWrap(const Str& s, float w, float size, int weight) { return TextWrap(nullptr, s, 0, 0, w, size, weight, 0); }

    void Icon(Painter* p, wchar_t glyph, float cx, float cy, float size, u32 argb)
    {
        static int fluent = -1;
        if (fluent < 0) fluent = Gfx::HasFamily(L"Segoe Fluent Icons") ? 1 : 0;
        IDWriteTextFormat* f = nullptr;
        Gfx::DW()->CreateTextFormat(fluent ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f);
        if (!f) return;
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ID2D1SolidColorBrush* b = Brush(p, argb);
        if (b)
        {
            D2D1_RECT_F r = { cx - size, cy - size, cx + size, cy + size };
            p->rt->DrawTextW(&glyph, 1, f, &r, b, D2D1_DRAW_TEXT_OPTIONS_NONE);
            b->Release();
        }
        f->Release();
    }
}

#include "gfx.h"

#include <dwrite_3.h>

#include <algorithm>
#include <cmath>

namespace {

D2D1_COLOR_F Rgb(int r, int g, int b, float a = 1) { return D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f, a); }
D2D1_COLOR_F White(float a) { return D2D1::ColorF(1, 1, 1, a); }

bool HasFamily(IDWriteFactory* dw, const wchar_t* name) {
    ComPtr<IDWriteFontCollection> fonts;
    UINT32 index;
    BOOL exists = FALSE;
    return SUCCEEDED(dw->GetSystemFontCollection(&fonts)) && SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) &&
           exists;
}

}  // namespace

bool Gfx::Init(HWND hwnd, HINSTANCE inst, int sansFont, int monoFont) {
    hwnd_ = hwnd;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dw_.GetAddressOf()))))
        return false;
    D2D1_STROKE_STYLE_PROPERTIES dash = D2D1::StrokeStyleProperties();
    dash.dashStyle = D2D1_DASH_STYLE_CUSTOM;
    float pattern[] = {6, 6};
    d2d_->CreateStrokeStyle(dash, pattern, 2, &dash_);
    D2D1_STROKE_STYLE_PROPERTIES round = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                     D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    d2d_->CreateStrokeStyle(round, nullptr, 0, &round_);
    if (LoadFonts(inst, sansFont, monoFont)) {
        sans_ = L"Geist";
        mono_ = L"Geist Mono";
    } else {   // needs Windows 10 1709 or later
        sans_ = HasFamily(dw_.Get(), L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
        mono_ = L"Consolas";
    }
    icons_ = HasFamily(dw_.Get(), L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";

    Theme& c = theme;
    c.bg = Rgb(7, 8, 10);
    c.text1 = Rgb(243, 244, 246); c.text2 = Rgb(141, 145, 155); c.text3 = Rgb(88, 92, 102); c.textDisabled = Rgb(243, 244, 246, .35f);
    c.card = Rgb(16, 17, 21); c.cardLine = White(.07f); c.dashed = White(.1f);
    c.ghost = White(.04f); c.ghostHover = White(.07f); c.ghostLine = White(.08f);
    c.accent = Rgb(62, 224, 161); c.accentHover = Rgb(85, 232, 176); c.accentInk = Rgb(4, 20, 13);
    c.accentTint = Rgb(62, 224, 161, .08f); c.accentLine = Rgb(62, 224, 161, .22f);
    c.warn = Rgb(245, 196, 81); c.warnTint = Rgb(245, 196, 81, .08f); c.warnLine = Rgb(245, 196, 81, .22f);
    c.danger = Rgb(255, 122, 122); c.dangerTint = Rgb(255, 122, 122, .08f); c.dangerLine = Rgb(255, 122, 122, .22f);
    c.track = White(.05f);
    return true;
}

bool Gfx::LoadFonts(HINSTANCE inst, int sansFont, int monoFont) {
    ComPtr<IDWriteFactory5> f5;
    ComPtr<IDWriteInMemoryFontFileLoader> loader;
    ComPtr<IDWriteFontSetBuilder> base;
    ComPtr<IDWriteFontSetBuilder1> builder;
    if (FAILED(dw_.As(&f5)) || FAILED(f5->CreateInMemoryFontFileLoader(&loader)) ||
        FAILED(f5->RegisterFontFileLoader(loader.Get())) || FAILED(f5->CreateFontSetBuilder(&base)) || FAILED(base.As(&builder)))
        return false;
    for (int id : {sansFont, monoFont}) {
        HRSRC res = FindResourceW(inst, MAKEINTRESOURCEW(id), RT_RCDATA);
        if (!res) return false;
        const void* data = LockResource(LoadResource(inst, res));
        ComPtr<IDWriteFontFileStream> stream;
        if (FAILED(f5->UnpackFontFile(DWRITE_CONTAINER_TYPE_WOFF2, data, SizeofResource(inst, res), &stream))) return false;
        UINT64 size = 0;
        const void* bytes = nullptr;
        void* ctx = nullptr;
        if (FAILED(stream->GetFileSize(&size)) || FAILED(stream->ReadFileFragment(&bytes, 0, size, &ctx))) return false;
        ComPtr<IDWriteFontFile> file;   // with no owner the loader keeps its own copy
        HRESULT hr = loader->CreateInMemoryFontFileReference(f5.Get(), bytes, UINT32(size), nullptr, &file);
        stream->ReleaseFileFragment(ctx);
        if (FAILED(hr) || FAILED(builder->AddFontFile(file.Get()))) return false;
    }
    ComPtr<IDWriteFontSet> set;
    ComPtr<IDWriteFontCollection1> collection;
    if (FAILED(builder->CreateFontSet(&set)) || FAILED(f5->CreateFontCollectionFromFontSet(set.Get(), &collection))) return false;
    fonts_ = collection;
    return true;
}

void Gfx::SetDpi(UINT dpi) {
    dpi_ = dpi;
    if (rt_) rt_->SetDpi(float(dpi), float(dpi));
}

void Gfx::Resize() {
    if (!rt_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    rt_->Resize(D2D1::SizeU(UINT(rc.right - rc.left), UINT(rc.bottom - rc.top)));
}

D2D1_SIZE_F Gfx::Size() const { return rt_ ? rt_->GetSize() : D2D1::SizeF(); }

bool Gfx::Begin() {
    if (!rt_) {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        // Drawn on the CPU: a few cards and lines don't need the GPU, and skipping it
        // keeps the graphics driver (tens of MB) out of the process.
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                  D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                                                  float(dpi_), float(dpi_));
        auto hprops = D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(UINT(rc.right - rc.left), UINT(rc.bottom - rc.top)));
        if (FAILED(d2d_->CreateHwndRenderTarget(props, hprops, &rt_))) return false;
        rt_->CreateSolidColorBrush(theme.text1, &brush_);
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);   // no colour fringes on thin icons
    }
    rt_->BeginDraw();
    offsetY_ = 0;
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->Clear(theme.bg);
    return true;
}

void Gfx::End() {
    if (rt_ && rt_->EndDraw() == D2DERR_RECREATE_TARGET) Rebuild();
}

void Gfx::Rebuild() {
    brush_.Reset();
    rt_.Reset();
    InvalidateRect(hwnd_, nullptr, FALSE);   // draw again on a new target
}

ID2D1SolidColorBrush* Gfx::Brush(D2D1_COLOR_F c) {
    brush_->SetColor(c);
    return brush_.Get();
}

IDWriteTextFormat* Gfx::Format(Font f) {
    auto& slot = formats_[int(f)];
    if (slot) return slot.Get();
    struct Spec { const std::wstring* family; float size; int weight; };
    Spec s{};
    switch (f) {
        case Font::Caption: s = {&sans_, 12, 400}; break;
        case Font::Body: s = {&sans_, 14, 400}; break;
        case Font::BodyStrong: s = {&sans_, 14, 500}; break;
        case Font::Subtitle: s = {&sans_, 20, 500}; break;
        case Font::Title: s = {&sans_, 30, 500}; break;
        case Font::Mono: s = {&mono_, 12.5f, 400}; break;
        case Font::Icon16: s = {&icons_, 16, 400}; break;
        case Font::Icon20: s = {&icons_, 20, 400}; break;
        case Font::Icon28: s = {&icons_, 28, 400}; break;
    }
    bool icon = s.family == &icons_;
    dw_->CreateTextFormat(s.family->c_str(), icon ? nullptr : fonts_.Get(), DWRITE_FONT_WEIGHT(s.weight),
                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, s.size, L"en-us", &slot);
    return slot.Get();
}

ComPtr<IDWriteTextLayout> Gfx::Layout(const std::wstring& s, Font f, float w, float h, Align a, bool wrap) {
    IDWriteTextFormat* fmt = Format(f);
    ComPtr<IDWriteTextLayout> layout;
    dw_->CreateTextLayout(s.c_str(), UINT32(s.size()), fmt, std::max(w, 1.f), std::max(h, 1.f), &layout);
    if (!layout) return layout;
    layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->SetTextAlignment(a == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                             : a == Align::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                 : DWRITE_TEXT_ALIGNMENT_LEADING);
    if (!wrap) {
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        dw_->CreateEllipsisTrimmingSign(fmt, &ellipsis);
        layout->SetTrimming(&trim, ellipsis.Get());
    }
    // big type is set tight
    float track = f == Font::Title ? -.035f : f == Font::Subtitle ? -.02f : 0;
    ComPtr<IDWriteTextLayout1> l1;
    if (track != 0 && SUCCEEDED(layout.As(&l1)))
        l1->SetCharacterSpacing(0, track * fmt->GetFontSize(), 0, DWRITE_TEXT_RANGE{0, UINT32(s.size())});
    return layout;
}

void Gfx::Fill(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius) {
    if (radius > 0) rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), Brush(c));
    else rt_->FillRectangle(r, Brush(c));
}

void Gfx::Stroke(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius, float width, bool dashed) {
    float h = width / 2;
    D2D1_RECT_F in = {r.left + h, r.top + h, r.right - h, r.bottom - h};
    ID2D1StrokeStyle* style = dashed ? dash_.Get() : nullptr;
    if (radius > 0) rt_->DrawRoundedRectangle(D2D1::RoundedRect(in, radius - h, radius - h), Brush(c), width, style);
    else rt_->DrawRectangle(in, Brush(c), width, style);
}

void Gfx::Line(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_COLOR_F c, float width) {
    // Centre straight hairlines on a device pixel, so they stay sharp at 125% and 150%.
    float s = Scale(), dy = offsetY_ * s;
    auto snap = [&](float v, float shift) { return (std::floor(v * s + shift) + .5f - shift) / s; };
    if (a.y == b.y) a.y = b.y = snap(a.y, dy);
    if (a.x == b.x) a.x = b.x = snap(a.x, 0);
    rt_->DrawLine(a, b, Brush(c), width);
}

void Gfx::Circle(D2D1_POINT_2F c, float r, D2D1_COLOR_F fill) {
    rt_->FillEllipse(D2D1::Ellipse(c, r, r), Brush(fill));
}

void Gfx::Area(const D2D1_RECT_F& r, const std::vector<D2D1_POINT_2F>& pts, D2D1_COLOR_F line, D2D1_COLOR_F fill) {
    if (pts.size() < 2) return;
    ComPtr<ID2D1PathGeometry> area, edge;
    ComPtr<ID2D1GeometrySink> sink;
    d2d_->CreatePathGeometry(&area);
    area->Open(&sink);
    // Catmull-Rom through the points, as cubic Beziers. Each segment depends only on its
    // neighbours, so adding a point can only nudge the end of the curve.
    auto curve = [&](ID2D1GeometrySink* sk) {
        for (size_t i = 0; i + 1 < pts.size(); i++) {
            D2D1_POINT_2F p0 = pts[i ? i - 1 : 0], p1 = pts[i], p2 = pts[i + 1], p3 = pts[std::min(i + 2, pts.size() - 1)];
            D2D1_POINT_2F c1 = {p1.x + (p2.x - p0.x) / 6, std::clamp(p1.y + (p2.y - p0.y) / 6, r.top, r.bottom)};
            D2D1_POINT_2F c2 = {p2.x - (p3.x - p1.x) / 6, std::clamp(p2.y - (p3.y - p1.y) / 6, r.top, r.bottom)};
            sk->AddBezier(D2D1::BezierSegment(c1, c2, p2));
        }
    };
    sink->BeginFigure(D2D1::Point2F(pts.front().x, r.bottom), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(pts.front());
    curve(sink.Get());
    sink->AddLine(D2D1::Point2F(pts.back().x, r.bottom));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    sink.Reset();
    d2d_->CreatePathGeometry(&edge);
    edge->Open(&sink);
    sink->BeginFigure(pts.front(), D2D1_FIGURE_BEGIN_HOLLOW);
    curve(sink.Get());
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();

    D2D1_COLOR_F clear = fill;
    clear.a = 0;
    D2D1_GRADIENT_STOP stops[] = {{0, fill}, {1, clear}};
    ComPtr<ID2D1GradientStopCollection> coll;
    ComPtr<ID2D1LinearGradientBrush> brush;
    rt_->CreateGradientStopCollection(stops, 2, &coll);
    rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, r.top), D2D1::Point2F(0, r.bottom)),
                                   coll.Get(), &brush);
    rt_->FillGeometry(area.Get(), brush.Get());
    rt_->DrawGeometry(edge.Get(), Brush(line), 1.6f, round_.Get());
}

float Gfx::Text(const std::wstring& s, Font f, const D2D1_RECT_F& r, D2D1_COLOR_F c, Align a, bool wrap) {
    auto layout = Layout(s, f, r.right - r.left, wrap ? 10000.f : r.bottom - r.top, a, wrap);
    if (!layout) return 0;
    rt_->DrawTextLayout(D2D1::Point2F(r.left, r.top), layout.Get(), Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.height;
}

float Gfx::TextHeight(const std::wstring& s, Font f, float width) {
    auto layout = Layout(s, f, width, 10000.f, Align::Left, true);
    DWRITE_TEXT_METRICS m{};
    if (layout) layout->GetMetrics(&m);
    return m.height;
}

float Gfx::TextWidth(const std::wstring& s, Font f) {
    auto layout = Layout(s, f, 10000.f, 10000.f, Align::Left, false);
    DWRITE_TEXT_METRICS m{};
    if (layout) layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

void Gfx::PushClip(const D2D1_RECT_F& r) { rt_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED); }
void Gfx::PopClip() { rt_->PopAxisAlignedClip(); }

void Gfx::Translate(float dy) {
    offsetY_ += dy;
    rt_->SetTransform(D2D1::Matrix3x2F::Translation(0, offsetY_));
}

#include "gfx.h"

#include <algorithm>
#include <cmath>

namespace {

D2D1_COLOR_F Rgb(int r, int g, int b, float a = 1) { return D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f, a); }
D2D1_COLOR_F White(float a) { return D2D1::ColorF(1, 1, 1, a); }
D2D1_COLOR_F Black(float a) { return D2D1::ColorF(0, 0, 0, a); }

bool HasFamily(IDWriteFactory* dw, const wchar_t* name) {
    ComPtr<IDWriteFontCollection> fonts;
    UINT32 index;
    BOOL exists = FALSE;
    return SUCCEEDED(dw->GetSystemFontCollection(&fonts)) && SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) &&
           exists;
}

ComPtr<IDWriteTextLayout> Layout(IDWriteFactory* dw, IDWriteTextFormat* fmt, const std::wstring& s, float w, float h,
                                 Align a, bool wrap) {
    ComPtr<IDWriteTextLayout> layout;
    dw->CreateTextLayout(s.c_str(), UINT32(s.size()), fmt, std::max(w, 1.f), std::max(h, 1.f), &layout);
    if (!layout) return layout;
    layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->SetTextAlignment(a == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                             : a == Align::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                 : DWRITE_TEXT_ALIGNMENT_LEADING);
    if (!wrap) {
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        dw->CreateEllipsisTrimmingSign(fmt, &ellipsis);
        layout->SetTrimming(&trim, ellipsis.Get());
    }
    return layout;
}

}  // namespace

bool Gfx::Init(HWND hwnd) {
    hwnd_ = hwnd;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dw_.GetAddressOf()))))
        return false;
    D2D1_STROKE_STYLE_PROPERTIES dash = D2D1::StrokeStyleProperties();
    dash.dashStyle = D2D1_DASH_STYLE_CUSTOM;
    float pattern[] = {4, 3};
    d2d_->CreateStrokeStyle(dash, pattern, 2, &dash_);
    // Windows 11 type and icons, falling back to their Windows 10 versions.
    bool variable = HasFamily(dw_.Get(), L"Segoe UI Variable Text");
    textFamily_ = variable ? L"Segoe UI Variable Text" : L"Segoe UI";
    displayFamily_ = variable ? L"Segoe UI Variable Display" : L"Segoe UI";
    iconFamily_ = HasFamily(dw_.Get(), L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    return true;
}

void Gfx::SetTheme(bool dark, bool useMica, D2D1_COLOR_F accentOnDark, D2D1_COLOR_F accentOnLight) {
    mica_ = useMica;
    Theme& c = theme;
    if (dark) {
        c.text1 = White(1); c.text2 = White(.786f); c.text3 = White(.544f); c.textDisabled = White(.363f);
        c.card = White(.0512f); c.cardStroke = Black(.1f);
        c.ctrl = White(.0605f); c.ctrlHover = White(.0837f); c.ctrlPress = White(.0326f); c.ctrlDisabled = White(.0419f);
        c.ctrlStroke = White(.0698f); c.ctrlStrokeBottom = White(.0465f);
        c.subtleHover = White(.0605f); c.subtlePress = White(.0419f);
        c.divider = White(.0837f); c.background = Rgb(32, 32, 32); c.footer = Rgb(58, 58, 58, .3f); c.dashed = White(.3f);
        c.accent = accentOnDark; c.onAccent = Black(1); c.accentDisabled = White(.1581f); c.onAccentDisabled = White(.53f);
        c.success = Rgb(108, 203, 95); c.successBg = Rgb(57, 61, 27);
        c.caution = Rgb(252, 225, 0); c.cautionBg = Rgb(67, 53, 25);
        c.critical = Rgb(255, 153, 164); c.criticalBg = Rgb(68, 39, 38);
        c.info = accentOnDark; c.infoBg = White(.0326f);
        c.smoke = Black(.3f); c.dialog = Rgb(43, 43, 43); c.dialogFooter = Rgb(32, 32, 32);
    } else {
        c.text1 = Black(.894f); c.text2 = Black(.606f); c.text3 = Black(.446f); c.textDisabled = Black(.361f);
        c.card = White(.7f); c.cardStroke = Black(.0578f);
        c.ctrl = White(.7f); c.ctrlHover = Rgb(249, 249, 249, .5f); c.ctrlPress = Rgb(249, 249, 249, .3f);
        c.ctrlDisabled = Rgb(249, 249, 249, .3f); c.ctrlStroke = Black(.0578f); c.ctrlStrokeBottom = Black(.1622f);
        c.subtleHover = Black(.0373f); c.subtlePress = Black(.0241f);
        c.divider = Black(.0803f); c.background = Rgb(243, 243, 243); c.footer = White(.5f); c.dashed = Black(.35f);
        c.accent = accentOnLight; c.onAccent = White(1); c.accentDisabled = Black(.2169f); c.onAccentDisabled = White(1);
        c.success = Rgb(15, 123, 15); c.successBg = Rgb(223, 246, 221);
        c.caution = Rgb(157, 93, 0); c.cautionBg = Rgb(255, 244, 206);
        c.critical = Rgb(196, 43, 28); c.criticalBg = Rgb(253, 231, 233);
        c.info = accentOnLight; c.infoBg = Rgb(246, 246, 246, .5f);
        c.smoke = Black(.3f); c.dialog = Rgb(255, 255, 255); c.dialogFooter = Rgb(243, 243, 243);
    }
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
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), float(dpi_), float(dpi_));
        auto hprops = D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(UINT(rc.right - rc.left), UINT(rc.bottom - rc.top)));
        if (FAILED(d2d_->CreateHwndRenderTarget(props, hprops, &rt_))) return false;
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);   // ClearType can't blend over Mica
        rt_->CreateSolidColorBrush(theme.text1, &brush_);
    }
    rt_->BeginDraw();
    offsetY_ = 0;
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->Clear(mica_ ? D2D1::ColorF(0, 0, 0, 0) : theme.background);
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
    struct Spec { const std::wstring* family; float size; DWRITE_FONT_WEIGHT weight; };
    Spec s{};
    switch (f) {
        case Font::Caption: s = {&textFamily_, 12, DWRITE_FONT_WEIGHT_REGULAR}; break;
        case Font::Body: s = {&textFamily_, 14, DWRITE_FONT_WEIGHT_REGULAR}; break;
        case Font::BodyStrong: s = {&textFamily_, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD}; break;
        case Font::Subtitle: s = {&displayFamily_, 20, DWRITE_FONT_WEIGHT_SEMI_BOLD}; break;
        case Font::Title: s = {&displayFamily_, 28, DWRITE_FONT_WEIGHT_SEMI_BOLD}; break;
        case Font::Large: s = {&displayFamily_, 40, DWRITE_FONT_WEIGHT_SEMI_BOLD}; break;
        case Font::Icon16: s = {&iconFamily_, 16, DWRITE_FONT_WEIGHT_REGULAR}; break;
        case Font::Icon20: s = {&iconFamily_, 20, DWRITE_FONT_WEIGHT_REGULAR}; break;
        case Font::Icon40: s = {&iconFamily_, 40, DWRITE_FONT_WEIGHT_REGULAR}; break;
    }
    dw_->CreateTextFormat(s.family->c_str(), nullptr, s.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                          s.size, L"en-us", &slot);
    return slot.Get();
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

void Gfx::Line(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_COLOR_F c, float width, bool dashed) {
    // Centre straight hairlines on a device pixel, so they stay sharp at 125% and 150%.
    float s = Scale(), dy = offsetY_ * s;
    auto snap = [&](float v, float shift) { return (std::floor(v * s + shift) + .5f - shift) / s; };
    if (a.y == b.y) a.y = b.y = snap(a.y, dy);
    if (a.x == b.x) a.x = b.x = snap(a.x, 0);
    rt_->DrawLine(a, b, Brush(c), width, dashed ? dash_.Get() : nullptr);
}

void Gfx::Circle(D2D1_POINT_2F c, float r, D2D1_COLOR_F fill) {
    rt_->FillEllipse(D2D1::Ellipse(c, r, r), Brush(fill));
}

float Gfx::Text(const std::wstring& s, Font f, const D2D1_RECT_F& r, D2D1_COLOR_F c, Align a, bool wrap) {
    auto layout = Layout(dw_.Get(), Format(f), s, r.right - r.left, wrap ? 10000.f : r.bottom - r.top, a, wrap);
    if (!layout) return 0;
    rt_->DrawTextLayout(D2D1::Point2F(r.left, r.top), layout.Get(), Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.height;
}

float Gfx::TextHeight(const std::wstring& s, Font f, float width) {
    auto layout = Layout(dw_.Get(), Format(f), s, width, 10000.f, Align::Left, true);
    DWRITE_TEXT_METRICS m{};
    if (layout) layout->GetMetrics(&m);
    return m.height;
}

float Gfx::TextWidth(const std::wstring& s, Font f) {
    auto layout = Layout(dw_.Get(), Format(f), s, 10000.f, 10000.f, Align::Left, false);
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

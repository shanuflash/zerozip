// Direct2D/DirectWrite drawing with Windows 11 (Fluent) colors and type.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

struct Theme {
    D2D1_COLOR_F text1, text2, text3, textDisabled;
    D2D1_COLOR_F card, cardStroke;
    D2D1_COLOR_F ctrl, ctrlHover, ctrlPress, ctrlDisabled, ctrlStroke, ctrlStrokeBottom;
    D2D1_COLOR_F subtleHover, subtlePress;
    D2D1_COLOR_F divider, background, footer, dashed;
    D2D1_COLOR_F accent, onAccent, accentDisabled, onAccentDisabled;
    D2D1_COLOR_F success, successBg, caution, cautionBg, critical, criticalBg, info, infoBg;
    D2D1_COLOR_F smoke, dialog, dialogFooter;
};

enum class Font { Caption, Body, BodyStrong, Subtitle, Title, Large, Icon16, Icon20, Icon40 };
enum class Align { Left, Center, Right };

class Gfx {
public:
    bool Init(HWND hwnd);
    void SetTheme(bool dark, bool mica, D2D1_COLOR_F accentOnDark, D2D1_COLOR_F accentOnLight);
    void SetDpi(UINT dpi);
    void Resize();
    bool Begin();   // false if there is nothing to draw on
    void End();
    D2D1_SIZE_F Size() const;
    float Scale() const { return dpi_ / 96.0f; }

    void Fill(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius = 0);
    void Stroke(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius = 0, float width = 1, bool dashed = false);
    void Line(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_COLOR_F c, float width = 1, bool dashed = false);
    void Circle(D2D1_POINT_2F c, float r, D2D1_COLOR_F fill);
    // Draws text in `r`, wrapped or cut off with an ellipsis. Returns the height used.
    float Text(const std::wstring& s, Font f, const D2D1_RECT_F& r, D2D1_COLOR_F c, Align a = Align::Left,
               bool wrap = false);
    float TextHeight(const std::wstring& s, Font f, float width);   // wrapped
    float TextWidth(const std::wstring& s, Font f);
    // A line chart of (0..1, value) points, optionally filled to the bottom.
    void Chart(const D2D1_RECT_F& r, const std::vector<std::pair<double, double>>& pts, double lo, double hi,
               D2D1_COLOR_F line, float width, bool fill);

    void PushClip(const D2D1_RECT_F& r);
    void PopClip();
    void Translate(float dy);   // shift everything drawn after this down by dy
    float OffsetY() const { return offsetY_; }

    Theme theme{};

private:
    IDWriteTextFormat* Format(Font f);
    ID2D1SolidColorBrush* Brush(D2D1_COLOR_F c);
    void Rebuild();

    HWND hwnd_ = nullptr;
    bool mica_ = false;
    UINT dpi_ = 96;
    float offsetY_ = 0;
    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IDWriteFactory> dw_;
    ComPtr<ID2D1HwndRenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1StrokeStyle> dash_;
    ComPtr<IDWriteTextFormat> formats_[9];
    std::wstring textFamily_, displayFamily_, iconFamily_;
};

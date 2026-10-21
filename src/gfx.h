// Direct2D/DirectWrite drawing in ZeroZip's own dark style.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

struct Theme {
    D2D1_COLOR_F bg, text1, text2, text3, textDisabled;
    D2D1_COLOR_F card, cardLine, dashed;
    D2D1_COLOR_F ghost, ghostHover, ghostLine;
    D2D1_COLOR_F accent, accentHover, accentInk, accentTint, accentLine;
    D2D1_COLOR_F warn, warnTint, warnLine, danger, dangerTint, dangerLine;
    D2D1_COLOR_F track;
};

enum class Font { Caption, Body, BodyStrong, Subtitle, Title, Mono, Icon16, Icon20, Icon28 };
enum class Align { Left, Center, Right };

class Gfx {
public:
    // Loads Geist and Geist Mono from the given RCDATA resources (WOFF2); falls back to Segoe UI.
    bool Init(HWND hwnd, HINSTANCE inst, int sansFont, int monoFont);
    void SetDpi(UINT dpi);
    void Resize();
    bool Begin();   // false if there is nothing to draw on
    void End();
    D2D1_SIZE_F Size() const;
    float Scale() const { return dpi_ / 96.0f; }

    void Fill(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius = 0);
    void Stroke(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius = 0, float width = 1, bool dashed = false);
    void Line(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_COLOR_F c, float width = 1);
    void Circle(D2D1_POINT_2F c, float r, D2D1_COLOR_F fill);
    // A smooth curve through `pts` (left to right) with the area under it filled, fading downwards.
    void Area(const D2D1_RECT_F& r, const std::vector<D2D1_POINT_2F>& pts, D2D1_COLOR_F line, D2D1_COLOR_F fill);
    // Draws text in `r`, wrapped or cut off with an ellipsis. Returns the height used.
    float Text(const std::wstring& s, Font f, const D2D1_RECT_F& r, D2D1_COLOR_F c, Align a = Align::Left,
               bool wrap = false);
    float TextHeight(const std::wstring& s, Font f, float width);   // wrapped
    float TextWidth(const std::wstring& s, Font f);

    void PushClip(const D2D1_RECT_F& r);
    void PopClip();
    void Translate(float dy);   // shift everything drawn after this down by dy
    float OffsetY() const { return offsetY_; }

    Theme theme{};

private:
    bool LoadFonts(HINSTANCE inst, int sansFont, int monoFont);
    IDWriteTextFormat* Format(Font f);
    ComPtr<IDWriteTextLayout> Layout(const std::wstring& s, Font f, float w, float h, Align a, bool wrap);
    ID2D1SolidColorBrush* Brush(D2D1_COLOR_F c);
    void Rebuild();

    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    float offsetY_ = 0;
    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IDWriteFactory> dw_;
    ComPtr<IDWriteFontCollection> fonts_;   // the embedded fonts, or null for the system's
    ComPtr<ID2D1HwndRenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1StrokeStyle> dash_, round_;
    ComPtr<IDWriteTextFormat> formats_[9];
    std::wstring sans_, mono_, icons_;
};

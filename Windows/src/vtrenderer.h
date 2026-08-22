#pragma once
#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <cstdint>
#include <string>

#include "terminal.h"

namespace dc::renderer {

constexpr uint8_t kDefaultBg[3] = {0x0A, 0x0A, 0x0A};
constexpr uint8_t kDefaultFg[3] = {0xD6, 0xD6, 0xD6};

class TermRenderer {
public:
    TermRenderer() = default;
    ~TermRenderer();

    TermRenderer(const TermRenderer&) = delete;
    TermRenderer& operator=(const TermRenderer&) = delete;

    bool Initialize(HWND hwnd);
    void Shutdown();

    void HandleResize(HWND hwnd);

    bool Render(dc::terminal::Terminal& terminal, int topOffset = 0,
                const dc::terminal::SelectionRange* selection = nullptr);

    int RowsForHeight(int height) const;
    int ColsForWidth(int width) const;
    dc::terminal::SelectionPoint CellAtPoint(POINT point, int topOffset,
                                             int rows, int cols) const;

private:
    ID2D1SolidColorBrush* SetBrushColor(uint8_t r, uint8_t g, uint8_t b);
    void DrawCellText(int left, int top, int right, int bottom,
                      int viewportRight, int viewportBottom,
                      const std::wstring& text,
                      bool bold, bool italic, uint8_t underline, bool strike,
                      uint8_t r, uint8_t g, uint8_t b);
    void DrawCellDecorations(int left, int top, int right, int bottom,
                             uint8_t underline, bool strike,
                             ID2D1SolidColorBrush* brush);
    bool DrawBlockElement(uint32_t codepoint, int left, int top, int right,
                          int bottom, ID2D1SolidColorBrush* brush);
    void DrawCursor(int left, int top, int right, int bottom);

    ID2D1Factory* factory_ = nullptr;
    IDWriteFactory* writeFactory_ = nullptr;
    ID2D1HwndRenderTarget* target_ = nullptr;
    IDWriteTextFormat* regular_ = nullptr;
    IDWriteTextFormat* bold_ = nullptr;
    IDWriteTextFormat* italic_ = nullptr;
    IDWriteTextFormat* boldItalic_ = nullptr;
    ID2D1SolidColorBrush* solidBrush_ = nullptr;

    float cellWidth_ = 8.0f;
    float cellHeight_ = 16.0f;
    float baselineOffset_ = 0.0f;
    bool initialized_ = false;
};

}

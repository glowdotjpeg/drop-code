#pragma once
#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "terminal.h"

namespace dc::renderer {

constexpr uint8_t kDefaultBg[3] = {0x0A, 0x0A, 0x0A};
constexpr uint8_t kDefaultFg[3] = {0xD6, 0xD6, 0xD6};

struct CellSize {
    float width = 8.0f;
    float height = 16.0f;
};

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

    CellSize CellMetrics() const { return {cellWidth_, cellHeight_}; }

    int RowsForHeight(int height) const;
    int ColsForWidth(int width) const;

private:
    struct BrushKey {
        uint8_t r, g, b;
        bool operator<(const BrushKey& other) const {
            return (r << 16 | g << 8 | b) < (other.r << 16 | other.g << 8 | other.b);
        }
    };

    ID2D1SolidColorBrush* Brush(uint8_t r, uint8_t g, uint8_t b);
    void DrawCellText(int row, int col, int runWidth, float topOffset,
                      const std::wstring& text,
                      bool bold, bool italic, uint8_t underline, bool strike,
                      const D2D1_COLOR_F& fg);
    void DrawCursor(int row, int col, float topOffset);

    ID2D1Factory* factory_ = nullptr;
    IDWriteFactory* writeFactory_ = nullptr;
    ID2D1HwndRenderTarget* target_ = nullptr;
    IDWriteTextFormat* regular_ = nullptr;
    IDWriteTextFormat* bold_ = nullptr;
    IDWriteTextFormat* italic_ = nullptr;
    IDWriteTextFormat* boldItalic_ = nullptr;
    std::map<BrushKey, ID2D1SolidColorBrush*> brushes_;

    float cellWidth_ = 8.0f;
    float cellHeight_ = 16.0f;
    float baselineOffset_ = 0.0f;
    bool initialized_ = false;
};

}

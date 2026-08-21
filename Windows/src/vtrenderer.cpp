#include "vtrenderer.h"

#include <d2d1helper.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace dc::renderer {
namespace {

struct ColorRun {
    int startCol = 0;
    int endCol = 0;
    std::wstring text;
    bool bold = false;
    bool italic = false;
    uint8_t underline = VTERM_UNDERLINE_OFF;
    bool strike = false;
    bool hasBackground = false;
    uint8_t r = 0, g = 0, b = 0;
    uint8_t br = 0, bg = 0, bb = 0;
};

struct ResolvedCell {
    bool bold = false;
    bool italic = false;
    uint8_t underline = VTERM_UNDERLINE_OFF;
    bool strike = false;
    bool hasBackground = false;
    uint8_t r = 0, g = 0, b = 0;
    uint8_t br = 0, bg = 0, bb = 0;
};

bool SameRun(const ResolvedCell& cell, const ColorRun& run) {
    if (cell.bold != run.bold || cell.italic != run.italic ||
        cell.underline != run.underline || cell.strike != run.strike ||
        cell.hasBackground != run.hasBackground) {
        return false;
    }
    return cell.r == run.r && cell.g == run.g && cell.b == run.b &&
           cell.br == run.br && cell.bg == run.bg && cell.bb == run.bb;
}

void ResolveColor(VTermScreen* screen, const VTermColor& color,
                  uint8_t out[3], bool isDefault, const uint8_t theme[3]) {
    if (isDefault) {
        out[0] = theme[0];
        out[1] = theme[1];
        out[2] = theme[2];
        return;
    }
    VTermColor copy = color;
    vterm_screen_convert_color_to_rgb(screen, &copy);
    out[0] = copy.rgb.red;
    out[1] = copy.rgb.green;
    out[2] = copy.rgb.blue;
}

ResolvedCell ResolveCell(VTermScreen* screen, const VTermScreenCell& cell) {
    const bool fgDefault = VTERM_COLOR_IS_DEFAULT_FG(&cell.fg) != 0;
    const bool bgDefault = VTERM_COLOR_IS_DEFAULT_BG(&cell.bg) != 0;

    ResolvedCell resolved;
    uint8_t fg[3] = {};
    uint8_t bg[3] = {};
    ResolveColor(screen, cell.fg, fg, fgDefault, kDefaultFg);
    ResolveColor(screen, cell.bg, bg, bgDefault, kDefaultBg);

    if (cell.attrs.reverse != 0) {
        std::swap(fg[0], bg[0]);
        std::swap(fg[1], bg[1]);
        std::swap(fg[2], bg[2]);
        resolved.hasBackground = true;
    } else {
        resolved.hasBackground = !bgDefault;
    }

    if (cell.attrs.conceal != 0) {
        fg[0] = bg[0];
        fg[1] = bg[1];
        fg[2] = bg[2];
    }

    resolved.bold = cell.attrs.bold != 0;
    resolved.italic = cell.attrs.italic != 0;
    resolved.underline = static_cast<uint8_t>(cell.attrs.underline);
    resolved.strike = cell.attrs.strike != 0;
    resolved.r = fg[0];
    resolved.g = fg[1];
    resolved.b = fg[2];
    resolved.br = bg[0];
    resolved.bg = bg[1];
    resolved.bb = bg[2];
    return resolved;
}

void AppendCodePoint(std::wstring& text, uint32_t codepoint) {
    if (codepoint == 0 || codepoint == UINT32_MAX) return;
    if (codepoint <= 0xFFFF) {
        text.push_back(static_cast<wchar_t>(codepoint));
        return;
    }
    if (codepoint > 0x10FFFF) return;
    codepoint -= 0x10000;
    text.push_back(static_cast<wchar_t>(0xD800 + (codepoint >> 10)));
    text.push_back(static_cast<wchar_t>(0xDC00 + (codepoint & 0x3FF)));
}

}

TermRenderer::~TermRenderer() {
    Shutdown();
}

bool TermRenderer::Initialize(HWND hwnd) {
    if (initialized_ || factory_ || writeFactory_ || target_) Shutdown();

    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   &factory_);
    if (FAILED(hr)) {
        Shutdown();
        return false;
    }
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                             __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(&writeFactory_));
    if (FAILED(hr)) {
        Shutdown();
        return false;
    }

    RECT client{};
    GetClientRect(hwnd, &client);
    const UINT width = std::max(1L, client.right - client.left);
    const UINT height = std::max(1L, client.bottom - client.top);

    hr = factory_->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_IGNORE)),
        D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(width, height)),
        &target_);
    if (FAILED(hr)) {
        Shutdown();
        return false;
    }

    target_->SetDpi(96.0f, 96.0f);
    target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    const UINT dpi = GetDpiForWindow(hwnd);
    const FLOAT dpiScale = std::max(1.0f, static_cast<FLOAT>(dpi) / 96.0f);
    const FLOAT emSize = 14.0f * dpiScale;
    const wchar_t* family = L"Cascadia Mono";

    IDWriteFontCollection* collection = nullptr;
    writeFactory_->GetSystemFontCollection(&collection, TRUE);
    UINT32 familyIndex = 0;
    BOOL familyExists = FALSE;
    if (collection) {
        collection->FindFamilyName(family, &familyIndex, &familyExists);
        if (!familyExists) {
            family = L"Consolas";
            collection->FindFamilyName(family, &familyIndex, &familyExists);
        }
    }

    auto makeFormat = [&](DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style) {
        IDWriteTextFormat* format = nullptr;
        writeFactory_->CreateTextFormat(family, collection, weight, style,
                                        DWRITE_FONT_STRETCH_NORMAL, emSize,
                                        L"", &format);
        return format;
    };
    regular_ = makeFormat(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL);
    bold_ = makeFormat(DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL);
    italic_ = makeFormat(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_ITALIC);
    boldItalic_ = makeFormat(DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_ITALIC);

    IDWriteFontFamily* fontFamily = nullptr;
    IDWriteFont* font = nullptr;
    IDWriteFontFace* face = nullptr;
    if (collection &&
        familyExists &&
        SUCCEEDED(collection->GetFontFamily(familyIndex, &fontFamily)) &&
        fontFamily &&
        SUCCEEDED(fontFamily->GetFirstMatchingFont(
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, &font)) &&
        font && SUCCEEDED(font->CreateFontFace(&face)) && face) {
        DWRITE_FONT_METRICS metrics{};
        face->GetMetrics(&metrics);
        const FLOAT scale =
            emSize / static_cast<FLOAT>(metrics.designUnitsPerEm);
        UINT16 glyph = 0;
        const UINT32 mCodepoint = L'M';
        if (SUCCEEDED(face->GetGlyphIndices(&mCodepoint, 1, &glyph))) {
            DWRITE_GLYPH_METRICS gm{};
            face->GetDesignGlyphMetrics(&glyph, 1, &gm);
            cellWidth_ = gm.advanceWidth * scale;
        }
        if (cellWidth_ <= 0) cellWidth_ = emSize * 0.6f;
        cellHeight_ =
            (metrics.ascent + metrics.descent + metrics.lineGap) * scale;
        if (cellHeight_ <= 0) cellHeight_ = emSize * 1.2f;
        baselineOffset_ =
            metrics.ascent * scale +
            (cellHeight_ - (metrics.ascent + metrics.descent) * scale) / 2.0f;
    } else {
        cellWidth_ = emSize * 0.6f;
        cellHeight_ = emSize * 1.2f;
        baselineOffset_ = cellHeight_ * 0.8f;
    }
    if (face) face->Release();
    if (font) font->Release();
    if (fontFamily) fontFamily->Release();
    cellWidth_ = std::max(1.0f, cellWidth_);
    cellHeight_ = std::max(1.0f, cellHeight_);
    for (IDWriteTextFormat* format : {regular_, bold_, italic_, boldItalic_}) {
        if (!format) continue;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
                               cellHeight_, baselineOffset_);
    }
    if (collection) collection->Release();

    initialized_ = true;
    return true;
}

void TermRenderer::Shutdown() {
    for (auto& [key, brush] : brushes_) {
        (void)key;
        if (brush) brush->Release();
    }
    brushes_.clear();
    if (regular_) { regular_->Release(); regular_ = nullptr; }
    if (bold_) { bold_->Release(); bold_ = nullptr; }
    if (italic_) { italic_->Release(); italic_ = nullptr; }
    if (boldItalic_) { boldItalic_->Release(); boldItalic_ = nullptr; }
    if (target_) { target_->Release(); target_ = nullptr; }
    if (factory_) { factory_->Release(); factory_ = nullptr; }
    if (writeFactory_) { writeFactory_->Release(); writeFactory_ = nullptr; }
    initialized_ = false;
}

void TermRenderer::HandleResize(HWND hwnd) {
    if (!target_) return;
    RECT client{};
    GetClientRect(hwnd, &client);
    const UINT width = std::max(1L, client.right - client.left);
    const UINT height = std::max(1L, client.bottom - client.top);
    if (width == 0 || height == 0) return;
    const D2D1_SIZE_U pixelSize = target_->GetPixelSize();
    if (pixelSize.width != width || pixelSize.height != height) {
        target_->Resize(D2D1::SizeU(width, height));
    }
}

int TermRenderer::RowsForHeight(int height) const {
    if (cellHeight_ <= 0) return 24;
    return std::max(1, static_cast<int>(std::floor(height / cellHeight_)));
}

int TermRenderer::ColsForWidth(int width) const {
    if (cellWidth_ <= 0) return 80;
    return std::max(1, static_cast<int>(std::floor(width / cellWidth_)));
}

ID2D1SolidColorBrush* TermRenderer::Brush(uint8_t r, uint8_t g, uint8_t b) {
    BrushKey key{r, g, b};
    auto it = brushes_.find(key);
    if (it != brushes_.end()) return it->second;
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(target_->CreateSolidColorBrush(
            D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f),
            &brush))) {
        return nullptr;
    }
    brushes_[key] = brush;
    return brush;
}

void TermRenderer::DrawCellText(int row, int col, int runWidth, float topOffset,
                                const std::wstring& text, bool bold, bool italic,
                                uint8_t underline, bool strike,
                                const D2D1_COLOR_F& fg) {
    if (text.empty()) return;
    IDWriteTextFormat* format = regular_;
    if (bold && italic) format = boldItalic_;
    else if (bold) format = bold_;
    else if (italic) format = italic_;
    if (!format) return;

    ID2D1SolidColorBrush* brush = Brush(
        static_cast<uint8_t>(fg.r * 255.0f), static_cast<uint8_t>(fg.g * 255.0f),
        static_cast<uint8_t>(fg.b * 255.0f));
    if (!brush) return;

    const float x = static_cast<float>(col * cellWidth_);
    const float y = topOffset + static_cast<float>(row) * cellHeight_;
    const float textWidth = static_cast<float>(runWidth) * cellWidth_;

    target_->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format,
                      D2D1::RectF(x, y, x + textWidth, y + cellHeight_),
                      brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);

    if (underline != VTERM_UNDERLINE_OFF && brush) {
        const float lineHeight = std::max(1.0f, cellHeight_ / 14.0f);
        target_->FillRectangle(
            D2D1::RectF(x, y + baselineOffset_ + lineHeight * 0.5f,
                        x + textWidth,
                        y + baselineOffset_ + lineHeight * 1.5f),
            brush);
        if (underline == VTERM_UNDERLINE_DOUBLE) {
            target_->FillRectangle(
                D2D1::RectF(x, y + baselineOffset_ + lineHeight * 2.0f,
                            x + textWidth,
                            y + baselineOffset_ + lineHeight * 3.0f),
                brush);
        }
    }
    if (strike && brush) {
        const float lineHeight = std::max(1.0f, cellHeight_ / 14.0f);
        const float strikeY = y + baselineOffset_ * 0.62f;
        target_->FillRectangle(
            D2D1::RectF(x, strikeY, x + textWidth, strikeY + lineHeight), brush);
    }
}

void TermRenderer::DrawCursor(int row, int col, float topOffset) {
    if (row < 0 || col < 0) return;
    const float x = static_cast<float>(col) * cellWidth_;
    const float y = topOffset + static_cast<float>(row) * cellHeight_;
    ID2D1SolidColorBrush* brush = Brush(kDefaultFg[0], kDefaultFg[1], kDefaultFg[2]);
    if (!brush) return;
    target_->FillRectangle(
        D2D1::RectF(x, y, x + static_cast<float>(cellWidth_), y + cellHeight_),
        brush);
}

bool TermRenderer::Render(dc::terminal::Terminal& terminal, int topOffset,
                          const dc::terminal::SelectionRange* selection) {
    if (!initialized_ || !target_) return false;

    RECT client{};
    GetClientRect(target_->GetHwnd(), &client);
    const UINT width = static_cast<UINT>(client.right - client.left);
    const UINT height = static_cast<UINT>(client.bottom - client.top);
    if (width == 0 || height == 0) return true;
    topOffset = std::clamp(topOffset, 0, static_cast<int>(height));
    const D2D1_SIZE_U pixelSize = target_->GetPixelSize();
    if (pixelSize.width != width || pixelSize.height != height) {
        target_->Resize(D2D1::SizeU(width, height));
    }

    struct RenderRow {
        int row = 0;
        std::vector<ColorRun> runs;
    };
    std::vector<RenderRow> rowsToRender;
    int cursorRow = -1;
    int cursorCol = -1;
    bool cursorVisible = false;
    int terminalCols = 0;

    {
        std::lock_guard lock(terminal.Lock());
        VTermScreen* screen = terminal.Screen();
        if (!screen) {
            target_->BeginDraw();
            target_->Clear(D2D1::ColorF(kDefaultBg[0] / 255.0f,
                                        kDefaultBg[1] / 255.0f,
                                        kDefaultBg[2] / 255.0f, 1.0f));
            const HRESULT hr = target_->EndDraw();
            if (hr == D2DERR_RECREATE_TARGET) return false;
            return SUCCEEDED(hr);
        }

        const int rows = terminal.Rows();
        const int cols = terminal.Cols();
        terminalCols = cols;
        const int offset = terminal.ScrollOffsetLocked();
        const auto& scrollback = terminal.Scrollback();

        auto collectRow = [&](int row, int lineCols,
                              const VTermScreenCell* lineCells,
                              std::vector<ColorRun>& out) {
            out.clear();
            ColorRun current;
            bool hasCurrent = false;
            const int cellCount =
                lineCells ? lineCols : (row < rows ? cols : 0);
            for (int col = 0; col < cellCount; ++col) {
                VTermScreenCell cell{};
                if (lineCells) {
                    cell = lineCells[col];
                } else {
                    VTermPos pos{row, col};
                    if (!vterm_screen_get_cell(screen, pos, &cell)) continue;
                }

                const ResolvedCell resolved = ResolveCell(screen, cell);
                if (!hasCurrent || !SameRun(resolved, current)) {
                    if (hasCurrent &&
                        (!current.text.empty() || current.hasBackground)) {
                        out.push_back(std::move(current));
                    }
                    current = ColorRun{};
                    current.startCol = col;
                    current.endCol = col;
                    current.bold = resolved.bold;
                    current.italic = resolved.italic;
                    current.underline = resolved.underline;
                    current.strike = resolved.strike;
                    current.hasBackground = resolved.hasBackground;
                    current.r = resolved.r;
                    current.g = resolved.g;
                    current.b = resolved.b;
                    current.br = resolved.br;
                    current.bg = resolved.bg;
                    current.bb = resolved.bb;
                    hasCurrent = true;
                }

                current.endCol = std::max(
                    current.endCol, col + std::max(1, static_cast<int>(cell.width)));
                if (cell.attrs.conceal == 0) {
                    for (int ch = 0; ch < VTERM_MAX_CHARS_PER_CELL; ++ch) {
                        if (cell.chars[ch] == 0) break;
                        AppendCodePoint(current.text, cell.chars[ch]);
                    }
                }
            }
            if (hasCurrent &&
                (!current.text.empty() || current.hasBackground)) {
                out.push_back(std::move(current));
            }
        };

        const int sbVisible = std::min(
            {offset, static_cast<int>(scrollback.size()), std::max(0, rows)});
        const int screenRowsToShow = std::max(0, rows - sbVisible);
        std::vector<ColorRun> runs;

        for (int i = 0; i < sbVisible; ++i) {
            const auto& line = scrollback[scrollback.size() - sbVisible + i];
            collectRow(i, static_cast<int>(line.cells.size()),
                       line.cells.data(), runs);
            rowsToRender.push_back(RenderRow{i, std::move(runs)});
            runs.clear();
        }

        for (int row = 0; row < screenRowsToShow; ++row) {
            collectRow(row, 0, nullptr, runs);
            rowsToRender.push_back(RenderRow{row + sbVisible, std::move(runs)});
            runs.clear();
        }

        cursorRow = terminal.CursorRow() + sbVisible;
        cursorCol = terminal.CursorCol();
        cursorVisible = terminal.CursorVisible();
    }

    target_->BeginDraw();
    target_->Clear(D2D1::ColorF(kDefaultBg[0] / 255.0f, kDefaultBg[1] / 255.0f,
                                kDefaultBg[2] / 255.0f, 1.0f));
    target_->PushAxisAlignedClip(
        D2D1::RectF(0.0f, static_cast<float>(topOffset),
                    static_cast<float>(width), static_cast<float>(height)),
        D2D1_ANTIALIAS_MODE_ALIASED);

    auto renderRow = [&](int screenRow, const std::vector<ColorRun>& runs) {
        const float y = static_cast<float>(topOffset) +
                        static_cast<float>(screenRow) * cellHeight_;

        for (const ColorRun& run : runs) {
            const float x = static_cast<float>(run.startCol) * cellWidth_;
            const float w = static_cast<float>(run.endCol - run.startCol) * cellWidth_;
            if (run.hasBackground &&
                (run.br != kDefaultBg[0] || run.bg != kDefaultBg[1] ||
                 run.bb != kDefaultBg[2])) {
                ID2D1SolidColorBrush* bgBrush = Brush(run.br, run.bg, run.bb);
                if (bgBrush) {
                    target_->FillRectangle(
                        D2D1::RectF(x, y, x + w, y + cellHeight_), bgBrush);
                }
            }
        }

        if (selection && terminalCols > 0) {
            int selectionStart = 0;
            int selectionEnd = 0;
            if (selection->BoundsForRow(screenRow, terminalCols,
                                        selectionStart, selectionEnd)) {
                ID2D1SolidColorBrush* selectionBrush =
                    Brush(56, 103, 181);
                if (selectionBrush) {
                    const float x =
                        static_cast<float>(selectionStart) * cellWidth_;
                    const float endX =
                        static_cast<float>(selectionEnd) * cellWidth_;
                    target_->FillRectangle(
                        D2D1::RectF(x, y, endX, y + cellHeight_),
                        selectionBrush);
                }
            }
        }

        for (const ColorRun& run : runs) {
            if (!run.text.empty()) {
                DrawCellText(screenRow, run.startCol, run.endCol - run.startCol,
                             static_cast<float>(topOffset),
                             run.text, run.bold, run.italic, run.underline,
                             run.strike,
                             D2D1::ColorF(run.r / 255.0f, run.g / 255.0f,
                                          run.b / 255.0f, 1.0f));
            }
        }
    };

    for (const RenderRow& row : rowsToRender) {
        renderRow(row.row, row.runs);
    }

    if (cursorVisible) {
        DrawCursor(cursorRow, cursorCol, static_cast<float>(topOffset));
    }

    target_->PopAxisAlignedClip();

    HRESULT hr = target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) return false;
    return SUCCEEDED(hr);
}

}

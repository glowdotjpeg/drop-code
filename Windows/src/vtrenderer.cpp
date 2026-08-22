#include "vtrenderer.h"

#include <d2d1helper.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace dc::renderer {
namespace {

struct RgbColor {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    bool operator==(const RgbColor&) const = default;
};

struct BackgroundRun {
    int startCol = 0;
    int endCol = 0;
    RgbColor color;
};

struct CellGlyph {
    int col = 0;
    int width = 1;
    std::wstring text;
    uint32_t primaryCodepoint = 0;
    int codepointCount = 0;
    bool bold = false;
    bool italic = false;
    uint8_t underline = VTERM_UNDERLINE_OFF;
    bool strike = false;
    RgbColor foreground;
};

struct ResolvedCell {
    bool bold = false;
    bool italic = false;
    uint8_t underline = VTERM_UNDERLINE_OFF;
    bool strike = false;
    RgbColor foreground;
    RgbColor background;
};

RgbColor ThemeColor(const uint8_t color[3]) {
    return {color[0], color[1], color[2]};
}

RgbColor ResolveColor(VTermScreen* screen, const VTermColor& color,
                      bool isDefault, const uint8_t theme[3]) {
    if (isDefault) {
        return ThemeColor(theme);
    }
    VTermColor copy = color;
    vterm_screen_convert_color_to_rgb(screen, &copy);
    return {copy.rgb.red, copy.rgb.green, copy.rgb.blue};
}

ResolvedCell ResolveCell(VTermScreen* screen, const VTermScreenCell& cell) {
    const bool fgDefault = VTERM_COLOR_IS_DEFAULT_FG(&cell.fg) != 0;
    const bool bgDefault = VTERM_COLOR_IS_DEFAULT_BG(&cell.bg) != 0;

    ResolvedCell resolved;
    RgbColor foreground = ResolveColor(screen, cell.fg, fgDefault, kDefaultFg);
    RgbColor background = ResolveColor(screen, cell.bg, bgDefault, kDefaultBg);

    if (cell.attrs.reverse != 0) {
        std::swap(foreground, background);
    }

    if (cell.attrs.conceal != 0) {
        foreground = background;
    }

    resolved.bold = cell.attrs.bold != 0;
    resolved.italic = cell.attrs.italic != 0;
    resolved.underline = static_cast<uint8_t>(cell.attrs.underline);
    resolved.strike = cell.attrs.strike != 0;
    resolved.foreground = foreground;
    resolved.background = background;
    return resolved;
}

ResolvedCell DefaultCell() {
    ResolvedCell cell;
    cell.foreground = ThemeColor(kDefaultFg);
    cell.background = ThemeColor(kDefaultBg);
    return cell;
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

std::vector<int> CellEdges(int origin, int count, float advance, int limit) {
    std::vector<int> edges(static_cast<size_t>(count) + 1);
    edges[0] = std::clamp(origin, 0, limit);
    for (int index = 1; index <= count; ++index) {
        const int edge = origin + static_cast<int>(std::lround(
                                      static_cast<double>(index) * advance));
        edges[static_cast<size_t>(index)] = std::clamp(edge, edges[0], limit);
    }
    return edges;
}

int FractionEdge(int start, int end, int numerator, int denominator) {
    const int extent = std::max(0, end - start);
    return start + (extent * numerator + denominator / 2) / denominator;
}

bool IsOrdinaryBatchableGlyph(const CellGlyph& glyph) {
    if (glyph.width != 1 || glyph.codepointCount != 1 ||
        glyph.text.size() != 1) {
        return false;
    }

    const uint32_t codepoint = glyph.primaryCodepoint;
    return codepoint >= 0x20 && codepoint != 0x7F &&
           (codepoint < 0x80 || codepoint > 0x9F) &&
           (codepoint < 0x2580 || codepoint > 0x259F) &&
           (codepoint < 0xD800 || codepoint > 0xDFFF);
}

bool HasMatchingTextStyle(const CellGlyph& left, const CellGlyph& right) {
    return left.bold == right.bold && left.italic == right.italic &&
           left.underline == right.underline && left.strike == right.strike &&
           left.foreground == right.foreground;
}

}

TermRenderer::~TermRenderer() {
    Shutdown();
}

bool TermRenderer::Initialize(HWND hwnd) {
    if (initialized_ || factory_ || writeFactory_ || target_ || solidBrush_) {
        Shutdown();
    }

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

    hr = target_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f),
                                        &solidBrush_);
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
    if (!regular_ || !bold_ || !italic_ || !boldItalic_) {
        if (collection) collection->Release();
        Shutdown();
        return false;
    }

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
    if (solidBrush_) { solidBrush_->Release(); solidBrush_ = nullptr; }
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

dc::terminal::SelectionPoint TermRenderer::CellAtPoint(
    POINT point, int topOffset, int rows, int cols) const {
    dc::terminal::SelectionPoint result{};
    if (rows <= 0 || cols <= 0 || !target_) return result;

    RECT client{};
    GetClientRect(target_->GetHwnd(), &client);
    const int width = std::max(1L, client.right - client.left);
    const int height = std::max(1L, client.bottom - client.top);
    topOffset = std::clamp(topOffset, 0, height);

    const std::vector<int> xEdges = CellEdges(0, cols, cellWidth_, width);
    const std::vector<int> yEdges =
        CellEdges(topOffset, rows, cellHeight_, height);
    const int x = std::clamp(static_cast<int>(point.x), 0, width - 1);
    const int y = std::clamp(static_cast<int>(point.y), topOffset,
                             std::max(topOffset, height - 1));

    result.col = std::clamp(
        static_cast<int>(std::upper_bound(xEdges.begin(), xEdges.end(), x) -
                         xEdges.begin()) -
            1,
        0, cols - 1);
    result.row = std::clamp(
        static_cast<int>(std::upper_bound(yEdges.begin(), yEdges.end(), y) -
                         yEdges.begin()) -
            1,
        0, rows - 1);
    return result;
}

ID2D1SolidColorBrush* TermRenderer::SetBrushColor(uint8_t r, uint8_t g,
                                                 uint8_t b) {
    if (!solidBrush_) return nullptr;
    solidBrush_->SetColor(
        D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f));
    return solidBrush_;
}

void TermRenderer::DrawCellText(int left, int top, int right, int bottom,
                                int viewportRight, int viewportBottom,
                                const std::wstring& text, bool bold, bool italic,
                                uint8_t underline, bool strike, uint8_t r,
                                uint8_t g, uint8_t b) {
    if (text.empty()) return;
    IDWriteTextFormat* format = regular_;
    if (bold && italic) format = boldItalic_;
    else if (bold) format = bold_;
    else if (italic) format = italic_;
    if (!format) return;

    ID2D1SolidColorBrush* brush = SetBrushColor(r, g, b);
    if (!brush) return;

    // The terminal viewport supplies the only ink clip. Giving each grapheme
    // the remainder of that viewport prevents DirectWrite from cutting italic,
    // fallback, wide, or combining glyph ink at a style-run boundary.
    target_->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format,
                      D2D1::RectF(static_cast<float>(left),
                                  static_cast<float>(top),
                                  static_cast<float>(viewportRight),
                                  static_cast<float>(viewportBottom)),
                      brush, D2D1_DRAW_TEXT_OPTIONS_NONE);

    DrawCellDecorations(left, top, right, bottom, underline, strike, brush);
}

void TermRenderer::DrawCellDecorations(int left, int top, int right, int bottom,
                                       uint8_t underline, bool strike,
                                       ID2D1SolidColorBrush* brush) {
    if (!brush || left >= right || top >= bottom) return;

    const int cellHeight = bottom - top;
    const int lineHeight =
        std::max(1, static_cast<int>(std::lround(cellHeight / 14.0)));
    auto fillLine = [&](int lineTop) {
        lineTop = std::clamp(lineTop, top, bottom);
        const int lineBottom = std::min(bottom, lineTop + lineHeight);
        if (lineTop < lineBottom) {
            target_->FillRectangle(
                D2D1::RectF(static_cast<float>(left),
                            static_cast<float>(lineTop),
                            static_cast<float>(right),
                            static_cast<float>(lineBottom)),
                brush);
        }
    };

    if (underline != VTERM_UNDERLINE_OFF) {
        const int firstUnderline =
            top + static_cast<int>(std::lround(baselineOffset_)) +
            std::max(1, lineHeight / 2);
        fillLine(firstUnderline);
        if (underline == VTERM_UNDERLINE_DOUBLE) {
            fillLine(firstUnderline + lineHeight * 2);
        }
    }
    if (strike) {
        fillLine(top + static_cast<int>(std::lround(baselineOffset_ * 0.62f)));
    }
}

bool TermRenderer::DrawBlockElement(uint32_t codepoint, int left, int top,
                                    int right, int bottom,
                                    ID2D1SolidColorBrush* brush) {
    if (codepoint < 0x2580 || codepoint > 0x259F) return false;
    if (!brush || left >= right || top >= bottom) return true;

    auto fill = [&](int fillLeft, int fillTop, int fillRight, int fillBottom) {
        fillLeft = std::clamp(fillLeft, left, right);
        fillRight = std::clamp(fillRight, left, right);
        fillTop = std::clamp(fillTop, top, bottom);
        fillBottom = std::clamp(fillBottom, top, bottom);
        if (fillLeft >= fillRight || fillTop >= fillBottom) return;
        target_->FillRectangle(
            D2D1::RectF(static_cast<float>(fillLeft),
                        static_cast<float>(fillTop),
                        static_cast<float>(fillRight),
                        static_cast<float>(fillBottom)),
            brush);
    };

    if (codepoint == 0x2580) {
        fill(left, top, right, FractionEdge(top, bottom, 1, 2));
        return true;
    }
    if (codepoint >= 0x2581 && codepoint <= 0x2588) {
        const int eighths = static_cast<int>(codepoint - 0x2580);
        fill(left, FractionEdge(top, bottom, 8 - eighths, 8), right, bottom);
        return true;
    }
    if (codepoint >= 0x2589 && codepoint <= 0x258F) {
        const int eighths = static_cast<int>(0x2590 - codepoint);
        fill(left, top, FractionEdge(left, right, eighths, 8), bottom);
        return true;
    }

    switch (codepoint) {
        case 0x2590:
            fill(FractionEdge(left, right, 1, 2), top, right, bottom);
            return true;
        case 0x2591:
        case 0x2592:
        case 0x2593: {
            const int coverage = static_cast<int>(codepoint - 0x2590);
            constexpr int bayer2x2[2][2] = {{0, 2}, {3, 1}};
            for (int y = top; y < bottom; ++y) {
                for (int x = left; x < right; ++x) {
                    if (bayer2x2[y & 1][x & 1] < coverage) {
                        fill(x, y, x + 1, y + 1);
                    }
                }
            }
            return true;
        }
        case 0x2594:
            fill(left, top, right, FractionEdge(top, bottom, 1, 8));
            return true;
        case 0x2595:
            fill(FractionEdge(left, right, 7, 8), top, right, bottom);
            return true;
        default:
            break;
    }

    const int middleX = FractionEdge(left, right, 1, 2);
    const int middleY = FractionEdge(top, bottom, 1, 2);
    unsigned int quadrantMask = 0;
    switch (codepoint) {
        case 0x2596: quadrantMask = 0x4; break;
        case 0x2597: quadrantMask = 0x8; break;
        case 0x2598: quadrantMask = 0x1; break;
        case 0x2599: quadrantMask = 0xD; break;
        case 0x259A: quadrantMask = 0x9; break;
        case 0x259B: quadrantMask = 0x7; break;
        case 0x259C: quadrantMask = 0xB; break;
        case 0x259D: quadrantMask = 0x2; break;
        case 0x259E: quadrantMask = 0x6; break;
        case 0x259F: quadrantMask = 0xE; break;
        default: return true;
    }
    if ((quadrantMask & 0x1) != 0) fill(left, top, middleX, middleY);
    if ((quadrantMask & 0x2) != 0) fill(middleX, top, right, middleY);
    if ((quadrantMask & 0x4) != 0) fill(left, middleY, middleX, bottom);
    if ((quadrantMask & 0x8) != 0) fill(middleX, middleY, right, bottom);
    return true;
}

void TermRenderer::DrawCursor(int left, int top, int right, int bottom) {
    if (left >= right || top >= bottom) return;
    ID2D1SolidColorBrush* brush =
        SetBrushColor(kDefaultFg[0], kDefaultFg[1], kDefaultFg[2]);
    if (!brush) return;
    target_->FillRectangle(
        D2D1::RectF(static_cast<float>(left), static_cast<float>(top),
                    static_cast<float>(right), static_cast<float>(bottom)),
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
        std::vector<BackgroundRun> backgrounds;
        std::vector<CellGlyph> glyphs;
    };
    std::vector<RenderRow> rowsToRender;
    int cursorRow = -1;
    int cursorCol = -1;
    bool cursorVisible = false;
    int terminalRows = 0;
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

        const int rows = terminal.RowsLocked();
        const int cols = terminal.ColsLocked();
        terminalRows = rows;
        terminalCols = cols;
        const auto& scrollback = terminal.Scrollback();
        const int scrollbackLines = static_cast<int>(scrollback.size());
        const int offset = std::clamp(terminal.ScrollOffsetLocked(), 0,
                                      scrollbackLines);
        const int firstVirtualRow = scrollbackLines - offset;

        auto collectRow = [&](int screenRow, int lineCols,
                              const VTermScreenCell* lineCells,
                              bool scrollbackRow, RenderRow& out) {
            out.backgrounds.clear();
            out.glyphs.clear();
            BackgroundRun currentBackground;
            bool hasBackground = false;
            for (int col = 0; col < cols; ++col) {
                VTermScreenCell cell{};
                bool haveCell = false;
                if (scrollbackRow && lineCells && col < lineCols) {
                    cell = lineCells[col];
                    haveCell = true;
                } else if (!scrollbackRow && screenRow >= 0 &&
                           screenRow < rows) {
                    VTermPos pos{screenRow, col};
                    haveCell = vterm_screen_get_cell(screen, pos, &cell) != 0;
                }

                const ResolvedCell resolved =
                    haveCell ? ResolveCell(screen, cell) : DefaultCell();
                if (!hasBackground ||
                    !(resolved.background == currentBackground.color)) {
                    if (hasBackground) {
                        out.backgrounds.push_back(currentBackground);
                    }
                    currentBackground = {col, col + 1, resolved.background};
                    hasBackground = true;
                } else {
                    currentBackground.endCol = col + 1;
                }

                if (!haveCell || cell.attrs.conceal != 0 ||
                    cell.chars[0] == 0 || cell.chars[0] == UINT32_MAX) {
                    continue;
                }

                CellGlyph glyph;
                glyph.col = col;
                glyph.width = std::clamp(
                    std::max(1, static_cast<int>(cell.width)), 1, cols - col);
                glyph.bold = resolved.bold;
                glyph.italic = resolved.italic;
                glyph.underline = resolved.underline;
                glyph.strike = resolved.strike;
                glyph.foreground = resolved.foreground;
                for (int ch = 0; ch < VTERM_MAX_CHARS_PER_CELL; ++ch) {
                    const uint32_t codepoint = cell.chars[ch];
                    if (codepoint == 0) break;
                    if (codepoint == UINT32_MAX || codepoint > 0x10FFFF) {
                        continue;
                    }
                    if (glyph.codepointCount == 0) {
                        glyph.primaryCodepoint = codepoint;
                    }
                    ++glyph.codepointCount;
                    AppendCodePoint(glyph.text, codepoint);
                }
                if (!glyph.text.empty()) out.glyphs.push_back(std::move(glyph));
            }
            if (hasBackground) {
                out.backgrounds.push_back(currentBackground);
            }
        };

        for (int viewportRow = 0; viewportRow < rows; ++viewportRow) {
            RenderRow renderedRow;
            renderedRow.row = viewportRow;
            const int virtualRow = firstVirtualRow + viewportRow;
            if (virtualRow >= 0 && virtualRow < scrollbackLines) {
                const auto& line = scrollback[virtualRow];
                collectRow(viewportRow, static_cast<int>(line.cells.size()),
                           line.cells.data(), true, renderedRow);
            } else if (virtualRow >= scrollbackLines &&
                       virtualRow < scrollbackLines + rows) {
                collectRow(virtualRow - scrollbackLines, 0, nullptr, false,
                           renderedRow);
            } else {
                collectRow(-1, 0, nullptr, true, renderedRow);
            }
            rowsToRender.push_back(std::move(renderedRow));
        }

        cursorRow = scrollbackLines + terminal.CursorRow() - firstVirtualRow;
        cursorCol = terminal.CursorCol();
        cursorVisible = offset == 0 && terminal.CursorVisible() &&
                        cursorRow >= 0 && cursorRow < rows &&
                        cursorCol >= 0 && cursorCol < cols;
    }

    const std::vector<int> xEdges = CellEdges(
        0, terminalCols, cellWidth_, static_cast<int>(width));
    const std::vector<int> yEdges = CellEdges(
        topOffset, terminalRows, cellHeight_, static_cast<int>(height));
    const int viewportRight = xEdges.empty() ? 0 : xEdges.back();
    const int viewportBottom = yEdges.empty() ? topOffset : yEdges.back();

    target_->BeginDraw();
    target_->Clear(D2D1::ColorF(kDefaultBg[0] / 255.0f, kDefaultBg[1] / 255.0f,
                                kDefaultBg[2] / 255.0f, 1.0f));
    const bool hasViewport = terminalCols > 0 && terminalRows > 0 &&
                             viewportRight > 0 && viewportBottom > topOffset;
    if (hasViewport) {
        target_->PushAxisAlignedClip(
            D2D1::RectF(0.0f, static_cast<float>(topOffset),
                        static_cast<float>(viewportRight),
                        static_cast<float>(viewportBottom)),
            D2D1_ANTIALIAS_MODE_ALIASED);
    }

    auto renderRow = [&](const RenderRow& row) {
        const int screenRow = row.row;
        if (!hasViewport || screenRow < 0 || screenRow >= terminalRows) return;
        const int top = yEdges[static_cast<size_t>(screenRow)];
        const int bottom = yEdges[static_cast<size_t>(screenRow + 1)];

        for (const BackgroundRun& run : row.backgrounds) {
            const int startCol = std::clamp(run.startCol, 0, terminalCols);
            const int endCol = std::clamp(run.endCol, startCol, terminalCols);
            if (startCol == endCol || run.color == ThemeColor(kDefaultBg)) {
                continue;
            }
            ID2D1SolidColorBrush* bgBrush =
                SetBrushColor(run.color.r, run.color.g, run.color.b);
            if (bgBrush) {
                const int left = xEdges[static_cast<size_t>(startCol)];
                const int right = xEdges[static_cast<size_t>(endCol)];
                target_->FillRectangle(
                    D2D1::RectF(static_cast<float>(left),
                                static_cast<float>(top),
                                static_cast<float>(right),
                                static_cast<float>(bottom)),
                    bgBrush);
            }
        }

        if (selection && terminalCols > 0) {
            int selectionStart = 0;
            int selectionEnd = 0;
            if (selection->BoundsForRow(screenRow, terminalCols,
                                        selectionStart, selectionEnd)) {
                ID2D1SolidColorBrush* selectionBrush =
                    SetBrushColor(56, 103, 181);
                if (selectionBrush) {
                    selectionStart =
                        std::clamp(selectionStart, 0, terminalCols);
                    selectionEnd =
                        std::clamp(selectionEnd, selectionStart, terminalCols);
                    target_->FillRectangle(
                        D2D1::RectF(
                            static_cast<float>(xEdges[static_cast<size_t>(
                                selectionStart)]),
                            static_cast<float>(top),
                            static_cast<float>(xEdges[static_cast<size_t>(
                                selectionEnd)]),
                            static_cast<float>(bottom)),
                        selectionBrush);
                }
            }
        }

        for (size_t glyphIndex = 0; glyphIndex < row.glyphs.size();) {
            const CellGlyph& glyph = row.glyphs[glyphIndex];
            const int startCol = std::clamp(glyph.col, 0, terminalCols);
            const int endCol = std::clamp(glyph.col + glyph.width,
                                          startCol, terminalCols);
            if (startCol == endCol || glyph.text.empty()) {
                ++glyphIndex;
                continue;
            }

            if (IsOrdinaryBatchableGlyph(glyph)) {
                std::wstring text = glyph.text;
                int runEndCol = glyph.col + 1;
                size_t runEnd = glyphIndex + 1;
                while (runEnd < row.glyphs.size()) {
                    const CellGlyph& next = row.glyphs[runEnd];
                    if (!IsOrdinaryBatchableGlyph(next) ||
                        next.col != runEndCol ||
                        !HasMatchingTextStyle(glyph, next)) {
                        break;
                    }
                    text.append(next.text);
                    ++runEndCol;
                    ++runEnd;
                }

                const int boundedRunEndCol =
                    std::clamp(runEndCol, startCol, terminalCols);
                DrawCellText(
                    xEdges[static_cast<size_t>(startCol)], top,
                    xEdges[static_cast<size_t>(boundedRunEndCol)], bottom,
                    viewportRight, viewportBottom, text, glyph.bold,
                    glyph.italic, glyph.underline, glyph.strike,
                    glyph.foreground.r, glyph.foreground.g,
                    glyph.foreground.b);
                glyphIndex = runEnd;
                continue;
            }

            const int left = xEdges[static_cast<size_t>(startCol)];
            const int right = xEdges[static_cast<size_t>(endCol)];
            ID2D1SolidColorBrush* brush = SetBrushColor(
                glyph.foreground.r, glyph.foreground.g, glyph.foreground.b);
            const bool blockDrawn =
                glyph.codepointCount == 1 &&
                DrawBlockElement(glyph.primaryCodepoint, left, top, right,
                                 bottom, brush);
            if (blockDrawn) {
                DrawCellDecorations(left, top, right, bottom, glyph.underline,
                                    glyph.strike, brush);
            } else {
                DrawCellText(left, top, right, bottom, viewportRight,
                             viewportBottom, glyph.text, glyph.bold,
                             glyph.italic, glyph.underline, glyph.strike,
                             glyph.foreground.r, glyph.foreground.g,
                             glyph.foreground.b);
            }
            ++glyphIndex;
        }
    };

    for (const RenderRow& row : rowsToRender) {
        renderRow(row);
    }

    if (hasViewport && cursorVisible && cursorRow >= 0 &&
        cursorRow < terminalRows && cursorCol >= 0 && cursorCol < terminalCols) {
        DrawCursor(xEdges[static_cast<size_t>(cursorCol)],
                   yEdges[static_cast<size_t>(cursorRow)],
                   xEdges[static_cast<size_t>(cursorCol + 1)],
                   yEdges[static_cast<size_t>(cursorRow + 1)]);
    }

    if (hasViewport) target_->PopAxisAlignedClip();

    HRESULT hr = target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) return false;
    return SUCCEEDED(hr);
}

}

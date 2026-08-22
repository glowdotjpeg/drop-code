#pragma once
#include <Windows.h>

#ifdef small
#undef small
#endif

#include <vterm.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace dc::terminal {

constexpr int kMaxScrollbackLines = 2000;

struct ScrollbackLine {
    std::vector<VTermScreenCell> cells;
    bool continuation = false;
};

struct SelectionPoint {
    int row = 0;
    int col = 0;
};

struct SelectionRange {
    bool active = false;
    SelectionPoint anchor{};
    SelectionPoint focus{};

    bool BoundsForRow(int row, int columnCount, int& startCol,
                      int& endCol) const {
        if (!active || columnCount <= 0) return false;

        SelectionPoint first = anchor;
        SelectionPoint last = focus;
        if (first.row > last.row ||
            (first.row == last.row && first.col > last.col)) {
            const SelectionPoint swapped = first;
            first = last;
            last = swapped;
        }
        if (row < first.row || row > last.row) return false;

        startCol = row == first.row ? first.col : 0;
        endCol = row == last.row ? last.col + 1 : columnCount;
        startCol = std::max(0, std::min(startCol, columnCount));
        endCol = std::max(0, std::min(endCol, columnCount));
        return startCol < endCol;
    }
};

class Terminal {
public:
    using InvalidateCallback = std::function<void()>;
    using ExitCallback = std::function<void()>;

    Terminal();
    ~Terminal();

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    bool Start(const std::wstring& launchCommand,
               const std::wstring& workingDirectory,
               int cols, int rows,
               uint64_t sessionId);
    void Stop();

    void Resize(int cols, int rows);

    void SendUnichar(uint32_t codepoint, VTermModifier mod);
    void SendKey(VTermKey key, VTermModifier mod);
    void SendPaste(const std::wstring& text);
    bool MouseReportingEnabled() const;
    std::wstring CopySelection(const SelectionRange& selection) const;
    void SendMouseMove(int row, int col, VTermModifier mod);
    void SendMouseButton(int button, bool pressed, VTermModifier mod);
    void SendMouseWheel(int row, int col, int direction, VTermModifier mod);

    void Scroll(int deltaLines);
    void ResetScroll();
    int ScrollOffset() const {
        std::lock_guard lock(mutex_);
        return scrollOffset_;
    }
    int ScrollOffsetLocked() const { return scrollOffset_; }
    int ScrollbackSize() const;

    int Rows() const { return rows_; }
    int Cols() const { return cols_; }

    std::mutex& Lock() { return mutex_; }
    VTermScreen* Screen() { return screen_; }
    const std::deque<ScrollbackLine>& Scrollback() const { return scrollback_; }

    void SetInvalidateCallback(InvalidateCallback callback) {
        invalidate_ = std::move(callback);
    }
    void SetExitCallback(ExitCallback callback) {
        exited_ = std::move(callback);
    }

    int CursorRow() const { return cursorRow_; }
    int CursorCol() const { return cursorCol_; }
    bool CursorVisible() const { return cursorVisible_; }

private:
    static void OutputCallback(const char* s, size_t len, void* user);
    void HandleOutput(const char* s, size_t len);

    static DWORD WINAPI ReaderThreadProc(LPVOID param);
    void ReaderLoop();
    static DWORD WINAPI WriterThreadProc(LPVOID param);
    void WriterLoop();

    static int OnDamage(VTermRect rect, void* user);
    static int OnMoveRect(VTermRect dest, VTermRect src, void* user);
    static int OnMoveCursor(VTermPos pos, VTermPos oldpos, int visible, void* user);
    static int OnSetTermProp(VTermProp prop, VTermValue* val, void* user);
    static int OnBell(void* user);
    static int OnScreenResize(int rows, int cols, void* user);
    static int OnSbPopLine(int cols, VTermScreenCell* cells, void* user);
    static int OnSbClear(void* user);
    static int OnSbPushLine4(int cols, const VTermScreenCell* cells, bool continuation, void* user);

    void Feed(const char* s, size_t len);
    void MarkDamaged() { damaged_ = true; }

    HPCON hpc_ = nullptr;
    HANDLE inRead_ = nullptr;
    HANDLE inWrite_ = nullptr;
    HANDLE outRead_ = nullptr;
    HANDLE outWrite_ = nullptr;
    HANDLE readerThread_ = nullptr;
    HANDLE writerThread_ = nullptr;
    HANDLE childProcess_ = nullptr;
    HANDLE childJob_ = nullptr;

    VTerm* vt_ = nullptr;
    VTermScreen* screen_ = nullptr;

    mutable std::mutex mutex_;
    std::deque<ScrollbackLine> scrollback_;
    int scrollOffset_ = 0;
    int rows_ = 24;
    int cols_ = 80;
    int mouseMode_ = VTERM_PROP_MOUSE_NONE;
    int cursorRow_ = 0;
    int cursorCol_ = 0;
    bool cursorVisible_ = true;
    bool damaged_ = false;
    InvalidateCallback invalidate_;
    ExitCallback exited_;
    std::condition_variable inputCondition_;
    std::deque<std::string> inputQueue_;
    bool inputClosed_ = true;
};

}

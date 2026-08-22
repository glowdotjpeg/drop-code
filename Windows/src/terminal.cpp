#include "terminal.h"

#include <cstdlib>

#include "launcher.h"

namespace dc::terminal {
namespace {

constexpr int kPipeBufferSize = 64 * 1024;
constexpr int kReadBufferSize = 16 * 1024;

std::wstring SystemCommandProcessorPath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const UINT length = GetSystemDirectoryW(
            buffer.data(), static_cast<UINT>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size()) {
            std::wstring path(buffer.data(), length);
            if (!path.empty() && path.back() != L'\\') path += L'\\';
            path += L"cmd.exe";
            return path;
        }
        buffer.resize(static_cast<size_t>(length) + 1);
    }
}

std::string Utf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                   static_cast<int>(wide.size()), nullptr, 0,
                                   nullptr, nullptr);
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        utf8.data(), size, nullptr, nullptr);
    return utf8;
}

void AppendSelectionCodePoint(std::wstring& text, uint32_t codepoint) {
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

void AppendSelectionCell(std::wstring& text, const VTermScreenCell& cell) {
    if (cell.chars[0] == UINT32_MAX) return;
    bool hasText = false;
    for (uint32_t codepoint : cell.chars) {
        if (codepoint == 0) break;
        AppendSelectionCodePoint(text, codepoint);
        hasText = true;
    }
    if (!hasText) text.push_back(L' ');
}

}

Terminal::Terminal() = default;

Terminal::~Terminal() {
    Stop();
}

bool Terminal::Start(const std::wstring& launchCommand,
                     const std::wstring& workingDirectory,
                     int cols, int rows,
                     uint64_t sessionId) {
    std::lock_guard lifecycleLock(lifecycleMutex_);
    StopImpl();

    {
        std::lock_guard lock(mutex_);
        rows_ = rows;
        cols_ = cols;
        inputQueue_.clear();
        inputClosed_ = false;
        cursorVisible_ = true;
    }

    if (!CreatePipe(&inRead_, &inWrite_, nullptr, kPipeBufferSize) ||
        !CreatePipe(&outRead_, &outWrite_, nullptr, kPipeBufferSize)) {
        StopImpl();
        return false;
    }
    if (!SetHandleInformation(inRead_, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(inWrite_, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(outRead_, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(outWrite_, HANDLE_FLAG_INHERIT, 0)) {
        StopImpl();
        return false;
    }

    const COORD size{static_cast<SHORT>(cols), static_cast<SHORT>(rows)};
    HPCON hpc = nullptr;
    HRESULT hr = CreatePseudoConsole(size, inRead_, outWrite_, 0, &hpc);
    if (FAILED(hr)) {
        StopImpl();
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        hpc_ = hpc;
    }

    STARTUPINFOEXW startupInfo{};
    startupInfo.StartupInfo.cb = sizeof(startupInfo);

    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<char> attributeBuffer(attributeSize);
    startupInfo.lpAttributeList =
        reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());
    if (!InitializeProcThreadAttributeList(startupInfo.lpAttributeList, 1, 0,
                                           &attributeSize)) {
        StopImpl();
        return false;
    }

    bool ok = UpdateProcThreadAttribute(
        startupInfo.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
        hpc, sizeof(hpc), nullptr, nullptr);

    const std::wstring scriptPath =
        launcher::ScriptPath(launchCommand, sessionId);
    if (scriptPath.empty()) {
        DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
        StopImpl();
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        scriptPath_ = scriptPath;
    }

    const std::wstring commandProcessor = SystemCommandProcessorPath();
    if (commandProcessor.empty()) {
        DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
        StopImpl();
        return false;
    }
    std::wstring commandLine = L"\"" + commandProcessor +
                               L"\" /d /k call \"" + scriptPath + L"\"";

    HANDLE childJob = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo{};
    jobInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!childJob || !SetInformationJobObject(
                         childJob, JobObjectExtendedLimitInformation, &jobInfo,
                         sizeof(jobInfo))) {
        if (childJob) CloseHandle(childJob);
        DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
        StopImpl();
        return false;
    }

    PROCESS_INFORMATION processInfo{};
    if (ok) {
        wchar_t* mutableCommandLine = commandLine.data();
        ok = CreateProcessW(
            commandProcessor.c_str(), mutableCommandLine, nullptr, nullptr,
            FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED, nullptr,
            workingDirectory.c_str(), &startupInfo.StartupInfo,
            &processInfo);
    }
    DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
    startupInfo.lpAttributeList = nullptr;

    if (!ok) {
        CloseHandle(childJob);
        StopImpl();
        return false;
    }

    if (!AssignProcessToJobObject(childJob, processInfo.hProcess) ||
        ResumeThread(processInfo.hThread) == static_cast<DWORD>(-1)) {
        TerminateProcess(processInfo.hProcess, 0);
        WaitForSingleObject(processInfo.hProcess, 500);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        CloseHandle(childJob);
        StopImpl();
        return false;
    }

    {
        std::lock_guard lock(mutex_);
        childProcess_ = processInfo.hProcess;
        childJob_ = childJob;
    }
    CloseHandle(processInfo.hThread);
    CloseHandle(inRead_);
    CloseHandle(outWrite_);
    inRead_ = nullptr;
    outWrite_ = nullptr;

    {
        std::lock_guard lock(mutex_);
        vt_ = vterm_new(rows, cols);
        if (vt_) {
            vterm_set_utf8(vt_, 1);
            vterm_output_set_callback(vt_, OutputCallback, this);
            screen_ = vterm_obtain_screen(vt_);

            static const VTermScreenCallbacks callbacks = [] {
                VTermScreenCallbacks value{};
                value.damage = OnDamage;
                value.moverect = OnMoveRect;
                value.movecursor = OnMoveCursor;
                value.settermprop = OnSetTermProp;
                value.bell = OnBell;
                value.resize = OnScreenResize;
                value.sb_pushline4 = OnSbPushLine4;
                value.sb_popline = OnSbPopLine;
                value.sb_clear = OnSbClear;
                return value;
            }();
            vterm_screen_set_callbacks(screen_, &callbacks, this);
            vterm_screen_callbacks_has_pushline4(screen_);
            vterm_screen_enable_altscreen(screen_, 1);
            vterm_screen_enable_reflow(screen_, true);

            vterm_screen_reset(screen_, 1);
        }
    }
    if (!vt_) {
        StopImpl();
        return false;
    }

    const HANDLE writerThread =
        CreateThread(nullptr, 0, WriterThreadProc, this, 0, nullptr);
    if (!writerThread) {
        StopImpl();
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        writerThread_ = writerThread;
    }

    const HANDLE readerThread =
        CreateThread(nullptr, 0, ReaderThreadProc, this, 0, nullptr);
    if (!readerThread) {
        StopImpl();
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        readerThread_ = readerThread;
    }
    return true;
}

void Terminal::Stop() {
    std::lock_guard lifecycleLock(lifecycleMutex_);
    StopImpl();
}

void Terminal::StopImpl() {
    HPCON hpc = nullptr;
    HANDLE readerThread = nullptr;
    HANDLE writerThread = nullptr;
    HANDLE childProcess = nullptr;
    HANDLE childJob = nullptr;
    std::wstring scriptPath;
    {
        std::lock_guard lock(mutex_);
        hpc = hpc_;
        hpc_ = nullptr;
        inputClosed_ = true;
        inputQueue_.clear();
        readerThread = readerThread_;
        writerThread = writerThread_;
        readerThread_ = nullptr;
        writerThread_ = nullptr;
        childProcess = childProcess_;
        childProcess_ = nullptr;
        childJob = childJob_;
        childJob_ = nullptr;
        scriptPath = scriptPath_;
    }
    inputCondition_.notify_all();
    if (childJob) {
        if (!TerminateJobObject(childJob, 0) && childProcess) {
            TerminateProcess(childProcess, 0);
        }
    } else if (childProcess) {
        TerminateProcess(childProcess, 0);
    }
    if (hpc) {
        ClosePseudoConsole(hpc);
    }

    if (childProcess) {
        DWORD wait = WaitForSingleObject(childProcess, 5000);
        if (wait != WAIT_OBJECT_0) {
            TerminateProcess(childProcess, 0);
            WaitForSingleObject(childProcess, INFINITE);
        }
    }

    for (HANDLE thread : {readerThread, writerThread}) {
        if (!thread) continue;
        CancelSynchronousIo(thread);
        DWORD wait = WaitForSingleObject(thread, 500);
        if (wait == WAIT_TIMEOUT) {
            CancelSynchronousIo(thread);
            WaitForSingleObject(thread, INFINITE);
        }
        CloseHandle(thread);
    }
    {
        std::lock_guard lock(mutex_);
        if (outRead_) {
            CloseHandle(outRead_);
            outRead_ = nullptr;
        }
        if (inWrite_) {
            CloseHandle(inWrite_);
            inWrite_ = nullptr;
        }
        if (inRead_) {
            CloseHandle(inRead_);
            inRead_ = nullptr;
        }
        if (outWrite_) {
            CloseHandle(outWrite_);
            outWrite_ = nullptr;
        }
        if (vt_) {
            vterm_free(vt_);
            vt_ = nullptr;
            screen_ = nullptr;
        }
        mouseMode_ = VTERM_PROP_MOUSE_NONE;
        alternateScreen_ = false;
        scrollback_.clear();
        scrollOffset_ = 0;
        cursorRow_ = 0;
        cursorCol_ = 0;
        cursorVisible_ = true;
        damaged_ = false;
        scriptPath_.clear();
    }
    if (childJob) CloseHandle(childJob);
    if (childProcess) CloseHandle(childProcess);
    if (!scriptPath.empty()) DeleteFileW(scriptPath.c_str());
}

void Terminal::Resize(int cols, int rows) {
    if (cols <= 0 || rows <= 0) return;
    std::lock_guard lifecycleLock(lifecycleMutex_);
    {
        std::lock_guard lock(mutex_);
        if (cols == cols_ && rows == rows_) return;
        rows_ = rows;
        cols_ = cols;
        if (vt_) {
            vterm_set_size(vt_, rows, cols);
        }
        if (scrollOffset_ > static_cast<int>(scrollback_.size())) {
            scrollOffset_ = static_cast<int>(scrollback_.size());
        }
        if (hpc_) {
            const COORD size{static_cast<SHORT>(cols), static_cast<SHORT>(rows)};
            ResizePseudoConsole(hpc_, size);
        }
    }
}

void Terminal::SendUnichar(uint32_t codepoint, VTermModifier mod) {
    std::lock_guard lock(mutex_);
    if (vt_) vterm_keyboard_unichar(vt_, codepoint, mod);
}

void Terminal::SendKey(VTermKey key, VTermModifier mod) {
    std::lock_guard lock(mutex_);
    if (vt_) vterm_keyboard_key(vt_, key, mod);
}

void Terminal::SendPaste(const std::wstring& text) {
    std::string utf8 = Utf8(text);
    if (utf8.empty()) return;
    std::lock_guard lock(mutex_);
    if (!vt_) return;
    vterm_keyboard_start_paste(vt_);
    HandleOutput(utf8.data(), utf8.size());
    vterm_keyboard_end_paste(vt_);
}

bool Terminal::MouseReportingEnabled() const {
    std::lock_guard lock(mutex_);
    return mouseMode_ != VTERM_PROP_MOUSE_NONE;
}

std::wstring Terminal::CopySelection(const SelectionRange& selection) const {
    std::lock_guard lock(mutex_);
    if (!selection.active || !screen_ || rows_ <= 0 || cols_ <= 0) return {};

    const int scrollbackLines = static_cast<int>(scrollback_.size());
    const int offset = std::clamp(scrollOffset_, 0, scrollbackLines);
    const int firstVirtualRow = scrollbackLines - offset;
    VTermState* state = vterm_obtain_state(vt_);
    std::wstring result;

    auto sourceForRow = [&](int viewportRow,
                            const std::vector<VTermScreenCell>*& scrollbackRow,
                            int& screenRow) {
        scrollbackRow = nullptr;
        screenRow = -1;

        const int virtualRow = firstVirtualRow + viewportRow;
        if (virtualRow < 0) return;
        if (virtualRow < scrollbackLines) {
            scrollbackRow = &scrollback_[virtualRow].cells;
            return;
        }

        const int candidateScreenRow = virtualRow - scrollbackLines;
        if (candidateScreenRow >= 0 && candidateScreenRow < rows_) {
            screenRow = candidateScreenRow;
        }
    };

    for (int row = 0; row < rows_; ++row) {
        int startCol = 0;
        int endCol = 0;
        if (!selection.BoundsForRow(row, cols_, startCol, endCol)) continue;

        const std::vector<VTermScreenCell>* scrollbackRow = nullptr;
        int screenRow = -1;
        sourceForRow(row, scrollbackRow, screenRow);

        const int availableColumns = scrollbackRow
                                         ? static_cast<int>(scrollbackRow->size())
                                         : screenRow >= 0 ? cols_ : 0;
        const int limit = std::min(endCol, availableColumns);
        std::wstring line;
        for (int col = std::min(startCol, limit); col < limit; ++col) {
            VTermScreenCell cell{};
            bool hasCell = false;
            if (scrollbackRow) {
                cell = (*scrollbackRow)[col];
                hasCell = true;
            } else if (screenRow >= 0 && screenRow < rows_) {
                hasCell = vterm_screen_get_cell(
                              screen_, VTermPos{screenRow, col}, &cell) != 0;
            }
            if (hasCell) AppendSelectionCell(line, cell);
        }

        while (!line.empty() &&
               (line.back() == L' ' || line.back() == L'\t')) {
            line.pop_back();
        }
        result += line;

        int nextStart = 0;
        int nextEnd = 0;
        if (selection.BoundsForRow(row + 1, cols_, nextStart, nextEnd)) {
            bool nextLineContinues = false;
            const std::vector<VTermScreenCell>* nextScrollbackRow = nullptr;
            int nextScreenRow = -1;
            sourceForRow(row + 1, nextScrollbackRow, nextScreenRow);
            if (nextScrollbackRow) {
                const int nextVirtualRow = firstVirtualRow + row + 1;
                nextLineContinues =
                    scrollback_[nextVirtualRow].continuation;
            } else if (state && nextScreenRow >= 0) {
                const VTermLineInfo* lineInfo =
                    vterm_state_get_lineinfo(state, nextScreenRow);
                nextLineContinues = lineInfo && lineInfo->continuation;
            }
            if (!nextLineContinues) result += L"\r\n";
        }
    }
    return result;
}

void Terminal::SendMouseMove(int row, int col, VTermModifier mod) {
    std::lock_guard lock(mutex_);
    if (vt_) vterm_mouse_move(vt_, row, col, mod);
}

void Terminal::SendMouseButtonAt(int row, int col, int button, bool pressed,
                                 VTermModifier mod) {
    std::lock_guard lock(mutex_);
    if (vt_) vterm_mouse_button_at(vt_, row, col, button, pressed, mod);
}

Terminal::WheelRoute Terminal::RouteWheel(int row, int col, int notches,
                                          VTermModifier mod) {
    if (notches == 0) return WheelRoute::Ignored;
    std::lock_guard lock(mutex_);
    return RouteWheelLocked(row, col, notches, mod);
}

std::optional<Terminal::WheelRoute> Terminal::TryRouteWheel(
    int row, int col, int notches, VTermModifier mod) {
    if (notches == 0) return WheelRoute::Ignored;
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return std::nullopt;
    return RouteWheelLocked(row, col, notches, mod);
}

Terminal::WheelRoute Terminal::RouteWheelLocked(int row, int col, int notches,
                                                VTermModifier mod) {
    if (!vt_) return WheelRoute::Ignored;

    if (mouseMode_ != VTERM_PROP_MOUSE_NONE) {
        if (scrollOffset_ != 0) {
            scrollOffset_ = 0;
            damaged_ = true;
        }
        const int button = notches > 0 ? 4 : 5;
        for (int i = 0; i < std::abs(notches); ++i) {
            vterm_mouse_button_at(vt_, row, col, button, true, mod);
        }
        return WheelRoute::Application;
    }

    VTermState* state = vterm_obtain_state(vt_);
    if (alternateScreen_) {
        if (state && vterm_state_get_alternate_scroll(state)) {
            constexpr int kAlternateScrollLines = 3;
            const VTermKey key = notches > 0 ? VTERM_KEY_UP : VTERM_KEY_DOWN;
            for (int notch = 0; notch < std::abs(notches); ++notch) {
                for (int line = 0; line < kAlternateScrollLines; ++line) {
                    vterm_keyboard_key(vt_, key, VTERM_MOD_NONE);
                }
            }
            return WheelRoute::Application;
        }
        return WheelRoute::Ignored;
    }

    const int oldOffset = scrollOffset_;
    scrollOffset_ = std::clamp(
        scrollOffset_ + notches, 0, static_cast<int>(scrollback_.size()));
    if (scrollOffset_ == oldOffset) return WheelRoute::Ignored;
    damaged_ = true;
    return WheelRoute::Scrollback;
}

void Terminal::SendFocus(bool focused) {
    std::lock_guard lock(mutex_);
    if (!vt_) return;
    VTermState* state = vterm_obtain_state(vt_);
    if (!state) return;
    if (focused) {
        vterm_state_focus_in(state);
    } else {
        vterm_state_focus_out(state);
    }
}

void Terminal::ResetScroll() {
    std::lock_guard lock(mutex_);
    if (scrollOffset_ != 0) {
        scrollOffset_ = 0;
        damaged_ = true;
    }
}

void Terminal::OutputCallback(const char* s, size_t len, void* user) {
    static_cast<Terminal*>(user)->HandleOutput(s, len);
}

void Terminal::HandleOutput(const char* s, size_t len) {
    if (!s || len == 0 || inputClosed_ || !inWrite_) return;
    inputQueue_.emplace_back(s, len);
    inputCondition_.notify_one();
}

DWORD WINAPI Terminal::ReaderThreadProc(LPVOID param) {
    static_cast<Terminal*>(param)->ReaderLoop();
    return 0;
}

void Terminal::ReaderLoop() {
    const HANDLE readHandle = outRead_;
    if (!readHandle) return;
    char buf[kReadBufferSize];
    for (;;) {
        DWORD n = 0;
        BOOL ok = ReadFile(readHandle, buf, sizeof(buf), &n, nullptr);
        if (!ok || n == 0) break;
        Feed(buf, n);
    }
    if (invalidate_) invalidate_();
    if (exited_) exited_();
}

DWORD WINAPI Terminal::WriterThreadProc(LPVOID param) {
    static_cast<Terminal*>(param)->WriterLoop();
    return 0;
}

void Terminal::WriterLoop() {
    for (;;) {
        std::string data;
        HANDLE writeHandle = nullptr;
        {
            std::unique_lock lock(mutex_);
            inputCondition_.wait(lock, [this] {
                return inputClosed_ || !inputQueue_.empty();
            });
            if (inputQueue_.empty()) return;
            data = std::move(inputQueue_.front());
            inputQueue_.pop_front();
            writeHandle = inWrite_;
        }
        if (!writeHandle) return;

        size_t offset = 0;
        while (offset < data.size()) {
            DWORD written = 0;
            const DWORD remaining = static_cast<DWORD>(
                std::min<size_t>(data.size() - offset, MAXDWORD));
            if (!WriteFile(writeHandle, data.data() + offset, remaining,
                           &written, nullptr) ||
                written == 0) {
                std::lock_guard lock(mutex_);
                inputClosed_ = true;
                inputQueue_.clear();
                return;
            }
            offset += written;
        }
    }
}

void Terminal::Feed(const char* s, size_t len) {
    bool shouldInvalidate = false;
    {
        std::lock_guard lock(mutex_);
        if (!vt_) return;
        vterm_input_write(vt_, s, len);
        shouldInvalidate = damaged_;
        damaged_ = false;
    }
    if (shouldInvalidate && invalidate_) {
        invalidate_();
    }
}

int Terminal::OnDamage(VTermRect, void* user) {
    static_cast<Terminal*>(user)->MarkDamaged();
    return 1;
}

int Terminal::OnMoveRect(VTermRect, VTermRect, void* user) {
    static_cast<Terminal*>(user)->MarkDamaged();
    return 1;
}

int Terminal::OnMoveCursor(VTermPos pos, VTermPos, int visible, void* user) {
    Terminal* self = static_cast<Terminal*>(user);
    self->cursorRow_ = pos.row;
    self->cursorCol_ = pos.col;
    self->cursorVisible_ = visible != 0;
    self->MarkDamaged();
    return 1;
}

int Terminal::OnSetTermProp(VTermProp prop, VTermValue* value, void* user) {
    Terminal* self = static_cast<Terminal*>(user);
    if (prop == VTERM_PROP_MOUSE && value) {
        self->mouseMode_ = value->number;
    } else if (prop == VTERM_PROP_ALTSCREEN && value) {
        self->alternateScreen_ = value->boolean;
        self->scrollOffset_ = 0;
    }
    self->MarkDamaged();
    return 1;
}

int Terminal::OnBell(void* user) {
    static_cast<Terminal*>(user)->MarkDamaged();
    return 1;
}

int Terminal::OnScreenResize(int, int, void* user) {
    static_cast<Terminal*>(user)->MarkDamaged();
    return 1;
}

int Terminal::OnSbPushLine4(int cols, const VTermScreenCell* cells,
                            bool continuation, void* user) {
    Terminal* self = static_cast<Terminal*>(user);
    const bool wasScrolled = self->scrollOffset_ > 0;
    self->scrollback_.push_back({});
    ScrollbackLine& line = self->scrollback_.back();
    line.continuation = continuation;
    line.cells.assign(cells, cells + cols);
    while (static_cast<int>(self->scrollback_.size()) > kMaxScrollbackLines) {
        self->scrollback_.pop_front();
    }
    if (wasScrolled) {
        self->scrollOffset_ = std::min(
            self->scrollOffset_ + 1,
            static_cast<int>(self->scrollback_.size()));
    }
    self->MarkDamaged();
    return 1;
}

int Terminal::OnSbPopLine(int cols, VTermScreenCell* cells, void* user) {
    Terminal* self = static_cast<Terminal*>(user);
    if (self->scrollback_.empty()) return 0;
    for (int col = 0; col < cols; ++col) {
        cells[col] = {};
        cells[col].width = 1;
        cells[col].fg.type = VTERM_COLOR_DEFAULT_FG;
        cells[col].bg.type = VTERM_COLOR_DEFAULT_BG;
    }
    const ScrollbackLine& line = self->scrollback_.back();
    size_t copyCount = line.cells.size();
    if (copyCount > static_cast<size_t>(cols)) copyCount = static_cast<size_t>(cols);
    std::copy_n(line.cells.begin(), copyCount, cells);
    self->scrollback_.pop_back();
    self->scrollOffset_ = std::min(
        self->scrollOffset_, static_cast<int>(self->scrollback_.size()));
    self->MarkDamaged();
    return 1;
}

int Terminal::OnSbClear(void* user) {
    Terminal* self = static_cast<Terminal*>(user);
    self->scrollback_.clear();
    self->scrollOffset_ = 0;
    self->MarkDamaged();
    return 1;
}

}

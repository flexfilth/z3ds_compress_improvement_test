#ifdef _WIN32

#include "frontend_common.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

constexpr UINT WM_APP_PROGRESS = WM_APP + 1;
constexpr UINT WM_APP_LOG = WM_APP + 2;
constexpr UINT WM_APP_DONE = WM_APP + 3;

struct ProgressPayload {
    std::size_t processed;
    std::size_t total;
    std::wstring label;
};

struct LogPayload {
    std::wstring message;
};

struct CompletionPayload {
    bool success;
    std::wstring summary;
};

struct AppState {
    HWND hwnd = nullptr;
    HWND input_edit = nullptr;
    HWND output_edit = nullptr;
    HWND batch_checkbox = nullptr;
    HWND recursive_checkbox = nullptr;
    HWND delete_checkbox = nullptr;
    HWND decompress_checkbox = nullptr;
    HWND frame_edit = nullptr;
    HWND level_edit = nullptr;
    HWND thread_edit = nullptr;
    HWND start_button = nullptr;
    HWND progress = nullptr;
    HWND log_view = nullptr;
    HWND status_label = nullptr;

    std::thread worker;
    std::atomic<bool> running{false};
};

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return L"";
    }
    int needed = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (needed <= 0) {
        return L"";
    }
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), needed);
    if (!result.empty() && result.back() == L'\0') {
        result.pop_back();
    }
    return result;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    int needed = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), needed, nullptr, nullptr);
    if (!result.empty() && result.back() == '\0') {
        result.pop_back();
    }
    return result;
}

std::wstring GetWindowTextWString(HWND control) {
    int length = GetWindowTextLengthW(control);
    std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, buffer.data(), length + 1);
    if (!buffer.empty() && buffer.back() == L'\0') {
        buffer.pop_back();
    }
    return buffer;
}

void AppendLog(HWND edit, const std::wstring& line) {
    int length = GetWindowTextLengthW(edit);
    SendMessageW(edit, EM_SETSEL, length, length);
    SendMessageW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
    SendMessageW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L"\r\n"));
}

void SetControlsEnabled(AppState* state, bool enabled) {
    EnableWindow(state->start_button, enabled);
    EnableWindow(state->input_edit, enabled);
    EnableWindow(state->output_edit, enabled);
    EnableWindow(state->batch_checkbox, enabled);
    EnableWindow(state->recursive_checkbox, enabled);
    EnableWindow(state->delete_checkbox, enabled);
    EnableWindow(state->decompress_checkbox, enabled);
    EnableWindow(state->frame_edit, enabled);
    EnableWindow(state->level_edit, enabled);
    EnableWindow(state->thread_edit, enabled);
}

std::wstring OpenFileDialog(HWND owner, bool decompress_mode) {
    wchar_t buffer[MAX_PATH] = {0};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = MAX_PATH;
    std::wstring filter;
    if (decompress_mode) {
        filter = L"Compressed Z3DS (*.zcia;*.zcci;*.zcxi;*.z3dsx)\0*.zcia;*.zcci;*.zcxi;*.z3dsx\0All files\0*.*\0\0";
    } else {
        filter = L"3DS ROMs (*.cia;*.cci;*.cxi;*.3dsx)\0*.cia;*.cci;*.cxi;*.3dsx\0All files\0*.*\0\0";
    }
    ofn.lpstrFilter = filter.c_str();
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        return buffer;
    }
    return L"";
}

std::wstring OpenFolderDialog(HWND owner) {
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = L"Select directory";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE result = SHBrowseForFolderW(&bi);
    if (!result) {
        return L"";
    }
    wchar_t path[MAX_PATH];
    if (!SHGetPathFromIDListW(result, path)) {
        CoTaskMemFree(result);
        return L"";
    }
    CoTaskMemFree(result);
    return path;
}

void UpdateProgressBar(AppState* state, std::size_t processed, std::size_t total, const std::wstring& label) {
    if (total == 0) {
        SendMessageW(state->progress, PBM_SETPOS, 0, 0);
        SetWindowTextW(state->status_label, L"Idle");
        return;
    }
    int percent = static_cast<int>((static_cast<double>(processed) / static_cast<double>(total)) * 100.0);
    SendMessageW(state->progress, PBM_SETPOS, percent, 0);
    std::wstring text = label + L" - " + std::to_wstring(percent) + L"%";
    SetWindowTextW(state->status_label, text.c_str());
}

void WorkerThread(AppState* state, std::wstring input, std::wstring output, bool batch_mode, bool recursive,
                  bool delete_source, bool decompress_mode, size_t frame_size_override, int compression_level,
                  unsigned int worker_count) {
    auto log = [&](const std::wstring& message) {
        auto payload = new LogPayload{message};
        PostMessageW(state->hwnd, WM_APP_LOG, reinterpret_cast<WPARAM>(payload), 0);
    };

    auto progress_bridge = [&](const std::wstring& label) {
        return [state, label](std::size_t processed, std::size_t total) {
            auto payload = new ProgressPayload{processed, total, label};
            PostMessageW(state->hwnd, WM_APP_PROGRESS, reinterpret_cast<WPARAM>(payload), 0);
        };
    };
    try {
        if (batch_mode) {
            auto dir = std::filesystem::path(WideToUtf8(input));
            auto files = CollectInputFiles(dir, recursive);
            if (files.empty()) {
                log(L"No supported files found in directory.");
            }
            for (const auto& file : files) {
                auto target = std::filesystem::path(GenerateOutputFilename(file));
                log(Utf8ToWide("Compressing " + file.filename().string()));
                auto progress = progress_bridge(Utf8ToWide(file.filename().string()));
                auto report = CompressSingleFile(file, target, frame_size_override, compression_level, worker_count,
                                                 progress);
                if (report.success) {
                    log(L"✔ " + Utf8ToWide(file.filename().string()) + L" (" + std::to_wstring(report.output_size) +
                        L" bytes)");
                    if (delete_source) {
                        std::error_code ec;
                        std::filesystem::remove(file, ec);
                        if (ec) {
                            log(L"Failed to delete source: " + Utf8ToWide(ec.message()));
                        }
                    }
                } else {
                    log(L"✖ " + Utf8ToWide(file.filename().string()) + L": " + Utf8ToWide(report.error_message));
                }
            }
            auto payload = new CompletionPayload{true, L"Batch finished"};
            PostMessageW(state->hwnd, WM_APP_DONE, reinterpret_cast<WPARAM>(payload), 0);
            return;
        }

        std::filesystem::path input_path = WideToUtf8(input);
        std::filesystem::path output_path = output.empty()
                                                ? (decompress_mode ? GenerateDecompressedFilename(input_path)
                                                                   : GenerateOutputFilename(input_path))
                                                : std::filesystem::path(WideToUtf8(output));

        auto progress = progress_bridge(Utf8ToWide(input_path.filename().string()));
        FileJobReport report = decompress_mode
                                   ? DecompressSingleFile(input_path, output_path, progress)
                                   : CompressSingleFile(input_path, output_path, frame_size_override, compression_level,
                                                        worker_count, progress);

        std::wstring summary;
        if (report.success) {
            summary = decompress_mode ? L"Decompression complete" : L"Compression complete";
            if (!decompress_mode && delete_source) {
                std::error_code ec;
                std::filesystem::remove(input_path, ec);
                if (ec) {
                    log(L"Failed to delete source: " + Utf8ToWide(ec.message()));
                }
            }
        } else {
            summary = Utf8ToWide(report.error_message);
        }

        auto payload = new CompletionPayload{report.success, summary};
        PostMessageW(state->hwnd, WM_APP_DONE, reinterpret_cast<WPARAM>(payload), 0);
    } catch (const std::exception& ex) {
        auto error = Utf8ToWide(ex.what());
        log(L"Error: " + error);
        auto payload = new CompletionPayload{false, error};
        PostMessageW(state->hwnd, WM_APP_DONE, reinterpret_cast<WPARAM>(payload), 0);
    }
}

void StartWork(AppState* state) {
    if (state->running.load()) {
        return;
    }

    std::wstring input = GetWindowTextWString(state->input_edit);
    std::wstring output = GetWindowTextWString(state->output_edit);
    bool batch_mode = SendMessageW(state->batch_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
    bool recursive = SendMessageW(state->recursive_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
    bool delete_source = SendMessageW(state->delete_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
    bool decompress_mode = SendMessageW(state->decompress_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;

    wchar_t buffer[32];
    GetWindowTextW(state->frame_edit, buffer, 32);
    size_t frame_size_override = 0;
    if (buffer[0] != L'\0') {
        frame_size_override = static_cast<size_t>(_wtoi64(buffer));
    }

    GetWindowTextW(state->level_edit, buffer, 32);
    int compression_level = buffer[0] ? _wtoi(buffer) : 15;

    GetWindowTextW(state->thread_edit, buffer, 32);
    unsigned int worker_count = buffer[0] ? static_cast<unsigned int>(_wtoi(buffer)) : std::thread::hardware_concurrency();
    if (worker_count == 0) {
        worker_count = 1;
    }

    if (batch_mode && decompress_mode) {
        AppendLog(state->log_view, L"Batch mode cannot be combined with decompression yet.");
        return;
    }

    if (input.empty()) {
        AppendLog(state->log_view, L"Select an input file or directory first.");
        return;
    }

    state->running = true;
    SetControlsEnabled(state, false);
    SendMessageW(state->progress, PBM_SETPOS, 0, 0);
    SetWindowTextW(state->status_label, L"Working...");

    state->worker = std::thread(WorkerThread, state, input, output, batch_mode, recursive, delete_source, decompress_mode,
                                frame_size_override, compression_level, worker_count);
}

void HandleBrowseInput(AppState* state) {
    bool batch_mode = SendMessageW(state->batch_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
    bool decompress_mode = SendMessageW(state->decompress_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
    std::wstring selection = batch_mode ? OpenFolderDialog(state->hwnd) : OpenFileDialog(state->hwnd, decompress_mode);
    if (!selection.empty()) {
        SetWindowTextW(state->input_edit, selection.c_str());
    }
}

void HandleBrowseOutput(AppState* state) {
    std::wstring selection = OpenFileDialog(state->hwnd, false);
    if (!selection.empty()) {
        SetWindowTextW(state->output_edit, selection.c_str());
    }
}

void OnWorkCompleted(AppState* state, CompletionPayload* payload) {
    bool success = payload->success;
    std::wstring summary = payload->summary;
    delete payload;
    AppendLog(state->log_view, summary);
    if (state->worker.joinable()) {
        state->worker.join();
    }
    state->running = false;
    SetControlsEnabled(state, true);
    SetWindowTextW(state->status_label, success ? L"Ready" : L"Failed");
    SendMessageW(state->progress, PBM_SETPOS, success ? 100 : 0, 0);
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppState* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        auto create_struct = reinterpret_cast<LPCREATESTRUCTW>(lParam);
        auto* new_state = new AppState{};
        new_state->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new_state));

        const int margin = 10;
        const int label_width = 80;
        const int edit_height = 24;
        const int button_width = 80;
        int y = margin;

        CreateWindowW(L"STATIC", L"Input:", WS_CHILD | WS_VISIBLE, margin, y + 4, label_width, edit_height, hwnd, nullptr,
                      create_struct->hInstance, nullptr);
        new_state->input_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                                margin + label_width, y, 360, edit_height, hwnd, (HMENU)100,
                                                create_struct->hInstance, nullptr);
        auto input_button = CreateWindowW(L"BUTTON", L"Browse", WS_CHILD | WS_VISIBLE, margin + label_width + 370, y,
                                          button_width, edit_height, hwnd, (HMENU)101, create_struct->hInstance, nullptr);
        y += edit_height + margin;

        CreateWindowW(L"STATIC", L"Output:", WS_CHILD | WS_VISIBLE, margin, y + 4, label_width, edit_height, hwnd, nullptr,
                      create_struct->hInstance, nullptr);
        new_state->output_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                                 margin + label_width, y, 360, edit_height, hwnd, (HMENU)102,
                                                 create_struct->hInstance, nullptr);
        auto output_button = CreateWindowW(L"BUTTON", L"Browse", WS_CHILD | WS_VISIBLE, margin + label_width + 370, y,
                                           button_width, edit_height, hwnd, (HMENU)103, create_struct->hInstance, nullptr);
        y += edit_height + margin;

        new_state->batch_checkbox = CreateWindowW(L"BUTTON", L"Batch (directory)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                                  margin, y, 160, edit_height, hwnd, (HMENU)104, create_struct->hInstance,
                                                  nullptr);
        new_state->recursive_checkbox = CreateWindowW(L"BUTTON", L"Recursive", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                                      margin + 170, y, 120, edit_height, hwnd, (HMENU)105,
                                                      create_struct->hInstance, nullptr);
        SendMessageW(new_state->recursive_checkbox, BM_SETCHECK, BST_CHECKED, 0);
        new_state->delete_checkbox = CreateWindowW(L"BUTTON", L"Delete source", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                                   margin + 300, y, 140, edit_height, hwnd, (HMENU)106,
                                                   create_struct->hInstance, nullptr);
        new_state->decompress_checkbox = CreateWindowW(L"BUTTON", L"Decompress", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                                       margin + 450, y, 140, edit_height, hwnd, (HMENU)107,
                                                       create_struct->hInstance, nullptr);
        y += edit_height + margin;

        CreateWindowW(L"STATIC", L"Frame bytes:", WS_CHILD | WS_VISIBLE, margin, y + 4, label_width + 20, edit_height,
                      hwnd, nullptr, create_struct->hInstance, nullptr);
        new_state->frame_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_NUMBER,
                                                margin + label_width + 20, y, 120, edit_height, hwnd, (HMENU)108,
                                                create_struct->hInstance, nullptr);
        CreateWindowW(L"STATIC", L"Level:", WS_CHILD | WS_VISIBLE, margin + 260, y + 4, 50, edit_height, hwnd, nullptr,
                      create_struct->hInstance, nullptr);
        new_state->level_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"15", WS_CHILD | WS_VISIBLE | ES_NUMBER,
                                                margin + 310, y, 60, edit_height, hwnd, (HMENU)109,
                                                create_struct->hInstance, nullptr);
        CreateWindowW(L"STATIC", L"Threads:", WS_CHILD | WS_VISIBLE, margin + 380, y + 4, 60, edit_height, hwnd, nullptr,
                      create_struct->hInstance, nullptr);
        new_state->thread_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_NUMBER,
                                                 margin + 440, y, 60, edit_height, hwnd, (HMENU)110,
                                                 create_struct->hInstance, nullptr);
        y += edit_height + margin;

        new_state->start_button = CreateWindowW(L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, margin, y,
                                                120, edit_height + 4, hwnd, (HMENU)111, create_struct->hInstance, nullptr);
        auto clear_button = CreateWindowW(L"BUTTON", L"Clear Log", WS_CHILD | WS_VISIBLE, margin + 130, y,
                                          120, edit_height + 4, hwnd, (HMENU)112, create_struct->hInstance, nullptr);
        new_state->status_label = CreateWindowW(L"STATIC", L"Idle", WS_CHILD | WS_VISIBLE, margin + 260, y + 6, 200,
                                                edit_height, hwnd, nullptr, create_struct->hInstance, nullptr);
        y += edit_height + margin;

        InitCommonControls();
        new_state->progress = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE,
                                              margin, y, 520, 20, hwnd, nullptr, create_struct->hInstance, nullptr);
        SendMessageW(new_state->progress, PBM_SETRANGE32, 0, 100);
        y += 30;

        new_state->log_view = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE |
                                                                                       ES_AUTOVSCROLL | ES_READONLY |
                                                                                       WS_VSCROLL,
                                              margin, y, 520, 200, hwnd, (HMENU)113, create_struct->hInstance, nullptr);

        state = new_state;
        SetFocus(new_state->input_edit);
        return 0;
    }
    case WM_COMMAND: {
        if (!state) {
            break;
        }
        switch (LOWORD(wParam)) {
        case 101:
            HandleBrowseInput(state);
            break;
        case 103:
            HandleBrowseOutput(state);
            break;
        case 111:
            StartWork(state);
            break;
        case 112:
            SetWindowTextW(state->log_view, L"");
            break;
        default:
            break;
        }
        return 0;
    }
    case WM_APP_PROGRESS: {
        auto payload = reinterpret_cast<ProgressPayload*>(wParam);
        if (state && payload) {
            UpdateProgressBar(state, payload->processed, payload->total, payload->label);
        }
        delete payload;
        return 0;
    }
    case WM_APP_LOG: {
        auto payload = reinterpret_cast<LogPayload*>(wParam);
        if (state && payload) {
            AppendLog(state->log_view, payload->message);
        }
        delete payload;
        return 0;
    }
    case WM_APP_DONE: {
        auto payload = reinterpret_cast<CompletionPayload*>(wParam);
        if (state && payload) {
            OnWorkCompleted(state, payload);
        }
        return 0;
    }
    case WM_DESTROY: {
        if (state) {
            if (state->worker.joinable()) {
                state->worker.join();
            }
            delete state;
        }
        PostQuitMessage(0);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int cmd_show) {
    INITCOMMONCONTROLSEX icc{sizeof(INITCOMMONCONTROLSEX), ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    const wchar_t kClassName[] = L"Z3DSCompressorGui";
    WNDCLASSEXW wc{sizeof(WNDCLASSEXW)};
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, kClassName, L"Z3DS Compressor GUI", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 600, 600, nullptr, nullptr, instance, nullptr);

    if (!hwnd) {
        return -1;
    }

    ShowWindow(hwnd, cmd_show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

#else

int main() {
    return 0;
}

#endif

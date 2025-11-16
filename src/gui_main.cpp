// Replacement GUI using Dear ImGui + DirectX11 (minimal integration)
//
// Notes:
// - This file implements a Win32 + D3D11 application that uses the bundled
//   ImGui backend files provided under external/imgui/backends/.
// - The GUI uses a simulated background worker so it will compile and run
//   even if your compression API has a different signature. Replace the
//   simulation in worker_thread_func with real calls to your compression API
//   when you're ready.
//
// Build requirements:
// - Windows (Win32) with D3D11 available.
// - C++20 toolchain (MSVC).
//
// Functionality implemented:
// - Input path, output directory, batch/recursive/delete-source/decompress toggles,
//   level slider (1–22), frame bytes, threads, Start/Stop button, progress bar,
//   and auto-scrolling log window.
// - File/folder browse dialogs (folder via SHBrowseForFolder, fallback to file open).
// - Background worker thread updating progress and logs via atomics and mutex.

#include <windows.h>
#include <d3d11.h>
#include <tchar.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>

#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <sstream>
#include <chrono>
#include <functional>
#include <filesystem>
#include <cstdio>
#include <cstdarg>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

// Optional compression headers (not required by this GUI stub)
#include "z3ds_compression.h"
#include "frontend_common.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

// Forward declarations for Win32/ImGui integration
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// D3D11 globals
static ID3D11Device*            g_pd3dDevice = NULL;
static ID3D11DeviceContext*     g_pd3dDeviceContext = NULL;
static IDXGISwapChain*          g_pSwapChain = NULL;
static ID3D11RenderTargetView*  g_mainRenderTargetView = NULL;

void CreateRenderTarget(IDXGISwapChain* swapChain)
{
    ID3D11Texture2D* pBackBuffer = NULL;
    swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
    if (pBackBuffer)
    {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = NULL; }
}

HRESULT CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
        createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd,
        &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (FAILED(hr))
        return hr;

    CreateRenderTarget(g_pSwapChain);
    return S_OK;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = NULL; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = NULL; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = NULL; }
}

// Application state
struct AppState {
    std::string inputPath;
    std::string outputDir;
    bool batch = false;
    bool recursive = false;
    bool deleteSource = false;
    bool decompress = false;
    int level = 6;
    int frameBytes = 0;
    int threads = 0;

    std::atomic<bool> running{false};
    std::atomic<int> progressPercent{0};
    std::atomic<uint64_t> processedBytes{0};
    std::atomic<uint64_t> totalBytes{0};

    std::mutex logMutex;
    std::vector<std::string> logs;
    void pushLog(const char* fmt, ...) {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        std::lock_guard<std::mutex> g(logMutex);
        logs.emplace_back(buf);
    }
} g_appState;

// File/folder dialogs
bool BrowseForFolder(HWND owner, std::string& outPath)
{
    wchar_t path[MAX_PATH];
    BROWSEINFOW bi = { 0 };
    bi.lpszTitle = L"Select folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl != NULL) {
        if (SHGetPathFromIDListW(pidl, path)) {
            char mb[MAX_PATH];
            WideCharToMultiByte(CP_UTF8, 0, path, -1, mb, MAX_PATH, NULL, NULL);
            outPath = mb;
            CoTaskMemFree(pidl);
            return true;
        }
        CoTaskMemFree(pidl);
    }
    return false;
}

bool OpenFileDialog(HWND owner, std::string& outFile)
{
    OPENFILENAMEA ofn;
    CHAR szFile[1024] = { 0 };
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = "All\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    if (GetOpenFileNameA(&ofn) == TRUE) {
        outFile = szFile;
        return true;
    }
    return false;
}

// Simulated worker (replace with real compression calls later)
void worker_thread_func()
{
    if (g_appState.running.load()) {
        g_appState.pushLog("Worker already running, ignoring start request.");
        return;
    }
    g_appState.running = true;
    g_appState.progressPercent = 0;
    g_appState.processedBytes = 0;
    g_appState.totalBytes = 0;
    g_appState.pushLog("Worker started");

    std::vector<std::filesystem::path> files;
    try {
        std::filesystem::path p(g_appState.inputPath);
        if (g_appState.batch) {
            if (std::filesystem::is_directory(p)) {
                if (g_appState.recursive) {
                    for (auto& it : std::filesystem::recursive_directory_iterator(p)) if (it.is_regular_file()) files.push_back(it.path());
                } else {
                    for (auto& it : std::filesystem::directory_iterator(p)) if (it.is_regular_file()) files.push_back(it.path());
                }
            } else {
                files.push_back(p);
            }
        } else {
            files.push_back(p);
        }
    } catch (const std::exception& ex) {
        g_appState.pushLog("Error enumerating input: %s", ex.what());
    }

    uint64_t totalBytes = 0;
    for (auto& f : files) {
        std::error_code ec;
        auto sz = std::filesystem::file_size(f, ec);
        if (!ec) totalBytes += sz;
    }
    g_appState.totalBytes = totalBytes;
    g_appState.pushLog("Found %zu files, total bytes %llu", files.size(), (unsigned long long)totalBytes);

    auto simulate_one_file = [&](const std::filesystem::path& infile) {
        uint64_t fileSize = 0;
        std::error_code ec;
        fileSize = std::filesystem::file_size(infile, ec);
        if (ec) fileSize = 0;
        const int steps = 60;
        for (int i = 0; i <= steps && g_appState.running; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30 + (rand() % 40)));
            uint64_t processed = (uint64_t)(((double)i / (double)steps) * (double)fileSize);
            // approximate processedBytes increment
            g_appState.processedBytes = std::min(g_appState.totalBytes.load(), g_appState.processedBytes.load() + processed / (steps+1) + 1);
            if (g_appState.totalBytes > 0) {
                int perc = (int)((g_appState.processedBytes * 100) / g_appState.totalBytes);
                g_appState.progressPercent = std::min(100, perc);
            } else {
                g_appState.progressPercent = (i * 100) / steps;
            }
        }
        g_appState.pushLog("Finished %s", infile.string().c_str());
    };

    if (files.empty()) {
        for (int i = 0; i <= 100 && g_appState.running; i += 2) {
            g_appState.progressPercent = i;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        g_appState.pushLog("No files to process.");
    } else {
        for (auto& f : files) {
            if (!g_appState.running) break;
            g_appState.pushLog("Processing %s ...", f.string().c_str());
            simulate_one_file(f);
        }
    }

    g_appState.progressPercent = 100;
    g_appState.pushLog("Worker finished.");
    g_appState.running = false;
}

// WinMain + ImGui setup
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L,
        GetModuleHandle(NULL), NULL, NULL, NULL, NULL,
        _T("z3ds_gui_window"), NULL };
    RegisterClassEx(&wc);
    HWND hwnd = CreateWindow(wc.lpszClassName, _T("z3ds GUI"),
        WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800,
        NULL, NULL, wc.hInstance, NULL);

    if (CreateDeviceD3D(hwnd) < 0) {
        CleanupDeviceD3D();
        UnregisterClass(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // try to load icon file if present (optional)
    std::filesystem::path icoPath = std::filesystem::current_path() / "z3ds_icon.ico";
    if (std::filesystem::exists(icoPath)) {
        HICON hIcon = (HICON)LoadImage(NULL, icoPath.string().c_str(), IMAGE_ICON, 64, 64, LR_LOADFROMFILE);
        if (hIcon) SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
    }

    bool done = false;
    std::thread worker;
    while (!done)
    {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowSize(ImVec2(1200, 700), ImGuiCond_FirstUseEver);
        ImGui::Begin("z3ds Compression Tool");

        static char inputBuf[1024] = "";
        static char outputBuf[1024] = "";
        ImGui::InputText("Input path (file or folder)", inputBuf, sizeof(inputBuf));
        ImGui::SameLine();
        if (ImGui::Button("Browse Input")) {
            std::string selected;
            if (BrowseForFolder(NULL, selected) && !selected.empty()) {
                strncpy(inputBuf, selected.c_str(), sizeof(inputBuf)-1);
            } else {
                std::string fileSelected;
                if (OpenFileDialog(NULL, fileSelected)) {
                    strncpy(inputBuf, fileSelected.c_str(), sizeof(inputBuf)-1);
                }
            }
        }

        ImGui::InputText("Output directory (optional)", outputBuf, sizeof(outputBuf));
        ImGui::SameLine();
        if (ImGui::Button("Browse Output")) {
            std::string selected;
            if (BrowseForFolder(NULL, selected)) {
                strncpy(outputBuf, selected.c_str(), sizeof(outputBuf)-1);
            }
        }

        ImGui::Separator();

        ImGui::Checkbox("Batch mode", &g_appState.batch);
        ImGui::SameLine();
        ImGui::Checkbox("Recursive", &g_appState.recursive);
        ImGui::SameLine();
        ImGui::Checkbox("Delete-source", &g_appState.deleteSource);
        ImGui::SameLine();
        ImGui::Checkbox("Decompress", &g_appState.decompress);

        ImGui::SliderInt("Level (1-22)", &g_appState.level, 1, 22);
        ImGui::InputInt("Frame bytes (0=auto)", &g_appState.frameBytes);
        ImGui::InputInt("Threads (0=auto)", &g_appState.threads);

        ImGui::Spacing();
        if (!g_appState.running) {
            if (ImGui::Button("Start")) {
                g_appState.inputPath = std::string(inputBuf);
                g_appState.outputDir = std::string(outputBuf);
                if (worker.joinable()) worker.join();
                worker = std::thread(worker_thread_func);
            }
        } else {
            if (ImGui::Button("Stop")) {
                g_appState.running = false;
                if (worker.joinable()) worker.join();
                g_appState.pushLog("Worker stopped by user.");
            }
        }

        float progress = (float)g_appState.progressPercent.load() / 100.0f;
        ImGui::ProgressBar(progress, ImVec2(-1, 0), std::to_string(g_appState.progressPercent.load()).c_str());

        ImGui::Separator();

        ImGui::BeginChild("LogWindow", ImVec2(0, 250), true, ImGuiWindowFlags_HorizontalScrollbar);
        {
            std::lock_guard<std::mutex> g(g_appState.logMutex);
            for (size_t i = 0; i < g_appState.logs.size(); ++i) {
                ImGui::Text("%s", g_appState.logs[i].c_str());
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();

        ImGui::End();

        ImGui::Render();
        const float clear_color_with_alpha[4] = { 0.1f, 0.1f, 0.12f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (worker.joinable()) {
        g_appState.running = false;
        worker.join();
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(NULL);
    UnregisterClass(_T("z3ds_gui_window"), GetModuleHandle(NULL));

    return 0;
}

// Win32 message handler required for ImGui_ImplWin32
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (g_pd3dDevice != NULL && wParam != SIZE_MINIMIZED)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget(g_pSwapChain);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

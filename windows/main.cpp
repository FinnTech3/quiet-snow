// Quiet Snow for Windows: a tray icon and one click-through overlay per display, drawn with Direct3D 11 into a
// DirectComposition swap chain. The snow itself lives in sim.hpp.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_3.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wtsapi32.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <string>
#include <vector>

#include "sim.hpp"
#include "version.h"

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "ole32.lib")
#endif

static const wchar_t* kVersion = QS_VERSION_W;

// ---------------------------------------------------------------- shaders

static const char* kShader = R"HLSL(
cbuffer U : register(b0) { float2 origin; float2 size; float t; float3 pad; };

struct FlakeIn { float4 a : A; float4 b : B; uint vid : SV_VertexID; };
struct V { float4 pos : SV_Position; float2 uv : UV; float alpha : AL; float shade : SH; };

V flakeV(FlakeIn i) {
    float2 corner = float2((i.vid & 1) ? 1.0 : -1.0, (i.vid & 2) ? 1.0 : -1.0);
    float2 p = i.a.xy + i.b.xy * t + corner * 0.5 * i.a.z - origin;
    float2 q = p / size;
    V o;
    o.pos = float4(q.x * 2.0 - 1.0, 1.0 - q.y * 2.0, 0.0, 1.0);
    o.uv = corner;
    o.alpha = i.a.w;
    o.shade = 0.0;
    return o;
}
float4 flakeP(V i) : SV_Target {
    float d = length(i.uv);
    float a = smoothstep(1.0, 0.25, d) * i.alpha;
    return float4(0.94 * a, 0.97 * a, 1.0 * a, a);
}

struct PileIn { float4 v : P; };
V pileV(PileIn i) {
    float2 q = (i.v.xy - origin) / size;
    V o;
    o.pos = float4(q.x * 2.0 - 1.0, 1.0 - q.y * 2.0, 0.0, 1.0);
    o.uv = float2(0, 0);
    o.alpha = i.v.z;
    o.shade = i.v.w;
    return o;
}
float4 pileP(V i) : SV_Target {
    float3 lit = float3(0.97, 0.985, 1.0);
    float3 shadow = float3(0.66, 0.76, 0.93);
    return float4(lerp(lit, shadow, i.shade) * i.alpha, i.alpha);
}
)HLSL";

struct Constants { float origin[2]; float size[2]; float t; float pad[3]; };

// ---------------------------------------------------------------- GPU

template <class T> static void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct Gpu {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGIDevice* dxgi = nullptr;
    IDXGIFactory2* factory = nullptr;
    IDCompositionDevice* dcomp = nullptr;
    ID3D11VertexShader *flakeVS = nullptr, *pileVS = nullptr;
    ID3D11PixelShader *flakePS = nullptr, *pilePS = nullptr;
    ID3D11InputLayout *flakeLayout = nullptr, *pileLayout = nullptr;
    ID3D11BlendState* blend = nullptr;
    ID3D11RasterizerState* raster = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11Buffer *flakeBuf = nullptr, *pileBuf = nullptr;
    UINT flakeCap = 0, pileCap = 0;
    bool warp = false;

    bool init(bool needDcomp) {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL got;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 3, D3D11_SDK_VERSION, &dev, &got, &ctx);
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 3, D3D11_SDK_VERSION, &dev, &got, &ctx);
            warp = true;
        }
        if (FAILED(hr)) return false;
        if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi))) return false;
        if (needDcomp) {
            IDXGIAdapter* adapter = nullptr;
            if (FAILED(dxgi->GetAdapter(&adapter))) return false;
            hr = adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
            adapter->Release();
            if (FAILED(hr)) return false;
            if (FAILED(DCompositionCreateDevice(dxgi, __uuidof(IDCompositionDevice), (void**)&dcomp))) return false;
        }
        return buildPipeline();
    }

    bool buildPipeline() {
        auto compile = [&](const char* entry, const char* target, ID3DBlob** out) {
            ID3DBlob* err = nullptr;
            HRESULT hr = D3DCompile(kShader, std::strlen(kShader), nullptr, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
            if (err) err->Release();
            return SUCCEEDED(hr);
        };
        ID3DBlob *fv = nullptr, *fp = nullptr, *pv = nullptr, *pp = nullptr;
        if (!compile("flakeV", "vs_4_0", &fv) || !compile("flakeP", "ps_4_0", &fp) ||
            !compile("pileV", "vs_4_0", &pv) || !compile("pileP", "ps_4_0", &pp)) return false;
        bool ok = SUCCEEDED(dev->CreateVertexShader(fv->GetBufferPointer(), fv->GetBufferSize(), nullptr, &flakeVS)) &&
                  SUCCEEDED(dev->CreatePixelShader(fp->GetBufferPointer(), fp->GetBufferSize(), nullptr, &flakePS)) &&
                  SUCCEEDED(dev->CreateVertexShader(pv->GetBufferPointer(), pv->GetBufferSize(), nullptr, &pileVS)) &&
                  SUCCEEDED(dev->CreatePixelShader(pp->GetBufferPointer(), pp->GetBufferSize(), nullptr, &pilePS));
        if (ok) {
            D3D11_INPUT_ELEMENT_DESC fl[] = {
                {"A", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
                {"B", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            };
            ok = SUCCEEDED(dev->CreateInputLayout(fl, 2, fv->GetBufferPointer(), fv->GetBufferSize(), &flakeLayout));
            D3D11_INPUT_ELEMENT_DESC pl[] = {{"P", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
            ok = ok && SUCCEEDED(dev->CreateInputLayout(pl, 1, pv->GetBufferPointer(), pv->GetBufferSize(), &pileLayout));
        }
        fv->Release(); fp->Release(); pv->Release(); pp->Release();
        if (!ok) return false;

        D3D11_BLEND_DESC bd = {};
        auto& rt = bd.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_ONE; rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(dev->CreateBlendState(&bd, &blend))) return false;
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        if (FAILED(dev->CreateRasterizerState(&rd, &raster))) return false;
        D3D11_BUFFER_DESC cbd = {};
        cbd.ByteWidth = sizeof(Constants); cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return SUCCEEDED(dev->CreateBuffer(&cbd, nullptr, &cb));
    }

    // Copy `data` into a dynamic vertex buffer, growing it when needed.
    bool upload(ID3D11Buffer*& buf, UINT& cap, const std::vector<float>& data) {
        const UINT bytes = (UINT)(data.size() * sizeof(float));
        if (bytes == 0) return false;
        if (!buf || cap < bytes) {
            release(buf);
            cap = std::max(bytes * 2, 4096u);
            D3D11_BUFFER_DESC d = {};
            d.ByteWidth = cap; d.Usage = D3D11_USAGE_DYNAMIC;
            d.BindFlags = D3D11_BIND_VERTEX_BUFFER; d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(dev->CreateBuffer(&d, nullptr, &buf))) { cap = 0; return false; }
        }
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx->Map(buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
        std::memcpy(m.pData, data.data(), bytes);
        ctx->Unmap(buf, 0);
        return true;
    }

    void draw(ID3D11RenderTargetView* rtv, UINT w, UINT h, float ox, float oy, float t, int pileN, int flakeN) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            Constants c = {{ox, oy}, {(float)w, (float)h}, t, {0, 0, 0}};
            std::memcpy(m.pData, &c, sizeof c);
            ctx->Unmap(cb, 0);
        }
        const float clear[4] = {0, 0, 0, 0};
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, clear);
        D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(raster);
        ctx->OMSetBlendState(blend, nullptr, 0xffffffff);
        ctx->VSSetConstantBuffers(0, 1, &cb);
        if (pileN > 0 && pileBuf) {
            UINT stride = 16, off = 0;
            ctx->IASetInputLayout(pileLayout);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->IASetVertexBuffers(0, 1, &pileBuf, &stride, &off);
            ctx->VSSetShader(pileVS, nullptr, 0);
            ctx->PSSetShader(pilePS, nullptr, 0);
            ctx->Draw((UINT)pileN, 0);
        }
        if (flakeN > 0 && flakeBuf) {
            UINT stride = 32, off = 0;
            ctx->IASetInputLayout(flakeLayout);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            ctx->IASetVertexBuffers(0, 1, &flakeBuf, &stride, &off);
            ctx->VSSetShader(flakeVS, nullptr, 0);
            ctx->PSSetShader(flakePS, nullptr, 0);
            ctx->DrawInstanced(4, (UINT)flakeN, 0, 0);
        }
    }
};

// ---------------------------------------------------------------- overlays

struct Overlay {
    HWND hwnd = nullptr;
    RECT mon{};          // monitor rectangle in virtual-screen pixels
    RECT work{};         // work area (monitor minus taskbar)
    IDXGISwapChain1* sc = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    IDCompositionTarget* target = nullptr;
    IDCompositionVisual* visual = nullptr;
    UINT w = 0, h = 0;
};

static const wchar_t* kOverlayClass = L"QuietSnowOverlay";
static const wchar_t* kHostClass = L"QuietSnowHost";
static const UINT WM_TRAY = WM_APP + 1;

static Gpu g_gpu;
static qs::Sim g_sim;
static std::vector<Overlay> g_overlays;
static HWND g_host = nullptr;
static HICON g_icon = nullptr;
static UINT g_taskbarCreated = 0;

static bool g_paused = false;       // user chose Pause
static bool g_away = false;         // locked, display off or asleep
static bool g_fullscreen = false;   // a full-screen app or presentation is up
static bool g_lowPower = false;
static std::vector<float> g_flakes, g_piles;
static int g_flakeN = 0, g_pileN = 0;
static double g_lastStep = 0, g_lastPoll = 0, g_lastSlow = 0, g_lastFrame = 0;
static int g_stepCount = 0;
static float g_glide = 0;

static double nowSeconds() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

// ---------------------------------------------------------------- settings

static const wchar_t* kKey = L"Software\\FinnTech3\\QuietSnow";
static DWORD getPref(const wchar_t* name, DWORD def) {
    DWORD v = def, sz = sizeof v;
    RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v;
}
static void setPref(const wchar_t* name, DWORD v) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof v);
        RegCloseKey(k);
    }
}

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static bool loginEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"QuietSnow", RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}
static void setLogin(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(k, L"QuietSnow", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"QuietSnow");
    }
    RegCloseKey(k);
}

static float windValue() { return (float)(int)getPref(L"wind", 0) ; }   // stored as a signed value in a DWORD

// ---------------------------------------------------------------- overlays: create and destroy

static LRESULT CALLBACK overlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

struct MonitorInfoLite { RECT mon, work; };
static BOOL CALLBACK enumMonitors(HMONITOR hm, HDC, LPRECT, LPARAM lp) {
    MONITORINFO mi = {sizeof mi};
    if (GetMonitorInfoW(hm, &mi)) reinterpret_cast<std::vector<MonitorInfoLite>*>(lp)->push_back({mi.rcMonitor, mi.rcWork});
    return TRUE;
}

static void destroyOverlays() {
    for (auto& o : g_overlays) {
        release(o.rtv); release(o.sc); release(o.visual); release(o.target);
        if (o.hwnd) DestroyWindow(o.hwnd);
    }
    g_overlays.clear();
    if (g_gpu.ctx) g_gpu.ctx->ClearState();
}

static bool createOverlay(const MonitorInfoLite& mi, Overlay& o) {
    o.mon = mi.mon; o.work = mi.work;
    o.w = (UINT)(mi.mon.right - mi.mon.left); o.h = (UINT)(mi.mon.bottom - mi.mon.top);
    o.hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE,
                             kOverlayClass, L"Quiet Snow", WS_POPUP, mi.mon.left, mi.mon.top, (int)o.w, (int)o.h,
                             nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!o.hwnd) return false;
    SetLayeredWindowAttributes(o.hwnd, 0, 255, LWA_ALPHA);

    DXGI_SWAP_CHAIN_DESC1 d = {};
    d.Width = o.w; d.Height = o.h;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(g_gpu.factory->CreateSwapChainForComposition(g_gpu.dev, &d, nullptr, &o.sc))) return false;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(o.sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) return false;
    HRESULT hr = g_gpu.dev->CreateRenderTargetView(back, nullptr, &o.rtv);
    back->Release();
    if (FAILED(hr)) return false;
    if (FAILED(g_gpu.dcomp->CreateTargetForHwnd(o.hwnd, TRUE, &o.target))) return false;
    if (FAILED(g_gpu.dcomp->CreateVisual(&o.visual))) return false;
    o.visual->SetContent(o.sc);
    o.target->SetRoot(o.visual);
    g_gpu.dcomp->Commit();
    ShowWindow(o.hwnd, SW_SHOWNOACTIVATE);
    return true;
}

static bool buildOverlays() {
    destroyOverlays();
    std::vector<MonitorInfoLite> mons;
    EnumDisplayMonitors(nullptr, nullptr, enumMonitors, (LPARAM)&mons);
    std::vector<qs::Rect> world;
    for (const auto& m : mons) {
        Overlay o;
        if (!createOverlay(m, o)) { destroyOverlays(); return false; }
        g_overlays.push_back(o);
        world.push_back({(float)m.mon.left, (float)m.mon.top, (float)(m.mon.right - m.mon.left), (float)(m.mon.bottom - m.mon.top)});
    }
    g_sim.setWorld(world);
    return !g_overlays.empty();
}

// ---------------------------------------------------------------- what is on screen

struct EnumCtx { std::vector<std::pair<int, qs::Rect>> rects; DWORD pid; };

static BOOL CALLBACK enumWindows(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumCtx*>(lp);
    if (ctx->rects.size() >= 40) return FALSE;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || IsZoomed(hwnd)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->pid) return TRUE;
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return TRUE;
    int cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
    if (cloaked) return TRUE;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd")) return TRUE;
    RECT r;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) GetWindowRect(hwnd, &r);
    const float w = (float)(r.right - r.left), h = (float)(r.bottom - r.top);
    if (w < 100 || h < 50) return TRUE;
    // A window pinned to the top and as wide as its screen is maximised or full screen: let the snow pass over it.
    for (const auto& m : g_sim.world()) {
        if (w >= m.w * 0.9f && (float)r.top < m.y + 40 && (float)r.left < m.maxX() && (float)r.right > m.x) return TRUE;
    }
    ctx->rects.push_back({(int)(INT_PTR)hwnd, {(float)r.left, (float)r.top, w, h}});
    return TRUE;
}

// Polling is slow while nothing moves and fast for a moment after any window moves, which is what a drag looks like.
static double g_fastUntil = 0;
static std::vector<std::pair<int, qs::Rect>> g_lastRects;

static void pollSurfaces() {
    EnumCtx ctx;
    ctx.pid = GetCurrentProcessId();
    EnumWindows(enumWindows, (LPARAM)&ctx);          // front to back
    bool moved = ctx.rects.size() != g_lastRects.size();
    for (size_t i = 0; !moved && i < ctx.rects.size(); ++i) {
        const auto& a = ctx.rects[i];
        const auto& b = g_lastRects[i];
        moved = a.first != b.first || a.second.x != b.second.x || a.second.y != b.second.y || a.second.w != b.second.w || a.second.h != b.second.h;
    }
    if (moved) g_fastUntil = nowSeconds() + 0.6;
    g_lastRects = ctx.rects;
    std::vector<qs::SurfaceInput> inputs;
    for (size_t i = 0; i < ctx.rects.size(); ++i) {
        qs::SurfaceInput in;
        in.id = ctx.rects[i].first;
        in.rect = {ctx.rects[i].second.x, ctx.rects[i].second.y, ctx.rects[i].second.w, 0};
        for (size_t k = 0; k < i; ++k) in.occluders.push_back(ctx.rects[k].second);
        inputs.push_back(std::move(in));
    }
    // The floor of each display: the top of the taskbar when it sits at the bottom, else the screen edge.
    for (size_t i = 0; i < g_overlays.size(); ++i) {
        const RECT& m = g_overlays[i].mon;
        bool stacked = false;
        for (size_t k = 0; k < g_overlays.size(); ++k) {
            if (k == i) continue;
            const RECT& o = g_overlays[k].mon;
            if (std::abs(o.top - m.bottom) < 2 && o.left < m.right && o.right > m.left) stacked = true;
        }
        if (stacked) continue;
        qs::SurfaceInput f;
        f.id = -(int)(i + 1);
        f.rect = {(float)m.left, (float)g_overlays[i].work.bottom, (float)(m.right - m.left), 0};
        f.isFloor = true;
        inputs.push_back(std::move(f));
    }
    g_sim.setSurfaces(inputs);
}

// ---------------------------------------------------------------- frames

static bool running() { return !g_paused && !g_away && !g_fullscreen && !g_overlays.empty(); }

static void frame() {
    const double now = nowSeconds();
    const double interval = g_lowPower ? 1.0 / 20 : 1.0 / 30;
    if (now - g_lastStep >= interval - 0.004) {
        const float dt = (float)(now - g_lastStep);
        g_lastStep = now;
        ++g_stepCount;
        if (now - g_lastPoll >= (now < g_fastUntil ? 0.05 : 0.15)) { g_lastPoll = now; pollSurfaces(); }

        POINT p;
        GetCursorPos(&p);
        qs::Pointer ptr{(float)p.x, (float)p.y};
        g_sim.step(dt, &ptr);
        g_flakeN = g_sim.fillFlakes(g_flakes);
        g_gpu.upload(g_gpu.flakeBuf, g_gpu.flakeCap, g_flakes);
        if (g_stepCount % 2 == 0 || g_pileN == 0) {
            g_pileN = g_sim.fillPiles(g_piles);
            g_gpu.upload(g_gpu.pileBuf, g_gpu.pileCap, g_piles);
        }
    }
    g_glide = (float)std::min(std::max(now - g_lastStep, 0.0), 0.07);
    for (auto& o : g_overlays) {
        g_gpu.draw(o.rtv, o.w, o.h, (float)o.mon.left, (float)o.mon.top, g_glide, g_pileN, g_flakeN);
        o.sc->Present(0, 0);
    }
}

// Slow checks, every two seconds: a full-screen app, battery saver.
static void slowChecks() {
    QUERY_USER_NOTIFICATION_STATE s = QUNS_ACCEPTS_NOTIFICATIONS;
    bool fs = false;
    if (SUCCEEDED(SHQueryUserNotificationState(&s))) fs = (s == QUNS_BUSY || s == QUNS_RUNNING_D3D_FULL_SCREEN || s == QUNS_PRESENTATION_MODE);
    SYSTEM_POWER_STATUS sps = {};
    const bool low = GetSystemPowerStatus(&sps) && sps.SystemStatusFlag == 1;
    if (low != g_lowPower) { g_lowPower = low; g_sim.setDensityScale(low ? 0.6f : 1.f); }
    if (fs != g_fullscreen) {
        g_fullscreen = fs;
        for (auto& o : g_overlays) ShowWindow(o.hwnd, fs ? SW_HIDE : SW_SHOWNOACTIVATE);
    }
}

// ---------------------------------------------------------------- tray

enum Id {
    ID_LIGHT = 1, ID_STEADY, ID_FLURRY,
    ID_WIND0, ID_WIND_R1, ID_WIND_L1, ID_WIND_R2, ID_WIND_L2,
    ID_MELT, ID_CLEAR, ID_PAUSE, ID_LOGIN, ID_QUIT,
};

static const int kWindValues[5] = {0, 60, -60, 220, -220};

static HICON makeIcon() {
    const int N = 32;
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof bi; bi.bV5Width = N; bi.bV5Height = -N; bi.bV5Planes = 1; bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00; bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    auto* px = (uint32_t*)bits;
    // Six arms, each with one pair of branches, drawn by distance to line segments so the edges are soft.
    auto segDist = [](float x, float y, float ax, float ay, float bx, float by) {
        const float vx = bx - ax, vy = by - ay;
        float t = ((x - ax) * vx + (y - ay) * vy) / (vx * vx + vy * vy);
        t = std::max(0.f, std::min(1.f, t));
        const float dx = x - (ax + t * vx), dy = y - (ay + t * vy);
        return std::sqrt(dx * dx + dy * dy);
    };
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            float d = 99;
            const float fx = x + 0.5f - N / 2.f, fy = y + 0.5f - N / 2.f;
            for (int k = 0; k < 6; ++k) {
                const float a = k * 3.14159265f / 3, c = std::cos(a), s = std::sin(a);
                auto rot = [&](float px, float py, float& ox, float& oy) { ox = px * c - py * s; oy = px * s + py * c; };
                float x0, y0, x1, y1, x2, y2, x3, y3;
                rot(0, 0, x0, y0); rot(0, -14, x1, y1);
                d = std::min(d, segDist(fx, fy, x0, y0, x1, y1));
                rot(-4.5f, -11.5f, x2, y2); rot(4.5f, -11.5f, x3, y3);
                d = std::min(d, segDist(fx, fy, x2, y2, x1, y1));
                d = std::min(d, segDist(fx, fy, x3, y3, x1, y1));
            }
            const float a = std::max(0.f, std::min(1.f, 1.6f - d));
            const uint32_t A = (uint32_t)(a * 255);
            // A mid blue reads on light and dark taskbars alike (premultiplied).
            const uint32_t R = (uint32_t)(0x4a * a), G = (uint32_t)(0x8c * a), B = (uint32_t)(0xe0 * a);
            px[y * N + x] = (A << 24) | (R << 16) | (G << 8) | B;
        }
    }
    HBITMAP mask = CreateBitmap(N, N, 1, 1, nullptr);
    ICONINFO ii = {TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color); DeleteObject(mask);
    return icon;
}

static void addTray() {
    NOTIFYICONDATAW n = {sizeof n};
    n.hWnd = g_host; n.uID = 1;
    n.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    n.uCallbackMessage = WM_TRAY;
    n.hIcon = g_icon ? g_icon : LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(n.szTip, L"Quiet Snow");
    Shell_NotifyIconW(NIM_ADD, &n);
}
static void removeTray() {
    NOTIFYICONDATAW n = {sizeof n};
    n.hWnd = g_host; n.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &n);
}

static void timerSync();

static void showMenu() {
    HMENU m = CreatePopupMenu();
    const qs::Weather w = g_sim.weather();
    AppendMenuW(m, MF_STRING | (w == qs::Weather::Light ? MF_CHECKED : 0), ID_LIGHT, L"Light");
    AppendMenuW(m, MF_STRING | (w == qs::Weather::Steady ? MF_CHECKED : 0), ID_STEADY, L"Steady");
    AppendMenuW(m, MF_STRING | (w == qs::Weather::Flurry ? MF_CHECKED : 0), ID_FLURRY, L"Flurry");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    const wchar_t* names[5] = {L"Still air", L"Breeze to the right", L"Breeze to the left", L"Gale to the right", L"Gale to the left"};
    for (int i = 0; i < 5; ++i) AppendMenuW(m, MF_STRING | ((int)g_sim.wind == kWindValues[i] ? MF_CHECKED : 0), ID_WIND0 + i, names[i]);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_sim.melts ? MF_CHECKED : 0), ID_MELT, L"Melt slowly");
    AppendMenuW(m, MF_STRING, ID_CLEAR, L"Clear piles");
    AppendMenuW(m, MF_STRING, ID_PAUSE, g_paused ? L"Resume" : L"Pause");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (loginEnabled() ? MF_CHECKED : 0), ID_LOGIN, L"Open at login");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_host);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, g_host, nullptr);
    PostMessageW(g_host, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (cmd) {
        case ID_LIGHT: g_sim.setWeather(qs::Weather::Light); setPref(L"weather", 0); break;
        case ID_STEADY: g_sim.setWeather(qs::Weather::Steady); setPref(L"weather", 1); break;
        case ID_FLURRY: g_sim.setWeather(qs::Weather::Flurry); setPref(L"weather", 2); break;
        case ID_WIND0: case ID_WIND_R1: case ID_WIND_L1: case ID_WIND_R2: case ID_WIND_L2: {
            const int v = kWindValues[cmd - ID_WIND0];
            g_sim.wind = (float)v; setPref(L"wind", (DWORD)v);
            break;
        }
        case ID_MELT: g_sim.melts = !g_sim.melts; setPref(L"melt", g_sim.melts ? 1 : 0); break;
        case ID_CLEAR: g_sim.clearPiles(); break;
        case ID_PAUSE: g_paused = !g_paused; setPref(L"paused", g_paused ? 1 : 0); timerSync(); break;
        case ID_LOGIN: setLogin(!loginEnabled()); break;
        case ID_QUIT: PostQuitMessage(0); break;
    }
}

// ---------------------------------------------------------------- host window and pacing

static UINT_PTR kSlowTimer = 1;
static void timerSync() {
    if (running()) { g_lastStep = nowSeconds(); }
    for (auto& o : g_overlays) ShowWindow(o.hwnd, (g_away || g_fullscreen) ? SW_HIDE : SW_SHOWNOACTIVATE);
}

static HPOWERNOTIFY g_powerNote = nullptr;
// GUID_CONSOLE_DISPLAY_STATE, spelled out so no extra library is needed to link it.
static const GUID kConsoleDisplayState = {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};

static LRESULT CALLBACK hostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == g_taskbarCreated && m != 0) { addTray(); return 0; }
    switch (m) {
        case WM_TRAY:
            if (l == WM_RBUTTONUP || l == WM_LBUTTONUP || l == WM_CONTEXTMENU) showMenu();
            return 0;
        case WM_TIMER:
            if (w == kSlowTimer) { slowChecks(); timerSync(); }
            return 0;
        case WM_WTSSESSION_CHANGE:
            if (w == WTS_SESSION_LOCK) { g_away = true; timerSync(); }
            else if (w == WTS_SESSION_UNLOCK) { g_away = false; timerSync(); }
            return 0;
        case WM_POWERBROADCAST:
            if (w == PBT_POWERSETTINGCHANGE) {
                auto* ps = (POWERBROADCAST_SETTING*)l;
                if (ps && ps->PowerSetting == kConsoleDisplayState && ps->DataLength >= sizeof(DWORD)) {
                    g_away = *(DWORD*)ps->Data == 0;   // 0 off, 1 on, 2 dimmed
                    timerSync();
                }
            } else if (w == PBT_APMSUSPEND) { g_away = true; timerSync(); }
            else if (w == PBT_APMRESUMEAUTOMATIC) { g_away = false; timerSync(); }
            return TRUE;
        case WM_DISPLAYCHANGE:
            if (g_gpu.dev) { buildOverlays(); pollSurfaces(); }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- snapshot (headless, for CI and the README)

#pragma pack(push, 1)
struct BmpHeader { uint16_t type = 0x4D42; uint32_t size; uint32_t reserved = 0; uint32_t offset = 54;
                   uint32_t hdr = 40; int32_t w; int32_t h; uint16_t planes = 1; uint16_t bpp = 32; uint32_t comp = 0;
                   uint32_t imgSize; int32_t xppm = 2835, yppm = 2835; uint32_t used = 0, important = 0; };
#pragma pack(pop)

static int snapshot(const wchar_t* path) {
    const int W = 1280, H = 800;
    Gpu gpu;
    if (!gpu.init(false)) { std::fprintf(stderr, "no Direct3D 11 device\n"); return 1; }
    qs::Sim sim(42);
    sim.setWeather(qs::Weather::Steady);
    sim.wind = 30;
    sim.setWorld({{0, 0, (float)W, (float)H}});
    const qs::Rect back{120, 150, 620, 420}, front{560, 300, 560, 340};
    const float dockTop = 742;
    sim.setSurfaces({{1, {front.x, front.y, front.w, 0}, {}, false},
                     {2, {back.x, back.y, back.w, 0}, {front}, false},
                     {-1, {0, dockTop, (float)W, 0}, {}, true}});
    sim.setWeather(qs::Weather::Flurry);
    for (int i = 0; i < 30 * 300; ++i) sim.step(1.f / 30);
    sim.setWeather(qs::Weather::Steady);
    for (int i = 0; i < 30 * 10; ++i) sim.step(1.f / 30);

    std::vector<float> flakes, piles;
    const int fn = sim.fillFlakes(flakes), pn = sim.fillPiles(piles);
    gpu.upload(gpu.flakeBuf, gpu.flakeCap, flakes);
    gpu.upload(gpu.pileBuf, gpu.pileCap, piles);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *tex = nullptr, *stage = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    if (FAILED(gpu.dev->CreateTexture2D(&td, nullptr, &tex)) || FAILED(gpu.dev->CreateRenderTargetView(tex, nullptr, &rtv))) return 1;
    gpu.draw(rtv, W, H, 0, 0, 0, pn, fn);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(gpu.dev->CreateTexture2D(&td, nullptr, &stage))) return 1;
    gpu.ctx->CopyResource(stage, tex);
    D3D11_MAPPED_SUBRESOURCE mp;
    if (FAILED(gpu.ctx->Map(stage, 0, D3D11_MAP_READ, 0, &mp))) return 1;

    // Composite the premultiplied snow over a drawn desktop, top-down BGRA.
    std::vector<uint32_t> out((size_t)W * H);
    auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const float t = (x / (float)W + y / (float)H) * 0.5f;
            float r = mix(26, 66, t), g = mix(36, 56, t), b = mix(61, 102, t);
            auto inRect = [&](const qs::Rect& q) { return q.contains((float)x, (float)y); };
            if (inRect(back)) { const bool bar = y < back.y + 34; r = g = b = bar ? 224 : 245; if (!bar && (y - (int)back.y - 70) % 28 < 9 && y > back.y + 70 && x > back.x + 28 && x < back.x + back.w * 0.72f) r = g = b = 205; }
            if (inRect(front)) { const bool bar = y < front.y + 34; r = bar ? 51 : 28; g = bar ? 51 : 31; b = bar ? 51 : 38; if (!bar && (y - (int)front.y - 70) % 28 < 9 && y > front.y + 70 && x > front.x + 28 && x < front.x + front.w * 0.72f) { r = 70; g = 72; b = 78; } }
            if (y >= (int)dockTop + 8 && y < (int)dockTop + 58 && x > 340 && x < 940) { r = mix(r, 255, 0.16f); g = mix(g, 255, 0.16f); b = mix(b, 255, 0.16f); }
            const uint32_t s = *(uint32_t*)((uint8_t*)mp.pData + (size_t)y * mp.RowPitch + (size_t)x * 4);
            const float sa = ((s >> 24) & 255) / 255.f;
            const float sr = ((s >> 16) & 255), sg = ((s >> 8) & 255), sb = (s & 255);
            const float R = sr + r * (1 - sa), G = sg + g * (1 - sa), B = sb + b * (1 - sa);
            out[(size_t)y * W + x] = (255u << 24) | ((uint32_t)std::min(255.f, R) << 16) | ((uint32_t)std::min(255.f, G) << 8) | (uint32_t)std::min(255.f, B);
        }
    }
    gpu.ctx->Unmap(stage, 0);

    // BMP wants bottom-up rows.
    std::vector<uint32_t> flipped((size_t)W * H);
    for (int y = 0; y < H; ++y) std::memcpy(&flipped[(size_t)y * W], &out[(size_t)(H - 1 - y) * W], (size_t)W * 4);
    BmpHeader hd;
    hd.w = W; hd.h = H; hd.imgSize = (uint32_t)(W * H * 4); hd.size = 54 + hd.imgSize;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return 1;
    std::fwrite(&hd, sizeof hd, 1, f);
    std::fwrite(flipped.data(), 4, flipped.size(), f);
    std::fclose(f);
    std::printf("snapshot: %s adapter, %d flakes, %d pile quads\n", gpu.warp ? "software (WARP)" : "hardware", fn, pn / 6);
    return 0;
}

// ---------------------------------------------------------------- entry

// A GUI program has no console of its own: borrow the parent's unless output is already redirected.
static void attachConsole() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) {
        AttachConsole(ATTACH_PARENT_PROCESS);
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR cmdLine, int) {
    const std::wstring cmd = cmdLine ? cmdLine : L"";
    if (cmd.find(L"--version") != std::wstring::npos) {
        attachConsole();
        std::fwprintf(stdout, L"Quiet Snow %ls\n", kVersion);
        return 0;
    }
    const size_t sp = cmd.find(L"--snapshot");
    if (sp != std::wstring::npos) {
        attachConsole();
        std::wstring path = cmd.substr(sp + 10);
        while (!path.empty() && path[0] == L' ') path.erase(0, 1);
        if (path.empty()) { std::fprintf(stderr, "usage: QuietSnow --snapshot out.bmp\n"); return 2; }
        return snapshot(path.c_str());
    }

    HANDLE once = CreateMutexW(nullptr, TRUE, L"Local\\QuietSnowSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW oc = {sizeof oc};
    oc.lpfnWndProc = overlayProc; oc.hInstance = hi; oc.lpszClassName = kOverlayClass;
    oc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&oc);
    WNDCLASSEXW hc = {sizeof hc};
    hc.lpfnWndProc = hostProc; hc.hInstance = hi; hc.lpszClassName = kHostClass;
    RegisterClassExW(&hc);

    if (!g_gpu.init(true)) {
        MessageBoxW(nullptr, L"Quiet Snow needs Direct3D 11 and Windows 10 or later.", L"Quiet Snow", MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    g_host = CreateWindowExW(0, kHostClass, L"Quiet Snow", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hi, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_icon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!g_icon) g_icon = makeIcon();

    const DWORD weather = getPref(L"weather", 1);
    g_sim.setWeather(weather == 0 ? qs::Weather::Light : weather == 2 ? qs::Weather::Flurry : qs::Weather::Steady);
    g_sim.wind = windValue();
    g_sim.melts = getPref(L"melt", 0) != 0;
    g_paused = getPref(L"paused", 0) != 0;

    if (!buildOverlays()) {
        MessageBoxW(nullptr, L"Quiet Snow could not create its overlay.", L"Quiet Snow", MB_OK | MB_ICONERROR);
        return 1;
    }
    pollSurfaces();
    addTray();
    WTSRegisterSessionNotification(g_host, NOTIFY_FOR_THIS_SESSION);
    g_powerNote = RegisterPowerSettingNotification(g_host, &kConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
    SetTimer(g_host, kSlowTimer, 2000, nullptr);
    slowChecks();
    g_lastStep = nowSeconds();

    // The loop. While running, frames are paced by the desktop compositor (DwmFlush waits for its next pass), and a frame
    // is skipped when the last one was under 13 ms ago, so a 144 Hz screen draws about 72 a second and a 60 Hz one draws 60.
    // While paused or hidden it blocks in GetMessage and nothing wakes up.
    MSG msg;
    bool quit = false;
    while (!quit) {
        if (!running()) {
            if (GetMessageW(&msg, nullptr, 0, 0) <= 0) break;
            TranslateMessage(&msg); DispatchMessageW(&msg);
            continue;
        }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; break; }
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (quit || !running()) continue;
        const double t0 = nowSeconds();
        if (t0 - g_lastFrame >= 0.013) { g_lastFrame = t0; frame(); }
        const HRESULT hr = DwmFlush();
        if (FAILED(hr) || nowSeconds() - t0 < 0.002) Sleep(8);   // no compositor to wait on: do not spin
    }

    removeTray();
    destroyOverlays();
    if (g_powerNote) UnregisterPowerSettingNotification(g_powerNote);
    WTSUnRegisterSessionNotification(g_host);
    if (once) CloseHandle(once);
    return 0;
}

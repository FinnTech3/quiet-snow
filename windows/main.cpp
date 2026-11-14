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
#include <map>
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
cbuffer U : register(b0) { float2 origin; float2 size; float t; float time; float2 pad; };

struct FlakeIn { float4 a : A; float4 b : B; uint vid : SV_VertexID; };
struct V { float4 pos : SV_Position; float2 uv : UV; float alpha : AL; float shade : SH; float kind : K; float ang : AN; };

// kind: 0 ball, 1 snowflake, 2 sleet, 3 sparkle, 4 hail (the SnowType values in sim.hpp).
V flakeV(FlakeIn i) {
    float2 corner = float2((i.vid & 1) ? 1.0 : -1.0, (i.vid & 2) ? 1.0 : -1.0);
    const float kind = i.b.z;
    const float d = i.a.z * (kind == 1.0 ? 2.4 : kind == 3.0 ? 2.0 : kind == 4.0 ? 0.8 : 1.0);
    float2 off = corner * 0.5 * d;
    if (kind == 2.0) {   // sleet is a streak along the way the flake is travelling
        float2 dir = normalize(i.b.xy + float2(0.0001, 0.0001));
        float2 perp = float2(-dir.y, dir.x);
        off = dir * corner.x * 0.5 * d * 3.2 + perp * corner.y * 0.5 * d * 0.4;
    }
    float2 p = i.a.xy + i.b.xy * t + off - origin;
    float2 q = p / size;
    V o;
    o.pos = float4(q.x * 2.0 - 1.0, 1.0 - q.y * 2.0, 0.0, 1.0);
    o.uv = corner;
    o.alpha = i.a.w;
    o.shade = 0.0;
    o.kind = kind;
    o.ang = i.b.w;
    return o;
}

float segDist(float2 p, float2 a, float2 b) {
    float2 pa = p - a, ba = b - a;
    return length(pa - ba * saturate(dot(pa, ba) / dot(ba, ba)));
}

float4 flakeP(V i) : SV_Target {
    const float2 u = i.uv;
    float m = 0.0, alpha = i.alpha;
    float3 tint = float3(0.94, 0.97, 1.0);
    if (i.kind < 0.5) {                      // ball: a soft disc
        m = smoothstep(1.0, 0.25, length(u));
    } else if (i.kind < 1.5) {               // snowflake: six arms, each with two pairs of branches
        const float c = cos(i.ang), sn = sin(i.ang);
        const float2 q = float2(u.x * c - u.y * sn, u.x * sn + u.y * c);
        const float sector = 3.14159265 / 3.0;
        const float a = abs(fmod(atan2(q.y, q.x) + 6.2831853 + sector * 0.5, sector) - sector * 0.5);
        const float2 p = float2(cos(a), sin(a)) * length(q);
        const float arm = p.x < 0.92 ? abs(p.y) : length(p - float2(0.92, 0.0));
        const float dd = min(arm, min(segDist(p, float2(0.40, 0.0), float2(0.53, 0.22)), segDist(p, float2(0.66, 0.0), float2(0.76, 0.17))));
        m = smoothstep(0.20, 0.07, dd) * step(length(q), 1.0);
    } else if (i.kind < 2.5) {               // sleet: a streak with a brighter head
        m = smoothstep(1.0, 0.2, length(float2(u.x * 0.85, u.y))) * lerp(0.45, 1.0, saturate(u.x * 0.5 + 0.5));
        alpha *= 0.85;
    } else if (i.kind < 3.5) {               // sparkle: a four-point star that twinkles
        const float2 q = abs(u);
        const float s = sqrt(q.x) + sqrt(q.y);
        m = max(smoothstep(1.0, 0.55, s), smoothstep(0.4, 0.0, length(u)));
        m *= 0.55 + 0.45 * sin(time * 2.6 + i.ang * 5.0);
        tint = float3(1.0, 0.99, 0.93);
    } else {                                 // hail: a small hard pellet with a highlight
        m = smoothstep(1.0, 0.78, length(u));
        tint = lerp(float3(0.78, 0.87, 0.97), float3(1.0, 1.0, 1.0), smoothstep(0.6, 0.0, length(u - float2(-0.35, -0.35))));
        alpha = min(1.0, alpha * 1.6);
    }
    const float a = m * alpha;
    return float4(tint * a, a);
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

struct Constants { float origin[2]; float size[2]; float t; float time; float pad[2]; };

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

    // ox, oy and the size are in points (pixels divided by `scale`); the viewport is in pixels.
    void draw(ID3D11RenderTargetView* rtv, UINT w, UINT h, float ox, float oy, float scale, float t, float time, int pileN, int flakeN) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            Constants c = {{ox, oy}, {(float)w / scale, (float)h / scale}, t, time, {0, 0}};
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
    bool shown = false;
    bool covered = false;   // desktop-only mode and an app is full screen on this display: nothing is drawn
    int clear = 0;          // polls in a row without a full screen app
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
static float g_scale = 1;           // display scaling: the snow is measured in points, so it looks the same on every screen
static DWORD g_scope = 0;           // 0 over all apps and screens, 1 desktop only (snow hides behind windows)
static bool g_lowPower = false;
static bool g_presenting = false;   // Windows is in presentation mode: stay out of the way
static bool g_hotkey = false;       // Ctrl+Alt+Shift+S pauses and resumes, when the user turns it on
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
    if (m == WM_DPICHANGED) {   // keep covering the whole display exactly, whatever scaling Windows suggests
        for (auto& o : g_overlays) if (o.hwnd == h) SetWindowPos(h, nullptr, o.mon.left, o.mon.top, (int)o.w, (int)o.h, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
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
    SetWindowPos(o.hwnd, HWND_TOPMOST, mi.mon.left, mi.mon.top, (int)o.w, (int)o.h, SWP_NOACTIVATE);   // exact, whatever the scaling

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
    o.shown = true;
    return true;
}

static bool buildOverlays() {
    destroyOverlays();
    g_scale = std::max(1.f, (float)GetDpiForSystem() / 96.f);
    std::vector<MonitorInfoLite> mons;
    EnumDisplayMonitors(nullptr, nullptr, enumMonitors, (LPARAM)&mons);
    std::vector<qs::Rect> world;
    for (const auto& m : mons) {
        Overlay o;
        if (!createOverlay(m, o)) { destroyOverlays(); return false; }
        g_overlays.push_back(o);
        world.push_back({m.mon.left / g_scale, m.mon.top / g_scale, (m.mon.right - m.mon.left) / g_scale, (m.mon.bottom - m.mon.top) / g_scale});
    }
    g_sim.setWorld(world);
    return !g_overlays.empty();
}

// ---------------------------------------------------------------- what is on screen

static void applyVisibility();

struct EnumCtx { std::vector<std::pair<int, qs::Rect>> rects; std::vector<qs::Rect> all; std::vector<char> full; DWORD pid; };

// Collects every ordinary window (`all`, for desktop-only mode) and, of those, the ones snow can rest on (`rects`).
// Everything is in points: pixels divided by the display scaling.
static BOOL CALLBACK enumWindows(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumCtx*>(lp);
    if (ctx->all.size() >= 80) return FALSE;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
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
    for (size_t k = 0; k < g_overlays.size(); ++k) {   // a window that covers a whole display is a full screen app
        const RECT& m = g_overlays[k].mon;
        if (r.left <= m.left + 2 && r.top <= m.top + 2 && r.right >= m.right - 2 && r.bottom >= m.bottom - 2) ctx->full[k] = 1;
    }
    const qs::Rect rect{r.left / g_scale, r.top / g_scale, (r.right - r.left) / g_scale, (r.bottom - r.top) / g_scale};
    if (rect.w < 100 / g_scale || rect.h < 50 / g_scale) return TRUE;
    ctx->all.push_back(rect);
    if (IsZoomed(hwnd)) return TRUE;
    // A window pinned to the top and as wide as its screen is maximised or full screen: let the snow pass over it.
    for (const auto& m : g_sim.world()) {
        if (rect.w >= m.w * 0.9f && rect.y < m.y + 40 / g_scale && rect.x < m.maxX() && rect.maxX() > m.x) return TRUE;
    }
    if (ctx->rects.size() < 40) ctx->rects.push_back({(int)(INT_PTR)hwnd, rect});
    return TRUE;
}

// Polling is slow while nothing moves and fast for a moment after any window moves, which is what a drag looks like.
static double g_fastUntil = 0;
static std::vector<qs::Rect> g_lastAll;

static bool sameRect(const qs::Rect& a, const qs::Rect& b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

static void pollSurfaces() {
    EnumCtx ctx;
    ctx.pid = GetCurrentProcessId();
    ctx.full.assign(g_overlays.size(), 0);
    EnumWindows(enumWindows, (LPARAM)&ctx);          // front to back
    for (size_t k = 0; k < g_overlays.size(); ++k) { // desktop-only mode shows nothing on a display with a full screen app
        Overlay& o = g_overlays[k];
        if (g_scope == 1 && ctx.full[k]) { o.covered = true; o.clear = 0; }
        else if (o.covered && ++o.clear >= 2) o.covered = false;   // two clear polls in a row, so it cannot flicker
    }
    applyVisibility();
    bool moved = ctx.all.size() != g_lastAll.size();
    for (size_t i = 0; !moved && i < ctx.all.size(); ++i) moved = !sameRect(ctx.all[i], g_lastAll[i]);
    if (moved) g_fastUntil = nowSeconds() + 0.6;
    g_lastAll = ctx.all;
    // A window that is being dragged or resized stirs the snow around it, in the direction it moved.
    static std::map<int, qs::Rect> prev;
    static double prevAt = 0;
    const double at = nowSeconds();
    const double elapsed = at - prevAt;
    if (elapsed > 0.02 && elapsed < 0.5) {
        for (const auto& r : ctx.rects) {
            auto it = prev.find(r.first);
            if (it == prev.end()) continue;
            const float dx = r.second.x - it->second.x, dy = r.second.y - it->second.y;
            if (std::abs(dx) + std::abs(dy) >= 2 && std::abs(r.second.w - it->second.w) < 1 && std::abs(r.second.h - it->second.h) < 1)
                g_sim.stir(r.second, dx / (float)elapsed, dy / (float)elapsed);
        }
    }
    prev.clear();
    for (const auto& r : ctx.rects) prev[r.first] = r.second;
    prevAt = at;
    std::vector<qs::SurfaceInput> inputs;
    for (size_t i = 0; i < ctx.rects.size(); ++i) {
        qs::SurfaceInput in;
        in.id = ctx.rects[i].first;
        in.rect = {ctx.rects[i].second.x, ctx.rects[i].second.y, ctx.rects[i].second.w, 0};
        for (size_t k = 0; k < i; ++k) in.occluders.push_back(ctx.rects[k].second);
        inputs.push_back(std::move(in));
    }
    // The floor of each display: the top of the taskbar when it sits at the bottom, else the screen edge. The work area
    // is read fresh each time, so moving or resizing the taskbar moves the floor with it.
    for (size_t i = 0; i < g_overlays.size(); ++i) {
        const RECT& m = g_overlays[i].mon;
        bool stacked = false;
        for (size_t k = 0; k < g_overlays.size(); ++k) {
            if (k == i) continue;
            const RECT& o = g_overlays[k].mon;
            if (std::abs(o.top - m.bottom) < 2 && o.left < m.right && o.right > m.left) stacked = true;
        }
        if (stacked) continue;
        MONITORINFO mi = {sizeof mi};
        const LONG bottom = GetMonitorInfoW(MonitorFromRect(&m, MONITOR_DEFAULTTONEAREST), &mi) ? mi.rcWork.bottom : g_overlays[i].work.bottom;
        qs::SurfaceInput f;
        f.id = -(int)(i + 1);
        f.rect = {m.left / g_scale, bottom / g_scale, (m.right - m.left) / g_scale, 0};
        f.isFloor = true;
        inputs.push_back(std::move(f));
    }
    g_sim.setSurfaces(inputs);
    g_sim.setOccluders(g_scope == 1 ? ctx.all : std::vector<qs::Rect>{});
}

// ---------------------------------------------------------------- frames

static bool anyShown() { for (const auto& o : g_overlays) if (o.shown) return true; return false; }
static bool running() { return !g_paused && !g_away && !g_presenting && anyShown(); }

// Show or hide each overlay: hidden while the screen is locked or off, and on a display with a full screen app in
// desktop-only mode. A window is only touched when its state really changes.
static void applyVisibility() {
    for (auto& o : g_overlays) {
        const bool want = !g_away && !g_presenting && !o.covered;
        if (o.shown == want) continue;
        ShowWindow(o.hwnd, want ? SW_SHOWNOACTIVATE : SW_HIDE);
        o.shown = want;
    }
}

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
        qs::Pointer ptr{p.x / g_scale, p.y / g_scale};
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
        if (!o.shown) continue;
        g_gpu.draw(o.rtv, o.w, o.h, o.mon.left / g_scale, o.mon.top / g_scale, g_scale, g_glide, (float)std::fmod(now, 1000.0), g_pileN, g_flakeN);
        o.sc->Present(0, 0);
    }
}

// Slow checks, every two seconds: battery saver, and keeping the overlay above whatever has taken the top
// (the taskbar and the Start menu do) so the snow reaches the very edge of the screen.
static void slowChecks() {
    SYSTEM_POWER_STATUS sps = {};
    const bool low = GetSystemPowerStatus(&sps) && sps.SystemStatusFlag == 1;
    if (low != g_lowPower) { g_lowPower = low; g_sim.setDensityScale(low ? 0.6f : 1.f); }
    QUERY_USER_NOTIFICATION_STATE qn;
    const bool presenting = SUCCEEDED(SHQueryUserNotificationState(&qn)) && qn == QUNS_PRESENTATION_MODE;
    if (presenting != g_presenting) { g_presenting = presenting; applyVisibility(); if (running()) g_lastStep = nowSeconds(); }
    for (auto& o : g_overlays) {
        if (o.shown && GetWindow(o.hwnd, GW_HWNDPREV)) SetWindowPos(o.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

// ---------------------------------------------------------------- tray

enum Id {
    ID_LIGHT = 1, ID_STEADY, ID_FLURRY,
    ID_WIND0, ID_WIND_R1, ID_WIND_L1, ID_WIND_R2, ID_WIND_L2,
    ID_MELT, ID_CLEAR, ID_PAUSE, ID_LOGIN, ID_QUIT, ID_SCOPE_ALL, ID_SCOPE_DESKTOP, ID_TYPE0,
    ID_HOTKEY = ID_TYPE0 + 6, ID_DIAG, ID_UPDATE, ID_RESET,
};

static const int kWindValues[5] = {0, 60, -60, 220, -220};

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


static const int kHotkeyId = 1;
static void applyHotkey() {
    UnregisterHotKey(g_host, kHotkeyId);
    if (g_hotkey) g_hotkey = RegisterHotKey(g_host, kHotkeyId, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'S') != 0;
}

static std::wstring diagnostics() {
    std::wstring t = L"Quiet Snow ";
    t += kVersion;
    t += L"\r\n";
    typedef LONG (WINAPI* RtlGetVersionFn)(OSVERSIONINFOW*);
    OSVERSIONINFOW vi = {sizeof vi};
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
        if (auto fn = (RtlGetVersionFn)GetProcAddress(nt, "RtlGetVersion")) fn(&vi);
    wchar_t b[256];
    swprintf_s(b, L"Windows %lu.%lu build %lu\r\n", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
    t += b;
    swprintf_s(b, L"Adapter: %ls\r\n", g_gpu.warp ? L"software (WARP)" : L"hardware");
    t += b;
    swprintf_s(b, L"Scaling: %.2f\r\n", (double)g_scale);
    t += b;
    for (size_t i = 0; i < g_overlays.size(); ++i) {
        const RECT& m = g_overlays[i].mon;
        swprintf_s(b, L"Display %zu: %ld x %ld at %ld,%ld, %ls%ls\r\n", i + 1, m.right - m.left, m.bottom - m.top, m.left, m.top,
                   g_overlays[i].shown ? L"shown" : L"hidden", g_overlays[i].covered ? L" (full-screen app)" : L"");
        t += b;
    }
    static const wchar_t* weatherNames[3] = {L"Light", L"Steady", L"Flurry"};
    static const wchar_t* typeNames[6] = {L"Balls", L"Snowflakes", L"Sleet", L"Sparkles", L"Hail", L"Mix of all"};
    swprintf_s(b, L"Weather %ls, wind %d, snow type %ls, %ls\r\n", weatherNames[(int)g_sim.weather()], (int)g_sim.wind,
               typeNames[(int)g_sim.snowType()], g_scope == 1 ? L"desktop only" : L"over all apps");
    t += b;
    swprintf_s(b, L"Flakes %d, pile quads %d, paused %ls, presentation mode %ls, low power %ls\r\n", g_flakeN, g_pileN / 12,
               g_paused ? L"yes" : L"no", g_presenting ? L"yes" : L"no", g_lowPower ? L"yes" : L"no");
    t += b;
    return t;
}

static void copyText(const std::wstring& t) {
    if (!OpenClipboard(g_host)) return;
    EmptyClipboard();
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t))) {
        if (void* p = GlobalLock(h)) {
            std::memcpy(p, t.c_str(), (t.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(h);
            if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
        } else GlobalFree(h);
    }
    CloseClipboard();
}

// Back to the defaults a fresh install starts with.
static void resetSettings() {
    RegDeleteKeyW(HKEY_CURRENT_USER, kKey);
    g_sim.setWeather(qs::Weather::Steady);
    g_sim.wind = 0;
    g_sim.melts = false;
    g_sim.setSnowType(qs::SnowType::Balls);
    g_paused = false;
    g_scope = 0;
    g_hotkey = false;
    applyHotkey();
    for (auto& o : g_overlays) o.covered = false;
    g_lastAll.clear();
    pollSurfaces();
    timerSync();
}

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
    HMENU types = CreatePopupMenu();
    static const wchar_t* typeNames[6] = {L"Balls", L"Snowflakes", L"Sleet", L"Sparkles", L"Hail", L"Mix of all"};
    for (int i = 0; i < 6; ++i) AppendMenuW(types, MF_STRING | ((int)g_sim.snowType() == i ? MF_CHECKED : 0), ID_TYPE0 + i, typeNames[i]);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)types, L"Snow type");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_scope == 0 ? MF_CHECKED : 0), ID_SCOPE_ALL, L"Over all apps and screens");
    AppendMenuW(m, MF_STRING | (g_scope == 1 ? MF_CHECKED : 0), ID_SCOPE_DESKTOP, L"Desktop only");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_sim.melts ? MF_CHECKED : 0), ID_MELT, L"Melt slowly");
    AppendMenuW(m, MF_STRING, ID_CLEAR, L"Clear piles");
    AppendMenuW(m, MF_STRING, ID_PAUSE, g_paused ? L"Resume" : L"Pause");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (loginEnabled() ? MF_CHECKED : 0), ID_LOGIN, L"Open at login");
    AppendMenuW(m, MF_STRING | (g_hotkey ? MF_CHECKED : 0), ID_HOTKEY, L"Pause with Ctrl+Alt+Shift+S");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_DIAG, L"Copy diagnostics");
    AppendMenuW(m, MF_STRING, ID_UPDATE, L"Check for updates");
    AppendMenuW(m, MF_STRING, ID_RESET, L"Reset settings");
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
        case ID_SCOPE_ALL: case ID_SCOPE_DESKTOP:
            g_scope = cmd == ID_SCOPE_DESKTOP ? 1 : 0; setPref(L"scope", g_scope);
            for (auto& o : g_overlays) o.covered = false;
            g_lastAll.clear(); pollSurfaces();
            break;
        case ID_TYPE0: case ID_TYPE0 + 1: case ID_TYPE0 + 2: case ID_TYPE0 + 3: case ID_TYPE0 + 4: case ID_TYPE0 + 5:
            g_sim.setSnowType((qs::SnowType)(cmd - ID_TYPE0)); setPref(L"snowType", (DWORD)(cmd - ID_TYPE0));
            break;
        case ID_MELT: g_sim.melts = !g_sim.melts; setPref(L"melt", g_sim.melts ? 1 : 0); break;
        case ID_CLEAR: g_sim.clearPiles(); break;
        case ID_PAUSE: g_paused = !g_paused; setPref(L"paused", g_paused ? 1 : 0); timerSync(); break;
        case ID_LOGIN: setLogin(!loginEnabled()); break;
        case ID_HOTKEY: g_hotkey = !g_hotkey; applyHotkey(); setPref(L"hotkey", g_hotkey ? 1 : 0); break;
        case ID_DIAG: copyText(diagnostics()); break;
        case ID_UPDATE: ShellExecuteW(nullptr, L"open", L"https://github.com/FinnTech3/quiet-snow/releases/latest", nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_RESET: resetSettings(); break;
        case ID_QUIT: PostQuitMessage(0); break;
    }
}

// ---------------------------------------------------------------- host window and pacing

static UINT_PTR kSlowTimer = 1;
static void timerSync() {
    if (running()) { g_lastStep = nowSeconds(); }
    applyVisibility();
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
            if (w == kSlowTimer) { slowChecks(); if (!running()) pollSurfaces(); timerSync(); }
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
        case WM_HOTKEY:
            if (w == kHotkeyId) { g_paused = !g_paused; setPref(L"paused", g_paused ? 1 : 0); timerSync(); }
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

static int snapshot(const wchar_t* path, bool mix) {
    const int W = 1280, H = 800;
    Gpu gpu;
    if (!gpu.init(false)) { std::fprintf(stderr, "no Direct3D 11 device\n"); return 1; }
    qs::Sim sim(42);
    sim.setWeather(qs::Weather::Steady);
    if (mix) sim.setSnowType(qs::SnowType::Mixed);
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
    gpu.draw(rtv, W, H, 0, 0, 1.f, 0, 0.8f, pn, fn);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(gpu.dev->CreateTexture2D(&td, nullptr, &stage))) return 1;
    gpu.ctx->CopyResource(stage, tex);
    D3D11_MAPPED_SUBRESOURCE mp;
    if (FAILED(gpu.ctx->Map(stage, 0, D3D11_MAP_READ, 0, &mp))) return 1;

    // Composite the premultiplied snow over a drawn desktop, top-down BGRA.
    std::vector<uint32_t> out((size_t)W * H);
    auto lerpf = [](float a, float b, float t) { return a + (b - a) * t; };
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const float t = (x / (float)W + y / (float)H) * 0.5f;
            float r = lerpf(26, 66, t), g = lerpf(36, 56, t), b = lerpf(61, 102, t);
            auto inRect = [&](const qs::Rect& q) { return q.contains((float)x, (float)y); };
            if (inRect(back)) { const bool bar = y < back.y + 34; r = g = b = bar ? 224 : 245; if (!bar && (y - (int)back.y - 70) % 28 < 9 && y > back.y + 70 && x > back.x + 28 && x < back.x + back.w * 0.72f) r = g = b = 205; }
            if (inRect(front)) { const bool bar = y < front.y + 34; r = bar ? 51 : 28; g = bar ? 51 : 31; b = bar ? 51 : 38; if (!bar && (y - (int)front.y - 70) % 28 < 9 && y > front.y + 70 && x > front.x + 28 && x < front.x + front.w * 0.72f) { r = 70; g = 72; b = 78; } }
            if (y >= (int)dockTop + 8 && y < (int)dockTop + 58 && x > 340 && x < 940) { r = lerpf(r, 255, 0.16f); g = lerpf(g, 255, 0.16f); b = lerpf(b, 255, 0.16f); }
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
    std::printf("snapshot: %s adapter, %d flakes, %d pile quads\n", gpu.warp ? "software (WARP)" : "hardware", fn, pn / 12);
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
        const size_t mp = path.find(L"--mix");
        const bool mix = mp != std::wstring::npos;
        if (mix) path = path.substr(0, mp);
        while (!path.empty() && path.back() == L' ') path.pop_back();
        if (path.empty()) { std::fprintf(stderr, "usage: QuietSnow --snapshot out.bmp [--mix]\n"); return 2; }
        return snapshot(path.c_str(), mix);
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

    // First run on a machine with animations switched off (Settings, Accessibility): start on the lightest weather.
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    const DWORD weather = getPref(L"weather", animations ? 1 : 0);
    g_sim.setWeather(weather == 0 ? qs::Weather::Light : weather == 2 ? qs::Weather::Flurry : qs::Weather::Steady);
    g_sim.wind = windValue();
    g_sim.melts = getPref(L"melt", 0) != 0;
    g_paused = getPref(L"paused", 0) != 0;
    g_scope = getPref(L"scope", 0) == 1 ? 1 : 0;
    g_sim.setSnowType((qs::SnowType)std::min<DWORD>(getPref(L"snowType", 0), 5));

    g_hotkey = getPref(L"hotkey", 0) != 0;
    applyHotkey();
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

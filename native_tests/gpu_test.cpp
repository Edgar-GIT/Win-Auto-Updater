#include "gpu_test.hpp"

#include "gpu_shaders.hpp"

#include "com_ptr.hpp"
#include "display.hpp"  // highest display resolution (shared with the macro workflow)
#include "ui.hpp"       // GDI+ (PNG snapshots)

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#if __has_include("gpu_shader_bytecode.inc")
#include "gpu_shader_bytecode.inc"
#define NT_HAVE_SHADER_BYTECODE 1
#endif

namespace nt {

namespace {

constexpr float kSceneQuality = 0.75f;
// The scene pass is drawn in horizontal bands: no single draw call can keep a
// weak GPU busy long enough to trip Windows' 2-second driver watchdog (TDR).
// The number of bands grows with the frame time (see stripsFor).
constexpr int kMinStrips = 6;
constexpr int kMaxStrips = 160;
constexpr const wchar_t* kWindowClass = L"WinAutoUpdaterVoidBurner";

struct FrameConstants {
    float res[4];
    float params[4];
    float blit[4];
};

template <typename T>
void** asVoid(ComPtr<T>& ptr) {
    return reinterpret_cast<void**>(ptr.put());
}

// ---------------------------------------------------------------------------
// Shader compilation (run-time fallback — normally the embedded bytecode is used)
// ---------------------------------------------------------------------------

using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                   LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

CompileFn loadCompiler() {
    static CompileFn cached = nullptr;
    static bool tried = false;
    if (tried) {
        return cached;
    }
    tried = true;
    for (const wchar_t* dll : {L"d3dcompiler_47.dll", L"d3dcompiler_46.dll", L"d3dcompiler_43.dll"}) {
        HMODULE module = LoadLibraryW(dll);
        if (module == nullptr) {
            continue;
        }
        cached = reinterpret_cast<CompileFn>(
            reinterpret_cast<void (*)()>(GetProcAddress(module, "D3DCompile")));
        if (cached != nullptr) {
            break;
        }
    }
    return cached;
}

// Compiled blobs are cached in %TEMP%\WinAutoUpdater keyed by the shader source,
// so only the first GPU test on a machine waits for the HLSL compiler.
std::filesystem::path shaderCachePath(const char* source, const char* entry, const char* target) {
    uint64_t hash = 0xCBF29CE484222325ULL;
    const auto mix = [&hash](const char* text) {
        for (const char* c = text; *c != '\0'; ++c) {
            hash = (hash ^ static_cast<unsigned char>(*c)) * 0x100000001B3ULL;
        }
    };
    mix(source);
    mix(entry);
    mix(target);
    wchar_t temp[MAX_PATH]{};
    const DWORD len = GetTempPathW(MAX_PATH, temp);
    if (len == 0 || len > MAX_PATH) {
        return {};
    }
    wchar_t name[64]{};
    swprintf(name, 64, L"shader_%016llx.bin", static_cast<unsigned long long>(hash));
    return std::filesystem::path(temp) / L"WinAutoUpdater" / name;
}

bool readShaderCache(const std::filesystem::path& path, std::vector<uint8_t>* out) {
    if (path.empty()) {
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    out->assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    // Every Direct3D shader container starts with "DXBC".
    return out->size() > 32 && std::memcmp(out->data(), "DXBC", 4) == 0;
}

void writeShaderCache(const std::filesystem::path& path, const std::vector<uint8_t>& blob) {
    if (path.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (file.is_open()) {
        file.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
    }
}

bool compileShader(const char* source, const char* entry, const char* target, std::vector<uint8_t>* out,
                   std::wstring* error, bool useCache = true) {
    const std::filesystem::path cachePath = shaderCachePath(source, entry, target);
    if (useCache && readShaderCache(cachePath, out)) {
        return true;
    }
    const CompileFn compile = loadCompiler();
    if (compile == nullptr) {
        if (error) {
            *error = L"d3dcompiler_47.dll não está disponível.";
        }
        return false;
    }
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> messages;
    const HRESULT hr = compile(source, std::strlen(source), "void_burner.hlsl", nullptr, nullptr, entry,
                               target, 0, 0, blob.put(), messages.put());
    if (FAILED(hr) || !blob) {
        std::string text;
        if (messages) {
            text.assign(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize());
        }
        const std::wstring detail = L"Compilação de " + std::wstring(entry, entry + std::strlen(entry)) +
                                    L" falhou (" + hresultText(hr) + L"): " +
                                    std::wstring(text.begin(), text.end());
        logLine(L"[GPU] " + detail);
        if (error) {
            *error = detail;
        }
        return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(blob->GetBufferPointer());
    out->assign(bytes, bytes + blob->GetBufferSize());
    writeShaderCache(cachePath, *out);
    return true;
}

// ---------------------------------------------------------------------------
// Adapter choice
// ---------------------------------------------------------------------------

bool isSoftwareAdapter(const DXGI_ADAPTER_DESC1& desc) {
    return (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || (desc.VendorId == 0x1414 && desc.DeviceId == 0x8c);
}

// The graphics card to test: on hybrid laptops the dedicated one (Windows 10
// 1803+ can tell which that is), otherwise the primary adapter.
ComPtr<IDXGIAdapter1> chooseAdapter(DXGI_ADAPTER_DESC1* desc) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), asVoid(factory))) || !factory) {
        return {};
    }
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIFactory6> factory6;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory6), asVoid(factory6))) && factory6) {
        if (SUCCEEDED(factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                           __uuidof(IDXGIAdapter1), asVoid(adapter))) &&
            adapter && SUCCEEDED(adapter->GetDesc1(desc)) && !isSoftwareAdapter(*desc)) {
            return adapter;
        }
        adapter.reset();
    }
    if (SUCCEEDED(factory->EnumAdapters1(0, adapter.put())) && adapter && SUCCEEDED(adapter->GetDesc1(desc))) {
        return adapter;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------

struct Renderer {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11RenderTargetView> backRtv;
    ComPtr<ID3D11Texture2D> sceneTex;
    ComPtr<ID3D11RenderTargetView> sceneRtv;
    ComPtr<ID3D11ShaderResourceView> sceneSrv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> psScene;
    ComPtr<ID3D11PixelShader> psBlit;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> raster;
    int width = 0;  // back buffer = scene texture
    int height = 0;
    bool fallbackShader = false;
    std::wstring adapterName;
    uint64_t vramBytes = 0;
    bool software = false;

    void release() {
        if (ctx) {
            ctx->ClearState();
        }
        raster.reset();
        sampler.reset();
        constants.reset();
        psBlit.reset();
        psScene.reset();
        vs.reset();
        sceneSrv.reset();
        sceneRtv.reset();
        sceneTex.reset();
        backRtv.reset();
        swap.reset();
        ctx.reset();
        device.reset();
    }

    bool fail(std::wstring* error, const wchar_t* what, HRESULT hr) {
        const std::wstring text = std::wstring(what) + L" (" + hresultText(hr) + L")";
        logLine(L"[GPU] " + text);
        if (error) {
            *error = text;
        }
        return false;
    }

    HRESULT createDevice(IDXGIAdapter* adapter, HWND hwnd, int w, int h) {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                            D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_10_0;
        // An explicit adapter must be paired with the "unknown" driver type.
        const D3D_DRIVER_TYPE type = adapter != nullptr ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
        swap.reset();
        device.reset();
        ctx.reset();
        if (hwnd == nullptr) {
            return D3D11CreateDevice(adapter, type, nullptr, 0, levels, 3, D3D11_SDK_VERSION, device.put(),
                                     &level, ctx.put());
        }
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferCount = 1;
        desc.BufferDesc.Width = static_cast<UINT>(w);
        desc.BufferDesc.Height = static_cast<UINT>(h);
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = hwnd;
        desc.SampleDesc.Count = 1;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        return D3D11CreateDeviceAndSwapChain(adapter, type, nullptr, 0, levels, 3, D3D11_SDK_VERSION, &desc,
                                             swap.put(), device.put(), &level, ctx.put());
    }

    // hwnd == nullptr → off-screen device (snapshots).
    bool init(HWND hwnd, int w, int h, std::wstring* error) {
        release();
        DXGI_ADAPTER_DESC1 chosenDesc{};
        ComPtr<IDXGIAdapter1> chosen = chooseAdapter(&chosenDesc);
        HRESULT hr = createDevice(chosen.get(), hwnd, w, h);
        if ((FAILED(hr) || !device || !ctx) && chosen) {
            logLine(L"[GPU] Placa preferida recusada (" + hresultText(hr) + L") — a usar a principal.");
            hr = createDevice(nullptr, hwnd, w, h);
        }
        if (FAILED(hr) || !device || !ctx) {
            return fail(error, L"Esta placa gráfica não suporta Direct3D 10/11", hr);
        }

        // Adapter identity.
        {
            ComPtr<IDXGIDevice> dxgiDevice;
            ComPtr<IDXGIAdapter> adapter;
            if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), asVoid(dxgiDevice))) && dxgiDevice &&
                SUCCEEDED(dxgiDevice->GetAdapter(adapter.put())) && adapter) {
                DXGI_ADAPTER_DESC desc{};
                if (SUCCEEDED(adapter->GetDesc(&desc))) {
                    adapterName = desc.Description;
                    vramBytes = desc.DedicatedVideoMemory;
                    software = desc.VendorId == 0x1414 && desc.DeviceId == 0x8c;
                }
                if (hwnd != nullptr) {
                    // Alt+Enter must not switch to exclusive full screen behind our back.
                    ComPtr<IDXGIFactory> factory;
                    if (SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory), asVoid(factory))) && factory) {
                        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
                    }
                }
            }
        }

        if (!createShaders(error) || !createStates(error)) {
            return false;
        }
        return createTargets(w, h, error);
    }

    bool createPixelShader(const void* data, size_t size, ComPtr<ID3D11PixelShader>& out) {
        out.reset();
        return data != nullptr && size > 0 &&
               SUCCEEDED(device->CreatePixelShader(data, size, nullptr, out.put())) && out;
    }

    bool createShaders(std::wstring* error) {
        fallbackShader = false;
#ifdef NT_HAVE_SHADER_BYTECODE
        // Pre-compiled with Microsoft's compiler at build time: nothing to
        // compile (or to go wrong) on the machine under test.
        device->CreateVertexShader(bytecode::kVertex, sizeof(bytecode::kVertex), nullptr, vs.put());
        createPixelShader(bytecode::kScene, sizeof(bytecode::kScene), psScene);
        createPixelShader(bytecode::kBlit, sizeof(bytecode::kBlit), psBlit);
        if (vs && psScene && psBlit) {
            return true;
        }
        logLine(L"[GPU] Bytecode embutido rejeitado — a compilar os shaders nesta máquina.");
#endif
        std::vector<uint8_t> blob;
        if (!vs) {
            if (!compileShader(kGpuShaderSource, "VSMain", "vs_4_0", &blob, error) ||
                FAILED(device->CreateVertexShader(blob.data(), blob.size(), nullptr, vs.put())) || !vs) {
                return fail(error, L"Não foi possível criar o vertex shader", E_FAIL);
            }
        }
        if (!psBlit) {
            if (!compileShader(kGpuShaderSource, "PSBlit", "ps_4_0", &blob, error) ||
                !createPixelShader(blob.data(), blob.size(), psBlit)) {
                return fail(error, L"Não foi possível criar o shader de apresentação", E_FAIL);
            }
        }
        if (!psScene) {
            bool created = compileShader(kGpuShaderSource, "PSScene", "ps_4_0", &blob, error) &&
                           createPixelShader(blob.data(), blob.size(), psScene);
            if (!created) {
                // A stale or damaged cache entry must not cost us the real scene.
                created = compileShader(kGpuShaderSource, "PSScene", "ps_4_0", &blob, error, false) &&
                          createPixelShader(blob.data(), blob.size(), psScene);
            }
            if (!created) {
                // Last resort: a much simpler procedural load.
                fallbackShader = true;
                if (!compileShader(kGpuFallbackSource, "PSScene", "ps_4_0", &blob, error) ||
                    !createPixelShader(blob.data(), blob.size(), psScene)) {
                    return fail(error, L"Não foi possível criar o shader da cena", E_FAIL);
                }
            }
        }
        return true;
    }

    bool createStates(std::wstring* error) {
        D3D11_BUFFER_DESC bufferDesc{};
        bufferDesc.ByteWidth = sizeof(FrameConstants);
        bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
        bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr = device->CreateBuffer(&bufferDesc, nullptr, constants.put());
        if (FAILED(hr)) {
            return fail(error, L"CreateBuffer falhou", hr);
        }

        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        hr = device->CreateSamplerState(&samplerDesc, sampler.put());
        if (FAILED(hr)) {
            return fail(error, L"CreateSamplerState falhou", hr);
        }

        D3D11_RASTERIZER_DESC rasterDesc{};
        rasterDesc.FillMode = D3D11_FILL_SOLID;
        rasterDesc.CullMode = D3D11_CULL_NONE;
        rasterDesc.DepthClipEnable = TRUE;
        rasterDesc.ScissorEnable = TRUE;
        hr = device->CreateRasterizerState(&rasterDesc, raster.put());
        if (FAILED(hr)) {
            return fail(error, L"CreateRasterizerState falhou", hr);
        }
        return true;
    }

    bool createTargets(int w, int h, std::wstring* error) {
        width = std::max(1, w);
        height = std::max(1, h);
        backRtv.reset();
        sceneSrv.reset();
        sceneRtv.reset();
        sceneTex.reset();

        if (swap) {
            ComPtr<ID3D11Texture2D> backBuffer;
            HRESULT hr = swap->GetBuffer(0, __uuidof(ID3D11Texture2D), asVoid(backBuffer));
            if (FAILED(hr) || !backBuffer) {
                return fail(error, L"GetBuffer falhou", hr);
            }
            hr = device->CreateRenderTargetView(backBuffer.get(), nullptr, backRtv.put());
            if (FAILED(hr)) {
                return fail(error, L"CreateRenderTargetView falhou", hr);
            }
        }

        // The scene is rendered into its own texture at the full output
        // resolution and copied to the back buffer.
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, sceneTex.put());
        if (FAILED(hr) || !sceneTex) {
            return fail(error, L"Sem memória de vídeo para o alvo de renderização", hr);
        }
        hr = device->CreateRenderTargetView(sceneTex.get(), nullptr, sceneRtv.put());
        if (FAILED(hr)) {
            return fail(error, L"CreateRenderTargetView (cena) falhou", hr);
        }
        hr = device->CreateShaderResourceView(sceneTex.get(), nullptr, sceneSrv.put());
        if (FAILED(hr)) {
            return fail(error, L"CreateShaderResourceView (cena) falhou", hr);
        }
        return true;
    }

    bool resize(int w, int h, std::wstring* error) {
        if (!swap || !ctx) {
            return false;
        }
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        backRtv.reset();
        const HRESULT hr = swap->ResizeBuffers(0, static_cast<UINT>(std::max(1, w)),
                                               static_cast<UINT>(std::max(1, h)), DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            return fail(error, L"ResizeBuffers falhou", hr);
        }
        return createTargets(w, h, error);
    }

    void bindCommon(const FrameConstants& frame) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, &frame, sizeof(frame));
            ctx->Unmap(constants.get(), 0);
        }
        ID3D11Buffer* buffer = constants.get();
        ID3D11SamplerState* samplerState = sampler.get();
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.get(), nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &buffer);
        ctx->PSSetConstantBuffers(0, 1, &buffer);
        ctx->PSSetSamplers(0, 1, &samplerState);
        ctx->RSSetState(raster.get());
    }

    void setViewport(int w, int h) {
        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(w);
        viewport.Height = static_cast<float>(h);
        viewport.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &viewport);
    }

    // Ray-marches the scene at the full resolution, in `strips` horizontal
    // bands. flushEachStrip submits every band to the GPU on its own, so a slow
    // card always has a short piece of work in front of it.
    void renderScene(float time, float quality, int strips, bool flushEachStrip) {
        FrameConstants frame{};
        frame.res[0] = static_cast<float>(width);
        frame.res[1] = static_cast<float>(height);
        frame.res[2] = 1.0f / static_cast<float>(width);
        frame.res[3] = 1.0f / static_cast<float>(height);
        frame.params[0] = time;
        frame.params[1] = quality;
        frame.params[2] = static_cast<float>(width) / static_cast<float>(height);
        frame.blit[0] = 1.0f;
        frame.blit[1] = 1.0f;
        frame.blit[2] = 1.0f;
        frame.blit[3] = 1.0f;

        ID3D11ShaderResourceView* noTexture = nullptr;
        ctx->PSSetShaderResources(0, 1, &noTexture);
        ID3D11RenderTargetView* target = sceneRtv.get();
        ctx->OMSetRenderTargets(1, &target, nullptr);
        bindCommon(frame);
        ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        ctx->PSSetShader(psScene.get(), nullptr, 0);
        setViewport(width, height);
        strips = std::clamp(strips, 1, std::max(1, height / 4));
        for (int strip = 0; strip < strips; ++strip) {
            D3D11_RECT rect{0, height * strip / strips, width, height * (strip + 1) / strips};
            ctx->RSSetScissorRects(1, &rect);
            ctx->Draw(3, 0);
            if (flushEachStrip) {
                ctx->Flush();
            }
        }
    }

    // Copies the scene to the back buffer.
    void composite() {
        ID3D11RenderTargetView* target = backRtv.get();
        ctx->OMSetRenderTargets(1, &target, nullptr);
        setViewport(width, height);
        D3D11_RECT full{0, 0, width, height};
        ctx->RSSetScissorRects(1, &full);
        ctx->PSSetShader(psBlit.get(), nullptr, 0);
        ID3D11ShaderResourceView* scene = sceneSrv.get();
        ctx->PSSetShaderResources(0, 1, &scene);
        ctx->Draw(3, 0);
    }
};

bool isDeviceLost(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
           hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

RECT monitorRectFor(HWND anchor) {
    HMONITOR monitor = MonitorFromWindow(anchor ? anchor : GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &info)) {
        return info.rcMonitor;
    }
    return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

// More, thinner bands the slower the card is: about 25 ms of work per band.
int stripsFor(double frameMs) {
    return std::clamp(static_cast<int>(frameMs / 25.0) + 1, kMinStrips, kMaxStrips);
}

}  // namespace

// ---------------------------------------------------------------------------
// GpuTest
// ---------------------------------------------------------------------------

GpuTest::~GpuTest() {
    stop();
}

bool GpuTest::active() const {
    return active_.load();
}

GpuStats GpuTest::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

bool GpuTest::queryAdapter(GpuAdapter* out) {
    DXGI_ADAPTER_DESC1 desc{};
    const ComPtr<IDXGIAdapter1> adapter = chooseAdapter(&desc);
    if (!adapter || out == nullptr) {
        return false;
    }
    out->name = desc.Description;
    out->vramBytes = desc.DedicatedVideoMemory;
    out->sharedBytes = desc.SharedSystemMemory;
    out->software = isSoftwareAdapter(desc);
    out->luid = desc.AdapterLuid;
    out->vendorId = desc.VendorId;
    return true;
}

bool GpuTest::start(const Options& options) {
    if (active_.exchange(true)) {
        return false;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    stopRequested_ = false;
    resizePending_ = false;
    closedByUser_ = false;
    history_.clear();
    {
        std::lock_guard lock(mutex_);
        stats_ = GpuStats{};
        stats_.state = State::Starting;
        stats_.message = L"A preparar a cena (na primeira vez compila os shaders, pode demorar)...";
    }
    thread_ = std::thread([this, options] { run(options); });
    return true;
}

void GpuTest::requestStop() {
    stopRequested_ = true;
}

void GpuTest::stop() {
    stopRequested_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
}

LRESULT CALLBACK GpuTest::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    auto* self = reinterpret_cast<GpuTest*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    switch (msg) {
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                self->closedByUser_ = true;
                self->stopRequested_ = true;
            }
            return 0;
        case WM_SIZE:
        case WM_DISPLAYCHANGE:
            self->resizePending_ = true;
            return 0;
        case WM_CLOSE:
            self->closedByUser_ = true;
            self->stopRequested_ = true;
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(nullptr);
                return TRUE;
            }
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void GpuTest::run(Options options) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND hwnd = nullptr;
    Renderer renderer;
    bool pacing = false;

    const auto finish = [&](State state, const std::wstring& message) {
        if (pacing) {
            TestClock::instance().setGpuPaced(false);
            pacing = false;
        }
        renderer.release();
        if (hwnd != nullptr) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
        {
            std::lock_guard lock(mutex_);
            stats_.state = state;
            stats_.message = message;
            stats_.fps = 0.0;
            stats_.closedByUser = closedByUser_.load();
        }
        logLine(L"[GPU] " + message);
        active_ = false;
    };

    // Always at the highest resolution the screen supports (the same call the
    // macro workflow makes before it opens FurMark).
    if (!options.keepDisplayMode) {
        const RECT before = monitorRectFor(options.placeNear);
        std::wstring detail;
        Display::setMaxResolution(&detail);
        logLine(L"[GPU] Ecrã: " + detail);
        const RECT after = monitorRectFor(options.placeNear);
        if (!EqualRect(&before, &after)) {
            Sleep(800);  // let the desktop settle on the new mode
        }
    }

    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = GpuTest::wndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = kWindowClass;
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
        wc.hIconSm = wc.hIcon;
        RegisterClassExW(&wc);  // fails harmlessly when already registered
    }

    // Borderless window covering the whole monitor: full screen without taking
    // the display away from the monitor bar, which stays on top of it.
    {
        const RECT rect = monitorRectFor(options.placeNear);
        hwnd = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, L"Void Burner — teste gráfico", WS_POPUP,
                               rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, nullptr,
                               nullptr, instance, this);
    }
    if (hwnd == nullptr) {
        finish(State::Unavailable, L"Não foi possível criar a janela do teste gráfico.");
        return;
    }
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);

    RECT client{};
    GetClientRect(hwnd, &client);
    std::wstring error;
    if (!renderer.init(hwnd, std::max<int>(1, client.right), std::max<int>(1, client.bottom), &error)) {
        finish(State::Unavailable, error.empty() ? L"Direct3D indisponível." : error);
        return;
    }
    if (renderer.software) {
        finish(State::Unavailable,
               L"Sem controlador gráfico (o Windows está a usar renderização por software). "
               L"Instala o driver da placa gráfica e repete o teste.");
        return;
    }

    {
        std::lock_guard lock(mutex_);
        stats_.state = State::Running;
        stats_.message = renderer.fallbackShader ? L"Cena simplificada (shader principal indisponível)."
                                                 : L"";
        stats_.adapter = renderer.adapterName;
        stats_.vramBytes = renderer.vramBytes;
        stats_.fallbackShader = renderer.fallbackShader;
        stats_.width = renderer.width;
        stats_.height = renderer.height;
    }
    logLine(L"[GPU] " + renderer.adapterName + L" — " + std::to_wstring(renderer.width) + L"×" +
            std::to_wstring(renderer.height) + (renderer.fallbackShader ? L" (shader simplificado)." : L"."));

    TestClock::instance().setGpuPaced(true);
    pacing = true;

    const double started = nowSeconds();
    double windowStart = started;
    double lastFrame = started;
    double frameMs = 20.0;  // smoothed, drives the number of bands
    int windowFrames = 0;
    uint64_t frames = 0;
    double minFps = 0.0;
    double maxFps = 0.0;
    int resets = 0;

    while (!stopRequested_) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (stopRequested_) {
            break;
        }

        if (IsIconic(hwnd)) {
            // Minimised: nothing is drawn, the clock keeps its nominal pace.
            Sleep(50);
            TestClock::instance().frame(0.05);
            lastFrame = nowSeconds();
            continue;
        }

        if (resizePending_.exchange(false)) {
            // Keep covering the whole monitor, whatever its mode is now.
            const RECT rect = monitorRectFor(hwnd);
            RECT window{};
            GetWindowRect(hwnd, &window);
            if (!EqualRect(&rect, &window)) {
                SetWindowPos(hwnd, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            GetClientRect(hwnd, &client);
            if (client.right > 0 && client.bottom > 0 &&
                (client.right != renderer.width || client.bottom != renderer.height)) {
                if (!renderer.resize(client.right, client.bottom, &error)) {
                    finish(State::Failed, error);
                    return;
                }
            }
        }

        const double now = nowSeconds();
        const float time = static_cast<float>(std::fmod(now - started, 7200.0));
        renderer.renderScene(time, kSceneQuality, stripsFor(frameMs), frameMs > 80.0);
        renderer.composite();
        const HRESULT hr = renderer.swap->Present(0, 0);

        if (isDeviceLost(hr)) {
            // The driver gave up under load: exactly what this test is looking for.
            ++resets;
            const HRESULT reason = renderer.device ? renderer.device->GetDeviceRemovedReason() : hr;
            logLine(L"[GPU] Dispositivo perdido (" + hresultText(hr) + L", motivo " + hresultText(reason) +
                    L").");
            {
                std::lock_guard lock(mutex_);
                stats_.deviceResets = resets;
            }
            renderer.release();
            if (resets >= 3) {
                break;
            }
            Sleep(1500);
            GetClientRect(hwnd, &client);
            if (!renderer.init(hwnd, std::max<int>(1, client.right), std::max<int>(1, client.bottom), &error)) {
                break;
            }
            lastFrame = nowSeconds();
            continue;
        }

        ++frames;
        ++windowFrames;
        const double after = nowSeconds();
        const double frameSeconds = after - lastFrame;
        lastFrame = after;
        TestClock::instance().frame(frameSeconds);
        frameMs = frameMs * 0.8 + frameSeconds * 1000.0 * 0.2;

        if (after - windowStart >= 0.5) {
            const double fps = windowFrames / (after - windowStart);
            windowStart = after;
            windowFrames = 0;
            history_.push(static_cast<float>(fps));
            // Skip the first seconds (shader warm-up).
            if (after - started > 3.0) {
                minFps = minFps <= 0.0 ? fps : std::min(minFps, fps);
                maxFps = std::max(maxFps, fps);
            }
            std::lock_guard lock(mutex_);
            stats_.fps = fps;
            stats_.frameMs = fps > 0.0 ? 1000.0 / fps : 0.0;
            stats_.avgFps = frames / std::max(0.001, after - started);
            stats_.minFps = minFps;
            stats_.maxFps = maxFps;
            stats_.frames = frames;
            stats_.elapsed = after - started;
            stats_.width = renderer.width;
            stats_.height = renderer.height;
        }
    }

    const double elapsed = nowSeconds() - started;
    const double avg = frames / std::max(0.001, elapsed);
    int width = 0;
    int height = 0;
    {
        std::lock_guard lock(mutex_);
        stats_.elapsed = elapsed;
        stats_.frames = frames;
        stats_.avgFps = avg;
        stats_.deviceResets = resets;
        width = stats_.width;
        height = stats_.height;
        if (resets == 0 && frames > 0 && elapsed >= 5.0) {
            // Pixel throughput, so results at different resolutions compare.
            stats_.score = static_cast<int>(std::lround(
                avg * 100.0 * (static_cast<double>(width) * height) / (1920.0 * 1080.0)));
        }
    }

    if (resets > 0) {
        finish(State::Failed, L"O controlador gráfico falhou " + std::to_wstring(resets) +
                                  L" vez(es) sob carga (reset do dispositivo).");
    } else if (frames > 0 && elapsed >= 5.0) {
        finish(State::Passed, L"Sem falhas do controlador em " + formatDuration(elapsed) + L" — " +
                                  formatNumber(avg, 1) + L" FPS médios a " + std::to_wstring(width) + L"×" +
                                  std::to_wstring(height) + L".");
    } else {
        finish(State::Stopped, L"Interrompido.");
    }
}

#ifdef SU_UI_SNAPSHOT
bool GpuTest::renderSnapshot(const std::filesystem::path& file, int width, int height, float time,
                             float quality, std::wstring* error) {
    Renderer renderer;
    if (!renderer.init(nullptr, width, height, error)) {
        return false;
    }
    renderer.renderScene(time, quality, kMinStrips, true);

    D3D11_TEXTURE2D_DESC desc{};
    renderer.sceneTex->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = renderer.device->CreateTexture2D(&desc, nullptr, staging.put());
    if (FAILED(hr) || !staging) {
        return renderer.fail(error, L"Staging texture", hr);
    }
    renderer.ctx->CopyResource(staging.get(), renderer.sceneTex.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = renderer.ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        return renderer.fail(error, L"Map staging", hr);
    }

    std::vector<uint8_t> bgra(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        const uint8_t* src = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        uint8_t* dst = bgra.data() + static_cast<size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = 255;
        }
    }
    renderer.ctx->Unmap(staging.get(), 0);

    CLSID png{};
    CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &png);
    Gdiplus::Bitmap image(width, height, width * 4, PixelFormat32bppRGB, bgra.data());
    const bool saved = image.Save(file.c_str(), &png, nullptr) == Gdiplus::Ok;
    if (!saved && error) {
        *error = L"Falha ao gravar o PNG.";
    }
    if (error && renderer.fallbackShader) {
        *error = L"(shader simplificado em uso)";
    }
    return saved;
}
#endif

}  // namespace nt

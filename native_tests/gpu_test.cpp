#include "gpu_test.hpp"

#include "gpu_shaders.hpp"

#include "com_ptr.hpp"
#include "ui.hpp"  // GDI+ (PNG snapshots)

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

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

constexpr int kBenchWidth = 1280;
constexpr int kBenchHeight = 720;
constexpr float kBenchQuality = 0.70f;
constexpr float kBurnQuality = 0.75f;
// The scene pass is drawn in horizontal bands: no single draw call can keep a
// weak GPU busy long enough to trip Windows' 2-second driver watchdog (TDR).
constexpr int kStrips = 6;
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
    ComPtr<ID3D11PixelShader> psHud;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11BlendState> blendAlpha;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11Texture2D> hudTex;
    ComPtr<ID3D11ShaderResourceView> hudSrv;
    int hudW = 0;
    int hudH = 0;
    int width = 0;   // back buffer
    int height = 0;
    int sceneW = 0;  // scene texture
    int sceneH = 0;
    bool fallbackShader = false;
    std::wstring adapterName;
    uint64_t vramBytes = 0;
    bool software = false;

    void release() {
        if (ctx) {
            ctx->ClearState();
        }
        hudSrv.reset();
        hudTex.reset();
        raster.reset();
        blendAlpha.reset();
        sampler.reset();
        constants.reset();
        psHud.reset();
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
        hudW = hudH = 0;
    }

    bool fail(std::wstring* error, const wchar_t* what, HRESULT hr) {
        const std::wstring text = std::wstring(what) + L" (" + hresultText(hr) + L")";
        logLine(L"[GPU] " + text);
        if (error) {
            *error = text;
        }
        return false;
    }

    // hwnd == nullptr → off-screen device (snapshots).
    bool init(HWND hwnd, int w, int h, std::wstring* error) {
        release();
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                            D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_10_0;
        HRESULT hr = E_FAIL;
        if (hwnd != nullptr) {
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
            hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 3,
                                               D3D11_SDK_VERSION, &desc, swap.put(), device.put(), &level,
                                               ctx.put());
        } else {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 3,
                                   D3D11_SDK_VERSION, device.put(), &level, ctx.put());
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
        createPixelShader(bytecode::kHud, sizeof(bytecode::kHud), psHud);
        if (vs && psScene && psBlit && psHud) {
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
        if (!psHud) {
            if (!compileShader(kGpuShaderSource, "PSHud", "ps_4_0", &blob, error) ||
                !createPixelShader(blob.data(), blob.size(), psHud)) {
                return fail(error, L"Não foi possível criar o shader do painel", E_FAIL);
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

        // Premultiplied alpha for the HUD overlay.
        D3D11_BLEND_DESC blendDesc{};
        blendDesc.RenderTarget[0].BlendEnable = TRUE;
        blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&blendDesc, blendAlpha.put());
        if (FAILED(hr)) {
            return fail(error, L"CreateBlendState falhou", hr);
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

        // The scene texture is at least benchmark-sized; a sub-rectangle of it is
        // rendered when the internal resolution is lower than the output.
        sceneW = swap ? std::max(width, kBenchWidth) : width;
        sceneH = swap ? std::max(height, kBenchHeight) : height;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(sceneW);
        desc.Height = static_cast<UINT>(sceneH);
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

    void setViewport(int x, int y, int w, int h) {
        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = static_cast<float>(x);
        viewport.TopLeftY = static_cast<float>(y);
        viewport.Width = static_cast<float>(w);
        viewport.Height = static_cast<float>(h);
        viewport.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &viewport);
    }

    // Ray-marches the scene into the top-left renderW × renderH of the scene texture.
    void renderScene(float time, float quality, int renderW, int renderH) {
        renderW = std::clamp(renderW, 16, sceneW);
        renderH = std::clamp(renderH, 16, sceneH);
        FrameConstants frame{};
        frame.res[0] = static_cast<float>(renderW);
        frame.res[1] = static_cast<float>(renderH);
        frame.res[2] = 1.0f / static_cast<float>(renderW);
        frame.res[3] = 1.0f / static_cast<float>(renderH);
        frame.params[0] = time;
        frame.params[1] = quality;
        frame.params[2] = static_cast<float>(renderW) / static_cast<float>(renderH);
        frame.blit[0] = static_cast<float>(renderW) / static_cast<float>(sceneW);
        frame.blit[1] = static_cast<float>(renderH) / static_cast<float>(sceneH);
        frame.blit[2] = (static_cast<float>(renderW) - 0.5f) / static_cast<float>(sceneW);
        frame.blit[3] = (static_cast<float>(renderH) - 0.5f) / static_cast<float>(sceneH);

        ID3D11ShaderResourceView* noTexture = nullptr;
        ctx->PSSetShaderResources(0, 1, &noTexture);
        ID3D11RenderTargetView* target = sceneRtv.get();
        ctx->OMSetRenderTargets(1, &target, nullptr);
        bindCommon(frame);
        ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        ctx->PSSetShader(psScene.get(), nullptr, 0);
        setViewport(0, 0, renderW, renderH);
        for (int strip = 0; strip < kStrips; ++strip) {
            D3D11_RECT rect{0, renderH * strip / kStrips, renderW, renderH * (strip + 1) / kStrips};
            ctx->RSSetScissorRects(1, &rect);
            ctx->Draw(3, 0);
        }
    }

    // Stretches the scene to the back buffer and draws the HUD on top.
    void composite(bool drawHud, int hudX, int hudY) {
        ID3D11RenderTargetView* target = backRtv.get();
        ctx->OMSetRenderTargets(1, &target, nullptr);
        setViewport(0, 0, width, height);
        D3D11_RECT full{0, 0, width, height};
        ctx->RSSetScissorRects(1, &full);
        ctx->PSSetShader(psBlit.get(), nullptr, 0);
        ID3D11ShaderResourceView* scene = sceneSrv.get();
        ctx->PSSetShaderResources(0, 1, &scene);
        ctx->Draw(3, 0);

        if (drawHud && hudSrv && hudW > 0 && hudH > 0) {
            setViewport(hudX, hudY, hudW, hudH);
            ctx->OMSetBlendState(blendAlpha.get(), nullptr, 0xFFFFFFFF);
            ctx->PSSetShader(psHud.get(), nullptr, 0);
            ID3D11ShaderResourceView* hud = hudSrv.get();
            ctx->PSSetShaderResources(0, 1, &hud);
            ctx->Draw(3, 0);
            ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        }
    }

    void uploadHud(int w, int h, const std::vector<uint8_t>& pixels) {
        if (w <= 0 || h <= 0 || pixels.size() < static_cast<size_t>(w) * static_cast<size_t>(h) * 4) {
            hudSrv.reset();
            hudTex.reset();
            hudW = hudH = 0;
            return;
        }
        if (!hudTex || w != hudW || h != hudH) {
            hudSrv.reset();
            hudTex.reset();
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = static_cast<UINT>(w);
            desc.Height = static_cast<UINT>(h);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, hudTex.put())) || !hudTex ||
                FAILED(device->CreateShaderResourceView(hudTex.get(), nullptr, hudSrv.put()))) {
                hudSrv.reset();
                hudTex.reset();
                hudW = hudH = 0;
                return;
            }
            hudW = w;
            hudH = h;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(hudTex.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            const size_t rowBytes = static_cast<size_t>(w) * 4;
            for (int y = 0; y < h; ++y) {
                std::memcpy(static_cast<uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                            pixels.data() + static_cast<size_t>(y) * rowBytes, rowBytes);
            }
            ctx->Unmap(hudTex.get(), 0);
        }
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

RECT workRectFor(HWND anchor) {
    HMONITOR monitor = MonitorFromWindow(anchor ? anchor : GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &info)) {
        return info.rcWork;
    }
    return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

constexpr DWORD kWindowedStyle = WS_OVERLAPPEDWINDOW;
constexpr DWORD kFullscreenStyle = WS_POPUP;

// Windowed placement: 1280×720 client, shrunk to fit the monitor's work area.
RECT windowedRect(HWND anchor) {
    const RECT work = workRectFor(anchor);
    RECT frame{0, 0, 0, 0};
    AdjustWindowRectEx(&frame, kWindowedStyle, FALSE, 0);
    const int frameW = frame.right - frame.left;
    const int frameH = frame.bottom - frame.top;
    const int availW = std::max<int>(320, (work.right - work.left) - frameW - 40);
    const int availH = std::max<int>(180, (work.bottom - work.top) - frameH - 40);
    const float fit = std::min({1.0f, static_cast<float>(availW) / kBenchWidth,
                                static_cast<float>(availH) / kBenchHeight});
    const int w = static_cast<int>(kBenchWidth * fit) + frameW;
    const int h = static_cast<int>(kBenchHeight * fit) + frameH;
    const int x = work.left + ((work.right - work.left) - w) / 2;
    const int y = work.top + ((work.bottom - work.top) - h) / 2;
    return RECT{x, y, x + w, y + h};
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

void GpuTest::setHud(int width, int height, std::vector<uint8_t> pixels) {
    std::lock_guard lock(hudMutex_);
    hudPixels_ = std::move(pixels);
    hudWidth_ = width;
    hudHeight_ = height;
    ++hudVersion_;
}

bool GpuTest::queryAdapter(std::wstring* name, uint64_t* vramBytes, bool* software) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), asVoid(factory))) || !factory) {
        return false;
    }
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(0, adapter.put())) || !adapter) {
        return false;
    }
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc))) {
        return false;
    }
    if (name) {
        *name = desc.Description;
    }
    if (vramBytes) {
        *vramBytes = desc.DedicatedVideoMemory;
    }
    if (software) {
        *software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ||
                    (desc.VendorId == 0x1414 && desc.DeviceId == 0x8c);
    }
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
    toggleFullscreen_ = false;
    hudVisible_ = true;
    history_.clear();
    {
        std::lock_guard lock(mutex_);
        stats_ = GpuStats{};
        stats_.state = State::Starting;
        stats_.message = L"A preparar a cena (na primeira vez compila os shaders, pode demorar)...";
        stats_.benchmark = options.mode == Mode::Benchmark;
        stats_.duration = options.mode == Mode::Benchmark ? options.benchmarkSeconds : 0.0;
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
                self->stopRequested_ = true;
            } else if (wParam == VK_F11) {
                self->toggleFullscreen_ = true;
            } else if (wParam == 'H') {
                self->hudVisible_ = !self->hudVisible_.load();
            }
            return 0;
        case WM_SIZE:
            self->resizePending_ = true;
            return 0;
        case WM_CLOSE:
            self->stopRequested_ = true;
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SETCURSOR:
            if (self->fullscreen_ && LOWORD(lParam) == HTCLIENT) {
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
    const bool benchmark = options.mode == Mode::Benchmark;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND hwnd = nullptr;
    Renderer renderer;

    const auto finish = [&](State state, const std::wstring& message) {
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
        }
        {
            std::lock_guard lock(hudMutex_);
            hudPixels_.clear();
            hudWidth_ = hudHeight_ = 0;
            ++hudVersion_;
        }
        logLine(L"[GPU] " + message);
        active_ = false;
    };

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

    bool fullscreen = options.fullscreen;
    RECT windowed = windowedRect(options.placeNear);
    {
        const RECT rect = fullscreen ? monitorRectFor(options.placeNear) : windowed;
        hwnd = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass,
                               benchmark ? L"Void Burner — benchmark" : L"Void Burner — teste gráfico",
                               fullscreen ? kFullscreenStyle : kWindowedStyle, rect.left, rect.top,
                               rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance,
                               this);
    }
    if (hwnd == nullptr) {
        finish(State::Unavailable, L"Não foi possível criar a janela do teste gráfico.");
        return;
    }
    fullscreen_ = fullscreen;
    ui::applyDarkFrame(hwnd);
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
        stats_.fullscreen = fullscreen;
    }
    logLine(L"[GPU] " + renderer.adapterName + L" — " + (benchmark ? L"benchmark" : L"burn") +
            (renderer.fallbackShader ? L" (shader simplificado)." : L"."));

    const double started = nowSeconds();
    double windowStart = started;
    double adaptStart = started;
    int windowFrames = 0;
    int adaptFrames = 0;
    uint64_t frames = 0;
    double minFps = 0.0;
    double maxFps = 0.0;
    float scale = 0.5f;  // burn mode: internal resolution relative to the window
    int resets = 0;
    uint64_t hudSeen = 0;
    bool completed = false;

    while (!stopRequested_) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (stopRequested_) {
            break;
        }

        if (toggleFullscreen_.exchange(false)) {
            fullscreen = !fullscreen;
            fullscreen_ = fullscreen;
            if (fullscreen) {
                GetWindowRect(hwnd, &windowed);
                const RECT rect = monitorRectFor(hwnd);
                SetWindowLongPtrW(hwnd, GWL_STYLE, kFullscreenStyle | WS_VISIBLE);
                SetWindowPos(hwnd, HWND_TOP, rect.left, rect.top, rect.right - rect.left,
                             rect.bottom - rect.top, SWP_FRAMECHANGED);
            } else {
                SetWindowLongPtrW(hwnd, GWL_STYLE, kWindowedStyle | WS_VISIBLE);
                SetWindowPos(hwnd, HWND_TOP, windowed.left, windowed.top, windowed.right - windowed.left,
                             windowed.bottom - windowed.top, SWP_FRAMECHANGED);
            }
            resizePending_ = true;
        }

        if (IsIconic(hwnd)) {
            Sleep(50);
            continue;
        }

        if (resizePending_.exchange(false)) {
            GetClientRect(hwnd, &client);
            if (client.right > 0 && client.bottom > 0 &&
                (client.right != renderer.width || client.bottom != renderer.height)) {
                if (!renderer.resize(client.right, client.bottom, &error)) {
                    finish(State::Failed, error);
                    return;
                }
            }
        }

        // New HUD bitmap from the bench window?
        {
            std::lock_guard lock(hudMutex_);
            if (hudVersion_ != hudSeen) {
                hudSeen = hudVersion_;
                renderer.uploadHud(hudWidth_, hudHeight_, hudPixels_);
            }
        }

        const double now = nowSeconds();
        const float time = static_cast<float>(std::fmod(now - started, 7200.0));
        int renderW = kBenchWidth;
        int renderH = kBenchHeight;
        if (!benchmark) {
            renderW = std::max(96, static_cast<int>(std::lround(renderer.width * scale)));
            renderH = std::max(54, static_cast<int>(std::lround(renderer.height * scale)));
        }
        renderer.renderScene(time, benchmark ? kBenchQuality : kBurnQuality, renderW, renderH);
        const int hudMargin = std::max(8, renderer.height / 45);
        renderer.composite(hudVisible_, hudMargin, hudMargin);
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
            hudSeen = 0;
            continue;
        }

        ++frames;
        ++windowFrames;
        ++adaptFrames;
        const double after = nowSeconds();

        if (after - windowStart >= 0.5) {
            const double fps = windowFrames / (after - windowStart);
            windowStart = after;
            windowFrames = 0;
            history_.push(static_cast<float>(fps));
            // Skip the first seconds (shader warm-up, resolution still adapting).
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
            stats_.renderWidth = renderW;
            stats_.renderHeight = renderH;
            stats_.fullscreen = fullscreen;
        }

        // Burn mode: keep the frame time comfortable by trading resolution.
        if (!benchmark && after - adaptStart >= 1.0) {
            const double fps = adaptFrames / (after - adaptStart);
            adaptStart = after;
            adaptFrames = 0;
            // Resolution is preferred over frame rate: settle at the sharpest
            // image that still gives roughly 20-40 FPS.
            if (fps < 20.0) {
                scale = std::max(0.2f, scale * 0.85f);
            } else if (fps > 40.0) {
                scale = std::min(1.0f, scale * 1.12f);
            }
        }

        if (benchmark && after - started >= options.benchmarkSeconds) {
            completed = true;
            break;
        }
    }

    const double elapsed = nowSeconds() - started;
    const double avg = frames / std::max(0.001, elapsed);
    {
        std::lock_guard lock(mutex_);
        stats_.elapsed = elapsed;
        stats_.frames = frames;
        stats_.avgFps = avg;
        stats_.deviceResets = resets;
        if (completed) {
            stats_.score = static_cast<int>(std::lround(avg * 100.0));
        }
    }

    if (resets > 0) {
        finish(State::Failed, L"O controlador gráfico falhou " + std::to_wstring(resets) +
                                  L" vez(es) sob carga (reset do dispositivo).");
    } else if (benchmark && completed) {
        finish(State::Passed, L"Benchmark concluído: " + formatNumber(avg, 1) + L" FPS médios a 1280×720.");
    } else if (!benchmark && frames > 0 && elapsed >= 5.0) {
        finish(State::Passed, L"Sem falhas do controlador em " + formatDuration(elapsed) + L" (" +
                                  formatNumber(avg, 1) + L" FPS médios).");
    } else {
        finish(State::Stopped, L"Interrompido.");
    }
}

#ifdef SU_UI_SNAPSHOT
bool GpuTest::renderSnapshot(const std::filesystem::path& file, int width, int height, float time,
                             float quality, std::wstring* error, const std::vector<uint8_t>* hud,
                             int hudWidth, int hudHeight, float renderScale) {
    Renderer renderer;
    if (!renderer.init(nullptr, width, height, error)) {
        return false;
    }
    ComPtr<ID3D11Texture2D> source;
    if (hud != nullptr) {
        // Stand-in for the swap chain's back buffer.
        D3D11_TEXTURE2D_DESC backDesc{};
        backDesc.Width = static_cast<UINT>(width);
        backDesc.Height = static_cast<UINT>(height);
        backDesc.MipLevels = 1;
        backDesc.ArraySize = 1;
        backDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        backDesc.SampleDesc.Count = 1;
        backDesc.Usage = D3D11_USAGE_DEFAULT;
        backDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        HRESULT created = renderer.device->CreateTexture2D(&backDesc, nullptr, source.put());
        if (FAILED(created) || !source ||
            FAILED(renderer.device->CreateRenderTargetView(source.get(), nullptr, renderer.backRtv.put()))) {
            return renderer.fail(error, L"Back buffer de teste", created);
        }
        renderer.renderScene(time, quality, static_cast<int>(width * renderScale),
                             static_cast<int>(height * renderScale));
        renderer.uploadHud(hudWidth, hudHeight, *hud);
        const int margin = std::max(8, height / 45);
        renderer.composite(true, margin, margin);
    } else {
        renderer.renderScene(time, quality, width, height);
    }
    ID3D11Texture2D* readFrom = hud != nullptr ? source.get() : renderer.sceneTex.get();

    D3D11_TEXTURE2D_DESC desc{};
    readFrom->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = renderer.device->CreateTexture2D(&desc, nullptr, staging.put());
    if (FAILED(hr) || !staging) {
        return renderer.fail(error, L"Staging texture", hr);
    }
    renderer.ctx->CopyResource(staging.get(), readFrom);
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

#include "renderer.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <deque>
#include <dxgi1_2.h>
#include <sstream>
#include <vrst/image.hpp>
#include <wrl/client.h>

namespace vrst::preview {
using Microsoft::WRL::ComPtr;
namespace {
constexpr char shader[] = R"(
cbuffer Params : register(b0) {
 float4 inputFov, outputFov, correction, focusFov, focusCorrection;
 float4 rect0, rect1, clamp0, clamp1, flags;
 float4 imageRowX, imageRowY, imageMode;
};
Texture2D contextImage : register(t0);
Texture2D focusImage : register(t1);
SamplerState linearClamp : register(s0);
struct Varying { float4 pos:SV_POSITION; float2 uv:TEXCOORD0; };
Varying VS(uint id:SV_VertexID) {
 Varying v; v.uv=float2((id<<1)&2,id&2);
 v.pos=float4(v.uv*float2(2,-2)+float2(-1,1),0,1); return v;
}
float3 rotateQ(float4 q,float3 v) { return v+2*cross(q.xyz,cross(q.xyz,v)+q.w*v); }
float2 project(float3 ray,float4 fov) {
 float2 t=ray.xy/max(.05,-ray.z);
 return float2((t.x-fov.x)/(fov.y-fov.x),(fov.z-t.y)/(fov.z-fov.w));
}
float4 PS(Varying v):SV_TARGET {
 if (imageMode.x>.5) {
   float3 at=float3(v.uv,1);
   float2 uv=float2(dot(imageRowX.xyz,at),dot(imageRowY.xyz,at));
   if (flags.x>.5) {
     if(any(uv < -imageMode.y)||any(uv > 1+imageMode.y))return float4(0,0,0,1);
     uv=1-abs(1-2*frac(uv*.5));
   }
   else if(any(uv<0)||any(uv>1))return float4(0,0,0,1);
   return float4(contextImage.Sample(linearClamp,clamp(uv,clamp0.xy,clamp0.zw)).rgb,1);
 }
 float3 targetRay=float3(lerp(outputFov.x,outputFov.y,v.uv.x),lerp(outputFov.z,outputFov.w,v.uv.y),-1);
 float3 ray=rotateQ(correction,targetRay);
 float2 uv=project(ray,inputFov);
 bool outside=any(uv<0)||any(uv>1)||ray.z>=-.05;
 if (flags.x>.5) {
   if(ray.z>=-.05||any(uv < -imageMode.y)||any(uv > 1+imageMode.y))return float4(0,0,0,1);
   uv=1-abs(1-2*frac(uv*.5));
 }
 else if (outside) return float4(0,0,0,1);
 float2 sampleUV=clamp(rect0.xy+uv*rect0.zw,clamp0.xy,clamp0.zw);
 float4 color=contextImage.Sample(linearClamp,sampleUV);
 if (flags.y>.5 && !outside) {
   float3 focusRay=rotateQ(focusCorrection,targetRay);
   float2 focusUV=project(focusRay,focusFov);
   if (focusRay.z<-.05 && all(focusUV>0) && all(focusUV<1)) {
     float2 edge=min(focusUV,1-focusUV)*flags.zw;
     float blend=saturate(min(edge.x,edge.y)*.5);
     float2 at=clamp(rect1.xy+focusUV*rect1.zw,clamp1.xy,clamp1.zw);
     color=lerp(color,focusImage.Sample(linearClamp,at),blend);
   }
 }
 return float4(color.rgb,1);
}
float4 Downsample(Varying v):SV_TARGET { return contextImage.Sample(linearClamp,v.uv); }
)";
struct Params {
    float input[4], output[4], correction[4], focus[4], focus_correction[4];
    float rect0[4], rect1[4], clamp0[4], clamp1[4], flags[4];
    float image_row_x[4], image_row_y[4], image_mode[4];
};
void put(float *to, Projection p) {
    to[0] = float(p.left);
    to[1] = float(p.right);
    to[2] = float(p.up);
    to[3] = float(p.down);
}
void put(float *to, Quat q) {
    to[0] = float(q.x);
    to[1] = float(q.y);
    to[2] = float(q.z);
    to[3] = float(q.w);
}
void rect(float *r, float *c, const View &v) {
    r[0] = float(v.x) / v.width;
    r[1] = float(v.y) / v.height;
    r[2] = float(v.rect_width) / v.width;
    r[3] = float(v.rect_height) / v.height;
    c[0] = (v.x + .5f) / v.width;
    c[1] = (v.y + .5f) / v.height;
    c[2] = (v.x + v.rect_width - .5f) / v.width;
    c[3] = (v.y + v.rect_height - .5f) / v.height;
}
bool same(LUID a, LUID b) {
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}
} // namespace

struct Renderer::Impl {
    HWND window{};
    LUID adapter{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11Texture2D> back;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11Texture2D> output;
    ComPtr<ID3D11RenderTargetView> output_target;
    ComPtr<ID3D11ShaderResourceView> output_view;
    ComPtr<ID3D11ShaderResourceView> waiting_view;
    std::wstring waiting_label;
    unsigned output_width{}, output_height{};
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11PixelShader> downsample;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Texture2D> local[2], shared[2];
    ComPtr<IDXGIKeyedMutex> keyed[2];
    ComPtr<ID3D11ShaderResourceView> srv[2];
    Frame frame{};
    Frame opened{};
    FrameStabilizer stabilizer;
    ImageStabilizer image_stabilizer;
    ImageResult image_result{};
    bool window_mode{};
    struct Pending {
        ComPtr<ID3D11Texture2D> full, small, staging;
        ComPtr<ID3D11ShaderResourceView> view;
        ComPtr<ID3D11RenderTargetView> target;
        unsigned width{}, height{}, analysis_width{}, analysis_height{};
        std::uint64_t time_ns{};
    };
    std::deque<std::unique_ptr<Pending>> pending;
    std::vector<std::unique_ptr<Pending>> free_pending;
    std::uint64_t last_window_time{}, window_sequence{};
    std::wstring error;
    unsigned width{}, height{};
    std::uint64_t demo_sequence{};

    bool fail(const wchar_t *where, HRESULT hr = E_FAIL) {
        std::wostringstream stream;
        stream << where << L" (0x" << std::hex << static_cast<unsigned long>(hr) << L")";
        if (device && FAILED(device->GetDeviceRemovedReason()))
            release();
        error = stream.str();
        return false;
    }
    void release() {
        if (context) {
            context->ClearState();
            context->Flush();
        }
        for (unsigned i = 0; i < 2; ++i) {
            srv[i].Reset();
            local[i].Reset();
            keyed[i].Reset();
            shared[i].Reset();
        }
        pending.clear();
        free_pending.clear();
        image_stabilizer.reset();
        image_result = {};
        window_mode = false;
        last_window_time = 0;
        downsample.Reset();
        target.Reset();
        back.Reset();
        output_view.Reset();
        output_target.Reset();
        waiting_view.Reset();
        waiting_label.clear();
        output.Reset();
        output_width = output_height = 0;
        sampler.Reset();
        constants.Reset();
        ps.Reset();
        vs.Reset();
        swap.Reset();
        context.Reset();
        device.Reset();
        width = height = 0;
        frame = {};
        opened = {};
        stabilizer.reset();
    }
    bool initialize(const LUID *wanted = nullptr) {
        release();
        ComPtr<IDXGIFactory1> factory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr))
            return fail(L"DXGI", hr);
        ComPtr<IDXGIAdapter1> selected;
        for (unsigned i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> item;
            if (factory->EnumAdapters1(i, &item) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 desc{};
            item->GetDesc1(&desc);
            if ((wanted && same(desc.AdapterLuid, *wanted)) ||
                (!wanted && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))) {
                selected = item;
                adapter = desc.AdapterLuid;
                break;
            }
        }
        if (!selected)
            return fail(L"Game graphics adapter not found");
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        hr = D3D11CreateDevice(selected.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, &level, 1, D3D11_SDK_VERSION, &device,
                               nullptr, &context);
        if (FAILED(hr))
            return fail(L"D3D11CreateDevice", hr);
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Width = 960;
        desc.BufferDesc.Height = 540;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.OutputWindow = window;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hr = factory->CreateSwapChain(device.Get(), &desc, &swap);
        if (FAILED(hr))
            return fail(L"Preview window", hr);
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        ComPtr<ID3DBlob> vblob, pblob, errors;
        hr = D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, "VS", "vs_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vblob, &errors);
        if (FAILED(hr))
            return fail(L"Vertex shader", hr);
        hr = D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, "PS", "ps_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pblob, &errors);
        if (FAILED(hr))
            return fail(L"Pixel shader", hr);
        if (FAILED(hr = device->CreateVertexShader(vblob->GetBufferPointer(), vblob->GetBufferSize(), nullptr,
                                                   &vs)) ||
            FAILED(hr = device->CreatePixelShader(pblob->GetBufferPointer(), pblob->GetBufferSize(), nullptr,
                                                  &ps)))
            return fail(L"Shader", hr);
        ComPtr<ID3DBlob> small_blob;
        hr = D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, "Downsample", "ps_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &small_blob, &errors);
        if (FAILED(hr) ||
            FAILED(hr = device->CreatePixelShader(small_blob->GetBufferPointer(), small_blob->GetBufferSize(),
                                                  nullptr, &downsample)))
            return fail(L"Analysis shader", hr);
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(Params);
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device->CreateBuffer(&cb, nullptr, &constants);
        if (FAILED(hr))
            return fail(L"Shader parameters", hr);
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = device->CreateSamplerState(&sd, &sampler);
        if (FAILED(hr))
            return fail(L"Sampler", hr);
        error.clear();
        return resize();
    }
    bool resize() {
        RECT client{};
        GetClientRect(window, &client);
        unsigned w = std::max<LONG>(16, client.right), h = std::max<LONG>(16, client.bottom);
        if (w == width && h == height && target)
            return true;
        context->OMSetRenderTargets(0, nullptr, nullptr);
        target.Reset();
        back.Reset();
        HRESULT hr = swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr))
            return fail(L"Window dimensions", hr);
        hr = swap->GetBuffer(0, IID_PPV_ARGS(&back));
        if (FAILED(hr))
            return fail(L"Output image", hr);
        D3D11_RENDER_TARGET_VIEW_DESC rt{};
        rt.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        rt.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        hr = device->CreateRenderTargetView(back.Get(), &rt, &target);
        if (FAILED(hr))
            return fail(L"Output target", hr);
        width = w;
        height = h;
        return true;
    }
    bool resize_output(const Options &options) {
        unsigned w = std::clamp(options.output_width, 320u, 7680u),
                 h = std::clamp(options.output_height, 180u, 4320u);
        if (output && w == output_width && h == output_height)
            return true;
        context->OMSetRenderTargets(0, nullptr, nullptr);
        output_view.Reset();
        output_target.Reset();
        output.Reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = d.ArraySize = 1;
        d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device->CreateTexture2D(&d, nullptr, &output);
        D3D11_RENDER_TARGET_VIEW_DESC rt{};
        rt.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        rt.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = rt.Format;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        if (FAILED(hr) || FAILED(hr = device->CreateRenderTargetView(output.Get(), &rt, &output_target)) ||
            FAILED(hr = device->CreateShaderResourceView(output.Get(), &sv, &output_view))) {
            output.Reset();
            return fail(L"Output resolution allocation", hr);
        }
        output_width = w;
        output_height = h;
        return true;
    }
    bool make_waiting(const wchar_t *label) {
        if (waiting_view && waiting_label == label)
            return true;
        constexpr int width = 512, height = 64;
        HDC dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void *pixels{};
        auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!dc || !bitmap) {
            if (bitmap)
                DeleteObject(bitmap);
            if (dc)
                DeleteDC(dc);
            return fail(L"Waiting screen text");
        }
        auto old = SelectObject(dc, bitmap);
        RECT area{0, 0, width, height};
        FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        auto font =
            CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        auto old_font = SelectObject(dc, font);
        SetTextColor(dc, RGB(160, 166, 176));
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, label, -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        GdiFlush();
        for (int i = 0; i < width * height; ++i)
            static_cast<std::uint32_t *>(pixels)[i] |= 0xff000000;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = width;
        d.Height = height;
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA input{pixels, width * 4, 0};
        ComPtr<ID3D11Texture2D> texture;
        HRESULT hr = device->CreateTexture2D(&d, &input, &texture);
        waiting_view.Reset();
        if (SUCCEEDED(hr))
            hr = device->CreateShaderResourceView(texture.Get(), nullptr, &waiting_view);
        SelectObject(dc, old_font);
        SelectObject(dc, old);
        DeleteObject(font);
        DeleteObject(bitmap);
        DeleteDC(dc);
        if (FAILED(hr))
            return fail(L"Waiting screen texture", hr);
        waiting_label = label;
        return true;
    }
    bool make_local(unsigned i, const D3D11_TEXTURE2D_DESC &input) {
        D3D11_TEXTURE2D_DESC desc = input;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        local[i].Reset();
        srv[i].Reset();
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &local[i]);
        if (FAILED(hr))
            return fail(L"Image copy", hr);
        hr = device->CreateShaderResourceView(local[i].Get(), nullptr, &srv[i]);
        if (FAILED(hr))
            return fail(L"Image format", hr);
        return true;
    }
};

Renderer::Renderer() : impl_(new Impl) {}
Renderer::~Renderer() = default;
ID3D11Device *Renderer::device() const {
    return impl_->device.Get();
}
bool Renderer::consume_window(ID3D11Texture2D *texture, std::uint64_t time_ns, const Options &options) {
    auto &r = *impl_;
    if (!r.device)
        return false;
    if (!r.window_mode) {
        source_changed();
        r.window_mode = true;
    }
    bool completed = false;
    // Read only completed small analysis buffers. The full-size source is kept
    // in the same queue entry, so delayed GPU readback cannot mix two frames.
    if (!r.pending.empty()) {
        auto &p = *r.pending.front();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const auto hr =
            r.context->Map(p.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == S_OK) {
            GrayImage gray;
            gray.width = p.analysis_width;
            gray.height = p.analysis_height;
            gray.pixels.resize(std::size_t(gray.width) * gray.height);
            for (unsigned y = 0; y < gray.height; ++y) {
                const auto *row = static_cast<const unsigned char *>(mapped.pData) + y * mapped.RowPitch;
                for (unsigned x = 0; x < gray.width; ++x)
                    gray.pixels[std::size_t(y) * gray.width + x] =
                        (.2126f * row[x * 4] + .7152f * row[x * 4 + 1] + .0722f * row[x * 4 + 2]) / 255.f;
            }
            r.context->Unmap(p.staging.Get(), 0);
            ImageSettings settings;
            settings.smoothing = options.smoothing;
            r.image_result = r.image_stabilizer.process(gray, p.time_ns, settings);
            std::swap(r.local[0], p.full);
            std::swap(r.srv[0], p.view);
            r.frame = {};
            r.frame.generation = r.frame.session = 1;
            r.frame.sequence = ++r.window_sequence;
            r.frame.display_time = static_cast<std::int64_t>(p.time_ns);
            r.frame.producer_pid = GetCurrentProcessId();
            r.frame.view_count = 1;
            auto &v = r.frame.views[0];
            v.width = v.rect_width = p.width;
            v.height = v.rect_height = p.height;
            v.texture_handle = 1;
            D3D11_TEXTURE2D_DESC full{};
            r.local[0]->GetDesc(&full);
            v.format = full.Format;
            r.free_pending.push_back(std::move(r.pending.front()));
            r.pending.pop_front();
            completed = true;
            r.error.clear();
        } else if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
            r.pending.clear();
            r.fail(L"Rebuilding image analysis", hr);
            return false;
        }
    }
    if (!texture || !time_ns || time_ns <= r.last_window_time || r.pending.size() >= 3)
        return completed;
    D3D11_TEXTURE2D_DESC input{};
    texture->GetDesc(&input);
    if (!input.Width || !input.Height || input.Width > 16384 || input.Height > 16384 ||
        input.ArraySize != 1 || input.SampleDesc.Count != 1)
        return r.fail(L"Invalid window image format");
    ComPtr<ID3D11Device> source_device;
    texture->GetDevice(&source_device);
    if (source_device.Get() != r.device.Get())
        return r.fail(L"Window image is on the wrong GPU");
    std::unique_ptr<Impl::Pending> p;
    if (r.free_pending.empty())
        p = std::make_unique<Impl::Pending>();
    else {
        p = std::move(r.free_pending.back());
        r.free_pending.pop_back();
    }
    D3D11_TEXTURE2D_DESC old{};
    if (p->full)
        p->full->GetDesc(&old);
    const auto storage_format = input.Format == DXGI_FORMAT_B8G8R8A8_UNORM   ? DXGI_FORMAT_B8G8R8A8_TYPELESS
                                : input.Format == DXGI_FORMAT_R8G8B8A8_UNORM ? DXGI_FORMAT_R8G8B8A8_TYPELESS
                                                                             : input.Format;
    if (!p->full || old.Width != input.Width || old.Height != input.Height || old.Format != storage_format) {
        p->full.Reset();
        p->view.Reset();
        auto d = input;
        d.MipLevels = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags = d.MiscFlags = 0;
        d.Format = storage_format;
        HRESULT hr = r.device->CreateTexture2D(&d, nullptr, &p->full);
        if (FAILED(hr))
            return r.fail(L"Window intermediate image", hr);
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        view.Format = input.Format == DXGI_FORMAT_B8G8R8A8_UNORM   ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                      : input.Format == DXGI_FORMAT_R8G8B8A8_UNORM ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                                                   : input.Format;
        hr = r.device->CreateShaderResourceView(p->full.Get(), &view, &p->view);
        if (FAILED(hr))
            return r.fail(L"Window color space", hr);
    }
    const double scale = std::min({1., 480. / input.Width, 270. / input.Height});
    const unsigned aw = std::max(1u, unsigned(input.Width * scale)),
                   ah = std::max(1u, unsigned(input.Height * scale));
    if (!p->small || p->analysis_width != aw || p->analysis_height != ah) {
        p->small.Reset();
        p->staging.Reset();
        p->target.Reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = aw;
        d.Height = ah;
        d.MipLevels = d.ArraySize = 1;
        d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.BindFlags = D3D11_BIND_RENDER_TARGET;
        HRESULT hr = r.device->CreateTexture2D(&d, nullptr, &p->small);
        if (FAILED(hr) || FAILED(hr = r.device->CreateRenderTargetView(p->small.Get(), nullptr, &p->target)))
            return r.fail(L"Analysis image", hr);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = r.device->CreateTexture2D(&d, nullptr, &p->staging);
        if (FAILED(hr))
            return r.fail(L"Analysis buffer", hr);
    }
    p->width = input.Width;
    p->height = input.Height;
    p->analysis_width = aw;
    p->analysis_height = ah;
    p->time_ns = time_ns;
    // GPU copies/downsampling are queued; there is no synchronous full-frame readback.
    r.context->CopySubresourceRegion(p->full.Get(), 0, 0, 0, 0, texture, 0, nullptr);
    auto *rt = p->target.Get();
    r.context->OMSetRenderTargets(1, &rt, nullptr);
    D3D11_VIEWPORT vp{0, 0, float(aw), float(ah), 0, 1};
    r.context->RSSetViewports(1, &vp);
    r.context->IASetInputLayout(nullptr);
    r.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    r.context->VSSetShader(r.vs.Get(), nullptr, 0);
    r.context->PSSetShader(r.downsample.Get(), nullptr, 0);
    auto *sampler = r.sampler.Get();
    r.context->PSSetSamplers(0, 1, &sampler);
    auto *srv = p->view.Get();
    r.context->PSSetShaderResources(0, 1, &srv);
    r.context->Draw(3, 0);
    ID3D11ShaderResourceView *empty{};
    r.context->PSSetShaderResources(0, 1, &empty);
    r.context->OMSetRenderTargets(0, nullptr, nullptr);
    r.context->CopyResource(p->staging.Get(), p->small.Get());
    r.context->Flush();
    r.last_window_time = time_ns;
    r.pending.push_back(std::move(p));
    return completed;
}
void Renderer::source_changed() {
    auto &r = *impl_;
    r.frame = {};
    r.opened = {};
    r.pending.clear();
    r.free_pending.clear();
    r.stabilizer.reset();
    r.image_stabilizer.reset();
    r.image_result = {};
    r.window_mode = false;
    r.last_window_time = 0;
    for (unsigned i = 0; i < 2; ++i) {
        r.local[i].Reset();
        r.srv[i].Reset();
        r.shared[i].Reset();
        r.keyed[i].Reset();
    }
}
bool Renderer::attach(HWND w) {
    impl_->window = w;
    return impl_->initialize();
}
void Renderer::reset() {
    impl_->stabilizer.reset();
    impl_->image_stabilizer.reset();
    impl_->image_result = {};
    impl_->pending.clear();
}
std::wstring Renderer::error() const {
    return impl_->error;
}
bool Renderer::has_frame() const {
    return impl_->frame.view_count > 0;
}
std::uint64_t Renderer::sequence() const {
    return impl_->frame.sequence;
}
unsigned Renderer::views() const {
    return impl_->frame.view_count;
}

bool Renderer::consume(Channel &channel) {
    auto &r = *impl_;
    if (r.window_mode)
        source_changed();
    if (!channel.lock())
        return false;
    struct Unlock {
        Channel &c;
        ~Unlock() {
            c.unlock();
        }
    } unlock{channel};
    auto &s = *channel.data();
    if (s.ready != 1)
        return false;
    auto recover = [&](const wchar_t *where, HRESULT hr = E_FAIL) {
        s.ready = 2;
        r.frame = {};
        r.opened = {};
        r.stabilizer.reset();
        for (unsigned i = 0; i < 2; ++i) {
            r.shared[i].Reset();
            r.keyed[i].Reset();
            r.local[i].Reset();
            r.srv[i].Reset();
        }
        InvalidateRect(r.window, nullptr, FALSE);
        return r.fail(where, hr);
    };
    const Frame packet = s.frame;
    if (!valid_frame(packet))
        return recover(L"Invalid image metadata");
    if (!r.device || !same(r.adapter, packet.adapter)) {
        if (!r.initialize(&packet.adapter)) {
            s.ready = 2;
            return false;
        }
    }
    bool changed = packet.generation != r.opened.generation || packet.producer_pid != r.opened.producer_pid ||
                   packet.view_count != r.opened.view_count || packet.session != r.opened.session;
    for (unsigned i = 0; i < packet.view_count; ++i)
        changed = changed || packet.views[i].texture_handle != r.opened.views[i].texture_handle;
    if (changed) {
        // Every retained texture has matching retained metadata. If replacement
        // opening or acquisition fails, never draw its predecessor's pose.
        r.frame = {};
        r.opened = {};
        r.stabilizer.reset();
        for (unsigned i = 0; i < 2; ++i) {
            r.shared[i].Reset();
            r.keyed[i].Reset();
            r.local[i].Reset();
            r.srv[i].Reset();
        }
        for (unsigned i = 0; i < packet.view_count; ++i) {
            HRESULT hr = r.device->OpenSharedResource(
                reinterpret_cast<HANDLE>(packet.views[i].texture_handle), IID_PPV_ARGS(&r.shared[i]));
            if (FAILED(hr))
                return recover(L"Open game image", hr);
            D3D11_TEXTURE2D_DESC d{};
            r.shared[i]->GetDesc(&d);
            if (d.Width != packet.views[i].width || d.Height != packet.views[i].height ||
                d.Format != packet.views[i].format || d.ArraySize != 1 || d.MipLevels != 1 ||
                d.SampleDesc.Count != 1)
                return recover(L"Image format does not match metadata");
            hr = r.shared[i].As(&r.keyed[i]);
            d.Width = packet.views[i].rect_width;
            d.Height = packet.views[i].rect_height;
            if (FAILED(hr) || !r.make_local(i, d))
                return recover(L"GPU image connection", hr);
        }
        r.opened = packet;
    }
    // A runtime may resize its submitted subrect without replacing the shared
    // texture. Resize only the private eye image; do not reset pose history.
    for (unsigned i = 0; i < packet.view_count; ++i) {
        D3D11_TEXTURE2D_DESC d{};
        r.local[i]->GetDesc(&d);
        if (d.Width != unsigned(packet.views[i].rect_width) ||
            d.Height != unsigned(packet.views[i].rect_height)) {
            r.frame = {}; // The retained pixels are being replaced, even if acquisition must retry.
            d.Width = packet.views[i].rect_width;
            d.Height = packet.views[i].rect_height;
            if (!r.make_local(i, d))
                return recover(L"Resize private eye image");
        }
    }
    unsigned acquired = 0;
    HRESULT acquisition = S_OK;
    for (; acquired < packet.view_count; ++acquired) {
        acquisition = r.keyed[acquired]->AcquireSync(1, 0);
        if (acquisition != S_OK)
            break;
    }
    if (acquired != packet.view_count) {
        // Keep the published frame pending; acquired views return to reader key.
        for (unsigned i = 0; i < acquired; ++i) {
            const auto hr = r.keyed[i]->ReleaseSync(1);
            if (FAILED(hr))
                acquisition = hr;
        }
        if (acquisition != WAIT_TIMEOUT)
            return recover(L"Rebuilding GPU connection", acquisition);
        return false;
    }
    for (unsigned i = 0; i < packet.view_count; ++i) {
        const auto &v = packet.views[i];
        const D3D11_BOX box{UINT(v.x), UINT(v.y), 0, UINT(v.x + v.rect_width), UINT(v.y + v.rect_height), 1};
        r.context->CopySubresourceRegion(r.local[i].Get(), 0, 0, 0, 0, r.shared[i].Get(), 0, &box);
    }
    r.context->Flush();
    HRESULT release = S_OK;
    for (unsigned i = 0; i < packet.view_count; ++i) {
        auto hr = r.keyed[i]->ReleaseSync(0);
        if (FAILED(hr))
            release = hr;
    }
    if (FAILED(release))
        return recover(L"Release GPU image", release);
    s.ready = 0;
    r.frame = packet;
    for (unsigned i = 0; i < packet.view_count; ++i) {
        auto &v = r.frame.views[i];
        v.x = v.y = 0;
        v.width = v.rect_width;
        v.height = v.rect_height;
    }
    r.error.clear();
    return true;
}

bool Renderer::demo(double seconds, bool quad) {
    auto &r = *impl_;
    if (!r.device)
        return false;
    constexpr unsigned size = 512;
    if (!r.frame.view_count || r.frame.producer_pid != GetCurrentProcessId() ||
        r.frame.view_count != (quad ? 2u : 1u)) {
        std::vector<std::uint32_t> pixels(size * size);
        for (unsigned i = 0; i < (quad ? 2u : 1u); ++i) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = d.Height = size;
            d.MipLevels = d.ArraySize = 1;
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.SampleDesc.Count = 1;
            if (!r.make_local(i, d))
                return false;
            for (unsigned y = 0; y < size; ++y)
                for (unsigned x = 0; x < size; ++x) {
                    if (i)
                        pixels[y * size + x] = 0xff20d020u;
                    else
                        pixels[y * size + x] = ((x / 32 + y / 32) % 2) ? 0xff803020u : 0xffc06040u;
                }
            r.context->UpdateSubresource(r.local[i].Get(), 0, nullptr, pixels.data(), size * 4, 0);
        }
    }
    Frame f{};
    f.adapter = r.adapter;
    f.generation = f.session = f.reference_epoch = 1;
    f.sequence = ++r.demo_sequence;
    f.display_time = 1000000000LL + static_cast<std::int64_t>(seconds * 1e9);
    f.producer_pid = GetCurrentProcessId();
    f.view_count = quad ? 2 : 1;
    auto q = axis_angle({0, 1, 0}, std::sin(seconds * 4) * .08);
    for (unsigned i = 0; i < f.view_count; ++i) {
        auto &v = f.views[i];
        v.width = v.height = v.rect_width = v.rect_height = size;
        v.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        v.texture_handle = 1;
        v.orientation[1] = float(q.y);
        v.orientation[3] = float(q.w);
        if (i) {
            v.fov[0] = v.fov[3] = -.3f;
            v.fov[1] = v.fov[2] = .3f;
        }
    }
    r.frame = f;
    return true;
}

bool Renderer::draw(const Options &options, bool present, const wchar_t *waiting_text) {
    auto &r = *impl_;
    if (!r.device || !r.swap || !r.resize() || !r.resize_output(options))
        return false;
    const float clear[4] = {waiting_text ? 0.f : .005f, waiting_text ? 0.f : .009f,
                            waiting_text ? 0.f : .016f, 1};
    r.context->ClearRenderTargetView(r.output_target.Get(), clear);
    if (waiting_text) {
        if (!r.make_waiting(waiting_text))
            return false;
        // Use the same GPU output path as live frames. GDI painting over a
        // flip-model swapchain cannot reliably replace a previously presented frame.
        const float scale = std::min(float(r.width) / r.output_width, float(r.height) / r.output_height);
        const float text_scale = std::min(1.f / std::max(.01f, scale), float(r.output_width) / 512);
        D3D11_VIEWPORT vp{(r.output_width - 512 * text_scale) / 2,
                          (r.output_height - 64 * text_scale) / 2,
                          512 * text_scale,
                          64 * text_scale,
                          0,
                          1};
        r.context->RSSetViewports(1, &vp);
        auto *target = r.output_target.Get();
        r.context->OMSetRenderTargets(1, &target, nullptr);
        r.context->IASetInputLayout(nullptr);
        r.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        r.context->VSSetShader(r.vs.Get(), nullptr, 0);
        r.context->PSSetShader(r.downsample.Get(), nullptr, 0);
        auto *sampler = r.sampler.Get();
        r.context->PSSetSamplers(0, 1, &sampler);
        auto *view = r.waiting_view.Get();
        r.context->PSSetShaderResources(0, 1, &view);
        r.context->Draw(3, 0);
        ID3D11ShaderResourceView *empty{};
        r.context->PSSetShaderResources(0, 1, &empty);
    } else if (r.frame.view_count) {
        Params p{};
        p.image_mode[1] =
            float(std::isfinite(options.mirror_limit) ? std::clamp(options.mirror_limit, 0., 50.) / 100 : 0.);
        if (r.window_mode) {
            Settings settings;
            settings.stabilization_crop = options.crop;
            settings.mirror_edges = options.mirror;
            settings.output_16_9 = false;
            auto result = r.image_result;
            if (!options.stabilize)
                result.correction = {};
            const auto warp =
                make_image_warp(r.frame.views[0].width, r.frame.views[0].height, settings, result);
            std::copy(warp.row_x.begin(), warp.row_x.end(), p.image_row_x);
            std::copy(warp.row_y.begin(), warp.row_y.end(), p.image_row_y);
            const auto region =
                framing_region(double(r.frame.views[0].width) / r.frame.views[0].height, options);
            for (auto row : {p.image_row_x, p.image_row_y}) {
                row[2] += float(row[0] * region.left + row[1] * region.top);
                row[0] *= float(region.right - region.left);
                row[1] *= float(region.bottom - region.top);
            }
            p.image_mode[0] = 1;
            rect(p.rect0, p.clamp0, r.frame.views[0]);
            p.flags[0] = options.mirror ? 1.f : 0.f;
        } else {
            const auto composition = r.stabilizer.update(r.frame, options);
            if (!composition.valid)
                return r.fail(L"Invalid pose or projection");
            put(p.input, composition.warp.input);
            put(p.output, composition.warp.output);
            put(p.correction, composition.warp.correction);
            put(p.focus_correction, composition.focus_correction);
            const auto &focus = r.frame.views[r.frame.view_count - 1];
            std::copy(focus.fov, focus.fov + 4, p.focus);
            rect(p.rect0, p.clamp0, r.frame.views[0]);
            rect(p.rect1, p.clamp1, focus);
            p.flags[0] = options.mirror ? 1.f : 0.f;
            p.flags[1] = r.frame.view_count == 2 ? 1.f : 0.f;
            p.flags[2] = float(focus.rect_width);
            p.flags[3] = float(focus.rect_height);
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr = r.context->Map(r.constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr))
            return r.fail(L"Update shader parameters", hr);
        std::memcpy(mapped.pData, &p, sizeof(p));
        r.context->Unmap(r.constants.Get(), 0);
        D3D11_VIEWPORT vp{};
        const float aspect =
            float(std::isfinite(options.aspect) ? std::clamp(options.aspect, .25, 4.) : 16. / 9.);
        vp.Width = float(r.output_width);
        vp.Height = vp.Width / aspect;
        if (vp.Height > r.output_height) {
            vp.Height = float(r.output_height);
            vp.Width = vp.Height * aspect;
        }
        vp.TopLeftX = (r.output_width - vp.Width) / 2;
        vp.TopLeftY = (r.output_height - vp.Height) / 2;
        vp.MaxDepth = 1;
        r.context->RSSetViewports(1, &vp);
        auto *rt = r.output_target.Get();
        r.context->OMSetRenderTargets(1, &rt, nullptr);
        r.context->IASetInputLayout(nullptr);
        r.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        r.context->VSSetShader(r.vs.Get(), nullptr, 0);
        r.context->PSSetShader(r.ps.Get(), nullptr, 0);
        auto *cb = r.constants.Get();
        r.context->PSSetConstantBuffers(0, 1, &cb);
        auto *sampler = r.sampler.Get();
        r.context->PSSetSamplers(0, 1, &sampler);
        ID3D11ShaderResourceView *views[] = {r.srv[0].Get(), r.srv[r.frame.view_count - 1].Get()};
        r.context->PSSetShaderResources(0, 2, views);
        r.context->Draw(3, 0);
        ID3D11ShaderResourceView *empty[2] = {};
        r.context->PSSetShaderResources(0, 2, empty);
    }
    // Present the complete requested-resolution canvas without distorting it
    // when the physical client area has a different aspect ratio.
    r.context->ClearRenderTargetView(r.target.Get(), clear);
    auto *rt = r.target.Get();
    r.context->OMSetRenderTargets(1, &rt, nullptr);
    const float scale = std::min(float(r.width) / r.output_width, float(r.height) / r.output_height);
    D3D11_VIEWPORT fitted{(r.width - r.output_width * scale) / 2,
                          (r.height - r.output_height * scale) / 2,
                          r.output_width * scale,
                          r.output_height * scale,
                          0,
                          1};
    r.context->RSSetViewports(1, &fitted);
    r.context->IASetInputLayout(nullptr);
    r.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    r.context->VSSetShader(r.vs.Get(), nullptr, 0);
    r.context->PSSetShader(r.downsample.Get(), nullptr, 0);
    auto *sample = r.sampler.Get();
    r.context->PSSetSamplers(0, 1, &sample);
    auto *view = r.output_view.Get();
    r.context->PSSetShaderResources(0, 1, &view);
    r.context->Draw(3, 0);
    ID3D11ShaderResourceView *empty{};
    r.context->PSSetShaderResources(0, 1, &empty);
    if (present) {
        const HRESULT hr = r.swap->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
        if (FAILED(hr) && hr != DXGI_ERROR_WAS_STILL_DRAWING)
            return r.fail(L"Present preview", hr);
    }
    return true;
}

bool Renderer::readback(std::vector<std::uint32_t> &pixels, unsigned &width, unsigned &height) {
    auto &r = *impl_;
    if (!r.output)
        return false;
    D3D11_TEXTURE2D_DESC d{};
    r.output->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = d.MiscFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = r.device->CreateTexture2D(&d, nullptr, &staging);
    if (FAILED(hr))
        return r.fail(L"Test readback", hr);
    r.context->CopyResource(staging.Get(), r.output.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    hr = r.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr))
        return r.fail(L"Test map", hr);
    pixels.resize(std::size_t(d.Width) * d.Height);
    for (unsigned y = 0; y < d.Height; ++y)
        std::memcpy(pixels.data() + std::size_t(y) * d.Width,
                    static_cast<const char *>(m.pData) + y * m.RowPitch, d.Width * 4);
    r.context->Unmap(staging.Get(), 0);
    width = d.Width;
    height = d.Height;
    return true;
}
} // namespace vrst::preview

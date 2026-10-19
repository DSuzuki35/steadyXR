#include "visual_effects.hpp"
#include <DirectXPackedVector.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <d3d11.h>
#include <dxgi.h>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>
#include <wrl/client.h>
using namespace vrst::preview;
using Microsoft::WRL::ComPtr;
using Pixel = std::array<float, 4>;
void require(bool b, const char *text) {
    if (!b)
        throw std::runtime_error(text);
}
void check_near(float a, float b, float eps, const char *text) {
    require(std::isfinite(a) && std::abs(a - b) <= eps, text);
}
struct GPU {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    GPU() {
        D3D_FEATURE_LEVEL fl{};
        require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                            D3D11_SDK_VERSION, &device, &fl, &context)) &&
                    fl >= D3D_FEATURE_LEVEL_11_0,
                "real FL11 hardware device");
    }
    ComPtr<ID3D11Texture2D> texture(unsigned w, unsigned h, Pixel p,
                                    DXGI_FORMAT format = DXGI_FORMAT_R16G16B16A16_FLOAT) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = d.ArraySize = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        unsigned bytes = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
        std::vector<unsigned char> data(w * h * bytes);
        for (unsigned i = 0; i < w * h; i++)
            for (unsigned c = 0; c < 4; c++) {
                if (bytes == 8) {
                    auto v = DirectX::PackedVector::XMConvertFloatToHalf(p[c]);
                    std::memcpy(data.data() + i * 8 + c * 2, &v, 2);
                } else
                    data[i * 4 + c] = static_cast<unsigned char>(std::round(p[c] * 255));
            }
        D3D11_SUBRESOURCE_DATA init{data.data(), w * bytes, 0};
        ComPtr<ID3D11Texture2D> t;
        require(SUCCEEDED(device->CreateTexture2D(&d, &init, &t)), "fixture texture");
        return t;
    }
    std::vector<Pixel> read(ID3D11Texture2D *t) {
        D3D11_TEXTURE2D_DESC d{};
        t->GetDesc(&d);
        require(d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT, "enabled output must be linear RGBA16F");
        unsigned w = d.Width, h = d.Height;
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = d.MiscFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        require(SUCCEEDED(device->CreateTexture2D(&d, nullptr, &staging)), "staging");
        context->CopyResource(staging.Get(), t);
        D3D11_MAPPED_SUBRESOURCE m{};
        require(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)), "test readback");
        std::vector<Pixel> out(w * h);
        for (unsigned y = 0; y < h; y++)
            for (unsigned x = 0; x < w; x++)
                for (unsigned c = 0; c < 4; c++) {
                    unsigned short v;
                    std::memcpy(&v, static_cast<char *>(m.pData) + y * m.RowPitch + x * 8 + c * 2, 2);
                    out[y * w + x][c] = DirectX::PackedVector::XMConvertHalfToFloat(v);
                }
        context->Unmap(staging.Get(), 0);
        return out;
    }
    Pixel run(VisualEffects &e, ID3D11Texture2D *t, VisualSettings s, uint64_t frame = 1, double time = 0) {
        return read(e.process(device.Get(), context.Get(), t, s, frame, time))[0];
    }
};
VisualSettings neutral_linear() {
    VisualSettings s;
    s.color = true;
    s.input_transfer = VisualTransfer::linear;
    return s;
}
int main() {
    GPU g;
    int failures = 0;
    auto test = [&](const char *name, std::function<void()> f) {
        try {
            f();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &e) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
        }
    };
    test("disabled and bypass allocate and dispatch nothing", [&] {
        VisualEffects e;
        auto t = g.texture(16, 16, {.2f, .3f, .4f, .5f});
        VisualSettings s;
        for (int i = 0; i < 3; i++) {
            require(e.process(g.device.Get(), g.context.Get(), t.Get(), s, i, i) == t.Get(),
                    "texture identity");
        }
        s.color = true;
        s.bypass = true;
        require(e.process(g.device.Get(), g.context.Get(), t.Get(), s, 4, 4) == t.Get(), "bypass identity");
        require(e.diagnostics().allocations == 0 && e.diagnostics().dispatches == 0,
                "zero disabled GPU work");
    });
    test("neutral linear alpha and resize", [&] {
        VisualEffects e;
        auto s = neutral_linear();
        for (auto size : {17u, 35u}) {
            auto t = g.texture(size, 19, {.125f, .25f, .5f, .375f});
            auto p = g.run(e, t.Get(), s);
            check_near(p[0], .125f, .001f, "neutral red");
            check_near(p[2], .5f, .001f, "neutral blue");
            check_near(p[3], .375f, .001f, "alpha preserved");
        }
        require(e.diagnostics().output_transfer == VisualTransfer::linear, "linear contract");
    });
    test("manual exposure is linear and bounded controls finite", [&] {
        VisualEffects e;
        auto t = g.texture(8, 8, {.125f, .2f, .3f, .5f});
        auto s = neutral_linear();
        s.color = false;
        s.exposure = true;
        s.exposure_ev = 1;
        auto p = g.run(e, t.Get(), s);
        check_near(p[0], .25f, .001f, "one stop doubles linear light");
        s.exposure_ev = std::numeric_limits<float>::quiet_NaN();
        p = g.run(e, t.Get(), s, 2, 1);
        check_near(p[0], .125f, .001f, "nonfinite neutral");
        s.exposure_ev = 100;
        p = g.run(e, t.Get(), s, 3, 2);
        check_near(p[0], 1, .001f, "bounded SDR highlights");
    });
    test("exact SDR transfer RGBA BGRA SRGB and typeless", [&] {
        for (auto f : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                       DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM}) {
            VisualEffects e;
            auto t = g.texture(8, 8, {128.f / 255, 128.f / 255, 128.f / 255, 1}, f);
            VisualSettings s;
            s.color = true;
            auto p = g.run(e, t.Get(), s);
            check_near(p[0], .21586f, .001f, "sRGB decoded exactly once");
        }
        VisualEffects e;
        auto t = g.texture(8, 8, {.04f, .04f, .04f, 1}, DXGI_FORMAT_R8G8B8A8_UNORM);
        VisualSettings s;
        s.color = true;
        check_near(g.run(e, t.Get(), s)[0], .00303527f, .00003f, "low piecewise transfer");
    });
    test("premultiplied encoded input unpremultiplies before decode", [&] {
        for (auto f : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB}) {
            VisualEffects e;
            auto t = g.texture(8, 8, {64.f / 255, 64.f / 255, 64.f / 255, 128.f / 255}, f);
            VisualSettings s;
            s.color = true;
            s.premultiplied_alpha = true;
            auto p = g.run(e, t.Get(), s);
            check_near(p[0], .10744f, .001f, "linear premultiplied output");
            check_near(p[3], 128.f / 255, .001f, "alpha");
        }
        VisualEffects e;
        auto t = g.texture(8, 8, {.5f, .5f, .5f, 0});
        auto s = neutral_linear();
        s.premultiplied_alpha = true;
        check_near(g.run(e, t.Get(), s)[0], 0, 0, "transparent finite black");
    });
    test("unsupported formats return input and diagnostics", [&] {
        VisualEffects e;
        auto t = g.texture(8, 8, {0, 0, 0, 0}, DXGI_FORMAT_R10G10B10A2_UNORM);
        auto s = neutral_linear();
        require(e.process(g.device.Get(), g.context.Get(), t.Get(), s, 1, 0) == t.Get(),
                "unsupported passthrough");
        require(e.diagnostics().outcome == VisualOutcome::unsupported_input &&
                    e.diagnostics().dispatches == 0,
                "reported fallback");
    });
    test("temporal adaptation bounds rates and repeat/reset", [&] {
        VisualEffects e;
        auto dark = g.texture(32, 32, {.045f, .045f, .045f, .7f});
        auto s = neutral_linear();
        s.color = false;
        s.adaptation = true;
        s.brighten_rate = 1;
        s.darken_rate = 4;
        s.min_gain = .5;
        s.max_gain = 2;
        auto first = g.run(e, dark.Get(), s, 1, 0);
        check_near(first[0], .045f, .001f, "reset starts unity");
        auto next = g.run(e, dark.Get(), s, 2, .25);
        check_near(next[0], .05495f, .001f, "bounded gain exponential brightening");
        check_near(g.run(e, dark.Get(), s, 2, .25)[0], next[0], .0001f, "same frame does not adapt twice");
        for (int i = 3; i < 30; i++)
            g.run(e, dark.Get(), s, i, (i - 1) * .25);
        check_near(g.run(e, dark.Get(), s, 30, 7.25)[0], .09f, .001f, "gain upper bound");
        auto bright = g.texture(32, 32, {.8f, .8f, .8f, 1});
        auto p = g.run(e, bright.Get(), s, 31, 7.5);
        check_near(p[0], .8414f, .004f, "separate fast darkening");
        check_near(g.run(e, dark.Get(), s, 1, 0)[0], .045f, .001f, "frame/time reset clears exposure");
        require(e.diagnostics().metering_samples <= 4096, "small GPU meter");
    });
    test("metering crop excludes black borders and tiny bright HUD", [&] {
        VisualEffects e;
        auto s = neutral_linear();
        s.color = false;
        s.adaptation = true;
        s.brighten_rate = 20;
        s.min_gain = .0625f;
        s.max_gain = 16;
        s.metering_region = {.25f, .25f, .75f, .75f};
        s.content_region = s.metering_region;
        auto t = g.texture(64, 64, {0, 0, 0, 1});
        auto center = g.texture(32, 32, {.18f, .18f, .18f, 1});
        D3D11_BOX box{0, 0, 0, 32, 32, 1};
        g.context->CopySubresourceRegion(t.Get(), 0, 16, 16, 0, center.Get(), 0, &box);
        g.run(e, t.Get(), s, 1, 0);
        auto p = g.read(e.process(g.device.Get(), g.context.Get(), t.Get(), s, 2, .25));
        check_near(p[32 * 64 + 32][0], .18f, .002f, "crop meters real content");
        auto hud = g.texture(2, 2, {1, 1, 1, 1});
        D3D11_BOX b{0, 0, 0, 2, 2, 1};
        g.context->CopySubresourceRegion(t.Get(), 0, 30, 30, 0, hud.Get(), 0, &b);
        p = g.read(e.process(g.device.Get(), g.context.Get(), t.Get(), s, 3, .5));
        check_near(p[32 * 64 + 32][0], .18f, .003f, "tiny HUD cannot dominate log meter");
    });
    test("transition impulse recovers independently of base adaptation", [&] {
        VisualEffects e;
        auto bright = g.texture(16, 16, {.8f, .8f, .8f, 1});
        auto dark = g.texture(16, 16, {.09f, .09f, .09f, 1});
        auto s = neutral_linear();
        s.color = false;
        s.adaptation = true;
        s.brighten_rate = 1;
        s.transition_strength = .15f;
        s.transition_recovery = 3;
        g.run(e, bright.Get(), s, 1, 0);
        check_near(g.run(e, dark.Get(), s, 2, .25)[0], .12341f, .001f, "restrained transition impulse");
        check_near(g.run(e, dark.Get(), s, 3, .5)[0], .13179f, .001f,
                   "transient must decay without feeding base gain");
        e.reset();
        check_near(g.run(e, dark.Get(), s, 4, .75)[0], .09f, .001f, "explicit reset starts unity");
    });
    test("disable bypass releases optional resources", [&] {
        VisualEffects e;
        auto t = g.texture(16, 16, {.1f, .1f, .1f, 1});
        auto s = neutral_linear();
        s.bloom = true;
        s.adaptation = true;
        g.run(e, t.Get(), s);
        auto memory = e.diagnostics().resource_bytes;
        s = {};
        require(e.process(g.device.Get(), g.context.Get(), t.Get(), s, 2, 1) == t.Get(),
                "disable texture identity");
        require(e.diagnostics().resource_bytes < memory, "disabled meter and bloom resources released");
    });
    test("color saturation contrast temperature tint shadows highlights", [&] {
        VisualEffects e;
        auto t = g.texture(8, 8, {.2f, .4f, .6f, .5f});
        auto s = neutral_linear();
        s.saturation = 0;
        auto p = g.run(e, t.Get(), s);
        check_near(p[0], .37192f, .001f, "Rec709 desaturation");
        check_near(p[1], p[0], .001f, "gray");
        s = neutral_linear();
        s.temperature = 1;
        s.tint = 1;
        p = g.run(e, t.Get(), s, 2, 1);
        require(p[0] > .2f && p[1] < .4f && p[2] < .6f, "white balance axes");
        s = neutral_linear();
        s.contrast = 0;
        p = g.run(e, t.Get(), s, 3, 2);
        check_near(p[0], .18f, .001f, "contrast pivot");
        s = neutral_linear();
        s.shadows = .5f;
        s.highlights = -.5f;
        p = g.run(e, t.Get(), s, 4, 3);
        require(p[0] > .2f && p[2] < .6f, "shadow highlight shaping");
    });
    test("bloom threshold radius alpha and disabled pass release", [&] {
        VisualEffects e;
        auto t = g.texture(64, 64, {0, 0, 0, .4f});
        auto patch = g.texture(8, 8, {1, 1, 1, .4f});
        D3D11_BOX b{0, 0, 0, 8, 8, 1};
        g.context->CopySubresourceRegion(t.Get(), 0, 28, 28, 0, patch.Get(), 0, &b);
        auto s = neutral_linear();
        s.color = false;
        s.bloom = true;
        s.bloom_threshold = .5f;
        s.bloom_knee = 0;
        s.bloom_strength = 1;
        s.bloom_radius = 16;
        auto p = g.read(e.process(g.device.Get(), g.context.Get(), t.Get(), s, 1, 0));
        require(p[32 * 64 + 24][0] > .001f, "bloom spreads outside source");
        check_near(p[32 * 64 + 24][3], .4f, .001f, "bloom preserves alpha");
        auto memory = e.diagnostics().resource_bytes;
        require(e.diagnostics().frame_dispatches == 4, "extract two reduced blur and combine");
        s.bloom_threshold = 2;
        p = g.read(e.process(g.device.Get(), g.context.Get(), t.Get(), s, 2, 1));
        check_near(p[32 * 64 + 24][0], 0, .0001f, "below threshold no bloom");
        s.bloom = false;
        s.color = true;
        g.run(e, t.Get(), s, 3, 2);
        require(e.diagnostics().frame_dispatches == 1 && e.diagnostics().resource_bytes < memory,
                "disabled bloom frees resources and skips passes");
    });
    test("surface view failure returns input and recovers on retry", [&] {
        for (int stage : {1,2}) {
            VisualEffects e;
            auto t=g.texture(16,16,{.2f,.3f,.4f,1});
            auto s=neutral_linear();
            vrst::preview::fail_next_surface_view(stage);
            require(e.process(g.device.Get(),g.context.Get(),t.Get(),s,1,0)==t.Get(),"failure returns original");
            require(e.diagnostics().outcome==VisualOutcome::resource_failure && !e.diagnostics().frame_dispatches,
                    "failed surface never dispatched");
            const auto p=g.run(e,t.Get(),s,2,.016);
            check_near(p[0],.2f,.001f,"retry creates complete writable output");
        }
    });
    test("caller compute bindings and graphics state preserved", [&] {
        VisualEffects e;
        auto t = g.texture(16, 16, {.2f, .2f, .2f, 1});
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> cb;
        g.device->CreateBuffer(&bd, nullptr, &cb);
        auto raw = cb.Get();
        g.context->CSSetConstantBuffers(0, 1, &raw);
        D3D11_VIEWPORT vp{3, 4, 11, 12, 0, 1};
        g.context->RSSetViewports(1, &vp);
        g.run(e, t.Get(), neutral_linear());
        ComPtr<ID3D11Buffer> restored;
        g.context->CSGetConstantBuffers(0, 1, &restored);
        require(restored.Get() == raw, "compute constant buffer restored");
        UINT n = 1;
        D3D11_VIEWPORT actual{};
        g.context->RSGetViewports(&n, &actual);
        require(actual.TopLeftX == 3 && actual.Height == 12, "graphics untouched");
    });
    test("4K timestamp benchmark", [&] {
        ComPtr<IDXGIDevice> dx;
        ComPtr<IDXGIAdapter> a;
        DXGI_ADAPTER_DESC ad{};
        g.device.As(&dx);
        dx->GetAdapter(&a);
        a->GetDesc(&ad);
        std::wcout << L"GPU " << ad.Description << L"\n";
        auto t = g.texture(3840, 2160, {.1f, .2f, .3f, 1});
        for (int mode = 0; mode < 5; mode++) {
            VisualEffects e;
            e.set_timing_enabled(true);
            auto s = neutral_linear();
            s.color = mode == 1 || mode == 4;
            s.exposure = mode == 2 || mode == 4;
            s.exposure_ev = 1;
            s.bloom = mode == 3 || mode == 4;
            s.adaptation = mode == 4;
            if (mode == 0)
                s = {};
            std::vector<double> times;
            for (int i = 0; i < 70; i++) {
                e.process(g.device.Get(), g.context.Get(), t.Get(), s, i, i / 60.);
                if (e.diagnostics().gpu_timing_valid && i > 10)
                    times.push_back(e.diagnostics().gpu_ms);
                g.context->Flush();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            double sum = 0;
            for (auto v : times)
                sum += v;
            std::cout << "BENCH mode=" << mode << " gpu_ms=" << (times.empty() ? 0 : sum / times.size())
                      << " samples=" << times.size() << " dispatches=" << e.diagnostics().frame_dispatches
                      << " bytes=" << e.diagnostics().resource_bytes << '\n';
            if (mode == 0)
                require(e.diagnostics().allocations == 0, "baseline zero allocation");
            else
                require(e.diagnostics().outcome == VisualOutcome::processed,
                        "benchmark effects actually processed");
        }
    });
    std::cout << "Failures: " << failures << '\n';
    return failures ? 1 : 0;
}

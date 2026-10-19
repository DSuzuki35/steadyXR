#include "visual_effects.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
namespace vrst::preview {
namespace {
const char shader_source[] = R"(
cbuffer Params : register(b0) {
 uint2 size; uint2 smallSize;
 float4 region;
 float4 exposure; // manual gain, min, max, target
 float4 temporal; // dt, brighten, darken, reset
 float4 bloom; // threshold, knee, strength, radius at low resolution
 float4 white; // temperature, tint, contrast, saturation
 float4 shaping; // shadows, highlights, transition strength, recovery
 uint4 flags; // decode sRGB, hardware sRGB, premultiplied, adaptation
 uint4 switches; // bloom, color, blur vertical, unused
};
Texture2D<float4> image : register(t0);
Texture2D<float4> glow : register(t1);
Texture2D<float4> previous : register(t2);
RWTexture2D<float4> output : register(u0);
float3 decode(float3 x) { return float3(x.r<=.04045?x.r/12.92:pow((x.r+.055)/1.055,2.4),x.g<=.04045?x.g/12.92:pow((x.g+.055)/1.055,2.4),x.b<=.04045?x.b/12.92:pow((x.b+.055)/1.055,2.4)); }
float3 encode(float3 x) { return float3(x.r<=.0031308?x.r*12.92:1.055*pow(x.r,1./2.4)-.055,x.g<=.0031308?x.g*12.92:1.055*pow(x.g,1./2.4)-.055,x.b<=.0031308?x.b*12.92:1.055*pow(x.b,1./2.4)-.055); }
float4 source(int2 p) {
 float4 c=image.Load(int3(clamp(p,int2(0,0),int2(size)-1),0));
 c = float4(isfinite(c.r)?max(0,c.r):0,isfinite(c.g)?max(0,c.g):0,isfinite(c.b)?max(0,c.b):0,isfinite(c.a)?saturate(c.a):0);
 if(flags.z) {
  if(flags.y) c.rgb=encode(c.rgb);
  c.rgb=c.a>1e-6?c.rgb/c.a:0;
  if(flags.x) c.rgb=decode(c.rgb);
 } else if(flags.x && !flags.y) c.rgb=decode(c.rgb);
 return c;
}
float luminance(float3 c) { return dot(c,float3(.2126,.7152,.0722)); }
float gain() { return exposure.x*(flags.w?previous.Load(int3(0,0,0)).x:1); }
groupshared float logs[256];
[numthreads(256,1,1)] void meter(uint i:SV_GroupIndex) {
 float sum=0;
 [unroll] for(uint n=0;n<16;n++) {
  uint sampleIndex=i+n*256;float2 uv=(float2(sampleIndex%64,sampleIndex/64)+.5)/64;
  int2 p=int2(lerp(region.xy,region.zw,uv)*size);
  float l=luminance(source(p).rgb);
  sum+=log(clamp(l,.0001,.6));
 }
 logs[i]=sum;GroupMemoryBarrierWithGroupSync();
 for(uint step=128;step>0;step/=2) { if(i<step)logs[i]+=logs[i+step];GroupMemoryBarrierWithGroupSync(); }
 if(i==0) {
  float target=clamp(exposure.w/exp(logs[0]/4096),exposure.y,exposure.z);
  float4 old=previous.Load(int3(0,0,0));
  float base=temporal.w?1:old.w;
  float rate=target>base?temporal.y:temporal.z;
  float g=lerp(base,target,1-exp(-temporal.x*rate));
  // Optional small transient undershoot/overshoot, decaying independently.
  float impulse=temporal.w?0:old.y*exp(-temporal.x*shaping.w);
  if(!temporal.w) impulse+=clamp(target-old.z,-1,1)*shaping.z;
  output[uint2(0,0)]=float4(clamp(g+impulse,exposure.y,exposure.z),impulse,target,g);
 }
}
[numthreads(8,8,1)] void extract(uint3 id:SV_DispatchThreadID) {
 if(any(id.xy>=smallSize))return;
 float3 c=0; [unroll]for(int y=0;y<4;y++)[unroll]for(int x=0;x<4;x++)c+=source(int2(id.xy)*4+int2(x,y)).rgb;
 c*=gain()/16;
 float l=max(c.r,max(c.g,c.b));float k=max(bloom.y,1e-5);
 float soft=clamp(l-bloom.x+k,0,2*k);soft=soft*soft/(4*k);
 float weight=max(l-bloom.x,soft)/max(l,1e-5);
 output[id.xy]=float4(min(c*weight,4),0);
}
[numthreads(8,8,1)] void blur(uint3 id:SV_DispatchThreadID) {
 if(any(id.xy>=smallSize))return;
 float3 c=0;float total=0;
 [unroll]for(int n=-4;n<=4;n++) {float w=exp(-float(n*n)/8);int2 offset=switches.z?int2(0,round(n*bloom.w/4)):int2(round(n*bloom.w/4),0);int2 p=clamp(int2(id.xy)+offset,int2(0,0),int2(smallSize)-1);c+=image.Load(int3(p,0)).rgb*w;total+=w;}
 output[id.xy]=float4(c/total,0);
}
float3 sampleGlow(float2 p) {
 int2 a=int2(floor(p));float2 f=frac(p);int2 hi=int2(smallSize)-1;
 return lerp(lerp(glow.Load(int3(clamp(a,0,hi),0)).rgb,glow.Load(int3(clamp(a+int2(1,0),0,hi),0)).rgb,f.x),lerp(glow.Load(int3(clamp(a+int2(0,1),0,hi),0)).rgb,glow.Load(int3(clamp(a+1,0,hi),0)).rgb,f.x),f.y);
}
[numthreads(8,8,1)] void finish(uint3 id:SV_DispatchThreadID) {
 if(any(id.xy>=size))return;
 float4 c=source(id.xy);c.rgb*=gain();
 if(switches.x)c.rgb+=sampleGlow((float2(id.xy)+.5)/4-.5)*bloom.z;
 if(switches.y) {
  c.rgb*=float3(1+.2*white.x,1-.15*white.y,1-.2*white.x);
  c.rgb=(c.rgb-.18)*white.z+.18;
  c.rgb=lerp(luminance(c.rgb).xxx,c.rgb,white.w);
  c.rgb+=shaping.x*.25*pow(saturate(1-c.rgb),2)*(1-saturate(c.rgb)*2);
  c.rgb+=shaping.y*.25*pow(saturate(c.rgb),2);
 }
 c.rgb=saturate(c.rgb);if(flags.z)c.rgb*=c.a;
 output[id.xy]=c;
}
)";
struct Surface {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    void clear() {
        uav.Reset();
        srv.Reset();
        texture.Reset();
    }
};
struct Constants {
    unsigned size[2], reduced[2];
    float region[4], exposure[4], temporal[4], bloom[4], white[4], shaping[4];
    unsigned flags[4], switches[4];
};
// Only compute slots 0..2, UAV0 and CB0 are touched. InitialCount=-1 retains
// append/consume counters of any restored caller UAV. No graphics mutation.
struct ComputeState {
    ID3D11DeviceContext *c;
    ComPtr<ID3D11ComputeShader> shader;
    std::array<ComPtr<ID3D11ClassInstance>, 256> classes;
    UINT count = 256;
    std::array<ComPtr<ID3D11ShaderResourceView>, 3> srvs;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11Buffer> cb;
    explicit ComputeState(ID3D11DeviceContext *context) : c(context) {
        ID3D11ClassInstance *raw[256]{};
        c->CSGetShader(&shader, raw, &count);
        for (UINT i = 0; i < count; i++)
            classes[i].Attach(raw[i]);
        for (UINT i = 0; i < 3; i++)
            c->CSGetShaderResources(i, 1, &srvs[i]);
        c->CSGetUnorderedAccessViews(0, 1, &uav);
        c->CSGetConstantBuffers(0, 1, &cb);
    }
    ~ComputeState() {
        ID3D11UnorderedAccessView *nil = nullptr;
        c->CSSetUnorderedAccessViews(0, 1, &nil, nullptr);
        ID3D11ShaderResourceView *r[3]{};
        for (int i = 0; i < 3; i++)
            r[i] = srvs[i].Get();
        c->CSSetShaderResources(0, 3, r);
        auto u = uav.Get();
        c->CSSetUnorderedAccessViews(0, 1, &u, nullptr);
        auto b = cb.Get();
        c->CSSetConstantBuffers(0, 1, &b);
        ID3D11ClassInstance *raw[256]{};
        for (UINT i = 0; i < count; i++)
            raw[i] = classes[i].Get();
        c->CSSetShader(shader.Get(), raw, count);
    }
};
} // namespace
struct VisualEffects::Impl {
    VisualDiagnostics diagnostics;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11ComputeShader> meter, extract, blur, finish;
    ComPtr<ID3D11Buffer> cb;
    Surface output, glowA, glowB, gains[2];
    ComPtr<ID3D11Texture2D> input;
    ComPtr<ID3D11ShaderResourceView> inputView;
    unsigned width = 0, height = 0, gainIndex = 0;
    bool temporalValid = false, timing = false;
    uint64_t lastFrame = 0;
    double lastTime = 0;
    struct Timing {
        ComPtr<ID3D11Query> disjoint, start, end;
        bool pending = false;
    };
    std::array<Timing, 4> queries;
    unsigned queryIndex = 0;
    bool makeShader(const char *entry, ComPtr<ID3D11ComputeShader> &shader) {
        ComPtr<ID3DBlob> blob, error;
        if (FAILED(D3DCompile(shader_source, sizeof(shader_source), nullptr, nullptr, nullptr, entry,
                              "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error))) {
            if (error)
                OutputDebugStringA(static_cast<const char *>(error->GetBufferPointer()));
            return false;
        }
        return SUCCEEDED(
            device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader));
    }
    bool surface(Surface &s, unsigned w, unsigned h) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.ArraySize = d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &s.texture)) ||
            FAILED(device->CreateShaderResourceView(s.texture.Get(), nullptr, &s.srv)) ||
            FAILED(device->CreateUnorderedAccessView(s.texture.Get(), nullptr, &s.uav)))
            return false;
        diagnostics.allocations++;
        return true;
    }
    void memory() {
        diagnostics.resource_bytes = output.texture ? uint64_t(width) * height * 8 : 0;
        if (glowA.texture)
            diagnostics.resource_bytes += uint64_t((width + 3) / 4) * ((height + 3) / 4) * 16;
        if (gains[0].texture)
            diagnostics.resource_bytes += 16;
    }
    bool init(ID3D11Device *d, unsigned w, unsigned h) {
        if (device.Get() != d) {
            auto keep = diagnostics;
            bool enabled = timing;
            *this = Impl{};
            diagnostics = keep;
            timing = enabled;
            device = d;
        }
        if (!finish || !meter || !extract || !blur || !cb) {
            if (!makeShader("finish", finish) || !makeShader("meter", meter) ||
                !makeShader("extract", extract) || !makeShader("blur", blur))
                return false;
            D3D11_BUFFER_DESC b{};
            b.ByteWidth = sizeof(Constants);
            b.Usage = D3D11_USAGE_DEFAULT;
            b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            if (FAILED(device->CreateBuffer(&b, nullptr, &cb)))
                return false;
            diagnostics.allocations++;
        }
        if (width != w || height != h || !output.texture) {
            width = w;
            height = h;
            output.clear();
            glowA.clear();
            glowB.clear();
            temporalValid = false;
            if (!surface(output, w, h))
                return false;
        }
        return true;
    }
    void dispatch(ID3D11DeviceContext *c, ID3D11ComputeShader *shader, const Constants &p,
                  ID3D11ShaderResourceView *image, ID3D11ShaderResourceView *glow,
                  ID3D11ShaderResourceView *gain, Surface &dest, unsigned x, unsigned y) {
        ID3D11ShaderResourceView *nil[3]{};
        ID3D11UnorderedAccessView *no = nullptr;
        c->CSSetShaderResources(0, 3, nil);
        c->CSSetUnorderedAccessViews(0, 1, &no, nullptr);
        c->UpdateSubresource(cb.Get(), 0, nullptr, &p, 0, 0);
        auto b = cb.Get();
        c->CSSetConstantBuffers(0, 1, &b);
        ID3D11ShaderResourceView *r[]{image, glow, gain};
        c->CSSetShaderResources(0, 3, r);
        auto u = dest.uav.Get();
        c->CSSetUnorderedAccessViews(0, 1, &u, nullptr);
        c->CSSetShader(shader, nullptr, 0);
        c->Dispatch(x, y, 1);
        c->CSSetUnorderedAccessViews(0, 1, &no, nullptr);
        c->CSSetShaderResources(0, 3, nil);
        diagnostics.dispatches++;
        diagnostics.frame_dispatches++;
    }
    Timing *beginTiming(ID3D11DeviceContext *c) {
        diagnostics.gpu_timing_valid = false;
        if (!timing)
            return nullptr;
        for (auto &q : queries)
            if (q.pending) {
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
                UINT64 start = 0, end = 0;
                if (c->GetData(q.disjoint.Get(), &d, sizeof(d), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                    c->GetData(q.start.Get(), &start, sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH) ==
                        S_OK &&
                    c->GetData(q.end.Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
                    q.pending = false;
                    if (!d.Disjoint && d.Frequency) {
                        diagnostics.gpu_ms = 1000. * double(end - start) / double(d.Frequency);
                        diagnostics.gpu_timing_valid = true;
                    }
                }
            }
        auto &q = queries[queryIndex++ % queries.size()];
        if (q.pending)
            return nullptr;
        if (!q.disjoint || !q.start || !q.end) {
            q.disjoint.Reset();
            q.start.Reset();
            q.end.Reset();
            D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            if (FAILED(device->CreateQuery(&d, &q.disjoint)))
                return nullptr;
            d.Query = D3D11_QUERY_TIMESTAMP;
            if (FAILED(device->CreateQuery(&d, &q.start)) || FAILED(device->CreateQuery(&d, &q.end)))
                return nullptr;
            diagnostics.allocations += 3;
        }
        c->Begin(q.disjoint.Get());
        c->End(q.start.Get());
        return &q;
    }
};
VisualEffects::VisualEffects() : impl_(std::make_unique<Impl>()) {}
VisualEffects::~VisualEffects() = default;
void VisualEffects::reset() {
    bool timing = impl_->timing;
    impl_ = std::make_unique<Impl>();
    impl_->timing = timing;
}
void VisualEffects::set_timing_enabled(bool value) {
    impl_->timing = value;
}
const VisualDiagnostics &VisualEffects::diagnostics() const {
    return impl_->diagnostics;
}
ID3D11Texture2D *VisualEffects::process(ID3D11Device *device, ID3D11DeviceContext *c, ID3D11Texture2D *input,
                                        const VisualSettings &settings, uint64_t frame, double time) {
    auto &i = *impl_;
    auto &diag = i.diagnostics;
    diag.frame_dispatches = 0;
    diag.metering_samples = 0;
    diag.gpu_timing_valid = false;
    diag.output_transfer = settings.input_transfer;
    diag.outcome = VisualOutcome::bypass;
    if (!settings.active()) {
        i.glowA.clear();
        i.glowB.clear();
        i.gains[0].clear();
        i.gains[1].clear();
        i.temporalValid = false;
        i.memory();
        return input;
    }
    auto s = settings.bounded();
    diag.outcome = VisualOutcome::unsupported_input;
    if (!device || !c || !input || device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0 ||
        c->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || s.input_transfer == VisualTransfer::unsupported)
        return input;
    ComPtr<ID3D11Device> owner, contextOwner;
    input->GetDevice(&owner);
    c->GetDevice(&contextOwner);
    if (owner.Get() != device || contextOwner.Get() != device)
        return input;
    D3D11_TEXTURE2D_DESC d{};
    input->GetDesc(&d);
    if (d.ArraySize != 1 || d.MipLevels != 1 || d.SampleDesc.Count != 1 ||
        !(d.BindFlags & D3D11_BIND_SHADER_RESOURCE))
        return input;
    DXGI_FORMAT view = d.Format;
    bool hw = false;
    switch (d.Format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        view = DXGI_FORMAT_R8G8B8A8_UNORM;
        break;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        view = DXGI_FORMAT_B8G8R8A8_UNORM;
        break;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        break;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        hw = true;
        if (s.input_transfer != VisualTransfer::srgb)
            return input;
        break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        if (s.input_transfer != VisualTransfer::linear)
            return input;
        break;
    default:
        return input;
    }
    diag.outcome = VisualOutcome::resource_failure;
    if (!i.init(device, d.Width, d.Height))
        return input;
    if (i.input.Get() != input || !i.inputView) {
        i.inputView.Reset();
        i.input = input;
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = view;
        v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        v.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateShaderResourceView(input, &v, &i.inputView)))
            return input;
        diag.allocations++;
    }
    if (!s.bloom) {
        i.glowA.clear();
        i.glowB.clear();
    }
    if (!s.adaptation) {
        i.gains[0].clear();
        i.gains[1].clear();
        i.temporalValid = false;
    }
    unsigned sw = (d.Width + 3) / 4, sh = (d.Height + 3) / 4;
    if (s.bloom && (!i.glowA.texture || !i.glowB.texture)) {
        i.glowA.clear();
        i.glowB.clear();
        if (!i.surface(i.glowA, sw, sh) || !i.surface(i.glowB, sw, sh))
            return input;
    }
    if (s.adaptation && (!i.gains[0].texture || !i.gains[1].texture)) {
        i.gains[0].clear();
        i.gains[1].clear();
        if (!i.surface(i.gains[0], 1, 1) || !i.surface(i.gains[1], 1, 1))
            return input;
        i.temporalValid = false;
    }
    Constants p{};
    p.size[0] = d.Width;
    p.size[1] = d.Height;
    p.reduced[0] = sw;
    p.reduced[1] = sh;
    p.region[0] = std::max(s.metering_region.left, s.content_region.left);
    p.region[1] = std::max(s.metering_region.top, s.content_region.top);
    p.region[2] = std::min(s.metering_region.right, s.content_region.right);
    p.region[3] = std::min(s.metering_region.bottom, s.content_region.bottom);
    // Disjoint user/host regions fall back to host content, never black borders.
    if (p.region[2] <= p.region[0] || p.region[3] <= p.region[1]) {
        p.region[0] = s.content_region.left;
        p.region[1] = s.content_region.top;
        p.region[2] = s.content_region.right;
        p.region[3] = s.content_region.bottom;
    }
    p.exposure[0] = s.exposure ? std::exp2(s.exposure_ev) : 1;
    p.exposure[1] = s.min_gain;
    p.exposure[2] = s.max_gain;
    p.exposure[3] = s.target_luminance;
    bool validTime = std::isfinite(time);
    bool repeated = i.temporalValid && frame == i.lastFrame && time == i.lastTime;
    bool restart =
        !i.temporalValid || !validTime || frame < i.lastFrame || time < i.lastTime || time - i.lastTime > 2;
    p.temporal[0] = restart ? 0 : float(std::clamp(time - i.lastTime, 0., .25));
    p.temporal[1] = s.brighten_rate;
    p.temporal[2] = s.darken_rate;
    p.temporal[3] = restart ? 1.f : 0;
    p.bloom[0] = s.bloom_threshold;
    p.bloom[1] = s.bloom_knee;
    p.bloom[2] = s.bloom_strength;
    p.bloom[3] = s.bloom_radius / 4;
    p.white[0] = s.temperature;
    p.white[1] = s.tint;
    p.white[2] = s.contrast;
    p.white[3] = s.saturation;
    p.shaping[0] = s.shadows;
    p.shaping[1] = s.highlights;
    p.shaping[2] = s.transition_strength;
    p.shaping[3] = s.transition_recovery;
    p.flags[0] = s.input_transfer == VisualTransfer::srgb;
    p.flags[1] = hw;
    p.flags[2] = s.premultiplied_alpha;
    p.flags[3] = s.adaptation;
    p.switches[0] = s.bloom;
    p.switches[1] = s.color;
    ComputeState saved(c);
    auto query = i.beginTiming(c);
    if (s.adaptation && !repeated) {
        if (restart) {
            const float one[]{1, 0, 1, 1};
            c->ClearUnorderedAccessViewFloat(i.gains[i.gainIndex].uav.Get(), one);
            diag.temporal_resets++;
        }
        unsigned next = 1 - i.gainIndex;
        i.dispatch(c, i.meter.Get(), p, i.inputView.Get(), nullptr, i.gains[i.gainIndex].srv.Get(),
                   i.gains[next], 1, 1);
        i.gainIndex = next;
        i.temporalValid = validTime;
        i.lastFrame = frame;
        i.lastTime = time;
        diag.metering_samples = 4096;
    }
    auto gain = s.adaptation ? i.gains[i.gainIndex].srv.Get() : nullptr;
    if (s.bloom) {
        i.dispatch(c, i.extract.Get(), p, i.inputView.Get(), nullptr, gain, i.glowA, (sw + 7) / 8,
                   (sh + 7) / 8);
        i.dispatch(c, i.blur.Get(), p, i.glowA.srv.Get(), nullptr, nullptr, i.glowB, (sw + 7) / 8,
                   (sh + 7) / 8);
        p.switches[2] = 1;
        i.dispatch(c, i.blur.Get(), p, i.glowB.srv.Get(), nullptr, nullptr, i.glowA, (sw + 7) / 8,
                   (sh + 7) / 8);
    }
    i.dispatch(c, i.finish.Get(), p, i.inputView.Get(), i.glowA.srv.Get(), gain, i.output, (d.Width + 7) / 8,
               (d.Height + 7) / 8);
    if (query) {
        c->End(query->end.Get());
        c->End(query->disjoint.Get());
        query->pending = true;
    }
    i.memory();
    diag.outcome = VisualOutcome::processed;
    diag.output_transfer = VisualTransfer::linear;
    return i.output.texture.Get();
}
} // namespace vrst::preview

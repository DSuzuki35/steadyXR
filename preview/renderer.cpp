#include "renderer.hpp"
#include "frame_buffer.hpp"
#include "speed_telemetry.hpp"
#include <algorithm>
#include <atomic>
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
 float4 imageRowX, imageRowY, imageMode, mirrorStyle;
 float4 comparison, originalFocus, originalRowX, originalRowY;
 float4 overlayRotation, overlayPosition, overlayShape, overlayMode;
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
float3 contextSample(float2 uv) {
 return contextImage.Sample(linearClamp,clamp(rect0.xy+uv*rect0.zw,clamp0.xy,clamp0.zw)).rgb;
}
float3 mirrorColor(float2 uv,float2 originalUV) {
 float2 over=max(max(-originalUV,originalUV-1),0);
 float distance=max(over.x,over.y);
 // Transition only outside the original image. Reuse the existing sample taps,
 // moving reflection toward its nearest real image edge within the same limit.
 [branch] if(mirrorStyle.w>0 && distance>0) {
   float stretch=mirrorStyle.w*smoothstep(0,max(.000001,imageMode.y),distance);
   uv=lerp(uv,saturate(originalUV),stretch);
 }
 float3 color=contextSample(uv);
 // Original pixels stay exact. Extra taps are used only in reflected areas.
 [branch] if(mirrorStyle.x<=0 || distance<=0) return color;
 float seam=smoothstep(0,max(.0001,min(.03,imageMode.y*.5)),distance);
 float amount=mirrorStyle.x*seam;
 float2 radius=mirrorStyle.yz*(18*amount);
 float3 blurred=color*.4;
 blurred+=contextSample(uv+float2(radius.x,radius.y))*.15;
 blurred+=contextSample(uv+float2(-radius.x,radius.y))*.15;
 blurred+=contextSample(uv+float2(radius.x,-radius.y))*.15;
 blurred+=contextSample(uv-radius)*.15;
 color=lerp(color,blurred,amount)*(1-.18*amount);
 // Feather the last part of the allowed reflection into the black border.
 float edge=smoothstep(imageMode.y*(1-.12*mirrorStyle.x),imageMode.y,distance);
 return color*(1-edge);
}
float4 PS(Varying v):SV_TARGET {
 bool original=comparison.x>.5 && v.uv.x>=.5;
 if(comparison.x>1.5) {
   // Two complete images fitted into half-width panels, without stretching.
   v.uv=float2(frac(v.uv.x*2), (v.uv.y-.25)*2);
   if(v.uv.y<0 || v.uv.y>1) return float4(0,0,0,1);
 }
 if (imageMode.x>.5) {
   float3 at=float3(v.uv,1);
   float2 uv=original ? float2(dot(originalRowX.xyz,at),dot(originalRowY.xyz,at)) :
                       float2(dot(imageRowX.xyz,at),dot(imageRowY.xyz,at));
   float2 originalUV=uv;
   if (flags.x>.5) {
     if(any(uv < -imageMode.y)||any(uv > 1+imageMode.y))return float4(0,0,0,1);
     uv=1-abs(1-2*frac(uv*.5));
   }
   else if(any(uv<0)||any(uv>1))return float4(0,0,0,1);
   return float4(mirrorColor(uv,originalUV),1);
 }
 float3 targetRay=float3(lerp(outputFov.x,outputFov.y,v.uv.x),lerp(outputFov.z,outputFov.w,v.uv.y),-1);
 float3 ray=original ? targetRay : rotateQ(correction,targetRay);
 float2 uv=project(ray,inputFov);
 float2 originalUV=uv;
 bool outside=any(uv<0)||any(uv>1)||ray.z>=-.05;
 if (flags.x>.5) {
   if(ray.z>=-.05||any(uv < -imageMode.y)||any(uv > 1+imageMode.y))return float4(0,0,0,1);
   uv=1-abs(1-2*frac(uv*.5));
 }
 else if (outside) return float4(0,0,0,1);
 float4 color=float4(mirrorColor(uv,originalUV),1);
 if (flags.y>.5 && !outside) {
   float3 focusRay=rotateQ(original ? originalFocus : focusCorrection,targetRay);
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
float4 VisualComposite(Varying v):SV_TARGET {
 bool original=comparison.x>.5 && v.uv.x>=.5;
 if(comparison.x>1.5) {
   v.uv=float2(frac(v.uv.x*2),(v.uv.y-.25)*2);
   if(v.uv.y<0 || v.uv.y>1) return float4(0,0,0,1);
 }
 if(any(v.uv<rect0.xy) || any(v.uv>rect0.xy+rect0.zw)) {
   if(comparison.x>1.5) return float4(0,0,0,1);
   discard; // Keep letterboxing outside the effects image untouched.
 }
 return original ? focusImage.Sample(linearClamp,v.uv) : contextImage.Sample(linearClamp,v.uv);
}
float4 Overlay(Varying v):SV_TARGET {
 bool original=comparison.x>.5 && comparison.y<.5 && v.uv.x>=.5;
 if(comparison.x>1.5) {
   v.uv=float2(frac(v.uv.x*2),(v.uv.y-.25)*2);
   if(v.uv.y<0 || v.uv.y>1) discard;
   if(comparison.y>.5) {
     v.uv=(v.uv-.5)/comparison.zw+.5;
     if(any(v.uv<0) || any(v.uv>1)) discard;
   }
 }
 float3 ray=float3(lerp(outputFov.x,outputFov.y,v.uv.x),lerp(outputFov.z,outputFov.w,v.uv.y),-1);
 if(!original) ray=rotateQ(correction,ray);
 // Pose is already relative to the captured context eye. Transform both the
 // eye origin and corrected viewing direction into the overlay's local space.
 float4 inverseRotation=float4(-overlayRotation.xyz,overlayRotation.w);
 float3 direction=rotateQ(inverseRotation,ray);
 float3 origin=rotateQ(inverseRotation,-overlayPosition.xyz);
 float2 uv;
 if(overlayMode.x<1.5) {
   if(direction.z>=-.000001) discard;
   float t=-origin.z/direction.z;
   if(t<=0) discard;
   float3 hit=origin+t*direction;
   uv=float2(hit.x/overlayShape.x+.5,.5-hit.y/overlayShape.y);
 } else if(overlayMode.x<2.5) {
   // OpenXR cylinder: inward-facing arc about local Y, centered on -Z.
   // Height is radius*centralAngle/aspectRatio. Radius zero denotes infinity.
   float3 hit;
   if(overlayShape.z==0) {
     float radial=length(direction.xz);
     if(radial<.000001) discard;
     hit=direction/radial;
   } else {
     float a=dot(direction.xz,direction.xz);
     float b=dot(origin.xz,direction.xz);
     float c=dot(origin.xz,origin.xz)-overlayShape.z*overlayShape.z;
     float discriminant=b*b-a*c;
     if(a<.000001 || discriminant<=0) discard;
     // The exit intersection faces the viewer from inside the cylinder.
     float t=(-b+sqrt(discriminant))/a;
     if(t<=0) discard;
     hit=(origin+t*direction)/overlayShape.z;
   }
   uv=float2(atan2(hit.x,-hit.z)/overlayShape.w+.5,
             .5-hit.y*overlayMode.w/overlayShape.w);
 } else {
   if(direction.z>=-.000001) discard;
   float2 tangent=direction.xy/-direction.z;
   uv=float2((tangent.x-focusFov.x)/(focusFov.y-focusFov.x),
             (focusFov.z-tangent.y)/(focusFov.z-focusFov.w));
 }
 if(any(uv<0)||any(uv>1)) discard;
 float4 color=contextImage.Sample(linearClamp,clamp(rect0.xy+uv*rect0.zw,clamp0.xy,clamp0.zw));
 if(overlayMode.y<.5) color.a=1;
 else if(overlayMode.z>.5) color.rgb*=color.a;
 return color;
}
)";
struct Params {
    float input[4], output[4], correction[4], focus[4], focus_correction[4];
    float rect0[4], rect1[4], clamp0[4], clamp1[4], flags[4];
    float image_row_x[4], image_row_y[4], image_mode[4], mirror_style[4];
    float comparison[4], original_focus[4], original_row_x[4], original_row_y[4];
    float overlay_rotation[4], overlay_position[4], overlay_shape[4], overlay_mode[4];
};
constexpr unsigned max_images = 2 + max_overlay_images;
unsigned image_count(const Frame &f) {
    return f.view_count + f.overlay_count;
}
const View &image_at(const Frame &f, unsigned i) {
    return i < f.view_count ? f.views[i] : f.overlays[i - f.view_count].image;
}
View &image_at(Frame &f, unsigned i) {
    return i < f.view_count ? f.views[i] : f.overlays[i - f.view_count].image;
}
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
Params full_eye_params(const Frame &frame, bool window) {
    Params p{};
    const auto &view = frame.views[0];
    const auto &focus = frame.views[frame.view_count - 1];
    rect(p.rect0, p.clamp0, view);
    if (window) {
        p.image_mode[0] = p.image_row_x[0] = p.image_row_y[1] = 1;
        return p;
    }
    std::copy_n(view.fov, 4, p.input);
    std::copy_n(view.fov, 4, p.output);
    std::copy_n(focus.fov, 4, p.focus);
    rect(p.rect1, p.clamp1, focus);
    auto orientation = [](const View &v) {
        return Quat{v.orientation[0], v.orientation[1], v.orientation[2], v.orientation[3]};
    };
    put(p.correction, Quat{});
    put(p.focus_correction, multiply(conjugate(orientation(focus)), orientation(view)));
    p.flags[1] = frame.view_count == 2;
    p.flags[2] = float(focus.rect_width);
    p.flags[3] = float(focus.rect_height);
    return p;
}
struct StillReadback {
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> staging;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    FramingStill image;
    unsigned row{};
    ~StillReadback() {
        if (mapped.pData)
            context->Unmap(staging.Get(), 0);
    }
};
// A conservative rectangle of captured pixels, excluding mirror/black borders.
// The projection is convex in front of the eye, so checking four corners is enough.
VisualRegion captured_region(const Params &p, const D3D11_VIEWPORT &vp, unsigned w, unsigned h) {
    auto inside = [&](double x, double y) {
        double u{}, v{};
        if (p.image_mode[0] > .5f) {
            u = p.image_row_x[0] * x + p.image_row_x[1] * y + p.image_row_x[2];
            v = p.image_row_y[0] * x + p.image_row_y[1] * y + p.image_row_y[2];
        } else {
            const Vec3 ray = rotate(Quat{p.correction[0], p.correction[1], p.correction[2], p.correction[3]},
                                    {p.output[0] + (p.output[1] - p.output[0]) * x,
                                     p.output[2] + (p.output[3] - p.output[2]) * y, -1});
            if (ray.z >= -.05)
                return false;
            u = (ray.x / -ray.z - p.input[0]) / (p.input[1] - p.input[0]);
            v = (p.input[2] - ray.y / -ray.z) / (p.input[2] - p.input[3]);
        }
        return u >= .001 && u <= .999 && v >= .001 && v <= .999;
    };
    double lo = 0, hi = .499;
    for (unsigned i = 0; i < 12; ++i) {
        const double radius = (lo + hi) * .5;
        if (inside(.5 - radius, .5 - radius) && inside(.5 + radius, .5 - radius) &&
            inside(.5 - radius, .5 + radius) && inside(.5 + radius, .5 + radius))
            lo = radius;
        else
            hi = radius;
    }
    float left = float(.5 - lo), right = float(.5 + lo), top = left, bottom = right;
    // Camera comparisons meter only the stabilized panel.
    if (p.comparison[0] > 1.5f) {
        left *= .5f;
        right *= .5f;
        top = .25f + top * .5f;
        bottom = .25f + bottom * .5f;
    } else if (p.comparison[0] > .5f)
        right = std::min(right, .499f);
    return {(vp.TopLeftX + left * vp.Width) / w, (vp.TopLeftY + top * vp.Height) / h,
            (vp.TopLeftX + right * vp.Width) / w, (vp.TopLeftY + bottom * vp.Height) / h};
}
std::atomic<std::uint64_t> next_output_generation{0};

// Keep the resources alive even when reporting device loss releases the renderer.
// Abandoned writes return to key 0; only a completed draw publishes key 1.
struct OutputWrite {
    ComPtr<IDXGIKeyedMutex> mutex;
    ComPtr<ID3D11DeviceContext> context;
    bool acquired{};
    HRESULT release(std::uint64_t key) {
        if (!acquired)
            return S_OK;
        context->Flush();
        acquired = false;
        return mutex->ReleaseSync(key);
    }
    ~OutputWrite() {
        release(0);
    }
};
} // namespace

struct Renderer::Impl {
    std::unique_ptr<StillReadback> still;
    HWND window{};
    bool offscreen{}, fixed_adapter{};
    LUID requested_adapter{};
    LUID adapter{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11Texture2D> back;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11Texture2D> output;
    ComPtr<ID3D11RenderTargetView> output_target;
    ComPtr<ID3D11ShaderResourceView> output_view;
    ComPtr<IDXGIKeyedMutex> output_mutex;
    std::uint64_t output_handle{}, output_generation{};
    ComPtr<ID3D11ShaderResourceView> waiting_view;
    std::wstring waiting_label;
    unsigned output_width{}, output_height{};
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11PixelShader> downsample;
    ComPtr<ID3D11PixelShader> visual_composite;
    ComPtr<ID3D11ShaderResourceView> visual_view, comparison_view;
    ComPtr<ID3D11Texture2D> comparison_image;
    VisualEffects visual_effects;
    bool visual_active{};
    std::uint64_t visual_session{}, visual_epoch{}, visual_generation{};
    unsigned visual_producer{};
    ComPtr<ID3D11PixelShader> overlay_shader;
    ComPtr<ID3D11BlendState> overlay_blend;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    void draw_eye(ID3D11RenderTargetView *target_view, const D3D11_VIEWPORT &viewport,
                  ID3D11Buffer *params, ID3D11ShaderResourceView *peripheral,
                  ID3D11ShaderResourceView *focus) {
        context->RSSetViewports(1, &viewport);
        context->OMSetRenderTargets(1, &target_view, nullptr);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        context->PSSetConstantBuffers(0, 1, &params);
        auto *sample = sampler.Get();
        context->PSSetSamplers(0, 1, &sample);
        ID3D11ShaderResourceView *images[]{peripheral, focus};
        context->PSSetShaderResources(0, 2, images);
        context->Draw(3, 0);
        ID3D11ShaderResourceView *empty[2]{};
        context->PSSetShaderResources(0, 2, empty);
    }
    ComPtr<ID3D11Texture2D> local[max_images], shared[max_images];
    ComPtr<IDXGIKeyedMutex> keyed[max_images];
    ComPtr<ID3D11ShaderResourceView> srv[max_images];
    Frame frame{};
    Frame opened{};
    FrameStabilizer stabilizer;
    FrameBuffer frame_buffer;
    DirectorController image_director;
    DirectorStatus director_status{};
    SpeedTelemetry speed_telemetry;
    bool speed_active{};
    unsigned gaze_capability{};
    ImageStabilizer image_stabilizer;
    ImageResult image_result{};
    bool window_mode{};
    struct Pending {
        ComPtr<ID3D11Texture2D> full, analysis_texture, staging;
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

    void release_visual() {
        visual_view.Reset();
        comparison_view.Reset();
        comparison_image.Reset();
        visual_composite.Reset();
        if (visual_active)
            visual_effects.reset();
        visual_active = false;
        visual_session = visual_epoch = visual_generation = 0;
        visual_producer = 0;
    }
    bool make_visual(bool paired) {
        if (!visual_composite) {
            ComPtr<ID3DBlob> blob, errors;
            HRESULT hr = D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, "VisualComposite",
                                    "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
            if (FAILED(hr) ||
                FAILED(hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                                      nullptr, &visual_composite)))
                return fail(L"Effects composite shader", hr);
        }
        if (!paired) {
            comparison_image.Reset();
            comparison_view.Reset();
            return true;
        }
        D3D11_TEXTURE2D_DESC d{}, existing{};
        output->GetDesc(&d);
        if (comparison_image)
            comparison_image->GetDesc(&existing);
        if (existing.Width == d.Width && existing.Height == d.Height)
            return true;
        d.MiscFlags = 0;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        HRESULT hr = device->CreateTexture2D(&d, nullptr, &texture);
        if (FAILED(hr) || FAILED(hr = device->CreateShaderResourceView(texture.Get(), &sv, &view)))
            return fail(L"Effects comparison image", hr);
        comparison_image = texture;
        comparison_view = view;
        return true;
    }

    bool fail(const wchar_t *where, HRESULT hr = E_FAIL) {
        std::wostringstream stream;
        stream << where << L" (0x" << std::hex << static_cast<unsigned long>(hr) << L")";
        if (device && FAILED(device->GetDeviceRemovedReason()))
            release();
        error = stream.str();
        return false;
    }
    void discard_output() {
        if (context)
            context->OMSetRenderTargets(0, nullptr, nullptr);
        output_view.Reset();
        output_target.Reset();
        output_mutex.Reset();
        output.Reset();
        output_handle = 0;
        output_width = output_height = 0;
        output_generation = ++next_output_generation;
    }
    void release() {
        if (context) {
            context->ClearState();
            context->Flush();
        }
        release_visual();
        reset_director();
        for (unsigned i = 0; i < max_images; ++i) {
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
        overlay_shader.Reset();
        overlay_blend.Reset();
        target.Reset();
        back.Reset();
        output_view.Reset();
        output_target.Reset();
        output_mutex.Reset();
        output_handle = 0;
        if (output)
            output_generation = ++next_output_generation;
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
    void reset_director() {
        frame_buffer.reset();
        image_director.reset();
        director_status = {};
        speed_telemetry.reset();
        speed_active = false;
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
        if (!offscreen) {
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
        }
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
        ComPtr<ID3DBlob> overlay_blob;
        hr = D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, "Overlay", "ps_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &overlay_blob, &errors);
        if (FAILED(hr) ||
            FAILED(hr = device->CreatePixelShader(overlay_blob->GetBufferPointer(),
                                                  overlay_blob->GetBufferSize(), nullptr, &overlay_shader)))
            return fail(L"Overlay shader", hr);
        D3D11_BLEND_DESC blend{};
        auto &b = blend.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = b.SrcBlendAlpha = D3D11_BLEND_ONE;
        b.DestBlend = b.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        b.BlendOp = b.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        b.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(hr = device->CreateBlendState(&blend, &overlay_blend)))
            return fail(L"Overlay blending", hr);
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
        return offscreen || resize();
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
        output_mutex.Reset();
        output_handle = 0;
        output_generation = ++next_output_generation;
        output.Reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = d.ArraySize = 1;
        d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (offscreen)
            d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
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
        if (offscreen) {
            ComPtr<IDXGIResource> resource;
            HANDLE handle{};
            if (FAILED(hr = output.As(&output_mutex)) || FAILED(hr = output.As(&resource)) ||
                FAILED(hr = resource->GetSharedHandle(&handle))) {
                output.Reset();
                output_mutex.Reset();
                return fail(L"Share offscreen output", hr);
            }
            output_handle = reinterpret_cast<std::uint64_t>(handle);
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
            r.frame.eye = std::min(options.eye, 1u);
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
    if (!p->analysis_texture || p->analysis_width != aw || p->analysis_height != ah) {
        p->analysis_texture.Reset();
        p->staging.Reset();
        p->target.Reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = aw;
        d.Height = ah;
        d.MipLevels = d.ArraySize = 1;
        d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.BindFlags = D3D11_BIND_RENDER_TARGET;
        HRESULT hr = r.device->CreateTexture2D(&d, nullptr, &p->analysis_texture);
        if (FAILED(hr) ||
            FAILED(hr = r.device->CreateRenderTargetView(p->analysis_texture.Get(), nullptr, &p->target)))
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
    r.context->CopyResource(p->staging.Get(), p->analysis_texture.Get());
    r.context->Flush();
    r.last_window_time = time_ns;
    r.pending.push_back(std::move(p));
    return completed;
}
void Renderer::source_changed() {
    auto &r = *impl_;
    r.reset_director();
    r.frame = {};
    r.opened = {};
    r.pending.clear();
    r.free_pending.clear();
    r.stabilizer.reset();
    r.release_visual();
    r.gaze_capability = 0;
    r.image_stabilizer.reset();
    r.image_result = {};
    r.window_mode = false;
    r.last_window_time = 0;
    for (unsigned i = 0; i < max_images; ++i) {
        r.local[i].Reset();
        r.srv[i].Reset();
        r.shared[i].Reset();
        r.keyed[i].Reset();
    }
}
bool Renderer::attach(HWND w) {
    impl_->window = w;
    impl_->offscreen = false;
    impl_->fixed_adapter = false;
    return impl_->initialize();
}
double Renderer::source_aspect() const {
    const auto &r = *impl_;
    if (!r.frame.view_count)
        return 0;
    const auto &v = r.frame.views[0];
    return r.window_mode ? double(v.rect_width) / v.rect_height
                         : double(v.fov[1] - v.fov[0]) / (v.fov[2] - v.fov[3]);
}
unsigned Renderer::source_eye() const {
    return impl_->frame.eye;
}

bool Renderer::begin_framing_still() {
    auto &r = *impl_;
    if (!r.device || r.still || !r.frame.view_count || r.frame.view_count > 2)
        return false;
    auto job = std::make_unique<StillReadback>();
    auto &image = job->image;
    image.eye = r.frame.eye;
    image.views = r.frame.view_count;
    image.window_source = r.window_mode;
    image.source_aspect = source_aspect();
    if (!std::isfinite(image.source_aspect) || image.source_aspect <= 0)
        return false;

    const auto &view = r.frame.views[0];
    const auto &focus = r.frame.views[image.views - 1];
    double width = std::max(double(view.rect_width), view.rect_height * image.source_aspect);
    if (image.views == 2) {
        // Preserve focus detail when the peripheral texture is low resolution.
        // The still covers the entire context FOV at square optical pixels.
        const double fx = (view.fov[1] - view.fov[0]) / (focus.fov[1] - focus.fov[0]);
        const double fy = (view.fov[2] - view.fov[3]) / (focus.fov[2] - focus.fov[3]);
        width = std::max({width, focus.rect_width * fx, focus.rect_height * fy * image.source_aspect});
    }
    // Bound this interactive reference's memory and GPU time on large HMDs.
    width = std::min({width, 2560., 2560. * image.source_aspect});
    image.width = std::max(1L, std::lround(width));
    image.height = std::max(1L, std::lround(width / image.source_aspect));
    image.pixels.reset(new (std::nothrow) std::uint32_t[std::size_t(image.width) * image.height]);
    if (!image.pixels)
        return false;
    job->context = r.context;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = image.width;
    d.Height = image.height;
    d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> color;
    ComPtr<ID3D11RenderTargetView> target;
    if (FAILED(r.device->CreateTexture2D(&d, nullptr, &color)) ||
        FAILED(r.device->CreateRenderTargetView(color.Get(), nullptr, &target)))
        return false;
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(r.device->CreateTexture2D(&d, nullptr, &job->staging)))
        return false;
    const auto params = full_eye_params(r.frame, r.window_mode);
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(params);
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    D3D11_SUBRESOURCE_DATA data{&params, 0, 0};
    ComPtr<ID3D11Buffer> cb;
    if (FAILED(r.device->CreateBuffer(&desc, &data, &cb)))
        return false;

    // The consumer already owns both eye textures. No producer mutex or live
    // output texture is acquired by this pass, including with an offscreen reader.
    D3D11_VIEWPORT vp{0, 0, float(image.width), float(image.height), 0, 1};
    r.context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    r.draw_eye(target.Get(), vp, cb.Get(), r.srv[0].Get(), r.srv[image.views - 1].Get());
    r.context->OMSetRenderTargets(0, nullptr, nullptr);
    r.context->CopyResource(job->staging.Get(), color.Get());
    r.context->Flush();
    r.still = std::move(job);
    return true;
}

StillStatus Renderer::poll_framing_still(FramingStill &result) {
    auto &job = impl_->still;
    if (!job)
        return StillStatus::failed;
    if (!job->mapped.pData) {
        const auto hr = job->context->Map(job->staging.Get(), 0, D3D11_MAP_READ,
                                          D3D11_MAP_FLAG_DO_NOT_WAIT, &job->mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
            return StillStatus::pending;
        if (FAILED(hr)) {
            job.reset();
            return StillStatus::failed;
        }
    }
    auto &image = job->image;
    const unsigned pitch = image.width * sizeof(std::uint32_t);
    const unsigned end = std::min(image.height, job->row + (2 * 1024 * 1024 / pitch));
    // Budget CPU copying as well as GPU waiting, including first-touch page faults.
    for (; job->row < end; ++job->row)
        std::memcpy(image.pixels.get() + std::size_t(job->row) * image.width,
                    static_cast<const BYTE *>(job->mapped.pData) + std::size_t(job->row) * job->mapped.RowPitch,
                    pitch);
    if (end != image.height)
        return StillStatus::pending;
    result = std::move(image);
    job.reset();
    return StillStatus::ready;
}
bool Renderer::source_frame(Frame &frame) const {
    const auto &r = *impl_;
    if (r.window_mode || !r.frame.view_count)
        return false;
    frame = r.frame;
    return true;
}
bool Renderer::attach_offscreen(const LUID *adapter) {
    impl_->window = nullptr;
    impl_->offscreen = true;
    impl_->fixed_adapter = adapter != nullptr;
    impl_->requested_adapter = adapter ? *adapter : LUID{};
    return impl_->initialize(adapter);
}
std::uint64_t Renderer::output_handle() const {
    return impl_->output_handle;
}
std::uint64_t Renderer::output_generation() const {
    return impl_->output_generation;
}
void Renderer::reset() {
    impl_->reset_director();
    impl_->stabilizer.reset();
    impl_->release_visual();
    impl_->image_stabilizer.reset();
    impl_->image_result = {};
    impl_->pending.clear();
}
void Renderer::reset_horizon() {
    impl_->stabilizer.reset_horizon();
}
void Renderer::learn_chat() {
    impl_->stabilizer.learn_chat();
}
void Renderer::cancel_chat() {
    impl_->stabilizer.cancel_chat();
}
void Renderer::clear_chat() {
    impl_->stabilizer.clear_chat();
}
SpectatorStatus Renderer::spectator_status() const {
    return impl_->stabilizer.spectator_status();
}
DirectorStatus Renderer::director_status() const {
    return impl_->director_status;
}
bool Renderer::calibration_active() const {
    return impl_->stabilizer.calibration_active();
}
unsigned Renderer::gaze_capability() const {
    return impl_->gaze_capability;
}
const VisualDiagnostics &Renderer::visual_diagnostics() const {
    return impl_->visual_effects.diagnostics();
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

bool Renderer::consume(Channel &channel, bool require_consumer) {
    auto &r = *impl_;
    if (r.window_mode)
        source_changed();
    if (!channel.lock(require_consumer))
        return false;
    r.gaze_capability = channel.data()->gaze_capability;
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
        for (unsigned i = 0; i < max_images; ++i) {
            r.shared[i].Reset();
            r.keyed[i].Reset();
            r.local[i].Reset();
            r.srv[i].Reset();
        }
        if (r.window)
            InvalidateRect(r.window, nullptr, FALSE);
        return r.fail(where, hr);
    };
    const Frame packet = s.frame;
    if (!valid_frame(packet))
        return recover(L"Invalid image metadata");
    if (r.fixed_adapter && !same(r.requested_adapter, packet.adapter))
        return recover(L"Game graphics adapter does not match the requested output adapter");
    if (!r.device || !same(r.adapter, packet.adapter)) {
        if (!r.initialize(&packet.adapter)) {
            s.ready = 2;
            return false;
        }
    }
    bool base_changed = packet.generation != r.opened.generation ||
                        packet.producer_pid != r.opened.producer_pid ||
                        packet.view_count != r.opened.view_count || packet.session != r.opened.session;
    for (unsigned i = 0; i < packet.view_count; ++i)
        base_changed = base_changed || packet.views[i].texture_handle != r.opened.views[i].texture_handle;
    bool changed = base_changed || packet.overlay_count != r.opened.overlay_count;
    const unsigned count = image_count(packet);
    if (!changed)
        for (unsigned i = 0; i < count; ++i) {
            const auto &a = image_at(packet, i), &b = image_at(r.opened, i);
            changed = changed || a.texture_handle != b.texture_handle || a.width != b.width ||
                      a.height != b.height || a.format != b.format;
        }
    if (changed) {
        // Every retained texture has matching retained metadata. If replacement
        // opening or acquisition fails, never draw its predecessor's pose.
        r.frame = {};
        r.opened = {};
        if (base_changed)
            r.stabilizer.reset();
        for (unsigned i = 0; i < max_images; ++i) {
            r.shared[i].Reset();
            r.keyed[i].Reset();
            r.local[i].Reset();
            r.srv[i].Reset();
        }
        for (unsigned i = 0; i < count; ++i) {
            const auto &v = image_at(packet, i);
            HRESULT hr = r.device->OpenSharedResource(reinterpret_cast<HANDLE>(v.texture_handle),
                                                      IID_PPV_ARGS(&r.shared[i]));
            if (FAILED(hr))
                return recover(L"Open game image", hr);
            D3D11_TEXTURE2D_DESC d{};
            r.shared[i]->GetDesc(&d);
            if (d.Width != v.width || d.Height != v.height || d.Format != v.format || d.ArraySize != 1 ||
                d.MipLevels != 1 || d.SampleDesc.Count != 1)
                return recover(L"Image format does not match metadata");
            hr = r.shared[i].As(&r.keyed[i]);
            d.Width = v.rect_width;
            d.Height = v.rect_height;
            if (FAILED(hr) || !r.make_local(i, d))
                return recover(L"GPU image connection", hr);
        }
        r.opened = packet;
    }
    // A runtime may resize its submitted subrect without replacing the shared
    // texture. Resize only the private eye image; do not reset pose history.
    for (unsigned i = 0; i < count; ++i) {
        const auto &v = image_at(packet, i);
        D3D11_TEXTURE2D_DESC d{};
        r.local[i]->GetDesc(&d);
        if (d.Width != unsigned(v.rect_width) || d.Height != unsigned(v.rect_height)) {
            r.frame = {}; // The retained pixels are being replaced, even if acquisition must retry.
            d.Width = v.rect_width;
            d.Height = v.rect_height;
            if (!r.make_local(i, d))
                return recover(L"Resize private eye image");
        }
    }
    unsigned acquired = 0;
    HRESULT acquisition = S_OK;
    for (; acquired < count; ++acquired) {
        acquisition = r.keyed[acquired]->AcquireSync(1, 0);
        if (acquisition != S_OK)
            break;
    }
    if (acquired != count) {
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
    for (unsigned i = 0; i < count; ++i) {
        const auto &v = image_at(packet, i);
        const D3D11_BOX box{UINT(v.x), UINT(v.y), 0, UINT(v.x + v.rect_width), UINT(v.y + v.rect_height), 1};
        r.context->CopySubresourceRegion(r.local[i].Get(), 0, 0, 0, 0, r.shared[i].Get(), 0, &box);
    }
    r.context->Flush();
    HRESULT release = S_OK;
    for (unsigned i = 0; i < count; ++i) {
        auto hr = r.keyed[i]->ReleaseSync(0);
        if (FAILED(hr))
            release = hr;
    }
    if (FAILED(release))
        return recover(L"Release GPU image", release);
    s.ready = 0;
    r.frame = packet;
    for (unsigned i = 0; i < count; ++i) {
        auto &v = image_at(r.frame, i);
        v.x = v.y = 0;
        v.width = v.rect_width;
        v.height = v.rect_height;
    }
    r.error.clear();
    return true;
}

bool Renderer::demo(double seconds, bool quad, unsigned eye) {
    auto &r = *impl_;
    if (!r.device)
        return false;
    const auto display_time = 1000000000LL + static_cast<std::int64_t>(seconds * 1e9);
    const auto view_count = quad ? 2u : 1u;
    eye = std::min(eye, 1u);
    // An overdue render and its catch-up iteration can share one Windows clock tick.
    // Re-present the same simulated pose and textures without advancing pose history.
    if (!r.window_mode && r.frame.producer_pid == GetCurrentProcessId() && r.frame.view_count == view_count &&
        r.frame.eye == eye && r.frame.sequence == r.demo_sequence && r.frame.display_time == display_time)
        return true;
    constexpr unsigned size = 512;
    if (!r.frame.view_count || r.frame.producer_pid != GetCurrentProcessId() || r.frame.view_count != view_count) {
        std::vector<std::uint32_t> pixels(size * size);
        for (unsigned i = 0; i < view_count; ++i) {
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
    f.display_time = display_time;
    f.producer_pid = GetCurrentProcessId();
    f.view_count = view_count;
    f.eye = eye;
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
    r.error.clear();
    return true;
}

bool Renderer::draw(const Options &options, bool present, const wchar_t *waiting_text) {
    auto &r = *impl_;
    if (!r.device || (!r.offscreen && (!r.swap || !r.resize())) || !r.resize_output(options))
        return false;
    OutputWrite writer{r.output_mutex, r.context};
    if (writer.mutex) {
        const HRESULT hr = writer.mutex->AcquireSync(0, 0);
        if (hr != S_OK) {
            if (hr == WAIT_ABANDONED)
                r.discard_output();
            return hr == WAIT_TIMEOUT ? false : r.fail(L"Acquire offscreen output", hr);
        }
        writer.acquired = true;
    }
    const auto director = sanitize_director_settings(options.director);
    const bool active_source = r.frame.view_count && !waiting_text;
    SpeedReading speed;
    if (active_source && director.speed_fov_enabled && !options.spectator.bypass) {
        speed = r.speed_telemetry.sample(static_cast<SpeedGame>(director.telemetry_game));
        r.speed_active = true;
    } else if (r.speed_active) {
        r.speed_telemetry.reset();
        r.speed_active = false;
    }
    if (active_source && !r.window_mode && director.lookahead_enabled && options.stabilize &&
        !options.spectator.bypass)
        r.frame_buffer.update(r.device.Get(), r.context.Get(), r.frame, r.local, r.srv,
                              unsigned(director.lookahead_frames), speed.valid ? speed.speed_kph : -1,
                              static_cast<unsigned>(speed.game));
    else
        r.frame_buffer.reset();
    if (!active_source) {
        r.image_director.reset();
        r.stabilizer.reset_director();
        r.director_status = {};
    }
    const auto *buffered = r.frame_buffer.selected();
    const auto &draw_frame = buffered ? buffered->frame : r.frame;
    const auto *draw_views = buffered ? buffered->views.data() : r.srv;
    const auto *future_target = r.frame_buffer.target();
    const double draw_speed = !speed.valid ? -1 : buffered ? buffered->speed_kph : speed.speed_kph;
    auto record_director_status = [&](DirectorStatus status) {
        const auto buffer = r.frame_buffer.status();
        status.buffered_frames = buffer.buffered_frames;
        status.buffered_bytes = buffer.buffered_bytes;
        status.delay_ms = buffer.delay_ms;
        status.buffer_limited = buffer.buffer_limited;
        status.telemetry_game = buffered ? buffered->game : static_cast<unsigned>(speed.game);
        r.director_status = status;
    };
    const bool black = waiting_text || r.offscreen;
    auto visual = options.visual;
    visual.bypass = visual.bypass || options.spectator.bypass;
    const bool effects = visual.active() && draw_frame.view_count && !waiting_text;
    if (!effects && r.visual_active)
        r.release_visual();
    const float clear[4] = {black ? 0.f : .005f, black ? 0.f : .009f, black ? 0.f : .016f, 1};
    r.context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    r.context->ClearRenderTargetView(r.output_target.Get(), clear);
    if (waiting_text) {
        if (!r.make_waiting(waiting_text))
            return false;
        // Use the same GPU output path as live frames. GDI painting over a
        // flip-model swapchain cannot reliably replace a previously presented frame.
        const float scale =
            r.offscreen ? 1.f : std::min(float(r.width) / r.output_width, float(r.height) / r.output_height);
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
    } else if (draw_frame.view_count) {
        Params p{};
        const bool paired = options.compare_original && !options.compare_effects &&
                            options.compare_mode >= 1 && options.compare_mode <= 2;
        p.comparison[0] = paired ? float(options.compare_mode) : 0.f;
        p.image_mode[1] =
            float(std::isfinite(options.mirror_limit) ? std::clamp(options.mirror_limit, 0., 50.) / 100 : 0.);
        p.mirror_style[0] =
            float(std::isfinite(options.mirror_softness) ? std::clamp(options.mirror_softness, 0., 100.) / 100
                                                         : 0.);
        p.mirror_style[1] = 1.f / std::max(1, draw_frame.views[0].rect_width);
        p.mirror_style[2] = 1.f / std::max(1, draw_frame.views[0].rect_height);
        p.mirror_style[3] = float(
            std::isfinite(options.mirror_stretch) ? std::clamp(options.mirror_stretch, 0., 100.) / 100 : 0.);
        if (r.window_mode) {
            Settings settings;
            settings.stabilization_crop = options.crop;
            const bool director_framing =
                !options.spectator.bypass && (director.auto_crop_enabled || director.speed_fov_enabled);
            settings.mirror_edges = options.mirror || (director_framing && director.auto_crop_enabled);
            settings.output_16_9 = false;
            settings.edge_reserve = sanitize_edge_reserve(options.edge_reserve[std::min(options.eye, 1u)]);
            auto result = r.image_result;
            if (!options.stabilize ||
                (options.compare_original && !options.compare_effects && !paired && !director_framing))
                result.correction = {};
            double source_aspect = double(draw_frame.views[0].width) / draw_frame.views[0].height;
            if (settings.edge_reserve.enabled) {
                const auto &edges = settings.edge_reserve;
                source_aspect *= (100 - edges.left - edges.right) / (100 - edges.top - edges.bottom);
            }
            const auto region = framing_region(source_aspect, options, settings.edge_reserve);
            const bool final_framing =
                options.crop > 40. || director_framing || settings.edge_reserve.enabled ||
                (std::isfinite(options.horizontal_offset) && options.horizontal_offset != 0) ||
                (std::isfinite(options.vertical_offset) && options.vertical_offset != 0);
            auto make_framed = [&](const ImageResult &motion) {
                auto warp =
                    make_image_warp(draw_frame.views[0].width, draw_frame.views[0].height, settings, motion,
                                    final_framing ? std::array<double, 4>{region.left, region.right,
                                                                          region.top, region.bottom}
                                                  : std::array<double, 4>{0, 1, 0, 1});
                if (!final_framing)
                    for (auto *row : {&warp.row_x, &warp.row_y}) {
                        (*row)[2] += (*row)[0] * region.left + (*row)[1] * region.top;
                        (*row)[0] *= region.right - region.left;
                        (*row)[1] *= region.bottom - region.top;
                    }
                return warp;
            };
            auto warp = make_framed(result);
            auto original = result;
            original.correction = {};
            auto original_warp = make_framed(original);
            r.image_director.update_image(warp, director, draw_frame.display_time, draw_frame.sequence,
                                          draw_speed, options.spectator.bypass, &original_warp);
            const auto status = r.image_director.status();
            record_director_status(status);
            if (director_framing) {
                const double keep =
                    (1 - status.auto_crop_percent / 100) * (1 - status.speed_crop_percent / 100);
                for (auto *row : {&original_warp.row_x, &original_warp.row_y}) {
                    (*row)[2] += ((*row)[0] + (*row)[1]) * (1 - keep) * .5;
                    (*row)[0] *= keep;
                    (*row)[1] *= keep;
                }
                if (options.compare_original && !options.compare_effects && !paired)
                    warp = original_warp;
            }
            std::copy(warp.row_x.begin(), warp.row_x.end(), p.image_row_x);
            std::copy(warp.row_y.begin(), warp.row_y.end(), p.image_row_y);
            if (paired) {
                std::copy(original_warp.row_x.begin(), original_warp.row_x.end(), p.original_row_x);
                std::copy(original_warp.row_y.begin(), original_warp.row_y.end(), p.original_row_y);
            }
            p.image_mode[0] = 1;
            rect(p.rect0, p.clamp0, draw_frame.views[0]);
            p.flags[0] = options.mirror ? 1.f : 0.f;
        } else {
            auto tuning = options;
            if (paired)
                tuning.compare_original = false;
            const auto composition = r.stabilizer.update(draw_frame, tuning, future_target, draw_speed);
            if (!composition.valid)
                return r.fail(L"Invalid pose or projection");
            record_director_status(r.stabilizer.director_status());
            put(p.input, composition.warp.input);
            put(p.output, composition.warp.output);
            put(p.correction, composition.warp.correction);
            put(p.focus_correction, composition.focus_correction);
            if (paired) {
                // A redraw of this same frame does not advance pose history.
                tuning.compare_original = true;
                const auto original = r.stabilizer.update(draw_frame, tuning, future_target, draw_speed);
                put(p.original_focus, original.focus_correction);
            }
            const auto &focus = draw_frame.views[draw_frame.view_count - 1];
            std::copy(focus.fov, focus.fov + 4, p.focus);
            rect(p.rect0, p.clamp0, draw_frame.views[0]);
            rect(p.rect1, p.clamp1, focus);
            p.flags[0] = options.mirror ? 1.f : 0.f;
            p.flags[1] = draw_frame.view_count == 2 ? 1.f : 0.f;
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
        auto *rt = r.output_target.Get();
        r.draw_eye(rt, vp, r.constants.Get(), draw_views[0].Get(), draw_views[draw_frame.view_count - 1].Get());
        ID3D11ShaderResourceView *empty[2] = {};
        if (effects) {
            const bool compare = options.compare_original && options.compare_effects;
            const unsigned layout = compare && options.compare_mode <= 2 ? options.compare_mode : 0;
            if (!r.make_visual(layout != 0))
                return false;
            r.visual_active = true;
            r.context->OMSetRenderTargets(0, nullptr, nullptr);
            if (r.visual_session != draw_frame.session || r.visual_epoch != draw_frame.reference_epoch ||
                r.visual_producer != draw_frame.producer_pid ||
                r.visual_generation != draw_frame.generation) {
                r.visual_view.Reset();
                r.visual_effects.reset();
                r.visual_session = draw_frame.session;
                r.visual_epoch = draw_frame.reference_epoch;
                r.visual_producer = draw_frame.producer_pid;
                r.visual_generation = draw_frame.generation;
            }
            if (layout)
                r.context->CopyResource(r.comparison_image.Get(), r.output.Get());
            visual.input_transfer = VisualTransfer::srgb;
            visual.premultiplied_alpha = false;
            visual.content_region = captured_region(p, vp, r.output_width, r.output_height);
            auto *processed =
                r.visual_effects.process(r.device.Get(), r.context.Get(), r.output.Get(), visual,
                                         draw_frame.sequence, double(draw_frame.display_time) * 1e-9);
            if (processed != r.output.Get() && !(compare && layout == 0)) {
                ComPtr<ID3D11Resource> current;
                if (r.visual_view)
                    r.visual_view->GetResource(&current);
                if (current.Get() != processed) {
                    ComPtr<ID3D11ShaderResourceView> view;
                    hr = r.device->CreateShaderResourceView(processed, nullptr, &view);
                    if (FAILED(hr))
                        return r.fail(L"Effects output view", hr);
                    r.visual_view = view;
                }
                auto composite = p;
                composite.comparison[0] = float(layout);
                composite.rect0[0] = vp.TopLeftX / r.output_width;
                composite.rect0[1] = vp.TopLeftY / r.output_height;
                composite.rect0[2] = vp.Width / r.output_width;
                composite.rect0[3] = vp.Height / r.output_height;
                hr = r.context->Map(r.constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
                if (FAILED(hr))
                    return r.fail(L"Effects comparison parameters", hr);
                std::memcpy(mapped.pData, &composite, sizeof(composite));
                r.context->Unmap(r.constants.Get(), 0);
                D3D11_VIEWPORT full{0, 0, float(r.output_width), float(r.output_height), 0, 1};
                r.context->RSSetViewports(1, &full);
                r.context->OMSetRenderTargets(1, &rt, nullptr);
                r.context->PSSetShader(r.visual_composite.Get(), nullptr, 0);
                ID3D11ShaderResourceView *images[]{r.visual_view.Get(), r.comparison_view.Get()};
                r.context->PSSetShaderResources(0, 2, images);
                r.context->Draw(3, 0);
                r.context->PSSetShaderResources(0, 2, empty);
                if (layout) {
                    p.comparison[0] = float(layout);
                    p.comparison[1] = 1; // Effects comparison keeps identical camera orientation.
                    p.comparison[2] = vp.Width / r.output_width;
                    p.comparison[3] = vp.Height / r.output_height;
                }
            }
            r.context->RSSetViewports(1, &vp);
            r.context->OMSetRenderTargets(1, &rt, nullptr);
        }
        if (!r.window_mode) {
            if (p.comparison[0] > 1.5f && p.comparison[1] > .5f) {
                D3D11_VIEWPORT full{0, 0, float(r.output_width), float(r.output_height), 0, 1};
                r.context->RSSetViewports(1, &full);
            }
            r.context->PSSetShader(r.overlay_shader.Get(), nullptr, 0);
            r.context->OMSetBlendState(r.overlay_blend.Get(), nullptr, 0xffffffff);
            for (unsigned i = 0; i < draw_frame.overlay_count; ++i) {
                const auto &overlay = draw_frame.overlays[i];
                if (!overlay_selected(options.overlays, draw_frame.producer_pid, draw_frame.session,
                                      overlay.slot))
                    continue;
                auto op = p;
                std::copy_n(overlay.orientation, 4, op.overlay_rotation);
                // Normalize within the transport's accepted quaternion tolerance.
                const float length = std::sqrt(op.overlay_rotation[0] * op.overlay_rotation[0] +
                                               op.overlay_rotation[1] * op.overlay_rotation[1] +
                                               op.overlay_rotation[2] * op.overlay_rotation[2] +
                                               op.overlay_rotation[3] * op.overlay_rotation[3]);
                for (auto &component : op.overlay_rotation)
                    component /= length;
                std::copy_n(overlay.position, 3, op.overlay_position);
                op.overlay_shape[0] = overlay.width;
                op.overlay_shape[1] = overlay.height;
                op.overlay_shape[2] = std::isfinite(overlay.radius) ? overlay.radius : 0;
                op.overlay_shape[3] = overlay.central_angle;
                op.overlay_mode[0] = float(overlay.type);
                op.overlay_mode[1] = (overlay.flags & 1) ? 1.f : 0.f;
                op.overlay_mode[2] = (overlay.flags & 2) ? 1.f : 0.f;
                op.overlay_mode[3] = overlay.aspect_ratio;
                std::copy_n(overlay.image.fov, 4, op.focus);
                rect(op.rect0, op.clamp0, overlay.image);
                hr = r.context->Map(r.constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
                if (FAILED(hr)) {
                    r.context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
                    return r.fail(L"Update overlay parameters", hr);
                }
                std::memcpy(mapped.pData, &op, sizeof(op));
                r.context->Unmap(r.constants.Get(), 0);
                auto *overlay_view = draw_views[draw_frame.view_count + i].Get();
                r.context->PSSetShaderResources(0, 1, &overlay_view);
                r.context->Draw(3, 0);
            }
            r.context->PSSetShaderResources(0, 2, empty);
            r.context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        }
    }
    if (r.offscreen) {
        r.context->OMSetRenderTargets(0, nullptr, nullptr);
        const HRESULT removed = r.device->GetDeviceRemovedReason();
        if (FAILED(removed))
            return r.fail(L"Render offscreen output", removed);
        const HRESULT hr = writer.release(1);
        if (FAILED(hr))
            return r.fail(L"Publish offscreen output", hr);
        r.error.clear();
        return true;
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
    OutputWrite reader{r.output_mutex, r.context};
    if (reader.mutex) {
        const HRESULT hr = reader.mutex->AcquireSync(1, 0);
        if (hr != S_OK) {
            if (hr == WAIT_ABANDONED)
                r.discard_output();
            return hr == WAIT_TIMEOUT ? false : r.fail(L"Acquire diagnostic output", hr);
        }
        reader.acquired = true;
    }
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

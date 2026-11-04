#include "workshop_clip.hpp"
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>

using namespace vrst::preview;
namespace vrst::preview::clip_detail {
bool bounded_h264_sps(const unsigned char *, std::size_t, unsigned, unsigned);
}
int failures{};
void check(bool ok, const char *what) {
    if (!ok) {
        std::cerr << "FAIL " << what << '\n';
        ++failures;
    }
}
Frame source(unsigned views = 2) {
    Frame f{};
    f.generation = 9;
    f.sequence = 1;
    f.session = 2;
    f.reference_epoch = 3;
    f.display_time = 1000000000;
    f.producer_pid = 5;
    f.view_count = views;
    for (unsigned i = 0; i < views; ++i) {
        f.views[i].width = f.views[i].rect_width = 320;
        f.views[i].height = f.views[i].rect_height = 240;
        f.views[i].format = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    return f;
}
ClipInfo valid_info() {
    ClipInfo info;
    info.width[0] = info.width[1] = 320;
    info.height[0] = info.height[1] = 240;
    for (int i = 0; i < 3; ++i) {
        ClipFrame sample;
        sample.frame = source();
        sample.frame.sequence += i;
        sample.frame.display_time += i * 40000000LL;
        sample.media_time = i * 400000LL;
        info.frames.push_back(sample);
    }
    return info;
}
std::vector<unsigned char> sps(unsigned width_mbs, unsigned height_mbs, unsigned references = 2) {
    std::vector<bool> bits;
    auto fixed = [&](unsigned value, unsigned count) {
        for (unsigned i = count; i; --i)
            bits.push_back((value >> (i - 1)) & 1);
    };
    auto ue = [&](unsigned value) {
        ++value;
        unsigned count = 0;
        for (auto v = value; v; v >>= 1)
            ++count;
        for (unsigned i = 1; i < count; ++i)
            bits.push_back(false);
        fixed(value, count);
    };
    fixed(77, 8);
    fixed(0, 8);
    fixed(31, 8);
    ue(0);
    ue(0);
    ue(0);
    ue(0);
    ue(references);
    fixed(0, 1);
    ue(width_mbs - 1);
    ue(height_mbs - 1);
    fixed(1, 1);
    fixed(1, 1);
    fixed(0, 1);
    fixed(0, 1);
    fixed(1, 1);
    std::vector<unsigned char> bytes{0x67};
    for (std::size_t i = 0; i < bits.size(); i += 8) {
        unsigned char b = 0;
        for (unsigned j = 0; j < 8; ++j)
            b = static_cast<unsigned char>((b << 1) | ((i + j < bits.size() && bits[i + j]) ? 1 : 0));
        bytes.push_back(b);
    }
    return bytes;
}
CaptureStatus wait_prepared(ClipRecorder &r) {
    for (int n = 0; n < 500 && r.status().state == CaptureState::preparing; ++n)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return r.status();
}
CaptureStatus wait_finished(ClipRecorder &r) {
    for (int n = 0; n < 1000 && r.status().state == CaptureState::finishing; ++n)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return r.status();
}
ULONG references(IUnknown *object) {
    object->AddRef();
    return object->Release();
}
void check_capture_reclaimed(ClipRecorder &recorder, ID3D11Device *device, ID3D11Texture2D *source_texture,
                             ULONG device_baseline, ULONG texture_baseline, CaptureState expected) {
    auto terminal = recorder.status();
    for (unsigned attempt = 0; attempt < 200; ++attempt) {
        recorder.status(); // Polling must eventually reap without destroying the recorder.
        if (references(device) == device_baseline && references(source_texture) == texture_baseline)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto after = recorder.status();
    check(references(source_texture) == texture_baseline,
          "terminal recorder releases retained source texture");
    check(references(device) == device_baseline,
          "terminal recorder releases all GPU surfaces/views/device references");
    check(after.state == expected && after.state == terminal.state && after.path == terminal.path &&
              after.message == terminal.message && after.recorded_frames == terminal.recorded_frames &&
              after.dropped_frames == terminal.dropped_frames,
          "resource reap preserves terminal recording status");
    if (references(device) != device_baseline)
        std::cout << "Retained recording device references: " << references(device)
                  << "; baseline: " << device_baseline << '\n';
}
void hardware_lifetime_cases(ID3D11Device *device, const std::filesystem::path &base) {
    for (auto format :
         {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) {
        auto f = source(1);
        f.views[0].format = format;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = 320;
        d.Height = 240;
        d.Format = format;
        d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        std::vector<std::uint32_t> pixels(320 * 240, 0xff2090e0u);
        D3D11_SUBRESOURCE_DATA data{pixels.data(), 320 * 4, 0};
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        check(SUCCEEDED(device->CreateTexture2D(&d, &data, &texture)), "lifetime test source texture");
        if (!texture)
            continue;
        ID3D11Texture2D *raw[2]{texture.Get(), nullptr};
        for (unsigned outcome = 0; outcome < 3; ++outcome) {
            ClipRecorder recorder;
            auto path = base;
            path += L".lifetime-" + std::to_wstring(unsigned(format)) + L"-" + std::to_wstring(outcome);
            std::error_code ec;
            if (outcome == 2)
                std::filesystem::create_directory(path, ec); // Worker initialization failure.
            const auto device_baseline = references(device), texture_baseline = references(texture.Get());
            check(recorder.start(device, path, f), "lifetime recording starts with GPU resources");
            auto status = wait_prepared(recorder);
            if (outcome != 2) {
                check(status.state == CaptureState::recording, "lifetime encoder ready");
                check(references(device) > device_baseline,
                      "lifetime regression observes live GPU resource references");
                check(recorder.submit(f, raw), "lifetime sample accepted");
                if (outcome == 0)
                    recorder.stop();
                else
                    recorder.cancel();
                status = wait_finished(recorder);
            }
            const auto expected = outcome == 0   ? CaptureState::ready
                                  : outcome == 1 ? CaptureState::cancelled
                                                 : CaptureState::failed;
            check_capture_reclaimed(recorder, device, texture.Get(), device_baseline, texture_baseline,
                                    expected);
            std::filesystem::remove_all(path, ec);
        }
    }
}
void hardware_edge_cases(ID3D11Device *device, ID3D11Texture2D *const textures[2], Frame f,
                         const std::filesystem::path &base) {
    ClipRecorder r;
    CaptureOptions options;
    options.duration_seconds = 1;
    auto pressure = base;
    pressure += L".pressure";
    f.sequence = 1;
    f.display_time = 1000000000LL;
    check(r.start(device, pressure, f, options) && wait_prepared(r).state == CaptureState::recording,
          "queue-pressure preparation");
    for (unsigned i = 0; i < 30; ++i) {
        f.sequence = i + 1;
        f.display_time = 1000000000LL + i * 34000000LL;
        r.submit(f, textures);
    }
    f.sequence = 31;
    f.display_time = 2020000000LL;
    r.submit(f, textures);
    auto status = wait_finished(r);
    check(status.state == CaptureState::ready && status.recorded_frames > 0 && status.recorded_frames <= 30,
          "duration-bound finalization under overload");
    check(status.dropped_frames > 0, "bounded queue drops instead of waiting");
    std::wcout << L"Queue pressure: " << status.recorded_frames << L" accepted, " << status.dropped_frames
               << L" dropped\n";
    ClipReader reader;
    check(reader.open(device, pressure) && !reader.info().partial &&
              reader.info().dropped_frames == status.dropped_frames,
          "duration completion and drops stored");
    reader.close();
    auto recenter = base;
    recenter += L".recenter";
    f.sequence = 1;
    f.display_time = 1000000000LL;
    check(r.start(device, recenter, f) && wait_prepared(r).state == CaptureState::recording,
          "recenter preparation");
    check(r.submit(f, textures), "initial recenter sample");
    ++f.sequence;
    f.display_time += 40000000LL;
    ++f.reference_epoch;
    check(!r.submit(f, textures), "recenter stops submission");
    status = wait_finished(r);
    check(status.state == CaptureState::ready && status.recorded_frames == 1,
          "recenter saves identified partial clip");
    check(reader.open(device, recenter) && reader.info().partial &&
              reader.info().stop_reason.find(L"recenter") != std::wstring::npos,
          "recenter reason persisted");
    reader.close();
    auto cancel = base;
    cancel += L".cancel";
    f.sequence = 1;
    f.display_time = 1000000000LL;
    check(r.start(device, cancel, f) && wait_prepared(r).state == CaptureState::recording,
          "cancel preparation");
    r.submit(f, textures);
    r.stop();
    r.cancel();
    status = wait_finished(r);
    check(status.state == CaptureState::cancelled && !std::filesystem::exists(cancel),
          "cancellation leaves no committed clip");
    auto mono = base;
    mono += L".mono";
    f.view_count = 1;
    check(r.start(device, mono, f) && wait_prepared(r).state == CaptureState::recording,
          "one-view preparation");
    r.submit(f, textures);
    r.stop();
    status = wait_finished(r);
    check(status.state == CaptureState::ready && reader.open(device, mono),
          "one-view clip recorded and opened");
    DecodedFrame decoded;
    check(reader.next(decoded) && decoded.frame.view_count == 1 && decoded.textures[0] &&
              !decoded.textures[1],
          "one-view decode has no invented focus image");
    reader.close();
    std::error_code ec;
    for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) {
        auto srgb = base;
        srgb += L".srgb-" + std::to_wstring(unsigned(format));
        f.views[0].format = format;
        D3D11_TEXTURE2D_DESC desc{};
        textures[0]->GetDesc(&desc);
        desc.Format = format;
        std::vector<std::uint32_t> srgb_pixels(
            desc.Width * desc.Height, format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? 0xff2090e0u : 0xffe09020u);
        D3D11_SUBRESOURCE_DATA srgb_data{srgb_pixels.data(), desc.Width * 4, 0};
        Microsoft::WRL::ComPtr<ID3D11Texture2D> srgb_texture;
        device->CreateTexture2D(&desc, &srgb_data, &srgb_texture);
        ID3D11Texture2D *srgb_views[2]{srgb_texture.Get(), nullptr};
        const auto srgb_started =
            r.start(device, srgb, f) && wait_prepared(r).state == CaptureState::recording;
        check(srgb_started, "sRGB OpenXR source recording supported");
        if (srgb_started) {
            r.submit(f, srgb_views);
            r.stop();
            check(wait_finished(r).state == CaptureState::ready, "sRGB OpenXR source clip saved");
            check(reader.open(device, srgb) && reader.next(decoded), "sRGB source decodes");
            if (decoded.textures[0]) {
                D3D11_TEXTURE2D_DESC d{};
                decoded.textures[0]->GetDesc(&d);
                d.Usage = D3D11_USAGE_STAGING;
                d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                d.BindFlags = 0;
                Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
                device->CreateTexture2D(&d, nullptr, &staging);
                Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
                device->GetImmediateContext(&context);
                context->CopyResource(staging.Get(), decoded.textures[0].Get());
                D3D11_MAPPED_SUBRESOURCE map{};
                if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
                    auto p = static_cast<const unsigned char *>(map.pData) + map.RowPitch * (d.Height / 2) +
                             (d.Width / 2) * 4;
                    check(std::abs(int(p[2]) - 224) < 15 && std::abs(int(p[1]) - 144) < 15,
                          "sRGB GPU copy preserves SDR code values");
                    context->Unmap(staging.Get(), 0);
                } else
                    check(false, "sRGB test readback");
            }
            reader.close();
        } else
            std::wcout << L"sRGB preparation: " << r.status().message << L'\n';
        std::filesystem::remove_all(srgb, ec);
    }
    for (const auto &path : {pressure, recenter, cancel, mono})
        std::filesystem::remove_all(path, ec);
}
void hardware_test(bool large, bool keep, bool padded = false) {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    auto hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
    check(SUCCEEDED(hr), "hardware D3D11 device available");
    if (FAILED(hr))
        return;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapter_desc{};
    device.As(&dxgi);
    dxgi->GetAdapter(&adapter);
    adapter->GetDesc(&adapter_desc);
    std::wcout << L"Hardware adapter: " << adapter_desc.Description << L'\n';
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, 32768);
    auto path = std::filesystem::path(executable).parent_path() /
                (L"steadyxr-hardware-clip-" + std::to_wstring(GetCurrentProcessId()) + L".sxclip");
    auto f = source();
    const unsigned source_width = padded ? 2048 : (large ? 2560 : 320);
    const unsigned source_height = padded ? 964 : (large ? 1920 : 240);
    for (auto &view : f.views) {
        view.width = view.rect_width = source_width;
        view.height = view.rect_height = source_height;
    }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> textures[2];
    std::vector<std::uint32_t> pixels(source_width * source_height);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = source_width;
    desc.Height = source_height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    for (auto &texture : textures)
        check(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)), "test source texture");
    ClipRecorder recorder;
    CaptureOptions capture;
    capture.fps = padded ? 60 : 30;
    check(recorder.start(device.Get(), path, f, capture), "hardware recorder starts");
    for (int n = 0; n < 500 && recorder.status().state == CaptureState::preparing; ++n)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto status = recorder.status();
    std::wcout << L"Hardware preparation: " << status.message << L'\n';
    check(status.state == CaptureState::recording, "verified hardware encoder ready");
    if (status.state != CaptureState::recording)
        return;
    for (int n = 0; n < 18; ++n) {
        f.sequence = n + 1;
        // Real OpenXR times are not aligned to the MP4 track clock. Keep the
        // original pose times while exercising rounding in both video views.
        f.display_time = 1000000000LL + n * 40000000LL + (n >= 9 ? 200000000LL : 0) +
                         (n % 3) * 7913LL;
        f.views[0].orientation[1] = float(n * .001);
        f.gaze.flags = vrst::GazeValid;
        f.gaze.sample_time_ns = f.display_time;
        for (unsigned view = 0; view < 2; ++view) {
            const unsigned intensity = 80 + n * 7;
            std::fill(pixels.begin(), pixels.end(),
                      view ? 0xff002020u + (intensity << 8) : 0xff202000u + intensity);
            // Distinct blue corners must survive the full-source proxy. A crop
            // that accidentally uses the live 16:9 framing removes these marks.
            for (unsigned y = 0; y < 32; ++y)
                for (unsigned x = 0; x < 32; ++x) {
                    pixels[y * source_width + x] = 0xffdd2020u;
                    pixels[(source_height - 1 - y) * source_width + (source_width - 1 - x)] = 0xffdd2020u;
                }
            context->UpdateSubresource(textures[view].Get(), 0, nullptr, pixels.data(), source_width * 4, 0);
        }
        ID3D11Texture2D *raw[2]{textures[0].Get(), textures[1].Get()};
        const bool submitted = recorder.submit(f, raw, 90 + n);
        if (!submitted)
            std::wcout << L"Submission " << n << L": " << recorder.status().message << L'\n';
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    recorder.stop();
    for (int n = 0; n < 1000 && recorder.status().state == CaptureState::finishing; ++n)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    status = recorder.status();
    std::wcout << L"Hardware completion: " << status.message << L"; frames=" << status.recorded_frames
               << L" drops=" << status.dropped_frames << L'\n';
    check(status.state == CaptureState::ready && status.recorded_frames >= 8, "hardware clip finalized");
    if (status.state != CaptureState::ready)
        return;
    ClipReader reader;
    check(reader.open(device.Get(), path), "hardware encoded clip opens");
    std::wcout << L"Reader: " << reader.error() << L'\n';
    check(reader.count() == status.recorded_frames, "video metadata count");
    check(reader.info().width[0] == (large || padded ? 2048u : 320u) &&
              reader.info().height[0] == (padded ? 964u : (large ? 1536u : 240u)),
          "full FOV proxy preserves source aspect without decoder padding");
    DecodedFrame decoded;
    for (std::size_t i = 0; i < reader.count(); ++i) {
        if (!reader.next(decoded)) {
            std::wcout << reader.error() << L'\n';
            check(false, "decode synchronized views");
            break;
        }
        check(decoded.frame.sequence == reader.metadata(i)->frame.sequence && decoded.index == i,
              "pixel pose index match");
        check(decoded.frame.views[0].texture_handle == 0 && decoded.frame.views[1].texture_handle == 0,
              "no replay handles");
        check(decoded.frame.view_count == 2 && decoded.textures[1], "focus view retained");
        check(decoded.speed_kph == 90 + decoded.frame.sequence - 1, "telemetry match");
        check(std::abs(decoded.frame.views[0].orientation[1] - float((decoded.frame.sequence - 1) * .001)) <
                      1e-6 &&
                  decoded.frame.gaze.sample_time_ns == decoded.frame.display_time,
              "orientation and gaze stay with exact recorded pixels");
        for (unsigned view = 0; view < 2; ++view) {
            D3D11_TEXTURE2D_DESC d{};
            decoded.textures[view]->GetDesc(&d);
            d.Usage = D3D11_USAGE_STAGING;
            d.BindFlags = 0;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
            device->CreateTexture2D(&d, nullptr, &staging);
            context->CopyResource(staging.Get(), decoded.textures[view].Get());
            D3D11_MAPPED_SUBRESOURCE map{};
            if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
                auto p = static_cast<const unsigned char *>(map.pData) + map.RowPitch * (d.Height / 2) +
                         (d.Width / 2) * 4;
                const int expected = 80 + int(decoded.frame.sequence - 1) * 7 + (view ? 32 : 0);
                check(std::abs(int(p[view ? 1 : 2]) - expected) < 15,
                      "encoded frame pixels match exact source sequence");
                for (unsigned corner = 0; corner < 2; ++corner) {
                    auto edge = static_cast<const unsigned char *>(map.pData) +
                                map.RowPitch * (corner ? d.Height - 8 : 8) + (corner ? d.Width - 8 : 8) * 4;
                    check(edge[0] > edge[2] + 100, "full source corners retained (no live crop)");
                }
                context->Unmap(staging.Get(), 0);
            } else
                check(false, "test readback");
        }
    }
    check(reader.seek(2) && reader.next(decoded) && decoded.index == 2, "indexed deterministic seek");
    auto original_info = reader.info();
    reader.close();
    if (original_info.frames.size() > 2) {
        auto mismatched = original_info;
        mismatched.frames[1].media_time += 100000; // 10 ms, not container rounding.
        mismatched.frames[1].frame.display_time += 10000000;
        std::wstring error;
        check(write_clip_metadata(path / L"metadata.bin", mismatched, error),
              "write structurally valid but mispaired pose time");
        check(!reader.open(device.Get(), path), "wrong image/pose pairing rejected despite timestamp rounding");
        reader.close();
        check(write_clip_metadata(path / L"metadata.bin", original_info, error), "restore fixture metadata");
    }
    // A modified video cannot be paired with the retained poses.
    const auto video = path / L"context.mp4";
    {
        std::ofstream out(video, std::ios::binary | std::ios::app);
        out.put('x');
    }
    check(!reader.open(device.Get(), path), "video checksum mismatch rejected");
    std::filesystem::resize_file(video, std::filesystem::file_size(video) - 1);
    if (!large && !padded) {
        hardware_lifetime_cases(device.Get(), path);
        ID3D11Texture2D *raw[2]{textures[0].Get(), textures[1].Get()};
        hardware_edge_cases(device.Get(), raw, source(), path);
    }
    if (keep) {
        std::wcout << L"Verified clip fixture: " << path.wstring() << L'\n';
        return;
    }
    // This unique test directory is the only deletion target.
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}
int main(int argc, char **argv) {
    std::wstring error;
    auto info = valid_info();
    check(validate_clip_metadata(info, error), "valid two-view metadata");
    auto sixty = info;
    sixty.options.duration_seconds = 60;
    check(validate_clip_metadata(sixty, error), "60-second workshop option supported");
    sixty.options.duration_seconds = 61;
    check(!validate_clip_metadata(sixty, error), "61-second duration rejected");
    sixty = info;
    sixty.options.duration_seconds = 60;
    sixty.options.fps = 60;
    sixty.frames.resize(3600, info.frames.front());
    for (std::size_t i = 0; i < sixty.frames.size(); ++i) {
        auto &sample = sixty.frames[i];
        sample.frame.sequence = i + 1;
        sample.frame.display_time = info.frames.front().frame.display_time + std::int64_t(i) * 16666667;
        sample.media_time = (sample.frame.display_time - info.frames.front().frame.display_time) / 100;
    }
    check(validate_clip_metadata(sixty, error), "60 seconds at 60 FPS fits bounded metadata");
    auto oversized = sixty;
    oversized.frames.push_back(sixty.frames.back());
    check(!validate_clip_metadata(oversized, error), "3601 frames rejected");
    auto header = sps(20, 15);
    check(clip_detail::bounded_h264_sps(header.data(), header.size(), 320, 240),
          "valid bounded H.264 sequence header");
    header = sps(1024, 1024);
    check(!clip_detail::bounded_h264_sps(header.data(), header.size(), 320, 240),
          "H.264 allocation dimensions cannot override metadata");
    header = sps(20, 15, 1000);
    check(!clip_detail::bounded_h264_sps(header.data(), header.size(), 320, 240),
          "H.264 reference buffer bound");
    header = {0x67, 77};
    check(!clip_detail::bounded_h264_sps(header.data(), header.size(), 320, 240),
          "truncated H.264 SPS rejected before decoding");
    auto bad = info;
    bad.frames[1].media_time = 0;
    check(!validate_clip_metadata(bad, error), "duplicate media timestamp rejected");
    bad = info;
    bad.frames[1].frame.display_time += 10000;
    check(!validate_clip_metadata(bad, error), "image/source timestamp mismatch rejected");
    bad = info;
    bad.frames[1].frame.views[1].orientation[0] = std::numeric_limits<float>::quiet_NaN();
    check(!validate_clip_metadata(bad, error), "nonfinite focus pose rejected");
    bad = info;
    bad.width[0] = 1000000;
    check(!validate_clip_metadata(bad, error), "unbounded video dimension rejected");
    bad = info;
    bad.frames[0].frame.views[0].texture_handle = 123;
    check(!validate_clip_metadata(bad, error), "persistent shared handle rejected");
    bad = info;
    bad.frames[1].frame.reference_epoch++;
    check(!validate_clip_metadata(bad, error), "reference change rejected");
    bad = info;
    bad.frames[1].frame.sequence = bad.frames[0].frame.sequence;
    check(!validate_clip_metadata(bad, error), "duplicate source sequence rejected");
    bad = info;
    bad.frames[1].frame.overlay_count = 1;
    check(!validate_clip_metadata(bad, error), "unrecorded overlay rejected explicitly");
    bad = info;
    bad.frames.resize(1801);
    check(!validate_clip_metadata(bad, error), "frame-count bound");
    auto f = source(), changed = f;
    changed.session++;
    check(!compatible_clip_source(f, changed, error), "session discontinuity");
    changed = f;
    changed.views[1].rect_width--;
    check(!compatible_clip_source(f, changed, error), "focus size discontinuity");
    changed = f;
    changed.views[0].fov[0] = -0.75f;
    check(compatible_clip_source(f, changed, error), "per-frame FOV permitted");
    const auto path = std::filesystem::temp_directory_path() /
                      (L"steadyxr-clip-test-" + std::to_wstring(GetCurrentProcessId()) + L".bin");
    check(write_clip_metadata(path, info, error), "metadata write");
    ClipInfo loaded;
    check(read_clip_metadata(path, loaded, error), "metadata read");
    check(loaded.frames.size() == 3 && loaded.frames.back().frame.sequence == 3,
          "metadata exact association");
    check(write_clip_metadata(path, sixty, error) && read_clip_metadata(path, loaded, error) &&
              loaded.frames.size() == 3600,
          "maximum 60-second clip metadata roundtrip stays within file allocation limit");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "not a clip";
    }
    check(!read_clip_metadata(path, loaded, error), "truncated/corrupt metadata rejected");
    std::filesystem::remove(path);
    if (argc > 1 && (std::string(argv[1]) == "--hardware" || std::string(argv[1]) == "--hardware-large" ||
                    std::string(argv[1]) == "--hardware-padded"))
        hardware_test(std::string(argv[1]) == "--hardware-large",
                      argc > 2 && std::string(argv[2]) == "--keep", std::string(argv[1]) == "--hardware-padded");
    std::cout << "Workshop metadata tests: " << (failures ? "FAILED" : "passed") << '\n';
    return failures ? 1 : 0;
}

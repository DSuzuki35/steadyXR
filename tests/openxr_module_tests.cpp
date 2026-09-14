#include "../preview/transport.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <d3d11.h>
#include <dxgi.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <obs-module.h>
#include <stdexcept>
#include <thread>
#include <vector>
#include <wrl/client.h>

namespace {
using Microsoft::WRL::ComPtr;
using namespace vrst::preview;
void check(bool pass, const char *message) {
    std::printf("%s: %s\n", pass ? "PASS" : "FAIL", message);
    if (!pass)
        throw std::runtime_error(message);
}
struct Capture {
    obs_source_t *source{};
    unsigned width{}, height{}, bright{}, black{};
    unsigned red{}, green{}, blue{}, alpha{};
    std::uint64_t checksum{};
    bool good{};
};
void capture(void *data) {
    auto &c = *static_cast<Capture *>(data);
    obs_enter_graphics();
    c.width = obs_source_get_width(c.source);
    c.height = obs_source_get_height(c.source);
    if (!c.width || !c.height) {
        obs_leave_graphics();
        return;
    }
    auto *target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    auto *stage = gs_stagesurface_create(c.width, c.height, GS_RGBA);
    if (gs_texrender_begin(target, c.width, c.height)) {
        vec4 zero{};
        gs_clear(GS_CLEAR_COLOR, &zero, 0, 0);
        gs_ortho(0, float(c.width), 0, float(c.height), -100, 100);
        gs_matrix_identity();
        gs_set_cull_mode(GS_NEITHER);
        gs_enable_blending(false);
        obs_source_video_render(c.source);
        gs_texrender_end(target);
        gs_stage_texture(stage, gs_texrender_get_texture(target));
        uint8_t *pixels{};
        uint32_t stride{};
        if (gs_stagesurface_map(stage, &pixels, &stride)) {
            const auto *center = pixels + (c.height / 4) * stride + (c.width / 4) * 4;
            c.red = center[0];
            c.green = center[1];
            c.blue = center[2];
            c.alpha = center[3];
            for (unsigned y = 0; y < c.height; ++y)
                for (unsigned x = 0; x < c.width; ++x) {
                    const auto *p = pixels + y * stride + x * 4;
                    c.bright += p[0] > 180 && p[1] > 180 && p[2] > 180;
                    c.black += p[0] < 3 && p[1] < 3 && p[2] < 3 && p[3] == 255;
                    c.checksum = c.checksum * 131 + p[0] + 257 * p[1] + 65537 * p[2] + p[3];
                }
            c.good = true;
            gs_stagesurface_unmap(stage);
        }
    }
    gs_stagesurface_destroy(stage);
    gs_texrender_destroy(target);
    obs_leave_graphics();
}
Capture frame(obs_source_t *source) {
    Capture c;
    c.source = source;
    obs_queue_task(OBS_TASK_GRAPHICS, capture, &c, true);
    return c;
}
Capture until(obs_source_t *source, const std::function<bool(const Capture &)> &predicate) {
    Capture c;
    const auto deadline = GetTickCount64() + 1800;
    do {
        c = frame(source);
        if (c.good && predicate(c))
            return c;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (GetTickCount64() < deadline);
    return c;
}
bool waiting(const Capture &c) {
    return c.good && c.black > c.width * c.height * 9 / 10 && c.bright > 30;
}
int srgb(unsigned v) {
    const double linear = v / 255.;
    return int(
        std::lround(255 * (linear <= .0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1. / 2.4) - .055)));
}
bool color(const Capture &c, unsigned r, unsigned g, unsigned b) {
    return c.good && std::abs(int(c.red) - srgb(r)) <= 3 && std::abs(int(c.green) - srgb(g)) <= 3 &&
           std::abs(int(c.blue) - srgb(b)) <= 3 && c.alpha == 255;
}
obs_hotkey_id hotkey(obs_source_t *source, const char *name) {
    struct Search {
        obs_source_t *source;
        const char *name;
        obs_hotkey_id id{OBS_INVALID_HOTKEY_ID};
    } search{source, name};
    obs_enum_hotkeys(
        [](void *data, obs_hotkey_id id, obs_hotkey_t *key) {
            auto &s = *static_cast<Search *>(data);
            if (obs_hotkey_get_registerer_type(key) == OBS_HOTKEY_REGISTERER_SOURCE &&
                obs_weak_source_references_source(
                    static_cast<obs_weak_source_t *>(obs_hotkey_get_registerer(key)), s.source) &&
                std::strcmp(obs_hotkey_get_name(key), s.name) == 0)
                s.id = id;
            return true;
        },
        &search);
    return search.id;
}
void press(obs_source_t *source, const char *name) {
    const auto id = hotkey(source, name);
    check(id != OBS_INVALID_HOTKEY_ID, "requested source hotkey is registered");
    obs_hotkey_trigger_routed_callback(id, true);
    obs_hotkey_trigger_routed_callback(id, false);
}
bool eventually(const std::function<bool()> &predicate, unsigned timeout = 1800) {
    const auto deadline = GetTickCount64() + timeout;
    do {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (GetTickCount64() < deadline);
    return false;
}
std::string status(obs_source_t *source) {
    auto *properties = obs_source_properties(source);
    auto *value = obs_properties_get(properties, "capture_status");
    const std::string text = value ? obs_property_description(value) : "";
    obs_properties_destroy(properties);
    return text;
}
struct PropertyRefreshProbe {
    obs_source_t *source;
    std::atomic<unsigned> count{};
    explicit PropertyRefreshProbe(obs_source_t *s) : source(s) {
        signal_handler_connect(obs_source_get_signal_handler(source), "update_properties", refresh, this);
    }
    ~PropertyRefreshProbe() {
        disconnect();
    }
    void disconnect() {
        if (source)
            signal_handler_disconnect(obs_source_get_signal_handler(source), "update_properties", refresh,
                                      this);
        source = nullptr;
    }
    static void refresh(void *data, calldata_t *call) {
        auto *source = static_cast<obs_source_t *>(calldata_ptr(call, "source"));
        auto *p = obs_source_properties(source);
        if (obs_properties_get(p, "capture_status"))
            ++static_cast<PropertyRefreshProbe *>(data)->count;
        obs_properties_destroy(p);
    }
};
struct Producer {
    Channel channel;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    LUID adapter{};
    std::vector<ComPtr<ID3D11Texture2D>> retained;
    std::uint64_t sequence{};
    static void initialize(void *data) {
        auto &p = *static_cast<Producer *>(data);
        obs_enter_graphics();
        auto *obs_device = static_cast<ID3D11Device *>(gs_get_device_obj());
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        HRESULT hr = obs_device->QueryInterface(IID_PPV_ARGS(&dxgi));
        if (SUCCEEDED(hr))
            hr = dxgi->GetAdapter(&adapter);
        if (SUCCEEDED(hr))
            hr = adapter->GetDesc(&desc);
        if (SUCCEEDED(hr))
            hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &p.device, nullptr, &p.context);
        p.adapter = desc.AdapterLuid;
        obs_leave_graphics();
        check(SUCCEEDED(hr), "synthetic producer uses an independent device on the OBS adapter");
    }
    void lock() {
        const auto until = GetTickCount64() + 1000;
        while (!channel.lock()) {
            if (GetTickCount64() > until)
                throw std::runtime_error("isolated channel mutex timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    void reconnect() {
        lock();
        channel.data()->ready = 2;
        channel.unlock();
    }
    unsigned eye() {
        lock();
        const auto e = channel.data()->eye;
        channel.unlock();
        return e;
    }
    bool active() {
        lock();
        const bool a = viewer_active(*channel.data(), GetTickCount64());
        channel.unlock();
        return a;
    }
    unsigned ready() {
        lock();
        const auto r = channel.data()->ready;
        channel.unlock();
        return r;
    }
    void publish(unsigned r, unsigned g, unsigned b, unsigned w = 512, unsigned h = 512,
                 bool mismatch = false, bool invalid_handle = false, double roll = 0, bool pattern = false) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        ComPtr<ID3D11Texture2D> texture;
        check(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)),
              "create synthetic shared game frame");
        ComPtr<IDXGIKeyedMutex> keyed;
        ComPtr<IDXGIResource> resource;
        texture.As(&keyed);
        texture.As(&resource);
        HANDLE handle{};
        resource->GetSharedHandle(&handle);
        check(keyed->AcquireSync(0, 0) == S_OK, "synthetic producer owns writer key");
        std::vector<std::uint32_t> pixels(std::size_t(w) * h, 0xff000000u | (b << 16) | (g << 8) | r);
        if (pattern)
            for (unsigned y = 0; y < h; ++y)
                for (unsigned x = 0; x < w; ++x)
                    pixels[std::size_t(y) * w + x] = 0xff400000u | ((255 * y / h) << 8) | (255 * x / w);
        context->UpdateSubresource(texture.Get(), 0, nullptr, pixels.data(), w * 4, 0);
        context->Flush();
        check(keyed->ReleaseSync(1) == S_OK, "synthetic producer publishes reader key");
        lock();
        auto &s = *channel.data();
        Frame f{};
        f.generation = f.sequence = ++sequence;
        f.session = f.reference_epoch = 1;
        f.display_time = 1000000000LL + std::int64_t(sequence) * 16666667;
        f.published_ms = GetTickCount64();
        f.producer_pid = GetCurrentProcessId();
        f.view_count = 1;
        f.eye = s.eye;
        f.adapter = adapter;
        if (mismatch)
            f.adapter.LowPart ^= 0x1000;
        auto &v = f.views[0];
        v.texture_handle = reinterpret_cast<std::uint64_t>(handle);
        if (invalid_handle)
            v.texture_handle = 1;
        v.width = v.rect_width = w;
        v.height = v.rect_height = h;
        v.format = desc.Format;
        // Exact identity view pose with symmetric tangents; pixels and metadata
        // are published in one channel critical section, as by the OpenXR layer.
        v.orientation[0] = v.orientation[1] = 0;
        v.orientation[2] = float(std::sin(roll / 2));
        v.orientation[3] = float(std::cos(roll / 2));
        v.fov[0] = v.fov[3] = -1;
        v.fov[1] = v.fov[2] = 1;
        s.frame = f;
        s.ready = 1;
        s.producer_pid = f.producer_pid;
        s.producer_heartbeat_ms = f.published_ms;
        channel.unlock();
        retained.push_back(texture);
    }
};
} // namespace
int main(int argc, char **argv) {
    if (argc != 5)
        return 2;
    const auto isolated = L"Local\\SteadyXR-OBS-module-test-" + std::to_wstring(GetCurrentProcessId());
    SetEnvironmentVariableW(L"HORIZONLOCK_PREVIEW_CHANNEL", isolated.c_str());
    const std::string obs_root = argv[3], graphics = obs_root + "/bin/64bit/libobs-d3d11.dll";
    std::filesystem::create_directories(argv[4]);
    if (!obs_startup("en-US", argv[4], nullptr))
        return 2;
    obs_hotkey_enable_callback_rerouting(true);
    obs_add_data_path((obs_root + "/data/libobs/").c_str());
    obs_video_info video{};
    video.graphics_module = graphics.c_str();
    video.base_width = video.output_width = 640;
    video.base_height = video.output_height = 360;
    video.fps_num = 60;
    video.fps_den = 1;
    video.output_format = VIDEO_FORMAT_RGBA;
    video.colorspace = VIDEO_CS_709;
    video.range = VIDEO_RANGE_FULL;
    video.scale_type = OBS_SCALE_BILINEAR;
    if (obs_reset_video(&video) != OBS_VIDEO_SUCCESS) {
        obs_shutdown();
        return 2;
    }
    obs_source_t *source{}, *second{};
    obs_data_t *settings{};
    int result = 0;
    try {
        Producer producer;
        check(producer.channel.open(false, isolated), "fixture opens an isolated channel");
        obs_queue_task(OBS_TASK_GRAPHICS, Producer::initialize, &producer, true);
        obs_module_t *module{};
        check(obs_open_module(&module, argv[1], argv[2]) == MODULE_SUCCESS && obs_init_module(module),
              "native plugin loads under real libobs");
        check((obs_get_source_output_flags("steadyxr_openxr_source") & OBS_SOURCE_VIDEO) != 0,
              "native OpenXR input registration is present");
        settings = obs_data_create();
        obs_data_set_int(settings, "output_width", 640);
        obs_data_set_int(settings, "output_height", 360);
        obs_data_set_double(settings, "crop", 0);
        obs_data_set_bool(settings, "stabilize", false);
        source = obs_source_create("steadyxr_openxr_source", "Isolated SteadyXR capture", settings, nullptr);
        check(source != nullptr, "registered native OpenXR input source creates");
        for (const auto *name :
             {"steadyxr.reset", "steadyxr.reset_horizon", "steadyxr.pause", "steadyxr.compare",
              "steadyxr.natural", "steadyxr.comfort", "steadyxr.strong", "steadyxr.preset_1",
              "steadyxr.preset_2", "steadyxr.preset_3"})
            check(hotkey(source, name) != OBS_INVALID_HOTKEY_ID,
                  "native source registers each assignable SteadyXR hotkey");
        obs_source_inc_showing(source);
        check(waiting(until(source, waiting)), "waiting output is black with readable status pixels");
        auto *properties = obs_source_properties(source);
        check(obs_properties_get(properties, "import") && obs_properties_get(properties, "comfort") &&
                  obs_properties_get(properties, "right_reserve_left") &&
                  obs_properties_get(properties, "reset_horizon") &&
                  obs_properties_get(properties, "compare") &&
                  obs_properties_get(properties, "preset_3_path") &&
                  status(source).find("FPS") != std::string::npos,
              "source exposes preset slots, comparison, horizon reset and compact capture status");
        obs_property_button_clicked(obs_properties_get(properties, "comfort"), source);
        auto *stored = obs_source_get_settings(source);
        check(obs_data_get_double(stored, "smoothing") == 60 && obs_data_get_double(stored, "crop") == 12 &&
                  obs_data_get_double(stored, "horizon_lock") == 50,
              "Stream Comfort button updates the source's native settings");
        obs_data_release(stored);
        press(source, "steadyxr.natural");
        stored = obs_source_get_settings(source);
        check(obs_data_get_double(stored, "smoothing") == 25 &&
                  obs_data_get_double(stored, "horizon_lock") == 25 &&
                  obs_data_get_int(stored, "output_width") == 640,
              "Natural hotkey changes tuning and preserves output geometry");
        obs_data_release(stored);
        press(source, "steadyxr.strong");
        stored = obs_source_get_settings(source);
        check(obs_data_get_double(stored, "smoothing") == 70 &&
                  obs_data_get_double(stored, "horizon_lock") == 85,
              "Strong leveling hotkey applies its tuning");
        obs_data_release(stored);
        press(source, "steadyxr.comfort");
        stored = obs_source_get_settings(source);
        check(obs_data_get_double(stored, "smoothing") == 60 && obs_data_get_double(stored, "crop") == 12,
              "Stream Comfort hotkey uses the same native source tuning as its button");
        obs_data_release(stored);
        const auto preset = std::filesystem::path(argv[4]) / "isolated-native-import.ini";
        const std::string ini =
            "[Preview]\nSmoothingTenths=427\nCropTenths=0\nStabilize=0\nOutputWidth=640\nOutputHeight=360\n["
            "RightEyeReserve]\nEnabled=1\nRightTenths=200\n[Render]\nEnabled=1\n";
        {
            std::ofstream file(preset, std::ios::binary);
            file << ini;
        }
        obs_data_set_string(settings, "import_path", preset.u8string().c_str());
        obs_data_set_string(settings, "preset_1_path", preset.u8string().c_str());
        obs_source_update(source, settings);
        obs_property_button_clicked(obs_properties_get(properties, "import"), source);
        stored = obs_source_get_settings(source);
        check(obs_data_get_double(stored, "smoothing") == 42.7 &&
                  obs_data_get_double(stored, "right_reserve_right") == 20,
              "native Import button applies literal preset values");
        obs_data_release(stored);
        press(source, "steadyxr.strong");
        press(source, "steadyxr.preset_1");
        stored = obs_source_get_settings(source);
        check(obs_data_get_double(stored, "smoothing") == 42.7 &&
                  obs_data_get_double(stored, "right_reserve_right") == 20 &&
                  std::string(obs_data_get_string(stored, "preset_1_path")) == preset.u8string(),
              "saved preset hotkey imports its configured INI and retains the slot path");
        obs_data_release(stored);
        press(source, "steadyxr.preset_2");
        stored = obs_source_get_settings(source);
        check(obs_data_get_double(stored, "smoothing") == 42.7,
              "unconfigured saved preset hotkey leaves current tuning unchanged");
        obs_data_release(stored);
        auto *saved = obs_save_source(source);
        auto *saved_settings = obs_data_get_obj(saved, "settings");
        check(obs_data_get_double(saved_settings, "smoothing") == 42.7 &&
                  obs_data_get_double(saved_settings, "right_reserve_right") == 20,
              "OBS scene serialization retains imported options without the preview app");
        obs_data_release(saved_settings);
        obs_data_release(saved);
        std::ifstream imported(preset, std::ios::binary);
        check(std::string((std::istreambuf_iterator<char>(imported)), {}) == ini,
              "native import leaves the fixture INI unchanged");
        obs_property_button_clicked(obs_properties_get(properties, "reset"), source);
        obs_property_button_clicked(obs_properties_get(properties, "reset_horizon"), source);
        press(source, "steadyxr.reset");
        press(source, "steadyxr.reset_horizon");
        obs_properties_destroy(properties);
        producer.publish(64, 128, 192);
        auto first = until(source, [](const auto &c) { return color(c, 64, 128, 192); });
        std::printf("First color: %u %u %u %u\n", first.red, first.green, first.blue, first.alpha);
        check(color(first, 64, 128, 192), "shared GPU frames preserve RGB and sRGB encoding through libobs");
        check(frame(source).checksum == first.checksum, "repeat render preserves the private output texture");
        check(status(source).find("Connected") != std::string::npos,
              "properties report an established OpenXR frame connection");
        // Opening/closing fresh properties on the real update signal catches
        // stale property storage and status-mutex reentrancy errors.
        PropertyRefreshProbe refreshes(source);
        properties = obs_source_properties(source);
        const auto before_button = refreshes.count.load();
        obs_property_button_clicked(obs_properties_get(properties, "reset_horizon"), source);
        obs_properties_destroy(properties);
        check(refreshes.count == before_button,
              "property buttons never synchronously rebuild the callback's owning OBS widget");
        stored = obs_source_get_settings(source);
        obs_data_set_bool(stored, "stabilize", true);
        obs_data_set_bool(stored, "adaptive_horizon", false);
        obs_data_set_bool(stored, "mirror", false);
        obs_data_set_double(stored, "horizon_lock", 100);
        obs_data_set_double(stored, "smoothing", 0);
        obs_source_update(source, stored);
        obs_data_release(stored);
        producer.publish(0, 0, 0, 512, 512, false, false, .35, true);
        const auto stabilized =
            until(source, [&](const auto &c) { return !waiting(c) && c.checksum != first.checksum; });
        properties = obs_source_properties(source);
        const auto compare_started = GetTickCount64();
        obs_property_button_clicked(obs_properties_get(properties, "compare"), source);
        obs_properties_destroy(properties);
        check(eventually([&] { return status(source).find("Comparison") != std::string::npos; }),
              "comparison button advertises its temporary original view in properties");
        const auto original = frame(source);
        const auto before_stream = refreshes.count.load();
        check(original.width == stabilized.width && original.height == stabilized.height &&
                  original.checksum != stabilized.checksum,
              "comparison bypasses real horizon correction while retaining output dimensions");
        saved = obs_save_source(source);
        saved_settings = obs_data_get_obj(saved, "settings");
        check(!obs_data_has_user_value(saved_settings, "compare_original") &&
                  obs_data_get_double(saved_settings, "horizon_lock") == 100 &&
                  obs_data_get_bool(saved_settings, "stabilize"),
              "temporary comparison never changes serialized tuning or persists its runtime flag");
        obs_data_release(saved_settings);
        obs_data_release(saved);
        auto republished = GetTickCount64();
        check(eventually(
                  [&] {
                      if (GetTickCount64() - republished >= 250) {
                          producer.publish(0, 0, 0, 512, 512, false, false, .35, true);
                          republished = GetTickCount64();
                      }
                      return status(source).find("Comparison") == std::string::npos;
                  },
                  21000) &&
                  GetTickCount64() - compare_started >= 19800 &&
                  frame(source).checksum == stabilized.checksum,
              "comparison automatically restores the corrected pixels after twenty seconds");
        check(refreshes.count == before_stream && status(source).find("0.0 FPS") == std::string::npos,
              "measured status is available without unsolicited property rebuilds during capture");
        properties = obs_source_properties(source);
        check(obs_property_button_clicked(obs_properties_get(properties, "refresh_status"), source) &&
                  std::string(obs_property_description(obs_properties_get(properties, "capture_status")))
                          .find("Connected") != std::string::npos &&
                  refreshes.count == before_stream,
              "explicit status refresh updates the snapshot without reentrant OBS reloads");
        obs_properties_destroy(properties);
        refreshes.disconnect();
        press(source, "steadyxr.preset_1");
        producer.publish(64, 128, 192);
        first = until(source, [](const auto &c) { return color(c, 64, 128, 192); });
        press(source, "steadyxr.compare");
        check(eventually([&] { return status(source).find("Comparison") != std::string::npos; }) &&
                  frame(source).checksum == first.checksum,
              "comparison hotkey adds no status pixels or framing changes to a bypassed source");
        press(source, "steadyxr.compare");
        check(eventually([&] { return status(source).find("Comparison") == std::string::npos; }, 500),
              "pressing the comparison action again returns immediately");
        producer.lock();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto contended = frame(source);
        producer.channel.unlock();
        check(contended.checksum == first.checksum,
              "transient channel mutex contention retains the current image");
        obs_data_set_int(settings, "output_width", 800);
        obs_data_set_int(settings, "output_height", 450);
        obs_source_update(source, settings);
        auto resized = until(source, [](const auto &c) { return c.width == 800 && color(c, 64, 128, 192); });
        check(resized.width == 800 && resized.height == 450 && color(resized, 64, 128, 192),
              "output resize opens the new shared generation");
        producer.publish(200, 32, 16, 768, 640);
        check(color(until(source, [](const auto &c) { return color(c, 200, 32, 16); }), 200, 32, 16),
              "new source dimensions and shared handle replace the old frame");
        obs_data_set_int(settings, "eye", 1);
        obs_source_update(source, settings);
        check(waiting(until(source, waiting)) && producer.eye() == 1,
              "eye change clears stale imagery and requests the new eye");
        producer.publish(16, 220, 32);
        check(color(until(source, [](const auto &c) { return color(c, 16, 220, 32); }), 16, 220, 32),
              "new selected eye resumes capture");
        second = obs_source_create("steadyxr_openxr_source", "Competing isolated capture", settings, nullptr);
        obs_source_inc_showing(second);
        check(waiting(until(second, waiting)),
              "second OBS source shows ownership conflict instead of stealing");
        producer.publish(32, 64, 220);
        check(color(until(source, [](const auto &c) { return color(c, 32, 64, 220); }), 32, 64, 220),
              "first source retains ownership while competitor is showing");
        obs_source_dec_showing(source);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        producer.publish(220, 64, 32);
        check(color(until(second, [](const auto &c) { return color(c, 220, 64, 32); }), 220, 64, 32),
              "hidden source releases ownership to the next active capture");
        obs_source_dec_showing(second);
        obs_source_release(second);
        second = nullptr;
        obs_source_inc_showing(source);
        check(waiting(until(source, waiting)), "reactivation waits for fresh frames");
        producer.publish(64, 128, 192);
        check(color(until(source, [](const auto &c) { return color(c, 64, 128, 192); }), 64, 128, 192),
              "capture resumes after ownership transfer and teardown");
        producer.reconnect();
        check(waiting(until(source, waiting)), "reconnect clears retained pixels immediately");
        producer.publish(64, 128, 192);
        check(color(until(source, [](const auto &c) { return color(c, 64, 128, 192); }), 64, 128, 192),
              "fresh generation recovers after reconnect");
        std::this_thread::sleep_for(std::chrono::milliseconds(2150));
        check(waiting(until(source, waiting)),
              "two-second timeout replaces stale game imagery with waiting status");
        producer.publish(64, 128, 192, 512, 512, true);
        const auto mismatch_deadline = GetTickCount64() + 1500;
        while (producer.ready() != 2 && GetTickCount64() < mismatch_deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        check(producer.ready() == 2 && waiting(until(source, waiting)),
              "adapter mismatch is rejected and shows an error card without cross-GPU reads");
        producer.publish(128, 64, 32);
        check(color(until(source, [](const auto &c) { return color(c, 128, 64, 32); }), 128, 64, 32),
              "capture recovers after adapter mismatch");
        producer.publish(64, 128, 192, 512, 512, false, true);
        const auto open_deadline = GetTickCount64() + 1500;
        while (producer.ready() != 2 && GetTickCount64() < open_deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        check(producer.ready() == 2 && waiting(until(source, waiting)),
              "failed game texture open clears stale imagery and requests new handles");
        producer.publish(32, 128, 64);
        check(color(until(source, [](const auto &c) { return color(c, 32, 128, 64); }), 32, 128, 64),
              "capture recovers from a failed texture open");
        press(source, "steadyxr.pause");
        check(waiting(until(source, waiting)) && !producer.active(),
              "pause hotkey shows status and releases the consumer");
        check(status(source).find("Paused") != std::string::npos,
              "properties distinguish pause from a disconnected capture");
        press(source, "steadyxr.preset_1");
        stored = obs_source_get_settings(source);
        check(obs_data_get_bool(stored, "paused"), "loading a saved preset preserves capture pause");
        obs_data_release(stored);
        press(source, "steadyxr.pause");
        check(eventually([&] { return producer.active(); }), "pause hotkey resumes the consumer");
        press(source, "steadyxr.pause");
        const auto removed_hotkey = hotkey(source, "steadyxr.reset_horizon");
        obs_source_dec_showing(source);
        obs_source_release(source);
        source = nullptr;
        obs_data_release(settings);
        settings = nullptr;
        obs_queue_task(OBS_TASK_GRAPHICS, [](void *) {}, nullptr, true);
        check(!producer.active(), "clean teardown leaves no active consumer");
        check(eventually([&] {
                  struct Search {
                      obs_hotkey_id id;
                      bool found{};
                  } search{removed_hotkey};
                  obs_enum_hotkeys(
                      [](void *data, obs_hotkey_id id, obs_hotkey_t *) {
                          auto &s = *static_cast<Search *>(data);
                          s.found = s.found || id == s.id;
                          return true;
                      },
                      &search);
                  return !search.found;
              }),
              "source destruction unregisters action callbacks before their state is freed");
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        result = 1;
    }
    if (second)
        obs_source_release(second);
    if (source)
        obs_source_release(source);
    if (settings)
        obs_data_release(settings);
    obs_shutdown();
    return result;
}

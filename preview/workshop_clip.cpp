#include "workshop_clip.hpp"
#include <algorithm>
#include <atomic>
#include <bcrypt.h>
#include <chrono>
#include <cmath>
#include <codecapi.h>
#include <condition_variable>
#include <d3d10.h>
#include <dxgi.h>
#include <evr.h>
#include <fstream>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <mutex>
#include <sstream>
#include <thread>

namespace vrst::preview {
using Microsoft::WRL::ComPtr;
namespace clip_detail {
// Clips use progressive 8-bit 4:2:0 H.264 Main. Check the actual SPS before
// creating a decoder: MP4 display dimensions alone do not bound its buffers.
bool bounded_h264_sps(const unsigned char *data, std::size_t size, unsigned width, unsigned height) {
    if (!data || size < 4 || size > 1024 || (data[0] & 0x9f) != 7)
        return false;
    std::vector<unsigned char> rbsp;
    rbsp.reserve(size);
    unsigned zeros = 0;
    for (std::size_t i = 1; i < size; ++i) {
        if (zeros >= 2 && data[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = data[i] ? 0 : zeros + 1;
        rbsp.push_back(data[i]);
    }
    struct Bits {
        const std::vector<unsigned char> &bytes;
        std::size_t at{};
        bool valid{true};
        unsigned read(unsigned count) {
            if (count > 32 || at + count > bytes.size() * 8) {
                valid = false;
                return 0;
            }
            unsigned n = 0;
            while (count--) {
                n = (n << 1) | ((bytes[at / 8] >> (7 - at % 8)) & 1);
                ++at;
            }
            return n;
        }
        unsigned ue() {
            unsigned zeros = 0;
            while (valid && !read(1)) {
                if (++zeros > 30) {
                    valid = false;
                    return 0;
                }
            }
            return valid ? ((1u << zeros) - 1) + read(zeros) : 0;
        }
    } bits{rbsp};
    const auto profile = bits.read(8);
    bits.read(8);
    bits.read(8);
    if ((profile != 66 && profile != 77) || bits.ue() > 31 || bits.ue() > 12)
        return false;
    const auto order = bits.ue();
    if (order == 0) {
        if (bits.ue() > 12)
            return false;
    } else if (order == 1) {
        bits.read(1);
        bits.ue();
        bits.ue();
        const auto cycle = bits.ue();
        if (cycle > 255)
            return false;
        for (unsigned i = 0; i < cycle; ++i)
            bits.ue();
    } else if (order != 2)
        return false;
    if (bits.ue() > 16)
        return false;
    bits.read(1);
    const auto w = bits.ue(), h = bits.ue();
    if (w >= 128 || h >= 128 || bits.read(1) != 1)
        return false;
    bits.read(1);
    unsigned left = 0, right = 0, top = 0, bottom = 0;
    if (bits.read(1)) {
        left = bits.ue();
        right = bits.ue();
        top = bits.ue();
        bottom = bits.ue();
    }
    const auto coded_width = (w + 1) * 16, coded_height = (h + 1) * 16;
    if (left > coded_width / 2 || right > coded_width / 2 || top > coded_height / 2 ||
        bottom > coded_height / 2)
        return false;
    return bits.valid && coded_width >= 2 * (left + right) && coded_height >= 2 * (top + bottom) &&
           coded_width - 2 * (left + right) == width && coded_height - 2 * (top + bottom) == height;
}
} // namespace clip_detail
namespace {
constexpr unsigned max_frames = 3600, max_edge = 2048, queue_slots = 4;
constexpr std::uint64_t max_video_bytes = 512ull * 1024 * 1024;
constexpr std::uint64_t max_metadata_bytes = 2ull * 1024 * 1024;
bool fail(std::wstring &error, const wchar_t *text) {
    error = text;
    return false;
}
std::wstring hr_text(const wchar_t *text, HRESULT hr) {
    std::wostringstream s;
    s << text << L" (0x" << std::hex << static_cast<unsigned long>(hr) << L")";
    return s.str();
}
bool valid_options(const CaptureOptions &o) {
    return o.duration_seconds >= 1 && o.duration_seconds <= 60 && (o.fps == 30 || o.fps == 60) &&
           o.longest_edge >= 64 && o.longest_edge <= max_edge;
}
bool sdr(std::uint32_t f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}
bool unit_quaternion(double x, double y, double z, double w) {
    const auto n = x * x + y * y + z * z + w * w;
    return std::isfinite(n) && n > .5 && n < 1.5;
}
bool source_valid(const Frame &f, std::wstring &error, bool persistent) {
    if (f.view_count < 1 || f.view_count > 2 || f.eye > 1 || !f.sequence || f.display_time <= 0)
        return fail(error, L"The clip has invalid source identity or timestamps.");
    if (f.overlay_count)
        return fail(
            error, L"Workshop recording does not support overlay images. Disable overlays before recording.");
    for (unsigned i = 0; i < f.view_count; ++i) {
        const auto &v = f.views[i];
        if (persistent && v.texture_handle)
            return fail(error, L"A clip cannot contain live shared texture handles.");
        if (!sdr(v.format))
            return fail(
                error,
                L"Workshop recording supports SDR RGBA/BGRA sources only; HDR recording is unavailable.");
        if (!v.width || !v.height || v.width > 16384 || v.height > 16384 || v.x < 0 || v.y < 0 ||
            v.rect_width < 2 || v.rect_height < 2 || std::uint64_t(v.x) + v.rect_width > v.width ||
            std::uint64_t(v.y) + v.rect_height > v.height)
            return fail(error, L"The clip source rectangle is invalid or too large.");
        if (!unit_quaternion(v.orientation[0], v.orientation[1], v.orientation[2], v.orientation[3]))
            return fail(error, L"The clip contains an invalid orientation.");
        for (auto n : v.fov)
            if (!std::isfinite(n) || std::abs(n) > 1000)
                return fail(error, L"The clip contains an invalid source FOV.");
        if (v.fov[0] >= v.fov[1] || v.fov[3] >= v.fov[2])
            return fail(error, L"The clip contains an inverted source FOV.");
    }
    const auto &g = f.gaze;
    if ((g.flags & ~7u) ||
        !unit_quaternion(g.reference_from_eye.x, g.reference_from_eye.y, g.reference_from_eye.z,
                         g.reference_from_eye.w) ||
        !std::isfinite(g.direction.x) || !std::isfinite(g.direction.y) || !std::isfinite(g.direction.z))
        return fail(error, L"The clip contains invalid gaze metadata.");
    const auto ray =
        g.direction.x * g.direction.x + g.direction.y * g.direction.y + g.direction.z * g.direction.z;
    if ((g.flags & GazeValid) && (ray < .5 || ray > 1.5))
        return fail(error, L"The clip contains an invalid gaze direction.");
    return true;
}
void strip_handles(Frame &f) {
    for (auto &v : f.views)
        v.texture_handle = 0;
    f.overlay_count = 0;
    for (auto &o : f.overlays)
        o = {};
}
template <class T> void put(std::ostream &s, const T &n) {
    s.write(reinterpret_cast<const char *>(&n), sizeof n);
}
template <class T> bool get(std::istream &s, T &n) {
    return bool(s.read(reinterpret_cast<char *>(&n), sizeof n));
}
template <class F> void fields(Frame &f, F value) {
    value(f.generation);
    value(f.sequence);
    value(f.session);
    value(f.reference_epoch);
    value(f.display_time);
    value(f.published_ms);
    value(f.producer_pid);
    value(f.view_count);
    value(f.eye);
    value(f.adapter.LowPart);
    value(f.adapter.HighPart);
    for (auto &v : f.views) {
        value(v.width);
        value(v.height);
        value(v.format);
        value(v.x);
        value(v.y);
        value(v.rect_width);
        value(v.rect_height);
        for (auto &n : v.orientation)
            value(n);
        for (auto &n : v.fov)
            value(n);
    }
    auto &g = f.gaze;
    value(g.reference_from_eye.x);
    value(g.reference_from_eye.y);
    value(g.reference_from_eye.z);
    value(g.reference_from_eye.w);
    value(g.direction.x);
    value(g.direction.y);
    value(g.direction.z);
    value(g.sample_time_ns);
    value(g.reference_epoch);
    value(g.flags);
}
std::filesystem::path video_path(const std::filesystem::path &folder, unsigned view) {
    return folder / (view ? L"focus.mp4" : L"context.mp4");
}
bool file_hash(const std::filesystem::path &path, std::uint64_t &size, std::array<unsigned char, 32> &digest,
               std::wstring &error) {
    std::error_code ec;
    size = std::filesystem::file_size(path, ec);
    if (ec || !size || size > max_video_bytes)
        return fail(error, L"The clip video is missing or exceeds its size limit.");
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return fail(error, L"SHA-256 is unavailable.");
    bool ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::ifstream in(path, std::ios::binary);
    std::array<unsigned char, 65536> block{};
    while (ok && in) {
        in.read(reinterpret_cast<char *>(block.data()), block.size());
        const auto n = in.gcount();
        if (n > 0)
            ok = BCryptHashData(hash, block.data(), ULONG(n), 0) >= 0;
    }
    ok = ok && in.eof() && BCryptFinishHash(hash, digest.data(), ULONG(digest.size()), 0) >= 0;
    if (hash)
        BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok || fail(error, L"The clip video could not be checked.");
}
struct MediaRuntime {
    HRESULT com{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    HRESULT mf{MFStartup(MF_VERSION, MFSTARTUP_FULL)};
    ~MediaRuntime() {
        if (SUCCEEDED(mf))
            MFShutdown();
        if (SUCCEEDED(com))
            CoUninitialize();
    }
    bool ok() const {
        return (SUCCEEDED(com) || com == RPC_E_CHANGED_MODE) && SUCCEEDED(mf);
    }
};
bool check_h264(const unsigned char *data, std::size_t size, unsigned width, unsigned height, bool &saw_sps) {
    if (!data || !size || size > 16u * 1024 * 1024)
        return false;
    auto nal = [&](const unsigned char *p, std::size_t n) {
        if (!n || (p[0] & 0x80))
            return false;
        const auto type = p[0] & 31;
        if (type == 7) {
            if (!clip_detail::bounded_h264_sps(p, n, width, height))
                return false;
            saw_sps = true;
        }
        // Scalable/multiview extensions are outside this bounded AVC proxy format.
        return type != 13 && type != 14 && type != 15 && type != 20;
    };
    auto prefix = [&](std::size_t p) -> unsigned {
        if (p + 3 <= size && data[p] == 0 && data[p + 1] == 0 && data[p + 2] == 1)
            return 3;
        if (p + 4 <= size && data[p] == 0 && data[p + 1] == 0 && data[p + 2] == 0 && data[p + 3] == 1)
            return 4;
        return 0;
    };
    if (prefix(0)) {
        std::size_t at = 0;
        while (at < size) {
            const auto start = prefix(at);
            if (!start)
                return false;
            at += start;
            auto end = at;
            while (end < size && !prefix(end))
                ++end;
            if (!nal(data + at, end - at))
                return false;
            at = end;
        }
        return true;
    }
    if (size >= 7 && data[0] == 1) {
        // AVCDecoderConfigurationRecord, with four-byte sample NAL lengths.
        if ((data[4] & 3) != 3)
            return false;
        std::size_t at = 6;
        unsigned count = data[5] & 31;
        for (unsigned pass = 0; pass < 2; ++pass) {
            for (unsigned i = 0; i < count; ++i) {
                if (at + 2 > size)
                    return false;
                const auto n = (unsigned(data[at]) << 8) | data[at + 1];
                at += 2;
                if (!n || n > size - at || !nal(data + at, n))
                    return false;
                at += n;
            }
            if (pass == 0) {
                if (at >= size)
                    return false;
                count = data[at++];
            }
        }
        return at == size;
    }
    std::size_t at = 0;
    while (at < size) {
        if (at + 4 > size)
            return false;
        std::uint32_t n = 0;
        for (unsigned i = 0; i < 4; ++i)
            n = (n << 8) | data[at++];
        if (!n || n > size - at || !nal(data + at, n))
            return false;
        at += n;
    }
    return true;
}
bool audit_compressed_video(IMFSourceReader *reader, IMFMediaType *native, const ClipInfo &info,
                            unsigned view, std::wstring &error) {
    bool saw_sps = false;
    UINT32 header_size{};
    if (SUCCEEDED(native->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &header_size))) {
        if (!header_size || header_size > 65536)
            return fail(error, L"The H.264 sequence header exceeds its supported limit.");
        std::vector<unsigned char> header(header_size);
        if (FAILED(native->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, header.data(), header_size, nullptr)) ||
            !check_h264(header.data(), header.size(), info.width[view], info.height[view], saw_sps))
            return fail(error, L"The H.264 sequence header does not match the bounded SDR proxy dimensions.");
    }
    auto hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, native);
    if (FAILED(hr))
        return fail(error, L"The compressed clip cannot be validated before decoding.");
    std::size_t count = 0;
    unsigned empty = 0;
    while (count <= info.frames.size()) {
        DWORD flags{}, stream{};
        LONGLONG time{};
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &time, &sample);
        if (FAILED(hr) || flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED))
            return fail(error, L"The compressed clip is malformed or changes format.");
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;
        if (!sample) {
            if (++empty > 8)
                return fail(error, L"The clip has too many empty video samples.");
            continue;
        }
        empty = 0;
        if (count == info.frames.size() || std::abs(time - info.frames[count].media_time) > 1)
            return fail(error,
                        L"The encoded image timestamps or frame count do not match the pose metadata.");
        ComPtr<IMFMediaBuffer> buffer;
        hr = sample->ConvertToContiguousBuffer(&buffer);
        DWORD length{};
        if (SUCCEEDED(hr))
            hr = buffer->GetCurrentLength(&length);
        if (FAILED(hr) || !length || length > 16u * 1024 * 1024)
            return fail(error, L"The encoded video sample exceeds its supported size.");
        BYTE *bytes{};
        hr = buffer->Lock(&bytes, nullptr, nullptr);
        if (FAILED(hr))
            return fail(error, L"The encoded video sample cannot be inspected.");
        const auto ok = check_h264(bytes, length, info.width[view], info.height[view], saw_sps);
        buffer->Unlock();
        if (!ok)
            return fail(error, L"The encoded H.264 source changes dimensions or is malformed.");
        ++count;
    }
    if (count != info.frames.size() || !saw_sps)
        return fail(error, L"The clip is truncated or has no validated H.264 sequence header.");
    PROPVARIANT start{};
    start.vt = VT_I8;
    start.hVal.QuadPart = 0;
    return SUCCEEDED(reader->SetCurrentPosition(GUID_NULL, start)) ||
           fail(error, L"The validated clip could not rewind.");
}
} // namespace

bool compatible_clip_source(const Frame &a, const Frame &b, std::wstring &reason) {
    if (!source_valid(b, reason, false))
        return false;
    if (a.session != b.session || a.reference_epoch != b.reference_epoch)
        return fail(reason, L"Recording stopped: the OpenXR session or reference space changed (recenter).");
    if (a.generation != b.generation || a.producer_pid != b.producer_pid || a.view_count != b.view_count ||
        a.eye != b.eye || a.adapter.LowPart != b.adapter.LowPart || a.adapter.HighPart != b.adapter.HighPart)
        return fail(reason, L"Recording stopped: the source, eye or graphics device changed.");
    for (unsigned i = 0; i < a.view_count; ++i) {
        const auto &x = a.views[i], &y = b.views[i];
        if (x.width != y.width || x.height != y.height || x.format != y.format || x.x != y.x || x.y != y.y ||
            x.rect_width != y.rect_width || x.rect_height != y.rect_height)
            return fail(reason, L"Recording stopped: source image size, rectangle or color format changed.");
    }
    return true;
}
bool validate_clip_metadata(const ClipInfo &info, std::wstring &error) {
    if (!valid_options(info.options) || info.frames.empty() || info.frames.size() > max_frames ||
        info.frames.size() > info.options.duration_seconds * info.options.fps ||
        info.stop_reason.size() > 512)
        return fail(error, L"The clip metadata exceeds its supported limits.");
    const auto &first = info.frames.front();
    for (unsigned i = 0; i < first.frame.view_count && i < 2; ++i) {
        if (info.width[i] < 2 || info.height[i] < 2 || info.width[i] > info.options.longest_edge ||
            info.height[i] > info.options.longest_edge || (info.width[i] & 1) || (info.height[i] & 1) ||
            info.video_bytes[i] > max_video_bytes)
            return fail(error, L"The clip proxy dimensions or video size are invalid.");
        const auto &v = first.frame.views[i];
        // NV12 requires even dimensions; only rounding by up to two pixels is permitted.
        if (v.rect_width > 0 && v.rect_height > 0 &&
            std::abs(double(info.width[i]) / info.height[i] - double(v.rect_width) / v.rect_height) >
                2.1 / info.height[i] + 2.1 * double(v.rect_width) / v.rect_height / info.height[i])
            return fail(error, L"The clip proxy does not preserve the full source aspect ratio.");
    }
    std::int64_t previous_time = -1;
    std::uint64_t previous_sequence = 0;
    for (const auto &sample : info.frames) {
        if (!source_valid(sample.frame, error, true) ||
            !compatible_clip_source(first.frame, sample.frame, error))
            return false;
        if (sample.media_time < 0 || sample.media_time <= previous_time ||
            sample.frame.sequence <= previous_sequence ||
            sample.frame.display_time < first.frame.display_time ||
            sample.media_time != (sample.frame.display_time - first.frame.display_time) / 100 ||
            sample.media_time >= std::int64_t(info.options.duration_seconds) * 10000000 ||
            !std::isfinite(sample.speed_kph) || sample.speed_kph < -1 || sample.speed_kph > 1500)
            return fail(error, L"The clip image timestamps, source sequence or telemetry do not match.");
        previous_time = sample.media_time;
        previous_sequence = sample.frame.sequence;
    }
    error.clear();
    return true;
}
bool write_clip_metadata(const std::filesystem::path &path, const ClipInfo &info, std::wstring &error) {
    if (!validate_clip_metadata(info, error))
        return false;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const std::uint64_t magic = 0x3150494c43525853ull;
    const std::uint32_t version = 1;
    put(out, magic);
    put(out, version);
    put(out, info.options.duration_seconds);
    put(out, info.options.fps);
    put(out, info.options.longest_edge);
    for (unsigned i = 0; i < 2; ++i) {
        put(out, info.width[i]);
        put(out, info.height[i]);
        put(out, info.video_bytes[i]);
        out.write(reinterpret_cast<const char *>(info.video_hash[i].data()), 32);
    }
    put(out, info.dropped_frames);
    put(out, std::uint32_t(info.partial));
    put(out, std::uint32_t(info.stop_reason.size()));
    for (auto c : info.stop_reason)
        put(out, std::uint16_t(c));
    put(out, std::uint32_t(info.frames.size()));
    for (auto sample : info.frames) {
        fields(sample.frame, [&](const auto &n) { put(out, n); });
        put(out, sample.speed_kph);
        put(out, sample.media_time);
    }
    out.flush();
    return bool(out) || fail(error, L"The clip metadata could not be saved.");
}
bool read_clip_metadata(const std::filesystem::path &path, ClipInfo &result, std::wstring &error) {
    result = {};
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > max_metadata_bytes || size < 160)
        return fail(error, L"The clip metadata is missing, truncated or too large.");
    std::ifstream in(path, std::ios::binary);
    ClipInfo info;
    std::uint64_t magic{};
    std::uint32_t version{}, partial{}, reason_length{}, count{};
    get(in, magic);
    get(in, version);
    if (magic != 0x3150494c43525853ull || version != 1)
        return fail(error, L"This clip format or version is not supported.");
    get(in, info.options.duration_seconds);
    get(in, info.options.fps);
    get(in, info.options.longest_edge);
    for (unsigned i = 0; i < 2; ++i) {
        get(in, info.width[i]);
        get(in, info.height[i]);
        get(in, info.video_bytes[i]);
        in.read(reinterpret_cast<char *>(info.video_hash[i].data()), 32);
    }
    get(in, info.dropped_frames);
    get(in, partial);
    get(in, reason_length);
    if (!in || partial > 1 || reason_length > 512)
        return fail(error, L"The clip metadata header is malformed.");
    info.partial = partial != 0;
    for (unsigned i = 0; i < reason_length; ++i) {
        std::uint16_t c{};
        get(in, c);
        info.stop_reason += wchar_t(c);
    }
    get(in, count);
    if (!in || !count || count > max_frames)
        return fail(error, L"The clip frame count exceeds the supported limit.");
    info.frames.resize(count);
    for (auto &sample : info.frames) {
        fields(sample.frame, [&](auto &n) { get(in, n); });
        get(in, sample.speed_kph);
        get(in, sample.media_time);
    }
    if (!in || in.peek() != std::char_traits<char>::eof())
        return fail(error, L"The clip metadata is truncated or has unexpected trailing data.");
    if (!validate_clip_metadata(info, error))
        return false;
    result = std::move(info);
    return true;
}
namespace {
struct CaptureSlot {
    // 0 free, 1 producer, 2 queued, 3 encoder. Release/acquire publishes metadata.
    std::atomic<unsigned> state{}, pending{};
    ComPtr<ID3D11Texture2D> producer[2], encoder[2];
    ComPtr<IDXGIKeyedMutex> producer_mutex[2], encoder_mutex[2];
    ComPtr<ID3D11VideoProcessorOutputView> output[2];
    HANDLE handles[2]{}; // Process-local sharing only; never written to disk.
    ClipFrame frame;
    void released() {
        if (pending.fetch_sub(1) == 1)
            state.store(0, std::memory_order_release);
    }
};
class SampleReleased final : public IMFAsyncCallback {
    std::atomic<ULONG> refs_{1};
    std::shared_ptr<CaptureSlot> slot_;
    unsigned view_{};

  public:
    SampleReleased(std::shared_ptr<CaptureSlot> slot, unsigned view) : slot_(std::move(slot)), view_(view) {}
    STDMETHODIMP QueryInterface(REFIID iid, void **out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFAsyncCallback)) {
            *out = static_cast<IMFAsyncCallback *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override {
        return ++refs_;
    }
    STDMETHODIMP_(ULONG) Release() override {
        auto n = --refs_;
        if (!n)
            delete this;
        return n;
    }
    STDMETHODIMP GetParameters(DWORD *, DWORD *) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP Invoke(IMFAsyncResult *) override {
        slot_->encoder_mutex[view_]->ReleaseSync(0);
        slot_->released();
        return S_OK;
    }
};
HRESULT make_media_type(ComPtr<IMFMediaType> &type, REFGUID subtype, unsigned width, unsigned height,
                        unsigned fps) {
    auto hr = MFCreateMediaType(&type);
    if (FAILED(hr))
        return hr;
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, subtype);
    MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    return S_OK;
}
struct Encoder {
    ComPtr<IMFSinkWriter> writer;
    DWORD stream{};
    void close() {
        ComPtr<IMFMediaSink> sink;
        if (writer)
            writer->GetServiceForStream(MF_SINK_WRITER_MEDIASINK, GUID_NULL, IID_PPV_ARGS(&sink));
        writer.Reset();
        if (sink)
            sink->Shutdown();
    }
    ~Encoder() {
        close();
    }
    bool initialize(const std::filesystem::path &path, IMFDXGIDeviceManager *manager, unsigned width,
                    unsigned height, unsigned fps, std::wstring &error) {
        ComPtr<IMFAttributes> attrs;
        MFCreateAttributes(&attrs, 4);
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        attrs->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, manager);
        // Keep Sink Writer backpressure enabled. Only this worker may block;
        // the producer always has a bounded, nonblocking four-slot queue.
        const wchar_t *stage = L"create MP4 writer";
        auto hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, attrs.Get(), &writer);
        ComPtr<IMFMediaType> output, input;
        if (SUCCEEDED(hr))
            hr = make_media_type(output, MFVideoFormat_H264, width, height, fps);
        if (SUCCEEDED(hr)) {
            output->SetUINT32(MF_MT_AVG_BITRATE,
                              std::min(24000000u, std::max(2000000u, width * height * fps / 5)));
            output->SetUINT32(MF_MT_MPEG2_PROFILE, 77); // Main, no HDR.
            stage = L"add H.264 stream";
            hr = writer->AddStream(output.Get(), &stream);
        }
        if (SUCCEEDED(hr))
            hr = make_media_type(input, MFVideoFormat_NV12, width, height, fps);
        if (SUCCEEDED(hr)) {
            stage = L"set NV12 input";
            hr = writer->SetInputMediaType(stream, input.Get(), nullptr);
        }
        if (SUCCEEDED(hr)) {
            stage = L"begin hardware encoding";
            hr = writer->BeginWriting();
        }
        if (FAILED(hr)) {
            error = hr_text((std::wstring(L"A hardware H.264 encoder could not ") + stage +
                             L"; no software fallback is used")
                                .c_str(),
                            hr);
            return false;
        }
        ComPtr<IMFSinkWriterEx> extended;
        if (FAILED(writer.As(&extended)))
            return fail(error, L"Windows cannot verify the recording encoder. Recording is unavailable.");
        bool hardware = false;
        for (DWORD i = 0; i < 8; ++i) {
            GUID category{};
            ComPtr<IMFTransform> transform;
            if (FAILED(extended->GetTransformForStream(stream, i, &category, &transform)))
                break;
            if (category != MFT_CATEGORY_VIDEO_ENCODER)
                continue;
            ComPtr<IMFAttributes> a;
            UINT32 len{};
            hardware = SUCCEEDED(transform->GetAttributes(&a)) &&
                       SUCCEEDED(a->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &len)) && len;
            if (!hardware)
                break;
            ComPtr<ICodecAPI> codec;
            if (SUCCEEDED(transform.As(&codec))) {
                VARIANT value{};
                value.vt = VT_BOOL;
                value.boolVal = VARIANT_TRUE;
                codec->SetValue(&CODECAPI_AVLowLatencyMode, &value);
                value.vt = VT_UI4;
                value.ulVal = 0;
                codec->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &value);
            }
        }
        if (!hardware) {
            writer.Reset();
            return fail(error, L"A verified hardware H.264 encoder is unavailable. Recording is disabled; "
                               L"software encoding is not used.");
        }
        return true;
    }
    bool write(const std::shared_ptr<CaptureSlot> &slot, unsigned view, unsigned fps, std::wstring &error) {
        auto hr = slot->encoder_mutex[view]->AcquireSync(1, 1000);
        if (hr != S_OK) {
            error = hr_text(L"Recording stopped: source GPU synchronization failed", hr);
            return false;
        }
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFTrackedSample> tracked;
        const wchar_t *stage = L"create tracked sample";
        hr = MFCreateTrackedSample(&tracked);
        if (SUCCEEDED(hr))
            hr = tracked.As(&sample);
        if (SUCCEEDED(hr)) {
            stage = L"wrap GPU surface";
            hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), slot->encoder[view].Get(), 0, FALSE,
                                           &buffer);
        }
        if (SUCCEEDED(hr)) {
            DWORD length{};
            hr = buffer->GetMaxLength(&length);
            if (SUCCEEDED(hr))
                hr = buffer->SetCurrentLength(length);
        }
        if (SUCCEEDED(hr))
            hr = sample->AddBuffer(buffer.Get());
        bool owns = false;
        if (SUCCEEDED(hr)) {
            ComPtr<IMFAsyncCallback> released;
            released.Attach(new SampleReleased(slot, view));
            slot->pending.fetch_add(1);
            stage = L"track GPU surface lifetime";
            hr = tracked->SetAllocator(released.Get(), nullptr);
            if (FAILED(hr))
                slot->pending.fetch_sub(1);
            else
                owns = true;
        }
        if (SUCCEEDED(hr))
            hr = sample->SetSampleTime(slot->frame.media_time);
        if (SUCCEEDED(hr))
            hr = sample->SetSampleDuration(10000000 / fps);
        if (SUCCEEDED(hr)) {
            stage = L"submit GPU sample";
            hr = writer->WriteSample(stream, sample.Get());
        }
        if (!owns)
            slot->encoder_mutex[view]->ReleaseSync(0);
        if (FAILED(hr)) {
            error = hr_text(
                (std::wstring(L"Recording stopped: hardware encoding could not ") + stage).c_str(), hr);
            return false;
        }
        return true;
    }
};
} // namespace
struct ClipRecorder::Impl {
    mutable std::mutex status_mutex;
    CaptureStatus status;
    std::atomic<CaptureState> state{CaptureState::idle};
    std::atomic<bool> cancelled{};
    std::atomic<unsigned> drops{};
    std::atomic<ULONGLONG> last_submit_ms{};
    std::atomic<std::int64_t> elapsed_time{};
    std::mutex commit_gate;
    std::thread worker;
    std::condition_variable wake;
    std::mutex wait_mutex;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VideoDevice> video;
    ComPtr<ID3D11VideoContext> video_context;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator[2];
    ComPtr<ID3D11VideoProcessor> processor[2];
    ComPtr<ID3D11Texture2D> input_texture[2];
    ComPtr<ID3D11Texture2D> srgb_copy[2];
    ComPtr<ID3D11VideoProcessorInputView> input_view[2];
    std::shared_ptr<CaptureSlot> slots[queue_slots];
    Frame source;
    ClipInfo info;
    std::filesystem::path destination, partial_path;
    std::wstring stop_reason;
    std::int64_t first_time{}, next_due{}, last_time{};
    std::uint64_t last_sequence{};
    unsigned submitted{};
    ~Impl() {
        cancelled = true;
        state = CaptureState::finishing;
        wake.notify_all();
        if (worker.joinable())
            worker.join();
    }
    void reap_terminal() {
        const auto current = state.load();
        if (current != CaptureState::ready && current != CaptureState::cancelled &&
            current != CaptureState::failed)
            return;
        // A terminal status can precede stack unwinding and MFShutdown. Inspect
        // the actual thread handle without waiting, rather than joining merely
        // because the worker has published its result.
        if (worker.joinable() && WaitForSingleObject(worker.native_handle(), 0) != WAIT_OBJECT_0)
            return;
        // Tracked samples may complete on Media Foundation callback threads.
        // pending guards GPU use, and sole ownership additionally proves the
        // callback object itself has released its retained surface references.
        for (const auto &slot : slots)
            if (slot && (slot->pending.load() != 0 || slot.use_count() != 1))
                return;
        if (worker.joinable())
            worker.join(); // The native thread handle is already signalled.
        for (unsigned i = 0; i < 2; ++i) {
            input_view[i].Reset();
            input_texture[i].Reset();
            srgb_copy[i].Reset();
            processor[i].Reset();
            enumerator[i].Reset();
        }
        for (auto &slot : slots)
            slot.reset();
        video_context.Reset();
        video.Reset();
        context.Reset();
        device.Reset();
        info = {}; // Release the bounded frame metadata allocation as well.
    }
    void set_state(CaptureState s, const std::wstring &message) {
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            status.state = s;
            status.message = message;
        }
        state.store(s);
    }
    bool preparation_failed(const std::wstring &error) {
        set_state(CaptureState::failed, error);
        return false;
    }
    bool prepare_gpu() {
        auto hr = device.As(&video);
        device->GetImmediateContext(&context);
        if (SUCCEEDED(hr))
            hr = context.As(&video_context);
        if (FAILED(hr))
            return preparation_failed(
                L"GPU video processing is unavailable. Workshop recording needs a hardware D3D11 device.");
        for (auto &slot : slots)
            slot = std::make_shared<CaptureSlot>();
        std::uint64_t conversion_bytes = 0;
        for (unsigned i = 0; i < source.view_count; ++i) {
            const auto &v = source.views[i];
            DXGI_FORMAT video_format = DXGI_FORMAT(v.format);
            if (video_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
                video_format = DXGI_FORMAT_R8G8B8A8_UNORM;
            if (video_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
                video_format = DXGI_FORMAT_B8G8R8A8_UNORM;
            const double scale =
                std::min(1., double(info.options.longest_edge) / std::max(v.rect_width, v.rect_height));
            info.width[i] = std::max(2u, unsigned(v.rect_width * scale) & ~1u);
            info.height[i] = std::max(2u, unsigned(v.rect_height * scale) & ~1u);
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
            content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            content.InputWidth = v.rect_width;
            content.InputHeight = v.rect_height;
            content.OutputWidth = info.width[i];
            content.OutputHeight = info.height[i];
            content.InputFrameRate = {info.options.fps, 1};
            content.OutputFrameRate = {info.options.fps, 1};
            content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
            hr = video->CreateVideoProcessorEnumerator(&content, &enumerator[i]);
            if (SUCCEEDED(hr))
                hr = video->CreateVideoProcessor(enumerator[i].Get(), 0, &processor[i]);
            UINT input_support{}, output_support{};
            if (SUCCEEDED(hr))
                hr = enumerator[i]->CheckVideoProcessorFormat(video_format, &input_support);
            if (SUCCEEDED(hr))
                hr = enumerator[i]->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &output_support);
            if (FAILED(hr) || !(input_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ||
                !(output_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
                return preparation_failed(
                    L"The GPU does not support full-FOV SDR conversion for this source format.");
            if (video_format != v.format) {
                // Video processors reject typed sRGB surfaces on common drivers.
                // A compatible-format GPU copy preserves the original SDR code
                // values; it does not linearize, crop or tonemap the source.
                conversion_bytes += std::uint64_t(v.rect_width) * v.rect_height * 4;
                if (conversion_bytes > 256ull * 1024 * 1024)
                    return preparation_failed(L"This sRGB source needs more than 256 MiB for recording "
                                              L"conversion. Reduce the source resolution before recording.");
                D3D11_TEXTURE2D_DESC d{};
                d.Width = v.rect_width;
                d.Height = v.rect_height;
                d.Format = video_format;
                d.ArraySize = d.MipLevels = d.SampleDesc.Count = 1;
                hr = device->CreateTexture2D(&d, nullptr, &srgb_copy[i]);
                D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input{};
                input.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
                if (SUCCEEDED(hr))
                    hr = video->CreateVideoProcessorInputView(srgb_copy[i].Get(), enumerator[i].Get(), &input,
                                                              &input_view[i]);
                if (FAILED(hr))
                    return preparation_failed(
                        hr_text(L"The bounded sRGB recording conversion surface could not be created", hr));
                input_texture[i] = srgb_copy[i];
            }
            RECT src{0, 0, v.rect_width, v.rect_height}, dst{0, 0, LONG(info.width[i]), LONG(info.height[i])};
            video_context->VideoProcessorSetStreamSourceRect(processor[i].Get(), 0, TRUE, &src);
            video_context->VideoProcessorSetStreamDestRect(processor[i].Get(), 0, TRUE, &dst);
            video_context->VideoProcessorSetOutputTargetRect(processor[i].Get(), TRUE, &dst);
            video_context->VideoProcessorSetStreamFrameFormat(processor[i].Get(), 0,
                                                              D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
            video_context->VideoProcessorSetStreamAutoProcessingMode(processor[i].Get(), 0, FALSE);
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgb{};
            rgb.RGB_Range = 0;
            rgb.YCbCr_Matrix = 1;
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE yuv{};
            yuv.YCbCr_Matrix = 1;
            yuv.Nominal_Range = 1;
            video_context->VideoProcessorSetStreamColorSpace(processor[i].Get(), 0, &rgb);
            video_context->VideoProcessorSetOutputColorSpace(processor[i].Get(), &yuv);
            for (auto &slot : slots) {
                D3D11_TEXTURE2D_DESC d{};
                d.Width = info.width[i];
                d.Height = info.height[i];
                d.Format = DXGI_FORMAT_NV12;
                d.ArraySize = d.MipLevels = d.SampleDesc.Count = 1;
                d.BindFlags = D3D11_BIND_RENDER_TARGET;
                d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
                hr = device->CreateTexture2D(&d, nullptr, &slot->producer[i]);
                if (SUCCEEDED(hr))
                    hr = slot->producer[i].As(&slot->producer_mutex[i]);
                ComPtr<IDXGIResource> resource;
                if (SUCCEEDED(hr))
                    hr = slot->producer[i].As(&resource);
                if (SUCCEEDED(hr))
                    hr = resource->GetSharedHandle(&slot->handles[i]);
                D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out{};
                out.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
                if (SUCCEEDED(hr))
                    hr = video->CreateVideoProcessorOutputView(slot->producer[i].Get(), enumerator[i].Get(),
                                                               &out, &slot->output[i]);
                if (FAILED(hr))
                    return preparation_failed(
                        hr_text(L"Bounded shared recording surfaces are unavailable", hr));
            }
        }
        return true;
    }
    void run() {
        MediaRuntime runtime;
        std::wstring error;
        Encoder encoders[2];
        ComPtr<ID3D11Device> encode_device;
        ComPtr<ID3D11DeviceContext> encode_context;
        ComPtr<IMFDXGIDeviceManager> manager;
        std::error_code ec;
        auto cleanup = [&] {
            for (auto &e : encoders)
                e.close();
            // Only this instance's freshly created sibling staging directory.
            if (!partial_path.empty())
                std::filesystem::remove_all(partial_path, ec);
        };
        if (!runtime.ok()) {
            set_state(CaptureState::failed, L"Windows Media Foundation could not start.");
            return;
        }
        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGIAdapter> adapter;
        HRESULT hr = device.As(&dxgi_device);
        if (SUCCEEDED(hr))
            hr = dxgi_device->GetAdapter(&adapter);
        if (SUCCEEDED(hr))
            hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   nullptr, 0, D3D11_SDK_VERSION, &encode_device, nullptr, &encode_context);
        ComPtr<ID3D10Multithread> protection;
        if (SUCCEEDED(hr))
            hr = encode_device.As(&protection);
        if (SUCCEEDED(hr))
            protection->SetMultithreadProtected(TRUE);
        UINT token{};
        if (SUCCEEDED(hr))
            hr = MFCreateDXGIDeviceManager(&token, &manager);
        if (SUCCEEDED(hr))
            hr = manager->ResetDevice(encode_device.Get(), token);
        for (auto &slot : slots)
            for (unsigned i = 0; i < source.view_count && SUCCEEDED(hr); ++i) {
                hr = encode_device->OpenSharedResource(slot->handles[i], IID_PPV_ARGS(&slot->encoder[i]));
                if (SUCCEEDED(hr))
                    hr = slot->encoder[i].As(&slot->encoder_mutex[i]);
            }
        if (FAILED(hr)) {
            set_state(CaptureState::failed, hr_text(L"The separate recording device could not start", hr));
            return;
        }
        if (std::filesystem::exists(destination, ec)) {
            set_state(CaptureState::failed, L"A clip already exists at this location. Choose a new name.");
            return;
        }
        std::filesystem::create_directories(destination.parent_path(), ec);
        if (ec) {
            set_state(CaptureState::failed, L"The clip folder could not be created.");
            return;
        }
        partial_path = destination;
        partial_path +=
            L".partial-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        if (!std::filesystem::create_directory(partial_path, ec)) {
            partial_path.clear();
            set_state(CaptureState::failed, L"A temporary clip folder could not be created.");
            return;
        }
        for (unsigned i = 0; i < source.view_count; ++i)
            if (!encoders[i].initialize(video_path(partial_path, i), manager.Get(), info.width[i],
                                        info.height[i], info.options.fps, error)) {
                cleanup();
                set_state(CaptureState::failed, error);
                return;
            }
        last_submit_ms = GetTickCount64();
        auto preparing = CaptureState::preparing;
        if (state.compare_exchange_strong(preparing, CaptureState::recording)) {
            std::lock_guard<std::mutex> lock(status_mutex);
            status.message = L"Recording full source FOV with verified hardware H.264 encoding.";
        }
        bool failed = false;
        while (!cancelled) {
            std::shared_ptr<CaptureSlot> slot;
            for (auto &candidate : slots)
                if (candidate->state.load(std::memory_order_acquire) == 2 &&
                    (!slot || candidate->frame.media_time < slot->frame.media_time))
                    slot = candidate;
            if (!slot) {
                if (state.load() == CaptureState::finishing)
                    break;
                if (GetTickCount64() - last_submit_ms.load() > 3000) {
                    {
                        std::lock_guard<std::mutex> lock(status_mutex);
                        stop_reason = L"Recording stopped: no new source frames (disconnect or pause).";
                    }
                    state = CaptureState::finishing;
                    continue;
                }
                std::unique_lock<std::mutex> lock(wait_mutex);
                wake.wait_for(lock, std::chrono::milliseconds(15));
                continue;
            }
            const auto recorded = slot->frame;
            slot->state = 3;
            slot->pending = 1; // Barrier protects two views from an early completion callback.
            for (unsigned i = 0; i < source.view_count; ++i)
                if (!encoders[i].write(slot, i, info.options.fps, error)) {
                    failed = true;
                    break;
                }
            slot->released();
            if (failed)
                break;
            info.frames.push_back(recorded);
            {
                std::lock_guard<std::mutex> lock(status_mutex);
                status.recorded_frames = unsigned(info.frames.size());
                status.elapsed_seconds = recorded.media_time / 10000000.;
            }
            if (FAILED(encode_device->GetDeviceRemovedReason())) {
                error = L"Recording stopped: the recording graphics device was lost.";
                failed = true;
                break;
            }
        }
        if (cancelled) {
            cleanup();
            set_state(CaptureState::cancelled, L"Recording cancelled; no clip was saved.");
            return;
        }
        set_state(CaptureState::finishing, L"Finishing the recorded clip...");
        if (info.frames.empty() && error.empty()) {
            std::lock_guard<std::mutex> lock(status_mutex);
            error = stop_reason;
        }
        for (unsigned i = 0; i < source.view_count; ++i) {
            hr = encoders[i].writer->Finalize();
            encoders[i].close();
            if (FAILED(hr)) {
                failed = true;
                if (error.empty())
                    error = hr_text(L"The hardware encoder could not finalize the clip", hr);
            }
        }
        if (cancelled) {
            cleanup();
            set_state(CaptureState::cancelled, L"Recording cancelled; no clip was saved.");
            return;
        }
        if (failed || info.frames.empty()) {
            cleanup();
            set_state(CaptureState::failed, error.empty() ? L"No source frames were recorded." : error);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            info.stop_reason = stop_reason;
        }
        info.partial = info.stop_reason != L"Duration reached";
        info.dropped_frames = drops;
        for (unsigned i = 0; i < source.view_count; ++i)
            if (!file_hash(video_path(partial_path, i), info.video_bytes[i], info.video_hash[i], error))
                failed = true;
        if (!failed && !write_clip_metadata(partial_path / L"metadata.bin", info, error))
            failed = true;
        if (!failed) {
            std::lock_guard<std::mutex> commit(commit_gate);
            if (cancelled) {
                cleanup();
                set_state(CaptureState::cancelled, L"Recording cancelled; no clip was saved.");
                return;
            }
            // Final path is visible only after both videos, hashes and metadata exist.
            std::filesystem::rename(partial_path, destination, ec);
            if (ec) {
                failed = true;
                error = L"The completed clip could not be committed (Windows error " +
                        std::to_wstring(ec.value()) + L").";
            } else {
                partial_path.clear();
                set_state(CaptureState::ready, info.partial ? L"Partial clip saved. " + info.stop_reason
                                                            : L"Clip saved with full source FOV.");
            }
        }
        if (failed) {
            cleanup();
            set_state(CaptureState::failed, error);
            return;
        }
    }
};
ClipRecorder::ClipRecorder() : impl_(new Impl) {}
ClipRecorder::~ClipRecorder() = default;
bool ClipRecorder::start(ID3D11Device *device, const std::filesystem::path &path, const Frame &source,
                         const CaptureOptions &options) {
    const auto s = impl_->state.load();
    if (s == CaptureState::preparing || s == CaptureState::recording || s == CaptureState::finishing)
        return false;
    impl_ = std::make_unique<Impl>();
    auto &r = *impl_;
    std::wstring error;
    if (!device || path.empty() || !valid_options(options))
        return r.preparation_failed(
            L"Recording needs a source device and a 1-60 second, 30/60 FPS SDR proxy up to 2048 pixels.");
    if (!source_valid(source, error, false))
        return r.preparation_failed(error);
    r.device = device;
    r.source = source;
    r.info.options = options;
    r.info.frames.reserve(options.duration_seconds * options.fps);
    r.destination = std::filesystem::absolute(path);
    r.status.path = r.destination;
    r.set_state(CaptureState::preparing, L"Preparing hardware recording...");
    if (!r.prepare_gpu())
        return false;
    r.worker = std::thread([&r] {
        try {
            r.run();
        } catch (const std::exception &) {
            r.set_state(CaptureState::failed,
                        L"Recording stopped because a bounded resource or file operation failed.");
        }
    });
    return true;
}
bool ClipRecorder::submit(const Frame &frame, ID3D11Texture2D *const views[2], double speed) {
    auto &r = *impl_;
    if (r.state.load() != CaptureState::recording)
        return false;
    std::wstring reason;
    if (!compatible_clip_source(r.source, frame, reason)) {
        stop(reason);
        return false;
    }
    if (frame.sequence <= r.last_sequence || frame.display_time <= r.last_time) {
        stop(L"Recording stopped: source sequence or clock moved backwards.");
        return false;
    }
    r.last_sequence = frame.sequence;
    r.last_time = frame.display_time;
    r.last_submit_ms = GetTickCount64();
    if (r.first_time)
        r.elapsed_time = std::min((frame.display_time - r.first_time) / 100,
                                  std::int64_t(r.info.options.duration_seconds) * 10000000);
    if (r.first_time &&
        frame.display_time - r.first_time >= std::int64_t(r.info.options.duration_seconds) * 1000000000) {
        stop(L"Duration reached");
        return false;
    }
    if (frame.display_time < r.next_due)
        return false;
    if (r.submitted >= r.info.options.duration_seconds * r.info.options.fps) {
        stop(L"Duration reached");
        return false;
    }
    if (!std::isfinite(speed) || speed < 0 || speed > 1500)
        speed = -1;
    std::shared_ptr<CaptureSlot> slot;
    for (auto &candidate : r.slots) {
        unsigned free = 0;
        if (candidate->state.compare_exchange_strong(free, 1)) {
            slot = candidate;
            break;
        }
    }
    if (!slot) {
        ++r.drops;
        return false;
    }
    unsigned acquired = 0;
    HRESULT hr = S_OK;
    for (; acquired < frame.view_count; ++acquired) {
        hr = slot->producer_mutex[acquired]->AcquireSync(0, 0);
        if (hr != S_OK)
            break;
    }
    if (acquired != frame.view_count) {
        for (unsigned i = 0; i < acquired; ++i)
            slot->producer_mutex[i]->ReleaseSync(0);
        slot->state = 0;
        ++r.drops;
        return false;
    }
    for (unsigned i = 0; i < frame.view_count; ++i) {
        D3D11_TEXTURE2D_DESC d{};
        if (views && views[i])
            views[i]->GetDesc(&d);
        const auto &v = frame.views[i];
        ComPtr<ID3D11Device> owner;
        if (views && views[i])
            views[i]->GetDevice(&owner);
        if (!views || !views[i] || owner.Get() != r.device.Get() || d.Width != unsigned(v.rect_width) ||
            d.Height != unsigned(v.rect_height) || d.Format != v.format || d.MipLevels != 1 ||
            d.ArraySize != 1 || d.SampleDesc.Count != 1) {
            reason = L"Recording stopped: the private source texture does not match its metadata.";
            hr = E_INVALIDARG;
            break;
        }
        auto *video_input = views[i];
        if (r.srgb_copy[i]) {
            r.context->CopyResource(r.srgb_copy[i].Get(), views[i]);
            video_input = r.srgb_copy[i].Get();
        }
        if (r.input_texture[i].Get() != video_input) {
            r.input_texture[i] = video_input;
            r.input_view[i].Reset();
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input{};
            input.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            hr = r.video->CreateVideoProcessorInputView(video_input, r.enumerator[i].Get(), &input,
                                                        &r.input_view[i]);
            if (FAILED(hr)) {
                reason = hr_text(
                    L"Recording stopped: the full-source texture cannot be used for GPU conversion", hr);
                break;
            }
        }
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = r.input_view[i].Get();
        hr = r.video_context->VideoProcessorBlt(r.processor[i].Get(), slot->output[i].Get(), 0, 1, &stream);
        if (FAILED(hr))
            break;
    }
    r.context->Flush();
    if (SUCCEEDED(hr))
        hr = r.device->GetDeviceRemovedReason();
    for (unsigned i = 0; i < frame.view_count; ++i) {
        auto release = slot->producer_mutex[i]->ReleaseSync(SUCCEEDED(hr) ? 1 : 0);
        if (FAILED(release))
            hr = release;
    }
    if (FAILED(hr)) {
        slot->state = 0;
        stop(reason.empty() ? hr_text(L"Recording stopped: GPU full-source conversion failed", hr) : reason);
        return false;
    }
    if (!r.first_time)
        r.first_time = frame.display_time;
    slot->frame = {frame, speed, (frame.display_time - r.first_time) / 100};
    strip_handles(slot->frame.frame);
    const auto interval = 1000000000LL / r.info.options.fps;
    r.next_due = r.first_time + ((frame.display_time - r.first_time) / interval + 1) * interval;
    ++r.submitted;
    slot->state.store(2, std::memory_order_release);
    r.wake.notify_one();
    return true;
}
void ClipRecorder::stop(const std::wstring &reason) {
    auto &r = *impl_;
    const auto state = r.state.load();
    if (state != CaptureState::recording && state != CaptureState::preparing)
        return;
    {
        std::lock_guard<std::mutex> lock(r.status_mutex);
        r.stop_reason = reason.substr(0, 512);
    }
    r.set_state(CaptureState::finishing, reason + L" Finishing the recorded clip...");
    r.wake.notify_one();
}
void ClipRecorder::cancel() {
    auto &r = *impl_;
    std::lock_guard<std::mutex> commit(r.commit_gate);
    auto state = r.state.load();
    if (state == CaptureState::recording || state == CaptureState::preparing ||
        state == CaptureState::finishing) {
        r.cancelled = true;
        r.set_state(CaptureState::finishing, L"Cancelling recording...");
        r.wake.notify_one();
    }
}
CaptureStatus ClipRecorder::status() {
    auto &r = *impl_;
    r.reap_terminal();
    std::lock_guard<std::mutex> lock(r.status_mutex);
    auto out = r.status;
    out.state = r.state.load();
    out.dropped_frames = r.drops;
    out.elapsed_seconds = std::max(out.elapsed_seconds, r.elapsed_time.load() / 10000000.);
    return out;
}
struct ClipReader::Impl {
    ClipInfo info;
    std::wstring error;
    std::unique_ptr<MediaRuntime> runtime;
    ComPtr<ID3D11Device> device;
    ComPtr<IMFSourceReader> readers[2];
    std::vector<unsigned char> pixels;
    std::size_t cursor{};
    bool valid{};
    ~Impl() {
        for (auto &r : readers)
            r.Reset();
        runtime.reset();
    }
    bool decode(DecodedFrame *decoded) {
        if (!valid || cursor >= info.frames.size())
            return false;
        const auto &metadata = info.frames[cursor];
        DecodedFrame output;
        output.frame = metadata.frame;
        output.speed_kph = metadata.speed_kph;
        output.index = cursor;
        for (unsigned i = 0; i < metadata.frame.view_count; ++i) {
            ComPtr<IMFSample> sample;
            DWORD flags{}, stream{};
            LONGLONG time{};
            HRESULT hr = S_OK;
            unsigned ticks = 0;
            do {
                hr = readers[i]->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &time,
                                            &sample);
                if (SUCCEEDED(hr) && flags & (MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED |
                                              MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)) {
                    ComPtr<IMFMediaType> current, native;
                    UINT32 w{}, h{};
                    GUID subtype{};
                    hr = readers[i]->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current);
                    if (SUCCEEDED(hr))
                        hr = MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &w, &h);
                    if (SUCCEEDED(hr))
                        hr = current->GetGUID(MF_MT_SUBTYPE, &subtype);
                    if (SUCCEEDED(hr) &&
                        (w != info.width[i] || h != info.height[i] || subtype != MFVideoFormat_RGB32))
                        hr = MF_E_INVALIDMEDIATYPE;
                    if (SUCCEEDED(hr))
                        hr = readers[i]->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native);
                    if (SUCCEEDED(hr))
                        hr = MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h);
                    if (SUCCEEDED(hr) && (w != info.width[i] || h != info.height[i]))
                        hr = MF_E_INVALIDMEDIATYPE;
                }
                if (FAILED(hr) || flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))
                    break;
            } while (!sample && ++ticks < 8);
            if (FAILED(hr) || !sample || flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)) {
                valid = false;
                error = hr_text(L"The clip video is truncated or its format changed", hr);
                return false;
            }
            if (std::abs(time - metadata.media_time) > 1) {
                valid = false;
                return fail(error, L"The decoded image timestamp does not match its pose metadata.");
            }
            if (!decoded)
                continue;
            ComPtr<IMFMediaBuffer> buffer;
            hr = sample->ConvertToContiguousBuffer(&buffer);
            ComPtr<IMF2DBuffer> image;
            BYTE *row{};
            LONG pitch{};
            bool lock2d = false, locked = false;
            const unsigned width = info.width[i], height = info.height[i];
            if (SUCCEEDED(hr) && SUCCEEDED(buffer.As(&image))) {
                hr = image->Lock2D(&row, &pitch);
                lock2d = SUCCEEDED(hr);
                locked = lock2d;
            } else if (SUCCEEDED(hr)) {
                DWORD capacity{}, length{};
                hr = buffer->Lock(&row, &capacity, &length);
                locked = SUCCEEDED(hr);
                ComPtr<IMFMediaType> current;
                UINT32 stride{};
                if (SUCCEEDED(hr))
                    hr = readers[i]->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current);
                if (SUCCEEDED(hr))
                    hr = current->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride);
                pitch = LONG(stride);
                if (SUCCEEDED(hr) && (std::abs(std::int64_t(pitch)) < width * 4 ||
                                      std::uint64_t(std::abs(std::int64_t(pitch))) * height > length))
                    hr = MF_E_INVALIDMEDIATYPE;
                if (SUCCEEDED(hr) && pitch < 0)
                    row += std::size_t(-pitch) * (height - 1);
            }
            if (SUCCEEDED(hr) && std::abs(std::int64_t(pitch)) < width * 4)
                hr = MF_E_INVALIDMEDIATYPE;
            if (SUCCEEDED(hr)) {
                pixels.resize(std::size_t(width) * height * 4);
                for (unsigned y = 0; y < height; ++y)
                    std::copy_n(row + std::ptrdiff_t(y) * pitch, width * 4,
                                pixels.data() + std::size_t(y) * width * 4);
            }
            if (locked) {
                if (lock2d)
                    image->Unlock2D();
                else
                    buffer->Unlock();
            }
            if (SUCCEEDED(hr)) {
                D3D11_TEXTURE2D_DESC d{};
                d.Width = width;
                d.Height = height;
                d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
                d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA data{pixels.data(), width * 4, 0};
                hr = device->CreateTexture2D(&d, &data, &output.textures[i]);
                auto &v = output.frame.views[i];
                v.x = v.y = 0;
                v.width = v.rect_width = width;
                v.height = v.rect_height = height;
                v.format = DXGI_FORMAT_B8G8R8A8_UNORM;
                v.texture_handle = 0;
            }
            if (FAILED(hr)) {
                valid = false;
                error = hr_text(L"The bounded clip image could not be decoded", hr);
                return false;
            }
        }
        ++cursor;
        if (decoded)
            *decoded = std::move(output);
        return true;
    }
};
ClipReader::ClipReader() : impl_(new Impl) {}
ClipReader::~ClipReader() = default;
bool ClipReader::open(ID3D11Device *device, const std::filesystem::path &path) {
    close();
    auto &r = *impl_;
    if (!device)
        return fail(r.error, L"The Workshop replay device is unavailable.");
    if (!read_clip_metadata(path / L"metadata.bin", r.info, r.error))
        return false;
    r.runtime = std::make_unique<MediaRuntime>();
    if (!r.runtime->ok())
        return fail(r.error, L"Windows Media Foundation could not start for replay.");
    for (unsigned i = 0; i < r.info.frames.front().frame.view_count; ++i) {
        std::uint64_t bytes{};
        std::array<unsigned char, 32> hash{};
        if (!file_hash(video_path(path, i), bytes, hash, r.error))
            return false;
        if (bytes != r.info.video_bytes[i] || hash != r.info.video_hash[i])
            return fail(r.error, L"The clip video does not match its metadata checksum.");
        ComPtr<IMFAttributes> attrs;
        MFCreateAttributes(&attrs, 2);
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        auto hr = MFCreateSourceReaderFromURL(video_path(path, i).c_str(), attrs.Get(), &r.readers[i]);
        ComPtr<IMFMediaType> native;
        GUID subtype{};
        UINT32 width{}, height{};
        if (SUCCEEDED(hr))
            hr = r.readers[i]->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native);
        if (SUCCEEDED(hr))
            hr = native->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (SUCCEEDED(hr))
            hr = MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &width, &height);
        if (FAILED(hr) || subtype != MFVideoFormat_H264 || width != r.info.width[i] ||
            height != r.info.height[i])
            return fail(r.error, L"The clip video format or dimensions do not match the bounded metadata.");
        r.readers[i]->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        r.readers[i]->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
        if (!audit_compressed_video(r.readers[i].Get(), native.Get(), r.info, i, r.error))
            return false;
        ComPtr<IMFMediaType> output;
        MFCreateMediaType(&output);
        output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, width, height);
        hr = r.readers[i]->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, output.Get());
        if (FAILED(hr)) {
            r.error = hr_text(L"The SDR clip decoder could not start", hr);
            return false;
        }
    }
    r.device = device;
    r.valid = true;
    return true;
}
void ClipReader::close() {
    impl_ = std::make_unique<Impl>();
}
bool ClipReader::seek(std::size_t index) {
    auto &r = *impl_;
    if (!r.valid || index >= r.info.frames.size())
        return fail(r.error, L"The requested clip frame is out of range.");
    PROPVARIANT position{};
    position.vt = VT_I8;
    position.hVal.QuadPart = 0;
    for (unsigned i = 0; i < r.info.frames.front().frame.view_count; ++i) {
        auto hr = r.readers[i]->SetCurrentPosition(GUID_NULL, position);
        if (FAILED(hr)) {
            r.error = hr_text(L"The clip could not seek to its beginning", hr);
            return false;
        }
    }
    r.cursor = 0;
    while (r.cursor < index)
        if (!r.decode(nullptr))
            return false;
    return true;
}
bool ClipReader::next(DecodedFrame &frame) {
    return impl_->decode(&frame);
}
std::size_t ClipReader::count() const {
    return impl_->valid ? impl_->info.frames.size() : 0;
}
const ClipFrame *ClipReader::metadata(std::size_t index) const {
    return index < impl_->info.frames.size() ? &impl_->info.frames[index] : nullptr;
}
const ClipInfo &ClipReader::info() const {
    return impl_->info;
}
std::wstring ClipReader::error() const {
    return impl_->error;
}
} // namespace vrst::preview

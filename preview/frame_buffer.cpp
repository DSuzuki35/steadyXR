#include "frame_buffer.hpp"
#include <algorithm>

namespace vrst::preview {
namespace {
const View &view_at(const Frame &f, unsigned i) {
    return i < f.view_count ? f.views[i] : f.overlays[i - f.view_count].image;
}
bool same_source(const Frame &a, const Frame &b) {
    if (!a.view_count || a.generation != b.generation || a.session != b.session ||
        a.reference_epoch != b.reference_epoch || a.producer_pid != b.producer_pid ||
        a.eye != b.eye || a.view_count != b.view_count || a.overlay_count != b.overlay_count ||
        b.sequence < a.sequence || b.display_time < a.display_time ||
        b.display_time - a.display_time > 500000000)
        return false;
    for (unsigned i = 0; i < a.view_count + a.overlay_count; ++i) {
        const auto &x = view_at(a, i), &y = view_at(b, i);
        if (x.width != y.width || x.height != y.height || x.format != y.format ||
            x.rect_width != y.rect_width || x.rect_height != y.rect_height)
            return false;
    }
    return true;
}
}
unsigned FrameBuffer::capacity(std::uint64_t bytes, unsigned requested) {
    if (!bytes || bytes > byte_limit / 2)
        return 0;
    return std::min({requested, 4u, unsigned(byte_limit / bytes - 1)});
}
void FrameBuffer::reset() {
    queue_.clear();
    poses_.clear();
    previous_ = {};
    status_ = {};
    requested_ = delay_ = 0;
    failed_ = false;
    selected_sequence_ = 0;
    target_ = {};
}
void FrameBuffer::update(ID3D11Device *device, ID3D11DeviceContext *context, const Frame &frame,
                         const Microsoft::WRL::ComPtr<ID3D11Texture2D> *textures,
                         const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> *views,
                         unsigned requested, double speed, unsigned game) {
    if (!requested || !frame.view_count) {
        reset();
        return;
    }
    if (requested != requested_ || !same_source(previous_, frame)) {
        reset();
        requested_ = requested;
    }
    if (frame.sequence == previous_.sequence)
        return;
    previous_ = frame;
    const unsigned count = frame.view_count + frame.overlay_count;
    if (count > max_images || failed_)
        return;
    std::array<D3D11_TEXTURE2D_DESC, max_images> descriptions{};
    std::uint64_t bytes{};
    for (unsigned i = 0; i < count; ++i) {
        if (!textures[i] || !views[i]) {
            reset();
            return;
        }
        textures[i]->GetDesc(&descriptions[i]);
        const auto &d = descriptions[i];
        const unsigned pixel_bytes = d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
        bytes += std::uint64_t(d.Width) * d.Height * pixel_bytes;
    }
    delay_ = capacity(bytes, requested);
    status_.buffer_limited = delay_ < requested;
    if (!delay_)
        return;
    std::unique_ptr<Entry> next;
    if (queue_.size() == delay_ + 1) {
        next = std::move(queue_.front());
        queue_.pop_front();
    } else
        next = std::make_unique<Entry>();
    for (unsigned i = 0; i < count; ++i) {
        if (!next->textures[i]) {
            auto d = descriptions[i];
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            d.CPUAccessFlags = d.MiscFlags = 0;
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            views[i]->GetDesc(&sv);
            if (FAILED(device->CreateTexture2D(&d, nullptr, &next->textures[i])) ||
                FAILED(device->CreateShaderResourceView(next->textures[i].Get(), &sv, &next->views[i]))) {
                // Disable until the source/settings change, rather than retrying
                // large allocations every frame under memory pressure.
                queue_.clear();
                poses_.clear();
                failed_ = true;
                selected_sequence_ = 0;
                status_ = {};
                status_.buffer_limited = true;
                return;
            }
        }
        context->CopyResource(next->textures[i].Get(), textures[i].Get());
    }
    next->frame = frame;
    next->speed_kph = speed;
    next->game = game;
    queue_.push_back(std::move(next));
    poses_.push_back(frame);
    while (poses_.size() > 2 * delay_ + 1)
        poses_.pop_front();
    // A held warmup image must keep the same target as well as its pixels.
    // Otherwise new future poses would change the camera without advancing its
    // frame clock, bypassing crop/spectator temporal updates.
    if (selected_sequence_ != queue_.front()->frame.sequence) {
        std::array<Frame, 9> centered{};
        unsigned n{};
        for (const auto &pose : poses_)
            centered[n++] = pose;
        target_ = centered_orientation(centered.data(), n);
        selected_sequence_ = queue_.front()->frame.sequence;
    }
    status_.buffered_frames = unsigned(queue_.size() - 1);
    status_.buffered_bytes = bytes * queue_.size();
    status_.delay_ms = double(frame.display_time - queue_.front()->frame.display_time) * 1e-6;
}
} // namespace vrst::preview

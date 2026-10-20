#pragma once
#include "director.hpp"
#include "transport.hpp"
#include <array>
#include <deque>
#include <memory>
#include <d3d11.h>
#include <wrl/client.h>

namespace vrst::preview {
// Consumer-owned textures only. GPU copies remain ordered on its immediate
// context; no query, readback or synchronization with the game is added.
class FrameBuffer {
  public:
    static constexpr unsigned max_images = 2 + max_overlay_images;
    static constexpr std::uint64_t byte_limit = 256ull * 1024 * 1024;
    struct Entry {
        Frame frame{};
        double speed_kph{-1};
        unsigned game{};
        std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, max_images> textures;
        std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, max_images> views;
    };
    void update(ID3D11Device *, ID3D11DeviceContext *, const Frame &,
                const Microsoft::WRL::ComPtr<ID3D11Texture2D> *,
                const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> *,
                unsigned delay_frames, double speed_kph, unsigned game);
    const Entry *selected() const { return queue_.empty() ? nullptr : queue_.front().get(); }
    const Quat *target() const { return queue_.empty() ? nullptr : &target_; }
    DirectorStatus status() const { return status_; }
    void reset();
    static unsigned capacity(std::uint64_t frame_bytes, unsigned requested);

  private:
    std::deque<std::unique_ptr<Entry>> queue_;
    std::deque<Frame> poses_;
    Frame previous_{};
    unsigned requested_{}, delay_{};
    bool failed_{};
    std::uint64_t selected_sequence_{};
    Quat target_{};
    DirectorStatus status_{};
};
} // namespace vrst::preview

#pragma once
#include <memory>
#include <d3d11.h>
#include <d3d12.h>

namespace vrst::preview {
// Called under the layer's state lock. All returned COM pointers are borrowed.
// The application queue receives only capture commands and fence signals; it
// never waits for the companion device or the preview consumer.
class D3D12Capture {
    struct Slot;
    struct PoolData;
    struct Impl;

  public:
    class Pool {
      public:
        Pool();
        ~Pool();
        Pool(Pool &&) noexcept;
        Pool &operator=(Pool &&) noexcept;
        Pool(const Pool &) = delete;
        Pool &operator=(const Pool &) = delete;

      private:
        friend class D3D12Capture;
        std::unique_ptr<PoolData> data_;
    };
    class Snapshot {
      public:
        ~Snapshot();
        Snapshot(const Snapshot &) = delete;
        Snapshot &operator=(const Snapshot &) = delete;

      private:
        friend class D3D12Capture;
        explicit Snapshot(std::shared_ptr<Slot>);
        std::shared_ptr<Slot> slot_;
    };
    using SnapshotPtr = std::shared_ptr<const Snapshot>;

    D3D12Capture();
    ~D3D12Capture();
    D3D12Capture(const D3D12Capture &) = delete;
    D3D12Capture &operator=(const D3D12Capture &) = delete;

    bool initialize(ID3D12Device *, ID3D12CommandQueue *);
    ID3D11Device *device() const;
    ID3D11DeviceContext *context() const;
    bool healthy() const;
    const wchar_t *error() const;

    // Captures mip 0 of every array slice. Source must be in RENDER_TARGET on
    // the binding queue. The queued capture restores it before later queue work.
    // Returns null without waiting when all three pool slots are still used.
    // A token is a candidate until the downstream OpenXR release succeeds.
    SnapshotPtr capture(Pool &, ID3D12Resource *, DXGI_FORMAT typed_format);
    static ID3D11Texture2D *texture(const SnapshotPtr &);

    // Call only after acquiring output keys, immediately before queueing copies
    // from this snapshot on context(). finish_reads() must follow every batch,
    // including a partially aborted batch, before any selected token is dropped.
    bool before_read(const SnapshotPtr &);
    bool finish_reads() noexcept;
    void collect();

  private:
    std::unique_ptr<Impl> impl_;
};
} // namespace vrst::preview

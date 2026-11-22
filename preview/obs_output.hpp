#pragma once
#include <memory>
#include <string>

namespace vrst::preview {
class Renderer;

// Publishes GPU texture metadata only; never transfers or reads back pixels.
// Call publish on every output tick, including paused/busy ticks for heartbeat.
class ObsOutputPublisher {
  public:
    ObsOutputPublisher();
    ~ObsOutputPublisher();
    bool open();
    bool publish(const Renderer &, unsigned width, unsigned height, bool visible = true);
    void close();
    std::wstring error() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vrst::preview

#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>
namespace vrst::preview {
struct ButtonEdge {
    bool released{};
    bool update(bool down) {
        const bool pressed = down && released;
        released = !down;
        return pressed;
    }
    void disconnect() {
        released = false;
    }
};
class WorkshopInput {
  public:
    struct Device {
        GUID id{};
        std::wstring name;
    };
    WorkshopInput();
    ~WorkshopInput();
    void initialize(HWND, const std::filesystem::path &, std::function<void()>);
    bool hotkey(unsigned modifiers, unsigned key);
    std::vector<Device> devices();
    void learn(const GUID &);
    void clear_button();
    void poll();
    std::wstring status() const;
    unsigned modifiers() const;
    unsigned key() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vrst::preview

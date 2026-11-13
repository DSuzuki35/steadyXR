#include "monitor_fallback.hpp"
#include <cstdio>
#include <cstdlib>

using Microsoft::WRL::ComPtr;
using namespace vrst::preview;
namespace {
ComPtr<ID3D11Device> device;
ComPtr<ID3D11Texture2D> frame;
bool active{}, lose{}, give_frame{}, retained{};
unsigned starts{}, pictures{}, slates{};
std::wstring slate, diagnostic;
void check(bool ok, const char *message) {
    if (!ok) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
}

// Exercise the production routing loop with deterministic capture/present events.
// Forcing Windows to invalidate a real duplication would interrupt the desktop.
namespace vrst::preview {
struct WindowCapture::Impl {};
WindowCapture::WindowCapture() = default;
WindowCapture::~WindowCapture() = default;
bool WindowCapture::running() const { return active; }
std::wstring WindowCapture::error() const { return diagnostic; }
void WindowCapture::stop() { active = false; diagnostic.clear(); }
bool WindowCapture::start_monitor(ID3D11Device *, HMONITOR monitor) {
    ++starts;
    diagnostic.clear();
    return active = monitor != nullptr;
}
bool WindowCapture::poll(ComPtr<ID3D11Texture2D> &texture, std::uint64_t &) {
    if (lose) {
        active = false;
        diagnostic = L"Read selected display with desktop duplication (0x887a0026)";
    } else if (give_frame) {
        texture = frame;
    }
    return texture != nullptr;
}
struct Renderer::Impl {};
Renderer::Renderer() = default;
Renderer::~Renderer() = default;
ID3D11Device *Renderer::device() const { return ::device.Get(); }
bool Renderer::attach(HWND) { return true; }
void Renderer::clear_monitor() { retained = false; }
bool Renderer::draw_monitor(ID3D11Texture2D *texture, unsigned, unsigned, bool) {
    if (texture) retained = true;
    if (retained) ++pictures;
    return retained;
}
bool Renderer::draw(const Options &, bool, const wchar_t *text) {
    ++slates;
    slate = text ? text : L"";
    return true;
}
}
int main() {
    check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
          D3D11_SDK_VERSION, &device, nullptr, nullptr)), "create software GPU");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    check(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &frame)), "create test frame");
    Renderer renderer;
    MonitorFallback clone(renderer);
    auto tick = [&](unsigned ms) { clone.tick(nullptr, L"", {}, 60, ms, false); };
    give_frame = true;
    tick(1000);
    check(clone.has_picture() && pictures == 1, "initial live clone");
    give_frame = false;
    lose = true;
    tick(1100);
    check(!active && clone.has_picture() && slates == 0, "keep last picture during short access loss");
    check(clone.status().find(L"887a0026") != std::wstring::npos, "record diagnostic for operator");
    tick(1150);
    check(starts == 1, "rate limit reconnection");
    lose = false;
    give_frame = true;
    tick(2100);
    check(active && pictures == 2 && slates == 0 && clone.status().empty(), "resume with fresh image");
    give_frame = false;
    lose = true;
    tick(2200);
    tick(3300);
    tick(4400);
    check(!clone.has_picture() && slates && slate.find(L"0x") == std::wstring::npos,
          "show a neutral slate after prolonged loss");
    clone.stop();
    check(!active && !retained, "release on VR transition");
    tick(4500);
    check(!clone.has_picture() && slate.find(L"0x") == std::wstring::npos,
          "first frame failure never broadcasts HRESULT");
    return 0;
}

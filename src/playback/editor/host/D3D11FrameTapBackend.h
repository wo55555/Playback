#pragma once

#include "playback/visuals/FrameTap.h"

#include <memory>
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace playback::editor::host {

class D3D11FrameTapBackend {
public:
    explicit D3D11FrameTapBackend(visuals::FrameTap& frameTap);
    ~D3D11FrameTapBackend();

    D3D11FrameTapBackend(D3D11FrameTapBackend const&)            = delete;
    D3D11FrameTapBackend& operator=(D3D11FrameTapBackend const&) = delete;

    void poll(ID3D11DeviceContext* context);
    bool capture(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source);
    void reset(visuals::FrameTapError error, std::string message);

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace playback::editor::host

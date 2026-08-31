#pragma once

#include <cstdint>

struct HWND__;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;

namespace cursoroverlay {

// Composites the Win32 hardware cursor over the already-copied game UI image.
// The caller owns the D3D pipeline state and releases the XR image afterwards.
void draw(ID3D11Device* device, ID3D11DeviceContext* context,
          ID3D11RenderTargetView* target, HWND__* window,
          uint32_t targetWidth, uint32_t targetHeight,
          bool targetIsSrgb);

// Same cursor mapping, placed inside a viewport of a larger render target.
void draw_in_rect(ID3D11Device* device, ID3D11DeviceContext* context,
                  ID3D11RenderTargetView* target, HWND__* window,
                  uint32_t targetWidth, uint32_t targetHeight,
                  float destX, float destY, float destWidth, float destHeight,
                  bool targetIsSrgb, float cursorScale = 1.0f);

void reset();

} // namespace cursoroverlay

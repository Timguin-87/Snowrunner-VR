#pragma once

#include <algorithm>

namespace cursorpolicy {

struct RectF {
    float left, top, right, bottom;
};

inline RectF texture_rect(float cursorX, float cursorY,
                          float hotspotX, float hotspotY,
                          float cursorW, float cursorH,
                          float clientW, float clientH,
                          float textureW, float textureH) noexcept
{
    if (clientW <= 0.0f || clientH <= 0.0f)
        return {};
    const float sx = textureW / clientW;
    const float sy = textureH / clientH;
    const float left = (cursorX - hotspotX) * sx;
    const float top  = (cursorY - hotspotY) * sy;
    return {left, top, left + cursorW * sx, top + cursorH * sy};
}

inline bool intersects(const RectF& r, float width, float height) noexcept
{
    return r.right > 0.0f && r.bottom > 0.0f &&
           r.left < width && r.top < height &&
           r.right > r.left && r.bottom > r.top;
}

} // namespace cursorpolicy

#include "common/cursor_policy.h"

#include <cassert>

int main()
{
    using namespace cursorpolicy;
    const RectF scaled = texture_rect(100, 50, 4, 6, 32, 24,
                                      200, 100, 400, 200);
    assert(scaled.left == 192 && scaled.top == 88);
    assert(scaled.right == 256 && scaled.bottom == 136);
    assert(intersects(scaled, 400, 200));

    const RectF hotspotClip = texture_rect(1, 1, 8, 8, 16, 16,
                                           100, 100, 100, 100);
    assert(hotspotClip.left == -7 && hotspotClip.top == -7);
    assert(intersects(hotspotClip, 100, 100));

    const RectF outside = texture_rect(-40, -40, 0, 0, 16, 16,
                                       100, 100, 100, 100);
    assert(!intersects(outside, 100, 100));
    assert(!intersects(texture_rect(0, 0, 0, 0, 1, 1, 0, 100, 100, 100),
                       100, 100));
    return 0;
}

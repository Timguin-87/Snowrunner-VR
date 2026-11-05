#pragma once

// THE WORLD MARKERS, placed on the UI plane over what they mark.
//
// SnowRunner's HUD is Scaleform. Every marker it pins to the world -- waypoint,
// objective and building icons, their distances and their off-screen arrows --
// is placed by the game's screen-markers module, which projects the tracked
// point through the UI's own camera object (class ui3D_CAMERA_MR). That object
// holds a pointer to the game's active combine::CAMERA at +0x18 and refreshes
// it in its Update, and every projection the UI makes reads through that one
// pointer: world to view, view to NDC, the eye and its axes, and the FOV the
// 3D markers' perspective is built from.
//
// So the pointer is replaced. Update runs, then the object is pointed at a
// camera of the mod's own: at the middle of the two eyes, looking along the UI
// plane's normal, with a projection whose NDC edges are the plane's edges. The
// game then puts each marker where the ray from between the eyes to the thing
// it marks crosses the plane, and its own off-screen handling clamps the rest
// to the plane's edge with an arrow. Nothing has to tell a marker from static
// UI: everything goes to the plane, and the markers on it are already where
// they belong.
//
// BETWEEN THE EYES so that both eyes' frames place every marker alike. Then the
// UI of either frame is the UI of both, and the plane takes every frame's
// (build_ui_quad_layer) instead of one eye's at half the rate. Each eye sees a
// marker off by half the parallax between the plane and the thing it marks, in
// opposite directions, which is where a single plane can put it for two eyes.
//
// TWO LISTS, ONE OF THEM APPROXIMATE. The module places 2D markers itself,
// through the projection above, and those land exactly. A second list is placed
// as Scaleform 3D objects whose perspective the game builds from the camera's
// FOV and aspect alone -- symmetric, so it cannot carry the plane's off-centre
// part: the offset of the middle of the eyes from the plane's axis (a lean, or
// half the IPD for a plane hung on one eye). Small at 7 m; those markers are
// near, not exact.
//
// Every screen whose UI is on its own plane -- gameplay, the map, and since
// 2026-11-05 the garage and the menus. Found 2026-11-02 by static analysis;
// game_addresses.cpp finds the class by name and checks every slot that reads
// the camera before this may run.
namespace hooks {

// Hooks the UI camera's Update. Call once the game's addresses are resolved.
// Without the address nothing is installed and the markers are the game's own,
// projected per eye -- which is when the UI plane goes back to one eye's UI.
void install_marker_camera();
bool marker_camera_available();

// One line for the settings menu: what it is doing right now.
const char* marker_camera_status();

} // namespace hooks

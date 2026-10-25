// Kirby Air Ride's own hooks (the runtime's game-specific part, beside src/runtime/fixes.cpp's code
// changes): what the renderer needs to know about the game's drawing.
#include "runtime/gx.h"
#include "runtime/runtime.h"

namespace rt {

// ---- in-between frames: the camera ----
// HAL's cameras (HSD_CObj) compute their view matrix with C_MTXLookAt as they begin drawing (the
// matrix is the camera's own, then loaded with each model). The matrices loaded after it are seen
// through it: blended as the objects' places in the world, under one blended camera
// (src/runtime/gxinterp.cpp), so the scene turns as one when the camera turns.
HLE(C_MTXLookAt) {  // C_MTXLookAt(Mtx m, Point3d* camPos, Vec* camUp, Point3d* target)
    static const GuestFn s_orig = originalOf("C_MTXLookAt");
    const u32 m = c.r[3];
    s_orig(c);
    gxViewMatrixAt(m);
}

}  // namespace rt

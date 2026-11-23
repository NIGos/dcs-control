// Simulated head for unattended rig flights (TEST-ONLY, part of the rig capture layer, rigtap.cpp).
// Pure pose math; the head program is a simgaze.h program read as x = yaw to the right, y = pitch up (degrees, about
// the head's own axes: yaw about +Y, then pitch about the yawed +X, no roll), applied on top of a base head pose.
// Tested by simgaze_harness.cpp.
#pragma once

#include <cmath>
#include "simgaze.h"

namespace simhead {

struct Pose { simgaze::Quat q{}; float p[3] = {0, 0, 0}; };

inline simgaze::Quat conjugate(const simgaze::Quat& a) { return {-a.x, -a.y, -a.z, a.w}; }
inline float norm(const simgaze::Quat& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w); }
// A runtime orientation that can be used (an untracked runtime may answer zeros).
inline bool usable(const simgaze::Quat& a) { const float n = norm(a); return n > 0.5f && n < 1.5f; }
inline simgaze::Quat normalized(const simgaze::Quat& a) { const float n = norm(a); return n > 0 ? simgaze::Quat{a.x / n, a.y / n, a.z / n, a.w / n} : simgaze::Quat{}; }

// a * b: b expressed in a's frame, then a applied (OpenXR pose composition, as XrPosef of b in a, a in base).
inline Pose compose(const Pose& a, const Pose& b) {
  Pose r; r.q = normalized(simgaze::multiply(a.q, b.q)); float t[3]; simgaze::rotate(a.q, b.p, t);
  r.p[0] = a.p[0] + t[0]; r.p[1] = a.p[1] + t[1]; r.p[2] = a.p[2] + t[2]; return r;
}
inline Pose inverse(const Pose& a) {
  Pose r; r.q = conjugate(a.q); float t[3]; simgaze::rotate(r.q, a.p, t); r.p[0] = -t[0]; r.p[1] = -t[1]; r.p[2] = -t[2]; return r;
}
// The head turn of a program sample (same convention as the gaze: x right, y up).
inline Pose turn(float x_deg, float y_deg) { Pose r; r.q = simgaze::gaze_orientation(x_deg, y_deg); return r; }

}  // namespace simhead

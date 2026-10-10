#pragma once

#include <cstdint>
#include <optional>

namespace xrfg {

struct Vec3 {
    float x{};
    float y{};
    float z{};
};

struct Quaternion {
    float x{};
    float y{};
    float z{};
    float w{1.0F};
};

struct Pose {
    Quaternion orientation{};
    Vec3 position{};
};

struct TimedPose {
    std::int64_t time_ns{};
    Pose pose{};
};

[[nodiscard]] Quaternion normalize(Quaternion value) noexcept;
[[nodiscard]] Quaternion slerp_shortest(Quaternion from, Quaternion to, float alpha) noexcept;
[[nodiscard]] Pose interpolate_pose(const Pose& from, const Pose& to, float alpha) noexcept;
// The pose `fraction` of the way along the motion from `from` to `to`: the
// rotation between them turned through `fraction` of its angle about its own
// axis, and the translation scaled alike. 0 is `from` and 1 is `to`; between
// them it is interpolate_pose, and past 1 the motion carries on at the same
// rate, for a frame shown after `to`. Negative fractions are taken as 0.
[[nodiscard]] Pose pose_at_fraction(const Pose& from, const Pose& to, float fraction) noexcept;
// The angle in radians between two orientations, 0 to pi.
[[nodiscard]] float rotation_angle(Quaternion from, Quaternion to) noexcept;
[[nodiscard]] std::optional<TimedPose> midpoint(const TimedPose& previous, const TimedPose& current) noexcept;

}  // namespace xrfg

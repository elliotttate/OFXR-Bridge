#include "xrfg/pose.hpp"

#include <algorithm>
#include <cmath>

namespace xrfg {
namespace {

[[nodiscard]] float dot(const Quaternion& left, const Quaternion& right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z + left.w * right.w;
}

[[nodiscard]] Quaternion scale_add(
    const Quaternion& left,
    float left_scale,
    const Quaternion& right,
    float right_scale) noexcept {
    return {
        left.x * left_scale + right.x * right_scale,
        left.y * left_scale + right.y * right_scale,
        left.z * left_scale + right.z * right_scale,
        left.w * left_scale + right.w * right_scale,
    };
}

}  // namespace

Quaternion normalize(Quaternion value) noexcept {
    const float length_squared = dot(value, value);
    if (!(length_squared > 0.0F) || !std::isfinite(length_squared)) {
        return {};
    }

    const float inverse_length = 1.0F / std::sqrt(length_squared);
    return {
        value.x * inverse_length,
        value.y * inverse_length,
        value.z * inverse_length,
        value.w * inverse_length,
    };
}

Quaternion slerp_shortest(Quaternion from, Quaternion to, float alpha) noexcept {
    const float clamped_alpha = std::clamp(alpha, 0.0F, 1.0F);
    from = normalize(from);
    to = normalize(to);

    float cosine = dot(from, to);
    if (cosine < 0.0F) {
        to = {-to.x, -to.y, -to.z, -to.w};
        cosine = -cosine;
    }

    cosine = std::clamp(cosine, -1.0F, 1.0F);
    if (cosine > 0.9995F) {
        return normalize(scale_add(from, 1.0F - clamped_alpha, to, clamped_alpha));
    }

    const float angle = std::acos(cosine);
    const float sine = std::sin(angle);
    if (!(std::abs(sine) > 1.0e-7F)) {
        return from;
    }

    const float from_scale = std::sin((1.0F - clamped_alpha) * angle) / sine;
    const float to_scale = std::sin(clamped_alpha * angle) / sine;
    return normalize(scale_add(from, from_scale, to, to_scale));
}

Pose interpolate_pose(const Pose& from, const Pose& to, float alpha) noexcept {
    const float clamped_alpha = std::clamp(alpha, 0.0F, 1.0F);
    return {
        slerp_shortest(from.orientation, to.orientation, clamped_alpha),
        {
            from.position.x + (to.position.x - from.position.x) * clamped_alpha,
            from.position.y + (to.position.y - from.position.y) * clamped_alpha,
            from.position.z + (to.position.z - from.position.z) * clamped_alpha,
        },
    };
}

Pose pose_at_fraction(const Pose& from, const Pose& to, float fraction) noexcept {
    const float clamped = std::isfinite(fraction) ? std::max(fraction, 0.0F) : 0.0F;
    const Quaternion start = normalize(from.orientation);
    const Quaternion end = normalize(to.orientation);
    // The rotation from `from` to `to` in from's own frame, the short way.
    Quaternion delta = normalize({
        start.w * end.x - start.x * end.w - start.y * end.z + start.z * end.y,
        start.w * end.y + start.x * end.z - start.y * end.w - start.z * end.x,
        start.w * end.z - start.x * end.y + start.y * end.x - start.z * end.w,
        start.w * end.w + start.x * end.x + start.y * end.y + start.z * end.z,
    });
    if (delta.w < 0.0F) {
        delta = {-delta.x, -delta.y, -delta.z, -delta.w};
    }
    const float axis_length =
        std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    // Below a few microradians the axis is noise; the small-angle form is exact
    // there to float precision.
    Quaternion turned = normalize({delta.x * clamped, delta.y * clamped, delta.z * clamped, 1.0F});
    if (axis_length > 1.0e-6F) {
        const float half_angle = std::atan2(axis_length, delta.w) * clamped;
        const float axis_scale = std::sin(half_angle) / axis_length;
        turned = {delta.x * axis_scale, delta.y * axis_scale, delta.z * axis_scale,
                  std::cos(half_angle)};
    }
    return {
        normalize({
            start.w * turned.x + start.x * turned.w + start.y * turned.z - start.z * turned.y,
            start.w * turned.y - start.x * turned.z + start.y * turned.w + start.z * turned.x,
            start.w * turned.z + start.x * turned.y - start.y * turned.x + start.z * turned.w,
            start.w * turned.w - start.x * turned.x - start.y * turned.y - start.z * turned.z,
        }),
        {
            from.position.x + (to.position.x - from.position.x) * clamped,
            from.position.y + (to.position.y - from.position.y) * clamped,
            from.position.z + (to.position.z - from.position.z) * clamped,
        },
    };
}

float rotation_angle(Quaternion from, Quaternion to) noexcept {
    from = normalize(from);
    to = normalize(to);
    // A quaternion and its negation are one rotation: take the nearer.
    const float sign = dot(from, to) < 0.0F ? -1.0F : 1.0F;
    const Quaternion difference = scale_add(from, 1.0F, to, -sign);
    // From the chord between them, which resolves thousandths of a degree,
    // rather than the acos of a dot product within a float ulp of 1, which
    // does not: the chord is 2 sin(angle / 4).
    return 4.0F * std::asin(std::min(std::sqrt(dot(difference, difference)) * 0.5F, 1.0F));
}

std::optional<TimedPose> midpoint(const TimedPose& previous, const TimedPose& current) noexcept {
    if (current.time_ns <= previous.time_ns) {
        return std::nullopt;
    }

    const std::int64_t delta = current.time_ns - previous.time_ns;
    return TimedPose{
        previous.time_ns + delta / 2,
        interpolate_pose(previous.pose, current.pose, 0.5F),
    };
}

}  // namespace xrfg

// SPDX-License-Identifier: GPL-3.0-or-later
// Pure functions from Vehkernel's frames to ROS2's (ADR-0014, REP 103, spec 0015):
// the local level frame north-east-down (NED) to east-north-up (ENU), and the body
// frame forward-right-down (FRD) to forward-left-up (FLU). They know neither
// rclcpp nor the schema: plain arrays in, plain arrays out, in double precision.
//
// Quaternions are Hamilton, w first, as the schema's `q` fields are ("w x y z"):
// `q_a_b` rotates a vector from frame b into frame a. Covariances are row-major
// 3x3 matrices of a vector's or a fixed-axis rotation's variances, transformed
// as R * S * R^T.
#pragma once

#include <array>
#include <cmath>
#include <cstddef>

namespace vk::bridge::frames {

using Vec3 = std::array<double, 3>;
using Mat3 = std::array<double, 9>;  // row-major
struct Quat {
    double w = 1.0;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

// The rotation matrices that take a vector from one frame to the other. Both are
// proper rotations, so a covariance of a vector or of a rotation vector
// transforms with the same matrix. NED to ENU swaps north and east and flips
// down to up; FRD to FLU flips right to left and down to up. Each is its own inverse.
inline constexpr Mat3 enu_from_ned{0, 1, 0, 1, 0, 0, 0, 0, -1};
inline constexpr Mat3 flu_from_frd{1, 0, 0, 0, -1, 0, 0, 0, -1};

[[nodiscard]] constexpr Vec3 rotate_vector(const Mat3& r, const Vec3& v)
{
    return {r[0] * v[0] + r[1] * v[1] + r[2] * v[2], r[3] * v[0] + r[4] * v[1] + r[5] * v[2],
            r[6] * v[0] + r[7] * v[1] + r[8] * v[2]};
}

[[nodiscard]] constexpr Mat3 transpose(const Mat3& m)
{
    return {m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]};
}

[[nodiscard]] constexpr Mat3 multiply(const Mat3& a, const Mat3& b)
{
    Mat3 out{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            for (std::size_t k = 0; k < 3; ++k) {
                out[row * 3 + column] += a[row * 3 + k] * b[k * 3 + column];
            }
        }
    }
    return out;
}

// R * S * R^T.
[[nodiscard]] constexpr Mat3 rotate_covariance(const Mat3& r, const Mat3& s)
{
    return multiply(multiply(r, s), transpose(r));
}

[[nodiscard]] constexpr Quat multiply(const Quat& a, const Quat& b)
{
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}

[[nodiscard]] inline Quat normalized(const Quat& q)
{
    const double norm = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (!(norm > 0.0)) {
        return {};  // not a rotation: the identity, which every consumer accepts
    }
    return {q.w / norm, q.x / norm, q.y / norm, q.z / norm};
}

// The unit quaternion of the same rotation with a non-negative w, so that a
// rotation has one quaternion and a round trip gives back what went in.
[[nodiscard]] inline Quat canonical(const Quat& q)
{
    const Quat n = normalized(q);
    return n.w < 0.0 ? Quat{-n.w, -n.x, -n.y, -n.z} : n;
}

// Vectors: a position or a velocity of the local level frame.
[[nodiscard]] constexpr Vec3 ned_to_enu(const Vec3& v) { return rotate_vector(enu_from_ned, v); }
[[nodiscard]] constexpr Vec3 enu_to_ned(const Vec3& v) { return rotate_vector(transpose(enu_from_ned), v); }
// Vectors of the body: a velocity, an angular rate or a specific force.
[[nodiscard]] constexpr Vec3 frd_to_flu(const Vec3& v) { return rotate_vector(flu_from_frd, v); }
[[nodiscard]] constexpr Vec3 flu_to_frd(const Vec3& v) { return rotate_vector(transpose(flu_from_frd), v); }

[[nodiscard]] constexpr Mat3 covariance_ned_to_enu(const Mat3& s) { return rotate_covariance(enu_from_ned, s); }
[[nodiscard]] constexpr Mat3 covariance_enu_to_ned(const Mat3& s)
{
    return rotate_covariance(transpose(enu_from_ned), s);
}
[[nodiscard]] constexpr Mat3 covariance_frd_to_flu(const Mat3& s) { return rotate_covariance(flu_from_frd, s); }
[[nodiscard]] constexpr Mat3 covariance_flu_to_frd(const Mat3& s)
{
    return rotate_covariance(transpose(flu_from_frd), s);
}

// The two frame changes as quaternions: NED to ENU is a half turn about the axis
// (1, 1, 0) / sqrt(2), FRD to FLU a half turn about x.
inline constexpr double half_sqrt2 = 0.70710678118654752440;
inline constexpr Quat enu_from_ned_q{0.0, half_sqrt2, half_sqrt2, 0.0};
inline constexpr Quat flu_from_frd_q{0.0, 1.0, 0.0, 0.0};

// The attitude of the body: FRD to NED (what the MCU publishes) becomes FLU to
// ENU, q_enu_flu = q_enu_ned * q_ned_frd * q_frd_flu. Without the magnetometer
// the MCU's heading zero is its start, so a vehicle starting level and facing
// "north" of the MCU faces +y of ENU.
[[nodiscard]] inline Quat attitude_ned_frd_to_enu_flu(const Quat& q_ned_frd)
{
    // Both frame changes are half turns, so each is its own inverse.
    return canonical(multiply(multiply(enu_from_ned_q, q_ned_frd), flu_from_frd_q));
}

[[nodiscard]] inline Quat attitude_enu_flu_to_ned_frd(const Quat& q_enu_flu)
{
    return canonical(multiply(multiply(enu_from_ned_q, q_enu_flu), flu_from_frd_q));
}

}  // namespace vk::bridge::frames

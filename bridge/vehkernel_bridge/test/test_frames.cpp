// SPDX-License-Identifier: GPL-3.0-or-later
// L1 tests of the frame conversions (ADR-0014, ADR-0018, spec 0015 seam D): NED to
// ENU and FRD to FLU, for quaternions, vectors and covariances, each way.
#include <vehkernel_bridge/frames.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

namespace vk::bridge::frames {
namespace {

constexpr double tolerance = 1e-12;
constexpr double pi = std::numbers::pi;

Vec3 rotate(const Quat& q, const Vec3& v)
{
    const Quat vector{0.0, v[0], v[1], v[2]};
    const Quat conjugate{q.w, -q.x, -q.y, -q.z};
    const Quat r = multiply(multiply(q, vector), conjugate);
    return {r.x, r.y, r.z};
}

Quat about(const Vec3& axis, double angle)
{
    const double s = std::sin(angle / 2.0);
    return {std::cos(angle / 2.0), axis[0] * s, axis[1] * s, axis[2] * s};
}

void expect_near(const Vec3& a, const Vec3& b)
{
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_NEAR(a[i], b[i], tolerance) << "component " << i;
    }
}

void expect_near(const Mat3& a, const Mat3& b)
{
    for (std::size_t i = 0; i < 9; ++i) {
        EXPECT_NEAR(a[i], b[i], tolerance) << "entry " << i;
    }
}

void expect_near(const Quat& a, const Quat& b)
{
    EXPECT_NEAR(a.w, b.w, tolerance);
    EXPECT_NEAR(a.x, b.x, tolerance);
    EXPECT_NEAR(a.y, b.y, tolerance);
    EXPECT_NEAR(a.z, b.z, tolerance);
}

// North-east-down covariance with every entry different.
constexpr Mat3 sample_covariance{1.0, 0.5, 0.2, 0.5, 2.0, 0.1, 0.2, 0.1, 3.0};

TEST(Frames, IdentityFacesPlusYOfEnu)
{
    // Level and facing the MCU's heading zero, the vehicle faces north in the
    // local level frame, which is +y of east-north-up: a quarter turn about up.
    const Quat q = attitude_ned_frd_to_enu_flu(Quat{});
    expect_near(q, Quat{std::cos(pi / 4), 0.0, 0.0, std::sin(pi / 4)});
    expect_near(rotate(q, {1.0, 0.0, 0.0}), {0.0, 1.0, 0.0});  // forward
    expect_near(rotate(q, {0.0, 1.0, 0.0}), {-1.0, 0.0, 0.0});  // left is west
    expect_near(rotate(q, {0.0, 0.0, 1.0}), {0.0, 0.0, 1.0});  // up stays up
}

TEST(Frames, YawOfNinetyDegreesTurnsToEast)
{
    // A yaw of +90 degrees about down faces east, which is +x of ENU: no rotation.
    const Quat q = attitude_ned_frd_to_enu_flu(about({0.0, 0.0, 1.0}, pi / 2));
    expect_near(q, Quat{});
    // Yawed a further 90 degrees the vehicle faces south, -y of ENU.
    const Quat south = attitude_ned_frd_to_enu_flu(about({0.0, 0.0, 1.0}, pi));
    expect_near(rotate(south, {1.0, 0.0, 0.0}), {0.0, -1.0, 0.0});
}

TEST(Frames, PitchUpRaisesTheNoseInBothFrames)
{
    // Nose up by 30 degrees, facing north: about +y (right) in FRD.
    const double angle = pi / 6;
    const Quat q = attitude_ned_frd_to_enu_flu(about({0.0, 1.0, 0.0}, angle));
    // Forward in the local level frame is (cos, 0, -sin) north-east-down: up is positive in ENU.
    expect_near(rotate(q, {1.0, 0.0, 0.0}), {0.0, std::cos(angle), std::sin(angle)});
}

TEST(Frames, QuaternionsAgreeWithVectors)
{
    // A vector rotated in FRD then turned to ENU is the FLU vector rotated by the converted quaternion.
    const Quat q = normalized(Quat{0.4, -0.3, 0.7, 0.2});
    const Vec3 v{0.3, -1.2, 2.5};
    expect_near(rotate(attitude_ned_frd_to_enu_flu(q), frd_to_flu(v)), ned_to_enu(rotate(q, v)));
}

TEST(Frames, QuaternionRoundTrip)
{
    const Quat q = canonical(Quat{0.4, -0.3, 0.7, 0.2});
    expect_near(attitude_enu_flu_to_ned_frd(attitude_ned_frd_to_enu_flu(q)), q);
    expect_near(attitude_ned_frd_to_enu_flu(attitude_enu_flu_to_ned_frd(q)), q);
}

TEST(Frames, VectorsEachWay)
{
    expect_near(ned_to_enu({1.0, 2.0, 3.0}), {2.0, 1.0, -3.0});  // north 1, east 2, down 3
    expect_near(enu_to_ned({2.0, 1.0, -3.0}), {1.0, 2.0, 3.0});
    expect_near(frd_to_flu({1.0, 2.0, 3.0}), {1.0, -2.0, -3.0});  // forward 1, right 2, down 3
    expect_near(flu_to_frd({1.0, -2.0, -3.0}), {1.0, 2.0, 3.0});
}

TEST(Frames, VectorRoundTrip)
{
    const Vec3 v{0.7, -1.9, 4.2};
    expect_near(enu_to_ned(ned_to_enu(v)), v);
    expect_near(flu_to_frd(frd_to_flu(v)), v);
}

TEST(Frames, CovarianceNedToEnu)
{
    // x of ENU is east (NED's y), y is north (NED's x), z is up (minus down).
    const Mat3 enu{2.0, 0.5, -0.1, 0.5, 1.0, -0.2, -0.1, -0.2, 3.0};
    expect_near(covariance_ned_to_enu(sample_covariance), enu);
    expect_near(covariance_enu_to_ned(enu), sample_covariance);
}

TEST(Frames, CovarianceFrdToFlu)
{
    // y and z change sign, so the entries pairing x with them, and y with z, do not.
    const Mat3 flu{1.0, -0.5, -0.2, -0.5, 2.0, 0.1, -0.2, 0.1, 3.0};
    expect_near(covariance_frd_to_flu(sample_covariance), flu);
    expect_near(covariance_flu_to_frd(flu), sample_covariance);
}

TEST(Frames, CovarianceRoundTrip)
{
    expect_near(covariance_enu_to_ned(covariance_ned_to_enu(sample_covariance)), sample_covariance);
    expect_near(covariance_flu_to_frd(covariance_frd_to_flu(sample_covariance)), sample_covariance);
}

TEST(Frames, DegenerateQuaternionBecomesIdentity)
{
    expect_near(normalized(Quat{0.0, 0.0, 0.0, 0.0}), Quat{});
}

}  // namespace
}  // namespace vk::bridge::frames

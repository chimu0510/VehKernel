// SPDX-License-Identifier: GPL-3.0-or-later
// L1 tests of the IMU noise reader (ADR-0012, ADR-0018, spec 0015 seam D): the
// requests it sends and the replies it reads, with a fake send function.
#include <vehkernel_bridge/imu_noise.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace vk::bridge {
namespace {

using vk::link::BodyWriter;
using vk::link::Frame;
using vk::link::FrameType;

constexpr std::uint64_t ms = 1000;

struct Sent {
    FrameType type;
    std::string name;
};

class ImuNoiseTest : public ::testing::Test {
protected:
    ImuNoiseTest()
        : reader_(
              [this](FrameType type, std::span<const std::uint8_t> body) {
                  const std::size_t length = body.empty() ? 0 : body[0];
                  sent_.push_back({type, std::string(reinterpret_cast<const char*>(body.data()) + 1, length)});
                  return true;
              },
              [this](const std::string& text) { log_.push_back(text); }, 500 * ms)
    {
    }

    // A param_value frame, as the MCU sends it.
    static bool reply(ImuNoiseReader& reader, const std::string& name, float value, std::uint8_t type, std::uint64_t now)
    {
        std::array<std::uint8_t, 64> storage{};
        BodyWriter writer(storage);
        writer.u16(0);
        writer.u16(1);
        writer.u8(type);
        writer.float32(value);
        writer.u8(0);
        writer.u8(static_cast<std::uint8_t>(name.size()));
        writer.bytes({reinterpret_cast<const std::uint8_t*>(name.data()), name.size()});
        return reader.on_frame(Frame{5, FrameType::ParamValue, 0, writer.written()}, now);
    }

    static bool reject(ImuNoiseReader& reader, const std::string& name, std::uint64_t now)
    {
        std::array<std::uint8_t, 64> storage{};
        BodyWriter writer(storage);
        writer.u8(1);
        writer.u8(static_cast<std::uint8_t>(name.size()));
        writer.bytes({reinterpret_cast<const std::uint8_t*>(name.data()), name.size()});
        return reader.on_frame(Frame{5, FrameType::ParamReject, 0, writer.written()}, now);
    }

    ImuNoiseReader reader_;
    std::vector<Sent> sent_;
    std::vector<std::string> log_;
};

TEST_F(ImuNoiseTest, AsksForBothAtTheStartAndReadsThem)
{
    reader_.restart(0);
    ASSERT_EQ(sent_.size(), 2U);
    EXPECT_EQ(sent_[0].type, FrameType::ParamGet);
    EXPECT_EQ(sent_[0].name, "EKF_GYR_NOISE");
    EXPECT_EQ(sent_[1].name, "EKF_ACC_NOISE");
    EXPECT_FALSE(reader_.complete());

    EXPECT_TRUE(reply(reader_, "EKF_GYR_NOISE", 0.0011F, 2, ms));
    EXPECT_TRUE(reply(reader_, "EKF_ACC_NOISE", 2.0F, 2, ms));
    const auto gyro = reader_.gyro();
    const auto accel = reader_.accel();
    ASSERT_TRUE(gyro.has_value());
    ASSERT_TRUE(accel.has_value());
    EXPECT_FLOAT_EQ(gyro.value_or(-1.0F), 0.0011F);
    EXPECT_FLOAT_EQ(accel.value_or(-1.0F), 2.0F);
    EXPECT_TRUE(reader_.complete());
    EXPECT_EQ(log_.size(), 2U);

    // Nothing more is asked once both are known.
    reader_.poll(10'000 * ms);
    EXPECT_EQ(sent_.size(), 2U);
}

TEST_F(ImuNoiseTest, AsksAgainOnlyForWhatIsMissingAfterTheRetryInterval)
{
    reader_.restart(0);
    reader_.poll(100 * ms);
    EXPECT_EQ(sent_.size(), 2U);  // too early
    EXPECT_TRUE(reply(reader_, "EKF_ACC_NOISE", 2.0F, 2, 100 * ms));
    reader_.poll(700 * ms);
    ASSERT_EQ(sent_.size(), 3U);
    EXPECT_EQ(sent_[2].name, "EKF_GYR_NOISE");
    reader_.poll(1300 * ms);
    EXPECT_EQ(sent_.size(), 4U);
}

TEST_F(ImuNoiseTest, ANewConnectionForgetsTheValuesAndAsksAgain)
{
    reader_.restart(0);
    EXPECT_TRUE(reply(reader_, "EKF_GYR_NOISE", 0.0011F, 2, ms));
    EXPECT_TRUE(reply(reader_, "EKF_ACC_NOISE", 2.0F, 2, ms));
    reader_.restart(5000 * ms);
    EXPECT_FALSE(reader_.gyro().has_value());
    EXPECT_FALSE(reader_.accel().has_value());
    EXPECT_EQ(sent_.size(), 4U);
}

TEST_F(ImuNoiseTest, IgnoresOtherParametersAndFrames)
{
    reader_.restart(0);
    EXPECT_FALSE(reply(reader_, "ATT_KP", 1.0F, 2, ms));
    EXPECT_FALSE(reject(reader_, "ATT_KP", ms));
    EXPECT_FALSE(reader_.on_frame(Frame{5, FrameType::ModeReply, 0, {}}, ms));
    EXPECT_FALSE(reader_.gyro().has_value());
}

TEST_F(ImuNoiseTest, ARefusalLeavesTheNoiseUnknownAndStopsAsking)
{
    reader_.restart(0);
    EXPECT_TRUE(reject(reader_, "EKF_GYR_NOISE", ms));
    EXPECT_TRUE(reply(reader_, "EKF_ACC_NOISE", 2.0F, 2, ms));
    EXPECT_FALSE(reader_.gyro().has_value());
    EXPECT_TRUE(reader_.complete());
    reader_.poll(10'000 * ms);
    EXPECT_EQ(sent_.size(), 2U);
}

TEST_F(ImuNoiseTest, AnUnusableValueIsNotTaken)
{
    reader_.restart(0);
    EXPECT_TRUE(reply(reader_, "EKF_GYR_NOISE", -1.0F, 2, ms));
    EXPECT_TRUE(reply(reader_, "EKF_ACC_NOISE", 2.0F, 1, ms));  // an int32, not a float
    EXPECT_FALSE(reader_.gyro().has_value());
    EXPECT_FALSE(reader_.accel().has_value());
}

TEST_F(ImuNoiseTest, DoesNothingBeforeTheFirstConnection)
{
    reader_.poll(10'000 * ms);
    EXPECT_TRUE(sent_.empty());
    EXPECT_FALSE(reply(reader_, "EKF_GYR_NOISE", 0.0011F, 2, ms));
}

}  // namespace
}  // namespace vk::bridge

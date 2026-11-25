// SPDX-License-Identifier: GPL-3.0-or-later
// `ros2 run vehkernel_bridge bridge` (ADR-0027, spec 0015).
#include <vehkernel_bridge/bridge_node.hpp>

#include <rclcpp/rclcpp.hpp>

#include <exception>
#include <memory>

int main(int argc, char** argv)
{
    rclcpp::init(argc, argv);
    int status = 0;
    try {
        rclcpp::spin(std::make_shared<vk::bridge::BridgeNode>());
    } catch (const std::exception& error) {
        RCLCPP_FATAL(rclcpp::get_logger("bridge"), "%s", error.what());
        status = 1;
    }
    rclcpp::shutdown();
    return status;
}

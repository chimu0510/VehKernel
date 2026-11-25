# SPDX-License-Identifier: GPL-3.0-or-later
# L5 test of the Bridge against SITL (ADR-0018, ADR-0027, spec 0015, Seam E).
#
# SITL with the built-in ground vehicle and the Bridge on UDP:
#
# - /odom, /tf, /imu/data and /vehkernel/vehicle_status arrive.
# - set_mode COMPANION and arm succeed.
# - /cmd_vel forward moves /odom forward, along the heading.
# - Silence on /cmd_vel for longer than COM_DISARM_IDLE (10 s) keeps the vehicle armed and stopped.
# - Killing the Bridge disarms SITL: a second Bridge, started afterwards, reports the setpoint timeout.
#   SITL's Link accepts one peer, so every command goes through the first Bridge, and the second one
#   only reads: it sends no zero setpoints until it learns its own Link instance.
#
# The test needs a SITL build: VK_SITL_EXECUTABLE names its executable (build/sitl/app/vehkernel-sitl).
# Without it the test is skipped, so a plain `colcon test` on a machine with no SITL build passes.
import math
import os
import signal
import socket
import subprocess
import time
import unittest

import launch
import launch.actions
import launch_testing.actions
import rclpy
from ament_index_python.packages import get_package_prefix, get_package_share_directory
from geometry_msgs.msg import TwistStamped
from nav_msgs.msg import Odometry
from rclpy.executors import SingleThreadedExecutor
from rclpy.qos import QoSProfile, ReliabilityPolicy, qos_profile_sensor_data
from sensor_msgs.msg import Imu
from std_srvs.srv import SetBool
from tf2_msgs.msg import TFMessage
from vehkernel_msgs.msg import VehicleStatus
from vehkernel_msgs.srv import SetMode

SITL = os.environ.get("VK_SITL_EXECUTABLE", "")
SKIP_REASON = "VK_SITL_EXECUTABLE does not name a SITL executable: build the sitl preset and set it"

# COM_DISARM_IDLE's default. SITL's parameters stay at their defaults, so the silence lasts longer
# than this, and the Bridge's zero setpoints must keep the vehicle armed through it.
DISARM_IDLE_S = 10.0
FORWARD = 0.2  # m/s
DRIVE_S = 3.0
MOVED_M = 0.2  # forward distance that counts as moving after DRIVE_S at FORWARD, with the ramp
STOPPED_M = 0.03  # distance moved while the silence is judged
# A message count over the first 2 s, well below each topic's rate, so that a loaded runner passes.
MIN_ODOM, MIN_IMU, MIN_STATUS = 20, 50, 5


def free_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


UDP_PORT = free_udp_port()


def generate_test_description():
    actions = []
    if SITL and os.access(SITL, os.X_OK):
        actions.append(launch.actions.ExecuteProcess(
            cmd=[SITL, "--duration", "0", "--udp", f"127.0.0.1:{UDP_PORT}"], name="sitl", output="screen"))
    else:
        # launch_testing counts the skipped tests as failed when the launch ends before they are
        # reported, and with no process to wait for it ends at once. Keep it open until they are.
        actions.append(launch.actions.ExecuteProcess(cmd=["sleep", "600"], name="no_sitl", output="screen"))
    actions.append(launch_testing.actions.ReadyToTest())
    return launch.LaunchDescription(actions)


def yaw_of(odometry: Odometry) -> float:
    q = odometry.pose.pose.orientation
    return math.atan2(2.0 * (q.w * q.z + q.x * q.y), 1.0 - 2.0 * (q.y * q.y + q.z * q.z))


@unittest.skipUnless(SITL and os.access(SITL, os.X_OK), SKIP_REASON)
class TestSitlBridge(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()
        cls.node = rclpy.create_node("sitl_bridge_test")
        cls.executor = SingleThreadedExecutor()
        cls.executor.add_node(cls.node)
        cls.odometry: list[Odometry] = []
        cls.imu: list[Imu] = []
        cls.status: list[VehicleStatus] = []
        cls.transforms: list = []
        cls.processes: list[subprocess.Popen] = []
        # The subscriptions exist before the Bridge starts, so no message is missed.
        reliable = QoSProfile(depth=100, reliability=ReliabilityPolicy.RELIABLE)
        cls.node.create_subscription(Odometry, "odom", cls.odometry.append, reliable)
        cls.node.create_subscription(Imu, "imu/data", cls.imu.append, qos_profile_sensor_data)
        cls.node.create_subscription(VehicleStatus, "vehkernel/vehicle_status", cls.status.append, reliable)
        cls.node.create_subscription(TFMessage, "tf", lambda m: cls.transforms.extend(m.transforms), reliable)
        cls.cmd_vel = cls.node.create_publisher(TwistStamped, "cmd_vel", 10)
        cls.set_mode = cls.node.create_client(SetMode, "vehkernel/set_mode")
        cls.arm = cls.node.create_client(SetBool, "vehkernel/arm")

    @classmethod
    def tearDownClass(cls):
        for process in cls.processes:
            stop(process)
        cls.executor.remove_node(cls.node)
        cls.node.destroy_node()
        rclpy.shutdown()

    @classmethod
    def spin_for(cls, seconds: float, each=None) -> None:
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            cls.executor.spin_once(timeout_sec=0.02)
            if each is not None:
                each()

    @classmethod
    def spin_until(cls, condition, timeout_s: float) -> bool:
        end = time.monotonic() + timeout_s
        while time.monotonic() < end:
            cls.executor.spin_once(timeout_sec=0.02)
            if condition():
                return True
        return condition()

    @classmethod
    def call(cls, client, request, timeout_s: float = 10.0):
        assert client.wait_for_service(timeout_sec=timeout_s), f"{client.srv_name} is not offered"
        future = client.call_async(request)
        assert cls.spin_until(future.done, timeout_s), f"{client.srv_name} did not answer in {timeout_s} s"
        return future.result()

    @classmethod
    def start_bridge(cls, command: list[str]) -> subprocess.Popen:
        # Own process group, so that stopping it stops the launch file's node too.
        process = subprocess.Popen(command, start_new_session=True)
        cls.processes.append(process)
        return process

    def test_01_outputs_arrive(self):
        bridge = os.path.join(get_package_prefix("vehkernel_bridge"), "lib", "vehkernel_bridge", "bridge")
        type(self).bridge = self.start_bridge([bridge, "--ros-args", "-p", "transport:=udp", "-p",
                                               f"udp_port:={UDP_PORT}"])
        self.assertTrue(self.spin_until(lambda: self.status, 30.0), "no /vehkernel/vehicle_status: no handshake")
        began = len(self.odometry), len(self.imu), len(self.status)
        self.spin_for(2.0)
        self.assertGreaterEqual(len(self.odometry) - began[0], MIN_ODOM, "/odom")
        self.assertGreaterEqual(len(self.imu) - began[1], MIN_IMU, "/imu/data")
        self.assertGreaterEqual(len(self.status) - began[2], MIN_STATUS, "/vehkernel/vehicle_status")
        self.assertTrue(any(t.header.frame_id == "odom" and t.child_frame_id == "base_link"
                            for t in self.transforms), "no TF odom -> base_link")
        self.assertEqual(self.odometry[-1].header.frame_id, "odom")
        self.assertEqual(self.odometry[-1].child_frame_id, "base_link")
        self.assertEqual(self.imu[-1].header.frame_id, "base_link")
        self.assertEqual(self.status[-1].mode, VehicleStatus.MODE_STANDBY)
        self.assertFalse(self.status[-1].armed)

    def test_02_companion_and_arm(self):
        mode = self.call(self.set_mode, SetMode.Request(mode_name="COMPANION"))
        self.assertTrue(mode.accepted, f"COMPANION refused: {mode.reason}")
        arm = self.call(self.arm, SetBool.Request(data=True))
        self.assertTrue(arm.success, f"arming refused: {arm.message}")
        self.assertTrue(self.spin_until(lambda: self.status[-1].armed, 5.0), "the status does not say armed")
        self.assertEqual(self.status[-1].mode, VehicleStatus.MODE_COMPANION)

    def test_03_cmd_vel_moves_odom_forward(self):
        before = self.odometry[-1]
        # The Bridge learns its Link from the first status after COMPANION; give it one.
        self.spin_for(0.5)

        def publish():
            twist = TwistStamped()
            twist.header.stamp = self.node.get_clock().now().to_msg()
            twist.twist.linear.x = FORWARD
            self.cmd_vel.publish(twist)

        end = time.monotonic() + DRIVE_S
        while time.monotonic() < end:
            publish()
            self.spin_for(0.05)
        after = self.odometry[-1]
        dx = after.pose.pose.position.x - before.pose.pose.position.x
        dy = after.pose.pose.position.y - before.pose.pose.position.y
        yaw = yaw_of(before)
        forward = dx * math.cos(yaw) + dy * math.sin(yaw)
        sideways = -dx * math.sin(yaw) + dy * math.cos(yaw)
        self.assertGreater(forward, MOVED_M,
                           f"/odom moved {forward:.3f} m along the heading (dx {dx:.3f}, dy {dy:.3f})")
        self.assertLess(abs(sideways), 0.1, f"/odom moved {sideways:.3f} m sideways")
        self.assertGreater(after.twist.twist.linear.x, 0.05, "/odom reports no forward speed while driving")
        self.assertTrue(self.status[-1].armed)

    def test_04_silence_keeps_armed_and_stopped(self):
        # A second after the last /cmd_vel the vehicle has stopped; the silence then runs past
        # COM_DISARM_IDLE, which would disarm it were the zero setpoints counted as idle.
        self.spin_for(1.5)
        settled = self.odometry[-1]
        armed_since = len(self.status)
        self.spin_for(DISARM_IDLE_S + 1.5)
        after = self.odometry[-1]
        moved = math.hypot(after.pose.pose.position.x - settled.pose.pose.position.x,
                           after.pose.pose.position.y - settled.pose.pose.position.y)
        self.assertLess(moved, STOPPED_M, f"/odom moved {moved:.3f} m during the silence")
        self.assertLess(abs(after.twist.twist.linear.x), 0.03)
        self.assertGreater(len(self.status), armed_since, "no vehicle status during the silence")
        for status in self.status[armed_since:]:
            self.assertTrue(status.armed, "the vehicle disarmed during the silence")
            self.assertEqual(status.mode, VehicleStatus.MODE_COMPANION)

    def test_05_killing_the_bridge_disarms(self):
        os.killpg(self.bridge.pid, signal.SIGKILL)
        self.bridge.wait(timeout=10)
        # COM_SP_TIMEOUT is 0.5 s; the second Bridge needs the vehicle to have timed out already
        # or it would read a stale armed status, so it waits for a disarmed one below.
        self.spin_for(1.5)
        self.status.clear()
        launch_file = os.path.join(get_package_share_directory("vehkernel_bridge"), "launch", "bridge.launch.py")
        self.start_bridge(["ros2", "launch", launch_file, "transport:=udp", f"udp_port:={UDP_PORT}"])
        self.assertTrue(self.spin_until(lambda: any(not s.armed for s in self.status), 30.0),
                        "the second Bridge reads no disarmed status: SITL stayed armed or did not answer")
        status = next(s for s in self.status if not s.armed)
        self.assertEqual(status.disarm_reason, VehicleStatus.DISARM_SETPOINT_TIMEOUT)
        self.assertEqual(status.mode, VehicleStatus.MODE_COMPANION)
        # The second Bridge learned no Link instance and sent no zero setpoints: still disarmed.
        self.spin_for(1.0)
        self.assertFalse(any(s.armed for s in self.status))


def stop(process: subprocess.Popen) -> None:
    if process.poll() is not None:
        return
    for sig, wait_s in ((signal.SIGINT, 5.0), (signal.SIGKILL, 5.0)):
        try:
            os.killpg(process.pid, sig)
            process.wait(timeout=wait_s)
            return
        except ProcessLookupError:
            return
        except subprocess.TimeoutExpired:
            continue

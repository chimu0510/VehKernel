# SPDX-License-Identifier: GPL-3.0-or-later
# The vehicle's mode, arming state and what an application needs to react to
# (ADR-0005, spec 0015), from the MCU's vehicle_status, on /vehkernel/vehicle_status.
# The constants are the enum values of the schema's enum files; the Bridge's tests
# compare them with the generated ones, so they cannot drift.

# The MCU's time of publication of the vehicle status, on the companion computer's
# clock. The status is sent as the latest value at the topic map's 5 Hz, so it may
# be up to one Link period (200 ms) old when it arrives.
std_msgs/Header header

uint8 MODE_STANDBY=0
uint8 MODE_TEST=1
uint8 MODE_MANUAL=2
uint8 MODE_COMPANION=3
uint8 mode

bool armed

# The source in control, SOURCE_NONE for none.
uint8 SOURCE_NONE=255
uint8 source

bool emergency_stop

uint8 ACTUATOR_POWER_NOT_AVAILABLE=0
uint8 ACTUATOR_POWER_UNPOWERED=1
uint8 ACTUATOR_POWER_SETTLING=2
uint8 ACTUATOR_POWER_POWERED=3
uint8 actuator_power

# The last arming request's refusal, ARMING_OK for none.
uint8 ARMING_OK=0
uint8 ARMING_BOARD_NOT_VERIFIED=1
uint8 ARMING_EMERGENCY_STOP=2
uint8 ARMING_WRONG_MODE=3
uint8 ARMING_NOT_IN_CONTROL=4
uint8 ARMING_SETPOINTS_ACTIVE=5
uint8 ARMING_BRINGUP_RUNNING=6
uint8 ARMING_ACTUATOR_POWER_OFF=7
uint8 ARMING_ACTUATORS_NOT_STARTED=8
uint8 ARMING_LOCAL_MENU_OPEN=9
uint8 ARMING_REBOOTING=10
uint8 ARMING_UNEXPECTED_RESET=11
uint8 ARMING_RC_NOT_CONNECTED=12
uint8 ARMING_STICKS_NOT_CENTRED=13
uint8 arming_refusal

# Why the vehicle last disarmed, DISARM_NONE for never.
uint8 DISARM_NONE=0
uint8 DISARM_REQUEST=1
uint8 DISARM_EMERGENCY_STOP=2
uint8 DISARM_ACTUATOR_POWER_LOST=3
uint8 DISARM_SETPOINT_TIMEOUT=4
uint8 DISARM_IDLE=5
uint8 DISARM_BRINGUP_ABORT=6
uint8 DISARM_RC_LOST=7
uint8 disarm_reason

# What caused the last reset, read at boot.
uint8 RESET_UNKNOWN=0
uint8 RESET_POWER_ON=1
uint8 RESET_PIN=2
uint8 RESET_SOFTWARE=3
uint8 RESET_WATCHDOG=4
uint8 RESET_FAULT=5
uint8 RESET_LOW_POWER=6
uint8 reset_cause

# Setpoints dropped by setpoint arbitration since boot.
uint32 setpoints_rejected

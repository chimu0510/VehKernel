# SPDX-License-Identifier: GPL-3.0-or-later
# The running firmware's identity (ADR-0023), from the MCU's firmware_info, on
# /vehkernel/firmware_info. Published when the Bridge connects and when it
# changes, with transient-local durability, so that a node started later gets it.

# The MCU's time of publication, on the companion computer's clock.
std_msgs/Header header

uint8 version_major
uint8 version_minor
uint8 version_patch
uint32 git_hash          # the commit's first 32 bits; 0 when unknown
bool git_dirty           # the build tree had changes not committed
uint16 board_id          # the board definition's ID, 0 in SITL

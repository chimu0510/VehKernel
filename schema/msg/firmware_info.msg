# SPDX-License-Identifier: GPL-3.0-or-later
# The running firmware's identity (ADR-0023), once a second, so a host knows
# which firmware a board runs without reading its Flash.
@rate 1
uint64 timestamp
uint8 version_major
uint8 version_minor
uint8 version_patch
uint32 git_hash                          # the commit's first 32 bits, from the app descriptor on a board; 0 when unknown
bool git_dirty                           # the build tree had changes not committed
uint16 board_id                          # the board definition's ID, 0 in SITL

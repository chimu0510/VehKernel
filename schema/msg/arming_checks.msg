# SPDX-License-Identifier: GPL-3.0-or-later
# The arming checks that would fail now and those skipped (spec 0016,
# ADR-0005), evaluated by the code an arm request runs, without arming. A
# topic of its own so that vehicle_status, which the Bridge requires, keeps
# its schema hash. Published by the commander five times a second and
# whenever the bits change.
@rate 5
uint64 timestamp
uint32 failing                           # bit n set: the check of arming_result value n would refuse an arm request now
uint32 skipped                           # bit n set: that check is configured off or does not apply in this mode

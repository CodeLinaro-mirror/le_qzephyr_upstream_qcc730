# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: Apache-2.0;
board_runner_args(jlink "--device=qcc730evbi" "--speed=4000")
board_runner_args(pyocd "--target=qcc730evbi" "--frequency=10000000")

include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
include(${ZEPHYR_BASE}/boards/common/pyocd.board.cmake)

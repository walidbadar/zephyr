#
# Copyright (c) 2021, Weidmueller Interface GmbH & Co. KG
# SPDX-License-Identifier: Apache-2.0
#

set(SUPPORTED_EMU_PLATFORMS qemu)
set(QEMU_BINARY_SUFFIX xilinx-aarch64)

set(QEMU_CPU_TYPE cortex-a9)

set(QEMU_BOARD_FLAGS
  -machine arm-generic-fdt-7series
  -dtb ${CMAKE_CURRENT_LIST_DIR}/fdt-zynq7000s.dtb
  )

if (CONFIG_SDHC)
  set(SD_IMG_PATH ${ZEPHYR_BINARY_DIR}/sd.img)

  if(NOT EXISTS ${SD_IMG_PATH})
    message(STATUS "Generating blank SD card image at ${SD_IMG_PATH}")
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E env
        dd if=/dev/zero of=${SD_IMG_PATH} bs=1M count=64
      RESULT_VARIABLE SD_IMG_GEN_RESULT
    )
  endif()

  list(APPEND QEMU_BOARD_FLAGS
    -drive file=${SD_IMG_PATH},format=raw,if=sd,index=0
  )
endif()

set(QEMU_KERNEL_OPTION
  "-device;loader,file=\$<TARGET_FILE:\${logical_target_for_zephyr_elf}>,cpu-num=0"
  )

include(${ZEPHYR_BASE}/boards/common/qemu.board.cmake)

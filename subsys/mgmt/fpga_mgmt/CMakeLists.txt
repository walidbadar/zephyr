# Copyright (c) 2026, Altera Corporation.
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(fpga_mgmt.c)
zephyr_library_sources_ifdef(CONFIG_FPGA_MGMT_BACKEND_FS fpga_mgmt_fs.c)

if(CONFIG_FPGA_MGMT_BACKEND_FLASH)
  zephyr_library_sources(fpga_mgmt_flash.c)
  # Only the image header layout is taken from the MCUboot module
  zephyr_library_include_directories(
    ${ZEPHYR_MCUBOOT_MODULE_DIR}/boot/bootutil/include
    ${ZEPHYR_MCUBOOT_MODULE_DIR}/boot/zephyr/include
  )
  if(CONFIG_FPGA_MGMT_BACKEND_FLASH_VERIFY_SHA256 AND CONFIG_MBEDTLS)
    zephyr_library_link_libraries(mbedTLS)
  endif()
endif()

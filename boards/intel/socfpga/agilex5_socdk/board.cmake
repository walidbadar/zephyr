# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_BUILD_WITH_TFA)
  dt_chosen(chosen_sram_path PROPERTY "zephyr,sram")
  dt_reg_addr(RAM_ADDR PATH "${chosen_sram_path}")

  if(CONFIG_AGILEX5_SOCDK_TFA_BOOT_SOURCE_QSPI)
    set(TFA_BOOT_SOURCE "SOCFPGA_BOOT_SOURCE_QSPI=1")
  elseif(CONFIG_AGILEX5_SOCDK_TFA_BOOT_SOURCE_NAND)
    set(TFA_BOOT_SOURCE "SOCFPGA_BOOT_SOURCE_NAND=1")
  else()
    set(TFA_BOOT_SOURCE "SOCFPGA_BOOT_SOURCE_SDMMC=1")
  endif()

  # BL2 loads BL33 from the FIP to PRELOADED_BL33_BASE. NEED_BL33=yes keeps
  # Zephyr in the FIP, which TF-A otherwise omits when PRELOADED_BL33_BASE
  # is set.
  set(TFA_PLAT "agilex5")
  set(TFA_EXTRA_ARGS "${TFA_BOOT_SOURCE};PRELOADED_BL33_BASE=${RAM_ADDR};NEED_BL33=yes")

  if(CONFIG_TFA_MAKE_BUILD_TYPE_DEBUG)
    set(BUILD_FOLDER "debug")
  else()
    set(BUILD_FOLDER "release")
  endif()
  set(TFA_OUT_DIR ${PROJECT_BINARY_DIR}/../tfa/${TFA_PLAT}/${BUILD_FOLDER})
  board_runner_args(quartus "--bl2=${TFA_OUT_DIR}/bl2.bin")

  # BL2 loads the FIP from the same QSPI flash as the FPGA configuration,
  # at PLAT_QSPI_DATA_BASE in TF-A
  if(CONFIG_AGILEX5_SOCDK_TFA_BOOT_SOURCE_QSPI)
    board_runner_args(quartus "--fip=${TFA_OUT_DIR}/fip.bin" "--fip-offset=0x3C00000")
  endif()
endif()

# QSPI configuration flash on the development kit
board_runner_args(quartus "--flash-device=MT25QU02G")

include(${ZEPHYR_BASE}/boards/common/quartus.board.cmake)

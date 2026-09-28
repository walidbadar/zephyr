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
endif()

# Copyright (c) 2026, Altera Corporation.
# SPDX-License-Identifier: Apache-2.0

config FPGA_MGMT_BACKEND_FS
	bool "File system backend"
	default y
	depends on FILE_SYSTEM
	help
	  Enable FPGA_MGMT_BACKEND_FS, which loads a bitstream file from a
	  mounted file system, for example on an SD card or eMMC. The file
	  holds the bitstream as is; its size is the bitstream size.

if FPGA_MGMT_BACKEND_FS

config FPGA_MGMT_BACKEND_FS_BOOT_FILE
	string "Bitstream file loaded at boot"
	default "/SD:/bitstream.bin"
	depends on FPGA_MGMT_LOAD_ON_BOOT_FS
	help
	  Absolute path of the bitstream file that FPGA_MGMT_LOAD_ON_BOOT
	  loads. Its file system must be mounted before
	  FPGA_MGMT_LOAD_ON_BOOT_PRIORITY, for example as an automounted
	  zephyr,fstab entry.

endif # FPGA_MGMT_BACKEND_FS

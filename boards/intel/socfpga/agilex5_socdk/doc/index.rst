.. zephyr:board:: intel_socfpga_agilex5_socdk

Intel® Agilex™ 5 SoC FPGA Development Kit
#########################################

Overview
********

The Intel® Agilex™ 5 SoC FPGA Development Kit offers a complete design
environment that includes both hardware and software for developing
Intel® Agilex™ 5 E-Series based FPGA designs. This kit is recommended for
developing custom ARM* processor-based SoC designs and ideal for intelligent
applications at the edge, embedded and more.

Hardware
********

The Intel® Agilex™ 5 Development Kit supports the following physical features:

- Intel® Agilex™ 5 E-Series FPGA, 50K-656K LEs integrated with
  multi-core ARM processors of Dual-core A55 and Dual-core A76
- On-board 8 GB DDR5 memory
- On-board JTAG Intel FPGA Download Cable II
- QSPI flash daughtercard

Supported Features
==================

.. zephyr:board-supported-hw::

Programming and Debugging
*************************

Zephyr Boot Flow
****************
Zephyr image will need to be loaded by Intel Arm Trusted Firmware (ATF).
ATF BL2 is the First Stage Boot Loader (FSBL) and ATF BL31 is the Run time resident firmware which
provides services like SMC (Secure monitor calls) and PSCI (Power state coordination interface).

Boot flow:
        ATF BL2 (EL3) -> ATF BL31 (EL3) -> Zephyr (EL1)

Intel Arm Trusted Firmware (ATF) can be downloaded from github:
        `altera-opensource/arm-trusted-firmware <https://github.com/altera-opensource/arm-trusted-firmware.git>`_

The build also produces ATF BL2, BL31 and a Firmware Image Package (FIP) that
contains BL31 and Zephyr as BL33, under ``build/tfa/agilex5/<release|debug>/``.
BL2 loads the FIP from the device selected with
``CONFIG_AGILEX5_SOCDK_TFA_BOOT_SOURCE_QSPI`` (default),
``CONFIG_AGILEX5_SOCDK_TFA_BOOT_SOURCE_SDMMC`` or
``CONFIG_AGILEX5_SOCDK_TFA_BOOT_SOURCE_NAND``. Disable ``CONFIG_BUILD_WITH_TFA``
to build Zephyr only, for use with a separately built ATF.

Flashing
========
Zephyr image can be loaded in DDR memory at address 0x80100000 from
SD Card or QSPI Flash or NAND in ATF BL2.

BL2 is loaded by the Secure Device Manager (SDM) from the FPGA configuration
image in QSPI flash. The :ref:`quartus <runner_quartus>` runner combines the
ATF ``bl2.bin`` from the build with the FPGA design ``.sof`` into a ``.jic``
file and programs it over JTAG. The runner uses the MT25QU02G QSPI flash of the
development kit; pass the design and the FPGA part number of your board:

.. code-block:: console

   west flash --sof design.sof --flash-loader <FPGA part>

With ``CONFIG_AGILEX5_SOCDK_TFA_BOOT_SOURCE_QSPI``, the runner also adds
``fip.bin`` to the ``.jic`` at offset ``0x3C00000`` of the QSPI flash, where
BL2 loads it from. For SD/MMC and NAND boot, write ``fip.bin`` to the boot
device yourself; on an SD card it goes in the partition of type ``a2``.

Debugging
=========
The Intel® Agilex™ 5 SoC Development Kit includes one JTAG connector on
board, connect it to Intel USB blaster download cables for debugging.

Zephyr applications running on the Cortex-A55/A76 core can be tested by
observing UART console output.

References
==========
`Intel® Agilex™ 5 FPGA and SoC FPGA <https://www.intel.in/content/www/in/en/products/details/fpga/agilex/5.html>`_

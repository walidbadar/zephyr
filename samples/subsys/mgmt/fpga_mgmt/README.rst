.. zephyr:code-sample:: fpga-mgmt
   :name: FPGA bitstream management
   :relevant-api: fpga_mgmt fpga_interface

   Load an FPGA bitstream from a flash partition at boot.

Overview
********

This sample enables the FPGA bitstream management subsystem with its flash partition backend and
:kconfig:option:`CONFIG_FPGA_MGMT_LOAD_ON_BOOT`. During system initialization the subsystem reads
the MCUboot image in the partition of the ``zephyr,fpga-bitstream-partition`` chosen node, checks
its header and SHA-256 hash, and passes the bitstream to the device of the ``zephyr,fpga`` chosen
node with :c:func:`fpga_load`, in chunks of :kconfig:option:`CONFIG_FPGA_LOAD_CHUNK_SIZE` bytes.
The application itself does nothing; ``main()`` is empty, and it starts once the load has finished
or failed.

The flash partition is the default backend for the load at boot, so the sample does not select it.
With the file system backend also enabled, :kconfig:option:`CONFIG_FPGA_MGMT_LOAD_ON_BOOT_FS` loads
the file :kconfig:option:`CONFIG_FPGA_MGMT_BACKEND_FS_BOOT_FILE` instead.

If the partition holds no valid image, an error is logged and the system keeps running.

Requirements
************

The sample supports the Agilex 5 SoC development kit. Its overlay in the ``boards`` directory
points ``zephyr,fpga`` at ``fpga0`` and ``zephyr,fpga-bitstream-partition`` at
``bitstream_partition``, which is at offset ``0x04400000`` of the QSPI flash and 8 MiB long.

The bitstream image has to be in the QSPI flash at that offset. The Quartus Programming File
Generator can place it there as a raw file of the ``.jic``.

On another board, add an overlay that points the two chosen nodes at the FPGA device and at the
bitstream partition. The FPGA driver must accept a bitstream in chunks
(:kconfig:option:`CONFIG_FPGA_LOAD_CHUNKED`).

Building and Running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/mgmt/fpga_mgmt
   :board: intel_socfpga_agilex5_socdk
   :goals: build

Wrap the bitstream in an MCUboot image with ``imgtool`` from the MCUboot repository, with the size
of ``bitstream_partition`` as the slot size:

.. code-block:: console

   imgtool sign --header-size 32 --pad-header --align 4 --version 1.0.0 \
           --slot-size 0x800000 design.core.rbf bitstream.bin

``imgtool`` puts a 32-byte header in front of the unchanged bitstream and a TLV area with the image
hash behind it.

Sample Output
=============

.. code-block:: console

   <inf> fpga_mgmt: fpga: loading <size> bytes from flash area 1
   <inf> fpga_mgmt: fpga configured at boot

With no image in the partition:

.. code-block:: console

   <err> fpga_mgmt: Bad magic or sizes
   <err> fpga_mgmt: Flash area 1: invalid FPGA image (-8)
   <err> fpga_mgmt: fpga: no bitstream loaded at boot (-8)

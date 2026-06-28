.. zephyr:board:: wr840n_v2

Overview
********

`TL-WR840N-v2`_ is a 300 Mbps wireless N router by TP-Link. It is based on the Qualcomm Atheros
QCA9531 SoC, a single-core MIPS 24Kc processor running at up to 650 MHz, with an integrated 2.4 GHz
802.11n radio and 5-port 10/100 Ethernet switch.

Hardware
********

.. list-table::
   :header-rows: 1

   * - Feature
     - Value
   * - SoC
     - Qualcomm Atheros QCA9531
   * - CPU
     - MIPS 24Kc, up to 650 MHz
   * - RAM
     - 32 MiB DDR2 (Winbond W9425G6JH-5)
   * - Flash
     - 4 MiB SPI NOR (Winbond W25Q32CSIG)
   * - WiFi
     - 802.11b/g/n 2x2:2 (integrated)
   * - Ethernet
     - 4x LAN + 1x WAN, 10/100 Mbps (integrated switch)

Supported Features
==================

.. zephyr:board-supported-hw::

Programming and Debugging
*************************

.. zephyr:board-supported-runners::

The Qualcomm Atheros QCA9531 SoC boots from SPI NOR flash via the on-board U-Boot 1.1.4 bootloader.
The Zephyr image is wrapped in a TP-Link compatible firmware header and written permanently to
SPI NOR via the bootloader's built-in TFTP recovery mode.

Serial Console
==============

Connect a 3.3V TTL UART adapter to the 4-pin header on the PCB:

.. list-table::
   :header-rows: 1

   * - Pin
     - Signal
   * - 1
     - VCC 3.3V (do not connect)
   * - 2
     - GND
   * - 3
     - Rx
   * - 4
     - Tx

Settings: **115200 8N1**, no hardware flow control.

Building
========

.. zephyr-app-commands::
   :zephyr-app: samples/synchronization
   :host-os: unix
   :board: wr840n_v2
   :goals: build

Flashing
========

**Step 1: Build mktplinkfw**

Clone and build the OpenWrt firmware utilities:

.. code-block:: console

   git clone https://github.com/openwrt/firmware-utils.git
   cd firmware-utils
   mkdir build && cd build
   cmake ..
   make mktplinkfw
   sudo cp mktplinkfw /usr/bin

The ``mktplinkfw`` binary will be at ``firmware-utils/build/mktplinkfw``.

**Step 2: Prepare the firmware image**

Wrap the Zephyr binary in a TP-Link compatible firmware header:

.. code-block:: console

   dd if=/dev/zero bs=1 count=4 of=build/zephyr/rootfs_dummy.bin
   mktplinkfw \
    -H 0x08400002 \
    -W 0x00000001 \
    -F 4Mlzma \
    -k build/zephyr/zephyr.bin \
    -r build/zephyr/rootfs_dummy.bin \
    -L 0x80001000 \
    -E 0x80001000 \
    -o build/zephyr/wr840nv2_en_tp_recovery.bin

Verify the image before flashing:

.. code-block:: console

   mktplinkfw -i build/zephyr/wr840nv2_en_tp_recovery.bin

Confirm ``Hardware ID: 0x08400002``, ``Kernel load address: 0x80001000``,
and ``Header MD5Sum1: (ok)``.

**Step 3: Set up TFTP server**

.. code-block:: console

   sudo apt install tftpd-hpa
   sudo cp build/zephyr/wr840nv2_en_tp_recovery.bin /var/lib/tftpboot/
   sudo systemctl start tftpd-hpa

Set your host IP to ``192.168.0.66``. The router will use ``192.168.0.86``.

**Step 4: Flash**

1. Connect an Ethernet cable between the router's WAN port and your PC.
2. Power off the router.
3. Hold the reset button, then power on while continuing to hold reset.
4. Release reset when the lock LED lights up.
5. Wait for the TFTP transfer to complete and the router to reboot.

A successful transfer looks like:

.. code-block:: text

   Connection received from 192.168.0.86 on port 4028
   Read request for file <wr840nv2_en_tp_recovery.bin>
   <wr840nv2_en_tp_recovery.bin>: sent 7938 blks, 4063744 bytes in 8 s.

Expected Output
===============

After booting, connect to the serial console at **115200 8N1**. You should see:

.. code-block:: text

   *** Booting Zephyr OS build v4.4.0-2727-g19e6d070523d ***
   thread_a: Hello World from cpu 0 on wr840n_v2!
   thread_b: Hello World from cpu 0 on wr840n_v2!

.. _TL-WR840N-v2:
   https://www.tp-link.com/en/home-networking/wifi-router/tl-wr840n/

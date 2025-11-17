.. zephyr:board:: qcc730mi

Overview
********
The QCC730MI evaluation kit is designed for evaluating and prototyping with the
Qualcomm QCC730 System-on-Chip (SoC). QCC730 is a micropower Cortex®-M4-based
single package Wi-Fi transceiver that supports dual-band 1 x 1 IEEE 802.11a/b/g/n WLAN standards.

Hardware
********

- QCC730 ARM Cortex-M4 processor at 60 MHz
- 38.5 MHz crystal oscillator for Qtimer
- 1.5 MB RRAM (600 KB for Applications) and 640 KB of SRAM (260 KB for Applications)
- XiP over QSPI Flash
- Dual band 1x1 802.11 a/b/g/n HT20 and up to MCS3 Wi-Fi function
- SPI (slave), QSPI (master), I2C (master), and UART (2-wire)
- 15x GPIO (mux’ed)
- Advanced power management scheme


Supported Features
==================

.. zephyr:board-supported-hw::

Connections and IOs
===================

External Connectors
-------------------

+-------+--------------+-------------------------+
| PIN # | Signal Name  | QCC730 Functions        |
+=======+==============+=========================+
| 1     | GND          | N/A                     |
+-------+--------------+-------------------------+
| 2     | GPIO12       | N/A                     |
+-------+--------------+-------------------------+
| 3     | GPIO11       | N/A                     |
+-------+--------------+-------------------------+
| 4     | GPIO6        | N/A                     |
+-------+--------------+-------------------------+
| 5     | GPIO5        | N/A                     |
+-------+--------------+-------------------------+
| 6     | GPIO7        | N/A                     |
+-------+--------------+-------------------------+
| 7     | GPIO4        | N/A                     |
+-------+--------------+-------------------------+
| 8     | EXT_WAKEUP   | N/A                     |
+-------+--------------+-------------------------+
| 9     | CHIP_ON      | N/A                     |
+-------+--------------+-------------------------+
| 10    | GND          | N/A                     |
+-------+--------------+-------------------------+
| 11    | VDD_1V8      | N/A                     |
+-------+--------------+-------------------------+
| 12    | GND          | N/A                     |
+-------+--------------+-------------------------+
| 13    | VDD_3V3      | N/A                     |
+-------+--------------+-------------------------+
| 14    | GND          | N/A                     |
+-------+--------------+-------------------------+
| 15    | VDD_3V3_TP   | N/A                     |
+-------+--------------+-------------------------+
| 16    | VDD_1V8_TP   | N/A                     |
+-------+--------------+-------------------------+
| 17    | VDD_5V0      | N/A                     |
+-------+--------------+-------------------------+
| 18    | GND          | N/A                     |
+-------+--------------+-------------------------+
| 19    | GPIO8        | N/A                     |
+-------+--------------+-------------------------+
| 20    | GPIO13       | UART RX                 |
+-------+--------------+-------------------------+
| 21    | GPIO14       | UART TX                 |
+-------+--------------+-------------------------+
| 22    | GPIO1        | N/A                     |
+-------+--------------+-------------------------+
| 23    | GPIO2        | N/A                     |
+-------+--------------+-------------------------+
| 24    | GPIO3        | N/A                     |
+-------+--------------+-------------------------+
| 25    | GPIO0        | N/A                     |
+-------+--------------+-------------------------+
| 26    | GPIO9        | N/A                     |
+-------+--------------+-------------------------+
| 27    | GPIO10       | N/A                     |
+-------+--------------+-------------------------+
| 28    | GND          | N/A                     |
+-------+--------------+-------------------------+

Programming and Debugging
*************************

.. zephyr:board-supported-runners::

The qccsdk runner requires the following environment variables to be set:
- ZEPHYR_SDK_INSTALL_DIR: Path to the zephyr SDK installation directory
- OPENOCD_PATH: Path to the installation directory with OpenOCD binary that supports CH347, download from https://github.com/WCHSoftGroup/ch347/releases/tag/CH347_OpenOCD_Release
- (Alteratively) JLINK_PATH: Path to the J-Link GDB server installation directory, download from https://www.segger.com/downloads/jlink/
- Download python 3.10.9 windows embeddable package from https://www.python.org/downloads/windows/?Windows%20embeddable%20package, add it to Path

Flashing
========

#. Build the ``hello_world`` sample application:

   .. zephyr-app-commands::
      :zephyr-app: samples/hello_world
      :board: qcc730mi
      :goals: build
      :compact:

#. Connect the QCC730MI board to your computer via the debug USB port.

#. Open the serial terminal of your choice to listen for output.
   Connection should use the following parameters:

   - Speed: 115200
   - Data: 8 bits
   - Parity: None
   - Stop bits: 1

#. Flash an image:

   .. zephyr-app-commands::
      :zephyr-app: samples/hello_world
      :board: qcc730mi
      :goals: flash
      :flash-args: -a
      :compact:

To flash the RRAM with Zephyr application, use:

.. code-block:: console

   west flash

To flash the Zephyr application and SBL, use:

.. code-block:: console

   west flash -- -a


If you want to write the Zephyr application to flash, call:

.. code-block:: console

   west flash -- -m flash


Debugging
=========
To debug using the on-board JTAG chip, call:

.. code-block:: console

   west debug

For debugging with J-Link, you need to specify the JTAG adapter by adding
the ``-j jlink`` option:

.. code-block:: console

   west debug -- -j jlink

References
**********
.. target-notes::

.. _Qualcomm Technologies, Inc.: http://www.qualcomm.com/products/technology/wi-fi/qcc730

.. _QCC730 Datasheet: https://docs.qualcomm.com/bundle/publicresource/80-WL730-1_REV_AF_QCC730_Dual_Band_Ultra_Low_Power_802_11A_B_G_N_Data_Sheet.pdf

.. _QCC730 Evaluation Kit Quck Start Guide: https://docs.qualcomm.com/bundle/publicresource/topics/80-Y8730-1/qcc730_dev_kit_introduction.html

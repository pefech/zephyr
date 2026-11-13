.. zephyr:code-sample:: ipc-rproc-rpmsg
   :name: IPC service: remoteproc RPMsg backend
   :relevant-api: ipc

   Talk to Linux over RPMsg from a core that Linux remoteproc manages.

Overview
********

This sample runs on a core that the Linux remoteproc framework manages, for example the
Cortex-M core of an i.MX SoC next to Cortex-A cores running Linux. It answers the standard
Linux RPMsg drivers:

* ``rpmsg-client-sample``: echoes the 100 messages of the
  `Linux rpmsg client sample <https://elixir.bootlin.com/linux/latest/source/samples/rpmsg/rpmsg_client_sample.c>`_
  and removes the endpoint afterwards.
* ``rpmsg-tty``: echoes every line written to the
  `Linux rpmsg tty driver <https://elixir.bootlin.com/linux/latest/source/drivers/tty/rpmsg_tty.c>`_
  device, prefixed with ``TTY:``.
* The Zephyr shell, on its own ``rpmsg-tty`` channel. The ``rpmsg-shell`` property of the
  ``zephyr,ipc-rproc-rpmsg`` node starts it together with
  :kconfig:option:`CONFIG_SHELL_BACKEND_RPMSG`, no application code is involved.

Nothing in the application deals with OpenAMP itself. The devicetree describes the shared
memory carveouts, the vrings, their mailbox channels and the RPMsg VirtIO device. From that
the build generates the resource table that Linux reads, and the
``openamp,rpmsg-virtio-device`` driver brings the RPMsg device up once Linux has set it up.
The application uses the :ref:`IPC service <ipc_service>` API on a ``zephyr,ipc-rproc-rpmsg``
instance: it opens the instance and registers its endpoints. Each endpoint is announced to
Linux through the RPMsg name service, and Linux creates the matching channel.

Zephyr doesn't wait for Linux during boot. The endpoints are announced as soon as Linux has set
the RPMsg device up, which can be seconds after ``main()`` when a bootloader starts the core.
The ``bound`` callback of an endpoint reports the first message from Linux.

Resource table location
***********************

Linux finds the resource table in one of two ways, and the build must match it:

* Linux loads the ELF image and starts the core: Linux reads and updates the table in the
  ``.resource_table`` section of the image. The Linux devicetree has no ``rsc-table`` region.
  Build the sample as is.
* A bootloader starts the core and Linux attaches to it: Linux uses the table in its
  ``rsc-table`` reserved memory region. Point the ``memory-region`` property of the
  ``openamp,resource-table`` node to the same memory. The driver copies the table there at
  startup.

The carveout names in the devicetree must match the names of the Linux reserved memory nodes,
Linux pairs them by name.

Linux loads the core
====================

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/ipc/ipc_service/rproc_rpmsg
   :board: phyboard_nash/mimx9352/m33
   :goals: build

Boot Linux with a devicetree overlay that does **not** have the ``rsc_table`` region defined,
copy :file:`zephyr.elf` to :file:`/lib/firmware` and start the core:

.. code-block:: console

   echo zephyr.elf > /sys/class/remoteproc/remoteproc0/firmware
   echo start > /sys/class/remoteproc/remoteproc0/state

U-Boot starts the core
======================

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/ipc/ipc_service/rproc_rpmsg
   :board: phyboard_nash/mimx9352/m33
   :gen-args: -DEXTRA_DTC_OVERLAY_FILE=boards/phyboard_nash_mimx9352_m33_rsc_table.overlay
   :goals: build

Copy :file:`zephyr.bin` to the boot partition, start the core from U-Boot and boot Linux with
a devicetree overlay that has the ``rsc_table`` region defined. Linux attaches to the running core:

.. code-block:: console

   load mmc 1:1 0x80000000 zephyr.bin; cp.b 0x80000000 0x201e0000 0x30000; bootaux 0x1ffe0000 0
   boot

Zephyr log in Linux
===================

With :file:`ram_console.conf`, the console and the log go to a RAM buffer that the resource
table publishes as a trace entry. Linux shows it in debugfs:

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/ipc/ipc_service/rproc_rpmsg
   :board: phyboard_nash/mimx9352/m33
   :gen-args: -DEXTRA_CONF_FILE=ram_console.conf
   :goals: build

.. code-block:: console

   $ cat /sys/kernel/debug/remoteproc/remoteproc0/trace0
   *** Booting Zephyr OS build v4.5.0 ***
   I: Linux responder ready
   I: Linux sample client responder started
   I: [Linux sample client] incoming msg 1: hello world!
   ...
   I: [Linux sample client] incoming msg 100: hello world!
   I: Linux sample client responder ended

Sample output
=============

Linux creates three channels and runs the client sample:

.. code-block:: console

   virtio_rpmsg_bus virtio0: creating channel rpmsg-tty addr 0x400
   virtio_rpmsg_bus virtio0: creating channel rpmsg-client-sample addr 0x401
   virtio_rpmsg_bus virtio0: creating channel rpmsg-tty addr 0x402
   rpmsg_client_sample virtio0.rpmsg-client-sample.-1.1025: incoming msg 1 (src: 0x401)
   ...
   rpmsg_client_sample virtio0.rpmsg-client-sample.-1.1025: incoming msg 100 (src: 0x401)
   rpmsg_client_sample virtio0.rpmsg-client-sample.-1.1025: goodbye!
   virtio_rpmsg_bus virtio0: destroying channel rpmsg-client-sample addr 0x401

The shell is announced first, so it is the ``rpmsg-tty`` channel with the lower address. The
address is part of the device name:

.. code-block:: console

   $ ls -l /sys/class/tty/ttyRPMSG*/device
   ... /sys/class/tty/ttyRPMSG0/device -> ../../../virtio0.rpmsg-tty.-1.1024
   ... /sys/class/tty/ttyRPMSG1/device -> ../../../virtio0.rpmsg-tty.-1.1026

   $ stty -F /dev/ttyRPMSG1 raw -echo
   $ cat /dev/ttyRPMSG1 &
   $ echo hello > /dev/ttyRPMSG1
   TTY: hello

Open the shell channel with a terminal program, for example ``microcom /dev/ttyRPMSG0``, and
press enter to get the ``ipc:~$`` prompt.

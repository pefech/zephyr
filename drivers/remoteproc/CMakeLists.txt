# SPDX-License-Identifier: Apache-2.0

if(CONFIG_OPENAMP_RPMSG_VDEV OR CONFIG_OPENAMP_RESOURCE_TABLE)
  zephyr_library()
endif()

if(CONFIG_OPENAMP_RPMSG_VDEV)
  zephyr_library_sources(rpmsg_virtio_device.c)
endif()

if(CONFIG_OPENAMP_RESOURCE_TABLE)
  zephyr_library_sources(resource_table.c)
  # The host finds the table by section name, so give the section a home of
  # its own instead of leaving it to the linker's orphan placement.
  zephyr_linker_sources(SECTIONS rsc_table.ld)
endif()

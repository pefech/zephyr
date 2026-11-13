/*
 * Copyright (c) 2026 Simon Maurer <mail@maurer.systems>
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_REMOTEPROC_H_
#define ZEPHYR_INCLUDE_DRIVERS_REMOTEPROC_H_

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/slist.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations for OpenAMP resource structures */
struct fw_rsc_carveout;
struct fw_rsc_vdev;

#ifdef CONFIG_OPENAMP_RPMSG_VDEV
struct rpmsg_device;
#endif

/**
 * @brief Get pointer to the OpenAMP resource table
 *
 * Returns a pointer to the resource table included in the firmware image.
 * The resource table describes shared memory regions, virtio devices,
 * and other resources used for inter-processor communication.
 *
 * @param size Pointer to a variable that will be filled with the size
 *             of the resource table in bytes.
 *
 * @return Pointer to the resource table, or NULL if not available.
 */
void *z_rproc_rsc_table(size_t *size);

/**
 * @brief Get carveout resource by name
 *
 * Searches the resource table for a carveout entry with the given name.
 *
 * @param name Null-terminated string identifying the carveout.
 *
 * @return Pointer to the carveout resource, or NULL if not found.
 */
struct fw_rsc_carveout *z_rproc_get_carveout_by_name(const char *name);

/**
 * @brief Get carveout resource by index
 *
 * Retrieves a carveout entry from the resource table by index.
 *
 * @param idx Index of the carveout entry.
 *
 * @return Pointer to the carveout resource, or NULL if the index
 *         is out of range.
 */
struct fw_rsc_carveout *z_rproc_get_carveout_by_index(unsigned int idx);

/**
 * @brief Get VirtIO device resource
 *
 * Retrieves a VirtIO device entry from the resource table.
 *
 * @param idx Index of the VirtIO device entry.
 *
 * @return Pointer to the vdev resource, or NULL if the index
 *         is out of range.
 */
struct fw_rsc_vdev *z_rproc_get_vdev(unsigned int idx);

#ifdef CONFIG_OPENAMP_RPMSG_VDEV

/**
 * @brief Get RPMsg device associated with a RemoteProc instance
 *
 * Returns the RPMsg device corresponding to the given Zephyr device.
 *
 * @param dev Pointer to the Zephyr device instance.
 *
 * @return Pointer to the RPMsg device, or NULL if the host has not set the
 *         device up yet.
 */
struct rpmsg_device *z_rproc_get_rpmsg_device(const struct device *dev);

/**
 * @brief Callback for the RPMsg device becoming usable
 *
 * The host sets the virtio device up at any time after Zephyr has started,
 * e.g. only when Linux attaches to a core started by a bootloader.
 */
struct z_rproc_rpmsg_ready_cb {
	/** @cond INTERNAL_HIDDEN */
	sys_snode_t node;
	/** @endcond */
	/** Called once with the RPMsg device; may create endpoints. */
	void (*handler)(const struct device *dev, struct rpmsg_device *rdev, void *user_data);
	/** Passed to @a handler. */
	void *user_data;
};

/**
 * @brief Register a callback for the RPMsg device becoming usable
 *
 * The handler runs once: from the driver thread when the host has set the
 * device up, or from the caller when that has already happened.
 *
 * @param dev Pointer to the Zephyr device instance.
 * @param cb Callback, must stay valid until it has run.
 *
 * @retval 0 Success.
 * @retval -EINVAL @a cb or its handler is NULL.
 */
int z_rproc_rpmsg_add_ready_cb(const struct device *dev, struct z_rproc_rpmsg_ready_cb *cb);

#endif /* CONFIG_OPENAMP_RPMSG_VDEV */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_REMOTEPROC_H_ */

/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * IPC service backend over the RPMsg device of an OpenAMP remoteproc remote.
 *
 * The remoteproc driver (drivers/remoteproc) owns the virtio device, its
 * vrings and the resource table the host set them up from; this backend only
 * maps ipc_service endpoints onto RPMsg endpoints of that device. It is the
 * backend for a Zephyr core whose host - typically Linux, through remoteproc
 * and virtio_rpmsg_bus - loads it and publishes the vdev, as opposed to the
 * static-vrings backend, where both sides agree on a layout in devicetree.
 *
 * Endpoints are announced through the RPMsg name service, which Linux answers
 * by creating a channel (and, for the name "rpmsg-raw", a /dev/rpmsgN node).
 * The name service carries no acknowledgment, so an endpoint cannot address
 * the host until the host has sent it something: that first message is what
 * binds it, and bound() fires just before it is delivered.
 */

#define DT_DRV_COMPAT zephyr_ipc_rproc_rpmsg

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/remoteproc.h>
#include <zephyr/ipc/ipc_service_backend.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <openamp/rpmsg.h>
#include <openamp/rpmsg_virtio.h>

LOG_MODULE_REGISTER(ipc_rproc_rpmsg, CONFIG_IPC_SERVICE_LOG_LEVEL);

#define NUM_ENDPOINTS CONFIG_IPC_SERVICE_BACKEND_RPROC_RPMSG_NUM_ENDPOINTS

#define STATE_READY  0
#define STATE_BUSY   1
#define STATE_INITED 2

struct rproc_rpmsg_ept {
	struct rpmsg_endpoint ep;
	/* Copied, so the caller's ipc_ept_cfg need not outlive registration. */
	struct ipc_service_cb cb;
	void *priv;
	atomic_t bound;
	bool used;
};

struct backend_config {
	const struct device *rvdev;
};

struct backend_data {
	struct rproc_rpmsg_ept ept[NUM_ENDPOINTS];
	struct rpmsg_device *rdev;
	struct k_mutex mtx;
	atomic_t state;
	int tx_buffer_size;
};

static int ept_cb(struct rpmsg_endpoint *ep, void *data, size_t len, uint32_t src, void *priv)
{
	struct rproc_rpmsg_ept *ept = priv;

	ARG_UNUSED(ep);
	ARG_UNUSED(src);

	/* The first message is the first moment the host's address is known. */
	if (atomic_cas(&ept->bound, 0, 1) && ept->cb.bound) {
		ept->cb.bound(ept->priv);
	}

	if (len > 0 && ept->cb.received) {
		ept->cb.received(data, len, ept->priv);
	}

	return RPMSG_SUCCESS;
}

static void ept_unbind_cb(struct rpmsg_endpoint *ep)
{
	struct rproc_rpmsg_ept *ept = ep->priv;

	/* rpmsg_destroy_ept() leaves ep->priv in place; the slot may be gone. */
	if (ept && atomic_cas(&ept->bound, 1, 0) && ept->cb.unbound) {
		ept->cb.unbound(ept->priv);
	}
}

static int open(const struct device *instance)
{
	const struct backend_config *conf = instance->config;
	struct backend_data *data = instance->data;

	if (!atomic_cas(&data->state, STATE_READY, STATE_BUSY)) {
		return -EALREADY;
	}

	/*
	 * The remoteproc driver finishes initializing only once the host has
	 * set the virtio device up, so a ready device is a live link.
	 */
	if (!device_is_ready(conf->rvdev)) {
		atomic_set(&data->state, STATE_READY);
		return -ENODEV;
	}

	data->rdev = z_rproc_get_rpmsg_device(conf->rvdev);
	data->tx_buffer_size = rpmsg_virtio_get_tx_buffer_size(data->rdev);
	if (data->tx_buffer_size <= 0) {
		atomic_set(&data->state, STATE_READY);
		return -EINVAL;
	}

	atomic_set(&data->state, STATE_INITED);
	return 0;
}

static int close(const struct device *instance)
{
	struct backend_data *data = instance->data;

	if (!atomic_cas(&data->state, STATE_INITED, STATE_BUSY)) {
		return -EALREADY;
	}

	for (int i = 0; i < NUM_ENDPOINTS; i++) {
		if (data->ept[i].used) {
			atomic_set(&data->state, STATE_INITED);
			return -EBUSY;
		}
	}

	/* The remoteproc driver owns the device; there is nothing to tear down. */
	data->rdev = NULL;
	atomic_set(&data->state, STATE_READY);
	return 0;
}

static int register_ept(const struct device *instance, void **token,
			const struct ipc_ept_cfg *cfg)
{
	struct backend_data *data = instance->data;
	struct rproc_rpmsg_ept *ept = NULL;
	int ret;

	if (atomic_get(&data->state) != STATE_INITED) {
		return -EBUSY;
	}

	if (!cfg || !cfg->name || cfg->name[0] == '\0') {
		return -EINVAL;
	}

	k_mutex_lock(&data->mtx, K_FOREVER);

	for (int i = 0; i < NUM_ENDPOINTS; i++) {
		if (!data->ept[i].used) {
			ept = &data->ept[i];
			break;
		}
	}

	if (!ept) {
		k_mutex_unlock(&data->mtx);
		return -ENOMEM;
	}

	ept->used = true;
	ept->cb = cfg->cb;
	ept->priv = cfg->priv;
	atomic_set(&ept->bound, 0);
	ept->ep.priv = ept;

	/* A destination of RPMSG_ADDR_ANY is what makes OpenAMP announce it. */
	ret = rpmsg_create_ept(&ept->ep, data->rdev, cfg->name, RPMSG_ADDR_ANY, RPMSG_ADDR_ANY,
			       ept_cb, ept_unbind_cb);
	if (ret) {
		ept->used = false;
		k_mutex_unlock(&data->mtx);
		LOG_ERR("rpmsg_create_ept(%s) failed: %d", cfg->name, ret);
		return -EIO;
	}

	k_mutex_unlock(&data->mtx);

	LOG_DBG("endpoint %s at 0x%x announced", cfg->name, ept->ep.addr);
	*token = ept;
	return 0;
}

static int deregister_ept(const struct device *instance, void *token)
{
	struct backend_data *data = instance->data;
	struct rproc_rpmsg_ept *ept = token;

	if (atomic_get(&data->state) != STATE_INITED) {
		return -EBUSY;
	}

	if (!ept || !ept->used) {
		return -ENOENT;
	}

	k_mutex_lock(&data->mtx, K_FOREVER);
	/* Tells the host, which removes the channel and its device node. */
	ept->ep.priv = NULL;
	rpmsg_destroy_ept(&ept->ep);
	atomic_set(&ept->bound, 0);
	ept->used = false;
	k_mutex_unlock(&data->mtx);

	return 0;
}

static int send(const struct device *instance, void *token, const void *msg, size_t len)
{
	struct backend_data *data = instance->data;
	struct rproc_rpmsg_ept *ept = token;
	int ret;

	if (atomic_get(&data->state) != STATE_INITED) {
		return -EBUSY;
	}

	if (!ept || !ept->used) {
		return -ENOENT;
	}

	if (len == 0) {
		return -EBADMSG;
	}

	if (!atomic_get(&ept->bound)) {
		/* The host has not addressed this endpoint yet. */
		return -EBUSY;
	}

	/*
	 * Never rpmsg_send(): it waits up to 15 s for a buffer. Transmit buffers
	 * come back through the used ring, which rpmsg_trysend() itself reaps,
	 * so a short bounded wait here needs nothing else to run - it is safe
	 * even from a received() callback.
	 */
	for (int waited = 0;; waited++) {
		ret = rpmsg_trysend(&ept->ep, msg, len);
		if (ret != RPMSG_ERR_NO_BUFF ||
		    waited >= CONFIG_IPC_SERVICE_BACKEND_RPROC_RPMSG_TX_WAIT_MS) {
			break;
		}
		k_msleep(1);
	}

	if (ret == RPMSG_ERR_NO_BUFF) {
		return -ENOMEM;
	}

	return ret < 0 ? -EIO : ret;
}

static int send_nocopy(const struct device *instance, void *token, const void *msg, size_t len)
{
	struct backend_data *data = instance->data;
	struct rproc_rpmsg_ept *ept = token;

	if (atomic_get(&data->state) != STATE_INITED) {
		return -EBUSY;
	}

	if (!ept || !ept->used) {
		return -ENOENT;
	}

	if (len == 0) {
		return -EBADMSG;
	}

	if (!atomic_get(&ept->bound)) {
		return -EBUSY;
	}

	return rpmsg_send_nocopy(&ept->ep, msg, len);
}

static int get_tx_buffer_size(const struct device *instance, void *token)
{
	struct backend_data *data = instance->data;

	ARG_UNUSED(token);

	return data->tx_buffer_size;
}

static int get_tx_buffer(const struct device *instance, void *token, void **r_data,
			 uint32_t *size, k_timeout_t wait)
{
	struct backend_data *data = instance->data;
	struct rproc_rpmsg_ept *ept = token;
	void *payload;

	if (!ept || !ept->used) {
		return -ENOENT;
	}

	if (!r_data || !size) {
		return -EINVAL;
	}

	/* OpenAMP only supports a binary wait / no-wait. */
	if (!K_TIMEOUT_EQ(wait, K_FOREVER) && !K_TIMEOUT_EQ(wait, K_NO_WAIT)) {
		return -ENOTSUP;
	}

	if (*size && *size > data->tx_buffer_size) {
		*size = data->tx_buffer_size;
		return -ENOMEM;
	}

	/* OpenAMP gives up after 15 s even when asked to wait; keep waiting. */
	do {
		payload = rpmsg_get_tx_payload_buffer(&ept->ep, size, K_TIMEOUT_EQ(wait, K_FOREVER));
	} while (!payload && K_TIMEOUT_EQ(wait, K_FOREVER));

	if (!payload) {
		return -ENOBUFS;
	}

	*r_data = payload;
	return 0;
}

static int drop_tx_buffer(const struct device *instance, void *token, const void *data)
{
	struct rproc_rpmsg_ept *ept = token;

	ARG_UNUSED(instance);

	if (!ept || !ept->used) {
		return -ENOENT;
	}

	return rpmsg_release_tx_buffer(&ept->ep, (void *)data) < 0 ? -EINVAL : 0;
}

static int hold_rx_buffer(const struct device *instance, void *token, void *data)
{
	struct rproc_rpmsg_ept *ept = token;

	ARG_UNUSED(instance);

	if (!ept || !ept->used) {
		return -ENOENT;
	}

	rpmsg_hold_rx_buffer(&ept->ep, data);
	return 0;
}

static int release_rx_buffer(const struct device *instance, void *token, void *data)
{
	struct rproc_rpmsg_ept *ept = token;

	ARG_UNUSED(instance);

	if (!ept || !ept->used) {
		return -ENOENT;
	}

	rpmsg_release_rx_buffer(&ept->ep, data);
	return 0;
}

static const struct ipc_service_backend backend_ops = {
	.open_instance = open,
	.close_instance = close,
	.register_endpoint = register_ept,
	.deregister_endpoint = deregister_ept,
	.send = send,
	.send_nocopy = send_nocopy,
	.get_tx_buffer = get_tx_buffer,
	.get_tx_buffer_size = get_tx_buffer_size,
	.drop_tx_buffer = drop_tx_buffer,
	.hold_rx_buffer = hold_rx_buffer,
	.release_rx_buffer = release_rx_buffer,
};

static int backend_init(const struct device *instance)
{
	struct backend_data *data = instance->data;

	k_mutex_init(&data->mtx);
	atomic_set(&data->state, STATE_READY);

	return 0;
}

#define DEFINE_BACKEND_DEVICE(i)                                                                   \
	static const struct backend_config backend_config_##i = {                                  \
		.rvdev = DEVICE_DT_GET(DT_INST_PHANDLE(i, rpmsg_device)),                          \
	};                                                                                         \
	static struct backend_data backend_data_##i;                                               \
	DEVICE_DT_INST_DEFINE(i, backend_init, NULL, &backend_data_##i, &backend_config_##i,       \
			      POST_KERNEL, CONFIG_IPC_SERVICE_REG_BACKEND_PRIORITY, &backend_ops);

DT_INST_FOREACH_STATUS_OKAY(DEFINE_BACKEND_DEVICE)

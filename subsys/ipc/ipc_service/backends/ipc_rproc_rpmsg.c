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

#ifdef CONFIG_SHELL_BACKEND_RPMSG
#include <zephyr/shell/shell_rpmsg.h>
#endif

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
	/* The RPMsg endpoint exists; until the host is ready it is pending. */
	bool created;
	char name[RPMSG_NAME_SIZE];
};

struct backend_config {
	const struct device *rvdev;
	bool rpmsg_shell;
};

struct backend_data {
	struct rproc_rpmsg_ept ept[NUM_ENDPOINTS];
	struct z_rproc_rpmsg_ready_cb ready_cb;
	/* Set once the host has set the RPMsg device up. */
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

/* Called with data->mtx held and data->rdev set. */
static int create_ept(struct backend_data *data, struct rproc_rpmsg_ept *ept)
{
	int ret;

	/* A destination of RPMSG_ADDR_ANY is what makes OpenAMP announce it. */
	ret = rpmsg_create_ept(&ept->ep, data->rdev, ept->name, RPMSG_ADDR_ANY, RPMSG_ADDR_ANY,
			       ept_cb, ept_unbind_cb);
	if (ret) {
		LOG_ERR("rpmsg_create_ept(%s) failed: %d", ept->name, ret);
		return -EIO;
	}

	ept->created = true;
	LOG_DBG("endpoint %s at 0x%x announced", ept->name, ept->ep.addr);

	return 0;
}

static int open(const struct device *instance)
{
	const struct backend_config *conf = instance->config;
	struct backend_data *data = instance->data;

	if (!atomic_cas(&data->state, STATE_READY, STATE_BUSY)) {
		return -EALREADY;
	}

	if (!device_is_ready(conf->rvdev)) {
		atomic_set(&data->state, STATE_READY);
		return -ENODEV;
	}

	/*
	 * The host may set the RPMsg device up only later. Endpoints registered
	 * before that are announced once it has, see rdev_ready().
	 */
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
	ept->created = false;
	ept->cb = cfg->cb;
	ept->priv = cfg->priv;
	atomic_set(&ept->bound, 0);
	ept->ep.priv = ept;
	strncpy(ept->name, cfg->name, sizeof(ept->name) - 1);
	ept->name[sizeof(ept->name) - 1] = '\0';

	if (data->rdev != NULL) {
		ret = create_ept(data, ept);
		if (ret < 0) {
			ept->used = false;
			k_mutex_unlock(&data->mtx);
			return ret;
		}
	}

	k_mutex_unlock(&data->mtx);

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
	if (ept->created) {
		/* Tells the host, which removes the channel and its device node. */
		ept->ep.priv = NULL;
		rpmsg_destroy_ept(&ept->ep);
		ept->created = false;
	}
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

	if (data->rdev == NULL) {
		return -EBUSY;
	}

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

	if (!ept->created) {
		return -EBUSY;
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

#ifdef CONFIG_SHELL_BACKEND_RPMSG
#define RPMSG_SHELL_COUNT(i) + DT_INST_PROP(i, rpmsg_shell)

BUILD_ASSERT((0 DT_INST_FOREACH_STATUS_OKAY(RPMSG_SHELL_COUNT)) <= 1,
	     "Only one instance can have the rpmsg-shell property");

static void start_rpmsg_shell(struct rpmsg_device *rdev)
{
	int ret = shell_backend_rpmsg_init_transport(rdev);

	if (ret < 0) {
		LOG_ERR("RPMsg shell init failed: %d", ret);
	}
}
#endif

static void rdev_ready(const struct device *rvdev, struct rpmsg_device *rdev, void *user_data)
{
	const struct device *instance = user_data;
	const struct backend_config *conf = instance->config;
	struct backend_data *data = instance->data;

	ARG_UNUSED(rvdev);

	k_mutex_lock(&data->mtx, K_FOREVER);

	data->tx_buffer_size = rpmsg_virtio_get_tx_buffer_size(rdev);
	data->rdev = rdev;

#ifdef CONFIG_SHELL_BACKEND_RPMSG
	/* Announced first, so the shell is the first channel of the instance. */
	if (conf->rpmsg_shell) {
		start_rpmsg_shell(rdev);
	}
#else
	ARG_UNUSED(conf);
#endif

	for (int i = 0; i < NUM_ENDPOINTS; i++) {
		struct rproc_rpmsg_ept *ept = &data->ept[i];

		if (ept->used && !ept->created && create_ept(data, ept) < 0) {
			ept->used = false;
		}
	}

	k_mutex_unlock(&data->mtx);
}

static int backend_init(const struct device *instance)
{
	const struct backend_config *conf = instance->config;
	struct backend_data *data = instance->data;

	k_mutex_init(&data->mtx);
	atomic_set(&data->state, STATE_READY);

	data->ready_cb.handler = rdev_ready;
	data->ready_cb.user_data = (void *)instance;

	return z_rproc_rpmsg_add_ready_cb(conf->rvdev, &data->ready_cb);
}

#define DEFINE_BACKEND_DEVICE(i)                                                                   \
	static const struct backend_config backend_config_##i = {                                  \
		.rvdev = DEVICE_DT_GET(DT_INST_PHANDLE(i, rpmsg_device)),                          \
		.rpmsg_shell = DT_INST_PROP(i, rpmsg_shell),                                       \
	};                                                                                         \
	static struct backend_data backend_data_##i;                                               \
	DEVICE_DT_INST_DEFINE(i, backend_init, NULL, &backend_data_##i, &backend_config_##i,       \
			      POST_KERNEL, CONFIG_IPC_SERVICE_REG_BACKEND_PRIORITY, &backend_ops);

DT_INST_FOREACH_STATUS_OKAY(DEFINE_BACKEND_DEVICE)

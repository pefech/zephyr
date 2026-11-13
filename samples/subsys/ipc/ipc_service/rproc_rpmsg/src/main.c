/*
 * Copyright (c) 2026 PHYTEC Messtechnik GmbH
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(rproc_rpmsg, LOG_LEVEL_INF);

/* The Linux rpmsg_client_sample driver sends this many messages. */
#define CLIENT_SAMPLE_MSG_COUNT 100U

#define TTY_PREFIX     "TTY: "
#define TTY_PREFIX_LEN (sizeof(TTY_PREFIX) - 1U)
/* RPMsg buffer payload size used by Linux virtio_rpmsg_bus. */
#define TTY_MSG_SIZE   496U

static const struct device *const ipc_instance = DEVICE_DT_GET(DT_NODELABEL(ipc0));

static struct ipc_ept client_ept;
static struct ipc_ept tty_ept;

static void client_done_handler(struct k_work *work)
{
	int ret;

	ARG_UNUSED(work);

	ret = ipc_service_deregister_endpoint(&client_ept);
	if (ret < 0) {
		LOG_ERR("[Linux sample client] deregister failed: %d", ret);
	}

	LOG_INF("Linux sample client responder ended");
}

static K_WORK_DEFINE(client_done_work, client_done_handler);

static void client_bound(void *priv)
{
	ARG_UNUSED(priv);

	LOG_INF("Linux sample client responder started");
}

static void client_received(const void *data, size_t len, void *priv)
{
	static uint32_t msg_cnt;
	int ret;

	ARG_UNUSED(priv);

	msg_cnt++;
	LOG_INF("[Linux sample client] incoming msg %u: %.*s", msg_cnt, (int)len,
		(const char *)data);

	ret = ipc_service_send(&client_ept, data, len);
	if (ret < 0) {
		LOG_ERR("[Linux sample client] send failed: %d", ret);
	}

	if (msg_cnt == CLIENT_SAMPLE_MSG_COUNT) {
		/* Deregistration announces the endpoint removal to Linux. */
		k_work_submit(&client_done_work);
	}
}

static void tty_bound(void *priv)
{
	ARG_UNUSED(priv);

	LOG_INF("Linux TTY responder started");
}

static void tty_received(const void *data, size_t len, void *priv)
{
	/* Callbacks of one instance run on a single thread. */
	static uint8_t tx_buf[TTY_MSG_SIZE];
	size_t payload_len = MIN(len, sizeof(tx_buf) - TTY_PREFIX_LEN);
	int ret;

	ARG_UNUSED(priv);

	LOG_INF("[Linux TTY] incoming msg: %.*s", (int)len, (const char *)data);

	memcpy(tx_buf, TTY_PREFIX, TTY_PREFIX_LEN);
	memcpy(&tx_buf[TTY_PREFIX_LEN], data, payload_len);

	ret = ipc_service_send(&tty_ept, tx_buf, TTY_PREFIX_LEN + payload_len);
	if (ret < 0) {
		LOG_ERR("[Linux TTY] send failed: %d", ret);
	}
}

static const struct ipc_ept_cfg client_ept_cfg = {
	.name = "rpmsg-client-sample",
	.cb = {
		.bound = client_bound,
		.received = client_received,
	},
};

static const struct ipc_ept_cfg tty_ept_cfg = {
	.name = "rpmsg-tty",
	.cb = {
		.bound = tty_bound,
		.received = tty_received,
	},
};

int main(void)
{
	int ret;

	ret = ipc_service_open_instance(ipc_instance);
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("ipc_service_open_instance() failed: %d", ret);
		return ret;
	}

	ret = ipc_service_register_endpoint(ipc_instance, &client_ept, &client_ept_cfg);
	if (ret < 0) {
		LOG_ERR("registering %s failed: %d", client_ept_cfg.name, ret);
		return ret;
	}

	ret = ipc_service_register_endpoint(ipc_instance, &tty_ept, &tty_ept_cfg);
	if (ret < 0) {
		LOG_ERR("registering %s failed: %d", tty_ept_cfg.name, ret);
		return ret;
	}

	LOG_INF("Linux responder ready");

	return 0;
}

/*
 * SPDX-FileCopyrightText: Copyright (c) 2026, Altera Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mailbox of the Altera SoC FPGA Secure Device Manager (SDM), reached
 * through the ARM SiP service.
 */

#define DT_DRV_COMPAT altr_socfpga_sdm

#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sip_svc/sip_svc_agilex_mailbox.h>
#include <zephyr/drivers/sip_svc/sip_svc_agilex_smc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sip_svc/sip_svc.h>
#include <zephyr/sys/util.h>

#include <altera_sdm.h>

LOG_MODULE_REGISTER(altera_sdm, CONFIG_LOG_DEFAULT_LEVEL);

BUILD_ASSERT(CONFIG_ALTERA_SDM_INIT_PRIORITY > CONFIG_ARM_SIP_SVC_SUBSYS_INIT_PRIORITY,
	     "SDM must be initialized after the SiP service");

/* Mailbox command and response header fields */
#define SDM_MB_CMD_ID_MASK     GENMASK(10, 0)
#define SDM_MB_RESP_ERROR_MASK GENMASK(10, 0)
#define SDM_MB_LENGTH_MASK     GENMASK(22, 12)

#define SDM_MB_HEADER_WORDS 1U

struct altera_sdm_config {
	const char *method;
};

struct altera_sdm_data {
	void *ctrl;
	uint32_t token;
	struct k_mutex lock;
};

/* One transaction, shared with the SiP service callback */
struct altera_sdm_xfer {
	struct k_sem done;
	uint32_t smc_status;
	struct altera_sdm_smc_res regs;
	uint32_t header;
	uint32_t *resp;
	size_t resp_cap;
	size_t resp_len;
};

static void altera_sdm_callback(uint32_t c_token, struct sip_svc_response *response)
{
	ARG_UNUSED(c_token);

	if (response == NULL) {
		return;
	}

	struct altera_sdm_xfer *xfer = response->priv_data;
	uint32_t *buf = (uint32_t *)(uintptr_t)response->resp_data_addr;
	size_t words = response->resp_data_size / sizeof(uint32_t);

	xfer->smc_status = SIP_SVC_PROTO_HEADER_GET_CODE(response->header);
	xfer->regs.a0 = response->a0;
	xfer->regs.a1 = response->a1;
	xfer->regs.a2 = response->a2;
	xfer->regs.a3 = response->a3;

	if ((xfer->smc_status == SMC_STATUS_OKAY) && (buf != NULL) &&
	    (words >= SDM_MB_HEADER_WORDS)) {
		xfer->header = buf[0];
		xfer->resp_len = MIN(words - SDM_MB_HEADER_WORDS, xfer->resp_cap);
		if (xfer->resp_len > 0U) {
			memcpy(xfer->resp, &buf[SDM_MB_HEADER_WORDS],
			       xfer->resp_len * sizeof(uint32_t));
		}
	}

	/* The client owns the response buffer, the SiP service frees the command buffer */
	k_free(buf);

	k_sem_give(&xfer->done);
}

/*
 * Run one request on the SiP service client and wait for its callback. For an
 * asynchronous request, the command and response buffers are freed here when
 * the request cannot be queued; otherwise their ownership has passed on.
 */
static int altera_sdm_submit(const struct device *dev, struct sip_svc_request *request,
			     struct altera_sdm_xfer *xfer)
{
	struct altera_sdm_data *data = dev->data;
	bool own_bufs = SIP_SVC_PROTO_HEADER_GET_CODE(request->header) == SIP_SVC_PROTO_CMD_ASYNC;
	int ret;
	int err;

	if (!device_is_ready(dev)) {
		ret = -ENODEV;
		goto free_bufs;
	}

	k_sem_init(&xfer->done, 0, 1);
	xfer->smc_status = SMC_STATUS_INVALID;
	request->priv_data = xfer;

	k_mutex_lock(&data->lock, K_FOREVER);

	ret = sip_svc_open(data->ctrl, data->token, K_MSEC(CONFIG_ALTERA_SDM_TIMEOUT_MS));
	if (ret != 0) {
		LOG_ERR("Failed to open SiP service client (%d)", ret);
		k_mutex_unlock(&data->lock);
		goto free_bufs;
	}

	ret = sip_svc_send(data->ctrl, data->token, request, altera_sdm_callback);
	if (ret < 0) {
		LOG_ERR("Failed to send SiP service request 0x%lx (%d)", request->a0, ret);
	} else {
		own_bufs = false;
		ret = k_sem_take(&xfer->done, K_MSEC(CONFIG_ALTERA_SDM_TIMEOUT_MS));
		if (ret != 0) {
			LOG_ERR("SiP service request 0x%lx timed out", request->a0);
			ret = -ETIMEDOUT;
		}
	}

	/*
	 * The callback runs with the client open. Once closed, a late response is
	 * dropped and its buffer freed by the SiP service, so xfer is not touched
	 * after this point.
	 */
	err = sip_svc_close(data->ctrl, data->token, NULL);
	if (err != 0) {
		LOG_ERR("Failed to close SiP service client (%d)", err);
		if (ret == 0) {
			ret = err;
		}
	}

	k_mutex_unlock(&data->lock);

free_bufs:
	if (own_bufs) {
		/* Not queued: both buffers are still owned here */
		k_free((void *)(uintptr_t)request->a2);
		k_free((void *)(uintptr_t)request->resp_data_addr);
	}

	return ret;
}

int altera_sdm_mailbox_send(const struct device *dev, uint16_t cmd, const uint32_t *args,
			    size_t nargs, uint32_t *resp, size_t *resp_len)
{
	struct altera_sdm_xfer xfer = {
		.resp = resp,
		.resp_cap = (resp_len != NULL) ? *resp_len : 0U,
	};
	struct sip_svc_request request = {0};
	size_t cmd_size = (SDM_MB_HEADER_WORDS + nargs) * sizeof(uint32_t);
	size_t resp_size = (SDM_MB_HEADER_WORDS + xfer.resp_cap) * sizeof(uint32_t);
	uint32_t *cmd_buf;
	uint32_t *resp_buf;
	int ret;

	if ((dev == NULL) || ((cmd & ~(SDM_MB_CMD_ID_MASK | ALTERA_SDM_MAILBOX_INDIRECT)) != 0U) ||
	    (nargs > ALTERA_SDM_MAILBOX_MAX_ARGS) || ((nargs > 0U) && (args == NULL)) ||
	    ((xfer.resp_cap > 0U) && (resp == NULL)) ||
	    (xfer.resp_cap > (SIP_SVP_MB_MAX_WORD_SIZE - SDM_MB_HEADER_WORDS))) {
		return -EINVAL;
	}

	cmd_buf = k_malloc(cmd_size);
	resp_buf = k_malloc(resp_size);
	if ((cmd_buf == NULL) || (resp_buf == NULL)) {
		k_free(cmd_buf);
		k_free(resp_buf);
		return -ENOMEM;
	}

	cmd_buf[0] = FIELD_PREP(SDM_MB_LENGTH_MASK, nargs) | (cmd & ALTERA_SDM_MAILBOX_INDIRECT) |
		     FIELD_PREP(SDM_MB_CMD_ID_MASK, cmd);
	if (nargs > 0U) {
		memcpy(&cmd_buf[SDM_MB_HEADER_WORDS], args, nargs * sizeof(uint32_t));
	}

	request.header = SIP_SVC_PROTO_HEADER(SIP_SVC_PROTO_CMD_ASYNC, 0);
	request.a0 = SMC_FUNC_ID_MAILBOX_SEND_COMMAND;
	request.a2 = (uint64_t)(uintptr_t)cmd_buf;
	request.a3 = cmd_size;
	request.resp_data_addr = (uint64_t)(uintptr_t)resp_buf;
	request.resp_data_size = resp_size;

	ret = altera_sdm_submit(dev, &request, &xfer);
	if (ret != 0) {
		return ret;
	}

	if (xfer.smc_status != SMC_STATUS_OKAY) {
		LOG_ERR("Mailbox command 0x%x: SMC call failed (%u)", cmd, xfer.smc_status);
		return -EIO;
	}

	if (FIELD_GET(SDM_MB_RESP_ERROR_MASK, xfer.header) != 0U) {
		LOG_ERR("Mailbox command 0x%x: SDM error 0x%lx", cmd,
			FIELD_GET(SDM_MB_RESP_ERROR_MASK, xfer.header));
		return -EIO;
	}

	if (FIELD_GET(SDM_MB_LENGTH_MASK, xfer.header) > xfer.resp_cap) {
		LOG_ERR("Mailbox command 0x%x: response of %lu words exceeds %zu", cmd,
			FIELD_GET(SDM_MB_LENGTH_MASK, xfer.header), xfer.resp_cap);
		return -EMSGSIZE;
	}

	if (resp_len != NULL) {
		*resp_len = xfer.resp_len;
	}

	return 0;
}

int altera_sdm_smc_call(const struct device *dev, uint32_t func_id, const uint64_t *args,
			size_t nargs, struct altera_sdm_smc_res *res)
{
	struct altera_sdm_xfer xfer = {0};
	struct sip_svc_request request = {0};
	uint64_t a[ALTERA_SDM_SMC_MAX_ARGS] = {0};
	int ret;

	if ((dev == NULL) || (nargs > ALTERA_SDM_SMC_MAX_ARGS) ||
	    ((nargs > 0U) && (args == NULL))) {
		return -EINVAL;
	}

	if (nargs > 0U) {
		memcpy(a, args, nargs * sizeof(uint64_t));
	}

	request.header = SIP_SVC_PROTO_HEADER(SIP_SVC_PROTO_CMD_SYNC, 0);
	request.a0 = func_id;
	request.a1 = a[0];
	request.a2 = a[1];
	request.a3 = a[2];
	request.a4 = a[3];
	request.a5 = a[4];
	request.a6 = a[5];
	request.a7 = a[6];

	ret = altera_sdm_submit(dev, &request, &xfer);
	if (ret != 0) {
		return ret;
	}

	if (res != NULL) {
		*res = xfer.regs;
	}

	/* The response header keeps only the low 16 bits of the status */
	if (xfer.regs.a0 != SMC_STATUS_OKAY) {
		LOG_ERR("SMC call 0x%x failed (0x%" PRIx64 ")", func_id, xfer.regs.a0);
		return -EIO;
	}

	return 0;
}

static int altera_sdm_init(const struct device *dev)
{
	const struct altera_sdm_config *config = dev->config;
	struct altera_sdm_data *data = dev->data;

	k_mutex_init(&data->lock);

	data->ctrl = sip_svc_get_controller((char *)config->method);
	if (data->ctrl == NULL) {
		LOG_ERR("SiP service controller \"%s\" not found", config->method);
		return -ENODEV;
	}

	data->token = sip_svc_register(data->ctrl, NULL);
	if (data->token == SIP_SVC_ID_INVALID) {
		LOG_ERR("Failed to register SiP service client");
		return -EBUSY;
	}

	return 0;
}

#define ALTERA_SDM_DEFINE(inst)                                                                    \
	BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_INST_PARENT(inst), intel_socfpga_agilex_sip_smc),       \
		     "SDM node must be a child of the SiP service controller node");               \
                                                                                                   \
	static const struct altera_sdm_config altera_sdm_config_##inst = {                         \
		.method = DT_PROP(DT_INST_PARENT(inst), method),                                   \
	};                                                                                         \
                                                                                                   \
	static struct altera_sdm_data altera_sdm_data_##inst;                                      \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, altera_sdm_init, NULL, &altera_sdm_data_##inst,                \
			      &altera_sdm_config_##inst, POST_KERNEL,                              \
			      CONFIG_ALTERA_SDM_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(ALTERA_SDM_DEFINE)

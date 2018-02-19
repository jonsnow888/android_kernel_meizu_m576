/*
 * escore.c  --  Audience earSmart Soc Audio driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "escore.h"
#include "escore-i2c.h"
#include "escore-slim.h"
#include "escore-spi.h"
#include "escore-uart.h"
#include "escore-i2s.h"
#include <linux/time.h>

#include "meizu-es705-codec.h"
#include "../board-meizu-audio.h"

struct escore_macro cmd_hist[ES_MAX_ROUTE_MACRO_CMD] = { {0} };

#ifdef ES_WDB_PROFILING
#define es_profiling(x) getnstimeofday(x)
#else
#define es_profiling(x)
#endif

int cmd_hist_index;
/* History struture, log route commands to debug */
/* Send a single command to the chip.
 *
 * If the SR (suppress response bit) is NOT set, will read the
 * response and cache it the driver object retrieve with escore_resp().
 *
 * Returns:
 * 0 - on success.
 * EITIMEDOUT - if the chip did not respond in within the expected time.
 * E* - any value that can be returned by the underlying HAL.
 */

int escore_cmd_nopm(struct escore_priv *escore, u32 cmd, u32 *resp)
{
	int sr;
	int err;

	*resp = 0;
	sr = cmd & BIT(28);
	err = escore->bus.ops.cmd(escore, cmd, resp);
	if (err || sr)
		goto exit;

	if (resp == 0) {
		err = -ETIMEDOUT;
		dev_err(escore->dev, "no response to command 0x%08x\n", cmd);
	} else {
		escore->bus.last_response = *resp;
		get_monotonic_boottime(&escore->last_resp_time);
	}

exit:
	return err;
}

int escore_cmd_locked(struct escore_priv *escore, u32 cmd, u32 *resp)
{
	int ret;

	mutex_lock(&escore->access_lock);
	ret = escore_pm_get_sync();
	if (ret > -1) {
		ret = escore_cmd_nopm(escore, cmd, resp);
		escore_pm_put_autosuspend();
	}
	mutex_unlock(&escore->access_lock);
	return ret;
}

int escore_cmd(struct escore_priv *escore, u32 cmd, u32 *resp)
{
	int ret;
	ret = escore_pm_get_sync();
	if (ret > -1) {
		ret = escore_cmd_nopm(escore, cmd, resp);
		escore_pm_put_autosuspend();
	}
	return ret;
}
int escore_write_block(struct escore_priv *escore, const u32 *cmd_block)
{
	int ret = 0;
	u32 resp;
	mutex_lock(&escore->access_lock);
	ret = escore_pm_get_sync();
	if (ret > -1) {
		while (*cmd_block != 0xffffffff) {
			escore_cmd_nopm(escore, *cmd_block, &resp);
			usleep_range(1000, 1005);
			cmd_block++;
		}
		escore_pm_put_autosuspend();
	}
	mutex_unlock(&escore->access_lock);
	return ret;
}

int escore_prepare_msg(struct escore_priv *escore, unsigned int reg,
		       unsigned int value, char *msg, int *len, int msg_type)
{
	struct escore_api_access *api_access;
	u32 api_word[2] = {0};
	unsigned int val_mask;
	int msg_len;

	if (reg > escore->api_addr_max) {
		pr_err("%s(): invalid address = 0x%04x\n", __func__, reg);
		return -EINVAL;
	}

	pr_debug("%s(): reg=%08x val=%d\n", __func__, reg, value);

	api_access = &escore->api_access[reg];
	val_mask = (1 << get_bitmask_order(api_access->val_max)) - 1;

	if (msg_type == ES_MSG_WRITE) {
		msg_len = api_access->write_msg_len;
		memcpy((char *)api_word, (char *)api_access->write_msg,
				msg_len);

		switch (msg_len) {
		case 8:
			api_word[1] |= ((val_mask & value) <<
						api_access->val_shift);
			break;
		case 4:
			api_word[0] |= ((val_mask & value) <<
						api_access->val_shift);
			break;
		}
	} else {
		msg_len = api_access->read_msg_len;
		memcpy((char *)api_word, (char *)api_access->read_msg,
				msg_len);
	}

	*len = msg_len;
	memcpy(msg, (char *)api_word, *len);

	return 0;

}

static unsigned int _escore_read(struct snd_soc_codec *codec, unsigned int reg)
{
	struct escore_priv *escore = &escore_priv;
	u32 api_word[2] = {0};
	unsigned int msg_len;
	unsigned int value = 0;
	u32 resp;
	int rc;

	rc = escore_prepare_msg(escore, reg, value, (char *) api_word,
			&msg_len, ES_MSG_READ);
	if (rc) {
		pr_err("%s(): Prepare read message fail %d\n",
		       __func__, rc);
		goto out;
	}

	rc = escore_cmd_nopm(escore, api_word[0], &resp);
	if (rc < 0) {
		pr_err("%s(): _escore_cmd failed, rc = %d", __func__, rc);
		return rc;
	}
	api_word[0] = escore->bus.last_response;

	value = api_word[0] & 0xffff;
out:
	return value;
}

/* Locked variant of escore_read():
 * Exclusive firmware access is guaranteed when this variant is called.
 */
unsigned int escore_read_locked(struct snd_soc_codec *codec, unsigned int reg)
{
	unsigned int ret = 0;
	int rc;
	struct escore_priv *escore = &escore_priv;

	mutex_lock(&escore->access_lock);
	rc = escore_pm_get_sync();
	if (rc > -1) {
		ret = _escore_read(codec, reg);
		escore_pm_put_autosuspend();
	}
	mutex_unlock(&escore->access_lock);
	return ret;
}

/* READ API to firmware:
 * This API may be interrupted. If there is a series of READs  being issued to
 * firmware, there must be a fw_access lock acquired in order to ensure the
 * atomicity of entire operation.
 */
unsigned int escore_read(struct snd_soc_codec *codec, unsigned int reg)
{
	unsigned int ret = 0;
	int rc;
	rc = escore_pm_get_sync();
	if (rc > -1) {
		ret = _escore_read(codec, reg);
		escore_pm_put_autosuspend();
	}
	return ret;
}

static int _escore_write(struct snd_soc_codec *codec, unsigned int reg,
		       unsigned int value)
{
	struct escore_priv *escore = &escore_priv;
	u32 api_word[2] = {0};
	int msg_len;
	u32 resp;
	int rc;
	int i;

	rc = escore_prepare_msg(escore, reg, value, (char *) api_word,
			&msg_len, ES_MSG_WRITE);
	if (rc) {
		pr_err("%s(): Failed to prepare write message %d\n",
		       __func__, rc);
		goto out;
	}

	for (i = 0; i < msg_len / 4; i++) {
		rc = escore_cmd_nopm(escore, api_word[i], &resp);
		if (rc < 0) {
			pr_err("%s(): escore_cmd()", __func__);
			return rc;
		}
	}
	pr_debug("%s(): mutex unlock\n", __func__);
out:
	return rc;
}

/* This function must be called with access_lock acquired */
int escore_reconfig_intr(struct escore_priv *escore)
{
	int rc = 0;
	u32 cmd, resp;

	cmd = ((ES_SYNC_CMD | ES_SUPRESS_RESPONSE) << 16);
	if (escore->pdata->gpioa_gpio != -1) {
		/* Set Interrupt Mode */
		escore->cmd_compl_mode = ES_CMD_COMP_INTR;
		cmd |= escore->pdata->gpio_a_irq_type;
	}

	rc = escore_cmd_nopm(escore, cmd, &resp);
	if (rc < 0) {
		dev_err(escore->dev,
				"%s() - failed sync cmd resume rc = %d\n",
				__func__, rc);
		if (escore->pdata->gpioa_gpio != -1)
			escore->cmd_compl_mode = ES_CMD_COMP_POLL;
		goto out;
	}

	if (escore->config_jack) {
		rc = escore->config_jack(escore);
		if (rc < 0) {
			dev_err(escore->dev, "%s() - jack config failed : %d\n",
					__func__, rc);
			goto out;
		}
	} else {
		/* Setup the Event response */
		cmd = (ES_SET_EVENT_RESP << 16) | \
						escore->pdata->gpio_b_irq_type;
		rc = escore_cmd_nopm(escore, cmd, &resp);
		if (rc < 0) {
			dev_err(escore->dev,
				"%s(): Error %d in setting event response\n",
				__func__, rc);
			goto out;
		}
	}
out:
	return rc;
}

int escore_datablock_open(struct escore_priv *escore)
{
	int rc = 0;
	if (escore->bus.ops.high_bw_open)
		rc = escore->bus.ops.high_bw_open(escore);
	return rc;
}

int escore_datablock_close(struct escore_priv *escore)
{
	int rc = 0;
	if (escore->bus.ops.high_bw_close)
		rc = escore->bus.ops.high_bw_close(escore);
	return rc;
}

int escore_datablock_wait(struct escore_priv *escore)
{
	int rc = 0;
	if (escore->bus.ops.high_bw_wait)
		rc = escore->bus.ops.high_bw_wait(escore);
	return rc;
}

int escore_datablock_read(struct escore_priv *escore, void *buf,
		size_t len, int id)
{
	int rc;
	int size;
	u32 cmd;
	int rdcnt = 0;
	u32 resp;
	u8 flush_extra_blk = 0;
	u32 flush_buf;

	/* Reset read data block size */
	escore->datablock_dev.rdb_read_count = 0;

	if (escore->bus.ops.rdb) {
		rc = escore->bus.ops.rdb(escore, buf, len, id);
		goto out;
	}
	cmd = (ES_READ_DATA_BLOCK << 16) | (id & 0xFFFF);

	rc = escore->bus.ops.high_bw_cmd(escore, cmd, &resp);
	if (rc < 0) {
		pr_err("%s(): escore_cmd() failed rc = %d\n", __func__, rc);
		goto out;
	}
	if ((resp >> 16) != ES_READ_DATA_BLOCK) {
		pr_err("%s(): Invalid response received: 0x%08x\n",
				__func__, resp);
		rc = -EINVAL;
		goto out;
	}

	size = resp & 0xFFFF;
	pr_debug("%s(): RDB size = %d\n", __func__, size);
	if (size == 0 || size % 4 != 0) {
		pr_err("%s(): Read Data Block with invalid size:%d\n",
				__func__, size);
		rc = -EINVAL;
		goto out;
	}

	if (len != size) {
		pr_debug("%s(): Requested:%zd Received:%d\n", __func__,
				len, size);
		if (len < size)
			flush_extra_blk = (size - len) % 4;
		else
			len = size;
	}

	for (rdcnt = 0; rdcnt < len;) {
		rc = escore->bus.ops.high_bw_read(escore, buf, 4);
		if (rc < 0) {
			pr_err("%s(): Read Data Block error %d\n",
					__func__, rc);
			goto out;
		}
		rdcnt += 4;
		buf += 4;
	}
	/* Store read data block size */
	escore->datablock_dev.rdb_read_count = size;

	/* No need to read in case of no extra bytes */
	if (flush_extra_blk) {
		/* Discard the extra bytes */
		rc = escore->bus.ops.high_bw_read(escore, &flush_buf,
							flush_extra_blk);
		if (rc < 0) {
			pr_err("%s(): Read Data Block error in flushing %d\n",
					__func__, rc);
			goto out;
		}
	}
	return len;
out:
	return rc;
}

int escore_datablock_write(struct escore_priv *escore, const void *buf,
		size_t len)
{
	int rc;
	int count;
	u32 resp;
	u32 cmd = ES_WRITE_DATA_BLOCK << 16;
	size_t size = 0;
	size_t remaining = len;
	u8 *dptr = (u8 *) buf;
#if defined(CONFIG_SND_SOC_ES_SPI) || \
	defined(CONFIG_SND_SOC_ES_HIGH_BW_BUS_SPI)
	u16 resp16;
#endif

#ifdef ES_WDB_PROFILING
	struct timespec tstart;
	struct timespec tend;
	struct timespec tstart_cmd;
	struct timespec tend_cmd;
	struct timespec tstart_wdb;
	struct timespec tend_wdb;
	struct timespec tstart_resp;
	struct timespec tend_resp;
#endif
	pr_debug("%s() len = %zd\n", __func__, len);
	es_profiling(&tstart);
	es_profiling(&tstart_cmd);

	while (remaining) {

		/* If multiple WDB blocks are written, some delay is required
		 * before starting next WDB. This delay is not documented but
		 * if this delay is not added, extra zeros are obsevred in
		 * escore_uart_read() causing WDB failure.
		 */
		if (len > ES_WDB_MAX_SIZE)
			usleep_range(2000, 2050);

		size = remaining > ES_WDB_MAX_SIZE ?
		       ES_WDB_MAX_SIZE : remaining;

		cmd = ES_WRITE_DATA_BLOCK << 16;
		cmd = cmd | (size & 0xFFFF);
		pr_debug("%s(): cmd = 0x%08x\n", __func__, cmd);
		rc = escore->bus.ops.high_bw_cmd(escore, cmd, &resp);
		if (rc < 0) {
			pr_err("%s(): escore_cmd() failed rc = %d\n",
					__func__, rc);
			goto out;
		}
		if ((resp >> 16) != ES_WRITE_DATA_BLOCK) {
			pr_err("%s(): Invalid response received: 0x%08x\n",
					__func__, resp);
			rc = -EIO;
			goto out;
		}
		es_profiling(&tend_cmd);
		es_profiling(&tstart_wdb);

		rc = escore->bus.ops.high_bw_write(escore, dptr, size);
		if (rc < 0) {
			pr_err("%s(): WDB error:%d\n", __func__, rc);
			goto out;
		}
		es_profiling(&tend_wdb);
		/* After completing wdb response should be 0x802f0000,
		   retry until we receive response*/
		es_profiling(&tstart_resp);
#if defined(CONFIG_SND_SOC_ES_SPI) || \
	defined(CONFIG_SND_SOC_ES_HIGH_BW_BUS_SPI)
		count = ES_SPI_MAX_RETRIES; /* retries for SPI only */
#else
		count = ES_MAX_RETRIES + 5;
#endif
		while (count-- > 0) {
			resp = 0;
#if (defined(CONFIG_SND_SOC_ES_SPI)) || \
	defined(CONFIG_SND_SOC_ES_HIGH_BW_BUS_SPI)
			resp16 = 0;
			rc = escore->bus.ops.high_bw_read(escore, &resp16,
					sizeof(resp16));
			if (rc < 0) {
				pr_err("%s(): WDB last ACK read error:%d\n",
					__func__, rc);
				goto out;
			}
			if (resp16 == ES_WRITE_DATA_BLOCK_SPI) {
				resp = (cpu_to_be16(resp16)) << 16;
				resp16 = 0;
				rc = escore->bus.ops.high_bw_read(escore,
						&resp16, sizeof(resp16));
				if (rc < 0) {
					pr_err("%s(): WDB last ACK read error:%d\n",
						__func__, rc);
					goto out;
				}
				resp |= cpu_to_be16(resp16);
				if (resp != (ES_WRITE_DATA_BLOCK << 16)) {
					pr_debug("%s(): response not ready 0x%0x\n",
							__func__, resp);
					rc = -EIO;
				} else {
					break;
				}
			} else {
				pr_debug("%s(): Invalid response 0x%0x\n",
						__func__, resp16);
				rc = -EIO;
			}
			if (count % ES_SPI_CONT_RETRY == 0) {
				usleep_range(ES_SPI_RETRY_DELAY,
				ES_SPI_RETRY_DELAY + 200);
			}
#else
			rc = escore->bus.ops.high_bw_read(escore, &resp,
					sizeof(resp));
			if (rc < 0) {
				pr_err("%s(): WDB last ACK read error:%d\n",
					__func__, rc);
				goto out;
			}

			resp = escore->bus.ops.bus_to_cpu(escore, resp);
			if (resp != (ES_WRITE_DATA_BLOCK << 16)) {
				pr_debug("%s(): response not ready 0x%0x\n",
						__func__, resp);
				rc = -EIO;
			} else {
				break;
			}
			usleep_range(1000, 1005);
#endif
		}
		if (rc == -EIO) {
			pr_err("%s(): write data block error 0x%0x\n",
					__func__, resp);
			goto out;
		}
		pr_debug("%s(): resp = 0x%08x\n", __func__, resp);

		dptr += size;
		remaining -= size;
	}
	es_profiling(&tend_resp);

	es_profiling(&tend);
#ifdef ES_WDB_PROFILING
	tstart = (timespec_sub(tstart, tend));
	tstart_cmd = (timespec_sub(tstart_cmd, tend_cmd));
	tstart_wdb = (timespec_sub(tstart_wdb, tend_wdb));
	tstart_resp = (timespec_sub(tstart_resp, tend_resp));

	dev_info(escore->dev, "tend-tstart = %lu,\n"
			"cmd = %lu,\n"
			"wdb = %lu,\n"
			"resp = %lu,\n",
			(tstart.tv_nsec)/1000000,
			(tstart_cmd.tv_nsec)/1000000,
			(tstart_wdb.tv_nsec)/1000000,
			(tstart_resp.tv_nsec)/1000000);
#endif
	return len;

out:
	return rc;
}

/* Locked variant of escore_write():
 * Exclusive firmware access is guaranteed when this variant is called.
 */
int escore_write_locked(struct snd_soc_codec *codec, unsigned int reg,
		       unsigned int value)
{
	int ret;
	struct escore_priv *escore = &escore_priv;

	mutex_lock(&escore->access_lock);
	ret = escore_pm_get_sync();
	if (ret > -1) {
		ret = _escore_write(codec, reg, value);
		escore_pm_put_autosuspend();
	}
	mutex_unlock(&escore->access_lock);
	return ret;

}

/* WRITE API to firmware:
 * This API may be interrupted. If there is a series of WRITEs or READs  being
 * issued to firmware, there must be a fw_access lock acquired in order to
 * ensure the atomicity of entire operation.
 */
int escore_write(struct snd_soc_codec *codec, unsigned int reg,
		       unsigned int value)
{
	int ret;
	ret = escore_pm_get_sync();
	if (ret > -1) {
		ret = _escore_write(codec, reg, value);
		escore_pm_put_autosuspend();
	}
	return ret;

}

int escore_start_int_osc(struct escore_priv *escore)
{
	int rc = 0;
	int retry = MAX_RETRY_TO_SWITCH_TO_LOW_POWER_MODE;
	u32 cmd, rsp;

	dev_info(escore->dev, "%s()\n", __func__);

	/* Start internal Osc. */
	cmd = ES_INT_OSC_MEASURE_START << 16;
	rc = escore_cmd_nopm(escore, cmd, &rsp);
	if (rc) {
		dev_err(escore->dev, "%s(): Int Osc Msr Start cmd fail %d\n",
			__func__, rc);
		goto escore_int_osc_exit;
	}

	/* Poll internal Osc. status */
	do {
		/*
		 * Wait 20ms each time before reading
		 * up to 100ms
		 */
		msleep(20);
		cmd = ES_INT_OSC_MEASURE_STATUS << 16;
		rc = escore_cmd_nopm(escore, cmd, &rsp);
		if (rc) {
			dev_err(escore->dev,
				"%s(): Int Osc Msr Sts cmd fail %d\n",
				__func__, rc);
			goto escore_int_osc_exit;
		}
		rsp &= 0xFFFF;
		dev_dbg(escore->dev,
			"%s(): OSC Measure Status = 0x%04x\n", __func__, rsp);
	} while (rsp && --retry);

	if (rsp > 0) {
		dev_err(escore->dev,
			"%s(): Unexpected OSC Measure Status = 0x%04x\n",
			__func__, rc);
		dev_err(escore->dev,
			"%s(): Can't switch to Low Power Mode\n", __func__);
	}

escore_int_osc_exit:
	return rc;
}


/* API Interrupt completion handler */
int escore_api_intr_wait_completion(struct escore_priv *escore)
{
	int rc = 0;

	pr_debug("%s(): Waiting for API interrupt", __func__);
	rc = wait_for_completion_timeout(&escore->cmd_compl,
			msecs_to_jiffies(ES_API_INTR_TOUT_MSEC));
	if (!rc) {
		rc = -ETIMEDOUT;
		dev_err(escore->dev,
			"%s(): API Interrupt wait timeout %d\n", __func__, rc);
		escore->wait_api_intr = 0;
	} else {
		rc = 0;
	}

	return rc;
}

int escore_wakeup(struct escore_priv *escore)
{
	u32 cmd = ES_SYNC_CMD << 16;
	u32 rsp;
	int rc = 0;
	int retry = 20;
	u32 p_cmd = ES_GET_POWER_STATE << 16;

	escore->cmd_compl_mode = ES_CMD_COMP_POLL;
	/* Enable the clocks */
	if (escore_priv.pdata->esxxx_clk_cb) {
		escore_priv.pdata->esxxx_clk_cb(1);
		/* Setup for clock stablization delay */
		msleep(ES_PM_CLOCK_STABILIZATION);
	}

	if (escore->pri_intf == ES_SPI_INTF)
		msleep(ES_WAKEUP_TIME);

	do {
		/* Set flag to Wait for API Interrupt */
		if (escore->pdata->gpioa_gpio != -1)
			escore_set_api_intr_wait(escore);

		/* Toggle the wakeup pin H->L the L->H */
		if (escore->wakeup_intf == ES_UART_INTF &&
				escore->escore_uart_wakeup) {
			rc = escore->escore_uart_wakeup(escore);
			if (rc) {
				dev_err(escore->dev,
						"%s() Wakeup failed rc = %d\n",
						__func__, rc);
				goto escore_wakeup_exit;
			}
		} else if (escore->pdata->wakeup_gpio != -1) {
			gpio_set_value(escore->pdata->wakeup_gpio, 1);
			usleep_range(1000, 1005);
			gpio_set_value(escore->pdata->wakeup_gpio, 0);
			usleep_range(1000, 1005);
			gpio_set_value(escore->pdata->wakeup_gpio, 1);
			usleep_range(1000, 1005);
			gpio_set_value(escore->pdata->wakeup_gpio, 0);
		}

		/* Wait for API Interrupt to confirm
		 * that device is active */
		if (escore->pdata->gpioa_gpio != -1) {
			rc = escore_api_intr_wait_completion(escore);
			if (rc) {
				pr_err("%s(): Wakeup wait failed %d\n",
						__func__, rc);
				goto escore_wakeup_exit;
			}
		} else {
			/* Give the device time to "wakeup" */
			msleep(ES_WAKEUP_TIME);
		}
		if (escore->pri_intf == ES_SPI_INTF) {
			if (escore->pdata->gpioa_gpio == -1)
				msleep(ES_WAKEUP_TIME);
			rc = escore_cmd_nopm(escore, p_cmd, &rsp);
			if (rc < 0) {
				pr_err("%s() - failed check power status" \
					" rc = %d\n", __func__, rc);
				continue;
			}
			if ((rsp != ES_PS_NORMAL) && (rsp != ES_PS_OVERLAY)) {
				rc = -1;
				continue;
			}
		}

		/* Set Interrupt mode after wakeup */
		if (escore->pdata->gpioa_gpio != -1) {
			cmd |= escore->pdata->gpio_a_irq_type;
			escore->cmd_compl_mode = ES_CMD_COMP_INTR;
			rc = escore_cmd_nopm(escore,
					(cmd | (ES_SUPRESS_RESPONSE << 16)),
					&rsp);
			if (rc < 0) {
				dev_err(escore->dev,
				    "%s() - Failed sync cmd resume rc = %d\n",
				    __func__, rc);
				escore->cmd_compl_mode = ES_CMD_COMP_POLL;
				goto escore_wakeup_exit;
			}
		}
		rc = escore_cmd_nopm(escore, cmd, &rsp);
		if (rc < 0) {
			dev_err(escore->dev,
				"%s(): failed sync cmd resume %d\n",
				__func__, rc);
		}
		if (cmd != rsp) {
			dev_err(escore->dev,
				"%s(): failed sync rsp resume %d\n",
				__func__, rc);
			rc = -EIO;
		}
	} while (rc && --retry);

	/* Set the Smooth Mute rate to Zero */
	cmd = ES_SET_SMOOTH_MUTE << 16 | ES_SMOOTH_MUTE_ZERO;
	rc = escore_cmd_nopm(escore, cmd, &rsp);
	if (rc) {
		dev_err(escore->dev, "%s(): Set Smooth Mute cmd fail %d\n",
			__func__, rc);
		goto escore_wakeup_exit;
	}

escore_wakeup_exit:
	return rc;
}

int escore_get_runtime_pm_enum(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.enumerated.item[0] = escore_priv.pm_enable;

	return 0;
}

int escore_put_runtime_pm_enum(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	unsigned int value;

	value = ucontrol->value.enumerated.item[0];

	if (value)
		escore_pm_enable();
	else
		escore_pm_disable();

	return 0;
}


int escore_put_control_enum(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	unsigned int reg = e->reg;
	unsigned int value;
	int rc = 0;
	struct escore_priv *escore = &escore_priv;

	mutex_lock(&escore->access_lock);
	rc = escore_pm_get_sync();
	if (rc > -1) {
		value = ucontrol->value.enumerated.item[0];
		rc = _escore_write(NULL, reg, value);
		escore_pm_put_autosuspend();
	}
	mutex_unlock(&escore->access_lock);
	return 0;
}

int escore_get_control_enum(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	int ret;
	struct soc_enum *e =
		(struct soc_enum *)kcontrol->private_value;
	unsigned int reg = e->reg;
	unsigned int value;
	struct escore_priv *escore = &escore_priv;

	mutex_lock(&escore->access_lock);
	ret = escore_pm_get_sync();
	if (ret > -1) {
		value = _escore_read(NULL, reg);
		ucontrol->value.enumerated.item[0] = value;
		escore_pm_put_autosuspend();
	}
	mutex_unlock(&escore->access_lock);
	return 0;
}

int escore_put_control_value(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	unsigned int reg = mc->reg;
	unsigned int value;
	int ret = 0;
	struct escore_priv *escore = &escore_priv;

	mutex_lock(&escore->access_lock);
	ret = escore_pm_get_sync();
	if (ret  > -1) {
		value = ucontrol->value.integer.value[0];
		ret = _escore_write(NULL, reg, value);
		escore_pm_put_autosuspend();
	}
	mutex_unlock(&escore->access_lock);
	return ret;
}

int escore_get_control_value(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	int ret;
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	unsigned int reg = mc->reg;
	unsigned int value;
	struct escore_priv *escore = &escore_priv;

	mutex_lock(&escore->access_lock);
	ret = escore_pm_get_sync();
	if (ret  > -1) {
		value = _escore_read(NULL, reg);
		ucontrol->value.integer.value[0] = value;
		escore_pm_put_autosuspend();
		ret = 0;
	}
	mutex_unlock(&escore->access_lock);
	return ret;
}

int escore_put_streaming_mode(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	escore_priv.es_streaming_mode = ucontrol->value.enumerated.item[0];
	return 0;
}

int escore_get_streaming_mode(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.enumerated.item[0] = escore_priv.es_streaming_mode;

	return 0;
}
#ifdef CONFIG_SND_SOC_ES_CVQ_SINGLE_INTF
int escore_switch_ext_osc(struct escore_priv *escore)
{
	u32 cmd_resp;
	int rc = 0;

	dev_dbg(escore->dev, "%s(): Switch ext oscillator", __func__);

	/* Sending preset command to switch to external oscillator */
	rc = escore_cmd(escore, (ES_SET_PRESET << 16 | ES_ASR_PRESET),
			&cmd_resp);
	if (rc) {
		dev_err(escore_priv.dev, "%s(): Set Preset fail %d\n",
			__func__, rc);
		goto cmd_error;
	}
	usleep_range(2000, 2005);

	rc = escore_cmd(escore, (ES_GET_POWER_LEVEL << 16), &cmd_resp);
	if (rc) {
		dev_err(escore->dev, "%s(): Error getting power level %d\n",
			__func__, rc);
		goto cmd_error;
	} else if (cmd_resp != ((ES_GET_POWER_LEVEL << 16)
				| ES_POWER_LEVEL_6)) {
		dev_err(escore->dev, "%s(): Invalid power level 0x%04x\n",
				__func__, cmd_resp);
		rc = -EINVAL;
	}
	usleep_range(2000, 2005);

cmd_error:
	return rc;
}
#endif

void escore_register_notify(struct blocking_notifier_head *list,
		struct notifier_block *nb)
{
	blocking_notifier_chain_register(list, nb);
}

void escore_gpio_reset(struct escore_priv *escore)
{
	if (escore->pdata->reset_gpio == -1) {
		pr_warn("%s(): Reset GPIO not initialized\n", __func__);
		return;
	}

	gpio_set_value(escore->pdata->reset_gpio, 0);
	/* Wait 1 ms then pull Reset signal in High */
	usleep_range(1000, 1005);
	gpio_set_value(escore->pdata->reset_gpio, 1);
	/* Wait 10 ms then */
	usleep_range(10000, 10050);
	/* eSxxx is READY */
	escore->flag.reset_done = 1;
	escore->mode = SBL;
}

int escore_probe(struct escore_priv *escore, struct device *dev, int curr_intf,
		int context)
{
	int rc = 0;

	mutex_lock(&escore->intf_probed_mutex);

	/* Update intf_probed only when a valid interface is probed */

	if (curr_intf != ES_INVAL_INTF)
		escore->intf_probed |= curr_intf;

	if (curr_intf == escore->pri_intf) {
		escore->dev = dev;

		es705_codec_add_dev(); // register es705 codec

		if (context == ES_CONTEXT_THREAD) {
			rc = escore_retrigger_probe();
			if (rc)
				pr_err("%s(): Adding UART dummy dev failed\n", __func__);
		}
	}

	if (escore->intf_probed != (escore->pri_intf | escore->high_bw_intf)) {
		pr_debug("%s(): Both interfaces are not probed %d\n",
				__func__, escore->intf_probed);
		mutex_unlock(&escore->intf_probed_mutex);
		return 0;
	}
	mutex_unlock(&escore->intf_probed_mutex);

	if (escore->wakeup_intf == ES_UART_INTF && !escore->uart_ready) {
		pr_err("%s(): Wakeup mechanism not initialized\n", __func__);
		return 0;
	}

#ifdef CONFIG_ARCH_EXYNOS
	rc = meizu_audio_regulator_init(escore->dev);
	if (rc) {
		dev_err(escore->dev, "Failed to init regulator\n");
		return rc;
	}
	meizu_audio_clock_init();
	usleep_range(1000, 1000);
#endif

	escore->bus.setup_prim_intf(escore);

	rc = escore->bus.setup_high_bw_intf(escore);
	if (rc) {
		pr_err("%s(): Error while setting up high bw interface %d\n",
				__func__, rc);
		goto out;
	}

	if (escore->flag.is_codec) {
		rc = snd_soc_register_codec(escore->dev,
				escore->soc_codec_dev_escore,
				escore->dai,
				escore->dai_nr);

		if (rc) {
			pr_err("%s(): Codec registration failed %d\n",
					__func__, rc);
			goto out;
		}
	}

	/* Enable the gpiob IRQ */
	if (escore_priv.pdata->gpiob_gpio != -1)
		enable_irq(gpio_to_irq(escore_priv.pdata->gpiob_gpio));

#ifdef CONFIG_SLIMBUS_MSM_NGD
	if (escore_priv.high_bw_intf != ES_SLIM_INTF)
		complete(&escore->fw_download);
#else
	complete(&escore->fw_download);
#endif
	escore_pm_enable();

out:
	return rc;
}

static struct platform_device *escore_dummy_device;

/*
 * Helper routine to retrigger the probe context when some probe() routines
 * have returned pre-maturely with -EPROBE_DEFER
 */
int escore_retrigger_probe(void)
{
	int rc = 0;

	/* Release previously alloc'ed device */
	if (escore_dummy_device)
		platform_device_put(escore_dummy_device);

	escore_dummy_device = platform_device_alloc("escore-codec.dummy", -1);
	if (!escore_dummy_device) {
		pr_err("%s(): dummy platform device allocation failed\n",
				__func__);
		rc = -ENOMEM;
		goto out;
	}

	rc = platform_device_add(escore_dummy_device);
	if (rc) {
		pr_err("%s(): Error while adding dummy device %d\n",
		       __func__, rc);
		platform_device_put(escore_dummy_device);
	}
out:
	return rc;
}

static int escore_plat_probe(struct platform_device *pdev)
{
	int rc = 0;
	struct escore_pdata *pdata;

	pr_debug("%s()\n", __func__);
	pdata = pdev->dev.platform_data;

	if (pdata && pdata->probe)
		rc = pdata->probe(pdev);

	return rc;
}

static int escore_plat_remove(struct platform_device *pdev)
{
	int rc = 0;
	struct escore_pdata *pdata;

	pr_debug("%s()\n", __func__);
	pdata = pdev->dev.platform_data;

	if (pdata && pdata->remove)
		rc = pdata->remove(pdev);

	return rc;
}

static struct platform_device_id escore_id_table[] = {
	{
		/* For UART device */
		.name = "escore-codec.uart",
	}, {
		/* For Dummy device to re-trigger probe context */
		.name = "escore-codec.dummy",
	}, {
		/* sentinel */
	}
};

struct platform_driver escore_plat_driver = {
	.driver = {
		.name = "escore-codec",
		.owner = THIS_MODULE,
	},
	.probe = escore_plat_probe,
	.remove = escore_plat_remove,
	.id_table = escore_id_table,
};

int escore_platform_init(void)
{
	int rc;

	rc = platform_driver_register(&escore_plat_driver);
	if (rc)
		return rc;

	pr_debug("%s(): Registered escore platform driver", __func__);

	return rc;
}

#ifdef CONFIG_ARCH_EXYNOS
static noinline_for_stack long fw_file_size(struct file *file)
{
	struct kstat st;
	if (vfs_getattr(&file->f_path, &st))
		return -1;
	if (!S_ISREG(st.mode))
		return -1;
	if (st.size != (long)st.size)
		return -1;
	return st.size;
}

int meizu_escore_request_firmware(const struct firmware **fw,
				   const char *file,
				   struct device *device)
{
	struct firmware *firmware;
	struct file *filp = NULL;
	long size, err = 0;
	char *buf;
	int i;
	char *path = __getname();
	unsigned long timeout = jiffies + msecs_to_jiffies(60 * 1000); // 60 seconds
	static const char * const fw_path[] = {
		"/etc/firmware/",
		"/data/data/com.android.settings/files",
		"/data/data/com.audience.voiceqmultikeyword/files/",
	};

	*fw = firmware = kzalloc(sizeof(*firmware), GFP_KERNEL);
	if (!firmware) {
		dev_err(device, "%s: kmalloc(struct firmware) failed\n", __func__);
		return -ENOMEM;
	}

	do {
		for (i = 0; i < ARRAY_SIZE(fw_path); i++) {
			/* skip the unset customized path */
			if (!fw_path[i][0])
				continue;

			snprintf(path, PATH_MAX, "%s/%s", fw_path[i], file);

			filp = filp_open(path, O_RDONLY, 0);
			if (!IS_ERR(filp))
				break;
		}

		err = PTR_ERR(filp);
	} while (time_before(jiffies, timeout) && err == -ENOENT);

	__putname(path);

	if (IS_ERR(filp)) {
		dev_err(device, "%s: open firmware failed\n", __func__);
		goto err_filp_open;
	}

	size = fw_file_size(filp);
	if (size <= 0)
		goto err_invalid_size;
	buf = kmalloc(size, GFP_KERNEL);
	if (!buf)
		goto err_kmalloc;

	if (kernel_read(filp, 0, buf, size) != size) {
		dev_err(device, "%s: read firmware failed\n", __func__);
		goto err_kernel_read;
	}
	firmware->data = buf;
	firmware->size = size;

	return 0;

err_kernel_read:
	kfree(buf);
err_kmalloc:
err_invalid_size:
	filp_close(filp, NULL);
err_filp_open:
	kfree(firmware);
	return -EINVAL;
}

void meizu_escore_release_firmware(const struct firmware *fw)
{
	if (fw) {
		if (fw->data)
			kfree(fw->data);
		kfree(fw);
	}
}
#endif


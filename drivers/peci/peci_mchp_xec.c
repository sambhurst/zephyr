/*
 * Copyright (c) 2020 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT microchip_xec_peci

#include <errno.h>
#include <device.h>
#ifdef CONFIG_SOC_SERIES_MEC172X
#include <drivers/clock_control/mchp_xec_clock_control.h>
#include <drivers/interrupt_controller/intc_mchp_xec_ecia.h>
#endif
#include <drivers/peci.h>
#ifdef CONFIG_PINCTRL
#include <drivers/pinctrl.h>
#endif
#include <soc.h>
#include <logging/log.h>
LOG_MODULE_REGISTER(peci_mchp_xec, CONFIG_PECI_LOG_LEVEL);

/* Maximum PECI core clock is the main clock 48Mhz */
#define MAX_PECI_CORE_CLOCK 48000u
/* 1 ms */
#define PECI_RESET_DELAY    1000u
#define PECI_RESET_DELAY_MS 1u
/* 100 us */
#define PECI_IDLE_DELAY     100u
/* 5 ms */
#define PECI_IDLE_TIMEOUT   50u
/* Maximum retries */
#define PECI_TIMEOUT_RETRIES 3u
/* Maximum read buffer fill wait retries */
#define PECI_RX_BUF_FILL_WAIT_RETRY 100u

/* 10 us */
#define PECI_IO_DELAY       10

#define OPT_BIT_TIME_MSB_OFS 8u

#define PECI_FCS_LEN         2

struct peci_xec_config {
	struct peci_regs * const regs;
	uint8_t irq_num;
	uint8_t girq;
	uint8_t girq_pos;
	uint8_t pcr_idx;
	uint8_t pcr_pos;
#ifdef CONFIG_PINCTRL
	const struct pinctrl_dev_config *pcfg;
#endif
};

struct peci_xec_data {
	struct k_sem tx_lock;
	uint32_t  bitrate;
	int    timeout_retries;
};

#ifdef CONFIG_SOC_SERIES_MEC172X
static inline void peci_girq_enable(const struct device *dev)
{
	const struct peci_xec_config * const cfg = dev->config;

	mchp_xec_ecia_girq_src_en(cfg->girq, cfg->girq_pos);
}

static inline void peci_girq_status_clear(const struct device *dev)
{
	const struct peci_xec_config * const cfg = dev->config;

	mchp_soc_ecia_girq_src_clr(cfg->girq, cfg->girq_pos);
}

static inline void peci_clr_slp_en(const struct device *dev)
{
	const struct peci_xec_config * const cfg = dev->config;

	z_mchp_xec_pcr_periph_sleep(cfg->pcr_idx, cfg->pcr_pos, 0);
}
#else
static inline void peci_girq_enable(const struct device *dev)
{
	const struct peci_xec_config * const cfg = dev->config;

	MCHP_GIRQ_ENSET(cfg->girq) = BIT(cfg->girq_pos);
}

static inline void peci_girq_status_clear(const struct device *dev)
{
	const struct peci_xec_config * const cfg = dev->config;

	MCHP_GIRQ_SRC(cfg->girq) = BIT(cfg->girq_pos);
}

static inline void peci_clr_slp_en(const struct device *dev)
{
	ARG_UNUSED(dev);

	mchp_pcr_periph_slp_ctrl(PCR_PECI, 0);
}
#endif

static int check_bus_idle(struct peci_regs * const regs)
{
	uint8_t delay_cnt = PECI_IDLE_TIMEOUT;

	/* Wait until PECI bus becomes idle.
	 * Note that when IDLE bit in the status register changes, HW do not
	 * generate an interrupt, so need to poll.
	 */
	while (!(regs->STATUS2 & MCHP_PECI_STS2_IDLE)) {
		k_busy_wait(PECI_IDLE_DELAY);
		delay_cnt--;

		if (!delay_cnt) {
			LOG_WRN("Bus is busy");
			return -EBUSY;
		}
	}
	return 0;
}

static int peci_xec_configure(const struct device *dev, uint32_t bitrate)
{
	const struct peci_xec_config * const cfg = dev->config;
	struct peci_xec_data * const data = dev->data;
	struct peci_regs * const regs = cfg->regs;
	uint16_t value;

	data->bitrate = bitrate;

	/* Power down PECI interface */
	regs->CONTROL = MCHP_PECI_CTRL_PD;

	/* Adjust bitrate */
	value = MAX_PECI_CORE_CLOCK / bitrate;
	regs->OPT_BIT_TIME_LSB = value & MCHP_PECI_OPT_BT_LSB_MASK;
	regs->OPT_BIT_TIME_MSB = ((value >> OPT_BIT_TIME_MSB_OFS) &
				  MCHP_PECI_OPT_BT_MSB_MASK);

	/* Power up PECI interface */
	regs->CONTROL &= ~MCHP_PECI_CTRL_PD;

	return 0;
}

static int peci_xec_disable(const struct device *dev)
{
	const struct peci_xec_config * const cfg = dev->config;
	struct peci_regs * const regs = cfg->regs;
	int ret;

	/* Make sure no transaction is interrupted before disabling the HW */
	ret = check_bus_idle(regs);
	if (ret) {
		return ret;
	}

#ifdef CONFIG_PECI_INTERRUPT_DRIVEN
	peci_girq_status_clear(dev);
	NVIC_ClearPendingIRQ(cfg->irq_num);
	irq_disable(cfg->irq_num);
#endif
	regs->CONTROL |= MCHP_PECI_CTRL_PD;

	return 0;
}

static int peci_xec_enable(const struct device *dev)
{
	const struct peci_xec_config * const cfg = dev->config;
	struct peci_regs * const regs = cfg->regs;

	regs->CONTROL &= ~MCHP_PECI_CTRL_PD;

#ifdef CONFIG_PECI_INTERRUPT_DRIVEN
	peci_girq_status_clear(dev);
	peci_girq_enable(dev);
	irq_enable(cfg->irq_num);
#endif
	return 0;
}

static void peci_xec_bus_recovery(const struct device *dev, bool full_reset)
{
	const struct peci_xec_config * const cfg = dev->config;
	struct peci_xec_data * const data = dev->data;
	struct peci_regs * const regs = cfg->regs;

	LOG_WRN("%s full_reset:%d", __func__, full_reset);
	if (full_reset) {
		regs->CONTROL = MCHP_PECI_CTRL_PD | MCHP_PECI_CTRL_RST;

		if (k_is_in_isr()) {
			k_busy_wait(PECI_RESET_DELAY_MS);
		} else {
			k_msleep(PECI_RESET_DELAY);
		}

		regs->CONTROL &= ~MCHP_PECI_CTRL_RST;

		peci_xec_configure(dev, data->bitrate);
	} else {
		/* Only reset internal FIFOs */
		regs->CONTROL |= MCHP_PECI_CTRL_FRST;
	}
}

static int peci_xec_write(const struct device *dev, struct peci_msg *msg)
{
	const struct peci_xec_config * const cfg = dev->config;
	struct peci_xec_data * const data = dev->data;
	struct peci_regs * const regs = cfg->regs;
	int i;
	int ret;

	struct peci_buf *tx_buf = &msg->tx_buffer;
	struct peci_buf *rx_buf = &msg->rx_buffer;

	/* Check if FIFO is full */
	if (regs->STATUS2 & MCHP_PECI_STS2_WFF) {
		LOG_WRN("%s FIFO is full", __func__);
		return -EIO;
	}

	regs->CONTROL &= ~MCHP_PECI_CTRL_FRST;

	/* Add PECI transaction header to TX FIFO */
	regs->WR_DATA = msg->addr;
	regs->WR_DATA = tx_buf->len;
	regs->WR_DATA = rx_buf->len;

	/* Add PECI payload to Tx FIFO only if write length is valid */
	if (tx_buf->len) {
		regs->WR_DATA = msg->cmd_code;
		for (i = 0; i < tx_buf->len - 1; i++) {
			if (!(regs->STATUS2 & MCHP_PECI_STS2_WFF)) {
				regs->WR_DATA = tx_buf->buf[i];
			}
		}
	}

	/* Check bus is idle before starting a new transfer */
	ret = check_bus_idle(regs);
	if (ret) {
		return ret;
	}

	regs->CONTROL |= MCHP_PECI_CTRL_TXEN;
	k_busy_wait(PECI_IO_DELAY);

	/* Wait for transmission to complete */
#ifdef CONFIG_PECI_INTERRUPT_DRIVEN
	if (k_sem_take(&data->tx_lock, PECI_IO_DELAY * tx_buf->len)) {
		return -ETIMEDOUT;
	}
#else
	/* In worst case, overall timeout will be 1msec (100 * 10usec) */
	uint8_t wait_timeout_cnt = 100;

	while (!(regs->STATUS1 & MCHP_PECI_STS1_EOF)) {
		k_busy_wait(PECI_IO_DELAY);
		wait_timeout_cnt--;
		if (!wait_timeout_cnt) {
			LOG_WRN("Tx timeout");
			data->timeout_retries++;
			/* Full reset only if multiple consecutive failures */
			if (data->timeout_retries > PECI_TIMEOUT_RETRIES) {
				peci_xec_bus_recovery(dev, true);
			} else {
				peci_xec_bus_recovery(dev, false);
			}

			return -ETIMEDOUT;
		}
	}
#endif
	data->timeout_retries = 0;

	return 0;
}

static int peci_xec_read(const struct device *dev, struct peci_msg *msg)
{
	const struct peci_xec_config * const cfg = dev->config;
	struct peci_regs * const regs = cfg->regs;
	int i;
	int ret;
	uint8_t tx_fcs;
	uint8_t bytes_rcvd;
	uint8_t wait_timeout_cnt;
	struct peci_buf *rx_buf = &msg->rx_buffer;

	/* Attempt to read data from RX FIFO */
	bytes_rcvd = 0;
	for (i = 0; i < (rx_buf->len + PECI_FCS_LEN); i++) {
		/* Worst case timeout will be 1msec (100 * 10usec) */
		wait_timeout_cnt = PECI_RX_BUF_FILL_WAIT_RETRY;
		/* Wait for read buffer to fill up */
		while (regs->STATUS2 & MCHP_PECI_STS2_RFE) {
			k_usleep(PECI_IO_DELAY);
			wait_timeout_cnt--;
			if (!wait_timeout_cnt) {
				LOG_WRN("Rx buffer empty");
				return -ETIMEDOUT;
			}
		}

		if (i == 0) {
			/* Get write block FCS just for debug */
			tx_fcs = regs->RD_DATA;
			LOG_DBG("TX FCS %x", tx_fcs);

			/* If a Ping is done, write Tx fcs to rx buffer*/
			if (msg->cmd_code == PECI_CMD_PING) {
				rx_buf->buf[0] = tx_fcs;
				break;
			}
		} else if (i == (rx_buf->len + 1)) {
			/* Get read block FCS, but don't count it */
			rx_buf->buf[i-1] = regs->RD_DATA;
		} else {
			/* Get response */
			rx_buf->buf[i-1] = regs->RD_DATA;
			bytes_rcvd++;
		}
	}

	/* Check if transaction is as expected */
	if (rx_buf->len != bytes_rcvd) {
		LOG_INF("Incomplete %x vs %x", bytes_rcvd, rx_buf->len);
	}

	/* Once write-read transaction is complete, ensure bus is idle
	 * before resetting the internal FIFOs
	 */
	ret = check_bus_idle(regs);
	if (ret) {
		return ret;
	}

	return 0;
}

static int peci_xec_transfer(const struct device *dev, struct peci_msg *msg)
{
	const struct peci_xec_config * const cfg = dev->config;
	struct peci_regs * const regs = cfg->regs;
	int ret;
	uint8_t err_val;

	ret = peci_xec_write(dev, msg);
	if (ret) {
		return ret;
	}

	/* If a PECI transmission is successful, it may or not involve
	 * a read operation, check if transaction expects a response
	 * Also perform a read when PECI cmd is Ping to get Write FCS
	 */
	if (msg->rx_buffer.len || (msg->cmd_code == PECI_CMD_PING)) {
		ret = peci_xec_read(dev, msg);
		if (ret) {
			return ret;
		}
	}

	/* Cleanup */
	if (regs->STATUS1 & MCHP_PECI_STS1_EOF) {
		regs->STATUS1 |= MCHP_PECI_STS1_EOF;
	}

	/* Check for error conditions and perform bus recovery if necessary */
	err_val = regs->ERROR;
	if (err_val) {
		if (err_val & MCHP_PECI_ERR_RDOV) {
			LOG_ERR("Read buffer is not empty");
		}

		if (err_val & MCHP_PECI_ERR_WRUN) {
			LOG_ERR("Write buffer is not empty");
		}

		if (err_val & MCHP_PECI_ERR_BERR) {
			LOG_ERR("PECI bus error");
		}

		LOG_DBG("PECI err %x", err_val);
		LOG_DBG("PECI sts1 %x", regs->STATUS1);
		LOG_DBG("PECI sts2 %x", regs->STATUS2);

		/* ERROR is a clear-on-write register, need to clear errors
		 * occurring at the end of a transaction. A temp variable is
		 * used to overcome complaints by the static code analyzer
		 */
		regs->ERROR = err_val;
		peci_xec_bus_recovery(dev, false);
		return -EIO;
	}

	return 0;
}

#ifdef CONFIG_PECI_INTERRUPT_DRIVEN
static void peci_xec_isr(const void *arg)
{
	const struct device *dev = arg;
	struct peci_xec_config * const cfg = dev->config;
	struct peci_xec_data * const data = dev->data;
	struct peci_regs * const regs = cfg->regs;
	uint8_t peci_error = regs->ERROR;
	uint8_t peci_status2 = regs->STATUS2;

	peci_girq_status_clear(dev);

	if (peci_error) {
		regs->ERROR = peci_error;
	}

	if (peci_status2 & MCHP_PECI_STS2_WFE) {
		LOG_WRN("TX FIFO empty ST2:%x", peci_status2);
		k_sem_give(&data->tx_lock);
	}

	if (peci_status2 & MCHP_PECI_STS2_RFE) {
		LOG_WRN("RX FIFO full ST2:%x", peci_status2);
	}
}
#endif

static const struct peci_driver_api peci_xec_driver_api = {
	.config = peci_xec_configure,
	.enable = peci_xec_enable,
	.disable = peci_xec_disable,
	.transfer = peci_xec_transfer,
};

static int peci_xec_init(const struct device *dev)
{
	const struct peci_xec_config * const cfg = dev->config;
	struct peci_regs * const regs = cfg->regs;

#ifdef CONFIG_PINCTRL
	int ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);

	if (ret != 0) {
		LOG_ERR("XEC PECI pinctrl init failed (%d)", ret);
		return ret;
	}
#endif

#ifdef CONFIG_PECI_INTERRUPT_DRIVEN
	k_sem_init(&data->tx_lock, 0, 1);
#endif

	peci_clr_slp_en(dev);

	/* Reset PECI interface */
	regs->CONTROL |= MCHP_PECI_CTRL_RST;
	k_msleep(PECI_RESET_DELAY_MS);
	regs->CONTROL &= ~MCHP_PECI_CTRL_RST;

#ifdef CONFIG_PECI_INTERRUPT_DRIVEN
	/* Enable interrupt for errors */
	regs->INT_EN1 = (MCHP_PECI_IEN1_EREN | MCHP_PECI_IEN1_EIEN);

	/* Enable interrupt for Tx FIFO is empty */
	regs->INT_EN2 |= MCHP_PECI_IEN2_ENWFE;
	/* Enable interrupt for Rx FIFO is full */
	regs->INT_EN2 |= MCHP_PECI_IEN2_ENRFF;

	regs->CONTROL |= MCHP_PECI_CTRL_MIEN;

	/* Direct NVIC */
	IRQ_CONNECT(cfg->irq_num,
		    DT_INST_IRQ(0, priority),
		    peci_xec_isr, NULL, 0);
#endif
	return 0;
}

static struct peci_xec_data peci_data;

#ifdef CONFIG_PINCTRL
PINCTRL_DT_INST_DEFINE(0);
#endif

static const struct peci_xec_config peci_xec_config = {
	.regs = (struct peci_regs * const)(DT_INST_REG_ADDR(0)),
	.irq_num = DT_INST_IRQN(0),
	.girq = DT_INST_PROP_BY_IDX(0, girqs, 0),
	.girq_pos = DT_INST_PROP_BY_IDX(0, girqs, 1),
	.pcr_idx = DT_INST_PROP_BY_IDX(0, pcrs, 0),
	.pcr_pos = DT_INST_PROP_BY_IDX(0, pcrs, 1),
#ifdef CONFIG_PINCTRL
	.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(0),
#endif
};

DEVICE_DT_INST_DEFINE(0,
		    &peci_xec_init,
		    NULL,
		    &peci_data, &peci_xec_config,
		    POST_KERNEL, CONFIG_PECI_INIT_PRIORITY,
		    &peci_xec_driver_api);

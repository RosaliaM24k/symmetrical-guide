/*
 * pxa-ssp.c  --  ALSA Soc Audio Layer
 *
 * Copyright 2005,2008 Wolfson Microelectronics PLC.
 * Author: Liam Girdwood
 *         Mark Brown <broonie@opensource.wolfsonmicro.com>
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 * TODO:
 *  o Test network mode for > 16bit sample size
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/pxa2xx_ssp.h>
#include <linux/of.h>
#include <linux/dmaengine.h>
#include <linux/delay.h>
#include <linux/platform_data/mmp_audio.h>
#include <linux/pinctrl/consumer.h>
#include <linux/of_gpio.h>

#include <asm/irq.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/pxa2xx-lib.h>
#include <sound/dmaengine_pcm.h>

#include <mach/hardware.h>

#include "../../arm/pxa2xx-pcm.h"
#include "pxa-ssp.h"

/*
 * SSP audio private data
 */
struct ssp_priv {
	struct ssp_device *ssp;
	unsigned int sysclk;
	int dai_fmt;
	unsigned int burst_size;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pin_ssp;
	struct pinctrl_state *pin_gpio;
	int mfp;
	int usr_cnt;
	bool mfp_init;
#ifdef CONFIG_PM
	uint32_t	cr0;
	uint32_t	cr1;
	uint32_t	to;
	uint32_t	psp;
#endif
	/*
	 * FixMe: for port 5 (gssp), it is shared by ap
	 * and cp. When AP want to handle it, AP need to
	 * configure APB to connect gssp. Also reset gssp
	 * clk to clear the potential impact from cp
	 */
	void __iomem	*apbcp_base;
};

static ssize_t ssp_mfp_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct ssp_priv *priv = dev_get_drvdata(dev);
	if (!priv)
		return sprintf(buf, "%s\n", "get ssp-priv failed!!!\n");

	if (priv->ssp->port_id == 2)
		pr_debug("i2s pin mfp setting:\n");
	else if (priv->ssp->port_id == 5)
		pr_debug("gssp pin mfp setting:\n");

	return sprintf(buf, "%s\n", (priv->mfp ? "ssp" : "gpio"));
}

static ssize_t ssp_mfp_set(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	int ret, i;
	u32 pins_ssp;
	struct ssp_priv *priv = dev_get_drvdata(dev);
	struct device_node *np = dev->of_node;

	if (!priv)
		return -EINVAL;

	if (IS_ERR(priv->pin_ssp) || IS_ERR(priv->pin_gpio))
		return -EINVAL;

	ret = kstrtoint(buf, 10, &priv->mfp);
	if (ret)
		return ret;

	/* mfp = 0, set to gpio; mfp = 1, set to ssp */
	if (priv->mfp) {
		ret = pinctrl_select_state(priv->pinctrl, priv->pin_ssp);
		if (ret) {
			dev_err(dev, "could not set ssp pins\n");
			goto err_out;
		}
	} else {
		ret = pinctrl_select_state(priv->pinctrl, priv->pin_gpio);
		if (ret) {
			dev_err(dev, "could not set default(gpio) pins\n");
			goto err_out;
		}

		for (i = 0; i < 4; i++) {
			pins_ssp = of_get_named_gpio(np, "ssp-gpio", i);
			gpio_request(pins_ssp, NULL);
			gpio_direction_input(pins_ssp);
			gpio_free(pins_ssp);
		}
	}

	if (priv->ssp->port_id == 2)
		pr_debug("i2s pin set to %s\n", (priv->mfp ? "ssp" : "gpio"));
	else if (priv->ssp->port_id == 5)
		pr_debug("gssp pin set to %s\n", (priv->mfp ? "ssp" : "gpio"));

err_out:
	return count;
}

static DEVICE_ATTR(gssp_mfp, 0644, ssp_mfp_show, ssp_mfp_set);
static DEVICE_ATTR(ssp_mfp, 0644, ssp_mfp_show, ssp_mfp_set);

static void dump_registers(struct ssp_device *ssp)
{
	dev_dbg(&ssp->pdev->dev, "SSCR0 0x%08x SSCR1 0x%08x SSTO 0x%08x\n",
		 pxa_ssp_read_reg(ssp, SSCR0), pxa_ssp_read_reg(ssp, SSCR1),
		 pxa_ssp_read_reg(ssp, SSTO));

	dev_dbg(&ssp->pdev->dev, "SSPSP 0x%08x SSSR 0x%08x SSACD 0x%08x\n",
		 pxa_ssp_read_reg(ssp, SSPSP), pxa_ssp_read_reg(ssp, SSSR),
		 pxa_ssp_read_reg(ssp, SSACD));
}

static void pxa_ssp_enable(struct ssp_device *ssp)
{
	uint32_t sscr0;

	sscr0 = __raw_readl(ssp->mmio_base + SSCR0) | SSCR0_SSE;
	__raw_writel(sscr0, ssp->mmio_base + SSCR0);
}

static void pxa_ssp_disable(struct ssp_device *ssp)
{
	uint32_t sscr0;

	sscr0 = __raw_readl(ssp->mmio_base + SSCR0) & ~SSCR0_SSE;
	__raw_writel(sscr0, ssp->mmio_base + SSCR0);
}

static void pxa_ssp_set_dma_params(struct ssp_device *ssp, int width4,
			int out, struct snd_dmaengine_dai_dma_data *dma)
{
	dma->addr_width = width4 ? DMA_SLAVE_BUSWIDTH_4_BYTES :
				   DMA_SLAVE_BUSWIDTH_2_BYTES;

	if (dma->maxburst > PXA_SSP_FIFO_DEPTH)
		dma->maxburst = PXA_SSP_FIFO_DEPTH;

	dma->addr = ssp->phys_base + SSDR;
}

static int pxa_ssp_startup(struct snd_pcm_substream *substream,
			   struct snd_soc_dai *cpu_dai)
{
	struct ssp_priv *priv = snd_soc_dai_get_drvdata(cpu_dai);
	struct ssp_device *ssp = priv->ssp;
	struct snd_dmaengine_dai_dma_data *dma;
	unsigned int gcer;
	int ret = 0;

	/*
	 * when audio stream start, set ssp1 to ssp mfp.
	 * always config gssp pins mfpr incase CP modify
	 */
	if ((ssp->port_id == 5) || !priv->mfp_init) {
		ret = pinctrl_select_state(priv->pinctrl, priv->pin_ssp);
		if (ret) {
			dev_err(cpu_dai->dev, "could not set ssp pins\n");
			return ret;
		}
		priv->mfp_init = true;
		priv->mfp = 1;
	}

	if (!cpu_dai->active) {
		/*
		 * FIXME: for port 5 (gssp), it is shared by ap
		 * and cp. When AP want to handle it, AP need to
		 * configure APB to connect gssp. Also reset gssp
		 * clk to clear the potential impact from cp
		 */
		if (ssp->port_id == 5) {
			/* GPB bus select: choose APB */
			__raw_writel(GSSP_BUS_APB_SEL, (priv->apbcp_base + APBC_GBS));
			/* GSSP clock control register: GCER */
			gcer = __raw_readl(priv->apbcp_base + APBC_GCER);
			gcer &= ~(GSSP_CLK_SEL_MASK << GSSP_CLK_SEL_OFF);
			gcer |= GSSP_RST;
			__raw_writel(gcer, (priv->apbcp_base + APBC_GCER));
			usleep_range(1, 2);
			gcer &= ~GSSP_RST;
			__raw_writel(gcer, (priv->apbcp_base + APBC_GCER));
			usleep_range(1, 2);
		}

		clk_prepare_enable(ssp->clk);
		/*
		 * Since gssp is used for hifi record, enable
		 * gssp port when startup to eliminate noise.
		 */
		if (ssp->port_id == 5) {
			/*
			 * gssp(ssp-dai.4) is shared by AP and CP.AP would read
			 * the gssp register configured by CP after voice call.
			 * it would impact DMA configuration. AP and CP use
			 * different GSSP configuration, and CP's configuration
			 * would set DMA to 16bit width while AP set it to 32
			 * bit. so we need to re-init GSSP register setting.
			 */
			__raw_writel(0x0, ssp->mmio_base + SSCR0);
			__raw_writel(0x0, ssp->mmio_base + SSCR1);
			__raw_writel(0x0, ssp->mmio_base + SSPSP);
			__raw_writel(0x0, ssp->mmio_base + SSTSA);
			/*
			 * Before enable gssp port, need to set frame format
			 * and data size for PCM format, otherwise there
			 * will be no frame clock.
			 * Also need set bit23: Receive Without Transmit of
			 * sscr1, or can't record.
			 */
			__raw_writel(0x0010001F, ssp->mmio_base + SSCR0);
			__raw_writel(0x00800000, ssp->mmio_base + SSCR1);
		}
		if (ssp->port_id == 2) {
			/*
			 * Before enable ssp port, need to set frame format
			 * and data size, otherwise there will be no frame
			 * clock.
			 * Also need set bit23: Receive Without Transmit of
			 * sscr1, or can't record.
			 */
			__raw_writel(0x0010003F, ssp->mmio_base + SSCR0);
			__raw_writel(0x00800000, ssp->mmio_base + SSCR1);
			__raw_writel(0x02100004, ssp->mmio_base + SSPSP);
		}
		pxa_ssp_enable(ssp);
		priv->usr_cnt = 0;
	}

	dma = kzalloc(sizeof(struct snd_dmaengine_dai_dma_data), GFP_KERNEL);
	if (!dma)
		return -ENOMEM;

	dma->filter_data = substream->stream == SNDRV_PCM_STREAM_PLAYBACK ?
				&ssp->drcmr_tx : &ssp->drcmr_rx;

	snd_soc_dai_set_dma_data(cpu_dai, substream, dma);

	return ret;
}

static void pxa_ssp_shutdown(struct snd_pcm_substream *substream,
			     struct snd_soc_dai *cpu_dai)
{
	struct ssp_priv *priv = snd_soc_dai_get_drvdata(cpu_dai);
	struct ssp_device *ssp = priv->ssp;

	if (!cpu_dai->active) {
		pxa_ssp_disable(ssp);
		clk_disable_unprepare(ssp->clk);

		/*
		 * FIXME: for port 5 (gssp), it is shared by ap
		 * and cp. When AP want to handle it, AP need to
		 * configure APB to connect gssp. Also reset gssp
		 * clk to clear the potential impact from cp
		 */
		if (ssp->port_id == 5) {
			/* GPB bus select: choose GPB */
			__raw_writel(0, (priv->apbcp_base + APBC_GBS));
		}
	}

	kfree(snd_soc_dai_get_dma_data(cpu_dai, substream));
	snd_soc_dai_set_dma_data(cpu_dai, substream, NULL);
}

#ifdef CONFIG_PM

static int pxa_ssp_suspend(struct snd_soc_dai *cpu_dai)
{
	return 0;
}

static int pxa_ssp_resume(struct snd_soc_dai *cpu_dai)
{
	return 0;
}

#else
#define pxa_ssp_suspend	NULL
#define pxa_ssp_resume	NULL
#endif

/**
 * ssp_set_clkdiv - set SSP clock divider
 * @div: serial clock rate divider
 */
static void pxa_ssp_set_scr(struct ssp_device *ssp, u32 div)
{
	u32 sscr0 = pxa_ssp_read_reg(ssp, SSCR0);

	if (ssp->type == PXA25x_SSP) {
		sscr0 &= ~0x0000ff00;
		sscr0 |= ((div - 2)/2) << 8; /* 2..512 */
	} else {
		sscr0 &= ~0x000fff00;
		sscr0 |= (div - 1) << 8;     /* 1..4096 */
	}
	pxa_ssp_write_reg(ssp, SSCR0, sscr0);
}

/**
 * pxa_ssp_get_clkdiv - get SSP clock divider
 */
static u32 pxa_ssp_get_scr(struct ssp_device *ssp)
{
	u32 sscr0 = pxa_ssp_read_reg(ssp, SSCR0);
	u32 div;

	if (ssp->type == PXA25x_SSP)
		div = ((sscr0 >> 8) & 0xff) * 2 + 2;
	else
		div = ((sscr0 >> 8) & 0xfff) + 1;
	return div;
}

/*
 * Set the SSP ports SYSCLK.
 */
static int pxa_ssp_set_dai_sysclk(struct snd_soc_dai *cpu_dai,
	int clk_id, unsigned int freq, int dir)
{
	struct ssp_priv *priv = snd_soc_dai_get_drvdata(cpu_dai);
	struct ssp_device *ssp = priv->ssp;
	int val;

	u32 sscr0 = pxa_ssp_read_reg(ssp, SSCR0) &
		~(SSCR0_ECS |  SSCR0_NCS | SSCR0_MOD | SSCR0_ACS);

	dev_dbg(&ssp->pdev->dev,
		"pxa_ssp_set_dai_sysclk id: %d, clk_id %d, freq %u\n",
		cpu_dai->id, clk_id, freq);

	switch (clk_id) {
	case PXA_SSP_CLK_NET_PLL:
		sscr0 |= SSCR0_MOD;
		break;
	case PXA_SSP_CLK_PLL:
		/* Internal PLL is fixed */
		if (ssp->type == PXA25x_SSP)
			priv->sysclk = 1843200;
		else
			priv->sysclk = 13000000;
		break;
	case PXA_SSP_CLK_EXT:
		priv->sysclk = freq;
		sscr0 |= SSCR0_ECS;
		break;
	case PXA_SSP_CLK_NET:
		priv->sysclk = freq;
		sscr0 |= SSCR0_NCS | SSCR0_MOD;
		break;
	case PXA_SSP_CLK_AUDIO:
		priv->sysclk = 0;
		pxa_ssp_set_scr(ssp, 1);
		sscr0 |= SSCR0_ACS;
		break;
	default:
		return -ENODEV;
	}

	/* The SSP clock must be disabled when changing SSP clock mode
	 * on PXA2xx.  On PXA3xx it must be enabled when doing so. */
	if (ssp->type != PXA3xx_SSP)
		clk_disable_unprepare(ssp->clk);
	val = pxa_ssp_read_reg(ssp, SSCR0) | sscr0;
	pxa_ssp_write_reg(ssp, SSCR0, val);
	if (ssp->type != PXA3xx_SSP)
		clk_prepare_enable(ssp->clk);

	return 0;
}

/*
 * Set the SSP clock dividers.
 */
static int pxa_ssp_set_dai_clkdiv(struct snd_soc_dai *cpu_dai,
	int div_id, int div)
{
	struct ssp_priv *priv = snd_soc_dai_get_drvdata(cpu_dai);
	struct ssp_device *ssp = priv->ssp;
	int val;

	switch (div_id) {
	case PXA_SSP_AUDIO_DIV_ACDS:
		val = (pxa_ssp_read_reg(ssp, SSACD) & ~0x7) | SSACD_ACDS(div);
		pxa_ssp_write_reg(ssp, SSACD, val);
		break;
	case PXA_SSP_AUDIO_DIV_SCDB:
		val = pxa_ssp_read_reg(ssp, SSACD);
		val &= ~SSACD_SCDB;
		if (ssp->type == PXA3xx_SSP)
			val &= ~SSACD_SCDX8;
		switch (div) {
		case PXA_SSP_CLK_SCDB_1:
			val |= SSACD_SCDB;
			break;
		case PXA_SSP_CLK_SCDB_4:
			break;
		case PXA_SSP_CLK_SCDB_8:
			if (ssp->type == PXA3xx_SSP)
				val |= SSACD_SCDX8;
			else
				return -EINVAL;
			break;
		default:
			return -EINVAL;
		}
		pxa_ssp_write_reg(ssp, SSACD, val);
		break;
	case PXA_SSP_AUDIO_DIV_ACPS:
		val = pxa_ssp_read_reg(ssp, SSACD);
		val &= ~0x70;
		pxa_ssp_write_reg(ssp, SSACD, val);
		val |= SSACD_ACPS(div);
		pxa_ssp_write_reg(ssp, SSACD, val);
		break;
	case PXA_SSP_DIV_SCR:
		pxa_ssp_set_scr(ssp, div);
		break;
	default:
		return -ENODEV;
	}

	return 0;
}

/*
 * Configure the PLL frequency pxa27x and (afaik - pxa320 only)
 */
static int pxa_ssp_set_dai_pll(struct snd_soc_dai *cpu_dai, int pll_id,
	int source, unsigned int freq_in, unsigned int freq_out)
{
	struct ssp_priv *priv = snd_soc_dai_get_drvdata(cpu_dai);
	struct ssp_device *ssp = priv->ssp;
	u32 ssacd = pxa_ssp_read_reg(ssp, SSACD) & ~0x70;

	if (ssp->type == PXA3xx_SSP)
		pxa_ssp_write_reg(ssp, SSACDD, 0);

	switch (freq_out) {
	case 5622000:
		break;
	case 11345000:
		ssacd |= (0x1 << 4);
		break;
	case 12235000:
		ssacd |= (0x2 << 4);
		break;
	case 14857000:
		ssacd |= (0x3 << 4);
		break;
	case 32842000:
		ssacd |= (0x4 << 4);
		break;
	case 48000000:
		ssacd |= (0x5 << 4);
		break;
	case 0:
		/* Disable */
		break;

	default:
		/* PXA3xx has a clock ditherer which can be used to generate
		 * a wider range of frequencies - calculate a value for it.
		 */
		if (ssp->type == PXA3xx_SSP) {
			u32 val;
			u64 tmp = 19968;
			tmp *= 1000000;
			do_div(tmp, freq_out);
			val = tmp;

			val = (val << 16) | 64;
			pxa_ssp_write_reg(ssp, SSACDD, val);

			ssacd |= (0x6 << 4);

			dev_dbg(&ssp->pdev->dev,
				"Using SSACDD %x to supply %uHz\n",
				val, freq_out);
			break;
		}

		return -EINVAL;
	}

	pxa_ssp_write_reg(ssp, SSACD, ssacd);

	return 0;
}

/*
 * Set the active slots in TDM/Network mode
 */
static int pxa_ssp_set_dai_tdm_slot(struct snd_soc_dai *cpu_dai,
	unsigned int tx_mask, unsigned int rx_mask, int slots, int slot_width)
{
	struct ssp_priv *priv = snd_soc_dai_get_drvdata(cpu_dai);
	struct ssp_device *ssp = priv->ssp;
	u32 sscr0;

	sscr0 = pxa_ssp_read_reg(ssp, SSCR0);
	sscr0 &= ~(SSCR0_MOD | SSCR0_SlotsPerFrm(8) | SSCR0_EDSS | SSCR0_DSS);

	/* set slot width */
	if (slot_width > 16)
		sscr0 |= SSCR0_EDSS | SSCR0_DataSize(slot_width - 16);
	else
		sscr0 |= SSCR0_DataSize(slot_width);

	if (slots > 1) {
		/* enable network mode */
		sscr0 |= SSCR0_MOD;

		/* set number of active slots */
		sscr0 |= SSCR0_SlotsPerFrm(slots);

		/* set active slot mask */
		pxa_ssp_write_reg(ssp, SSTSA, tx_mask);
		pxa_ssp_write_reg(ssp, SSRSA, rx_mask);
	}
	pxa_ssp_write_reg(ssp, SSCR0, sscr0);

	return 0;
}

/*
 * Tristate the SSP DAI lines
 */
static int pxa_ssp_set_dai_tristate(struct snd_soc_dai *cpu_dai,
	int tristate)
{
	struct ssp_priv *priv = snd_soc_dai_get_drvdata(cpu_dai);
	struct ssp_device *ssp = priv->ssp;
	u32 sscr1;

	sscr1 = pxa_ssp_read_reg(ssp, SSCR1);
	if (tristate)
		sscr1 &= ~SSCR1_TTE;
	else
		sscr1 |= SSCR1_TTE;
	pxa_ssp_write_reg(ssp, SSCR1, sscr1);

	return 0;
}

/*
 * Set up the SSP DAI format.
 * The SSP Port must be inactive before calling this function as the
 * physical interface format is changed.
 */
static int pxa_ssp_set_dai_fmt(struct snd_soc_dai *cpu_dai,
		unsigned int fmt)
{
	struct ssp_priv *priv = snd_soc_dai_get_drvdata(cpu_dai);
	struct ssp_device *ssp = priv->ssp;
	u32 sscr0, sscr1, sspsp, scfr;

	/* check if we need to change anything at all */
	if (priv->dai_fmt == fmt)
		return 0;

	/* we can only change the settings if the port is not in use */
	if (priv->usr_cnt) {
		dev_err(&ssp->pdev->dev,
			"can't change hardware dai format: stream is in use");
		return -EINVAL;
	}

	/* reset port settings */
	sscr0 = pxa_ssp_read_reg(ssp, SSCR0) &
		~(SSCR0_ECS |  SSCR0_NCS | SSCR0_MOD | SSCR0_ACS);
	sscr1 = SSCR1_RxTresh(8) | SSCR1_TxTresh(7);
	sspsp = 0;

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
		sscr1 |= SSCR1_SCLKDIR | SSCR1_SFRMDIR | SSCR1_SCFR;
		break;
	case SND_SOC_DAIFMT_CBM_CFS:
		sscr1 |= SSCR1_SCLKDIR | SSCR1_SCFR;
		break;
	case SND_SOC_DAIFMT_CBS_CFS:
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		sspsp |= SSPSP_SFRMP;
		break;
	case SND_SOC_DAIFMT_NB_IF:
		break;
	case SND_SOC_DAIFMT_IB_IF:
		sspsp |= SSPSP_SCMODE(2);
		break;
	case SND_SOC_DAIFMT_IB_NF:
		sspsp |= SSPSP_SCMODE(2) | SSPSP_SFRMP;
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		sscr0 |= SSCR0_PSP;
		sscr1 |= SSCR1_RWOT | SSCR1_TRAIL;
		/* See hw_params() */
		break;

	case SND_SOC_DAIFMT_DSP_A:
		sspsp |= SSPSP_FSRT;
	case SND_SOC_DAIFMT_DSP_B:
		sscr0 |= SSCR0_MOD | SSCR0_PSP;
		sscr1 |= SSCR1_TRAIL | SSCR1_RWOT;
		break;

	default:
		return -EINVAL;
	}

	pxa_ssp_write_reg(ssp, SSCR0, sscr0);
	pxa_ssp_write_reg(ssp, SSCR1, sscr1);
	pxa_ssp_write_reg(ssp, SSPSP, sspsp);

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
	case SND_SOC_DAIFMT_CBM_CFS:
		scfr = pxa_ssp_read_reg(ssp, SSCR1) | SSCR1_SCFR;
		pxa_ssp_write_reg(ssp, SSCR1, scfr);

		while (pxa_ssp_read_reg(ssp, SSSR) & SSSR_BSY)
			cpu_relax();
		break;
	}

	dump_registers(ssp);

	/* Since we are configuring the timings for the format by hand
	 * we have to defer some things until hw_params() where we
	 * know parameters like the sample size.
	 */
	priv->dai_fmt = fmt;

	return 0;
}

/*
 * Set the SSP audio DMA parameters and sample size.
 * Can be called multiple times by oss emulation.
 */
static int pxa_ssp_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *cpu_dai)
{
	struct ssp_priv *priv = snd_soc_dai_get_drvdata(cpu_dai);
	struct ssp_device *ssp = priv->ssp;
	int chn = params_channels(params);
	u32 sscr0;
	u32 sspsp;
	int width = snd_pcm_format_physical_width(params_format(params));
	int ttsa = pxa_ssp_read_reg(ssp, SSTSA) & 0xf;
	struct snd_dmaengine_dai_dma_data *dma_data;
	u32 rate;
	int ret;

	dma_data = snd_soc_dai_get_dma_data(cpu_dai, substream);

	dma_data->maxburst = priv->burst_size;

	/* Network mode with one active slot (ttsa == 1) can be used
	 * to force 16-bit frame width on the wire (for S16_LE), even
	 * with two channels. Use 16-bit DMA transfers for this case.
	 */
	pxa_ssp_set_dma_params(ssp,
		((chn == 2) && (ttsa != 1)) || (width == 32),
		substream->stream == SNDRV_PCM_STREAM_PLAYBACK, dma_data);

	/* we can only change the settings if the port is not in use */
	if (priv->usr_cnt)
		return 0;

	/* clear selected SSP bits */
	sscr0 = pxa_ssp_read_reg(ssp, SSCR0) & ~(SSCR0_DSS | SSCR0_EDSS);

	/* bit size */
	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		if (ssp->type == PXA3xx_SSP)
			sscr0 |= SSCR0_FPCKE;
		sscr0 |= SSCR0_DataSize(16);
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		sscr0 |= (SSCR0_EDSS | SSCR0_DataSize(8));
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		sscr0 |= (SSCR0_EDSS | SSCR0_DataSize(16));
		break;
	}
	pxa_ssp_write_reg(ssp, SSCR0, sscr0);

	switch (priv->dai_fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
	       sspsp = pxa_ssp_read_reg(ssp, SSPSP);

		if ((pxa_ssp_get_scr(ssp) == 4) && (width == 16)) {
			/* This is a special case where the bitclk is 64fs
			* and we're not dealing with 2*32 bits of audio
			* samples.
			*
			* The SSP values used for that are all found out by
			* trying and failing a lot; some of the registers
			* needed for that mode are only available on PXA3xx.
			*/
			if (ssp->type != PXA3xx_SSP)
				return -EINVAL;

			sspsp |= SSPSP_SFRMWDTH(width * 2);
			sspsp |= SSPSP_SFRMDLY(width * 4);
			sspsp |= SSPSP_EDMYSTOP(3);
			sspsp |= SSPSP_DMYSTOP(3);
			sspsp |= SSPSP_DMYSTRT(1);
		} else {
			/* The frame width is the width the LRCLK is
			 * asserted for; the delay is expressed in
			 * half cycle units.  We need the extra cycle
			 * because the data starts clocking out one BCLK
			 * after LRCLK changes polarity.
			 */
			sspsp |= SSPSP_SFRMWDTH(width + 1);
			if (ssp->type == PXA910_SSP)
				sspsp |= SSPSP_FSRT;
			else {
				sspsp |= SSPSP_SFRMDLY((width + 1) * 2);
				sspsp |= SSPSP_DMYSTRT(1);
			}
		}

		pxa_ssp_write_reg(ssp, SSPSP, sspsp);
		break;
	default:
		break;
	}

	if (ssp->port_id == 5) {
		/*
		 * FIXME: set clk rate to 0 incase CP modify gssp
		 * clk cause can't set clk rate to right value.
		 */
		ret = clk_set_rate(ssp->clk, 0);
		rate = params_rate(params);
		/* Final "* 32" required by SSP hardware */
		rate *= 32;
		ret = clk_set_rate(ssp->clk, rate);
		if (ret) {
			dev_err(&ssp->pdev->dev,
					"Can't set I2S clock rate: %d\n", ret);
			return ret;
		}
	}

	/* When we use a network mode, we always require TDM slots
	 * - complain loudly and fail if they've not been set up yet.
	 */
	if ((sscr0 & SSCR0_MOD) && !ttsa) {
		dev_err(&ssp->pdev->dev, "No TDM timeslot configured\n");
		return -EINVAL;
	}

	dump_registers(ssp);

	return 0;
}

static void pxa_ssp_set_running_bit(struct snd_pcm_substream *substream,
				    struct ssp_device *ssp, int value)
{
	uint32_t sscr1 = pxa_ssp_read_reg(ssp, SSCR1);

	/*
	 * here we only enable/disable SSP TX/RX DMA request. ssp enable/
	 * disable is handled in startup/shutdown to avoid noise.
	 */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		if (value)
			sscr1 |= SSCR1_TSRE;
		else
			sscr1 &= ~SSCR1_TSRE;
	} else {
		if (value)
			sscr1 |= SSCR1_RSRE;
		else
			sscr1 &= ~SSCR1_RSRE;
	}

	pxa_ssp_write_reg(ssp, SSCR1, sscr1);
}

static int pxa_ssp_trigger(struct snd_pcm_substream *substream, int cmd,
			   struct snd_soc_dai *cpu_dai)
{
	int ret = 0;
	struct ssp_priv *priv = snd_soc_dai_get_drvdata(cpu_dai);
	struct ssp_device *ssp = priv->ssp;
	int val;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_RESUME:
		pxa_ssp_enable(ssp);
		break;
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		pxa_ssp_set_running_bit(substream, ssp, 1);
		val = pxa_ssp_read_reg(ssp, SSSR);
		pxa_ssp_write_reg(ssp, SSSR, val);
		break;
	case SNDRV_PCM_TRIGGER_START:
		priv->usr_cnt++;
		pxa_ssp_set_running_bit(substream, ssp, 1);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		priv->usr_cnt--;
		pxa_ssp_set_running_bit(substream, ssp, 0);
		break;
	case SNDRV_PCM_TRIGGER_SUSPEND:
		pxa_ssp_disable(ssp);
		break;
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		pxa_ssp_set_running_bit(substream, ssp, 0);
		break;

	default:
		ret = -EINVAL;
	}

	dump_registers(ssp);

	return ret;
}

static int pxa_ssp_probe(struct snd_soc_dai *dai)
{
	struct device *dev = dai->dev;
	struct ssp_priv *priv;
	struct mmp_audio_sspdata *pdata;
	int ret;

	priv = kzalloc(sizeof(struct ssp_priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	if (dev->of_node) {
		struct device_node *ssp_handle;

		ssp_handle = of_parse_phandle(dev->of_node, "port", 0);
		if (!ssp_handle) {
			dev_err(dev, "unable to get 'port' phandle\n");
			ret = -ENODEV;
			goto err_priv;
		}

		priv->ssp = pxa_ssp_request_of(ssp_handle, "SoC audio");
		if (priv->ssp == NULL) {
			ret = -ENODEV;
			goto err_priv;
		}

		of_property_read_u32(dev->of_node, "burst_size",
				     &priv->burst_size);
	} else {
		priv->ssp = pxa_ssp_request(dai->id + 1, "SoC audio");
		if (priv->ssp == NULL) {
			ret = -ENODEV;
			goto err_priv;
		}

		pdata = dev_get_platdata(dai->dev);

		priv->burst_size = pdata->burst_size;
	}
	/*
	 * FixMe: for port 5 (gssp), it is shared by ap
	 * and cp. When AP want to handle it, AP need to
	 * configure APB to connect gssp. Also reset gssp
	 * clk to clear the potential impact from cp
	 */
	if (priv->ssp->port_id == 5) {
		priv->apbcp_base = devm_ioremap(dev, APBCONTROL_BASE,
						APBCONTROL_SIZE);
		if (priv->apbcp_base == NULL) {
			dev_err(dev, "failed to ioremap() registers\n");
			ret = -ENODEV;
			goto err_priv;
		}
	}

	priv->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(priv->pinctrl)) {
		ret = PTR_ERR(priv->pinctrl);
		goto err_priv;
	}
	priv->pin_ssp = pinctrl_lookup_state(priv->pinctrl, "ssp");
	if (IS_ERR(priv->pin_ssp)) {
		dev_err(dev, "could not get ssp pinstate\n");
		ret = IS_ERR(priv->pin_ssp);
		goto err_priv;
	}

	priv->pin_gpio = pinctrl_lookup_state(priv->pinctrl, "default");
	if (IS_ERR(priv->pin_gpio)) {
		dev_err(dev, "could not get default(gpio) pinstate\n");
		ret = IS_ERR(priv->pin_gpio);
		goto err_priv;
	}

	priv->dai_fmt = (unsigned int) -1;
	snd_soc_dai_set_drvdata(dai, priv);
	priv->usr_cnt = 0;
	priv->mfp_init = false;

	/* clear gssp init clock status */
	if (priv->ssp->port_id == 5) {
		clk_prepare_enable(priv->ssp->clk);
		clk_disable_unprepare(priv->ssp->clk);
	}
	return 0;

err_priv:
	kfree(priv);
	return ret;
}

static int pxa_ssp_remove(struct snd_soc_dai *dai)
{
	struct ssp_priv *priv = snd_soc_dai_get_drvdata(dai);

	pxa_ssp_free(priv->ssp);
	kfree(priv);
	return 0;
}

#define PXA_SSP_RATES (SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_11025 |\
			  SNDRV_PCM_RATE_16000 | SNDRV_PCM_RATE_22050 |	\
			  SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 |	\
			  SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_64000 |	\
			  SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000)

#define PXA_SSP_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S32_LE)

static const struct snd_soc_dai_ops pxa_ssp_dai_ops = {
	.startup	= pxa_ssp_startup,
	.shutdown	= pxa_ssp_shutdown,
	.trigger	= pxa_ssp_trigger,
	.hw_params	= pxa_ssp_hw_params,
	.set_sysclk	= pxa_ssp_set_dai_sysclk,
	.set_clkdiv	= pxa_ssp_set_dai_clkdiv,
	.set_pll	= pxa_ssp_set_dai_pll,
	.set_fmt	= pxa_ssp_set_dai_fmt,
	.set_tdm_slot	= pxa_ssp_set_dai_tdm_slot,
	.set_tristate	= pxa_ssp_set_dai_tristate,
};

static struct snd_soc_dai_driver pxa_ssp_dai = {
		.probe = pxa_ssp_probe,
		.remove = pxa_ssp_remove,
		.suspend = pxa_ssp_suspend,
		.resume = pxa_ssp_resume,
		.playback = {
			.channels_min = 1,
			.channels_max = 8,
			.rates = PXA_SSP_RATES,
			.formats = PXA_SSP_FORMATS,
		},
		.capture = {
			 .channels_min = 1,
			 .channels_max = 8,
			.rates = PXA_SSP_RATES,
			.formats = PXA_SSP_FORMATS,
		 },
		.ops = &pxa_ssp_dai_ops,
};

static const struct snd_soc_component_driver pxa_ssp_component = {
	.name		= "pxa-ssp",
};

#ifdef CONFIG_OF
static const struct of_device_id pxa_ssp_of_ids[] = {
	{ .compatible = "mrvl,pxa-ssp-dai" },
};
#endif

static int asoc_ssp_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	char const *platform_driver_name;
	int ret;

	if (of_property_read_string(np,
				"platform_driver_name",
				&platform_driver_name)) {
		dev_err(&pdev->dev,
			"Missing platform_driver_name property in the DT\n");
		return -EINVAL;
	}

	ret = snd_soc_register_component(&pdev->dev, &pxa_ssp_component,
					  &pxa_ssp_dai, 1);
	if (ret != 0) {
		dev_err(&pdev->dev, "Failed to register DAI\n");
		return ret;
	}

	if (strcmp(platform_driver_name, "tdma_platform") == 0) {
		ret = mmp_pcm_platform_register(&pdev->dev);
		/* add ssp_mfp sysfs entries */
		ret = device_create_file(&pdev->dev, &dev_attr_ssp_mfp);
		if (ret < 0)
			dev_err(&pdev->dev,
				"%s: failed to add ssp_mfp sysfs files: %d\n",
				__func__, ret);
	} else if (strcmp(platform_driver_name, "pdma_platform") == 0) {
		ret = pxa_pcm_platform_register(&pdev->dev);
		/* add gssp_mfp sysfs entries */
		ret = device_create_file(&pdev->dev, &dev_attr_gssp_mfp);
		if (ret < 0)
			dev_err(&pdev->dev,
				"%s: failed to add gssp_mfp sysfs files: %d\n",
				__func__, ret);
	}

	return ret;
}

static int asoc_ssp_remove(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	char const *platform_driver_name;

	if (of_property_read_string(np,
				"platform_driver_name",
				&platform_driver_name)) {
		dev_err(&pdev->dev,
			"Missing platform_driver_name property in the DT\n");
		return -EINVAL;
	}

	if (strcmp(platform_driver_name, "tdma_platform") == 0) {
		device_remove_file(&pdev->dev, &dev_attr_ssp_mfp);
		mmp_pcm_platform_unregister(&pdev->dev);
	} else if (strcmp(platform_driver_name, "pdma_platform") == 0) {
		device_remove_file(&pdev->dev, &dev_attr_gssp_mfp);
		snd_soc_unregister_component(&pdev->dev);
	}

	return 0;
}

static struct platform_driver asoc_ssp_driver = {
	.driver = {
		.name = "pxa-ssp-dai",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(pxa_ssp_of_ids),
	},

	.probe = asoc_ssp_probe,
	.remove = asoc_ssp_remove,
};

module_platform_driver(asoc_ssp_driver);

/* Module information */
MODULE_AUTHOR("Mark Brown <broonie@opensource.wolfsonmicro.com>");
MODULE_DESCRIPTION("PXA SSP/PCM SoC Interface");
MODULE_LICENSE("GPL");

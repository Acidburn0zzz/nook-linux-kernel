/* sound/soc/s3c24xx/s3c-i2c-v2.c
 *
 * ALSA Soc Audio Layer - I2S core for newer Samsung SoCs.
 *
 * Copyright (c) 2006 Wolfson Microelectronics PLC.
 *	Graeme Gregory graeme.gregory@wolfsonmicro.com
 *	linux@wolfsonmicro.com
 *
 * Copyright (c) 2008, 2007, 2004-2005 Simtec Electronics
 *	http://armlinux.simtec.co.uk/
 *	Ben Dooks <ben@simtec.co.uk>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/kernel.h>
#include <linux/io.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/initval.h>
#include <sound/soc.h>

#include <plat/regs-s3c2412-iis.h>

#include <mach/audio.h>
#include <mach/dma.h>

#include "s3c-i2s-v2.h"

#ifdef CONFIG_SND_DEBUG
#define s3cdbg(x...) printk(x)
#else
#define s3cdbg(x...) //
#endif

#define S3C2412_I2S_DEBUG_CON 1

static inline struct s3c_i2sv2_info *to_info(struct snd_soc_dai *cpu_dai)
{
	return cpu_dai->private_data;
}

#define bit_set(v, b) (((v) & (b)) ? 1 : 0)

#if S3C2412_I2S_DEBUG_CON
static void dbg_showcon(const char *fn, u32 con)
{
	s3cdbg(KERN_DEBUG "%s: LRI=%d, TXFEMPT=%d, RXFEMPT=%d, TXFFULL=%d, RXFFULL=%d\n", fn,
	       bit_set(con, S3C2412_IISCON_LRINDEX),
	       bit_set(con, S3C2412_IISCON_TXFIFO_EMPTY),
	       bit_set(con, S3C2412_IISCON_RXFIFO_EMPTY),
	       bit_set(con, S3C2412_IISCON_TXFIFO_FULL),
	       bit_set(con, S3C2412_IISCON_RXFIFO_FULL));

	s3cdbg(KERN_DEBUG "%s: PAUSE: TXDMA=%d, RXDMA=%d, TXCH=%d, RXCH=%d\n",
	       fn,
	       bit_set(con, S3C2412_IISCON_TXDMA_PAUSE),
	       bit_set(con, S3C2412_IISCON_RXDMA_PAUSE),
	       bit_set(con, S3C2412_IISCON_TXCH_PAUSE),
	       bit_set(con, S3C2412_IISCON_RXCH_PAUSE));
	s3cdbg(KERN_DEBUG "%s: ACTIVE: TXDMA=%d, RXDMA=%d, IIS=%d\n", fn,
	       bit_set(con, S3C2412_IISCON_TXDMA_ACTIVE),
	       bit_set(con, S3C2412_IISCON_RXDMA_ACTIVE),
	       bit_set(con, S3C2412_IISCON_IIS_ACTIVE));
}
#else
static inline void dbg_showcon(const char *fn, u32 con)
{
}
#endif


/* Turn on or off the transmission path. */
void s3c2412_snd_txctrl(struct s3c_i2sv2_info *i2s, int on)
{
	void __iomem *regs = i2s->regs;
	u32 fic, con, mod;



	fic = readl(regs + S3C2412_IISFIC);
	con = readl(regs + S3C2412_IISCON);
	mod = readl(regs + S3C2412_IISMOD);

	if (on) {
		con |= S3C2412_IISCON_TXDMA_ACTIVE | S3C2412_IISCON_IIS_ACTIVE;
		con &= ~S3C2412_IISCON_TXDMA_PAUSE;
		con &= ~S3C2412_IISCON_TXCH_PAUSE;

		switch (mod & S3C2412_IISMOD_MODE_MASK) {
		case S3C2412_IISMOD_MODE_TXONLY:
		case S3C2412_IISMOD_MODE_TXRX:
			/* do nothing, we are in the right mode */
			break;

		case S3C2412_IISMOD_MODE_RXONLY:
			mod &= ~S3C2412_IISMOD_MODE_MASK;
			mod |= S3C2412_IISMOD_MODE_TXRX;
			break;

		default:
			dev_err(i2s->dev, "TXEN: Invalid MODE in IISMOD\n");
		}

		writel(con, regs + S3C2412_IISCON);
		writel(mod, regs + S3C2412_IISMOD);
	s3cdbg(KERN_NOTICE "%s: IIS: CON=%x MOD=%x FIC=%x\n", __func__, con, mod, fic);
	} else {
		/* Note, we do not have any indication that the FIFO problems
		 * tha the S3C2410/2440 had apply here, so we should be able
		 * to disable the DMA and TX without resetting the FIFOS.
		 */

		con |=  S3C2412_IISCON_TXDMA_PAUSE;
		con |=  S3C2412_IISCON_TXCH_PAUSE;
		con &= ~S3C2412_IISCON_TXDMA_ACTIVE;

		switch (mod & S3C2412_IISMOD_MODE_MASK) {
		case S3C2412_IISMOD_MODE_TXRX:
			mod &= ~S3C2412_IISMOD_MODE_MASK;
			mod |= S3C2412_IISMOD_MODE_RXONLY;
			break;

		case S3C2412_IISMOD_MODE_TXONLY:
			mod &= ~S3C2412_IISMOD_MODE_MASK;
			con &= ~S3C2412_IISCON_IIS_ACTIVE;
			break;

		default:
			dev_err(i2s->dev, "TXDIS: Invalid MODE in IISMOD\n");
		}

		writel(mod, regs + S3C2412_IISMOD);
		writel(con, regs + S3C2412_IISCON);
	}

	fic = readl(regs + S3C2412_IISFIC);
	dbg_showcon(__func__, con);
	s3cdbg(KERN_NOTICE "%s: IIS: CON=%x MOD=%x FIC=%x\n", __func__, con, mod, fic);
}
EXPORT_SYMBOL_GPL(s3c2412_snd_txctrl);

void s3c2412_snd_rxctrl(struct s3c_i2sv2_info *i2s, int on)
{
	void __iomem *regs = i2s->regs;
	u32 fic, con, mod;

	s3cdbg(KERN_NOTICE "%s(%d)\n", __func__, on);

	fic = readl(regs + S3C2412_IISFIC);
	con = readl(regs + S3C2412_IISCON);
	mod = readl(regs + S3C2412_IISMOD);

	s3cdbg(KERN_NOTICE "%s: IIS: CON=%x MOD=%x FIC=%x\n", __func__, con, mod, fic);

	if (on) {
		con |= S3C2412_IISCON_RXDMA_ACTIVE | S3C2412_IISCON_IIS_ACTIVE;
		con &= ~S3C2412_IISCON_RXDMA_PAUSE;
		con &= ~S3C2412_IISCON_RXCH_PAUSE;

		switch (mod & S3C2412_IISMOD_MODE_MASK) {
		case S3C2412_IISMOD_MODE_TXRX:
		case S3C2412_IISMOD_MODE_RXONLY:
			/* do nothing, we are in the right mode */
			break;

		case S3C2412_IISMOD_MODE_TXONLY:
			mod &= ~S3C2412_IISMOD_MODE_MASK;
			mod |= S3C2412_IISMOD_MODE_TXRX;
			break;

		default:
			dev_err(i2s->dev, "RXEN: Invalid MODE in IISMOD\n");
		}

		writel(mod, regs + S3C2412_IISMOD);
		writel(con, regs + S3C2412_IISCON);
	} else {
		/* See txctrl notes on FIFOs. */

		con &= ~S3C2412_IISCON_RXDMA_ACTIVE;
		con |=  S3C2412_IISCON_RXDMA_PAUSE;
		con |=  S3C2412_IISCON_RXCH_PAUSE;

		switch (mod & S3C2412_IISMOD_MODE_MASK) {
		case S3C2412_IISMOD_MODE_RXONLY:
			con &= ~S3C2412_IISCON_IIS_ACTIVE;
			mod &= ~S3C2412_IISMOD_MODE_MASK;
			break;

		case S3C2412_IISMOD_MODE_TXRX:
			mod &= ~S3C2412_IISMOD_MODE_MASK;
			mod |= S3C2412_IISMOD_MODE_TXONLY;
			break;

		default:
			dev_err(i2s->dev, "RXEN: Invalid MODE in IISMOD\n");
		}

		writel(con, regs + S3C2412_IISCON);
		writel(mod, regs + S3C2412_IISMOD);
	}

	fic = readl(regs + S3C2412_IISFIC);
	s3cdbg(KERN_NOTICE "%s: IIS: CON=%x MOD=%x FIC=%x\n", __func__, con, mod, fic);
}
EXPORT_SYMBOL_GPL(s3c2412_snd_rxctrl);

/*
 * Wait for the LR signal to allow synchronisation to the L/R clock
 * from the codec. May only be needed for slave mode.
 */
static int s3c2412_snd_lrsync(struct s3c_i2sv2_info *i2s)
{
	u32 iiscon;
	unsigned long timeout = jiffies + msecs_to_jiffies(5);

	s3cdbg(KERN_NOTICE "Entered %s\n", __func__);

	while (1) {
		iiscon = readl(i2s->regs + S3C2412_IISCON);
		if (iiscon & S3C2412_IISCON_LRINDEX)
			break;

		if (timeout < jiffies) {
			printk(KERN_ERR "%s: timeout\n", __func__);
			return -ETIMEDOUT;
		}
	}

	return 0;
}

/*
 * Set S3C2412 I2S DAI format
 */
static int s3c2412_i2s_set_fmt(struct snd_soc_dai *cpu_dai,
			       unsigned int fmt)
{
	struct s3c_i2sv2_info *i2s = to_info(cpu_dai);
	u32 iismod;

	s3cdbg(KERN_NOTICE "Entered %s, cpu_dai(%s), fmt(%d)\n", __func__, cpu_dai->name, fmt);

	iismod = readl(i2s->regs + S3C2412_IISMOD);
	s3cdbg(KERN_NOTICE "hw_params r: IISMOD: %x \n", iismod);

#if defined(CONFIG_CPU_S3C2412) || defined(CONFIG_CPU_S3C2413)
#define IISMOD_MASTER_MASK S3C2412_IISMOD_MASTER_MASK
#define IISMOD_SLAVE S3C2412_IISMOD_SLAVE
#define IISMOD_MASTER S3C2412_IISMOD_MASTER_INTERNAL
#endif

#if defined(CONFIG_PLAT_S3C64XX)
/* From Rev1.1 datasheet, we have two master and two slave modes:
 * IMS[11:10]:
 *	00 = master mode, fed from PCLK
 *	01 = master mode, fed from CLKAUDIO
 *	10 = slave mode, using PCLK
 *	11 = slave mode, using I2SCLK
 */
#define IISMOD_MASTER_MASK (0x3<<10)
#define IISMOD_SLAVE (1 << 11)
#define IISMOD_MASTER (0x0)
#define IISMOD_MASTER_PCLK (0x0<<10)
#define IISMOD_MASTER_CLKAUDIO (0x1<<10)
#endif

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
		i2s->master = 0;
		iismod &= ~IISMOD_MASTER_MASK;
		iismod |= IISMOD_SLAVE;
		break;
	case SND_SOC_DAIFMT_CBS_CFS:
		i2s->master = 1;
		iismod &= ~IISMOD_MASTER_MASK;
		iismod |= IISMOD_MASTER_CLKAUDIO;  //By default we use FOUTepll as AUDIO CLOCK
		break;
	default:
		printk(KERN_NOTICE "unknwon master/slave format\n");
		return -EINVAL;
	}

	iismod &= ~S3C2412_IISMOD_SDF_MASK;

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_RIGHT_J:
		iismod |= S3C2412_IISMOD_SDF_MSB;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		iismod |= S3C2412_IISMOD_SDF_LSB;
		break;
	case SND_SOC_DAIFMT_I2S:
		iismod |= S3C2412_IISMOD_SDF_IIS;
		break;
	default:
		printk(KERN_NOTICE "Unknown data format\n");
		return -EINVAL;
	}

	writel(iismod, i2s->regs + S3C2412_IISMOD);
	s3cdbg(KERN_NOTICE "s3c2412_i2s_set_fmt: IISMOD(0x%x)\n", readl(i2s->regs + S3C2412_IISMOD));
	return 0;
}

static int s3c2412_i2s_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params,
				 struct snd_soc_dai *socdai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai_link *dai = rtd->dai;
	struct s3c_i2sv2_info *i2s = to_info(dai->cpu_dai);
	u32 iismod;

	s3cdbg(KERN_NOTICE "Entered %s\n", __func__);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		dai->cpu_dai->dma_data = i2s->dma_playback;
	else
		dai->cpu_dai->dma_data = i2s->dma_capture;

	/* Working copies of register */
	iismod = readl(i2s->regs + S3C2412_IISMOD);
	s3cdbg(KERN_NOTICE "%s: r: IISMOD: %x\n", __func__, iismod);

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S8:
		iismod |= S3C2412_IISMOD_8BIT;
		break;
	case SNDRV_PCM_FORMAT_S16_LE:
		iismod &= ~S3C2412_IISMOD_8BIT;
		break;
	}

	writel(iismod, i2s->regs + S3C2412_IISMOD);
	s3cdbg(KERN_NOTICE "%s: w: IISMOD: %x\n", __func__, iismod);
	return 0;
}

static int s3c2412_i2s_trigger(struct snd_pcm_substream *substream, int cmd,
			       struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct s3c_i2sv2_info *i2s = to_info(rtd->dai->cpu_dai);
	int capture = (substream->stream == SNDRV_PCM_STREAM_CAPTURE);
	unsigned long irqs;
	int ret = 0;

	s3cdbg(KERN_NOTICE "Entered %s\n", __func__);
	s3cdbg(KERN_NOTICE "dai_link(%s),cpu_dai(%s),C_P(%d), master(%d)\n", rtd->dai->name, rtd->dai->cpu_dai->name, substream->stream, i2s->master);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		/* On start, ensure that the FIFOs are cleared and reset. */

		writel(capture ? S3C2412_IISFIC_RXFLUSH : S3C2412_IISFIC_TXFLUSH,
		       i2s->regs + S3C2412_IISFIC);

		/* clear again, just in case */
		writel(0x0, i2s->regs + S3C2412_IISFIC);

	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (!i2s->master) {
			ret = s3c2412_snd_lrsync(i2s);
			if (ret)
				goto exit_err;
		}

		local_irq_save(irqs);

		if (capture)
			s3c2412_snd_rxctrl(i2s, 1);
		else
			s3c2412_snd_txctrl(i2s, 1);

		local_irq_restore(irqs);
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		local_irq_save(irqs);

		if (capture)
			s3c2412_snd_rxctrl(i2s, 0);
		else
			s3c2412_snd_txctrl(i2s, 0);

		local_irq_restore(irqs);
		break;
	default:
		ret = -EINVAL;
		break;
	}

exit_err:
	return ret;
}

/*
 * Set S3C2412 Clock dividers
 */
static int s3c2412_i2s_set_clkdiv(struct snd_soc_dai *cpu_dai,
				  int div_id, int div)
{
	struct s3c_i2sv2_info *i2s = to_info(cpu_dai);
	u32 reg;

	s3cdbg(KERN_NOTICE "%s(%p, %d, %d)\n", __func__, cpu_dai, div_id, div);

	switch (div_id) {
	case S3C_I2SV2_DIV_BCLK:
		reg = readl(i2s->regs + S3C2412_IISMOD);
		reg &= ~S3C2412_IISMOD_BCLK_MASK;
		writel(reg | div, i2s->regs + S3C2412_IISMOD);

		s3cdbg(KERN_NOTICE "%s: MOD=%08x\n", __func__, readl(i2s->regs + S3C2412_IISMOD));
		break;

	case S3C_I2SV2_DIV_RCLK:
		if (div > 3) {
			/* convert value to bit field */

			switch (div) {
			case 256:
				div = S3C2412_IISMOD_RCLK_256FS;
				break;

			case 384:
				div = S3C2412_IISMOD_RCLK_384FS;
				break;

			case 512:
				div = S3C2412_IISMOD_RCLK_512FS;
				break;

			case 768:
				div = S3C2412_IISMOD_RCLK_768FS;
				break;

			default:
				return -EINVAL;
			}
		}

		reg = readl(i2s->regs + S3C2412_IISMOD);
		reg &= ~S3C2412_IISMOD_RCLK_MASK;
		writel(reg | div, i2s->regs + S3C2412_IISMOD);
		s3cdbg(KERN_NOTICE "%s: MOD=%08x\n", __func__, readl(i2s->regs + S3C2412_IISMOD));
		break;

	case S3C_I2SV2_DIV_PRESCALER:
		if (div >= 0) {
			writel((div << 8) | S3C2412_IISPSR_PSREN,
			       i2s->regs + S3C2412_IISPSR);
		} else {
			writel(0x0, i2s->regs + S3C2412_IISPSR);
		}
		s3cdbg(KERN_NOTICE "s3c2412_i2s_set_clkdiv:PSR=%08x\n", (unsigned int)(readl(i2s->regs + S3C2412_IISPSR)));
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

/* default table of all avaialable root fs divisors */
static unsigned int iis_fs_tab[] = { 256, 512, 384, 768 };

int s3c_i2sv2_iis_calc_rate(struct s3c_i2sv2_rate_calc *info,
			    unsigned int *fstab,
			    unsigned int rate, struct clk *clk)
{
	unsigned long clkrate = clk_get_rate(clk);
	unsigned int div;
	unsigned int fsclk;
	unsigned int actual;
	unsigned int fs;
	unsigned int fsdiv;
	signed int deviation = 0;
	unsigned int best_fs = 0;
	unsigned int best_div = 0;
	unsigned int best_rate = 0;
	unsigned int best_deviation = INT_MAX;

	if (fstab == NULL)
		fstab = iis_fs_tab;

	for (fs = 0; fs < ARRAY_SIZE(iis_fs_tab); fs++) {
		fsdiv = iis_fs_tab[fs];

		fsclk = clkrate / fsdiv;
		div = fsclk / rate;

		if ((fsclk % rate) > (rate / 2))
			div++;

		if (div <= 1)
			continue;

		actual = clkrate / (fsdiv * div);
		deviation = actual - rate;

		s3cdbg(KERN_DEBUG "%dfs: div %d => result %d, deviation %d\n",
		       fsdiv, div, actual, deviation);

		deviation = abs(deviation);

		if (deviation < best_deviation) {
			best_fs = fsdiv;
			best_div = div;
			best_rate = actual;
			best_deviation = deviation;
		}

		if (deviation == 0)
			break;
	}

	s3cdbg(KERN_DEBUG "best: fs=%d, div=%d, rate=%d\n",
	       best_fs, best_div, best_rate);

	info->fs_div = best_fs;
	info->clk_div = best_div;

	return 0;
}
EXPORT_SYMBOL_GPL(s3c_i2sv2_iis_calc_rate);

int s3c_i2sv2_probe(struct platform_device *pdev,
		    struct snd_soc_dai *dai,
		    struct s3c_i2sv2_info *i2s,
		    unsigned long base)
{
	struct device *dev = &pdev->dev;

	i2s->dev = dev;

	/* record our i2s structure for later use in the callbacks */
	dai->private_data = i2s;

	i2s->regs = ioremap(base, 0x100);
	if (i2s->regs == NULL) {
		dev_err(dev, "cannot ioremap registers\n");
		return -ENXIO;
	}

	i2s->iis_pclk = clk_get(dev, "iis");
	if (i2s->iis_pclk == NULL) {
		dev_err(dev, "failed to get iis_clock\n");
		iounmap(i2s->regs);
		return -ENOENT;
	}

	clk_enable(i2s->iis_pclk);

	s3c2412_snd_txctrl(i2s, 0);
	s3c2412_snd_rxctrl(i2s, 0);

	return 0;
}

EXPORT_SYMBOL_GPL(s3c_i2sv2_probe);

#ifdef CONFIG_PM
static int s3c2412_i2s_suspend(struct snd_soc_dai *dai)
{
	struct s3c_i2sv2_info *i2s = to_info(dai);
	u32 iismod;

	if (dai->active) {
		i2s->suspend_iismod = readl(i2s->regs + S3C2412_IISMOD);
		i2s->suspend_iiscon = readl(i2s->regs + S3C2412_IISCON);
		i2s->suspend_iispsr = readl(i2s->regs + S3C2412_IISPSR);

		/* some basic suspend checks */

		iismod = readl(i2s->regs + S3C2412_IISMOD);

		if (iismod & S3C2412_IISCON_RXDMA_ACTIVE)
			pr_warning("%s: RXDMA active?\n", __func__);

		if (iismod & S3C2412_IISCON_TXDMA_ACTIVE)
			pr_warning("%s: TXDMA active?\n", __func__);

		if (iismod & S3C2412_IISCON_IIS_ACTIVE)
			pr_warning("%s: IIS active\n", __func__);
	}

	return 0;
}

static int s3c2412_i2s_resume(struct snd_soc_dai *dai)
{
	struct s3c_i2sv2_info *i2s = to_info(dai);

	pr_info("dai_active %d, IISMOD %08x, IISCON %08x\n",
		dai->active, i2s->suspend_iismod, i2s->suspend_iiscon);

	if (dai->active) {
		writel(i2s->suspend_iiscon, i2s->regs + S3C2412_IISCON);
		writel(i2s->suspend_iismod, i2s->regs + S3C2412_IISMOD);
		writel(i2s->suspend_iispsr, i2s->regs + S3C2412_IISPSR);

		writel(S3C2412_IISFIC_RXFLUSH | S3C2412_IISFIC_TXFLUSH,
		       i2s->regs + S3C2412_IISFIC);

		ndelay(250);
		writel(0x0, i2s->regs + S3C2412_IISFIC);
	}

	return 0;
}
#else
#define s3c2412_i2s_suspend NULL
#define s3c2412_i2s_resume  NULL
#endif

int s3c_i2sv2_register_dai(struct snd_soc_dai *dai)
{
	struct snd_soc_dai_ops *ops = &(dai->ops);

	ops->trigger = s3c2412_i2s_trigger;
	ops->hw_params = s3c2412_i2s_hw_params;
	ops->set_fmt = s3c2412_i2s_set_fmt;
	ops->set_clkdiv = s3c2412_i2s_set_clkdiv;

	dai->suspend = s3c2412_i2s_suspend;
	dai->resume = s3c2412_i2s_resume;

	return snd_soc_register_dai(dai);
}
EXPORT_SYMBOL_GPL(s3c_i2sv2_register_dai);

MODULE_LICENSE("GPL");

/* drivers/media/video/s3c_camera_driver.c
 *
 * Copyright (c) 2008 Samsung Electronics
 *
 * Samsung S3C Camera driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/signal.h>
#include <linux/ioport.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/kmod.h>
#include <linux/vmalloc.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/mm.h>
#include <linux/videodev2.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/semaphore.h>
#include <linux/gpio.h>

#include <asm/io.h>
#include <asm/page.h>

#include <mach/hardware.h>

#include <plat/regs-gpio.h>
#include <plat/regs-camif.h>
#include <plat/s3c64xx-dvfs.h>
#include <plat/power-clock-domain.h>
#include <plat/pm.h>

#include <media/v4l2-dev.h>
#include <media/v4l2-ioctl.h>

#include "s3c_camif.h"
#include "videodev2_s3c.h"

struct clk *cam_clock;
struct clk *cam_hclk;
EXPORT_SYMBOL(cam_clock);
EXPORT_SYMBOL(cam_hclk);

static camif_cis_t default_msdma_input = {
	itu_fmt:       	CAMIF_ITU601,
	order422:      	CAMIF_CBYCRY,	/* another case: YCRYCB */
	camclk:        	44000000,		/* for 20 fps: 44MHz, for 12 fps(more stable): 26MHz */
	source_x:      	800,
	source_y:      	600,
	win_hor_ofst:  	0,
	win_ver_ofst:  	0,
	win_hor_ofst2: 	0,
	win_ver_ofst2: 	0,
	polarity_pclk: 	0,
	polarity_vsync:	1,
	polarity_href: 	0,
	reset_type:		CAMIF_EX_RESET_AL,
	reset_udelay: 	5000,
};

static struct v4l2_input v4l2_msdma_input = {
	.index		= 0,
	.name		= "Memory Input (MSDMA)",
	.type		= V4L2_INPUT_TYPE_MSDMA,
	.audioset	= 0,
	.tuner		= 0,
	.std		= V4L2_STD_PAL_BG | V4L2_STD_NTSC_M,
	.status		= 0,
};

#define MAX_NUMBER_OF_INPUTS	5

static struct v4l2_input *fimc_inputs[MAX_NUMBER_OF_INPUTS] = {
	&v4l2_msdma_input,	/* MSDMA Input */
	NULL, NULL, NULL, NULL	/* Camera Inputs */
};

static struct v4l2_input_handler v412_msdma_input_handler = {
	NULL, NULL
};

static struct v4l2_input_handler *fimc_input_handlers[MAX_NUMBER_OF_INPUTS] = {
	&v412_msdma_input_handler,	/* MSDMA Input Handler */
	NULL, NULL, NULL, NULL	/* Camera Input Handlers */
};

camif_cis_t *msdma_input = &default_msdma_input;

static struct v4l2_output fimc_outputs[] = {
	{
		.index		= 0,
		.name		= "Pingpong Memory Output",
		.type		= 0,
		.audioset	= 0,
		.modulator	= 0,
		.std		= 0,
	},
	{
		.index		= 1,
		.name		= "LCD FIFO Output",
		.type		= 0,
		.audioset	= 0,
		.modulator	= 0,
		.std		= 0,
	}
};

#define NUMBER_OF_OUTPUTS	ARRAY_SIZE(fimc_outputs)

static int NUMBER_OF_INPUTS = 0;

CAM_PP property;

extern int s3c_camif_do_postprocess(camif_cfg_t *cfg);

/*************************************************************************
 * Utility part
 ************************************************************************/
#if defined(FSM_ON_PREVIEW)
static int s3c_camif_check_global_status(camif_cfg_t *cfg)
{
	int ret = 0;

        if (down_interruptible((struct semaphore *) &cfg->cis->lock))
		return -ERESTARTSYS;

	if (cfg->cis->status & CWANT2START) {
		cfg->cis->status &= ~CWANT2START;
		cfg->auto_restart = 1;
		ret = 1;
	} else {
	        ret = 0; 		/* There is no codec */
		cfg->auto_restart = 0; 	/* Duplicated ..Dummy */
	}

	up((struct semaphore *) &cfg->cis->lock);

        return ret;
}
#endif

static int s3c_camif_convert_format(int pixfmt, int *fmtptr)
{
	int fmt = CAMIF_YCBCR420;
	int depth = 12;

	switch (pixfmt) {
	case V4L2_PIX_FMT_RGB565:
	case V4L2_PIX_FMT_RGB565X:
		fmt = CAMIF_RGB16;
		depth = 16;
		break;

	case V4L2_PIX_FMT_BGR24: /* Not tested */
	case V4L2_PIX_FMT_RGB24:
		fmt = CAMIF_RGB24;
		depth = 24;
		break;

	case V4L2_PIX_FMT_BGR32:
	case V4L2_PIX_FMT_RGB32:
		fmt = CAMIF_RGB24;
		depth = 32;
		break;

	case V4L2_PIX_FMT_GREY:	/* Not tested  */
		fmt = CAMIF_YCBCR420;
		depth = 8;
		break;

	case V4L2_PIX_FMT_YUYV:
	case V4L2_PIX_FMT_UYVY:
		fmt = CAMIF_YCBCR422I;
		depth = 16;
		break;

	case V4L2_PIX_FMT_YUV422P:
		fmt = CAMIF_YCBCR422;
		depth = 16;
		break;

	case V4L2_PIX_FMT_YUV420:
		fmt = CAMIF_YCBCR420;
		depth = 12;
		break;
	}

	if (fmtptr)
		*fmtptr = fmt;

	return depth;
}

static int s3c_camif_set_fb_info(camif_cfg_t *cfg, int depth, int fourcc)
{
	/* To define v4l2_format used currently */
	cfg->v2.frmbuf.fmt.width = cfg->target_x;
	cfg->v2.frmbuf.fmt.height = cfg->target_y;
	cfg->v2.frmbuf.fmt.field = V4L2_FIELD_NONE;
	cfg->v2.frmbuf.fmt.pixelformat = fourcc;
	cfg->v2.frmbuf.fmt.bytesperline = cfg->v2.frmbuf.fmt.width * depth >> 3;
	cfg->v2.frmbuf.fmt.sizeimage = cfg->v2.frmbuf.fmt.height * cfg->v2.frmbuf.fmt.bytesperline;

	return 0;
}

static int s3c_camif_convert_type(camif_cfg_t *cfg, int f)
{
	int pixfmt;
	cfg->target_x = cfg->v2.frmbuf.fmt.width;
	cfg->target_y = cfg->v2.frmbuf.fmt.height;

	s3c_camif_convert_format(cfg->v2.frmbuf.fmt.pixelformat, &pixfmt);

	cfg->dst_fmt = pixfmt;

	return 0;
}



static int s3c_camif_get_sensor_format(unsigned int    in_width, unsigned int    in_height)
{
	int          sensor_type = SENSOR_XGA;

	#if defined(CONFIG_VIDEO_SAMSUNG_S5K4CA)
	{
		if(2048 <= in_width || 1536 <= in_height)
			sensor_type = SENSOR_QXGA;
		else
			sensor_type = SENSOR_XGA;
	}	
	#elif defined(CONFIG_VIDEO_SAMSUNG_S5K3AA)
	{
		if(1280 <= in_width || 1024 <= in_height)
			sensor_type = SENSOR_SXGA;
		else
			sensor_type = SENSOR_VGA;
	}
	#elif defined(CONFIG_VIDEO_SAMSUNG_S5K3BA)
	{
		if(1600 <= in_width || 1024 <= in_height)
			sensor_type = SENSOR_UXGA;
		else
			sensor_type = SENSOR_SVGA;
	}
	#elif defined(CONFIG_VIDEO_SAMSUNG_S5K4BA)
	{
		/*
		if(800 <= in_width || 600 <= in_height)
			sensor_type = SENSOR_SVGA;
		else
			sensor_type = SENSOR_SVGA;
		*/
		sensor_type = SENSOR_SVGA;
	}
	#endif

	return sensor_type;
}

/*************************************************************************
 * Control part
 ************************************************************************/
static int s3c_camif_start_capture(camif_cfg_t * cfg)
{
	int ret = 0;

	cfg->capture_enable = CAMIF_DMA_ON;

	s3c_camif_start_dma(cfg);

	cfg->status = CAMIF_STARTED;

	if (!(cfg->fsm == CAMIF_SET_LAST_INT || cfg->fsm == CAMIF_CONTINUOUS_INT)) {
		cfg->fsm = CAMIF_DUMMY_INT;
		cfg->perf.frames = 0;
	}

#if defined(CONFIG_CPU_S3C6400) || defined(CONFIG_CPU_S3C6410)
	if (cfg->input_channel == MSDMA_FROM_CODEC)
		s3c_camif_start_codec_msdma(cfg);
#endif
	return ret;
}

ssize_t s3c_camif_start_preview(camif_cfg_t *cfg)
{
	cfg->capture_enable = CAMIF_DMA_ON;

	s3c_camif_start_dma(cfg);

	cfg->status = CAMIF_STARTED;
	cfg->fsm = CAMIF_1st_INT;
	cfg->perf.frames = 0;

	return 0;
}

ssize_t s3c_camif_stop_preview(camif_cfg_t *cfg)
{
	cfg->capture_enable = CAMIF_DMA_OFF;
	cfg->status = CAMIF_STOPPED;

	s3c_camif_stop_dma(cfg);

	cfg->perf.frames = 0;

	return 0;
}

ssize_t s3c_camif_stop_capture(camif_cfg_t *cfg)
{
	cfg->capture_enable = CAMIF_DMA_OFF;
	cfg->status = CAMIF_STOPPED;

	s3c_camif_stop_dma(cfg);

	cfg->perf.frames = 0;

	return 0;
}

ssize_t s3c_camif_stop_fimc(camif_cfg_t *cfg)
{
	cfg->capture_enable = CAMIF_BOTH_DMA_OFF;
	cfg->fsm = CAMIF_DUMMY_INT;
	cfg->perf.frames = 0;

	s3c_camif_stop_dma(cfg);

	return 0;
}

#if defined(FSM_ON_PREVIEW)
static void s3c_camif_start_preview_with_codec(camif_cfg_t *cfg)
{
	camif_cfg_t *other = (camif_cfg_t *)cfg->other;

	/* Preview Stop */
	cfg->capture_enable = CAMIF_DMA_OFF;
	s3c_camif_stop_dma(cfg);

	/* Start Preview and CODEC */
	cfg->capture_enable =CAMIF_BOTH_DMA_ON;

	s3c_camif_start_dma(cfg);
	cfg->fsm = CAMIF_1st_INT; /* For Preview */

	if (!other)
		panic("Unexpected error: other is null\n");

	switch (other->pp_num) {
	case 4:
		other->fsm = CAMIF_1st_INT; /* For CODEC */
		break;

	case 1:
		other->fsm = CAMIF_Yth_INT;
		break;

	default:
		panic("Invalid pingpong number");
		break;
	}
}

static void s3c_camif_auto_restart(camif_cfg_t *cfg)
{
	if (cfg->auto_restart)
		s3c_camif_start_preview_with_codec(cfg);
}
#endif

static void s3c_camif_change_mode(camif_cfg_t *cfg, int mode)
{
	camif_cis_t *cis = cfg->cis;
	int res = SENSOR_DEFAULT;

	if (mode == SENSOR_MAX)
	{
#if defined(CONFIG_VIDEO_SAMSUNG_S5K4CA)
		res = SENSOR_QXGA;
#elif defined(CONFIG_VIDEO_SAMSUNG_S5K3AA)
		res = SENSOR_SXGA;
#elif defined(CONFIG_VIDEO_SAMSUNG_S5K3BA)
		res = SENSOR_UXGA;
#elif defined(CONFIG_VIDEO_SAMSUNG_S5K4BA)
		res = SENSOR_SVGA;
#endif
	}
	else if (mode == SENSOR_DEFAULT)
	{
#if defined(CONFIG_VIDEO_SAMSUNG_S5K4CA)
		res = SENSOR_XGA;
#elif defined(CONFIG_VIDEO_SAMSUNG_S5K4BA)
		res = SENSOR_SVGA;
#else
		res = SENSOR_SVGA;
#endif
	}
	else
		res = mode;

	s3c_camif_stop_fimc(cfg);

	switch (res) 
	{
	case SENSOR_QSXGA:
		printk("Resolution changed into QSXGA (2592x1944) mode\n");
		cis->sensor->driver->command(cis->sensor, SENSOR_QSXGA, NULL);
		cis->source_x = 2592;
		cis->source_y = 1944;
		break;

	case SENSOR_QXGA:
		printk("Resolution changed into QXGA (2048x1536) mode\n");
		cis->sensor->driver->command(cis->sensor, SENSOR_QXGA, NULL);
		cis->source_x = 2048;
		cis->source_y = 1536;
		break;

	case SENSOR_UXGA:
		printk("Resolution changed into UXGA (1600x1200) mode\n");
		cis->sensor->driver->command(cis->sensor, SENSOR_UXGA, NULL);
		cis->source_x = 1600;
		cis->source_y = 1200;
		break;

	case SENSOR_SXGA:
		printk("Resolution changed into SXGA (1280x1024) mode\n");
		cis->sensor->driver->command(cis->sensor, SENSOR_SXGA, NULL);
		cis->source_x = 1280;
		cis->source_y = 1024;
		break;

	case SENSOR_XGA:
		printk("Resolution changed into XGA (1024x768) mode\n");
		cis->sensor->driver->command(cis->sensor, SENSOR_XGA, NULL);
		cis->source_x = 1024;
		cis->source_y = 768;
		break;

	case SENSOR_SVGA:
			printk("Resolution changed into SVGA (800x600) mode\n");
		cis->sensor->driver->command(cis->sensor, SENSOR_SVGA, NULL);
		cis->source_x = 800;
		cis->source_y = 600;
		break;

	case SENSOR_VGA:
			printk("Resolution changed into VGA (640x480) mode\n");
		cis->sensor->driver->command(cis->sensor, SENSOR_VGA, NULL);
		cis->source_x = 640;
		cis->source_y = 480;
		break;
	}

	cis->win_hor_ofst = cis->win_hor_ofst2 = 0;
	cis->win_ver_ofst = cis->win_ver_ofst2 = 0;

	s3c_camif_set_source_format(cis);
}

static int s3c_camif_check_zoom_range(camif_cfg_t *cfg, int type)
{
	switch (type) {
	case V4L2_CID_ZOOMIN:
		if (((cfg->sc.modified_src_x - (cfg->cis->win_hor_ofst + \
			ZOOM_AT_A_TIME_IN_PIXELS + cfg->cis->win_hor_ofst2 + \
			ZOOM_AT_A_TIME_IN_PIXELS)) / cfg->sc.prehratio) > ZOOM_IN_MAX) {
	                printk(KERN_INFO "Invalid Zoom-in: this zoom-in on preview scaler already comes to the maximum\n");
			return 0;
		}

		cfg->sc.zoom_in_cnt++;
		break;

	case V4L2_CID_ZOOMOUT:
		if (cfg->sc.zoom_in_cnt > 0) {
			cfg->sc.zoom_in_cnt--;
			break;
		} else {
	                printk(KERN_INFO "Invalid Zoom-out: this zoom-out on preview scaler already comes to the minimum\n");
			return 0;
		}

		break;

	default:
		break;
	}

	return 1;
}


static int s3c_camif_restart(camif_cfg_t *cfg)
{
	int ret = 0;
	unsigned int current_status = cfg->status;
	
	if(cfg->status != CAMIF_STOPPED)
	{
		if (cfg->dma_type & CAMIF_PREVIEW) 
			s3c_camif_stop_preview(cfg);
		else if (cfg->dma_type & CAMIF_CODEC)
			s3c_camif_stop_capture(cfg);
	}

	if (s3c_camif_control_fimc(cfg)) {
		printk(KERN_ERR "S3C fimc control failed\n");
		ret = -1;
	}

	if(current_status != CAMIF_STOPPED)
	{
		if (cfg->dma_type & CAMIF_PREVIEW) 
			s3c_camif_start_preview(cfg);
		else if (cfg->dma_type & CAMIF_CODEC)
			s3c_camif_stop_capture(cfg);	
	}

	return ret;
	
}
/*
{
	int ret = 0;

	s3c_camif_stop_preview(cfg);

	if (s3c_camif_control_fimc(cfg)) {
		printk(KERN_ERR "S3C fimc control failed\n");
		ret = -1;
	}

	s3c_camif_start_preview(cfg);

	return ret;
}
*/

static int s3c_camif_send_sensor_command(camif_cfg_t *cfg, unsigned int cmd, void *arg)
{
	cfg->cis->sensor->driver->command(cfg->cis->sensor, cmd, arg);

	return 0;
}

/*************************************************************************
 * V4L2 part
 ************************************************************************/
static int s3c_camif_v4l2_querycap(camif_cfg_t *cfg, unsigned long arg)
{
	struct v4l2_capability *cap = (struct v4l2_capability *) arg;

	strcpy(cap->driver, "S3C FIMC Camera driver");
	strlcpy(cap->card, cfg->v->name, sizeof(cap->card));
	sprintf(cap->bus_info, "FIMC AHB Bus");

	cap->version = 0;
	cap->capabilities = (V4L2_CAP_VIDEO_OVERLAY | V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING);

	return 0;
}

static int s3c_camif_v4l2_g_fbuf(camif_cfg_t *cfg, unsigned long arg)
{
	struct v4l2_framebuffer *fb = (struct v4l2_framebuffer *) arg;

	*fb = cfg->v2.frmbuf;

	fb->base = cfg->v2.frmbuf.base;
	fb->capability = V4L2_FBUF_CAP_LIST_CLIPPING;

	fb->fmt.pixelformat  = cfg->v2.frmbuf.fmt.pixelformat;
	fb->fmt.width = cfg->v2.frmbuf.fmt.width;
	fb->fmt.height = cfg->v2.frmbuf.fmt.height;
	fb->fmt.bytesperline = cfg->v2.frmbuf.fmt.bytesperline;

	return 0;
}

static int s3c_camif_v4l2_s_fbuf(camif_cfg_t *cfg, unsigned long arg)
{
	struct v4l2_framebuffer *fb = (struct v4l2_framebuffer *) arg;
	int i, depth;

	for (i = 0; i < NUMBER_OF_PREVIEW_FORMATS; i++)
		if (fimc_preview_formats[i].pixelformat == fb->fmt.pixelformat)
			break;

	if (i == NUMBER_OF_PREVIEW_FORMATS)
		return -EINVAL;

	cfg->v2.frmbuf.base  = fb->base;
	cfg->v2.frmbuf.flags = fb->flags;
	cfg->v2.frmbuf.capability = fb->capability;

	cfg->target_x = fb->fmt.width;
	cfg->target_y = fb->fmt.height;

	depth = s3c_camif_convert_format(fb->fmt.pixelformat, (int *) &(cfg->dst_fmt));
	s3c_camif_set_fb_info(cfg, depth, fb->fmt.pixelformat);

	return s3c_camif_control_fimc(cfg);
}

static int s3c_camif_v4l2_g_fmt(camif_cfg_t *cfg, unsigned long arg)
{
	struct v4l2_format *f = (struct v4l2_format *) arg;
	int size = sizeof(struct v4l2_pix_format);
	int ret = -1;

	switch (f->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		memset(&f->fmt.pix, 0, size);
		memcpy(&f->fmt.pix, &cfg->v2.frmbuf.fmt, size);
		ret = 0;
		break;

	default:
		break;
	}

	return ret;
}

static int s3c_camif_v4l2_s_fmt(camif_cfg_t *cfg, unsigned long arg)
{
	struct v4l2_format *f = (struct v4l2_format *) arg;
	int ret = -1;

	int sensor_type          = SENSOR_DEFAULT;

	switch (f->type)
	{
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		cfg->v2.frmbuf.fmt   = f->fmt.pix;
		cfg->v2.status       |= CAMIF_v4L2_DIRTY;
		cfg->v2.status      &= ~CAMIF_v4L2_DIRTY; /* dummy ? */

		sensor_type = s3c_camif_get_sensor_format(f->fmt.pix.width, f->fmt.pix.height);
		
		// set source
		// kcoolsw
		//s3c_camif_change_mode(cfg, sensor_type);
		//s3c_camif_change_mode(cfg, SENSOR_MAX);
		s3c_camif_change_mode(cfg, SENSOR_SVGA);

		s3c_camif_convert_type(cfg, 1);

		s3c_camif_control_fimc(cfg);

		ret = 0;
		break;

	default:
		break;
	}

	return ret;
}

static int s3c_camif_v4l2_enum_fmt(camif_cfg_t *cfg, unsigned long arg)
{
	struct v4l2_fmtdesc *f = (struct v4l2_fmtdesc *) arg;
	int index = f->index;

	if (index >= NUMBER_OF_CODEC_FORMATS)
		return -EINVAL;

	switch (f->type)
	{
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		break;

	case V4L2_BUF_TYPE_VIDEO_OVERLAY:
	default:
		return -EINVAL;
	}

	memset(f, 0, sizeof(*f));
	memcpy(f, cfg->v2.fmtdesc + index, sizeof(*f));

	return 0;
}

static int s3c_camif_v4l2_overlay(camif_cfg_t *cfg, unsigned long arg)
{
	int on = *((int *) arg);
	int ret;

	if (on != 0)
		ret = s3c_camif_start_preview(cfg);
	else
		ret = s3c_camif_stop_preview(cfg);

	return ret;
}

static int s3c_camif_v4l2_s_roangle(camif_cfg_t *cfg, unsigned long arg)
{
	struct v4l2_control *ctrl = (struct v4l2_control *)arg;

	switch (ctrl->value) {
		case 0:
		default :
			cfg->flip = CAMIF_FLIP;
			break;
		case 1:
			cfg->flip = CAMIF_ROTATE_90;
			break;

		case 2:
			cfg->flip = CAMIF_FLIP_MIRROR;
			break;

		case 3:
			cfg->flip = CAMIF_FLIP_ROTATE_270;
			break;
	}
	s3c_camif_change_flip(cfg);
	return 0;
}

static int s3c_camif_init_campp(void)
{
	property.auto_focus = 0;
	property.bright = 0;
	property.dig_zoom = 0;
	property.width = 0;
	property.height = 0;
	property.opt_zoom = 0;
	property.rotate_angle = 0;
	property.white_balance = 0;

	return 0;
}

static int s3c_camif_v4l2_g_campp(camif_cfg_t *cfg, unsigned long arg)
{
	CAM_PP * pproperty = (CAM_PP *)arg;

	printk("\n %d, %d, %d, %d, %d, %d, %d, %d  \n",
		property.width, property.height, property.opt_zoom,	property.dig_zoom,
		property.white_balance, property.bright, property.auto_focus, property.rotate_angle);
	pproperty->width = property.width = cfg-> cis->source_x - (cfg->cis->win_hor_ofst+ cfg->cis->win_hor_ofst2);
	pproperty->height = property.height = cfg-> cis->source_y -(cfg->cis->win_ver_ofst+ cfg->cis->win_ver_ofst2);
	pproperty->opt_zoom = property.opt_zoom = 0;
	pproperty->dig_zoom = property.dig_zoom = cfg->sc.zoom_in_cnt;
	pproperty->white_balance = property.white_balance;
	pproperty->bright = property.bright;
	pproperty->auto_focus = property.auto_focus;
	pproperty->rotate_angle = property.rotate_angle;

	return 0;
}

static int s3c_camif_v4l2_g_ctrl(camif_cfg_t *cfg, unsigned long arg)
{
	return 0;
}

static int s3c_camif_v4l2_s_ctrl(camif_cfg_t *cfg, unsigned long arg)
{
	struct v4l2_control *ctrl = (struct v4l2_control *) arg;

	switch (ctrl->id) {
		case V4L2_CID_ORIGINAL:
		case V4L2_CID_ARBITRARY:
		case V4L2_CID_NEGATIVE:
		case V4L2_CID_EMBOSSING:
		case V4L2_CID_ART_FREEZE:
		case V4L2_CID_SILHOUETTE:
			cfg->effect = ctrl->value;
			s3c_camif_change_effect(cfg);
			break;

		case V4L2_CID_HFLIP:
			cfg->flip = CAMIF_FLIP_X;
			s3c_camif_change_flip(cfg);
			break;

		case V4L2_CID_VFLIP:
			cfg->flip = CAMIF_FLIP_Y;
			s3c_camif_change_flip(cfg);
			break;

		case V4L2_CID_ROTATE_180:
			cfg->flip = CAMIF_FLIP_MIRROR;
			s3c_camif_change_flip(cfg);
			property.rotate_angle = 2;
			break;

#if defined(CONFIG_CPU_S3C6400) || defined(CONFIG_CPU_S3C6410)
		case V4L2_CID_ROTATE_90:
			cfg->flip = CAMIF_ROTATE_90;
			s3c_camif_change_flip(cfg);
			property.rotate_angle = 1;
			break;

		case V4L2_CID_ROTATE_270:
			cfg->flip = CAMIF_FLIP_ROTATE_270;
			s3c_camif_change_flip(cfg);
			property.rotate_angle = 3;
			break;
#endif

		case V4L2_CID_ROTATE_BYPASS:
			cfg->flip = CAMIF_FLIP;
			s3c_camif_change_flip(cfg);
			property.rotate_angle = 0;
			break;

		case V4L2_CID_ZOOMIN:
			if (s3c_camif_check_zoom_range(cfg, ctrl->id)) {
				cfg->cis->win_hor_ofst += ZOOM_AT_A_TIME_IN_PIXELS;
				cfg->cis->win_ver_ofst += ZOOM_AT_A_TIME_IN_PIXELS;
				cfg->cis->win_hor_ofst2 += ZOOM_AT_A_TIME_IN_PIXELS;
				cfg->cis->win_ver_ofst2 += ZOOM_AT_A_TIME_IN_PIXELS;

				s3c_camif_restart(cfg);
			}

			break;

		case V4L2_CID_ZOOMOUT:
			if (s3c_camif_check_zoom_range(cfg, ctrl->id)) {
				cfg->cis->win_hor_ofst -= ZOOM_AT_A_TIME_IN_PIXELS;
				cfg->cis->win_ver_ofst -= ZOOM_AT_A_TIME_IN_PIXELS;
				cfg->cis->win_hor_ofst2 -= ZOOM_AT_A_TIME_IN_PIXELS;
				cfg->cis->win_ver_ofst2 -= ZOOM_AT_A_TIME_IN_PIXELS;

				s3c_camif_restart(cfg);
			}

			break;

		case V4L2_CID_CONTRAST:
		case V4L2_CID_AUTO_WHITE_BALANCE:
			s3c_camif_send_sensor_command(cfg, SENSOR_WB, ctrl);
			property.white_balance = ctrl->value;
			break;

		case V4L2_CID_BRIGHTNESS:
			s3c_camif_send_sensor_command(cfg, SENSOR_BRIGHTNESS, ctrl);
			property.bright = ctrl->value;
			break;

		case V4L2_CID_FLASH_CAMERA:
			s3c_camif_send_sensor_command(cfg, SENSOR_FLASH_CAMERA, ctrl);
			break;

		case V4L2_CID_FLASH_MOVIE:
			s3c_camif_send_sensor_command(cfg, SENSOR_FLASH_MOVIE, ctrl);
			break;
	
		case SENSOR_EFFECT:
			s3c_camif_send_sensor_command(cfg, SENSOR_EFFECT, ctrl);
			break;

		case V4L2_CID_MODE_SET:
				s3c_camif_send_sensor_command(cfg, SENSOR_MODE_SET, ctrl);
			break;

		default:
			printk(KERN_ERR "Invalid control id: %d\n", ctrl->id);
			return -1;
	}

	return 0;
}

static int s3c_camif_v4l2_streamon(camif_cfg_t *cfg, unsigned long arg)
{
	int ret = 0;

	ret = s3c_camif_start_capture(cfg);

	return ret;
}

static int s3c_camif_v4l2_streamoff(camif_cfg_t *cfg, unsigned long arg)
{
	int ret = 0;

	cfg->cis->status &= ~C_WORKING;
	s3c_camif_stop_capture(cfg);

	return ret;
}

static int s3c_camif_v4l2_g_input(camif_cfg_t *cfg, unsigned long arg)
{
	*((int *) arg) = cfg->v2.input->index;

	return 0;
}

static int s3c_camif_v4l2_s_input(camif_cfg_t *cfg, unsigned int index)
{
	unsigned int old_index;

	if (index >= NUMBER_OF_INPUTS)
		return -EINVAL;
	else
	{
		/* Old input index */
		old_index = cfg->v2.input->index;
		/* New input index */
		cfg->v2.input = fimc_inputs[index];

		if (cfg->v2.input->type == V4L2_INPUT_TYPE_MSDMA)
		{
			if (cfg->dma_type & CAMIF_PREVIEW)
				cfg->input_channel = MSDMA_FROM_PREVIEW;
			else if (cfg->dma_type & CAMIF_CODEC)
				cfg->input_channel = MSDMA_FROM_CODEC;
			}
		else
			cfg->input_channel = CAMERA_INPUT;

		if (fimc_input_handlers[old_index]->exit_input_handler)
			fimc_input_handlers[old_index]->exit_input_handler();

		cfg->cis->user--;

		if (fimc_input_handlers[index]->init_input_handler)
		{
			if (fimc_input_handlers[index]->init_input_handler())
			{
				printk(KERN_ERR "Index %d -> Init Failed!\n", index);
				return -EIO;
			}
		}

		cfg->cis->user++;

		s3c_camif_init_sensor(cfg);

		s3c_camif_init_campp();
	}

	return 0;
}

static int s3c_camif_v4l2_g_output(camif_cfg_t *cfg, unsigned long arg)
{
	unsigned int index = *((int *) arg);

	index = cfg->v2.output->index;

	return 0;
}

static int s3c_camif_v4l2_s_output(camif_cfg_t *cfg, unsigned int index)
{
	if (index >= NUMBER_OF_OUTPUTS)
		return -EINVAL;
	else {
		cfg->v2.output = (struct v4l2_output *) &fimc_outputs[index];
		return 0;
	}
}

static int s3c_camif_v4l2_enum_input(camif_cfg_t *cfg, unsigned long arg)
{
	struct v4l2_input *i = (struct v4l2_input *) arg;

	if (i->index >= NUMBER_OF_INPUTS)
		return -EINVAL;

	memcpy(i, fimc_inputs[i->index], sizeof(struct v4l2_input));

	return 0;
}

static int s3c_camif_v4l2_enum_output(camif_cfg_t *cfg, unsigned long arg)
{
	struct v4l2_output *i = (struct v4l2_output *) arg;

	if ((i->index) >= NUMBER_OF_OUTPUTS)
		return -EINVAL;

	memcpy(i, &fimc_outputs[i->index], sizeof(struct v4l2_output));

	return 0;
}

static int s3c_camif_v4l2_reqbufs(camif_cfg_t *cfg, unsigned long arg)
{
	struct v4l2_requestbuffers *req = (struct v4l2_requestbuffers *) arg;

	if (req->memory != V4L2_MEMORY_USERPTR && req->memory != V4L2_MEMORY_MMAP)
	{
		printk(KERN_ERR "Only V4L2_MEMORY_USERPTR or V4L2_MEMORY_MMAPcapture is supported\n");
		return -EINVAL;
	}

	if(MAX_PPNUM < req->count)
{
		printk(KERN_ERR "Invalid pingpong number %d\n", req->count);
		return -EINVAL;
	}

	cfg->pp_num = req->count;

	return 0;
}
/*
{
	struct v4l2_requestbuffers *req = (struct v4l2_requestbuffers *) arg;

	if (req->memory != V4L2_MEMORY_MMAP) {
		printk(KERN_ERR "Only V4L2_MEMORY_MMAP capture is supported\n");
		return -EINVAL;
	}

	// control user input
	if (req->count > 2)
		req->count = 4;
	else if (req->count > 1)
		req->count = 2;
	else
		req->count = 1;

	return 0;
}
*/


static int s3c_camif_v4l2_querybuf(camif_cfg_t *cfg, unsigned long  arg)
{
	struct v4l2_buffer *buf = (struct v4l2_buffer *) arg;

	if (buf->type != V4L2_BUF_TYPE_VIDEO_CAPTURE && buf->memory != V4L2_MEMORY_MMAP)
		return -1;

	buf->length = cfg->buffer_size;
	buf->m.offset = buf->length * buf->index;

	return 0;
}

static int s3c_camif_v4l2_qbuf(camif_cfg_t *cfg, unsigned long arg)
{
	struct v4l2_buffer *buf = (struct v4l2_buffer *) arg;

	if(cfg->pp_num <= buf->index)
	{
		printk("cfg->pp_num(%d) bigger than buf->index(%d)\n", cfg->pp_num, buf->index);
		return -EINVAL;
	}

	int             index     = buf->index;
	unsigned int    buf_size  = buf->length;
	unsigned int    phys_addr = buf->m.offset;
	unsigned char * virt_addr = ioremap_nocache(phys_addr, buf_size);
	unsigned char * old_virt_addr = NULL;
	
	// set by user.
	cfg->flag_img_buf_user_set = 1;

	if (cfg->dma_type & CAMIF_PREVIEW)
	{
		old_virt_addr = cfg->img_buf[index].virt_rgb;

		cfg->img_buf[index].phys_rgb = phys_addr;
		cfg->img_buf[index].virt_rgb = virt_addr;		
	}
	else if (cfg->dma_type & CAMIF_CODEC)
	{
		if ((cfg->dst_fmt & CAMIF_RGB16) || (cfg->dst_fmt & CAMIF_RGB24))
		{
			old_virt_addr = cfg->img_buf[index].virt_rgb;

			cfg->img_buf[index].phys_rgb = phys_addr;
			cfg->img_buf[index].virt_rgb = virt_addr;
		}
		else
		{
			old_virt_addr = cfg->img_buf[index].virt_y;

			cfg->img_buf[index].phys_y  = phys_addr;
			cfg->img_buf[index].virt_y  = virt_addr;
			
			/*
			cfg->img_buf[index].virt_cb = cfg->pp_virt_buf + area;
			cfg->img_buf[index].phys_cb = cfg->pp_phys_buf + area;
			cfg->img_buf[index].virt_cr = cfg->pp_virt_buf + area + cbcr_size;
			cfg->img_buf[index].phys_cr = cfg->pp_phys_buf + area + cbcr_size;
			*/
		}
	}
	else 
	{
		printk("invalid cfg->dma_type : %d\n", cfg->dma_type);
		return -EINVAL;
	}

	if(old_virt_addr)
		iounmap(old_virt_addr);

	return 0;
}
/*
{
	return 0;
}
*/

static int s3c_camif_v4l2_dqbuf(camif_cfg_t *cfg, unsigned long arg)
{
	struct v4l2_buffer * buf = (struct v4l2_buffer *) arg;

	buf->index      = cfg->cur_frame_num;
	buf->length     = cfg->buffer_size;
	
	if (cfg->dma_type & CAMIF_PREVIEW)
	{
		buf->m.offset = cfg->img_buf[buf->index].phys_rgb;
	}
	else if (cfg->dma_type & CAMIF_CODEC)
	{
		if ((cfg->dst_fmt & CAMIF_RGB16) || (cfg->dst_fmt & CAMIF_RGB24))
			buf->m.offset = cfg->img_buf[buf->index].phys_rgb;
		else
			buf->m.offset = cfg->img_buf[buf->index].phys_y;
	}

	return 0;
}

/*
 * S3C specific
 */
static int s3c_camif_v4l2_s_msdma(camif_cfg_t *cfg, unsigned long arg)
{
	struct v4l2_msdma_format *f = (struct v4l2_msdma_format *) arg;
	int ret = -1;

	switch(f->input_path) {
	case V4L2_MSDMA_PREVIEW:
		cfg->cis->user--;   /* CIS will be replaced with a CIS for MSDMA */
		cfg->cis = msdma_input;
		cfg->cis->user++;
		cfg->input_channel = MSDMA_FROM_PREVIEW;
		break;

	case V4L2_MSDMA_CODEC:
		cfg->cis->user--;   /* CIS will be replaced with a CIS for MSDMA */
		cfg->cis = msdma_input;
		cfg->cis->user++;
		cfg->input_channel = MSDMA_FROM_CODEC;
		break;

	default:
		cfg->input_channel = CAMERA_INPUT;
		break;
	}

	cfg->cis->source_x = f->width;
	cfg->cis->source_y = f->height;

	s3c_camif_convert_format(f->pixelformat, (int *) &cfg->src_fmt);

	cfg->cis->win_hor_ofst = 0;
	cfg->cis->win_ver_ofst = 0;
	cfg->cis->win_hor_ofst2 = 0;
	cfg->cis->win_ver_ofst2 = 0;

	ret = s3c_camif_control_fimc(cfg);

	switch(f->input_path) {
	case V4L2_MSDMA_PREVIEW:
		ret = s3c_camif_start_preview(cfg);
		break;

	case V4L2_MSDMA_CODEC:
		ret = s3c_camif_start_capture(cfg);
		break;

	default:
		break;

	}

	return ret;
}

static int s3c_camif_v4l2_msdma_start(camif_cfg_t *cfg, unsigned long arg)
{
	if (cfg->input_channel == MSDMA_FROM_PREVIEW) {
		cfg->msdma_status = 1;
		s3c_camif_start_preview_msdma(cfg);
	}

	return 0;
}

static int s3c_camif_v4l2_msdma_stop(camif_cfg_t *cfg, unsigned long arg)
{
	struct v4l2_msdma_format *f = (struct v4l2_msdma_format *) arg;
	int ret = -1;

	cfg->cis->status &= ~C_WORKING;
	cfg->msdma_status = 0;

	switch(f->input_path) {
	case V4L2_MSDMA_PREVIEW:
		ret = s3c_camif_stop_preview(cfg);
		break;

	case V4L2_MSDMA_CODEC:
		ret = s3c_camif_stop_capture(cfg);
		break;

	default:
		break;
	}

	return ret;
}

static int s3c_camif_v4l2_camera_start(camif_cfg_t *cfg, unsigned long arg)
{
	return 0;
}

static int s3c_camif_v4l2_camera_stop(camif_cfg_t *cfg, unsigned long arg)
{
	return 0;
}

static int s3c_camif_v4l2_cropcap(camif_cfg_t *cfg, unsigned long arg)
{
	struct v4l2_cropcap *cap = (struct v4l2_cropcap *) arg;

	if (cap->type != V4L2_BUF_TYPE_VIDEO_CAPTURE &&
	    cap->type != V4L2_BUF_TYPE_VIDEO_OVERLAY)
		return -EINVAL;

	/* crop limitations */
	cfg->v2.crop_bounds.left = 0;
	cfg->v2.crop_bounds.top = 0;
	cfg->v2.crop_bounds.width = cfg->cis->source_x;
	cfg->v2.crop_bounds.height = cfg->cis->source_y;

	/* crop default values */
	cfg->v2.crop_defrect.left = (cfg->cis->source_x - CROP_DEFAULT_WIDTH) / 2;
	cfg->v2.crop_defrect.top = (cfg->cis->source_y - CROP_DEFAULT_HEIGHT) / 2;
	cfg->v2.crop_defrect.width = CROP_DEFAULT_WIDTH;
	cfg->v2.crop_defrect.height = CROP_DEFAULT_HEIGHT;

	cap->bounds = cfg->v2.crop_bounds;
	cap->defrect = cfg->v2.crop_defrect;

	return 0;
}

static int s3c_camif_v4l2_g_crop(camif_cfg_t *cfg, unsigned long arg)
{
	struct v4l2_crop *crop = (struct v4l2_crop *) arg;

	if (crop->type != V4L2_BUF_TYPE_VIDEO_CAPTURE &&
	    crop->type != V4L2_BUF_TYPE_VIDEO_OVERLAY)
		return -EINVAL;

	crop->c = cfg->v2.crop_current;

	return 0;
}

static int s3c_camif_v4l2_s_crop(camif_cfg_t *cfg, unsigned long arg)
{
	struct v4l2_crop *crop = (struct v4l2_crop *) arg;

	if (crop->type != V4L2_BUF_TYPE_VIDEO_CAPTURE &&
	    crop->type != V4L2_BUF_TYPE_VIDEO_OVERLAY)
		return -EINVAL;

	if (crop->c.height < 0)
		return -EINVAL;

	if (crop->c.width < 0)
		return -EINVAL;

	if ((crop->c.left + crop->c.width > cfg->cis->source_x) || \
		(crop->c.top + crop->c.height > cfg->cis->source_y))
		return -EINVAL;

	cfg->v2.crop_current = crop->c;

	cfg->cis->win_hor_ofst = (cfg->cis->source_x - crop->c.width) / 2;
	cfg->cis->win_ver_ofst = (cfg->cis->source_y - crop->c.height) / 2;

	cfg->cis->win_hor_ofst2 = cfg->cis->win_hor_ofst;
	cfg->cis->win_ver_ofst2 = cfg->cis->win_ver_ofst;

	s3c_camif_restart(cfg);

	return 0;
}

static int s3c_camif_v4l2_s_parm(camif_cfg_t *cfg, unsigned long arg)
{
	struct v4l2_streamparm *sp = (struct v4l2_streamparm *) arg;

	if (sp->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (sp->parm.capture.capturemode == V4L2_MODE_HIGHQUALITY)
	{
		s3c_camif_change_mode(cfg, SENSOR_MAX);
		s3c_camif_control_fimc(cfg);
	}
	else
	{
		s3c_camif_change_mode(cfg, SENSOR_DEFAULT);
		s3c_camif_control_fimc(cfg);
	}

	return 0;
}

/*************************************************************************
 * Interrupt part
 ************************************************************************/
#if defined(FSM_ON_CODEC) && !defined(USE_LAST_IRQ)
int s3c_camif_do_fsm_codec(camif_cfg_t *cfg)
{
	int ret;

	cfg->perf.frames++;

	if ((cfg->fsm == CAMIF_DUMMY_INT) && (cfg->perf.frames > CAMIF_CAPTURE_SKIP_FRAMES))
		cfg->fsm = CAMIF_NORMAL_INT;

	switch (cfg->fsm) {
	case CAMIF_DUMMY_INT:
		DPRINTK(KERN_INFO "CAMIF_DUMMY_INT: %d\n", cfg->perf.frames);
		cfg->status = CAMIF_STARTED;
		cfg->fsm = CAMIF_DUMMY_INT;
		ret = INSTANT_SKIP;
		break;

	case CAMIF_NORMAL_INT:
		DPRINTK(KERN_INFO "CAMIF_NORMAL_INT: %d\n", cfg->perf.frames);
		cfg->status = CAMIF_INT_HAPPEN;
		cfg->fsm = CAMIF_CONTINUOUS_INT;
		ret = INSTANT_GO;
		break;

	case CAMIF_CONTINUOUS_INT:
		DPRINTK(KERN_INFO "CAMIF_CONTINUOS_INT: %d\n", cfg->perf.frames);
		cfg->status = CAMIF_INT_HAPPEN;
		cfg->fsm = CAMIF_CONTINUOUS_INT;
		ret = INSTANT_GO;
		break;

	default:
		printk(KERN_INFO "Unexpected INT: %d\n", cfg->fsm);
		ret = INSTANT_SKIP;
		break;
	}

	return ret;
}
#endif

#if defined(FSM_ON_CODEC) && defined(USE_LAST_IRQ)
int s3c_camif_do_fsm_codec_lastirq(camif_cfg_t *cfg)
{
	int ret;

	cfg->perf.frames++;

	if ((cfg->fsm == CAMIF_DUMMY_INT) && (cfg->perf.frames > (CAMIF_CAPTURE_SKIP_FRAMES - 2)))
		cfg->fsm = CAMIF_SET_LAST_INT;

	switch (cfg->fsm) {
	case CAMIF_DUMMY_INT:
		DPRINTK(KERN_INFO "CAMIF_DUMMY_INT: %d\n", cfg->perf.frames);
		cfg->status = CAMIF_STARTED;
		cfg->fsm = CAMIF_DUMMY_INT;
		ret = INSTANT_SKIP;
		break;

	case CAMIF_SET_LAST_INT:
		DPRINTK(KERN_INFO "CAMIF_SET_LAST_INT: %d\n", cfg->perf.frames);
		s3c_camif_enable_lastirq(cfg);

/* in 64xx, lastirq is not auto cleared. */
#if defined(CONFIG_CPU_S3C6400) || defined(CONFIG_CPU_S3C6410)
		s3c_camif_disable_lastirq(cfg);
#endif
		cfg->status = CAMIF_INT_HAPPEN;
		cfg->fsm = CAMIF_STOP_CAPTURE;
		ret = INSTANT_SKIP;
		break;

	case CAMIF_STOP_CAPTURE:
		DPRINTK(KERN_INFO "CAMIF_STOP_CAPTURE: %d\n", cfg->perf.frames);
		cfg->capture_enable = CAMIF_DMA_OFF;
		s3c_camif_stop_dma(cfg);
		cfg->fsm = CAMIF_LAST_IRQ;
		ret = INSTANT_SKIP;
		break;

	case CAMIF_LAST_IRQ:
		DPRINTK(KERN_INFO "CAMIF_LAST_IRQ: %d\n", cfg->perf.frames);
		cfg->fsm = CAMIF_SET_LAST_INT;
		cfg->status = CAMIF_INT_HAPPEN;
		ret = INSTANT_GO;
		break;

	default:
		printk(KERN_INFO "Unexpected INT: %d\n", cfg->fsm);
		ret = INSTANT_SKIP;
		break;
	}

	return ret;
}
#endif

#if defined(FSM_ON_PREVIEW)
static int s3c_camif_do_lastirq_preview(camif_cfg_t *cfg)
{
	int ret = 0;

	cfg->perf.frames++;

	if (cfg->fsm == CAMIF_NORMAL_INT) {
		if (cfg->perf.frames % CHECK_FREQ == 0)
			ret = s3c_camif_check_global_status(cfg);
	}

	if (ret > 0)
		cfg->fsm = CAMIF_Xth_INT;

	switch (cfg->fsm) {
	case CAMIF_1st_INT:
		DPRINTK(KERN_INFO "CAMIF_1st_INT INT\n");
		cfg->fsm = CAMIF_NORMAL_INT;
		ret = INSTANT_SKIP;
		break;

	case CAMIF_NORMAL_INT:
		DPRINTK(KERN_INFO "CAMIF_NORMAL_INT\n");
		cfg->status = CAMIF_INT_HAPPEN;
		cfg->fsm = CAMIF_NORMAL_INT;
		ret = INSTANT_GO;
		break;

	case CAMIF_Xth_INT:
		DPRINTK(KERN_INFO "CAMIF_Xth_INT\n");
		s3c_camif_enable_lastirq(cfg);
		cfg->status = CAMIF_INT_HAPPEN;
		cfg->fsm = CAMIF_Yth_INT;
		ret = INSTANT_GO;
		break;

	case CAMIF_Yth_INT:
		DPRINTK(KERN_INFO "CAMIF_Yth_INT\n");
		s3c_camif_disable_lastirq(cfg);
		cfg->capture_enable = CAMIF_DMA_OFF;
		cfg->status = CAMIF_INT_HAPPEN;
		s3c_camif_stop_dma(cfg);
		cfg->fsm = CAMIF_Zth_INT;
		ret = INSTANT_GO;
		break;

	case CAMIF_Zth_INT:
		DPRINTK(KERN_INFO "CAMIF_Zth_INT\n");
		cfg->fsm = CAMIF_DUMMY_INT;
		cfg->status = CAMIF_INT_HAPPEN;
		ret = INSTANT_GO;
		s3c_camif_auto_restart(cfg);
		break;

        case CAMIF_DUMMY_INT:
		DPRINTK(KERN_INFO "CAMIF_DUMMY_INT\n");
		cfg->status = CAMIF_STOPPED;
		ret = INSTANT_SKIP;
		break;

	default:
		printk(KERN_INFO "Unexpected INT %d\n", cfg->fsm);
		ret = INSTANT_SKIP;
		break;
	}

	return ret;
}
#endif

static irqreturn_t s3c_camif_do_irq_codec(int irq, void *dev_id)
{
	camif_cfg_t *cfg = (camif_cfg_t *) dev_id;

#if defined(CONFIG_CPU_S3C6400) || defined(CONFIG_CPU_S3C6410)
	gpio_set_value(S3C64XX_GPN(15), 1);
#endif
	s3c_camif_clear_irq(irq);
	s3c_camif_get_fifo_status(cfg);
	s3c_camif_get_frame_num(cfg);

#if defined(FSM_ON_CODEC) && !defined(USE_LAST_IRQ)
	if (s3c_camif_do_fsm_codec(cfg) == INSTANT_SKIP)
		return IRQ_HANDLED;
#endif

#if defined(FSM_ON_CODEC) && defined(USE_LAST_IRQ)
	if (s3c_camif_do_fsm_codec_lastirq(cfg) == INSTANT_SKIP)
		return IRQ_HANDLED;
#endif
	wake_up_interruptible(&cfg->waitq);

	return IRQ_HANDLED;
}

static irqreturn_t s3c_camif_do_irq_preview(int irq, void *dev_id)
{
	camif_cfg_t *cfg = (camif_cfg_t *) dev_id;

	s3c_camif_clear_irq(irq);
	s3c_camif_get_fifo_status(cfg);
	s3c_camif_get_frame_num(cfg);
	wake_up_interruptible(&cfg->waitq);

#if defined(FSM_ON_PREVIEW)
	if (s3c_camif_do_lastirq_preview(cfg) == INSTANT_SKIP)
		return IRQ_HANDLED;

	wake_up_interruptible(&cfg->waitq);
#endif
	cfg->status = CAMIF_INT_HAPPEN;

	return IRQ_HANDLED;
}

static void s3c_camif_release_irq(camif_cfg_t * cfg)
{
	disable_irq(cfg->irq);
	free_irq(cfg->irq, cfg);
}

static int s3c_camif_request_irq(camif_cfg_t * cfg)
{
	int ret = 0;

	if (cfg->dma_type & CAMIF_CODEC) {
		if ((ret = request_irq(cfg->irq, s3c_camif_do_irq_codec, IRQF_DISABLED, cfg->shortname, cfg)))
			printk(KERN_ERR "Request irq (CAM_C) failed\n");
		else
			printk(KERN_INFO "Request irq %d for codec\n", cfg->irq);
	}

	if (cfg->dma_type & CAMIF_PREVIEW) {
		if ((ret = request_irq(cfg->irq, s3c_camif_do_irq_preview, IRQF_DISABLED, cfg->shortname, cfg)))
			printk("Request_irq (CAM_P) failed\n");
		else
			printk(KERN_INFO "Request irq %d for preview\n", cfg->irq);
	}

	return 0;
}

/*************************************************************************
 * Standard file operations part
 ************************************************************************/
int s3c_camif_ioctl(struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg)
{
	camif_cfg_t *cfg = file->private_data;
	struct v4l2_control *ctrl;
	int ret = -1;

	switch (cmd)
	{
        case VIDIOC_QUERYCAP:
		ret = s3c_camif_v4l2_querycap(cfg, arg);
		break;

	case VIDIOC_G_FBUF:
		ret = s3c_camif_v4l2_g_fbuf(cfg, arg);
		break;

	case VIDIOC_S_FBUF:
		ret = s3c_camif_v4l2_s_fbuf(cfg, arg);
		break;

	case VIDIOC_G_FMT:
		ret = s3c_camif_v4l2_g_fmt(cfg, arg);
		break;

	case VIDIOC_S_FMT:
		ret = s3c_camif_v4l2_s_fmt(cfg, arg);
		break;

	case VIDIOC_ENUM_FMT:
		ret = s3c_camif_v4l2_enum_fmt(cfg, arg);
		break;

	case VIDIOC_OVERLAY:
		ret = s3c_camif_v4l2_overlay(cfg, arg);
		break;

	case VIDIOC_S_CTRL:
		ret = s3c_camif_v4l2_s_ctrl(cfg, arg);
		break;

	case VIDIOC_S_DIG_ZOOM:
		ctrl = (struct v4l2_control *) arg;

		if ((ctrl->value < 0) || (ctrl->value > 9))	{
			printk(KERN_INFO "Invalid Zoom-level: this zoom-in on preview scaler already comes to the maximum\n");
			return 0;
		}

		cfg->sc.zoom_in_cnt = ctrl->value;

		cfg->cis->win_hor_ofst = ctrl->value * ZOOM_AT_A_TIME_IN_PIXELS;
		cfg->cis->win_ver_ofst = ctrl->value * ZOOM_AT_A_TIME_IN_PIXELS;
		cfg->cis->win_hor_ofst2 = ctrl->value * ZOOM_AT_A_TIME_IN_PIXELS;
		cfg->cis->win_ver_ofst2 = ctrl->value * ZOOM_AT_A_TIME_IN_PIXELS;

		ret = s3c_camif_restart(cfg);
		break;

	case VIDIOC_G_CTRL:
		ret = s3c_camif_v4l2_g_ctrl(cfg, arg);
		break;

	case VIDIOC_STREAMON:
		ret = s3c_camif_v4l2_streamon(cfg, arg);
		break;

	case VIDIOC_STREAMOFF:
		ret = s3c_camif_v4l2_streamoff(cfg, arg);
		break;

	case VIDIOC_G_INPUT:
		ret = s3c_camif_v4l2_g_input(cfg, arg);
		break;

	case VIDIOC_S_INPUT:
		ret = s3c_camif_v4l2_s_input(cfg, *((int *) arg));
		break;

	case VIDIOC_G_OUTPUT:
		ret = s3c_camif_v4l2_g_output(cfg, arg);
		break;

	case VIDIOC_S_OUTPUT:
		ret = s3c_camif_v4l2_s_output(cfg, *((int *) arg));
		break;

	case VIDIOC_ENUMINPUT:
		ret = s3c_camif_v4l2_enum_input(cfg, arg);
		break;

	case VIDIOC_ENUMOUTPUT:
		ret = s3c_camif_v4l2_enum_output(cfg, arg);
		break;

	case VIDIOC_REQBUFS:
		ret = s3c_camif_v4l2_reqbufs(cfg, arg);
		break;

	case VIDIOC_QUERYBUF:
		ret = s3c_camif_v4l2_querybuf(cfg, arg);
		break;

	case VIDIOC_QBUF:
		ret = s3c_camif_v4l2_qbuf(cfg, arg);
		break;

	case VIDIOC_DQBUF:
		ret = s3c_camif_v4l2_dqbuf(cfg, arg);
		break;

	case VIDIOC_S_MSDMA:
		ret = s3c_camif_v4l2_s_msdma(cfg, arg);
		break;

	case VIDIOC_MSDMA_START:
		ret = s3c_camif_v4l2_msdma_start(cfg, arg);
		break;

	case VIDIOC_MSDMA_STOP:
		ret = s3c_camif_v4l2_msdma_stop(cfg, arg);
		break;

	case VIDIOC_S_CAMERA_START:
		ret = s3c_camif_v4l2_camera_start(cfg, arg);
		break;

	case VIDIOC_S_CAMERA_STOP:
		ret = s3c_camif_v4l2_camera_stop(cfg, arg);
		break;

	case VIDIOC_CROPCAP:
		ret = s3c_camif_v4l2_cropcap(cfg, arg);
		break;

	case VIDIOC_G_CROP:
		ret = s3c_camif_v4l2_g_crop(cfg, arg);
		break;

	case VIDIOC_S_CROP:
		ret = s3c_camif_v4l2_s_crop(cfg, arg);
		break;

	case VIDIOC_S_PARM:
		ret = s3c_camif_v4l2_s_parm(cfg, arg);
		break;

	case VIDIOC_I2C_WRITE:
		ret = cfg->cis->sensor->driver->command(cfg->cis->sensor, SENSOR_USER_WRITE, (void *) arg);
		break;

	case VIDIOC_I2C_READ:
		ret = cfg->cis->sensor->driver->command(cfg->cis->sensor, SENSOR_USER_READ, (void *) arg);
		break;

	case VIDIOC_S_SENSOR:
		ret = cfg->cis->sensor->driver->command(cfg->cis->sensor, SENSOR_MODE_SET, (void *) arg);
		break;

	case VIDIOC_G_CAMPP:
		ret = s3c_camif_v4l2_g_campp(cfg, arg);
		break;

	case VIDIOC_S_ROTATE_ANGLE:
		ctrl = (struct v4l2_control *) arg;
		property.rotate_angle = ctrl->value;
		ret = s3c_camif_v4l2_s_roangle(cfg, arg);
		break;

	case VIDIOC_S_FOCUS_AUTO:
		ctrl = (struct v4l2_control *) arg;
		property.auto_focus = ctrl->value;
		ret = cfg->cis->sensor->driver->command(cfg->cis->sensor, SENSOR_AF, (void *) arg);
		break;

	default:	/* For v4l compatability */
		ret = v4l_compat_translate_ioctl(inode, file, cmd, (void *) arg, (v4l2_kioctl)s3c_camif_ioctl);
		break;
	} /* End of Switch  */

	return ret;
}

int s3c_camif_open(struct inode *inode, struct file *file)
{
	int err;

	s3c_set_normal_cfg(S3C64XX_DOMAIN_I, S3C64XX_ACTIVE_MODE, S3C64XX_CAMERA);
	if(s3c_wait_blk_pwr_ready(S3C64XX_BLK_I)) {
		return -1;
	}
	clk_enable(cam_clock);
	clk_enable(cam_hclk);

	camif_cfg_t *cfg = s3c_camif_get_fimc_object(MINOR(inode->i_rdev));

	if (!cfg->cis) {
		printk(KERN_ERR "An object for a CIS is missing\n");
		printk(KERN_ERR "Using default_msdma_input as a CIS data structure\n");

		cfg->cis = msdma_input;

		/* global lock for both Codec and Preview */
		sema_init((struct semaphore *) &cfg->cis->lock, 1);
		cfg->cis->status |= P_NOT_WORKING;
	}

	if (cfg->dma_type & CAMIF_PREVIEW) {
			cfg->cis->status &= ~P_NOT_WORKING;
		up((struct semaphore *) &cfg->cis->lock);
	}
#ifdef CONFIG_CPU_FREQ
	set_dvfs_level(0);
#endif
	err = video_exclusive_open(inode, file);
	cfg->cis->user++;
	cfg->status = CAMIF_STOPPED;

	cfg->flag_img_buf_user_set = 0;

	if (err < 0)
		return err;

	if (file->f_flags & O_NONCAP) {
		printk(KERN_ERR "Don't support non-capturing open\n");
		return 0;
	}

	file->private_data = cfg;

	return 0;
}

int s3c_camif_release(struct inode *inode, struct file *file)
{
	camif_cfg_t *cfg = s3c_camif_get_fimc_object(MINOR(inode->i_rdev));

	if (cfg->dma_type & CAMIF_PREVIEW) {
		cfg->cis->status &= ~PWANT2START;
		cfg->cis->status |= P_NOT_WORKING;
		s3c_camif_stop_preview(cfg);
		up((struct semaphore *) &cfg->cis->lock);
	} else {
		cfg->cis->status &= ~CWANT2START;
		s3c_camif_stop_capture(cfg);
	}

	video_exclusive_release(inode, file);

#ifdef CONFIG_CPU_FREQ
	set_dvfs_level(1);
#endif

	if (cfg->cis->sensor == NULL)
		DPRINTK("A CIS sensor for MSDMA has been used\n");
	else
		cfg->cis->sensor->driver->command(cfg->cis->sensor, USER_EXIT, NULL);

	cfg->cis->user--;
	cfg->status = CAMIF_STOPPED;

	clk_disable(cam_clock);
	clk_disable(cam_hclk);
	s3c_set_normal_cfg(S3C64XX_DOMAIN_I, S3C64XX_LP_MODE, S3C64XX_CAMERA);

	return 0;
}

ssize_t s3c_camif_read(struct file * file, char *buf, size_t count, loff_t * pos)
{
	camif_cfg_t *cfg = NULL;
	size_t end;

	cfg = s3c_camif_get_fimc_object(MINOR(file->f_dentry->d_inode->i_rdev));

#if defined(FSM_ON_PREVIEW)
	if (cfg->dma_type == CAMIF_PREVIEW) {
		if (wait_event_interruptible(cfg->waitq, cfg->status == CAMIF_INT_HAPPEN))
			return -ERESTARTSYS;

		cfg->status = CAMIF_STOPPED;
	}
#endif

#if defined(FSM_ON_CODEC)
	if (cfg->dma_type == CAMIF_CODEC) {
		if (wait_event_interruptible(cfg->waitq, cfg->status == CAMIF_INT_HAPPEN))
			return -ERESTARTSYS;

		cfg->status = CAMIF_STOPPED;
	}
#endif
	end = min_t(size_t, cfg->pp_totalsize / cfg->pp_num, count);

	if (copy_to_user(buf, s3c_camif_get_frame(cfg), end))
		return -EFAULT;

	return end;
}

ssize_t s3c_camif_write(struct file * f, const char *b, size_t c, loff_t * offset)
{
	camif_cfg_t *cfg;
	int ret = 0;

	cfg = s3c_camif_get_fimc_object(MINOR(f->f_dentry->d_inode->i_rdev));

	switch (*b) {
	case 'O':
		if (cfg->dma_type & CAMIF_PREVIEW)
			s3c_camif_start_preview(cfg);
		else {
			ret = s3c_camif_start_capture(cfg);

			if (ret < 0)
				ret = 1;
		}

		break;

	case 'X':
		if (cfg->dma_type & CAMIF_PREVIEW) {
			s3c_camif_stop_preview(cfg);
			cfg->cis->status |= P_NOT_WORKING;
		} else {
			cfg->cis->status &= ~C_WORKING;
			s3c_camif_stop_capture(cfg);
		}

		break;

#if defined(CONFIG_CPU_S3C2443) || defined(CONFIG_CPU_S3C2450) || defined(CONFIG_CPU_S3C2416)
	case 'P':
		if (cfg->dma_type & CAMIF_PREVIEW) {
			s3c_camif_start_preview(cfg);
			s3c_camif_do_postprocess(cfg);
			return 0;
		} else
			return -EFAULT;
#endif
	default:
		panic("s3c_camera_driver.c: s3c_camif_write() - Unexpected Parameter\n");
	}

	return ret;
}

int s3c_camif_mmap(struct file* filp, struct vm_area_struct *vma)
{
	camif_cfg_t *cfg = filp->private_data;

	unsigned long pageFrameNo;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long total_size;

	if (cfg->dma_type == CAMIF_PREVIEW)
		total_size = RGB_MEM;
	else
		total_size = YUV_MEM;

	// page frame number of the address for a source RGB frame to be stored at.
	pageFrameNo = __phys_to_pfn(cfg->pp_phys_buf);

	if (total_size < size)
	{
		printk(KERN_ERR "The size of RGB_MEM mapping is too big\n");
		return -EINVAL;
	}

	if ((vma->vm_flags & VM_WRITE) && !(vma->vm_flags & VM_SHARED))
	{
		printk(KERN_ERR "Writable RGB_MEM mapping must be shared\n");
		return -EINVAL;
	}

	if (remap_pfn_range(vma, vma->vm_start, pageFrameNo + vma->vm_pgoff, size, vma->vm_page_prot))
		return -EINVAL;

	return 0;
}
/*
{
	camif_cfg_t *cfg = filp->private_data;

	unsigned long pageFrameNo;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long total_size;

	if (cfg->dma_type == CAMIF_PREVIEW)
		total_size = RGB_MEM;
	else
		total_size = YUV_MEM;

	// page frame number of the address for a source RGB frame to be stored at.
	pageFrameNo = __phys_to_pfn(cfg->pp_phys_buf);

	if (size > total_size) {
		printk(KERN_ERR "The size of RGB_MEM mapping is too big\n");
		return -EINVAL;
	}

	if ((vma->vm_flags & VM_WRITE) && !(vma->vm_flags & VM_SHARED)) {
		printk(KERN_ERR "Writable RGB_MEM mapping must be shared\n");
		return -EINVAL;
	}

	if (remap_pfn_range(vma, vma->vm_start, pageFrameNo + vma->vm_pgoff, size, vma->vm_page_prot))
		return -EINVAL;

	return 0;
}
*/

static unsigned int s3c_camif_poll(struct file *file, poll_table *wait)
{
	unsigned int mask = 0;
	camif_cfg_t *cfg = file->private_data;

	poll_wait(file, &cfg->waitq, wait);

	if (cfg->status == CAMIF_INT_HAPPEN)
		mask = POLLIN | POLLRDNORM;

	cfg->status = CAMIF_STOPPED;

	return mask;
}

struct file_operations camif_c_fops = {
	.owner = THIS_MODULE,
	.open = s3c_camif_open,
	.release = s3c_camif_release,
	.ioctl = s3c_camif_ioctl,
	.read = s3c_camif_read,
	.write = s3c_camif_write,
	.mmap = s3c_camif_mmap,
	.poll = s3c_camif_poll,
};

struct file_operations camif_p_fops = {
	.owner = THIS_MODULE,
	.open = s3c_camif_open,
	.release = s3c_camif_release,
	.ioctl = s3c_camif_ioctl,
	.read = s3c_camif_read,
	.write = s3c_camif_write,
	.mmap = s3c_camif_mmap,
	.poll = s3c_camif_poll,
};

/*************************************************************************
 * Templates for V4L2
 ************************************************************************/
void camif_vdev_release (struct video_device *vdev) {
	kfree(vdev);
}

struct video_device codec_template = {
	.name = CODEC_DEV_NAME,
	.fops = &camif_c_fops,
	.release  = camif_vdev_release,
	.minor = CODEC_MINOR,
};

struct video_device preview_template = {
	.name = PREVIEW_DEV_NAME,
	.fops = &camif_p_fops,
	.release  = camif_vdev_release,
	.minor = PREVIEW_MINOR,
};

/*************************************************************************
 * Initialize part
 ************************************************************************/
void s3c_camif_init_sensor(camif_cfg_t *cfg)
{
	camif_cis_t *cis = cfg->cis;
	camif_cis_t *initialized_cis;

	if (!cis->sensor) {
		initialized_cis = (camif_cis_t *)((cis->init_sensor) ? cis : NULL);
		if (initialized_cis == NULL) {
			printk(KERN_ERR "An I2C client for CIS sensor isn't registered\n");
			return;
		}

		cis = cfg->cis = initialized_cis;
		cfg->input_channel = 0;
		cfg->cis->user++;
	}

	if (!cis->init_sensor) {
		cis->sensor->driver->command(cis->sensor, SENSOR_INIT, NULL);
		cis->init_sensor = 1;

// kcoolsw
/*
#if defined(CONFIG_VIDEO_SAMSUNG_S5K3BA)
		cis->sensor->driver->command(cis->sensor, SENSOR_VGA, NULL);
	        cis->source_x = 640;
	        cis->source_y = 480;
#elif defined(CONFIG_VIDEO_SAMSUNG_S5K4BA)
		cis->sensor->driver->command(cis->sensor, SENSOR_SVGA, NULL);
	        cis->source_x = 800;
	        cis->source_y = 600;
#endif
*/
	}

	cis->sensor->driver->command(cis->sensor, USER_ADD, NULL);
}

static int s3c_camif_init_preview(camif_cfg_t * cfg)
{
	cfg->target_x = PREVIEW_DEFAULT_WIDTH;
	cfg->target_y = PREVIEW_DEFAULT_HEIGHT;
	cfg->pp_num = PREVIEW_DEFAULT_PPNUM;
	cfg->dma_type = CAMIF_PREVIEW;
	cfg->input_channel = CAMERA_INPUT;
	cfg->src_fmt = CAMIF_YCBCR422;
	cfg->output_channel = CAMIF_OUT_PP;
	cfg->dst_fmt = CAMIF_RGB16;
	cfg->flip = CAMIF_FLIP_Y;
	cfg->v = &preview_template;
	cfg->v2.input = &v4l2_msdma_input;

	init_MUTEX((struct semaphore *) &cfg->v->lock);
	init_waitqueue_head(&cfg->waitq);

	cfg->status = CAMIF_STOPPED;

	/* To get the handle of CODEC */
	cfg->other = s3c_camif_get_fimc_object(CODEC_MINOR);

	return cfg->status;
}

static int s3c_camif_init_codec(camif_cfg_t * cfg)
{
	cfg->target_x = CODEC_DEFAULT_WIDTH;
	cfg->target_y = CODEC_DEFAULT_HEIGHT;
	cfg->pp_num = CODEC_DEFAULT_PPNUM;
	cfg->dma_type = CAMIF_CODEC;
	cfg->src_fmt = CAMIF_YCBCR422;
	cfg->input_channel = CAMERA_INPUT;
	cfg->dst_fmt = CAMIF_YCBCR420;
	cfg->output_channel = CAMIF_OUT_PP;
	cfg->flip = CAMIF_FLIP_X;
	cfg->v = &codec_template;
	cfg->v2.input       = &v4l2_msdma_input;

	init_MUTEX((struct semaphore *) &cfg->v->lock);

	init_waitqueue_head(&cfg->waitq);

	cfg->status = CAMIF_STOPPED;

	/* To get the handle of PREVIEW */
	cfg->other = s3c_camif_get_fimc_object(PREVIEW_MINOR);

	return cfg->status;
}

static int s3c_camif_probe(struct platform_device *pdev)
{
	struct resource *res;
	camif_cfg_t *codec, *preview;
	int ret = 0;

	s3c_set_normal_cfg(S3C64XX_DOMAIN_I, S3C64XX_ACTIVE_MODE, S3C64XX_CAMERA);
	if(s3c_wait_blk_pwr_ready(S3C64XX_BLK_I)) {
		return -1;
	}

	/* Initialize fimc objects */
	codec = s3c_camif_get_fimc_object(CODEC_MINOR);
	preview = s3c_camif_get_fimc_object(PREVIEW_MINOR);

	memset(codec, 0, sizeof(camif_cfg_t));
	memset(preview, 0, sizeof(camif_cfg_t));

	/* Set the fimc name */
	strcpy(codec->shortname, CODEC_DEV_NAME);
	strcpy(preview->shortname, PREVIEW_DEV_NAME);

	/* get resource for io memory */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	if (!res) {
		printk("Failed to get io memory region resouce.\n");
		return -1;
	}

	/* request mem region */
	res = request_mem_region(res->start, res->end - res->start + 1, pdev->name);

	if (!res) {
		printk("Failed to request io memory region.\n");
		return -1;
	}

	/* ioremap for register block */
	codec->regs = preview->regs = ioremap(res->start, res->end - res->start + 1);

	if (codec->regs == NULL) {
		printk(KERN_ERR "Failed to remap register block\n");
		return -1;
	}

	/* ioremap for reserved memory */
	// kcoolsw
	//codec->pp_phys_buf = CAMERA_MEM_START;
	//codec->pp_virt_buf = ioremap_nocache(codec->pp_phys_buf, YUV_MEM);

	//preview->pp_phys_buf = CAMERA_MEM_START + (CAMERA_MEM_SIZE - YUV_MEM);
	//preview->pp_virt_buf = ioremap_nocache(preview->pp_phys_buf, RGB_MEM);

	/* Device init */
	s3c_camif_init();
	s3c_camif_init_codec(codec);
	s3c_camif_init_preview(preview);

	/* Set irq */
	codec->irq = platform_get_irq(pdev, FIMC_CODEC_INDEX);
	preview->irq = platform_get_irq(pdev, FIMC_PREVIEW_INDEX);

	s3c_camif_request_irq(codec);
	s3c_camif_request_irq(preview);

	/* Register to video device */
	if (video_register_device(codec->v, VFL_TYPE_GRABBER, CODEC_MINOR) != 0) {
		printk(KERN_ERR "Couldn't register this codec driver\n");
		return -1;
	}

	if (video_register_device(preview->v, VFL_TYPE_GRABBER, PREVIEW_MINOR) != 0) {
		printk(KERN_ERR "Couldn't register this preview driver\n");
		return -1;
	}

#if defined(CONFIG_CPU_S3C6400) || defined(CONFIG_CPU_S3C6410)
	cam_clock = clk_get(&pdev->dev, "sclk_cam");
	cam_hclk = clk_get(&pdev->dev, "hclk_camera");
#elif defined(CONFIG_CPU_S3C2443) || defined(CONFIG_CPU_S3C2416) || defined(CONFIG_CPU_S3C2450)
	cam_clock = clk_get(&pdev->dev, "camif-upll");
#else
#error	cam_clock should be defined
#endif

	if (IS_ERR(cam_clock)) {
		printk("Failed to find camera clock source\n");
		ret = PTR_ERR(cam_clock);
	}
	
	if (IS_ERR(cam_hclk)) {
		printk("Failed to find camera h-clock source\n");
		ret = PTR_ERR(cam_hclk);
	}

	/* Print banner */
	printk(KERN_INFO "S3C FIMC v%s\n", FIMC_VER);

	clk_disable(cam_clock);
	clk_disable(cam_hclk);

	s3c_set_normal_cfg(S3C64XX_DOMAIN_I, S3C64XX_LP_MODE, S3C64XX_CAMERA);

	NUMBER_OF_INPUTS++;

	return 0;
}

static int s3c_camif_remove(struct platform_device *pdev)
{
	camif_cfg_t *codec, *preview;

	codec = s3c_camif_get_fimc_object(CODEC_MINOR);
	preview = s3c_camif_get_fimc_object(PREVIEW_MINOR);

	s3c_camif_release_irq(codec);
	s3c_camif_release_irq(preview);

	if(codec->pp_virt_buf)
	iounmap(codec->pp_virt_buf);
	codec->pp_virt_buf = 0;

	if(preview->pp_virt_buf)
	iounmap(preview->pp_virt_buf);
	preview->pp_virt_buf = 0;

	video_unregister_device(codec->v);
	video_unregister_device(preview->v);

	s3c_camif_set_priority(0);

	memset(codec, 0, sizeof(camif_cfg_t));
	memset(preview, 0, sizeof(camif_cfg_t));

	NUMBER_OF_INPUTS--;

	return 0;
}

#if !defined(CONFIG_PM)

static struct sleep_save_phy s3c_camif_save[] = {
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CISRCFMT),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIWDOFST),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIGCTRL),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIDOWSFT2),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CICOYSA1),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CICOYSA2),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CICOYSA3),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CICOYSA4),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CICOCBSA1),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CICOCBSA2),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CICOCBSA3),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CICOCBSA4),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CICOTRGFMT),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CICOCTRL),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CICOSCPRERATIO),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CICOSCPREDST),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CICOSCCTRL),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CICOTAREA),

#if defined(CONFIG_CPU_S3C2443) || defined(CONFIG_CPU_S3C2450)
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRCLRSA1),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRCLRSA2),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRCLRSA3),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRCLRSA4),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRTRGFMT),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRCTRL),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRSCPRERATIO),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRSCPREDST),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRSCCTRL),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRTAREA),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIIMGCPT),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CICOCPTSEQ),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CICOSCOS),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIIMGEFF),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIMSYSA),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIMSCBSA),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIMSCRSA),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIMSYEND),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIMSCBEND),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIMSCREND),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIMSYOFF),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIMSCBOFF),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIMSCROFF),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIMSWIDTH),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIMSCTRL),

#elif defined(CONFIG_CPU_S3C6400) || defined(CONFIG_CPU_S3C6410)
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRYSA1),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRYSA2),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRYSA3),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRYSA4),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRCBSA1),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRCBSA2),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRCBSA3),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRCBSA4),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRCRSA1),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRCRSA2),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRCRSA3),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRCRSA4),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRTRGFMT),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRCTRL),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRSCPRERATIO),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRSCPREDST),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRSCCTRL),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRTAREA),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRSTATUS),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIIMGCPT),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CICOCPTSEQ),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIIMGEFF),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_MSCOY0SA),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_MSCOCB0SA),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_MSCOY0END),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_MSCOCB0END),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_MSCOCR0END),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_MSCOYOFF),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_MSCOCBOFF),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_MSCOCROFF),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_MSCOWIDTH),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_MSCOCTRL),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_MSPRY0SA),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_MSPRCB0SA),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_MSPRY0END),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_MSPRCB0END),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_MSPRCR0END),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_MSPRYOFF),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_MSPRCBOFF),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_MSPRCROFF),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_MSPRWIDTH),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIMSCTRL),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CICOSCOSY),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CICOSCOSCB),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CICOSCOSCR),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRSCOSY),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRSCOSCB),
	SAVE_ITEM(S3C6400_PA_CAMIF + S3C_CIPRSCOSCR),

#endif
};


/*
 *  Suspend
 */
int s3c_camif_suspend(struct platform_device *dev, pm_message_t state)
{
	s3c2410_pm_do_save_phy(s3c_camif_save, ARRAY_SIZE(s3c_camif_save));
	clk_disable(cam_clock);
	msleep(1);

	return 0;
}

/*
 *  Resume
 */
int s3c_camif_resume(struct platform_device *dev)
{
	clk_enable(cam_clock);
	msleep(1);

	s3c_camif_init();
	s3c_camif_init_sensor(s3c_camif_get_fimc_object(CODEC_MINOR));
	s3c2410_pm_do_restore_phy(s3c_camif_save, ARRAY_SIZE(s3c_camif_save));

	return 0;
}

#else

int s3c_camif_suspend(struct platform_device *dev, pm_message_t state)
{
	return 0;
}

int s3c_camif_resume(struct platform_device *dev)
{
	return 0;
}

#endif

static struct platform_driver s3c_camif_driver =
{
        .probe          = s3c_camif_probe,
        .remove         = s3c_camif_remove,
        .suspend	= s3c_camif_suspend,
        .resume		= s3c_camif_resume,
	.driver		= {
		.name	= "s3c-camif",
		.owner	= THIS_MODULE,
	},
};

#if 1
static int s3c_camif_register(void)
{
	return platform_driver_register(&s3c_camif_driver);
}

static void s3c_camif_unregister(void)
{
	platform_driver_unregister(&s3c_camif_driver);
}
#else

#ifdef CONFIG_VIDEO_SAMSUNG_S5K4CA
extern int s5k4ca_sensor_add(void);
extern void s5k4ca_sensor_remove(void);
#endif

#ifdef CONFIG_VIDEO_SAMSUNG_PO4010
extern int po4010_sensor_add(void);
extern void po4010_sensor_remove(void);
#endif

#ifdef CONFIG_VIDEO_SAMSUNG_AIT848
extern int ait848_sensor_add(void);
extern void ait848_sensor_remove(void);
#endif

static int s3c_camif_register(void)
{
	platform_driver_register(&s3c_camif_driver);

#ifdef CONFIG_VIDEO_SAMSUNG_S5K4CA
	s5k4ca_sensor_add();
#endif

#ifdef CONFIG_VIDEO_SAMSUNG_PO4010
	po4010_sensor_add();
#endif

#ifdef CONFIG_VIDEO_SAMSUNG_AIT848
	ait848_sensor_add();
#endif

	return 0;
}

static void s3c_camif_unregister(void)
{
#ifdef CONFIG_VIDEO_SAMSUNG_AIT848
	ait848_sensor_remove();
#endif

#ifdef CONFIG_VIDEO_SAMSUNG_PO4010
	po4010_sensor_remove();
#endif

#ifdef CONFIG_VIDEO_SAMSUNG_S5K4CA
	s5k4ca_sensor_remove();
#endif

	platform_driver_unregister(&s3c_camif_driver);
}
#endif

int s3c_camif_add_sensor(struct v4l2_input *input, struct v4l2_input_handler * input_handler)
{
	if ((input == NULL) ||
		(input_handler == NULL) ||
		(NUMBER_OF_INPUTS >= MAX_NUMBER_OF_INPUTS))
		return -EINVAL;

	fimc_inputs[NUMBER_OF_INPUTS] = input;

	fimc_inputs[NUMBER_OF_INPUTS]->index = NUMBER_OF_INPUTS;

	fimc_input_handlers[NUMBER_OF_INPUTS] = input_handler;

	NUMBER_OF_INPUTS++;

	return 0;
}

void s3c_camif_remove_sensor(struct v4l2_input *input, struct v4l2_input_handler * input_handler)
{
	int i;

	if ((input == NULL) ||
		(input_handler == NULL) ||
		(NUMBER_OF_INPUTS >= MAX_NUMBER_OF_INPUTS))
		return;

	for (i = 0; i < NUMBER_OF_INPUTS; i++)
		if ((fimc_inputs[i] == input) &&
			(fimc_input_handlers[i] == input_handler))
			break;

	if (i == NUMBER_OF_INPUTS)
		return;

	fimc_inputs[i]->index = 0;

	fimc_inputs[i] = NULL;

	fimc_input_handlers[i] = NULL;

	NUMBER_OF_INPUTS--;
}

void s3c_camif_open_sensor(camif_cis_t *cis)
{
#if 1
	clk_set_rate(cam_clock, cis->camclk);
#endif
	s3c_camif_reset(cis->reset_type, cis->reset_udelay);
}

void s3c_camif_register_sensor(camif_cis_t *cis)
{
	camif_cfg_t *codec, *preview;

	codec = s3c_camif_get_fimc_object(CODEC_MINOR);
	preview = s3c_camif_get_fimc_object(PREVIEW_MINOR);

	codec->cis = preview->cis = cis;

	sema_init((struct semaphore *) &codec->cis->lock, 1);
	sema_init((struct semaphore *) &preview->cis->lock, 1);

	preview->cis->status |= P_NOT_WORKING;	/* Default Value */

	s3c_camif_set_polarity(preview);
	s3c_camif_set_source_format(cis);
	s3c_camif_set_priority(1);
}

void s3c_camif_unregister_sensor(camif_cis_t *cis)
{
	cis->init_sensor = 0;
}

module_init(s3c_camif_register);
module_exit(s3c_camif_unregister);

EXPORT_SYMBOL(s3c_camif_open_sensor);

EXPORT_SYMBOL(s3c_camif_add_sensor);
EXPORT_SYMBOL(s3c_camif_remove_sensor);

EXPORT_SYMBOL(s3c_camif_register_sensor);
EXPORT_SYMBOL(s3c_camif_unregister_sensor);

MODULE_AUTHOR("Jinsung Yang <jsgood.yang@samsung.com>");
MODULE_DESCRIPTION("S3C Camera Driver for FIMC Interface");
MODULE_LICENSE("GPL");


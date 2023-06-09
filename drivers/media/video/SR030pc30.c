/*
 * Driver for SR030pc30 (VGA camera) from Samsung Electronics
 * 
 * 1/10" VGA CMOS Image Sensor SoC with an Embedded Image Processor
 *
 * Copyright (C) 2009, Jinsung Yang <jsgood.yang@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/version.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-i2c-drv.h>
#include <media/SR030pc30_platform.h>

#ifdef CONFIG_VIDEO_SAMSUNG_V4L2
#include <linux/videodev2_samsung.h>
#endif

#include "SR030pc30.h"
#include <mach/gpio.h>
#include <plat/gpio-cfg.h>
#include <mach/regs-gpio.h>
#include <mach/regs-clock.h>
#include <mach/max8998_function.h>

#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <asm/uaccess.h>

extern void s3c_i2c0_force_stop();

//#define VGA_CAM_DEBUG
#define CAM_TUNING_MODE

#ifdef VGA_CAM_DEBUG
#define dev_dbg	dev_err
#endif

#define SR030pc30_DRIVER_NAME	"SR030pc30"

/* Default resolution & pixelformat. plz ref SR030pc30_platform.h */
#define DEFAULT_RES			VGA				/* Index of resoultion */
#define DEFAUT_FPS_INDEX	SR030pc30_15FPS
#define DEFAULT_FMT			V4L2_PIX_FMT_UYVY	/* YUV422 */
#define POLL_TIME_MS			10

/*
 * Specification
 * Parallel : ITU-R. 656/601 YUV422, RGB565, RGB888 (Up to VGA), RAW10 
 * Serial : MIPI CSI2 (single lane) YUV422, RGB565, RGB888 (Up to VGA), RAW10
 * Resolution : 1280 (H) x 1024 (V)
 * Image control : Brightness, Contrast, Saturation, Sharpness, Glamour
 * Effect : Mono, Negative, Sepia, Aqua, Sketch
 * FPS : 15fps @full resolution, 30fps @VGA, 24fps @720p
 * Max. pixel clock frequency : 48MHz(upto)
 * Internal PLL (6MHz to 27MHz input frequency)
 */

static int SR030pc30_init(struct v4l2_subdev *sd, u32 val);

/* Camera functional setting values configured by user concept */
struct SR030pc30_userset {
	signed int exposure_bias;	/* V4L2_CID_EXPOSURE */
	unsigned int ae_lock;
	unsigned int awb_lock;
	unsigned int auto_wb;	/* V4L2_CID_AUTO_WHITE_BALANCE */
	unsigned int manual_wb;	/* V4L2_CID_WHITE_BALANCE_PRESET */
	unsigned int wb_temp;	/* V4L2_CID_WHITE_BALANCE_TEMPERATURE */
	unsigned int effect;	/* Color FX (AKA Color tone) */
	unsigned int contrast;	/* V4L2_CID_CONTRAST */
	unsigned int saturation;	/* V4L2_CID_SATURATION */
	unsigned int sharpness;		/* V4L2_CID_SHARPNESS */
	unsigned int glamour;
};

struct SR030pc30_state {
	struct SR030pc30_platform_data *pdata;
	struct v4l2_subdev sd;
	struct v4l2_pix_format pix;
	struct v4l2_fract timeperframe;
	struct SR030pc30_userset userset;
	int framesize_index;
	int freq;	/* MCLK in KHz */
	int is_mipi;
	int isize;
	int ver;
	int fps;
	int vt_mode; /*For VT camera*/
	int check_dataline;
	int check_previewdata;
};

enum {
	SR030pc30_PREVIEW_VGA,
} SR030pc30_FRAME_SIZE;

struct SR030pc30_enum_framesize {
	unsigned int index;
	unsigned int width;
	unsigned int height;	
};

struct SR030pc30_enum_framesize SR030pc30_framesize_list[] = {
	{SR030pc30_PREVIEW_VGA, 640, 480}
};

/* Here we store the status of I2C */
static int i2c_fail_check = 0; 

static char *SR030pc30_cam_tunning_table = NULL;

static int SR030pc30_cam_tunning_table_size;

static unsigned short SR030pc30_cam_tuning_data[1] = {
0xffff
};

static int CamTunningStatus = 1;

static inline struct SR030pc30_state *to_state(struct v4l2_subdev *sd)
{
	return container_of(sd, struct SR030pc30_state, sd);
}

/**
 * SR030pc30_i2c_read: Read 2 bytes from sensor 
 */

static inline int SR030pc30_i2c_read(struct i2c_client *client, 
	unsigned char subaddr, unsigned char *data)
{
	unsigned char buf[1];
	int ret = 0;
	int retry_count = 1;
	struct i2c_msg msg = {client->addr, 0, 1, buf};

	buf[0] = subaddr;
	
	//printk(KERN_DEBUG "SR030pc30_i2c_read address buf[0] = 0x%x!!", buf[0]); 	
	
	while(retry_count--){
		ret = i2c_transfer(client->adapter, &msg, 1);
		if(ret == 1)
			break;
		msleep(POLL_TIME_MS);
	}

	if(ret < 0)
		return -EIO;

	msg.flags = I2C_M_RD;

	retry_count = 1;
	while(retry_count--){
		ret  = i2c_transfer(client->adapter, &msg, 1);
		if(ret == 1)
			break;
		msleep(POLL_TIME_MS);
	}
	
	/*
	 * [Arun c]Data comes in Little Endian in parallel mode; So there
	 * is no need for byte swapping here
	 */
	*data = buf[0];
	
	return (ret == 1) ? 0 : -EIO;
}

#if 1
static int SR030pc30_i2c_write_unit(struct i2c_client *client, unsigned char addr, unsigned char val)
{
	struct i2c_msg msg[1];
	unsigned char reg[2];
	int ret = 0;
	int retry_count = 1;

	if (!client->adapter)
		return -ENODEV;

again:
	msg->addr = client->addr;
	msg->flags = 0;
	msg->len = 2;
	msg->buf = reg;

	reg[0] = addr ;
	reg[1] = val ;	

	//printk("[wsj] ---> address: 0x%02x, value: 0x%02x\n", reg[0], reg[1]);
	
	while(retry_count--)
	{
		ret  = i2c_transfer(client->adapter, msg, 1);
		if(ret == 1)
			break;
		msleep(POLL_TIME_MS);
	}

	return (ret == 1) ? 0 : -EIO;
}

static int SR030pc30_i2c_write(struct v4l2_subdev *sd, unsigned short i2c_data[], 
							int index)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int i =0, err =0;
	int delay;
	int count = index/sizeof(unsigned short);

	dev_dbg(&client->dev, "%s: count %d \n", __func__, count);
	for (i=0 ; i <count ; i++) {
		err = SR030pc30_i2c_write_unit(client, i2c_data[i] >>8, i2c_data[i] & 0xff );
		if (unlikely(err < 0)) {
			v4l_info(client, "%s: register set failed\n", \
			__func__);
			return err;
		}
	}
	return 0;
}

#else
static int SR030pc30_i2c_write(struct v4l2_subdev *sd, unsigned short i2c_data[],
				unsigned short length)
{
	int ret = -1;
	int retry_count = 1;
	
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	unsigned char buf[length*2];
	unsigned short i, j;
	struct i2c_msg msg = {client->addr, 0, 1, &buf[0]};

	j=0;
	for (i = 0; i < length; i++) {
		buf[j] = (unsigned char)((i2c_data[i] & 0xff00)>>8);
		buf[j+1] = (unsigned char)(i2c_data[i] & 0x00ff);
		j=j+2;
	}
	
	if(j != length*2)
	{
		printk(KERN_DEBUG "i2c message lengh is incorrect!!\n");
		return -EIO;
	}
	
#ifdef VGA_CAM_DEBUG
	printk("i2c cmd Length : %d, j = %d\n", length*2, j);
	for (i = 0; i < length*2; i++) {
		printk("buf[%d] = %x  ", i, buf[i]);
		if(i == length*2)
			printk("\n");
	}
	mdelay(4);	
#endif

#if 1
	for(i =0; i<length*2; i++)
	{
		msg.len = 1;
		msg.buf = buf[i];

		ret  = i2c_transfer(client->adapter, &msg, 1);
		if(ret < 0)
		{
			printk(KERN_DEBUG "i2c transper error!!\n");
			return -EIO;
		}
		udelay(1);
	}
#else
	while(retry_count--){
		ret  = i2c_transfer(client->adapter, &msg, 1);
		if(ret == 1)
			break;
		msleep(10);
	}
#endif

	return (ret == 1) ? 0 : -EIO;
}
#endif

int SR030pc30_CamTunning_table_init(void)
{
#if !defined(CAM_TUNING_MODE)
	printk(KERN_DEBUG "%s always sucess, L = %d!!", __func__, __LINE__);
	return 1;
#endif

	struct file *filp;
	char *dp;
	long l;
	loff_t pos;
	int i;
	int ret;
	mm_segment_t fs = get_fs();

	printk(KERN_DEBUG "%s %d\n", __func__, __LINE__);

	set_fs(get_ds());

	filp = filp_open("/mnt/sdcard/SR030pc30.h", O_RDONLY, 0);

	if (IS_ERR(filp)) {
		printk(KERN_DEBUG "file open error or SR030pc30.h file is not.\n");
		return PTR_ERR(filp);
	}
	
	l = filp->f_path.dentry->d_inode->i_size;	
	printk(KERN_DEBUG "l = %ld\n", l);
	dp = kmalloc(l, GFP_KERNEL);
	if (dp == NULL) {
		printk(KERN_DEBUG "Out of Memory\n");
		filp_close(filp, current->files);
	}
	
	pos = 0;
	memset(dp, 0, l);
	ret = vfs_read(filp, (char __user *)dp, l, &pos);
	
	if (ret != l) {
		printk(KERN_DEBUG "Failed to read file ret = %d\n", ret);
		vfree(dp);
		filp_close(filp, current->files);
		return -EINVAL;
	}

	filp_close(filp, current->files);
		
	set_fs(fs);
	
	SR030pc30_cam_tunning_table = dp;
		
	SR030pc30_cam_tunning_table_size = l;
	
	*((SR030pc30_cam_tunning_table + SR030pc30_cam_tunning_table_size) - 1) = '\0';
	
	printk(KERN_DEBUG "SR030pc30_regs_table 0x%08x, %ld\n", dp, l);
	printk(KERN_DEBUG "%s end, line = %d\n",__func__, __LINE__);
	
	return 0;
}

static int SR030pc30_regs_table_write(struct v4l2_subdev *sd, char *name)
{
	char *start, *end, *reg;	
	unsigned short addr;
	unsigned int count = 0;
	char reg_buf[7];

	printk(KERN_DEBUG "%s start, name = %s\n",__func__, name);

	*(reg_buf + 6) = '\0';

	start = strstr(SR030pc30_cam_tunning_table, name);
	end = strstr(start, "};");

	while (1) {	
		/* Find Address */	
		reg = strstr(start,"0x");		
		if ((reg == NULL) || (reg > end))
		{
			break;
		}

		if (reg)
			start = (reg + 8); 

		/* Write Value to Address */	
		if (reg != NULL) {
			memcpy(reg_buf, reg, 6);	
			addr = (unsigned short)simple_strtoul(reg_buf, NULL, 16); 
			if (((addr&0xff00)>>8) == 0xff)
			{
				mdelay(addr&0xff);
				printk(KERN_DEBUG "delay 0x%04x,\n", addr&0xff);
			}	
			else
			{
#ifdef VGA_CAM_DEBUG
				printk(KERN_DEBUG "addr = 0x%x, ", addr);
				if((count%10) == 0)
					printk(KERN_DEBUG "\n");
#endif
				SR030pc30_cam_tuning_data[0] = addr;
				SR030pc30_i2c_write(sd, SR030pc30_cam_tuning_data, 2); // 2byte
			}
		}
		count++;
	}
	printk(KERN_DEBUG "count = %d, %s end, line = %d\n", count, __func__, __LINE__);
	return 0;
}

static int SR030pc30_regs_write(struct v4l2_subdev *sd, unsigned short i2c_data[], unsigned short length, char *name)
{
	int err = -EINVAL;	
	
	printk(KERN_DEBUG "%s start, Status is %s mode, parameter name = %s\n",\
						__func__, (CamTunningStatus != 0) ? "binary"  : "tuning",name);
	
	 if(CamTunningStatus) // binary mode
 	{
		err = SR030pc30_i2c_write(sd, i2c_data, length);
 	}
	 else // cam tuning mode
 	{
		err = SR030pc30_regs_table_write(sd, name);
 	}

	return err;
}				

static void SR030pc30_ldo_en(bool onoff)
{
	int err;
	
	/* CAM_SENSOR_A2.8V - GPB(7) */
	err = gpio_request(GPIO_GPB7, "GPB7");

	if(err) {
		printk(KERN_ERR "failed to request GPB7 for camera control\n");

		return err;
	}
	
	if(onoff){
		// Turn CAM_SENSOR_A2.8V on
		gpio_direction_output(GPIO_GPB7, 0);	
		gpio_set_value(GPIO_GPB7, 1);
		
		// Turn VCAM_RAM_1.8V on
		Set_MAX8998_PM_OUTPUT_Voltage(LDO12, VCC_1p800);
		Set_MAX8998_PM_REG(ELDO12, 1);

		udelay(20); 
		
		// Trun 3M_CAM_D_1.2V on
		Set_MAX8998_PM_OUTPUT_Voltage(BUCK4, VCC_1p200);
		Set_MAX8998_PM_REG(EN4, 1);
		
		udelay(15); 

		// Turn CAM_IO_2.8V on
		Set_MAX8998_PM_OUTPUT_Voltage(LDO13, VCC_2p800);
		Set_MAX8998_PM_REG(ELDO13, 1);
		
		// Turn 3M_CAM_AF_2.8V_on
		Set_MAX8998_PM_OUTPUT_Voltage(LDO11, VCC_2p800);
		Set_MAX8998_PM_REG(ELDO11, 1);	
	
} else {
		// Turn CAM_IO_2.8V off
		Set_MAX8998_PM_REG(ELDO13, 0);

		// Turn 3M_CAM_AF_2.8V off
		Set_MAX8998_PM_REG(ELDO11, 0);

		// Turn 3M_CAM_D_1.2V off
		Set_MAX8998_PM_REG(EN4, 0);

		// Turn VCAM_RAM_1.8V off
		Set_MAX8998_PM_REG(ELDO12, 0);
	
		// Turn CAM_SENSOR_A2.8V off
		gpio_direction_output(GPIO_GPB7, 1);
		gpio_set_value(GPIO_GPB7, 0);
	}
	
	gpio_free(GPIO_GPB7);
}

static int SR030pc30_power_on(void)
{	
	int err;

	printk(KERN_DEBUG "(mach)SR030pc30_power_on\n");

	/* CAM_MEGA_EN - GPG3(5) */
	err = gpio_request(GPIO_CAM_MEGA_EN, "GPG3(5)");
	if(err) {
		printk(KERN_ERR "failed to request GPG3(5) for camera control\n");
		return err;
	}

	/* CAM_MEGA_nRST - GPG3(6) */
	err = gpio_request(GPIO_CAM_MEGA_nRST, "GPG3(6)");
	if(err) {
		printk(KERN_ERR "failed to request GPG3(6) for camera control\n");
		return err;
	}

	/* CAM_VGA_nSTBY - GPB(0)  */
	err = gpio_request(GPIO_CAM_VGA_nSTBY, "GPB(0)");
	if (err) {
		printk(KERN_ERR "failed to request GPB(0) for camera control\n");

		return err;
	}

	/* CAM_VGA_nRST - GPB(2) */
	err = gpio_request(GPIO_CAM_VGA_nRST, "GPB(2)");
	if (err) {
		printk(KERN_ERR "failed to request GPB(2) for camera control\n");

		return err;
	}
		
	//LDO enable
	SR030pc30_ldo_en(TRUE);

	udelay(20); //20us
	
	// CAM_VGA_nSTBY HIGH
	gpio_direction_output(GPIO_CAM_VGA_nSTBY, 0);
	gpio_set_value(GPIO_CAM_VGA_nSTBY, 1);
	
	// Mclk enable
	s3c_gpio_cfgpin(GPIO_CAM_MCLK, S5PV210_GPE1_3_CAM_A_CLKOUT);

	udelay(10); //10us

	// CAM_MEGA_EN HIGH
	gpio_direction_output(GPIO_CAM_MEGA_EN, 0);
	gpio_set_value(GPIO_CAM_MEGA_EN, 1);

	mdelay(6); //5ms 

	// CAM_MEGA_nRST HIGH
	gpio_direction_output(GPIO_CAM_MEGA_nRST, 0);
	gpio_set_value(GPIO_CAM_MEGA_nRST, 1);

	mdelay(7); // 6.5ms

	// CAM_MEGA_EN LOW
	gpio_direction_output(GPIO_CAM_MEGA_EN, 1);
	gpio_set_value(GPIO_CAM_MEGA_EN, 0);

	udelay(10); //10us

	// CAM_VGA_nRST HIGH
	gpio_direction_output(GPIO_CAM_VGA_nRST, 0);
	gpio_set_value(GPIO_CAM_VGA_nRST, 1);

	msleep(50); //50ms

	//CAM_GPIO free
	gpio_free(GPIO_CAM_MEGA_EN);
	gpio_free(GPIO_CAM_MEGA_nRST);
	gpio_free(GPIO_CAM_VGA_nSTBY);
	gpio_free(GPIO_CAM_VGA_nRST);
	
	return 0;

}

static int SR030pc30_power_off(void)
{	
	int err;
	
	printk(KERN_DEBUG "(mach)SR030pc30_power_off\n");

	/* CAM_MEGA_EN - GPG3(5) */
	err = gpio_request(GPIO_CAM_MEGA_EN, "GPG3(5)");
	if(err) {
		printk(KERN_ERR "failed to request GPG3(5) for camera control\n");
		return err;
	}

	/* CAM_MEGA_nRST - GPG3(6) */
	err = gpio_request(GPIO_CAM_MEGA_nRST, "GPG3(6)");
	if(err) {
		printk(KERN_ERR "failed to request GPG3(6) for camera control\n");
		return err;
	}

	/* CAM_VGA_nSTBY - GPB(0)  */
	err = gpio_request(GPIO_CAM_VGA_nSTBY, "GPB(0)");
	if (err) {
		printk(KERN_ERR "failed to request GPB(0) for camera control\n");

		return err;
	}

	/* CAM_VGA_nRST - GPB(2) */
	err = gpio_request(GPIO_CAM_VGA_nRST, "GPB(2)");
	if (err) {
		printk(KERN_ERR "failed to request GPB(2) for camera control\n");

		return err;
	}

	// CAM_VGA_nRST LOW	
	gpio_direction_output(GPIO_CAM_VGA_nRST, 0);
	gpio_set_value(GPIO_CAM_VGA_nRST, 0);


	// CAM_MEGA_nRST LOW
	gpio_direction_output(GPIO_CAM_MEGA_nRST, 0);
	gpio_set_value(GPIO_CAM_MEGA_nRST, 0);

	udelay(50); //50us
	
	// Mclk disable
	s3c_gpio_cfgpin(GPIO_CAM_MCLK, 0);

	
	// CAM_VGA_nSTBY LOW
	gpio_direction_output(GPIO_CAM_VGA_nSTBY, 0);
	gpio_set_value(GPIO_CAM_VGA_nSTBY, 0);

	// CAM_MEGA_EN LOW
	gpio_direction_output(GPIO_CAM_MEGA_EN, 0);
	gpio_set_value(GPIO_CAM_MEGA_EN, 0);

	//LDO disable
	SR030pc30_ldo_en(FALSE);
	
	//CAM_GPIO free
	gpio_free(GPIO_CAM_MEGA_EN);
	gpio_free(GPIO_CAM_MEGA_nRST);
	gpio_free(GPIO_CAM_VGA_nSTBY);
	gpio_free(GPIO_CAM_VGA_nRST);

	return 0;

}

static int SR030pc30_power_en(int onoff)
{
	if(onoff){
		SR030pc30_power_on();
	} else {
		SR030pc30_power_off();
		s3c_i2c0_force_stop();
	}

	return 0;
}
static int SR030pc30_reset(struct v4l2_subdev *sd)
{
	SR030pc30_power_en(0);
	mdelay(5);
	SR030pc30_power_en(1);
	mdelay(5);
	SR030pc30_init(sd, 0);
	return 0;
}


static struct v4l2_queryctrl SR030pc30_controls[] = {
};

const char **SR030pc30_ctrl_get_menu(u32 id)
{
	printk(KERN_DEBUG "SR030pc30_ctrl_get_menu is called... id : %d \n", id);

	switch (id) {
	default:
		return v4l2_ctrl_get_menu(id);
	}
}

static inline struct v4l2_queryctrl const *SR030pc30_find_qctrl(int id)
{
	int i;

	printk(KERN_DEBUG "SR030pc30_find_qctrl is called...  id : %d \n", id);

	return NULL;
}

static int SR030pc30_queryctrl(struct v4l2_subdev *sd, struct v4l2_queryctrl *qc)
{
	int i;

	printk(KERN_DEBUG "SR030pc30_queryctrl is called... \n");

	return -EINVAL;
}

static int SR030pc30_querymenu(struct v4l2_subdev *sd, struct v4l2_querymenu *qm)
{
	struct v4l2_queryctrl qctrl;

	printk(KERN_DEBUG "SR030pc30_querymenu is called... \n");

	qctrl.id = qm->id;
	SR030pc30_queryctrl(sd, &qctrl);

	return v4l2_ctrl_query_menu(qm, &qctrl, SR030pc30_ctrl_get_menu(qm->id));
}

/*
 * Clock configuration
 * Configure expected MCLK from host and return EINVAL if not supported clock
 * frequency is expected
 * 	freq : in Hz
 * 	flag : not supported for now
 */
static int SR030pc30_s_crystal_freq(struct v4l2_subdev *sd, u32 freq, u32 flags)
{
	int err = -EINVAL;

	printk(KERN_DEBUG "SR030pc30_s_crystal_freq is called... \n");

	return err;
}

static int SR030pc30_g_fmt(struct v4l2_subdev *sd, struct v4l2_format *fmt)
{
	int err = 0;

	printk(KERN_DEBUG "SR030pc30_g_fmt is called... \n");

	return err;
}

static int SR030pc30_s_fmt(struct v4l2_subdev *sd, struct v4l2_format *fmt)
{
	int err = 0;

	printk(KERN_DEBUG "SR030pc30_s_fmt is called... \n");

	return err;
}
static int SR030pc30_enum_framesizes(struct v4l2_subdev *sd, \
					struct v4l2_frmsizeenum *fsize)
{
	struct  SR030pc30_state *state = to_state(sd);
	int num_entries = sizeof(SR030pc30_framesize_list)/sizeof(struct SR030pc30_enum_framesize);	
	struct SR030pc30_enum_framesize *elem;	
	int index = 0;
	int i = 0;

	printk(KERN_DEBUG "SR030pc30_enum_framesizes is called... \n");

	/* The camera interface should read this value, this is the resolution
 	 * at which the sensor would provide framedata to the camera i/f
 	 *
 	 * In case of image capture, this returns the default camera resolution (WVGA)
 	 */
	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;

	index = state->framesize_index;

	for(i = 0; i < num_entries; i++){
		elem = &SR030pc30_framesize_list[i];
		if(elem->index == index){
			fsize->discrete.width = SR030pc30_framesize_list[index].width;
			fsize->discrete.height = SR030pc30_framesize_list[index].height;
			return 0;
		}
	}

	return -EINVAL;
}


static int SR030pc30_enum_frameintervals(struct v4l2_subdev *sd, 
					struct v4l2_frmivalenum *fival)
{
	int err = 0;

	printk(KERN_DEBUG "SR030pc30_enum_frameintervals is called... \n");
	
	return err;
}

static int SR030pc30_enum_fmt(struct v4l2_subdev *sd, struct v4l2_fmtdesc *fmtdesc)
{
	int err = 0;

	printk(KERN_DEBUG "SR030pc30_enum_fmt is called... \n");

	return err;
}

static int SR030pc30_try_fmt(struct v4l2_subdev *sd, struct v4l2_format *fmt)
{
	int err = 0;

	printk(KERN_DEBUG "SR030pc30_enum_fmt is called... \n");

	return err;
}

static int SR030pc30_g_parm(struct v4l2_subdev *sd, struct v4l2_streamparm *param)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int err = 0;

	dev_dbg(&client->dev, "%s\n", __func__);

	return err;
}

static int SR030pc30_s_parm(struct v4l2_subdev *sd, struct v4l2_streamparm *param)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int err = 0;

	dev_dbg(&client->dev, "%s: numerator %d, denominator: %d\n", \
		__func__, param->parm.capture.timeperframe.numerator, \
		param->parm.capture.timeperframe.denominator);

	return err;
}

/* set sensor register values for adjusting brightness */
static int SR030pc30_set_brightness(struct v4l2_subdev *sd, struct v4l2_control *ctrl)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct SR030pc30_state *state = to_state(sd);

	int err = -EINVAL;
	int ev_value = 0;

	dev_dbg(&client->dev, "%s: value : %d state->vt_mode %d \n", __func__, ctrl->value, state->vt_mode);

	ev_value = ctrl->value;

	printk(KERN_DEBUG "state->vt_mode : %d \n", state->vt_mode);
	if(state->vt_mode == 1)
	{
		switch(ev_value)
		{	
			case EV_MINUS_4:
				err = SR030pc30_regs_write(sd, SR030pc30_ev_vt_m4, sizeof(SR030pc30_ev_vt_m4), "SR030pc30_ev_vt_m4");
			break;

			case EV_MINUS_3:
				err = SR030pc30_regs_write(sd, SR030pc30_ev_vt_m3, sizeof(SR030pc30_ev_vt_m3), "SR030pc30_ev_vt_m3");
			break;

			
			case EV_MINUS_2:
				err = SR030pc30_regs_write(sd, SR030pc30_ev_vt_m2, sizeof(SR030pc30_ev_vt_m2), "SR030pc30_ev_vt_m2");
			break;
			
			case EV_MINUS_1:
				err = SR030pc30_regs_write(sd, SR030pc30_ev_vt_m1, sizeof(SR030pc30_ev_vt_m1), "SR030pc30_ev_vt_m1");
			break;

			case EV_DEFAULT:
				err = SR030pc30_regs_write(sd, SR030pc30_ev_vt_default, sizeof(SR030pc30_ev_vt_default), "SR030pc30_ev_vt_default");
			break;

			case EV_PLUS_1:
				err = SR030pc30_regs_write(sd, SR030pc30_ev_vt_p1, sizeof(SR030pc30_ev_vt_p1), "SR030pc30_ev_vt_p1");
 			break;

			case EV_PLUS_2:
				err = SR030pc30_regs_write(sd, SR030pc30_ev_vt_p2, sizeof(SR030pc30_ev_vt_p2), "SR030pc30_ev_vt_p2");
 			break;

			case EV_PLUS_3:
				err = SR030pc30_regs_write(sd, SR030pc30_ev_vt_p3, sizeof(SR030pc30_ev_vt_p3), "SR030pc30_ev_vt_p3");
 			break;

			case EV_PLUS_4:
				err = SR030pc30_regs_write(sd, SR030pc30_ev_vt_p4, sizeof(SR030pc30_ev_vt_p4), "SR030pc30_ev_vt_p4");
 			break;	
			
			default:
				err = SR030pc30_regs_write(sd, SR030pc30_ev_vt_default, sizeof(SR030pc30_ev_vt_default), "SR030pc30_ev_vt_default");
 			break;
		}
	}
	else
	{
		switch(ev_value)
		{	
			case EV_MINUS_4:
				err = SR030pc30_regs_write(sd, SR030pc30_ev_m4, sizeof(SR030pc30_ev_m4), "SR030pc30_ev_m4");
 			break;

			case EV_MINUS_3:
				err = SR030pc30_regs_write(sd, SR030pc30_ev_m3, sizeof(SR030pc30_ev_m3), "SR030pc30_ev_m3");
 			break;
			
			case EV_MINUS_2:
				err = SR030pc30_regs_write(sd, SR030pc30_ev_m2, sizeof(SR030pc30_ev_m2), "SR030pc30_ev_m2");
 			break;
			
			case EV_MINUS_1:
				err = SR030pc30_regs_write(sd, SR030pc30_ev_m1, sizeof(SR030pc30_ev_m1), "SR030pc30_ev_m1");
 			break;

			case EV_DEFAULT:
				err = SR030pc30_regs_write(sd, SR030pc30_ev_default, sizeof(SR030pc30_ev_default), "SR030pc30_ev_default");
 			break;

			case EV_PLUS_1:
				err = SR030pc30_regs_write(sd, SR030pc30_ev_p1, sizeof(SR030pc30_ev_p1), "SR030pc30_ev_p1");
 			break;

			case EV_PLUS_2:
				err = SR030pc30_regs_write(sd, SR030pc30_ev_p2, sizeof(SR030pc30_ev_p2), "SR030pc30_ev_p2");
 			break;

			case EV_PLUS_3:
				err = SR030pc30_regs_write(sd, SR030pc30_ev_p3, sizeof(SR030pc30_ev_p3), "SR030pc30_ev_p3");
			break;

			case EV_PLUS_4:
				err = SR030pc30_regs_write(sd, SR030pc30_ev_p4, sizeof(SR030pc30_ev_p4), "SR030pc30_ev_p4");
			break;	
			
			default:
				err = SR030pc30_regs_write(sd, SR030pc30_ev_default, sizeof(SR030pc30_ev_default), "SR030pc30_ev_default");
			break;
		}
	}
	if (err < 0)
	{
		v4l_info(client, "%s: register set failed\n", __func__);
		return -EIO;
	}
	return err;
}

/* set sensor register values for adjusting whitebalance, both auto and manual */
static int SR030pc30_set_wb(struct v4l2_subdev *sd, struct v4l2_control *ctrl)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int err = -EINVAL;

	dev_dbg(&client->dev, "%s:  value : %d \n", __func__, ctrl->value);

	switch(ctrl->value)
	{
	case WHITE_BALANCE_AUTO:
		err = SR030pc30_regs_write(sd, SR030pc30_wb_auto, sizeof(SR030pc30_wb_auto), "SR030pc30_wb_auto");
		break;

	case WHITE_BALANCE_SUNNY:
		err = SR030pc30_regs_write(sd, SR030pc30_wb_sunny, sizeof(SR030pc30_wb_sunny), "SR030pc30_wb_sunny");
		break;

	case WHITE_BALANCE_CLOUDY:
		err = SR030pc30_regs_write(sd, SR030pc30_wb_cloudy, sizeof(SR030pc30_wb_cloudy), "SR030pc30_wb_cloudy");
		break;

	case WHITE_BALANCE_TUNGSTEN:
		err = SR030pc30_regs_write(sd, SR030pc30_wb_tungsten, sizeof(SR030pc30_wb_tungsten), "SR030pc30_wb_tungsten");
		break;

	case WHITE_BALANCE_FLUORESCENT:
		err = SR030pc30_regs_write(sd, SR030pc30_wb_fluorescent, sizeof(SR030pc30_wb_fluorescent), "SR030pc30_wb_fluorescent");
		break;

	default:
		dev_dbg(&client->dev, "%s: Not Support value \n", __func__);
		err = 0;
		break;

	}
	return err;
}

/* set sensor register values for adjusting color effect */
static int SR030pc30_set_effect(struct v4l2_subdev *sd, struct v4l2_control *ctrl)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int err = -EINVAL;

	dev_dbg(&client->dev, "%s: value : %d \n", __func__, ctrl->value);

	switch(ctrl->value)
	{
	case IMAGE_EFFECT_NONE:
		err = SR030pc30_regs_write(sd, SR030pc30_effect_none, sizeof(SR030pc30_effect_none), "SR030pc30_effect_none");
		break;

	case IMAGE_EFFECT_BNW:		//Gray
		err = SR030pc30_regs_write(sd, SR030pc30_effect_gray, sizeof(SR030pc30_effect_gray), "SR030pc30_effect_gray");
		break;

	case IMAGE_EFFECT_SEPIA:
		err = SR030pc30_regs_write(sd, SR030pc30_effect_sepia, sizeof(SR030pc30_effect_sepia), "SR030pc30_effect_sepia");
		break;

	case IMAGE_EFFECT_AQUA:
		err = SR030pc30_regs_write(sd, SR030pc30_effect_aqua, sizeof(SR030pc30_effect_aqua), "SR030pc30_effect_aqua");
		break;

	case IMAGE_EFFECT_NEGATIVE:
		err = SR030pc30_regs_write(sd, SR030pc30_effect_negative, sizeof(SR030pc30_effect_negative), "SR030pc30_effect_negative");
		break;

	default:
		dev_dbg(&client->dev, "%s: Not Support value \n", __func__);
		err = 0;
		break;

	}
	
	return err;
}

/* set sensor register values for frame rate(fps) setting */
static int SR030pc30_set_frame_rate(struct v4l2_subdev *sd, struct v4l2_control *ctrl)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct SR030pc30_state *state = to_state(sd);

	int err = -EINVAL;
	int i = 0;

	dev_dbg(&client->dev, "%s: value : %d \n", __func__, ctrl->value);
	
	printk(KERN_DEBUG "state->vt_mode : %d \n", state->vt_mode);
	if(state->vt_mode == 1)
	{
		switch(ctrl->value)
		{
		case 7:
			err = SR030pc30_regs_write(sd, SR030pc30_vt_fps_7, sizeof(SR030pc30_vt_fps_7), "SR030pc30_vt_fps_7");
			break;

		case 10:
			err = SR030pc30_regs_write(sd, SR030pc30_vt_fps_10, sizeof(SR030pc30_vt_fps_10), "SR030pc30_vt_fps_10");
			break;
			
		case 15:
			err = SR030pc30_regs_write(sd, SR030pc30_vt_fps_15, sizeof(SR030pc30_vt_fps_15), "SR030pc30_vt_fps_15");
			break;

		default:
			dev_dbg(&client->dev, "%s: Not Support value \n", __func__);
			err = 0;
			break;
		}
	}
	else
	{
		switch(ctrl->value)
		{
		case 7:
			err = SR030pc30_regs_write(sd, SR030pc30_fps_7, sizeof(SR030pc30_fps_7), "SR030pc30_fps_7");
			break;

		case 10:
			err = SR030pc30_regs_write(sd, SR030pc30_fps_10, sizeof(SR030pc30_fps_10), "SR030pc30_fps_10");
			break;
			
		case 15:
			err = SR030pc30_regs_write(sd, SR030pc30_fps_15, sizeof(SR030pc30_fps_15), "SR030pc30_fps_15");
			break;

		default:
			dev_dbg(&client->dev, "%s: Not Support value \n", __func__);
			err = 0;
			break;
		}
	}
	return err;
}

/* set sensor register values for adjusting blur effect */
static int SR030pc30_set_blur(struct v4l2_subdev *sd, struct v4l2_control *ctrl)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct SR030pc30_state *state = to_state(sd);
	int err = -EINVAL;

	dev_dbg(&client->dev, "%s: value : %d \n", __func__, ctrl->value);
	
	printk(KERN_DEBUG "state->vt_mode : %d \n", state->vt_mode);
	if(state->vt_mode == 1)
	{
		switch(ctrl->value)
		{
			case BLUR_LEVEL_0:
				err = SR030pc30_regs_write(sd, SR030pc30_blur_vt_none, sizeof(SR030pc30_blur_vt_none), "SR030pc30_blur_vt_none");
				break;

			case BLUR_LEVEL_1:	
				err = SR030pc30_regs_write(sd, SR030pc30_blur_vt_p1, sizeof(SR030pc30_blur_vt_p1), "SR030pc30_blur_vt_p1");
				break;

			case BLUR_LEVEL_2:
				err = SR030pc30_regs_write(sd, SR030pc30_blur_vt_p2, sizeof(SR030pc30_blur_vt_p2), "SR030pc30_blur_vt_p2");
				break;

			case BLUR_LEVEL_3:
				err = SR030pc30_regs_write(sd, SR030pc30_blur_vt_p3, sizeof(SR030pc30_blur_vt_p3), "SR030pc30_blur_vt_p3");
				break;

			default:
				dev_dbg(&client->dev, "%s: Not Support value \n", __func__);
				err = 0;
				break;

		}
	}
	else
	{
		switch(ctrl->value)
		{
			case BLUR_LEVEL_0:
				err = SR030pc30_regs_write(sd, SR030pc30_blur_none, sizeof(SR030pc30_blur_none), "SR030pc30_blur_none");
				break;

			case BLUR_LEVEL_1:	
				err = SR030pc30_regs_write(sd, SR030pc30_blur_p1, sizeof(SR030pc30_blur_p1), "SR030pc30_blur_p1");
				break;

			case BLUR_LEVEL_2:
				err = SR030pc30_regs_write(sd, SR030pc30_blur_p2, sizeof(SR030pc30_blur_p2), "SR030pc30_blur_p2");
				break;

			case BLUR_LEVEL_3:
				err = SR030pc30_regs_write(sd, SR030pc30_blur_p3, sizeof(SR030pc30_blur_p3), "SR030pc30_blur_p3");
				break;

			default:
				dev_dbg(&client->dev, "%s: Not Support value \n", __func__);
				err = 0;
				break;
		}		
	}
	return err;
}

static int SR030pc30_check_dataline_stop(struct v4l2_subdev *sd)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct SR030pc30_state *state = to_state(sd);
	int err = -EINVAL, i;

	dev_dbg(&client->dev, "%s\n", __func__);

	err = SR030pc30_regs_write(sd, SR030pc30_dataline_stop, \
				sizeof(SR030pc30_dataline_stop), "SR030pc30_dataline_stop");
	if (err < 0)
	{
		v4l_info(client, "%s: register set failed\n", __func__);
		return -EIO;
	}

	state->check_dataline = 0;
	err = SR030pc30_reset(sd);
	if (err < 0)
	{
		v4l_info(client, "%s: register set failed\n", __func__);
		return -EIO;
	}
	return err;
}

/* if you need, add below some functions below */

static int SR030pc30_g_ctrl(struct v4l2_subdev *sd, struct v4l2_control *ctrl)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct SR030pc30_state *state = to_state(sd);
	struct SR030pc30_userset userset = state->userset;
	int err = -EINVAL;

	dev_dbg(&client->dev, "%s: id : 0x%08x \n", __func__, ctrl->id);

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ctrl->value = userset.exposure_bias;
		err = 0;
		break;

	case V4L2_CID_AUTO_WHITE_BALANCE:
		ctrl->value = userset.auto_wb;
		err = 0;
		break;

	case V4L2_CID_WHITE_BALANCE_PRESET:
		ctrl->value = userset.manual_wb;
		err = 0;
		break;

	case V4L2_CID_COLORFX:
		ctrl->value = userset.effect;
		err = 0;
		break;

	case V4L2_CID_CONTRAST:
		ctrl->value = userset.contrast;
		err = 0;
		break;

	case V4L2_CID_SATURATION:
		ctrl->value = userset.saturation;
		err = 0;
		break;

	case V4L2_CID_SHARPNESS:
		ctrl->value = userset.saturation;
		err = 0;
		break;

#if 0
	case V4L2_CID_CAM_FRAMESIZE_INDEX:
		ctrl->value = SR030pc30_get_framesize_index(sd);
		err = 0;
		break;
#endif

	default:
		dev_dbg(&client->dev, "%s: no such ctrl\n", __func__);
		break;
	}
	
	return err;
}

static int SR030pc30_s_ctrl(struct v4l2_subdev *sd, struct v4l2_control *ctrl)
{
#ifdef SR030pc30_COMPLETE
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct SR030pc30_state *state = to_state(sd);

	int err = -EINVAL;

	printk(KERN_DEBUG "SR030pc30_s_ctrl() : ctrl->id 0x%08x, ctrl->value %d \n",ctrl->id, ctrl->value);

	if (i2c_fail_check == -1)
	{
		return -EIO;
	}

	switch (ctrl->id) {

	case V4L2_CID_CAMERA_BRIGHTNESS:	//V4L2_CID_EXPOSURE:
		dev_dbg(&client->dev, "%s: V4L2_CID_CAMERA_BRIGHTNESS\n", __func__);
		err = SR030pc30_set_brightness(sd, ctrl);
		break;

	case V4L2_CID_CAMERA_WHITE_BALANCE: //V4L2_CID_AUTO_WHITE_BALANCE:
		dev_dbg(&client->dev, "%s: V4L2_CID_AUTO_WHITE_BALANCE\n", __func__);
		err = SR030pc30_set_wb(sd, ctrl);
		break;

	case V4L2_CID_CAMERA_EFFECT:	//V4L2_CID_COLORFX:
		dev_dbg(&client->dev, "%s: V4L2_CID_CAMERA_EFFECT\n", __func__);
		err = SR030pc30_set_effect(sd, ctrl);
		break;

	case V4L2_CID_CAMERA_FRAME_RATE:
		dev_dbg(&client->dev, "%s: V4L2_CID_CAMERA_FRAME_RATE\n", __func__);
		err = SR030pc30_set_frame_rate(sd, ctrl);	
		break;
		
	case V4L2_CID_CAMERA_VGA_BLUR:
		dev_dbg(&client->dev, "%s: V4L2_CID_CAMERA_FRAME_RATE\n", __func__);
		err = SR030pc30_set_blur(sd, ctrl);	
		break;

	case V4L2_CID_CAMERA_VT_MODE:
		state->vt_mode = ctrl->value;
		dev_dbg(&client->dev, "%s: V4L2_CID_CAMERA_VT_MODE : state->vt_mode %d \n", __func__, state->vt_mode);
		err = 0;
		break;

	case V4L2_CID_CAMERA_CHECK_DATALINE:
		state->check_dataline = ctrl->value;
		err = 0;
		break;	

	case V4L2_CID_CAMERA_CHECK_DATALINE_STOP:
		err = SR030pc30_check_dataline_stop(sd);
		break;

	case V4L2_CID_CAM_PREVIEW_ONOFF:
		if(state->check_previewdata == 0)
		{
			if(state->check_dataline)
			{	
				err = SR030pc30_i2c_write(sd, SR030pc30_dataline, \
							sizeof(SR030pc30_dataline));
				if (err < 0)
				{
					v4l_info(client, "%s: register set failed\n", __func__);
					return -EIO;
				}	
			}
			err = 0;
		}
		else
		{
			err = -EIO;	
		}
		break;

	//s1_camera [ Defense process by ESD input ] _[
	case V4L2_CID_CAMERA_RESET:
		dev_dbg(&client->dev, "%s: V4L2_CID_CAMERA_RESET \n", __func__);
		err = SR030pc30_reset(sd);
		break;
	// _]

	default:
		dev_dbg(&client->dev, "%s: no support control in camera sensor, SR030pc30\n", __func__);
		//err = -ENOIOCTLCMD;
		err = 0;
		break;
	}

	if (err < 0)
		goto out;
	else
		return 0;

out:
	dev_dbg(&client->dev, "%s: vidioc_s_ctrl failed\n", __func__);
	return err;
#else
	return 0;
#endif
}

static int SR030pc30_init(struct v4l2_subdev *sd, u32 val)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct SR030pc30_state *state = to_state(sd);
	int err = -EINVAL, i;
	unsigned char read_value;
	i2c_fail_check = 0;

	//v4l_info(client, "%s: camera initialization start : state->vt_mode %d \n", __func__, state->vt_mode);
	printk(KERN_DEBUG "camera initialization start, state->vt_mode : %d \n", state->vt_mode); 
	printk(KERN_DEBUG "state->check_dataline : %d \n", state->check_dataline); 
	CamTunningStatus = SR030pc30_CamTunning_table_init();
	err = CamTunningStatus;
	if (CamTunningStatus==0) {
		msleep(100);
	}

	err = SR030pc30_i2c_read(client, 0x04, &read_value); //device ID
	
	if (err < 0) {
		//This is preview fail 
		v4l_err(client, "%s: camera initialization failed. err(%d)\n", \
			__func__, err);
		i2c_fail_check = -1; //I2C fail
		return -EIO;	/* FIXME */	
	}
	printk(KERN_DEBUG "SR030pc30 Device ID 0x8C = 0x%x \n!!", read_value); 
	mdelay(3);	
			
	if(state->vt_mode == 0)
	{
		if(0)
		{	
			err = SR030pc30_i2c_write(sd, SR030pc30_dataline, \
						sizeof(SR030pc30_dataline));
			if (err < 0)
			{
				v4l_info(client, "%s: register set failed\n", \
				__func__);
			}	
		}
		else
		{
			err = SR030pc30_regs_write(sd, SR030pc30_init_reg, \
				sizeof(SR030pc30_init_reg), "SR030pc30_init_reg");
			if (err < 0)
			{
				v4l_info(client, "%s: register set failed\n", \
				__func__);
			}
#if defined(VGA_CAM_DEBUG)
			else
			{
				printk(KERN_DEBUG "SR030pc30_init_reg done\n!!"); 
				mdelay(2);
			}
#endif	
		}
	}
	else
	{
		err = SR030pc30_regs_write(sd, SR030pc30_init_vt_reg, \
					sizeof(SR030pc30_init_vt_reg), "SR030pc30_init_vt_reg");
		if (err < 0)
		{
			v4l_info(client, "%s: register set failed\n", \
			__func__);
		}	
	}

	if (err < 0) {
		//This is preview fail 
		state->check_previewdata = 100;
		v4l_err(client, "%s: camera initialization failed. err(%d)\n", \
			__func__, state->check_previewdata);
		return -EIO;	/* FIXME */	
	}

	//This is preview success
	state->check_previewdata = 0;
	return 0;
}

/*
 * s_config subdev ops
 * With camera device, we need to re-initialize every single opening time therefor,
 * it is not necessary to be initialized on probe time. except for version checking
 * NOTE: version checking is optional
 */
static int SR030pc30_s_config(struct v4l2_subdev *sd, int irq, void *platform_data)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct SR030pc30_state *state = to_state(sd);
	struct SR030pc30_platform_data *pdata;

	dev_dbg(&client->dev, "fetching platform data\n");

	pdata = client->dev.platform_data;

	if (!pdata) {
		dev_err(&client->dev, "%s: no platform data\n", __func__);
		return -ENODEV;
	}

	/*
	 * Assign default format and resolution
	 * Use configured default information in platform data
	 * or without them, use default information in driver
	 */
	if (!(pdata->default_width && pdata->default_height)) {
		/* TODO: assign driver default resolution */
	} else {
		state->pix.width = pdata->default_width;
		state->pix.height = pdata->default_height;
	}

	if (!pdata->pixelformat)
		state->pix.pixelformat = DEFAULT_FMT;
	else
		state->pix.pixelformat = pdata->pixelformat;

	if (!pdata->freq)
		state->freq = 24000000;	/* 24MHz default */
	else
		state->freq = pdata->freq;

	if (!pdata->is_mipi) {
		state->is_mipi = 0;
		dev_dbg(&client->dev, "parallel mode\n");
	} else
		state->is_mipi = pdata->is_mipi;

	return 0;
}

static const struct v4l2_subdev_core_ops SR030pc30_core_ops = {
	.init = SR030pc30_init,	/* initializing API */
	.s_config = SR030pc30_s_config,	/* Fetch platform data */
	.queryctrl = SR030pc30_queryctrl,
	.querymenu = SR030pc30_querymenu,
	.g_ctrl = SR030pc30_g_ctrl,
	.s_ctrl = SR030pc30_s_ctrl,
};

static const struct v4l2_subdev_video_ops SR030pc30_video_ops = {
	.s_crystal_freq = SR030pc30_s_crystal_freq,
	.g_fmt = SR030pc30_g_fmt,
	.s_fmt = SR030pc30_s_fmt,
	.enum_framesizes = SR030pc30_enum_framesizes,
	.enum_frameintervals = SR030pc30_enum_frameintervals,
	.enum_fmt = SR030pc30_enum_fmt,
	.try_fmt = SR030pc30_try_fmt,
	.g_parm = SR030pc30_g_parm,
	.s_parm = SR030pc30_s_parm,
};

static const struct v4l2_subdev_ops SR030pc30_ops = {
	.core = &SR030pc30_core_ops,
	.video = &SR030pc30_video_ops,
};

/*
 * SR030pc30_probe
 * Fetching platform data is being done with s_config subdev call.
 * In probe routine, we just register subdev device
 */
static int SR030pc30_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct SR030pc30_state *state;
	struct v4l2_subdev *sd;

	state = kzalloc(sizeof(struct SR030pc30_state), GFP_KERNEL);
	if (state == NULL)
		return -ENOMEM;

	sd = &state->sd;
	strcpy(sd->name, SR030pc30_DRIVER_NAME);

	/* Registering subdev */
	v4l2_i2c_subdev_init(sd, client, &SR030pc30_ops);

	dev_dbg(&client->dev, "SR030pc30 has been probed\n");
	return 0;
}


static int SR030pc30_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);

	v4l2_device_unregister_subdev(sd);
	kfree(to_state(sd));
	return 0;
}

static const struct i2c_device_id SR030pc30_id[] = {
	{ SR030pc30_DRIVER_NAME, 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, SR030pc30_id);

static struct v4l2_i2c_driver_data v4l2_i2c_data = {
	.name = SR030pc30_DRIVER_NAME,
	.probe = SR030pc30_probe,
	.remove = SR030pc30_remove,
	.id_table = SR030pc30_id,
};

MODULE_DESCRIPTION("Samsung Electronics SR030pc30 UXGA camera driver");
MODULE_AUTHOR("Jinsung Yang <jsgood.yang@samsung.com>");
MODULE_LICENSE("GPL");


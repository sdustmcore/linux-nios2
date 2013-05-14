/*
 *  altfb.c -- Altera framebuffer driver
 *
 *  Based on vfb.c -- Virtual frame buffer device
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/fb.h>
#include <linux/init.h>

/*
 *  RAM we reserve for the frame buffer. This defines the maximum screen
 *  size
 *
 *  The default can be overridden if the driver is compiled as a module
 */

static struct fb_var_screeninfo altfb_default __devinitdata = {
	.activate = FB_ACTIVATE_NOW,
	.height = -1,
	.width = -1,
	.vmode = FB_VMODE_NONINTERLACED,
};

static struct fb_fix_screeninfo altfb_fix __devinitdata = {
	.id = "altfb",
	.type = FB_TYPE_PACKED_PIXELS,
	.visual = FB_VISUAL_TRUECOLOR,
	.accel = FB_ACCEL_NONE,
};

static int altfb_setcolreg(unsigned regno, unsigned red, unsigned green,
			   unsigned blue, unsigned transp, struct fb_info *info)
{
	/*
	 *  Set a single color register. The values supplied have a 32/16 bit
	 *  magnitude.
	 *  Return != 0 for invalid regno.
	 */

	if (regno > 255)
		return 1;

    if(info->var.bits_per_pixel == 16) {
	    red >>= 11;
	    green >>= 10;
	    blue >>= 11;

	    if (regno < 255) {
		    ((u32 *) info->pseudo_palette)[regno] = ((red & 31) << 11) |
		        ((green & 63) << 5) | (blue & 31);
	    }
    } else {
	    red >>= 8;
	    green >>= 8;
	    blue >>= 8;

	    if (regno < 255) {
		    ((u32 *) info->pseudo_palette)[regno] = ((red & 255) << 16) |
		        ((green & 255) << 8) | (blue & 255);
	    }
    }

	return 0;
}

static struct fb_ops altfb_ops = {
	.owner = THIS_MODULE,
	.fb_fillrect = cfb_fillrect,
	.fb_copyarea = cfb_copyarea,
	.fb_imageblit = cfb_imageblit,
	.fb_setcolreg = altfb_setcolreg,
};

/*
 *  Initialization
 */

#define ALTERA_SGDMA_IO_EXTENT 0x400

#define ALTERA_SGDMA_STATUS 0
#define ALTERA_SGDMA_STATUS_BUSY_MSK (0x10)

#define ALTERA_SGDMA_CONTROL 16
#define ALTERA_SGDMA_CONTROL_RUN_MSK  (0x20)
#define ALTERA_SGDMA_CONTROL_SOFTWARERESET_MSK (0X10000)
#define ALTERA_SGDMA_CONTROL_PARK_MSK (0X20000)

#define ALTERA_SGDMA_NEXT_DESC_POINTER 32

/* SGDMA can only transfer this many bytes per descriptor */
#define DISPLAY_BYTES_PER_DESC 0xFF00UL
#define ALTERA_SGDMA_DESCRIPTOR_CONTROL_GENERATE_EOP_MSK (0x1)
#define ALTERA_SGDMA_DESCRIPTOR_CONTROL_GENERATE_SOP_MSK (0x4)
#define ALTERA_SGDMA_DESCRIPTOR_CONTROL_OWNED_BY_HW_MSK (0x80)
#define DISPLAY_DESC_COUNT(len) (((len) + DISPLAY_BYTES_PER_DESC - 1) \
				/ DISPLAY_BYTES_PER_DESC)
#define DISPLAY_DESC_SIZE(len) (DISPLAY_DESC_COUNT(len) \
				* sizeof(struct sgdma_desc))

struct sgdma_desc {
	u32 read_addr;
	u32 read_addr_pad;

	u32 write_addr;
	u32 write_addr_pad;

	u32 next;
	u32 next_pad;

	u16 bytes_to_transfer;
	u8 read_burst;
	u8 write_burst;

	u16 actual_bytes_transferred;
	u8 status;
	u8 control;

} __attribute__ ((packed));

static int __init altfb_dma_start(unsigned long base, unsigned long start, unsigned long len,
				  void *descp)
{
	unsigned long first_desc_phys = start + len;
	unsigned long next_desc_phys = first_desc_phys;
	struct sgdma_desc *desc = descp;
	unsigned ctrl = ALTERA_SGDMA_DESCRIPTOR_CONTROL_OWNED_BY_HW_MSK;

	writel(ALTERA_SGDMA_CONTROL_SOFTWARERESET_MSK, \
	       base + ALTERA_SGDMA_CONTROL);	/* halt current transfer */
	writel(0, base + ALTERA_SGDMA_CONTROL);	/* disable interrupts */
	writel(0xff, base + ALTERA_SGDMA_STATUS);	/* clear status */
	writel(first_desc_phys, base + ALTERA_SGDMA_NEXT_DESC_POINTER);

	while (len) {
		unsigned long cc = min(len, DISPLAY_BYTES_PER_DESC);
		next_desc_phys += sizeof(struct sgdma_desc);
		desc->read_addr = start;
		desc->next = next_desc_phys;
		desc->bytes_to_transfer = cc;
		desc->control = ctrl;
		start += cc;
		len -= cc;
		desc++;
	}

	desc--;
	desc->next = first_desc_phys;
	desc->control = ctrl | ALTERA_SGDMA_DESCRIPTOR_CONTROL_GENERATE_EOP_MSK;
	desc = descp;
	desc->control = ctrl | ALTERA_SGDMA_DESCRIPTOR_CONTROL_GENERATE_SOP_MSK;
	writel(ALTERA_SGDMA_CONTROL_RUN_MSK | ALTERA_SGDMA_CONTROL_PARK_MSK, \
	       base + ALTERA_SGDMA_CONTROL);	/* start */
	return 0;
}

	/* R   G   B */
#define COLOR_WHITE	{204, 204, 204}
#define COLOR_AMBAR	{208, 208,   0}
#define COLOR_CIAN	{  0, 206, 206}
#define	COLOR_GREEN	{  0, 239,   0}
#define COLOR_MAGENTA	{239,   0, 239}
#define COLOR_RED	{205,   0,   0}
#define COLOR_BLUE	{  0,   0, 255}
#define COLOR_BLACK	{  0,   0,   0}

struct bar_std {
	u8 bar[8][3];
};

/* Maximum number of bars are 10 - otherwise, the input print code
   should be modified */
static struct bar_std __initdata bars[] = {
	{			/* Standard ITU-R color bar sequence */
	 {
	  COLOR_WHITE,
	  COLOR_AMBAR,
	  COLOR_CIAN,
	  COLOR_GREEN,
	  COLOR_MAGENTA,
	  COLOR_RED,
	  COLOR_BLUE,
	  COLOR_BLACK,
	  }
	 }
};

static void __init altfb_color_bar(struct fb_info *info)
{
	unsigned short *p16 = (void *)info->screen_base;
	unsigned *p32 = (void *)info->screen_base;
	unsigned xres = info->var.xres;
	unsigned xbar = xres / 8;
	unsigned yres = info->var.yres;
	unsigned x, y, i;
	for (y = 0; y < yres; y++) {
		for (i = 0; i < 8; i++) {
		    if(info->var.bits_per_pixel == 16) {
			    unsigned short d;
			    d = bars[0].bar[i][2] >> 3;
			    d |= (bars[0].bar[i][1] << 2) & 0x7e0;
			    d |= (bars[0].bar[i][0] << 8) & 0xf800;
			    for (x = 0; x < xbar; x++)
				    *p16++ = d;
		    } else {
			    unsigned d;
			    d = bars[0].bar[i][2];
			    d |= bars[0].bar[i][1] << 8;
			    d |= bars[0].bar[i][0] << 16;
			    for (x = 0; x < xbar; x++)
				    *p32++ = d;
		    }
		}
	}
}

static int __devinit altfb_probe(struct platform_device *pdev)
{
	struct fb_info *info;
	struct resource *res;
	int retval = -ENOMEM;
	void *fbmem_virt;
	u8 *desc_virt;
	const __be32* val;
	void *sgdma_base;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
	    return -ENODEV;

	info = framebuffer_alloc(sizeof(u32) * 256, &pdev->dev);
	if (!info)
		goto err;

	info->fbops = &altfb_ops;
	info->var = altfb_default;

	val = of_get_property(pdev->dev.of_node, "width", NULL);
	if (!val) {
		dev_err(&pdev->dev, "Missing required parameter 'width'");
		return -ENODEV;
	}

	info->var.xres = be32_to_cpup(val),
	info->var.xres_virtual = info->var.xres,

	val = of_get_property(pdev->dev.of_node, "height", NULL);
	if (!val) {
		dev_err(&pdev->dev, "Missing required parameter 'height'");
		return -ENODEV;
	}

	info->var.yres = be32_to_cpup(val);
	info->var.yres_virtual = info->var.yres;

	val = of_get_property(pdev->dev.of_node, "bpp", NULL);
	if (!val) {
		dev_err(&pdev->dev, "Missing required parameter 'bpp'");
		return -ENODEV;
	}

	info->var.bits_per_pixel = be32_to_cpup(val);
	if(info->var.bits_per_pixel == 24) {
		dev_info(&pdev->dev, "BPP is set to 24. Using 32 to align to 16bit addresses");
		info->var.bits_per_pixel = 32;
	}
	if(info->var.bits_per_pixel == 16) {
		info->var.red.offset = 11;
		info->var.red.length = 5;
		info->var.red.msb_right = 0;
		info->var.green.offset = 5;
		info->var.green.length = 6;
		info->var.green.msb_right = 0;
		info->var.blue.offset = 0;
		info->var.blue.length = 5;
		info->var.blue.msb_right = 0;
	} else {
		info->var.red.offset = 16;
		info->var.red.length = 8;
		info->var.red.msb_right = 0;
		info->var.green.offset = 8;
		info->var.green.length = 8;
		info->var.green.msb_right = 0;
		info->var.blue.offset = 0;
		info->var.blue.length = 8;
		info->var.blue.msb_right = 0;
	}
	info->fix = altfb_fix;
	info->fix.line_length = (info->var.xres * (info->var.bits_per_pixel >> 3));
	info->fix.smem_len = info->fix.line_length * info->var.yres;

	/* sgdma descriptor table is located at the end of display memory */
	fbmem_virt = dma_alloc_coherent(NULL,
					info->fix.smem_len +
					DISPLAY_DESC_SIZE(info->fix.smem_len),
					(void *)&(info->fix.smem_start),
					GFP_KERNEL);
	if (!fbmem_virt) {
		dev_err(&pdev->dev, "altfb: unable to allocate %ld Bytes fb memory\n",
			info->fix.smem_len + DISPLAY_DESC_SIZE(info->fix.smem_len));
		return -ENOMEM;
	}

	info->screen_base = fbmem_virt;
	info->pseudo_palette = info->par;
	info->par = NULL;
	info->flags = FBINFO_FLAG_DEFAULT;

	retval = fb_alloc_cmap(&info->cmap, 256, 0);
	if (retval < 0)
		goto err1;

	platform_set_drvdata(pdev, info);

	desc_virt = fbmem_virt;
	desc_virt += info->fix.smem_len;

	if (!request_mem_region(res->start, resource_size(res), pdev->name)) {
		dev_err(&pdev->dev, "Memory region busy\n");
		retval = -EBUSY;
		goto err2;
	}
	sgdma_base = ioremap_nocache(res->start, resource_size(res));
	if(!sgdma_base) {
		retval = -EIO;
		goto err3;
	}
	if (altfb_dma_start((unsigned long)sgdma_base, info->fix.smem_start, info->fix.smem_len, desc_virt))
		goto err4;
	iounmap(sgdma_base);
	release_region(res->start, resource_size(res));

	printk(KERN_INFO "fb%d: %s frame buffer device at 0x%x+0x%x\n",
		info->node, info->fix.id, (unsigned)info->fix.smem_start,
		info->fix.smem_len);
	altfb_color_bar(info);
	retval = register_framebuffer(info);
	if (retval < 0)
		goto err4;
	return 0;
err4:
	iounmap(sgdma_base);
err3:
	release_region(res->start, resource_size(res));
err2:
	fb_dealloc_cmap(&info->cmap);
err1:
	framebuffer_release(info);
err:
	dma_free_coherent(NULL, altfb_fix.smem_len, fbmem_virt,
			  altfb_fix.smem_start);
	return retval;
}

static int __devexit altfb_remove(struct platform_device *dev)
{
	struct fb_info *info = platform_get_drvdata(dev);

	if (info) {
		unregister_framebuffer(info);
		dma_free_coherent(NULL, info->fix.smem_len, info->screen_base,
				  info->fix.smem_start);
		framebuffer_release(info);
	}
	return 0;
}

static struct of_device_id altfb_match[] = {
	{ .compatible = "ALTR,altfb-12.1", },
	{ .compatible = "ALTR,altfb-1.0", },
	{},
};
MODULE_DEVICE_TABLE(of, altfb_match);

static struct platform_driver altfb_driver = {
	.probe = altfb_probe,
	.remove = __devexit_p(altfb_remove),
	.driver = {
	    .owner = THIS_MODULE,
		.name = "altfb",
		.of_match_table = altfb_match,
    },
};

module_platform_driver(altfb_driver);

MODULE_DESCRIPTION("Altera framebuffer driver");
MODULE_AUTHOR("Thomas Chou <thomas@wytron.com.tw>");
MODULE_LICENSE("GPL");

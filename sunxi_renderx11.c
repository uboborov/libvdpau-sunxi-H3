/*
 * Copyright (c) 2015-2016 Jens Kuske <jenskuske@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "kernel-headers/sunxi_display2.h"
#include "vdpau_private.h"
#include "sunxi_disp.h"
#include <stdio.h>
#include <X11/Intrinsic.h>
#include <X11/Xutil.h>
#ifdef DEF_SHM
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#endif /*DEF_SHM*/

#ifdef HAS_NEON
#include "csc_neon.h"
#endif

struct sunxi_dispx11_private
{
	struct sunxi_disp pub;
	VdpDevice device;

  //int fd;
  //	disp_layer_config video_config;
  //	unsigned int screen_width;
  //	disp_layer_config osd_config;
  Display *display;
  Drawable drawable;
     int depth;
    int shm;
    char *data[3];
    int bytes_per_line;
    char *image;
    void *ximage;
    void *context;
};

static void sunxi_dispx11_close(struct sunxi_disp *sunxi_disp);
static int sunxi_dispx11_set_video_layer(struct sunxi_disp *sunxi_disp, int x, int y, int width, int height, output_surface_ctx_t *surface);
static void sunxi_dispx11_close_video_layer(struct sunxi_disp *sunxi_disp);
static int sunxi_dispx11_set_osd_layer(struct sunxi_disp *sunxi_disp, int x, int y, int width, int height, output_surface_ctx_t *surface);
static void sunxi_dispx11_close_osd_layer(struct sunxi_disp *sunxi_disp);

/* Planar picture buffer.
 * Pitch corresponds to luminance component in bytes. Chrominance pitches are
 * inferred from the color subsampling ratio. */
struct yuv_planes {
    void *y, *u, *v;
    size_t pitch;
};

/* Packed picture buffer. Pitch is in bytes (_not_ pixels). */
struct yuv_pack {
    void *yuv;
    size_t pitch;
};

#define USE_MEASUREMENT

#ifdef USE_MEASUREMENT
#include <time.h>

struct timespec tm[4];

# ifndef MAX
#  define MAX(x, y) ( ((x)>(y))?(x):(y) )
# endif

# ifndef MIN
#  define MIN(x, y) ( ((x)<(y))?(x):(y) )
# endif
/*
*/
unsigned long int time_diff(struct timespec *ts1, struct timespec *ts2) {
    static struct timespec ts;
    ts.tv_sec = MAX(ts2->tv_sec, ts1->tv_sec) - MIN(ts2->tv_sec, ts1->tv_sec);
    ts.tv_nsec = MAX(ts2->tv_nsec, ts1->tv_nsec) - MIN(ts2->tv_nsec, ts1->tv_nsec);

    if (ts.tv_sec > 0) {
        ts.tv_sec--;
        ts.tv_nsec += 1000000000;
    }

    return((ts.tv_sec * 1000000000) + ts.tv_nsec);
}
#endif

struct sunxi_disp *sunxi_dispx11_open(Display *display, Drawable drawable, VdpDevice device)
{
  struct sunxi_dispx11_private *disp = calloc(1, sizeof(*disp));

  if (disp) {
    disp->display = XOpenDisplay(XDisplayString(display));
    disp->drawable = drawable;
    disp->pub.close = sunxi_dispx11_close;
    disp->pub.set_video_layer = sunxi_dispx11_set_video_layer;
    disp->pub.close_video_layer = sunxi_dispx11_close_video_layer;
    disp->pub.set_osd_layer = sunxi_dispx11_set_osd_layer;
    disp->pub.close_osd_layer = sunxi_dispx11_close_osd_layer;
    disp->device = device;

    fprintf(stderr, "%s:%d\n", __func__, __LINE__);
    return (struct sunxi_disp *)disp;
  }

  return NULL;
}

static void sunxi_dispx11_close(struct sunxi_disp *sunxi_disp)
{
  //struct sunxi_dispx11_private *disp = (struct sunxi_dispx11_private *)sunxi_disp;

	free(sunxi_disp);
}

static int sunxi_dispx11_set_video_layer(struct sunxi_disp *sunxi_disp, int x, int y, int width, int height, output_surface_ctx_t *surface)
{
	struct sunxi_dispx11_private *disp = (struct sunxi_dispx11_private *)sunxi_disp;

	switch (surface->vs->source_format) {
	case VDP_YCBCR_FORMAT_YUYV:
	        fprintf(stderr, "%s(%d): VDP_YCBCR_FORMAT_YUYV %dx%d\n", __func__, __LINE__, surface->vs->width, surface->vs->height);
		break;
	case VDP_YCBCR_FORMAT_UYVY:
	        fprintf(stderr, "%s(%d): VDP_YCBCR_FORMAT_UYVY %dx%d\n", __func__, __LINE__, surface->vs->width, surface->vs->height);
		break;
	case VDP_YCBCR_FORMAT_NV12:
	case VDP_YCBCR_FORMAT_YV12:
		{
		  static XShmSegmentInfo Shminfo;
		  const int lwidth = surface->vs->width, lheight = surface->vs->height;

		  if (disp->data[0] == NULL) {
		    XWindowAttributes attribs;
		    XImage *ximage;
		    fprintf(stderr, "%s(%d): VDP_YCBCR_FORMAT_NV12 %dx%d\n", __func__, __LINE__, surface->vs->width, surface->vs->height);
		    fprintf(stderr, "=NULL\n");
		    disp->shm = XShmQueryExtension(disp->display);
		    XGetWindowAttributes(disp->display, disp->drawable, &attribs);
		    disp->depth = attribs.depth;
            //XSetStandardProperties(disp->display, disp->drawable,"My Window","HI!",None,NULL,0,NULL);
		    disp->context = XCreateGC (disp->display, disp->drawable, 0, NULL);
		    if (disp->shm) {
		      	disp->ximage = ximage = XShmCreateImage(disp->display, NULL, disp->depth, ZPixmap, NULL, &Shminfo, lwidth, lheight);
		      	if (!disp->ximage) fprintf(stderr, "XShmCreateImage failed...\n");
		      	Shminfo.shmid = shmget(IPC_PRIVATE, ximage->bytes_per_line * ximage->height, IPC_CREAT | 0777);
		      	if (Shminfo.shmid < 0)	{ fprintf(stderr, "shmget failed, errno %d\n", errno); }
		      	Shminfo.shmaddr = (char *) shmat(Shminfo.shmid, 0, 0);
		      	if (Shminfo.shmaddr == ((char *) -1)) { fprintf(stderr, "shmat failed, errno %d\n", errno); }
		      	disp->image = ximage->data = Shminfo.shmaddr;
		      	Shminfo.readOnly = False;
		      	XShmAttach(disp->display, &Shminfo);
		      	XSync(disp->display, False);
		      	shmctl(Shminfo.shmid, IPC_RMID, 0);
		      	fprintf(stderr, "=SHM\n");
		    } else {
		      	disp->ximage = ximage = XCreateImage (disp->display, NULL, disp->depth, ZPixmap, 0, NULL, lwidth, lheight, 8, 0);
		      	disp->bytes_per_line = ximage->bytes_per_line;
		      	disp->image = malloc(disp->bytes_per_line * surface->vs->height);
		      	ximage->data = disp->image;
		      	XSync(disp->display, False);
		    }

		    switch (surface->rgba.format) {
			 case VDP_RGBA_FORMAT_R8G8B8A8:
				printf("RGBA\n");
				break;
			 case VDP_RGBA_FORMAT_B8G8R8A8:
			 default:
				printf("BGRA\n");
				break;
			}
		  }
		  
		  disp->data[0] = cedrus_mem_get_pointer(surface->yuv->data);
		  disp->data[1] = cedrus_mem_get_pointer(surface->yuv->data) + surface->vs->luma_size;
		  disp->data[2] = cedrus_mem_get_pointer(surface->yuv->data) + surface->vs->luma_size + surface->vs->chroma_size / 2;
#if 0
		  { // ffplay -f rawvideo -pix_fmt nv12 -video_size 1280x718 ""
		  	// ffplay -f rawvideo -pix_fmt yuv420p -video_size 1280x718 ""
		    char filename[32]; static int n = 0; sprintf(filename, "%08d.raw", n++);
		    if (!(n%50)) { FILE *video_dst_file = fopen(filename, "wb"); fwrite(disp->data[0], 1, surface->vs->width * surface->vs->height    , video_dst_file);
		      fwrite(disp->data[1], 1, surface->vs->width * surface->vs->height / 2, video_dst_file); fclose(video_dst_file); }
		  }
#endif /*0*/


		  {
#ifdef HAS_NEON
#ifdef USE_MEASUREMENT            
		    clock_gettime(CLOCK_MONOTONIC, &tm[0]);
#endif		    
		    if (surface->vs->source_format == VDP_YCBCR_FORMAT_YV12) {
			    I420ToRGBA( (unsigned char *)disp->data[0],
					    	(unsigned char *)disp->data[1],
					    	(unsigned char *)disp->data[2],
					     	lwidth, lheight, (int *)disp->image);
			} else {
				NV12ToRGBA( (unsigned char *)disp->data[0], 
							(unsigned char *)disp->data[1],
							lwidth, lheight, (int *)disp->image);
			}
#ifdef USE_MEASUREMENT
		    clock_gettime(CLOCK_MONOTONIC, &tm[1]);
		    int dt_ms = (time_diff(&tm[0], &tm[1])) / 1000000;
		    printf("Coversion time %d ms\n", dt_ms);
#endif            
#endif		    
            if (disp->shm) {
		      	XShmPutImage(disp->display, disp->drawable, disp->context, disp->ximage, 0, 0, 0, 0, lwidth, lheight, True);
		    	XFlush(disp->display);
		    } else {
		      	XPutImage(disp->display, disp->drawable, disp->context, disp->ximage, 0, 0, 0, 0, lwidth, lheight);
		    }	 
		    //fprintf(stderr, "=T6\n");
		    //(void) XCopyArea (disp->display, pixmap, NULL, disp->context, 0,0,   lwidth, lheight, 0, 0);
		  	       //XCopyArea (display,pixmap,win1,gr_context1,0,0,200,100,100,125); 
		  }
		}
		break;

	default:
	case INTERNAL_YCBCR_FORMAT:
	        fprintf(stderr, "%s(%d): INTERNAL_YCBCR_FORMAT %dx%d\n", __func__, __LINE__, surface->vs->width, surface->vs->height);
		break;
	}

	/*-----------------*/
#if 0
	fprintf(stderr, "%s:%d\n", __func__, __LINE__);
	disp_rect src = { .x = surface->video_src_rect.x0, .y = surface->video_src_rect.y0,
			  .width = surface->video_src_rect.x1 - surface->video_src_rect.x0,
			  .height = surface->video_src_rect.y1 - surface->video_src_rect.y0 };
	disp_rect scn = { .x = x + surface->video_dst_rect.x0, .y = y + surface->video_dst_rect.y0,
			  .width = surface->video_dst_rect.x1 - surface->video_dst_rect.x0,
			  .height = surface->video_dst_rect.y1 - surface->video_dst_rect.y0 };

	clip (&src, &scn, disp->screen_width);

	unsigned long args[4] = { 0, (unsigned long)(&disp->video_config), 1, 0 };
	switch (surface->vs->source_format)
	{
	case VDP_YCBCR_FORMAT_YUYV:
		disp->video_config.info.fb.format = DISP_FORMAT_YUV422_I_YUYV;
		break;
	case VDP_YCBCR_FORMAT_UYVY:
		disp->video_config.info.fb.format = DISP_FORMAT_YUV422_I_UYVY;
		break;
	case VDP_YCBCR_FORMAT_NV12:
	  fprintf(stderr, "%s:%d\n", __func__, __LINE__);
		disp->video_config.info.fb.format = DISP_FORMAT_YUV420_SP_UVUV;
		break;
	case VDP_YCBCR_FORMAT_YV12:
	default:
	case INTERNAL_YCBCR_FORMAT:
		disp->video_config.info.fb.format = DISP_FORMAT_YUV420_P;
		break;
	}

	fprintf(stderr, "%s:%d\n", __func__, __LINE__);
	disp->video_config.info.fb.addr[0] = cedarv_virt2phys(surface->vs->dataY);
	disp->video_config.info.fb.addr[1] = cedarv_virt2phys(surface->vs->dataU) /*+ surface->vs->luma_size*/;
        if( cedarv_isValid(surface->vs->dataV))
	  disp->video_config.info.fb.addr[2] = cedarv_virt2phys(surface->vs->dataV) /*+ surface->vs->luma_size + surface->vs->chroma_size / 2*/;

	fprintf(stderr, "%s:%d\n", __func__, __LINE__);
	disp->video_config.info.fb.size[0].width = surface->vs->width;
	disp->video_config.info.fb.size[0].height = surface->vs->height;
	disp->video_config.info.fb.align[0] = 32;
	disp->video_config.info.fb.size[1].width = surface->vs->width / 2;
	disp->video_config.info.fb.size[1].height = surface->vs->height / 2;
	disp->video_config.info.fb.align[1] = 16;
	disp->video_config.info.fb.size[2].width = surface->vs->width / 2;
	disp->video_config.info.fb.size[2].height = surface->vs->height / 2;
	disp->video_config.info.fb.align[2] = 16;
	disp->video_config.info.fb.crop.x = (unsigned long long)(src.x) << 32;
	disp->video_config.info.fb.crop.y = (unsigned long long)(src.y) << 32;
	disp->video_config.info.fb.crop.width = (unsigned long long)(src.width) << 32;
	disp->video_config.info.fb.crop.height = (unsigned long long)(src.height) << 32;
	disp->video_config.info.screen_win = scn;
	disp->video_config.enable = 1;

	fprintf(stderr, "%s:%d\n", __func__, __LINE__);
	if (ioctl(disp->fd, DISP_LAYER_SET_CONFIG, args))
		return -EINVAL;

	fprintf(stderr, "%s:%d\n", __func__, __LINE__);
#endif
	return 0;
}

static void sunxi_dispx11_close_video_layer(struct sunxi_disp *sunxi_disp)
{
  //struct sunxi_dispx11_private *disp = (struct sunxi_dispx11_private *)sunxi_disp;
}

static int sunxi_dispx11_set_osd_layer(struct sunxi_disp *sunxi_disp, int x, int y, int width, int height, output_surface_ctx_t *surface)
{
  //struct sunxi_dispx11_private *disp = (struct sunxi_dispx11_private *)sunxi_disp;


	/*	switch (surface->rgba.format)
	{
	case VDP_RGBA_FORMAT_R8G8B8A8:
		disp->osd_config.info.fb.format = DISP_FORMAT_ABGR_8888;
		break;
	case VDP_RGBA_FORMAT_B8G8R8A8:
	default:
		disp->osd_config.info.fb.format = DISP_FORMAT_ARGB_8888;
		break;
	}

	disp->osd_config.info.fb.addr[0] = cedarv_virt2phys(surface->rgba.data);
	disp->osd_config.info.fb.size[0].width = surface->rgba.width;
	disp->osd_config.info.fb.size[0].height = surface->rgba.height;
	disp->osd_config.info.fb.align[0] = 1;
	disp->osd_config.info.fb.crop.x = (unsigned long long)(src.x) << 32;
	disp->osd_config.info.fb.crop.y = (unsigned long long)(src.y) << 32;
	disp->osd_config.info.fb.crop.width = (unsigned long long)(src.width) << 32;
	disp->osd_config.info.fb.crop.height = (unsigned long long)(src.height) << 32;
	disp->osd_config.info.screen_win = scn;
	disp->osd_config.enable = 1;

	if (ioctl(disp->fd, DISP_LAYER_SET_CONFIG, args))
	return -EINVAL;*/

	return 0;
}

static void sunxi_dispx11_close_osd_layer(struct sunxi_disp *sunxi_disp)
{
  //struct sunxi_dispx11_private *disp = (struct sunxi_dispx11_private *)sunxi_disp;
}

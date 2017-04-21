/*

* rockchip hwcomposer( 2D graphic acceleration unit) .

*

* Copyright (C) 2015 Rockchip Electronics Co., Ltd.

*/

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "version.h"
#include "rk_hwcomposer.h"
#include "rk_hwcomposer_api.h"
#include <hardware/hardware.h>

#include <utils/String8.h>

#include <sys/prctl.h>
#include <stdlib.h>
#include <errno.h>
#include <cutils/properties.h>
#include <fcntl.h>
#include <sync/sync.h>
#include <time.h>
#include <poll.h>
#include "rk_hwcomposer_hdmi.h"
#include <ui/PixelFormat.h>
#include <sys/stat.h>
//#include "hwc_ipp.h"
#include "hwc_rga.h"
#include <linux/ion.h>
#include <ion/ion.h>
#include <utils/Trace.h>

#include <system/graphics_extended.h>

/*---------------------------------------------------------------------------*/

#include <sys/mman.h>

#define HWC_TOWIN0  1001
#define HWC_TOWIN1  1002
#define HWC_LCDC    1003
#define HWC_NODRAW  1004
#define HWC_MIX     1005
#define HWC_MIX_V2  1006

//primary,hotplug and virtual device context
static hwcContext * _contextAnchor = NULL;
static hwcContext * _contextAnchor1 = NULL;
static hwcContext * _contextAnchor2 = NULL;
//#define ENABLE_HDMI_APP_LANDSCAP_TO_PORTRAIT
#undef LOGV
#define LOGV(...)
static int crcTable[256];
bool hdmi_noready = true;
static mix_info gmixinfo[2];
static int SkipFrameCount = 0;
//#if  (ENABLE_TRANSFORM_BY_RGA | ENABLE_LCDC_IN_NV12_TRANSFORM)
static hwbkupmanage bkupmanage;
//#endif
#ifdef ENABLE_HDMI_APP_LANDSCAP_TO_PORTRAIT
static int bootanimFinish = 0;
#endif

int gwin_tab[MaxZones] = {win0,win1,win2_0,win2_1,win2_2,win2_3,win3_0,win3_1,win3_2,win3_3};


static int      hwc_blank(struct hwc_composer_device_1 *dev, int dpy,int blank);
static int      hwc_query(struct hwc_composer_device_1* dev,int what,int* value);
static int      hwc_event_control(struct hwc_composer_device_1* dev,int dpy,int event,int enabled);
static int      hwc_prepare(hwc_composer_device_1_t * dev,size_t numDisplays,hwc_display_contents_1_t** displays);
static int      hwc_set(hwc_composer_device_1_t * dev,size_t numDisplays,hwc_display_contents_1_t  ** displays);
static int      hwc_device_close(struct hw_device_t * dev);
static int      hwc_alloc_buffer(buffer_handle_t *hnd, int w,int h,int *s,int fmt,int usage);
static int      hwc_free_buffer(buffer_handle_t hnd);


int             hwc_sprite_replace(hwcContext * Context, hwc_display_contents_1_t * list);
int             hwc_add_rga_blit_fbinfo(hwcContext * ctx, struct hwc_fb_info *hfi);
int             hwc_rga_fix_zones_for_yuv_ten_bit(ZoneManager* pZones);
int             hwc_free_rga_blit_buffers(hwcContext * context);
int             hwc_alloc_rga_blit_buffers(hwcContext * context);
int             hwc_rga_blit_alloc_rects(hwcContext * context);
int             hwc_rga_blit_free_rects(hwcContext * context);
void*           hwc_control_3dmode_thread(void *arg);

buffer_handle_t hwc_rga_blit_get_current_buffer(hwcContext * context);
buffer_handle_t hwc_rga_blit_get_next_buffer(hwcContext * context);

void*   hotplug_try_register(void *arg);
void    hotplug_get_resolution(int* w,int* h);
int     hotplug_set_config();
int     hotplug_parse_mode(int *outX,int *outY);
int     hwc_parse_screen_info(int *outX, int *outY);
int     hotplug_get_config(int flag);
int     hotplug_set_overscan(int flag);
int     hotplug_reset_dstposition(struct rk_fb_win_cfg_data * fb_info,int flag);
int     hotplug_set_frame(hwcContext * context,int flag);
bool    hotplug_free_dimbuffer();
void*   hotplug_invalidate_refresh(void *arg);
bool    hwcPrimaryToExternalCheckConfig(hwcContext * ctx,struct rk_fb_win_cfg_data fb_info);

static unsigned int     createCrc32(unsigned int crc,unsigned const char *buffer,unsigned int size);
static void             initCrcTable(void);
static int              fence_merge(char * value, int fd1, int fd2);
static int              dual_view_vop_config(struct rk_fb_win_cfg_data * fbinfo);
static int              mipi_dual_vop_config(hwcContext *ctx, struct rk_fb_win_cfg_data * fbinfo);

/**
 * 返回 rk_vop 是否支持输出指定的 hal_pixel_format.
 */
static bool is_hal_format_supported_by_vop(int hal_format)
{
    if ( HAL_PIXEL_FORMAT_YV12 == hal_format
        || HAL_PIXEL_FORMAT_YCbCr_420_888 == hal_format )
    {
        return false;
    }

    return true;
}

/*-------------------------------------------------------*/

#ifdef USE_AFBC_LAYER
inline static bool isAfbcInternalFormat(uint64_t internal_format)
{
    return (internal_format & GRALLOC_ARM_INTFMT_AFBC);
}
#endif

/*---------------------------------------------------------------------------*/

/**
 * 从 hal_pixel_format 得到对应的 rk_vop_pixel_format.
 *
 * .trick : 因为历史原因, userspace(graphics.h) 和 vop_driver 中对 rk_ext_hal_pixel_format 定义的常数标识符 value 不同.
 *          参见 kernel/include/linux/rk_fb.h.
 */
static int hwChangeFormatandroidL(IN int fmt)
{
	switch (fmt)
	{
		case HAL_PIXEL_FORMAT_YCrCb_NV12:	/* YUV420---uvuvuv */
			return 0x20;                   /*android4.4 HAL_PIXEL_FORMAT_YCrCb_NV12 is 0x20*/
		case HAL_PIXEL_FORMAT_YCrCb_NV12_10:	/* yuv444 */
			return 0x22;				        /*android4.4 HAL_PIXEL_FORMAT_YCrCb_NV12_10 is 0x20*/
        case HAL_PIXEL_FORMAT_YCbCr_422_SP_10:
            return 0x23;
        case HAL_PIXEL_FORMAT_YCrCb_420_SP_10:
            return 0x24;
        case HAL_PIXEL_FORMAT_YCbCr_420_888:    // 0x23.
            return -1;
#ifdef GPU_G6110
		case HAL_PIXEL_FORMAT_BGRX_8888:
		    return HAL_PIXEL_FORMAT_RGBA_8888;
#endif
		default:
			return fmt;
	}
}

static int hwcGetBytePerPixelFromAndroidFromat(int fmt)
{
    switch (fmt) {
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_RGBX_8888:
        case HAL_PIXEL_FORMAT_BGRA_8888:
            return 4;
        case HAL_PIXEL_FORMAT_RGB_888:
            return 3;
        case HAL_PIXEL_FORMAT_RGB_565:
        case HAL_PIXEL_FORMAT_YCbCr_422_SP:
        case HAL_PIXEL_FORMAT_YCrCb_420_SP:
        case HAL_PIXEL_FORMAT_YCbCr_422_I:
        case HAL_PIXEL_FORMAT_YCrCb_NV12:
        case HAL_PIXEL_FORMAT_YCrCb_NV12_VIDEO:
        case HAL_PIXEL_FORMAT_YCrCb_NV12_10:
            return 2;
    }
    return 0;
}

static int
hwc_device_open(
    const struct hw_module_t * module,
    const char * name,
    struct hw_device_t ** device
    );

static struct hw_module_methods_t hwc_module_methods =
{
    open: hwc_device_open
};

hwc_module_t HAL_MODULE_INFO_SYM =
{
    common:
    {
        tag:           HARDWARE_MODULE_TAG,
        version_major: 1,
        version_minor: 2,
        id:            HWC_HARDWARE_MODULE_ID,
        name:          "Hardware Composer Module",
        author:        "Rockchip Corporation",
        methods:       &hwc_module_methods,
        dso:           NULL,
        reserved:      {0, }
    }
};

int _HasAlpha(RgaSURF_FORMAT Format)
{
    return (Format == RK_FORMAT_RGB_565) ? false
          : (
                (Format == RK_FORMAT_RGBA_8888) ||
                (Format == RK_FORMAT_BGRA_8888)
            );
}

#if G6110_SUPPORT_FBDC
int HALPixelFormatGetCompression(int iFormat)
{
	/* Extension format. Return only the compression bits. */
	if (iFormat >= 0x100 && iFormat <= 0x1FF)
		return (iFormat & 0x70) >> 4;

	/* Upstream formats are not compressible unless they are redefined as
	 * extension formats (e.g. RGB_565, BGRA_8888).
	 */
	return HAL_FB_COMPRESSION_NONE;
}

int HALPixelFormatGetRawFormat(int iFormat)
{
	/* If the format is within the "vendor format" range, just mask out the
	 * compression bits.
	 */
	if (iFormat >= 0x100 && iFormat <= 0x1FF)
	{
		switch (iFormat & 0xF)
		{
			/* These formats will be *rendered* by the GPU and we want to
			 * support compression, but they are "upstream" formats too, so
			 * remap them.
			 */
			case HAL_PIXEL_FORMAT_RGB_565:
				return HAL_PIXEL_FORMAT_RGB_565;
			case HAL_PIXEL_FORMAT_BGRA_8888:
				return HAL_PIXEL_FORMAT_BGRA_8888;
		    case HAL_PIXEL_FORMAT_RGBA_8888:
				return HAL_PIXEL_FORMAT_RGBA_8888;
			/* Vendor format */
			default:
				return iFormat & ~0xF0;
		}
	}

	/* Upstream format */
	return iFormat;
}

int HALPixelFormatSetCompression(int iFormat, int iCompression)
{
	/* We can support compressing some "upstream" formats. If the compression
	 * is not disabled, convert the formats to our extension formats.
	 */
	if (iCompression != HAL_FB_COMPRESSION_NONE)
	{
		switch (iFormat)
		{
			case HAL_PIXEL_FORMAT_RGB_565:
			case HAL_PIXEL_FORMAT_BGRA_8888:
			case HAL_PIXEL_FORMAT_RGBA_8888:
				iFormat |= 0x100;
				break;
		}
	}

	/* Can only set compression on extension formats */
	if (iFormat < 0x100 || iFormat > 0x1FF)
		return iFormat;

	/* Clear any existing compression bits */
	iFormat &= ~0x70;

	/* Mask out invalid compression formats */
	switch (iCompression)
	{
		case HAL_FB_COMPRESSION_NONE:
		case HAL_FB_COMPRESSION_DIRECT_8x8:
		case HAL_FB_COMPRESSION_DIRECT_16x4:
		case HAL_FB_COMPRESSION_DIRECT_32x2:
		case HAL_FB_COMPRESSION_INDIRECT_8x8:
		case HAL_FB_COMPRESSION_INDIRECT_16x4:
		case HAL_FB_COMPRESSION_INDIRECT_4TILE_8x8:
		case HAL_FB_COMPRESSION_INDIRECT_4TILE_16x4:
			return iFormat | (iCompression << 4);
		default:
			return iFormat;
	}
}

#endif

static void hwc_list_nodraw(hwc_display_contents_1_t  *list)
{
    if (list == NULL)
    {
        return;
    }
    for (unsigned int i = 0; i < list->numHwLayers - 1; i++)
    {
        list->hwLayers[i].compositionType = HWC_NODRAW;
    }
    return;
}

static int hwc_init_version()
{
    char acVersion[100];
    memset(acVersion,0,sizeof(acVersion));
    if (sizeof(GHWC_VERSION) > 12) {
        strncpy(acVersion,GHWC_VERSION,12);
    } else {
        strcpy(acVersion,GHWC_VERSION);
    }
#ifdef TARGET_BOARD_PLATFORM_RK3288
    strcat(acVersion,"-3288");
#endif
#ifdef TARGET_BOARD_PLATFORM_RK3368
    strcat(acVersion,"-3368");
#endif
#ifdef TARGET_BOARD_PLATFORM_RK3366
	strcat(acVersion,"-3366");
#endif
#ifdef TARGET_BOARD_PLATFORM_RK3399
	strcat(acVersion,"-3399");
#endif

#ifdef RK_MID
    strcat(acVersion,"-MID");
#endif
#ifdef RK_BOX
    strcat(acVersion,"-BOX");
#endif
#ifdef RK_PHONE
    strcat(acVersion,"-PHONE");
#endif
#ifdef RK_VR
    strcat(acVersion,"-VR");
#endif

    strcat(acVersion,"-");
    strcat(acVersion,RK_GRAPHICS_VER);
    property_set("sys.ghwc.version", acVersion);
    ALOGD(RK_GRAPHICS_VER);
    return 0;
}

static int init_log_level()
{
    hwcContext * ctxp = _contextAnchor;
    char value[PROPERTY_VALUE_MAX];
    property_get("sys.hwc.log", value, "0");
    ctxp->mLogL = atoi(value);
    return 0;
}

static bool log(int in)
{
    hwcContext * ctxp = _contextAnchor;
    return ctxp->mLogL & in;
}

//return property value of pcProperty
static int hwc_get_int_property(const char* pcProperty,const char* default_value)
{
    char value[PROPERTY_VALUE_MAX];
    int new_value = 0;

    if(pcProperty == NULL || default_value == NULL)
    {
        ALOGE("hwc_get_int_property: invalid param");
        return -1;
    }

    property_get(pcProperty, value, default_value);
    new_value = atoi(value);

    return new_value;
}

static int hwc_get_string_property(const char* pcProperty,const char* default_value,char* retult)
{
    if(pcProperty == NULL || default_value == NULL || retult == NULL)
    {
        ALOGE("hwc_get_string_property: invalid param");
        return -1;
    }

    property_get(pcProperty, retult, default_value);

    return 0;
}

static int LayerZoneCheck( hwc_layer_1_t * Layer , int disp)
{
	hwcContext * context = NULL;
#if HWC_EXTERNAL
	switch (disp){
		case HWC_DISPLAY_PRIMARY:
			context = _contextAnchor;
			break;
		case HWC_DISPLAY_EXTERNAL:
			context = _contextAnchor1;
			break;
		default:
			context = _contextAnchor;
	}
#else
	context = _contextAnchor;
#endif
    hwc_region_t * Region = &(Layer->visibleRegionScreen);
    hwc_rect_t const * rects = Region->rects;
    int i;
    for (i = 0; i < (int) Region->numRects ;i++)
    {
        if(rects[i].left < 0 || rects[i].top < 0
           || rects[i].right > context->fbhandle.width
           || rects[i].bottom > context->fbhandle.height)
        {
            if (context && context->mIsMipiDualOutMode)
                continue;
            if (!(context == _contextAnchor1 && context->mIsDualViewMode)) {
                ALOGW("LayerZoneCheck out line at %d",__LINE__);
                return -1;
            }
        }
    }

    return 0;
}

static void hwc_sync(hwc_display_contents_1_t  *list)
{
	bool forceSkip = false;
	if (list == NULL) {
		return ;
	}

#ifndef RK_VR
	forceSkip = false;
	for (int i=0; i< (int)list->numHwLayers; i++){
		if (list->hwLayers[i].acquireFenceFd>0){
			sync_wait(list->hwLayers[i].acquireFenceFd,3001);  // add 40ms timeout
		}
	}
#else
	for (int i=0; i< (int)list->numHwLayers; i++)
	{
		hwc_layer_1_t* layer = &list->hwLayers[i];
		if (layer == NULL)
			continue;

		struct private_handle_t * hnd = (struct private_handle_t *)layer->handle;
		if(hnd && (hnd->usage & 0x08000000))
		{
			forceSkip = true;
		}
		if (layer->acquireFenceFd > 0 && !forceSkip) {
			sync_wait(layer->acquireFenceFd,3001);  // add 40ms timeout
		}
	}
#endif

}

#if 0
int rga_video_reset()
{
    if (_contextAnchor->video_hd || _contextAnchor->video_base)
    {
        ALOGV(" rga_video_reset,%x",_contextAnchor->video_hd);
        _contextAnchor->video_hd = 0;
        _contextAnchor->video_base =0;
    }

    return 0;
}
#endif

static void hwc_sync_release(hwc_display_contents_1_t  *list)
{
	for (int i=0; i< (int)list->numHwLayers; i++){
		hwc_layer_1_t* layer = &list->hwLayers[i];
		if (layer == NULL){
			return ;
		}
		if (layer->acquireFenceFd>0){
			close(layer->acquireFenceFd);
			list->hwLayers[i].acquireFenceFd = -1;
		}
	}

	if (list->outbufAcquireFenceFd>0){
		ALOGV(">>>close outbufAcquireFenceFd:%d",list->outbufAcquireFenceFd);
		close(list->outbufAcquireFenceFd);
		list->outbufAcquireFenceFd = -1;
	}

}

static int hwc_single_buffer_close_rel_fence(hwc_display_contents_1_t  *list)
{
	hwcContext * ctxp = _contextAnchor;
	if (ctxp && !ctxp->isVr)
		ALOGE("Compile error for this platform,not vr");

	if (!list)
		return -EINVAL;

	for (int i=0; i< (int)list->numHwLayers; i++)
	{
		hwc_layer_1_t* layer = &list->hwLayers[i];
		if (layer == NULL)
			continue;

		struct private_handle_t * hnd = (struct private_handle_t *) layer->handle;
		if (hnd == NULL)
			continue;

		if (layer->releaseFenceFd > -1 && (hnd->usage & 0x08000000))
		{
			close(layer->releaseFenceFd);
			layer->releaseFenceFd = -1;
		}
	}

	return 0;
}

static int rgaRotateScale(hwcContext * ctx,int tranform,int fd_dst, int Dstfmt,bool isTry)
{
    ATRACE_CALL();
    struct rga_req  Rga_Request;
    RECT clip;
    unsigned char RotateMode = 0;
    int Rotation = 0;
    int SrcVirW,SrcVirH,SrcActW,SrcActH;
    int DstVirW,DstVirH,DstActW,DstActH;
    int xoffset = 0;
    int yoffset = 0;
    int rga_fd = _contextAnchor->engine_fd;
    void *cpu_ptr = MAP_FAILED;
    void *baseAddr = NULL;
    hwcContext * context = ctx;
    struct private_handle_t *handle = context->mRgaTBI.hdl;
    hwc_rect_t * psrc_rect = &context->mRgaTBI.zone_info.src_rect;
    hwc_rect_t * pdst_rect = &context->mRgaTBI.zone_info.disp_rect;
    int src_stride = context->mRgaTBI.zone_info.stride;
    int src_width = context->mRgaTBI.zone_info.width;
    int src_height = context->mRgaTBI.zone_info.height;

    int index_v = context->mCurVideoIndex%MaxVideoBackBuffers;
    if (rga_fd < 0) {
       return -1;
    }

    if(context->base_video_bk[index_v] == 0) {
        ALOGE("It hasn't direct_base for dst buffer");
        return -1;
    }

    Dstfmt = RK_FORMAT_YCbCr_420_SP;
    //pthread_mutex_lock(&_contextAnchor->lock);
    memset(&Rga_Request, 0x0, sizeof(Rga_Request));

    switch (tranform){
        case HWC_TRANSFORM_ROT_90:
            RotateMode      = 1;
            Rotation    = 90;
            SrcVirW = src_stride;
            SrcVirH = src_height;
            SrcActW = psrc_rect->bottom - psrc_rect->top;
            SrcActH = psrc_rect->right - psrc_rect->left;
            DstVirW = rkmALIGN(pdst_rect->right - pdst_rect->left,32);
            DstVirH = pdst_rect->bottom - pdst_rect->top;
            DstActW = pdst_rect->bottom - pdst_rect->top;
            DstActH = pdst_rect->right - pdst_rect->left;

            DstVirH -= DstVirH % 2;
            DstActW -= DstActW % 2;
            DstActH -= DstActH % 2;

            xoffset = DstActH - 1;
            yoffset = 0;

            break;

        case HWC_TRANSFORM_ROT_180:
            RotateMode      = 1;
            Rotation    = 180;
            SrcVirW = src_stride;
            SrcVirH = src_height;
            SrcActW = psrc_rect->right - psrc_rect->left;
            SrcActH = psrc_rect->bottom - psrc_rect->top;
            DstVirW = rkmALIGN(pdst_rect->right - pdst_rect->left,32);
            DstVirH = pdst_rect->bottom - pdst_rect->top;
            DstActW = pdst_rect->right - pdst_rect->left;
            DstActH = pdst_rect->bottom - pdst_rect->top;

            DstVirH -= DstVirH % 2;
            DstActW -= DstActW % 2;
            DstActH -= DstActH % 2;

            xoffset = DstActW - 1;
            yoffset = DstActH - 1;
            clip.xmin = 0;
            clip.xmax =  handle->width - 1;
            clip.ymin = 0;
            clip.ymax = handle->height - 1;

            break;

        case HWC_TRANSFORM_ROT_270:
            RotateMode      = 1;
            Rotation        = 270;
            SrcVirW = src_stride;
            SrcVirH = src_height;
            SrcActW = psrc_rect->bottom - psrc_rect->top;
            SrcActH = psrc_rect->right - psrc_rect->left;
            DstVirW = rkmALIGN(pdst_rect->right - pdst_rect->left,32);
            DstVirH = pdst_rect->bottom - pdst_rect->top;
            DstActW = pdst_rect->bottom - pdst_rect->top;
            DstActH = pdst_rect->right - pdst_rect->left;

            DstVirH -= DstVirH % 2;
            DstActW -= DstActW % 2;
            DstActH -= DstActH % 2;

            xoffset = 0;
            yoffset = DstActW - 1;
            break;
        case 0:
        default:
        {
            SrcVirW = src_stride;
            SrcVirH = src_height;
            SrcActW = psrc_rect->right - psrc_rect->left;
            SrcActH = psrc_rect->bottom - psrc_rect->top;
            DstVirW = rkmALIGN(pdst_rect->right - pdst_rect->left,32);
            DstVirH = pdst_rect->bottom - pdst_rect->top;
            DstActW = pdst_rect->right - pdst_rect->left;
            DstActH = pdst_rect->bottom - pdst_rect->top;

            DstVirH -= DstVirH % 2;
            DstActW -= DstActW % 2;
            DstActH -= DstActH % 2;

            xoffset = 0;
            yoffset = 0;

            clip.xmin = 0;
            clip.xmax = DstActW - 1;
            clip.ymin = 0;
            clip.ymax = DstActH - 1;
            break;
        }
    }
    int size = hwcGetBufferSizeForRga(DstActW,DstActH,Dstfmt) ;
    if(size > RLAGESIZE){
        ALOGD_IF(log(HLLSEV),"now size=%d,largesize=%d,w_h_f[%d,%d,%d]",size,RLAGESIZE,DstActW,DstActH,Dstfmt);
        return -1;
    }
    if(DstVirW > 4096 || DstVirH > 4096 || DstActW > 4096 || DstActH > 4096)
        return -1;

    if(isTry)
        return 0;

    ALOGD_IF(log(HLLSIX),"src addr=[%x],w-h[%d,%d],act[%d,%d][f=%d]",
        handle->share_fd, SrcVirW, SrcVirH,SrcActW,SrcActH,hwChangeRgaFormat(handle->format));
    ALOGD_IF(log(HLLSIX),"dst fd=[%x],Index=%d,w-h[%d,%d],act[%d,%d][%d,%d][f=%d],rot=%d,rot_mod=%d",
        fd_dst, index_v, DstVirW, DstVirH,DstActW,DstActH,xoffset,yoffset,Dstfmt,Rotation,RotateMode);

    int fd_dup = -1;
    if (handle->share_fd > -1) {
	    fd_dup = dup(handle->share_fd);
	    if (fd_dup == -1)
		    ALOGE("dup fd fail %s",strerror(errno));
        else
            cpu_ptr = mmap(NULL, handle->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_dup, 0);
    }

    if (cpu_ptr != MAP_FAILED)
        baseAddr = (void *)cpu_ptr;
    else
        baseAddr = (void *)GPU_BASE;

    if(handle->type == 1)
        RGA_set_src_vir_info(&Rga_Request, 0, (unsigned long)(GPU_BASE), 0,SrcVirW, SrcVirH, hwChangeRgaFormat(handle->format), 0);
    else
        RGA_set_src_vir_info(&Rga_Request, handle->share_fd, 0, 0,SrcVirW, SrcVirH, hwChangeRgaFormat(handle->format), 0);

    RGA_set_dst_vir_info(&Rga_Request, fd_dst, 0, 0,DstVirW,DstVirH,&clip, Dstfmt, 0);
    RGA_set_bitblt_mode(&Rga_Request, 0, RotateMode,Rotation,0,0,0);
    RGA_set_src_act_info(&Rga_Request,SrcActW,SrcActH, 0,0);
    RGA_set_dst_act_info(&Rga_Request,DstActW,DstActH, xoffset,yoffset);

//debug: clear source data
#if 0
    memset((void*)handle->base,0x0,SrcVirW*SrcVirH*3/2);
#endif

    if(handle->type == 1) {
        RGA_set_dst_vir_info(&Rga_Request, 0,context->base_video_bk[index_v], 0,DstVirW,DstVirH,&clip, Dstfmt, 0);
        RGA_set_mmu_info(&Rga_Request, 1, 0, 0, 0, 0, 2);
        Rga_Request.mmu_info.mmu_flag |= (1<<31) | (1<<10) | (1<<8);
    }

    if(context->relFenceFd[index_v] > 0) {
        ALOGD_IF(log(HLLSIX),"Goto dst sync wait %d",context->relFenceFd[index_v]);
        sync_wait(context->relFenceFd[index_v],-1);
        close(context->relFenceFd[index_v]);
        context->relFenceFd[index_v] = -1;
        ALOGD_IF(log(HLLSIX),"Goout dst sync wait %d",context->relFenceFd[index_v]);
    }

    if(ioctl(rga_fd, RGA_BLIT_SYNC, &Rga_Request)) {
        LOGE(" %s(%d) RGA_BLIT fail",__FUNCTION__, __LINE__);
        ALOGE("src addr=[%x],w-h[%d,%d],act[%d,%d][f=%d]",
            handle->share_fd, SrcVirW, SrcVirH,SrcActW,SrcActH,hwChangeRgaFormat(handle->format));
        ALOGE("dst fd=[%x],Index=%d,w-h[%d,%d],act[%d,%d][%d,%d][f=%d],rot=%d,rot_mod=%d",
            fd_dst, index_v, DstVirW, DstVirH,DstActW,DstActH,xoffset,yoffset,Dstfmt,Rotation,RotateMode);
    }

    if (fd_dup > -1) {
	    close(fd_dup);
	    if (cpu_ptr != MAP_FAILED)
	        munmap(cpu_ptr, handle->size);
    }

    return 0;
}

static int rga_video_copybit(struct private_handle_t *handle,int tranform,int w_valid,int h_valid,int fd_dst, int Dstfmt,int specialwin,int dpyID,bool isTry)
{
    ATRACE_CALL();
    struct rga_req  Rga_Request;
    RECT clip;
    unsigned char RotateMode = 0;
    void *cpu_ptr = MAP_FAILED;
    int Rotation = 0;
    int SrcVirW,SrcVirH,SrcActW,SrcActH;
    int DstVirW,DstVirH,DstActW,DstActH;
    int xoffset = 0;
    int yoffset = 0;
    int rga_fd = _contextAnchor->engine_fd;
    void *baseAddr = NULL;

    hwcContext * context = _contextAnchor;

    if(dpyID == HWCE){
        context = _contextAnchor1;
    }

    int index_v = context->mCurVideoIndex%MaxVideoBackBuffers;

    if (rga_fd < 0 || !handle) {
       return -1;
    }

    if (_contextAnchor->video_fmt != HAL_PIXEL_FORMAT_YCrCb_NV12 && !specialwin) {
        return -1;
    }

    if(specialwin && context->base_video_bk[index_v] == 0) {
        ALOGE("It hasn't direct_base for dst buffer");
        return -1;
    }

    if ((handle->video_width <= 0 || handle->video_height <= 0 ||
       handle->video_width >= 8192 || handle->video_height >= 4096 )&&!specialwin) {
        ALOGE("rga invalid w_h[%d,%d]",handle->video_width , handle->video_height);
        return -1;
    }

    //pthread_mutex_lock(&_contextAnchor->lock);
    memset(&Rga_Request, 0x0, sizeof(Rga_Request));
    clip.xmin = 0;
    clip.xmax = handle->height - 1;
    clip.ymin = 0;
    clip.ymax = handle->width - 1;
    switch (tranform){
        case HWC_TRANSFORM_ROT_90:
            RotateMode      = 1;
            Rotation    = 90;
            SrcVirW = specialwin ? handle->stride:handle->video_width;
            SrcVirH = specialwin ? handle->height:handle->video_height;
            SrcActW = w_valid;
            SrcActH = h_valid;
            DstVirW = rkmALIGN(h_valid,8);
            DstVirH = w_valid;
            DstActW = w_valid;
            DstActH = h_valid;
            xoffset = h_valid -1;
            yoffset = 0;

            break;

        case HWC_TRANSFORM_ROT_180:
            RotateMode      = 1;
            Rotation    = 180;
            SrcVirW = specialwin ? handle->stride:handle->video_width;
            SrcVirH = specialwin ? handle->height:handle->video_height;
            SrcActW = w_valid;
            SrcActH = h_valid;
            DstVirW = w_valid;
            DstVirH = h_valid;
            DstActW = w_valid;
            DstActH = h_valid;
            clip.xmin = 0;
            clip.xmax =  handle->width - 1;
            clip.ymin = 0;
            clip.ymax = handle->height - 1;
            xoffset = w_valid -1;
            yoffset = h_valid -1;

            break;

        case HWC_TRANSFORM_ROT_270:
            RotateMode      = 1;
            Rotation        = 270;
            SrcVirW = specialwin ? handle->stride:handle->video_width;
            SrcVirH = specialwin ? handle->height:handle->video_height;
            SrcActW = w_valid;
            SrcActH = h_valid;
            DstVirW = rkmALIGN(h_valid,8);
            DstVirH = w_valid;
            DstActW = w_valid;
            DstActH = h_valid;
            xoffset = 0;
            yoffset = w_valid -1;
            break;
        case 0:
        default:
        {
            char property[PROPERTY_VALUE_MAX];
            int fmtflag = 0;
			if (property_get("sys.yuv.rgb.format", property, NULL) > 0) {
				fmtflag = atoi(property);
			}
			//if(fmtflag == 1)
               // Dstfmt = RK_FORMAT_RGB_565;
			//else
               // Dstfmt = RK_FORMAT_RGBA_8888;
            SrcVirW = handle->video_width;
            SrcVirH = handle->video_height;
            SrcActW = handle->video_disp_width;
            SrcActH = handle->video_disp_height;
            DstVirW = handle->width;
            DstVirH = handle->height;
            DstActW = handle->video_disp_width;
            DstActH = handle->video_disp_height;
            clip.xmin = 0;
            clip.xmax =  handle->width - 1;
            clip.ymin = 0;
            clip.ymax = handle->height - 1;
            Dstfmt = RK_FORMAT_YCbCr_420_SP;
            break;
        }
    }
    int size = hwcGetBufferSizeForRga(DstActW,DstActH,Dstfmt) ;
    if(size > RLAGESIZE){
        ALOGD_IF(log(HLLSEV),"now size=%d,largesize=%d,w_h_f[%d,%d,%d]",size,RLAGESIZE,DstActW,DstActH,Dstfmt);
        return -1;
    }
    if(DstVirW > 4096 || DstVirH > 4096 || DstActW > 4096 || DstActH > 4096)
        return -1;
    if(isTry)
        return 0;
    ALOGD_IF(log(HLLSIX),"src addr=[%x],w-h[%d,%d],act[%d,%d][f=%d]",
        specialwin ? handle->share_fd:handle->video_addr, SrcVirW, SrcVirH,SrcActW,SrcActH,specialwin ?  hwChangeRgaFormat(handle->format):RK_FORMAT_YCbCr_420_SP);
    ALOGD_IF(log(HLLSIX),"dst fd=[%x],Index=%d,w-h[%d,%d],act[%d,%d][f=%d],rot=%d,rot_mod=%d",
        fd_dst, index_v, DstVirW, DstVirH,DstActW,DstActH,Dstfmt,Rotation,RotateMode);

    int fd_dup = -1;
    if (handle->share_fd > -1) {
	    fd_dup = dup(handle->share_fd);
	    if (fd_dup == -1)
		    ALOGE("dup fd fail %s",strerror(errno));
        else
            cpu_ptr = mmap(NULL, handle->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_dup, 0);
    }

    if (cpu_ptr != MAP_FAILED)
        baseAddr = (void *)cpu_ptr;
    else
        baseAddr = (void *)GPU_BASE;

    if(specialwin)
    {
        if(handle->type == 1)
            RGA_set_src_vir_info(&Rga_Request, 0, (unsigned long)(baseAddr), (unsigned long)(baseAddr) + SrcVirW * SrcVirH,SrcVirW, SrcVirH, hwChangeRgaFormat(handle->format), 0);
        else
            RGA_set_src_vir_info(&Rga_Request, handle->share_fd, 0, 0,SrcVirW, SrcVirH, hwChangeRgaFormat(handle->format), 0);
    }
    else
        RGA_set_src_vir_info(&Rga_Request, 0, handle->video_addr, handle->video_addr + SrcVirW * SrcVirH,SrcVirW, SrcVirH, RK_FORMAT_YCbCr_420_SP, 0);
    RGA_set_dst_vir_info(&Rga_Request, fd_dst, 0, 0,DstVirW,DstVirH,&clip, Dstfmt, 0);
    RGA_set_bitblt_mode(&Rga_Request, 0, RotateMode,Rotation,0,0,0);
    RGA_set_src_act_info(&Rga_Request,SrcActW,SrcActH, 0,0);
    RGA_set_dst_act_info(&Rga_Request,DstActW,DstActH, xoffset,yoffset);

//debug: clear source data
#if 0
    memset((void*)handle->base,0x0,SrcVirW*SrcVirH*3/2);
#endif

    if(handle->type == 1) {
        if( !specialwin) {
#if defined(__arm64__) || defined(__aarch64__)
            RGA_set_dst_vir_info(&Rga_Request, 0,(unsigned long)(baseAddr), (unsigned long)(baseAddr) + DstVirW * DstVirH,DstVirW,DstVirH,&clip, Dstfmt, 0);
#else
            RGA_set_dst_vir_info(&Rga_Request, 0,(unsigned int)(baseAddr), (unsigned int)(baseAddr) + DstVirW * DstVirH,DstVirW,DstVirH,&clip, Dstfmt, 0);
#endif
            ALOGW("Debugmem mmu_en fd=%d in vmalloc ,base=%p,[%dX%d],fmt=%d,src_addr=%x", fd_dst,baseAddr,DstVirW,DstVirH,handle->video_addr);
        } else {
            RGA_set_dst_vir_info(&Rga_Request, 0,context->base_video_bk[index_v], context->base_video_bk[index_v] + DstVirW * DstVirH,DstVirW,DstVirH,&clip, Dstfmt, 0);
            ALOGD_IF(log(HLLSEV),"rga_video_copybit fd_dst=%d,base=%x,index_v=%d",fd_dst,context->base_video_bk[index_v],index_v);
        }
        RGA_set_mmu_info(&Rga_Request, 1, 0, 0, 0, 0, 2);
        Rga_Request.mmu_info.mmu_flag |= (1<<31) | (1<<10) | (1<<8);
    }

    if(context->relFenceFd[index_v] > 0) {
        ATRACE_CALL();
        ALOGD_IF(log(HLLSIX),"Goto dst sync wait %d",context->relFenceFd[index_v]);
        sync_wait(context->relFenceFd[index_v],-1);
        close(context->relFenceFd[index_v]);
        context->relFenceFd[index_v] = -1;
        ALOGD_IF(log(HLLSIX),"Goout dst sync wait %d",context->relFenceFd[index_v]);
    }

    if(ioctl(rga_fd, RGA_BLIT_SYNC, &Rga_Request)) {
        LOGE(" %s(%d) RGA_BLIT fail",__FUNCTION__, __LINE__);
        ALOGE("err src addr=[%x],w-h[%d,%d],act[%d,%d][f=%d],x_y_offset[%d,%d]",
            specialwin ? handle->share_fd:handle->video_addr, SrcVirW, SrcVirH,SrcActW,SrcActH,specialwin ?  hwChangeRgaFormat(handle->format):RK_FORMAT_YCbCr_420_SP,xoffset,yoffset);
        ALOGE("err dst fd=[%x],w-h[%d,%d],act[%d,%d][f=%d],rot=%d,rot_mod=%d",
            fd_dst, DstVirW, DstVirH,DstActW,DstActH,Dstfmt,Rotation,RotateMode);
    }

    if (fd_dup > -1) {
	    close(fd_dup);
	    if (cpu_ptr != MAP_FAILED)
	        munmap(cpu_ptr, handle->size);
    }

    //  pthread_mutex_unlock(&_contextAnchor->lock);

#if DUMP_AFTER_RGA_COPY_IN_GPU_CASE
    FILE * pfile = NULL;
    int srcStride = android::bytesPerPixel(handle->format);
    char layername[100];

    if(hwc_get_int_property("sys.hwc.dump_after_rga_copy","0"))
    {
        memset(layername,0,sizeof(layername));
        system("mkdir /data/dumplayer/ && chmod /data/dumplayer/ 777 ");
        sprintf(layername,"/data/dumplayer/dmlayer%d_%d_%d.bin",\
               handle->stride,handle->height,srcStride);

        pfile = fopen(layername,"wb");
        if(pfile)
        {
            fwrite((const void *)(baseAddr),(size_t)(3 * handle->stride*handle->height /2),1,pfile);
            fclose(pfile);
        }
    }
#endif
    return 0;
}

static int is_x_intersect(hwc_rect_t * rec,hwc_rect_t * rec2)
{
    if(rec2->top == rec->top)
        return 1;
    else if(rec2->top < rec->top)
    {
        if(rec2->bottom > rec->top)
            return 1;
        else
            return 0;
    }
    else
    {
        if(rec->bottom > rec2->top  )
            return 1;
        else
            return 0;
    }
    return 0;
}

static int is_zone_combine(ZoneInfo * zf,ZoneInfo * zf2)
{
    if(zf->format != zf2->format)
    {
        ALOGV("line=%d",__LINE__);
        ALOGV("format:%x=>%x",zf->format,zf2->format);
        return 0;
    }
    if(zf->zone_alpha!= zf2->zone_alpha)
    {
        ALOGV("line=%d",__LINE__);
        ALOGV("zone_alpha:%x=>%x",zf->zone_alpha,zf2->zone_alpha);

        return 0;
    }
    if(zf->is_stretch || zf2->is_stretch )
    {
        ALOGV("line=%d",__LINE__);
        ALOGV("is_stretch:%x=>%x",zf->is_stretch,zf2->is_stretch);

        return 0;
    }
    if(is_x_intersect(&(zf->disp_rect),&(zf2->disp_rect)))
    {
        ALOGV("line=%d",__LINE__);
        ALOGV("is_x_intersect rec(%d,%d,%d,%d)=rec2(%d,%d,%d,%d)",zf->disp_rect.left,zf->disp_rect.top,zf->disp_rect.right,\
        zf->disp_rect.bottom,zf2->disp_rect.left,zf2->disp_rect.top,zf2->disp_rect.right,zf2->disp_rect.bottom);
        return 0;
    }
    else
        return 1;
}

static int is_yuv(int format)
{
    int ret = 0;
    switch(format){
        case HAL_PIXEL_FORMAT_YCrCb_NV12:
        case HAL_PIXEL_FORMAT_YCrCb_NV12_VIDEO:
        case HAL_PIXEL_FORMAT_YCrCb_NV12_10:
            ret = 1;
            break;

        default:
            break;
    }
    return ret;
}

static int is_special_wins(hwcContext * Context)
{
    return 0;
    ZoneManager* pzone_mag = &(Context->zone_manager);
    return 0;
}

static bool is_same_rect(hwc_rect_t rect1,hwc_rect_t rect2)
{
    if(rect1.left == rect2.left && rect1.top == rect2.top
        && rect1.right == rect2.right && rect1.bottom == rect2.bottom)
        return true;
    else
        return false;
}

static bool is_hotplug_connected()
{
    hwcContext * ctxp = _contextAnchor;
    return (getHdmiMode() == 1 || _contextAnchor->mHdmiSI.CvbsOn);
}

static bool is_need_post(hwc_display_contents_1_t *list,int dpyID,int flag)
{
    hwcContext * ctxp = _contextAnchor;

    if (ctxp->isRk3399 || ctxp->isRk3366)
        return true;

#ifdef RK3288_BOX
    hwcContext * context = _contextAnchor;
    if(context->mLcdcNum == 2) {
        return true;
    }
#endif
#if DUAL_VIEW_MODE
    if(!hdmi_noready && dpyID == HWCP) {
        return false;
    }
#endif
    switch(flag) {
        case 0://hotplug device not realdy,so we not post:from set_screen
#if (defined(GPU_G6110) || defined(RK3288_BOX) || defined(RK3399_BOX))
            if(hdmi_noready && dpyID == HWCE) {
#if (defined(RK3368_BOX) || defined(RK3288_BOX) || defined(RK3399_BOX))
                if((!hdmi_noready && is_hotplug_connected())) {
                    hotplug_set_frame(_contextAnchor,0);
                }
#endif
                return false;
            }
#endif
            break;

        case 1://hotplug is realdy so primary not post:from fb_post
#if (defined(GPU_G6110) || defined(RK3288_BOX) || defined(RK3399_BOX))
            if((!hdmi_noready && is_hotplug_connected()) && dpyID==0) {
                return false;
            }
#endif
            break;
        case 2://hotplug is realdy so primary not post:from set_lcdc
#if (defined(GPU_G6110) || defined(RK3288_BOX) || defined(RK3399_BOX))
            if(!(hdmi_noready || dpyID == HWCE)) {
                return false;
            }
#endif
            break;
        default:
            break;
    }
    return true;
}

static bool is_gpu_or_nodraw(hwc_display_contents_1_t *list,int dpyID)
{
    hwcContext * ctxp = _contextAnchor;

    if (ctxp->isRk3399 || ctxp->isRk3366)
        return false;

#ifdef RK3288_BOX
    if(ctxp->mLcdcNum == 2) {
#if DUAL_VIEW_MODE
        if(ctxp->mIsDualViewMode) {
            if(!hdmi_noready  && dpyID == HWCP && is_hotplug_connected()) {
                for (unsigned int i = 0; i < (list->numHwLayers - 1); i++) {
                    hwc_layer_1_t * layer = &list->hwLayers[i];
                    layer->compositionType = HWC_NODRAW;
                }
                ALOGD_IF(log(HLLSIX),"Primary nodraw %s,%d",__FUNCTION__,__LINE__);
                return true;
            }
            if(hdmi_noready && dpyID == HWCE) {
                ALOGD_IF(log(HLLSIX),"Hotplug nodraw %s,%d",__FUNCTION__,__LINE__);
                return true;
            }
        }
#endif
        return false;
    }
#endif
#if DUAL_VIEW_MODE
    if(ctxp->mIsDualViewMode) {
        if(!hdmi_noready  && dpyID == HWCP && is_hotplug_connected()) {
            for (unsigned int i = 0; i < (list->numHwLayers - 1); i++) {
                hwc_layer_1_t * layer = &list->hwLayers[i];
                layer->compositionType = HWC_NODRAW;
            }
            ALOGD_IF(log(HLLSIX),"Primary nodraw %s,%d",__FUNCTION__,__LINE__);
            return true;
        }
        if(hdmi_noready && dpyID == HWCE) {
            ALOGD_IF(log(HLLSIX),"Hotplug nodraw %s,%d",__FUNCTION__,__LINE__);
            return true;
        }
    }
#endif
#if (defined(GPU_G6110) || defined(RK3288_BOX) || defined(RK3399_BOX))
    if(!hdmi_noready  && dpyID == HWCP && is_hotplug_connected()) {
        for (unsigned int i = 0; i < (list->numHwLayers - 1); i++) {
            hwc_layer_1_t * layer = &list->hwLayers[i];
            layer->compositionType = HWC_NODRAW;
        }
        ALOGD_IF(log(HLLSIX),"Primary nodraw %s,%d",__FUNCTION__,__LINE__);
        return true;
    }
    if(hdmi_noready && dpyID == HWCE) {
        ALOGD_IF(log(HLLSIX),"Hotplug nodraw %s,%d",__FUNCTION__,__LINE__);
        return true;
    }
#endif
    return false;
}

static bool is_primary_and_resolution_changed(hwcContext * ctx)
{
    hwcContext * context = _contextAnchor;
    bool ret = false;
    if (ctx == context)
        ret = true;
    if (context && (context->isRk3399 || context->isRk3366))
        ret = ret && context->mResolutionChanged;
    else
        ret = false;

    return ret;
}

static bool is_vop_connected(int vopIndex)
{
    const char node[] = "/sys/class/graphics/fb%u/hot_plug_state";
    char nodeName[100] = {0};
    char value[100] = {0};
    bool connected = false;
    int fbindx = 0;
    int fbFd = -1;
    int ret = 0;
    if (vopIndex == 0)
        fbindx = 1;
    if (vopIndex == 1)
        fbindx = 6;

    snprintf(nodeName, 64, node, fbindx);

    ALOGD("nodeName=%s",nodeName);
    fbFd = open(nodeName,O_RDONLY);
    if(fbFd > -1) {
        ret = read(fbFd,value,80);
        if(ret <= 0) {
            ALOGE("fb%d/win_property read fail %s", fbindx, strerror(errno));
        } else {
            connected = !!atoi(value);
            ALOGI("fb%d/win_property winFeature:%d", fbindx, connected);
        }
        close(fbFd);
    }

    return connected;
}

static bool is_common_vop(int vopIndex)
{
    const char node[] = "/sys/class/graphics/fb%u/win_property";
    char nodeName[100] = {0};
    char value[100] = {0};
    unsigned int winFeature = 0;
    int fbindx = 0;
    int fbFd = -1;
    int ret = 0;
    if (vopIndex == 0)
        fbindx = 1;
    if (vopIndex == 1)
        fbindx = 6;

    snprintf(nodeName, 64, node, fbindx);

    ALOGD("nodeName=%s",nodeName);
    fbFd = open(nodeName,O_RDONLY);
    if(fbFd > -1) {
        ret = read(fbFd,value,80);
        if(ret <= 0) {
            ALOGE("fb%d/win_property read fail %s", fbindx, strerror(errno));
        } else {
            sscanf(value, "feature: %d", &winFeature);
            ALOGI("fb%d/win_property winFeature:0x%x", fbindx, winFeature);
        }
        close(fbFd);
    }

    return winFeature & 0x2;
}

static bool isVopNeedLargeBandwidth(hwcContext * ctx,
                                                float hfactor, float vfactor)
{
    int w,h,rw,rh;
    float hScale,vScale;
    bool ret = false;
    if (!ctx)
        return -1;

    w = h = rw = rh = 0;
    if (ctx->mContextIndex == 0) {
        w = ctx->dpyAttr[HWC_DISPLAY_PRIMARY].xres;
        h = ctx->dpyAttr[HWC_DISPLAY_PRIMARY].yres;
        rw = ctx->dpyAttr[HWC_DISPLAY_PRIMARY].relxres;
        rh = ctx->dpyAttr[HWC_DISPLAY_PRIMARY].relyres;
    } else if (ctx->mContextIndex == 1) {
        w = _contextAnchor->dpyAttr[HWC_DISPLAY_EXTERNAL].xres;
        h = _contextAnchor->dpyAttr[HWC_DISPLAY_EXTERNAL].yres;
        rw = _contextAnchor1->dpyAttr[HWC_DISPLAY_EXTERNAL].xres;
        rh = _contextAnchor1->dpyAttr[HWC_DISPLAY_EXTERNAL].yres;
    }

    if (rw > rh)
        return false;

    hScale = hfactor * w / rw;
    vScale = vfactor * h / rh;

    if (hScale >= 2)
        ret = true;

    if (vScale >= 1.5 && vScale < 2)
        ret = true;

    if (vScale >= 3.5 && vScale < 4)
        ret = true;

    if (vScale >= 7.5 && vScale < 8)
        ret = true;

    ALOGD_IF(log(HLLTWO), "w-h-rw-rh-h-v[%d,%d,%d,%d,%f,%f,%f,%f]",
                        w, h, rw, rh, hfactor, vfactor, hScale, vScale);
    return ret;
}

static int queryVopScreenMode(hwcContext* ctx)
{
    if (!ctx)
        return -EINVAL;

    const char node[] = "/sys/class/graphics/fb%u/dsp_mode";
    char nodeName[80];
    char value[20];
    int fbindex = 0;
    int ret;

    if (ctx->mContextIndex == 0)
        fbindex = 0;
    else if (ctx->mContextIndex == 1 && (ctx->isRk3399 || ctx->isRk3399))
        fbindex = 5;
    else
        fbindex = 4;

    snprintf(nodeName, 64, node, fbindex);

    ALOGD("%s:%d:nodeName=%s",__func__,__LINE__,nodeName);
    int fd = open(nodeName,O_RDONLY);
    if (fd > -1) {
        ret = read(fd, value, 20);
        if (ret <= 0) {
            ALOGE("fb%d/win_property read fail %s", fbindex, strerror(errno));
        }
        close(fd);
    }

    ctx->vopDispMode = atoi(value);
    ALOGI("ctx[%d]->vopDispMode=%d", ctx->mContextIndex, ctx->vopDispMode);

    return 0;
}

static int initPlatform(hwcContext* ctx)
{
    if (!ctx)
        return -EINVAL;

    ctx->isRk3288 = false;
    ctx->isRk3368 = false;
    ctx->isRk3366 = false;
    ctx->isRk3399 = false;

#ifdef TARGET_BOARD_PLATFORM_RK3288
    ctx->isRk3288 = true;
#elif TARGET_BOARD_PLATFORM_RK3368
    ctx->isRk3368 = true;
#elif TARGET_BOARD_PLATFORM_RK3366
    ctx->isRk3366 = true;
#elif TARGET_BOARD_PLATFORM_RK3399
    ctx->isRk3399 = true;
#else
    ALOGE("Who is this platform?");
#endif

    ALOGI("rk3288:%s;  rk3368:%s;  rk3366:%s;  rk3399:%s;",
       ctx->isRk3288 ? "Yes" : "No",ctx->isRk3368 ? "Yes" : "No",
       ctx->isRk3366 ? "Yes" : "No",ctx->isRk3399 ? "Yes" : "No");

    ctx->isVr = false;
    ctx->isBox = false;
    ctx->isMid = false;
    ctx->isPhone = false;

#ifdef RK_MID
    ctx->isMid = true;
#elif RK_BOX
    ctx->isBox = true;
#elif RK_PHONE
    ctx->isPhone = true;
#elif RK_VR
    ctx->isVr = true;
#else
    ALOGE("Who is the platform?NOT these:box,mid,phone?");
#endif

    ALOGI("isBox:%s;  isMid:%s;  isPhone:%s;  isVr:%s",
           ctx->isBox ? "Yes" : "No",ctx->isMid ? "Yes" : "No",
           ctx->isPhone? "Yes" : "No",ctx->isVr ? "Yes" : "No");
    return 0;
}

static int ZoneDispatchedCheck(hwcContext* ctx,ZoneManager* pzone_mag,int flag)
{
    int ret = 0;
    hwcContext* context = _contextAnchor;
    bool Is4K = context->mHdmiSI.NeedReDst;
    for(int i=0;i<pzone_mag->zone_cnt;i++){
        int disptched = pzone_mag->zone_info[i].dispatched;
        /*win2 win3 not support YUV*/
        if(disptched > win1 && is_yuv(pzone_mag->zone_info[i].format))
            return -1;
        /*scal not support whoes source bigger than 2560 to dst 4k*/
        if(pzone_mag->zone_info[i].width > 2160 && Is4K)
            return -1;
    }
    return ret;
}

static int initLayerCompositionType(hwcContext * Context,hwc_display_contents_1_t * list)
{
    if(!list) {
        ALOGW("List is null");
        return -1;
    }
    for(unsigned int i=0;i < list->numHwLayers - 1;i++) {
        hwc_layer_1_t * layer = &list->hwLayers[i];
        layer->compositionType = HWC_LCDC;
        ALOGD_IF(log(HLLFOU),"layer[%d]=%d",i,layer->compositionType);
    }
    return 0;
}

static int collect_all_zones( hwcContext * Context,hwc_display_contents_1_t * list)
{
    size_t i,j;
    int tsize = 0;
    int factor =1;
    bool useRgaScale = false;
    bool firsttfrmbyrga = true;
    Context->mMultiwindow = false;
    Context->mIsLargeVideo = false;
#if (defined(RK3368_BOX) || defined(RK3288_BOX) || defined(RK3399_BOX))
    bool NeedScale = false;
    bool NeedFullScreen = false;
    char value[PROPERTY_VALUE_MAX];
    property_get("persist.sys.video.cvrs", value, "false");
    NeedScale = !strcmp(value,"true");
    property_get("persist.sys.video.cvrs.fs", value, "false");
    NeedFullScreen = !strcmp(value,"true");
#endif
    for (i = 0; i < (list->numHwLayers - 1) ; i++) {
        hwc_layer_1_t * layer = &list->hwLayers[i];

        if (!layer)
            continue;

        struct private_handle_t* hnd = (struct private_handle_t *) layer->handle;

        if (!hnd)
            continue;

	    if (hnd->format == HAL_PIXEL_FORMAT_YCrCb_NV12_10) {
	        Context->mHasYuvTenBit = true;
	        if (hnd->width * hnd->height > 2160 * 1200)
	            Context->mIsLargeVideo = true;
	    }

        if (hnd->format == HAL_PIXEL_FORMAT_YCrCb_NV12) {
	        if (hnd->width * hnd->height > 2160 * 1200)
	            Context->mIsLargeVideo = true;
	    }
    }

    for (i = 0,j=0; i < (list->numHwLayers - 1) ; i++,j++){
        hwc_layer_1_t * layer = &list->hwLayers[i];
        hwc_region_t * Region = &layer->visibleRegionScreen;
        hwc_rect_t * SrcRect = &layer->sourceCrop;
        hwc_rect_t tmpSourceCrop;
        hwc_rect_t * DstRect = &layer->displayFrame;
        struct private_handle_t* SrcHnd = (struct private_handle_t *) layer->handle;
        float hfactor;
        float vfactor;
        hwcRECT dstRects[16];
        unsigned int m = 0;
        bool is_stretch = 0;
        hwc_rect_t const * rects = Region->rects;
        hwc_rect_t  rect_merge;
        bool haveStartwin = false;
        bool trsfrmbyrga = false;
        int glesPixels = 0;
        int overlayPixels = 0;
#if (defined(RK3368_BOX) || defined(RK3288_BOX) || defined(RK3399_BOX))
        int d_w = 0;  //external weight & height
        int d_h = 0;
        int s_w = 0;
        int s_h = 0;
        float v_scale = 0.0;  //source v_scale & h_scale
        float h_scale = 0.0;
        hwcRECT DstRectScale;
        hotplug_get_resolution(&d_w,&d_h);
        DstRectScale.left  = 0;
        DstRectScale.top   = 0;
        DstRectScale.right = d_w;
        DstRectScale.bottom= d_h;
#endif

        if (!SrcHnd) {
            Context->zone_manager.zone_info[j].source_err = true;
            Context->zone_manager.zone_info[j].zone_index = j;
            Context->zone_manager.zone_info[j].layer_index = i;
            Context->zone_manager.zone_info[j].skipLayer = layer->flags & HWC_SKIP_LAYER;
            continue;
        }

        if (SrcHnd && (SrcHnd->usage & GRALLOC_USAGE_PROTECTED))
            Context->mSecureLayer |= 1 << i;

#if !ENABLE_LCDC_IN_NV12_TRANSFORM
        if(Context->mGtsStatus)
#endif
        {
            ALOGV("In gts status,go into lcdc when rotate video");
            if(layer->transform && SrcHnd->format == HAL_PIXEL_FORMAT_YCrCb_NV12){
                trsfrmbyrga = true;
            }
        }

        if(j>=MaxZones){
            ALOGD("Overflow [%d] >max=%d",m+j,MaxZones);
            return -1;
        }
        if((layer->transform == HWC_TRANSFORM_ROT_90)
            ||(layer->transform == HWC_TRANSFORM_ROT_270)){
            hfactor = (float) (SrcRect->bottom - SrcRect->top)
                    / (DstRect->right - DstRect->left);
            vfactor = (float) (SrcRect->right - SrcRect->left)
                    / (DstRect->bottom - DstRect->top);
        }else{
            hfactor = (float) (SrcRect->right - SrcRect->left)
                    / (DstRect->right - DstRect->left);

            vfactor = (float) (SrcRect->bottom - SrcRect->top)
                    / (DstRect->bottom - DstRect->top);
        }
        if(hfactor >= 8.0 || vfactor >= 8.0 || hfactor <= 0.125 || vfactor <= 0.125  ){
            Context->zone_manager.zone_info[j].scale_err = true;
            ALOGD_IF(log(HLLSIX),"stretch[%f,%f]not support!",hfactor,vfactor);
        }
        is_stretch = (hfactor != 1.0) || (vfactor != 1.0);
        if(Context == _contextAnchor1) {
            is_stretch = is_stretch || _contextAnchor->mHdmiSI.NeedReDst;
        }
#if ONLY_USE_ONE_VOP
#ifdef RK3288_BOX
        if(_contextAnchor->mLcdcNum == 1)
#endif
        {
            is_stretch = is_stretch || _contextAnchor->mHdmiSI.NeedReDst;
        }
#endif
#ifdef RK3288_BOX
        if(Context==_contextAnchor && Context->mResolutionChanged && Context->mLcdcNum == 2){
            is_stretch = true;
        }
#endif

#ifdef USE_AFBC_LAYER
        if ( isAfbcInternalFormat(SrcHnd->internal_format) )
        {
            is_stretch = true; // .trick : 为保证 afbc_layer 可以送 win0, 目前只保证 win0 可正常显示 afbc_layer.
        }
#endif

        if (is_primary_and_resolution_changed(Context))
            is_stretch = true;

        int left_min=0 ;
        int top_min=0;
        int right_max=0;
        int bottom_max=0;
        int isLarge = 0;
        int srcw,srch;
        if(rects){
             left_min = rects[0].left;
             top_min  = rects[0].top;
             right_max  = rects[0].right;
             bottom_max = rects[0].bottom;
        }
        for (int r = 0; r < (int) Region->numRects ; r++){
            int r_left;
            int r_top;
            int r_right;
            int r_bottom;

            r_left   = hwcMAX(DstRect->left,   rects[r].left);
            left_min = hwcMIN(r_left,left_min);
            r_top    = hwcMAX(DstRect->top,    rects[r].top);
            top_min  = hwcMIN(r_top,top_min);
            r_right    = hwcMIN(DstRect->right,  rects[r].right);
            right_max  = hwcMAX(r_right,right_max);
            r_bottom = hwcMIN(DstRect->bottom, rects[r].bottom);
            bottom_max  = hwcMAX(r_bottom,bottom_max);
            glesPixels += (r_right-r_left)*(r_bottom-r_top);
        }
        rect_merge.left = left_min;
        rect_merge.top = top_min;
        rect_merge.right = right_max;
        rect_merge.bottom = bottom_max;

        if(Region->numRects > 1 && i == 0 && !log(65536)) {
            ALOGD_IF(log(HLLTHR),"Is in multiwindow");
            Context->mMultiwindow = true;
        }

        overlayPixels = (DstRect->right-DstRect->left)*(DstRect->bottom-DstRect->top);
        Context->zone_manager.zone_info[j].glesPixels = glesPixels;
        Context->zone_manager.zone_info[j].overlayPixels = overlayPixels;
        overlayPixels = (SrcRect->right-SrcRect->left)*(SrcRect->bottom-SrcRect->top);
        glesPixels = int(1.0 * glesPixels /Context->zone_manager.zone_info[j].overlayPixels * overlayPixels);
        Context->zone_manager.zone_info[j].glesPixels += glesPixels;
        Context->zone_manager.zone_info[j].overlayPixels += overlayPixels;

        Context->zone_manager.zone_info[j].skipLayer = layer->flags & HWC_SKIP_LAYER;

        unsigned const char* pBuffer = (unsigned const char*)DstRect;
        unsigned int crc32 = createCrc32(0xFFFFFFFF,pBuffer,sizeof(hwc_rect_t));
        if(false) {
            pBuffer = (unsigned const char*)rects;
            crc32 = createCrc32(crc32,pBuffer,sizeof(hwc_rect_t)*Region->numRects);
        }
        Context->zone_manager.zone_info[j].zoneCrc = crc32;

        //zxl:If in video mode,then use all area.
        if(SrcHnd->format == HAL_PIXEL_FORMAT_YCrCb_NV12_VIDEO
            || SrcHnd->format == HAL_PIXEL_FORMAT_YCrCb_NV12){
            dstRects[0].left   = DstRect->left;
            dstRects[0].top    = DstRect->top;
            dstRects[0].right  = DstRect->right;
            dstRects[0].bottom = DstRect->bottom;
#if (defined(RK3368_BOX) || defined(RK3288_BOX) || defined(RK3399_BOX))
            if(Context == _contextAnchor1 && NeedScale){
                s_w = SrcRect->right - SrcRect->left;
                s_h = SrcRect->bottom - SrcRect->top;
                if(NeedFullScreen) {
                    DstRectScale.left   = 0;
                    DstRectScale.top    = 0;
                    DstRectScale.right  = d_w;
                    DstRectScale.bottom = d_h;
                } else if(s_w*d_h-s_h*d_w > 0) { //d_w standard
                    ALOGV("%s,%d,[%d,%d][%d,%d]",__FUNCTION__,__LINE__,d_w,d_h,s_w,s_h);
                    DstRectScale.left   = 0;
                    DstRectScale.top    = ((d_h-s_h*d_w/s_w)%2==0)?((d_h-s_h*d_w/s_w)/2):((d_h-s_h*d_w/s_w)/2);
                    DstRectScale.right  = d_w;
                    DstRectScale.bottom = d_h - DstRectScale.top;
                } else {
                    ALOGV("%s,%d,[%d,%d][%d,%d]",__FUNCTION__,__LINE__,d_w,d_h,s_w,s_h);
                    DstRectScale.left   = ((d_w-s_w*d_h/s_h)%2==0)?((d_w-s_w*d_h/s_h)/2):((d_w-s_w*d_h/s_h+1)/2);;
                    DstRectScale.top    = 0;
                    DstRectScale.right  = d_w - DstRectScale.left;
                    DstRectScale.bottom = d_h;
                }
            }
#endif
        }else {
            dstRects[0].left   = hwcMAX(DstRect->left,   rect_merge.left);
            dstRects[0].top    = hwcMAX(DstRect->top,    rect_merge.top);
            dstRects[0].right  = hwcMIN(DstRect->right,  rect_merge.right);
            dstRects[0].bottom = hwcMIN(DstRect->bottom, rect_merge.bottom);
        }
        /* Check dest area. */
        if ((dstRects[m].right <= dstRects[m].left)
            ||  (dstRects[m].bottom <= dstRects[m].top)){
			Context->zone_manager.zone_info[j].zone_err = true;
			LOGI("%s(%d):  skip empty rectangle [%d,%d,%d,%d]",__FUNCTION__,__LINE__,
			dstRects[m].left,dstRects[m].top,dstRects[m].right,dstRects[m].bottom);
        }
        if((dstRects[m].right - dstRects[m].left) < 16
            || (dstRects[m].bottom - dstRects[m].top) < 16){
            Context->zone_manager.zone_info[j].toosmall = true;
        }

        LOGV("%s(%d): Region rect[%d]:  [%d,%d,%d,%d]",__FUNCTION__,__LINE__,
            m,rects[m].left,rects[m].top,rects[m].right,rects[m].bottom);

        Context->zone_manager.zone_info[j].zone_alpha = (layer->blending) >> 16;
        Context->zone_manager.zone_info[j].is_stretch = is_stretch;
        if(SrcHnd->format == HAL_PIXEL_FORMAT_YCrCb_NV12_VIDEO
            || SrcHnd->format == HAL_PIXEL_FORMAT_YCrCb_NV12){
#if (defined(RK3368_BOX) || defined(RK3288_BOX) || defined(RK3399_BOX))
            if(Context == _contextAnchor1 && NeedScale){
                Context->zone_manager.zone_info[j].is_stretch = true;
            }
#endif
        }
    	Context->zone_manager.zone_info[j].hfactor = hfactor;;
        Context->zone_manager.zone_info[j].zone_index = j;
        Context->zone_manager.zone_info[j].layer_index = i;
        Context->zone_manager.zone_info[j].dispatched = 0;
        Context->zone_manager.zone_info[j].direct_fd = 0;
        Context->zone_manager.zone_info[j].sort = 0;
        Context->zone_manager.zone_info[j].addr = 0;
        Context->zone_manager.zone_info[j].handle = (struct private_handle_t *)layer->handle;
        Context->zone_manager.zone_info[j].transform = layer->transform;
        Context->zone_manager.zone_info[j].disp_rect.left = dstRects[0].left;
        Context->zone_manager.zone_info[j].disp_rect.top = dstRects[0].top;

        Context->zone_manager.zone_info[j].disp_rect.right = dstRects[0].right;
        Context->zone_manager.zone_info[j].disp_rect.bottom = dstRects[0].bottom;

#if USE_HWC_FENCE
        Context->zone_manager.zone_info[j].acq_fence_fd = layer->acquireFenceFd;
        Context->zone_manager.zone_info[j].pRelFenceFd = &(layer->releaseFenceFd);
#endif

        bool supportPlatform = false;
        supportPlatform = supportPlatform || Context->isRk3366;
        supportPlatform = supportPlatform || Context->isRk3399;

        if (SrcHnd->format == HAL_PIXEL_FORMAT_YCrCb_NV12_10 && supportPlatform) {
            if (isVopNeedLargeBandwidth(Context, hfactor, vfactor))
                useRgaScale = true;
            else if (layer->transform)
                useRgaScale = true;
            trsfrmbyrga = useRgaScale;
            Context->mTrsfrmbyrga |= useRgaScale;
        }

        if (SrcHnd->format == HAL_PIXEL_FORMAT_YCrCb_NV12 && !trsfrmbyrga) {
            if (isVopNeedLargeBandwidth(Context, hfactor, vfactor))
                useRgaScale = true;
            trsfrmbyrga = useRgaScale;
            Context->mTrsfrmbyrga |= useRgaScale;
        }

        //firsttfrmbyrga:every display only support one layer trfmbyrga for too slow
        if((SrcHnd->format == HAL_PIXEL_FORMAT_YCrCb_NV12_VIDEO
                                                || (trsfrmbyrga)) && firsttfrmbyrga){
            firsttfrmbyrga = false;
            int w_valid = 0 ,h_valid = 0;
#if USE_VIDEO_BACK_BUFFERS
            int index_v = Context->mCurVideoIndex%MaxVideoBackBuffers;
            int video_fd = Context->fd_video_bk[index_v];
#else
            int video_fd;
            int index_v;
            if(trsfrmbyrga){
                index_v = Context->mCurVideoIndex%MaxVideoBackBuffers;
                video_fd = Context->fd_video_bk[index_v];
            }else{
                video_fd = SrcHnd->share_fd;
            }
#endif
		    hwc_rect_t * psrc_rect = &(Context->zone_manager.zone_info[j].src_rect);
            //HAL_PIXEL_FORMAT_YCrCb_NV12;
            Context->zone_manager.zone_info[j].format = trsfrmbyrga ? SrcHnd->format:Context->video_fmt;
            ALOGV("HAL_PIXEL_FORMAT_YCrCb_NV12 transform=%d, addr[%x][%dx%d],ori_fd[%d][%dx%d]",
                    layer->transform,SrcHnd->video_addr,SrcHnd->video_width,SrcHnd->video_height,
                    SrcHnd->share_fd,SrcHnd->width,SrcHnd->height);
            switch (layer->transform){
                case 0:
                    psrc_rect->left   = SrcRect->left
                    - (int) ((DstRect->left   - dstRects[0].left)   * hfactor);
                    psrc_rect->top    = SrcRect->top
                    - (int) ((DstRect->top    - dstRects[0].top)    * vfactor);
                    psrc_rect->right  = SrcRect->right
                    - (int) ((DstRect->right  - dstRects[0].right)  * hfactor);
                    psrc_rect->bottom = SrcRect->bottom
                    - (int) ((DstRect->bottom - dstRects[0].bottom) * vfactor);
                    Context->zone_manager.zone_info[j].layer_fd = 0;
                    Context->zone_manager.zone_info[j].addr = SrcHnd->video_addr;
                    Context->zone_manager.zone_info[j].width = SrcHnd->video_width;
                    Context->zone_manager.zone_info[j].height = SrcHnd->video_height;
                    Context->zone_manager.zone_info[j].stride = SrcHnd->video_width;
                    if (useRgaScale) {
                        Context->zone_manager.zone_info[j].addr = 0;
                        Context->zone_manager.zone_info[j].layer_fd = video_fd;
                        Context->zone_manager.zone_info[j].width = SrcHnd->width;
                        Context->zone_manager.zone_info[j].height = SrcHnd->height;
                        Context->zone_manager.zone_info[j].stride = SrcHnd->stride;
                    }
                    //Context->zone_manager.zone_info[j].format = SrcHnd->format;
                    break;

        		 case HWC_TRANSFORM_ROT_270:
                    if(trsfrmbyrga){
                        psrc_rect->left = SrcRect->top;
                        psrc_rect->top  = SrcHnd->width -  SrcRect->right;//SrcRect->top;
                        psrc_rect->right = SrcRect->bottom;//SrcRect->right;
                        psrc_rect->bottom = SrcHnd->width - SrcRect->left;//SrcRect->bottom;
                    }else{
                        psrc_rect->left   = SrcRect->top +  (SrcRect->right - SrcRect->left)
                        - ((dstRects[0].bottom - DstRect->top)    * vfactor);

                        psrc_rect->top    =  SrcRect->left
                        - (int) ((DstRect->left   - dstRects[0].left)   * hfactor);

                        psrc_rect->right  = psrc_rect->left
                        + (int) ((dstRects[0].bottom - dstRects[0].top) * vfactor);

                        psrc_rect->bottom = psrc_rect->top
                        + (int) ((dstRects[0].right  - dstRects[0].left) * hfactor);
                    }
                    h_valid = trsfrmbyrga ? SrcHnd->height : (psrc_rect->bottom - psrc_rect->top);
                    w_valid = trsfrmbyrga ? SrcHnd->width : (psrc_rect->right - psrc_rect->left);
                    Context->zone_manager.zone_info[j].layer_fd = video_fd;
                    Context->zone_manager.zone_info[j].width = w_valid;
                    Context->zone_manager.zone_info[j].height = rkmALIGN(h_valid,8);
                    Context->zone_manager.zone_info[j].stride = w_valid;
                    //Context->zone_manager.zone_info[j].format = HAL_PIXEL_FORMAT_RGB_565;
                    break;

                case HWC_TRANSFORM_ROT_90:
                    if(trsfrmbyrga){
                        psrc_rect->left = SrcHnd->height - SrcRect->bottom;
                        psrc_rect->top  = SrcRect->left;//SrcRect->top;
                        psrc_rect->right = SrcHnd->height - SrcRect->top;//SrcRect->right;
                        psrc_rect->bottom = SrcRect->right;//SrcRect->bottom;
                    }else{
                        psrc_rect->left   = SrcRect->top
                            - (int) ((DstRect->top    - dstRects[0].top)    * vfactor);

                        psrc_rect->top    =  SrcRect->left
                            - (int) ((DstRect->left   - dstRects[0].left)   * hfactor);

                        psrc_rect->right  = psrc_rect->left
                            + (int) ((dstRects[0].bottom - dstRects[0].top) * vfactor);

                        psrc_rect->bottom = psrc_rect->top
                            + (int) ((dstRects[0].right  - dstRects[0].left) * hfactor);
                    }
                    h_valid = trsfrmbyrga ? SrcHnd->height : (psrc_rect->bottom - psrc_rect->top);
                    w_valid = trsfrmbyrga ? SrcHnd->width : (psrc_rect->right - psrc_rect->left);

                    Context->zone_manager.zone_info[j].layer_fd = video_fd;
                    Context->zone_manager.zone_info[j].width = w_valid;
                    Context->zone_manager.zone_info[j].height = rkmALIGN(h_valid,8); ;
                    Context->zone_manager.zone_info[j].stride = w_valid;
                    //Context->zone_manager.zone_info[j].format = HAL_PIXEL_FORMAT_RGB_565;
                    break;

        		case HWC_TRANSFORM_ROT_180:
                    if(trsfrmbyrga){
                        psrc_rect->left = SrcHnd->width - SrcRect->right;
                        psrc_rect->top  = SrcHnd->height - SrcRect->bottom;//SrcRect->top;
                        psrc_rect->right = SrcHnd->width - SrcRect->left;//SrcRect->right;
                        psrc_rect->bottom = SrcHnd->height - SrcRect->top;//SrcRect->bottom;
                    }else{
                        psrc_rect->left   = SrcRect->left +  (SrcRect->right - SrcRect->left)
                        - ((dstRects[0].right - DstRect->left)   * hfactor);

                        psrc_rect->top    = SrcRect->top
                        - (int) ((DstRect->top    - dstRects[0].top)    * vfactor);

                        psrc_rect->right  = psrc_rect->left
                        + (int) ((dstRects[0].right  - dstRects[0].left) * hfactor);

                        psrc_rect->bottom = psrc_rect->top
                        + (int) ((dstRects[0].bottom - dstRects[0].top) * vfactor);
                    }
                    // w_valid = psrc_rect->right - psrc_rect->left;
                    //h_valid = psrc_rect->bottom - psrc_rect->top;
                    w_valid = trsfrmbyrga ? SrcHnd->width : (psrc_rect->right - psrc_rect->left);
                    h_valid = trsfrmbyrga ? SrcHnd->height : (psrc_rect->bottom - psrc_rect->top);

                    Context->zone_manager.zone_info[j].layer_fd = video_fd;
                    Context->zone_manager.zone_info[j].width = w_valid;
                    Context->zone_manager.zone_info[j].height = h_valid;
                    Context->zone_manager.zone_info[j].stride = w_valid;

                    //Context->zone_manager.zone_info[j].format = HAL_PIXEL_FORMAT_RGB_565;
                    break;

                default:
                    ALOGD("Unsupport transform=0x%x",layer->transform);
                    return -1;
            }

            ALOGV("layer->transform=%d",layer->transform);
            if(layer->transform || useRgaScale){
                int lastfd = -1;
                bool fd_update = true;
                lastfd = Context->mRgaTBI.lastfd;
                if(trsfrmbyrga && lastfd == SrcHnd->share_fd &&
                    (SrcHnd->format != HAL_PIXEL_FORMAT_YCrCb_NV12
                    || SrcHnd->format != HAL_PIXEL_FORMAT_YCrCb_NV12_10)){
                    fd_update = true;
                }
                if(fd_update){
                    ALOGV("Zone[%d]->layer[%d],"
                        "[%d,%d,%d,%d] =>[%d,%d,%d,%d],"
                        "w_h_s_f[%d,%d,%d,%d],tr_rtr_bled[%d,%d,%d],acq_fence_fd=%d,"
                        "fd=%d",
                        Context->zone_manager.zone_info[j].zone_index,
                        Context->zone_manager.zone_info[j].layer_index,
                        Context->zone_manager.zone_info[j].src_rect.left,
                        Context->zone_manager.zone_info[j].src_rect.top,
                        Context->zone_manager.zone_info[j].src_rect.right,
                        Context->zone_manager.zone_info[j].src_rect.bottom,
                        Context->zone_manager.zone_info[j].disp_rect.left,
                        Context->zone_manager.zone_info[j].disp_rect.top,
                        Context->zone_manager.zone_info[j].disp_rect.right,
                        Context->zone_manager.zone_info[j].disp_rect.bottom,
                        Context->zone_manager.zone_info[j].width,
                        Context->zone_manager.zone_info[j].height,
                        Context->zone_manager.zone_info[j].stride,
                        Context->zone_manager.zone_info[j].format,
                        Context->zone_manager.zone_info[j].transform,
                        Context->zone_manager.zone_info[j].realtransform,
                        Context->zone_manager.zone_info[j].blend,
                        Context->zone_manager.zone_info[j].acq_fence_fd,
                        Context->zone_manager.zone_info[j].layer_fd);
                    Context->mRgaTBI.hdl = SrcHnd;
                    Context->mRgaTBI.index = i;
                    Context->mRgaTBI.w_valid = w_valid;
                    Context->mRgaTBI.h_valid = h_valid;
                    Context->mRgaTBI.transform = layer->transform;
                    Context->mRgaTBI.trsfrmbyrga = trsfrmbyrga;
					Context->mRgaTBI.lastfd = SrcHnd->share_fd;
					memcpy(&Context->mRgaTBI.zone_info,&Context->zone_manager.zone_info[j],
					                                                    sizeof(ZoneInfo));
                    Context->mRgaTBI.layer_fd = Context->zone_manager.zone_info[j].layer_fd;
                    Context->mRgaTBI.type = useRgaScale ? 1 : 0;
                    Context->mNeedRgaTransform = true;
                }
            }
            if (useRgaScale) {
                psrc_rect->left = 0;
                psrc_rect->top = 0;
                switch (layer->transform) {
                    case HWC_TRANSFORM_ROT_90:
                    case HWC_TRANSFORM_ROT_270:
                        psrc_rect->right = Context->zone_manager.zone_info[j].disp_rect.right -
                                             Context->zone_manager.zone_info[j].disp_rect.left;
                        psrc_rect->bottom = Context->zone_manager.zone_info[j].disp_rect.bottom -
                                                Context->zone_manager.zone_info[j].disp_rect.top;
                        Context->zone_manager.zone_info[j].stride = psrc_rect->bottom;
                        Context->zone_manager.zone_info[j].width = psrc_rect->bottom - psrc_rect->bottom % 2;
                        Context->zone_manager.zone_info[j].height = rkmALIGN(psrc_rect->right,32);
                        break;
                    case 0:
                    case HWC_TRANSFORM_ROT_180:
                        psrc_rect->bottom = Context->zone_manager.zone_info[j].disp_rect.bottom -
                                                Context->zone_manager.zone_info[j].disp_rect.top;
                        psrc_rect->right = Context->zone_manager.zone_info[j].disp_rect.right -
                                             Context->zone_manager.zone_info[j].disp_rect.left;
                        Context->zone_manager.zone_info[j].stride = rkmALIGN(psrc_rect->right,32);
                        Context->zone_manager.zone_info[j].width = psrc_rect->right - psrc_rect->right % 2;
                        Context->zone_manager.zone_info[j].height = psrc_rect->bottom - psrc_rect->bottom % 2;
                        break;
                    case 2:
                        psrc_rect->right = Context->zone_manager.zone_info[j].disp_rect.bottom -
                                                Context->zone_manager.zone_info[j].disp_rect.top;
                        psrc_rect->bottom = Context->zone_manager.zone_info[j].disp_rect.right -
                                             Context->zone_manager.zone_info[j].disp_rect.left;
                        Context->zone_manager.zone_info[j].stride = rkmALIGN(psrc_rect->right,32);
                        Context->zone_manager.zone_info[j].width = psrc_rect->right - psrc_rect->right % 2;
                        Context->zone_manager.zone_info[j].height = psrc_rect->bottom - psrc_rect->bottom % 2;
                        break;
                    default:
                        break;
                }
                Context->zone_manager.zone_info[j].format = HAL_PIXEL_FORMAT_YCrCb_NV12;
            } else {
                int tmp = 0;
                switch (layer->transform) {
                    case HWC_TRANSFORM_ROT_90:
                    case HWC_TRANSFORM_ROT_270:
                        tmp = Context->zone_manager.zone_info[j].stride;
                        Context->zone_manager.zone_info[j].stride = Context->zone_manager.zone_info[j].height;
                        Context->zone_manager.zone_info[j].width = Context->zone_manager.zone_info[j].height;
                        Context->zone_manager.zone_info[j].height = tmp;
                        break;
                    default:
                        break;
                }
            }
			psrc_rect->left = psrc_rect->left - psrc_rect->left%2;
			psrc_rect->top = psrc_rect->top - psrc_rect->top%2;
			psrc_rect->right = psrc_rect->right - psrc_rect->right%2;
			psrc_rect->bottom = psrc_rect->bottom - psrc_rect->bottom%2;
			Context->zone_manager.zone_info[j].transform = 0;
			Context->zone_manager.zone_info[j].pRelFenceFd = &(Context->relFenceFd[index_v]);
			ALOGV("Zone[%d]->layer[%d],"
                        "[%d,%d,%d,%d] =>[%d,%d,%d,%d],"
                        "w_h_s_f[%d,%d,%d,%d],tr_rtr_bled[%d,%d,%d],acq_fence_fd=%d,"
                        "fd=%d",
                        Context->zone_manager.zone_info[j].zone_index,
                        Context->zone_manager.zone_info[j].layer_index,
                        Context->zone_manager.zone_info[j].src_rect.left,
                        Context->zone_manager.zone_info[j].src_rect.top,
                        Context->zone_manager.zone_info[j].src_rect.right,
                        Context->zone_manager.zone_info[j].src_rect.bottom,
                        Context->zone_manager.zone_info[j].disp_rect.left,
                        Context->zone_manager.zone_info[j].disp_rect.top,
                        Context->zone_manager.zone_info[j].disp_rect.right,
                        Context->zone_manager.zone_info[j].disp_rect.bottom,
                        Context->zone_manager.zone_info[j].width,
                        Context->zone_manager.zone_info[j].height,
                        Context->zone_manager.zone_info[j].stride,
                        Context->zone_manager.zone_info[j].format,
                        Context->zone_manager.zone_info[j].transform,
                        Context->zone_manager.zone_info[j].realtransform,
                        Context->zone_manager.zone_info[j].blend,
                        Context->zone_manager.zone_info[j].acq_fence_fd,
                        Context->zone_manager.zone_info[j].layer_fd);
		} else {
            Context->zone_manager.zone_info[j].src_rect.left   = hwcMAX ((SrcRect->left \
            - (int) ((DstRect->left   - dstRects[0].left)   * hfactor)),0);
            Context->zone_manager.zone_info[j].src_rect.top    = hwcMAX ((SrcRect->top \
            - (int) ((DstRect->top    - dstRects[0].top)    * vfactor)),0);

            Context->zone_manager.zone_info[j].src_rect.right  = SrcRect->right \
            - (int) ((DstRect->right  - dstRects[0].right)  * hfactor);
            Context->zone_manager.zone_info[j].src_rect.bottom = SrcRect->bottom \
            - (int) ((DstRect->bottom - dstRects[0].bottom) * vfactor);

            Context->zone_manager.zone_info[j].format = SrcHnd->format;

#ifdef USE_AFBC_LAYER
            D("to set internal_format to 0x%llx.", SrcHnd->internal_format);
            Context->zone_manager.zone_info[j].internal_format = SrcHnd->internal_format;
#endif

            Context->zone_manager.zone_info[j].width = SrcHnd->width;
            Context->zone_manager.zone_info[j].height = SrcHnd->height;
            Context->zone_manager.zone_info[j].stride = SrcHnd->stride;
            Context->zone_manager.zone_info[j].layer_fd = SrcHnd->share_fd;

            //odd number will lead to lcdc composer fail with error display.
            if(SrcHnd->format == HAL_PIXEL_FORMAT_YCrCb_NV12){
                Context->zone_manager.zone_info[j].src_rect.left = \
                    Context->zone_manager.zone_info[j].src_rect.left - Context->zone_manager.zone_info[j].src_rect.left%2;
                Context->zone_manager.zone_info[j].src_rect.top = \
                    Context->zone_manager.zone_info[j].src_rect.top - Context->zone_manager.zone_info[j].src_rect.top%2;
                Context->zone_manager.zone_info[j].src_rect.right = \
                    Context->zone_manager.zone_info[j].src_rect.right - Context->zone_manager.zone_info[j].src_rect.right%2;
                Context->zone_manager.zone_info[j].src_rect.bottom = \
                    Context->zone_manager.zone_info[j].src_rect.bottom - Context->zone_manager.zone_info[j].src_rect.bottom%2;
            }
        }
        srcw = Context->zone_manager.zone_info[j].src_rect.right - \
                Context->zone_manager.zone_info[j].src_rect.left;
        srch = Context->zone_manager.zone_info[j].src_rect.bottom -  \
                Context->zone_manager.zone_info[j].src_rect.top;
        int bpp = android::bytesPerPixel(Context->zone_manager.zone_info[j].format);
        if(Context->zone_manager.zone_info[j].format == HAL_PIXEL_FORMAT_YCrCb_NV12
            || Context->zone_manager.zone_info[j].format == HAL_PIXEL_FORMAT_YCrCb_NV12_10
            || Context->zone_manager.zone_info[j].format == HAL_PIXEL_FORMAT_YCrCb_NV12_VIDEO
            || haveStartwin)
            bpp = 2;
#if G6110_SUPPORT_FBDC
        else if(Context->zone_manager.zone_info[j].format == HAL_PIXEL_FORMAT_BGRX_8888)
            bpp = 4;
        else if(HALPixelFormatGetCompression(Context->zone_manager.zone_info[j].format) != HAL_FB_COMPRESSION_NONE)
        {
            bpp = 4;
            Context->bFbdc = true;
        }
#endif
        else
            bpp = 4;

        Context->zone_manager.zone_info[j].is_yuv = is_yuv(Context->zone_manager.zone_info[j].format);
        // ALOGD("haveStartwin=%d,bpp=%d",haveStartwin,bpp);
        Context->zone_manager.zone_info[j].size = srcw*srch*bpp;
        if(Context->zone_manager.zone_info[j].hfactor > 1.0 || Context->mIsMediaView)
            factor = 2;
        else
            factor = 1;
        tsize += (Context->zone_manager.zone_info[j].size *factor);
        if(Context->zone_manager.zone_info[j].size > \
            (Context->fbhandle.width * Context->fbhandle.height*3) ){  // w*h*4*3/4
            Context->zone_manager.zone_info[j].is_large = 1;
        }else
            Context->zone_manager.zone_info[j].is_large = 0;
#if (defined(RK3368_BOX) || defined(RK3288_BOX) || defined(RK3399_BOX))
        if((SrcHnd->format == HAL_PIXEL_FORMAT_YCrCb_NV12_VIDEO ||
            SrcHnd->format == HAL_PIXEL_FORMAT_YCrCb_NV12) &&(Context == _contextAnchor1 && NeedScale)){
            Context->zone_manager.zone_info[j].disp_rect.left  = DstRectScale.left;
            Context->zone_manager.zone_info[j].disp_rect.top   = DstRectScale.top;
            Context->zone_manager.zone_info[j].disp_rect.right = DstRectScale.right;
            Context->zone_manager.zone_info[j].disp_rect.bottom = DstRectScale.bottom;
        }
#endif
    }
    Context->zone_manager.zone_cnt = j;
    if(tsize)
        Context->zone_manager.bp_size = tsize / (1024 *1024) * 60 ;// MB
    // query ddr is enough ,if dont enough back to gpu composer
    ALOGV("tsize=%dMB,Context->ddrFd=%d,RK_QUEDDR_FREQ",tsize,Context->ddrFd);
    for(i=0;i<j;i++){
        ALOGD_IF(log(HLLONE),"Zone[%d]->layer[%d],"
            "[%d,%d,%d,%d] =>[%d,%d,%d,%d],"
            "w_h_s_f[%d,%d,%d,%d],tr_rtr_bled[%d,%d,%d],acq_fence_fd=%d,"
            "s_g_o[%d,%d,%d]",
            Context->zone_manager.zone_info[i].zone_index,
            Context->zone_manager.zone_info[i].layer_index,
            Context->zone_manager.zone_info[i].src_rect.left,
            Context->zone_manager.zone_info[i].src_rect.top,
            Context->zone_manager.zone_info[i].src_rect.right,
            Context->zone_manager.zone_info[i].src_rect.bottom,
            Context->zone_manager.zone_info[i].disp_rect.left,
            Context->zone_manager.zone_info[i].disp_rect.top,
            Context->zone_manager.zone_info[i].disp_rect.right,
            Context->zone_manager.zone_info[i].disp_rect.bottom,
            Context->zone_manager.zone_info[i].width,
            Context->zone_manager.zone_info[i].height,
            Context->zone_manager.zone_info[i].stride,
            Context->zone_manager.zone_info[i].format,
            Context->zone_manager.zone_info[i].transform,
            Context->zone_manager.zone_info[i].realtransform,
            Context->zone_manager.zone_info[i].blend,
            Context->zone_manager.zone_info[i].acq_fence_fd,
            Context->zone_manager.zone_info[i].is_stretch,
            Context->zone_manager.zone_info[i].glesPixels,
            Context->zone_manager.zone_info[i].overlayPixels);
    }
    return 0;
}

// return 0: suess
// return -1: fail
static int try_wins_dispatch_hor(void * ctx,hwc_display_contents_1_t * list)
{
    int win_disphed_flag[4] = {0,}; // win0, win1, win2, win3 flag which is dispatched
    int win_disphed[4] = {win0,win1,win2_0,win3_0};
    int i,j;
    int sort = 1;
    int cnt = 0;
    int srot_tal[4][2] = {0,};
    int sort_stretch[4] = {0};
    int sort_pre;
    float hfactor_max = 1.0;
    int large_cnt = 0;
    int bw = 0;
    bool isyuv = false;
    BpVopInfo  bpvinfo;
    int same_cnt = 0;

    hwcContext * Context = (hwcContext *)ctx;
    hwcContext * contextAh = _contextAnchor;
    initLayerCompositionType(Context,list);

    memset(&bpvinfo,0,sizeof(BpVopInfo));
    ZoneManager zone_m;
    memcpy(&zone_m,&Context->zone_manager,sizeof(ZoneManager));
    ZoneManager* pzone_mag = &zone_m;
    // try dispatch stretch wins
    char const* compositionTypeName[] = {
            "win0",
            "win1",
            "win2_0",
            "win2_1",
            "win2_2",
            "win2_3",
            "win3_0",
            "win3_1",
            "win3_2",
            "win3_3",
            };

#if OPTIMIZATION_FOR_TRANSFORM_UI
    //ignore transform ui layer case.
    for(i=0;i<pzone_mag->zone_cnt;i++)
    {
        if((pzone_mag->zone_info[i].transform != 0) && Context->mtrsformcnt > 1
            && (pzone_mag->zone_info[i].format != HAL_PIXEL_FORMAT_YCrCb_NV12)) {
            ALOGD_IF(log(HLLFOU),"i = %d is transform");
            return -1;
        }
    }
#endif
#if DUAL_VIEW_MODE
    if(Context != contextAh && Context->mIsDualViewMode) {
        if(list->numHwLayers > 3) {
            ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
            return -1;
        }
    }
#endif

    if(Context->mAlphaError){
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }

    for(int k = 0; k < pzone_mag->zone_cnt; k++)
    {
         if(pzone_mag->zone_info[k].scale_err || pzone_mag->zone_info[k].toosmall ||
            pzone_mag->zone_info[k].zone_err || pzone_mag->zone_info[k].transform ||
            pzone_mag->zone_info[k].skipLayer || pzone_mag->zone_info[k].source_err) {
            ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
            return -1;
        }
    }

    pzone_mag->zone_info[0].sort = sort;
    for(i=0;i<(pzone_mag->zone_cnt-1);)
    {
        bool is_winfull = false;
        pzone_mag->zone_info[i].sort = sort;
        sort_pre  = sort;
        cnt = 0;
        //means 4: win2 or win3 most has 4 zones
        for(j=1;j<MOST_WIN_ZONES && (i+j) < pzone_mag->zone_cnt;j++)
        {
            ZoneInfo * next_zf = &(pzone_mag->zone_info[i+j]);
            bool is_combine = false;
            int k;
            for(k=0;k<=cnt;k++)  // compare all sorted_zone info
            {
                ZoneInfo * sorted_zf = &(pzone_mag->zone_info[i+j-1-k]);
                if(is_zone_combine(sorted_zf,next_zf)
                    #if ENBALE_WIN_ANY_ZONES
                    && same_cnt < 1
                    #endif
                   )
                {
                    is_combine = true;
                    same_cnt ++;
                }
                else
                {
                    is_combine = false;
                    #if ENBALE_WIN_ANY_ZONES
                    if(same_cnt >= 1)
                    {
                        is_winfull = true;
                        same_cnt = 0;
                    }
                    #endif
                    break;
                }
            }
            if(is_combine)
            {
                pzone_mag->zone_info[i+j].sort = sort;
                cnt++;
                ALOGV("combine [%d]=%d,cnt=%d",i+j,sort,cnt);
            }
            else
            {
                if(!is_winfull)
                sort++;
                pzone_mag->zone_info[i+j].sort = sort;
                cnt++;
                ALOGV("Not combine [%d]=%d,cnt=%d",i+j,sort,cnt);
                break;
            }
        }
        if( sort_pre == sort && (i+cnt) < (pzone_mag->zone_cnt-1) )  // win2 ,4zones ,win3 4zones,so sort ++,but exit not ++
        {
            if(!is_winfull)
            sort ++;
            ALOGV("sort++ =%d,[%d,%d,%d]",sort,i,cnt,pzone_mag->zone_cnt);
        }
        i += cnt;
    }
    if(sort >4)  // lcdc dont support 5 wins
    {
        ALOGD_IF(log(HLLFOU),"try %s lcdc<5wins sort=%d,%d",__FUNCTION__,sort,__LINE__);
        return -1;
    }
	//pzone_mag->zone_info[i].sort: win type
	// srot_tal[i][0] : tatal same wins
	// srot_tal[0][i] : dispatched lcdc win
    for(i=0;i<pzone_mag->zone_cnt;i++)
    {
        ALOGV("sort[%d].type=%d",i,pzone_mag->zone_info[i].sort);
        if( pzone_mag->zone_info[i].sort == 1){
            srot_tal[0][0]++;
            if(pzone_mag->zone_info[i].is_stretch || pzone_mag->zone_info[i].is_yuv)
                sort_stretch[0] = 1;
        }
        else if(pzone_mag->zone_info[i].sort == 2){
            srot_tal[1][0]++;
            if(pzone_mag->zone_info[i].is_stretch || pzone_mag->zone_info[i].is_yuv)
                sort_stretch[1] = 1;
        }
        else if(pzone_mag->zone_info[i].sort == 3){
            srot_tal[2][0]++;
            if(pzone_mag->zone_info[i].is_stretch || pzone_mag->zone_info[i].is_yuv)
                sort_stretch[2] = 1;
        }
        else if(pzone_mag->zone_info[i].sort == 4){
            srot_tal[3][0]++;
            if(pzone_mag->zone_info[i].is_stretch || pzone_mag->zone_info[i].is_yuv)
                sort_stretch[3] = 1;
        }
        if(pzone_mag->zone_info[i].hfactor > hfactor_max)
        {
            hfactor_max = pzone_mag->zone_info[i].hfactor;
        }
        if(pzone_mag->zone_info[i].is_large )
        {
            large_cnt ++;
        }
        if(pzone_mag->zone_info[i].format== HAL_PIXEL_FORMAT_YCrCb_NV12)
        {
            isyuv = true;
        }

    }
    if(hfactor_max >=1.4)
        bw ++;
    if(isyuv)
    {
        if(pzone_mag->zone_cnt <5)
        bw += 2;
        else
            bw += 4;
    }
    // first dispatch more zones win
    j = 0;
    for(i=0;i<4;i++)
    {
        if( srot_tal[i][0] >=2)  // > twice zones
        {
            srot_tal[i][1] = win_disphed[j+2];
            win_disphed_flag[j+2] = 1; // win2 ,win3 is dispatch flag
            ALOGV("more twice zones srot_tal[%d][1]=%d",i,srot_tal[i][1]);
            j++;
            if(j > 2)  // lcdc only has win2 and win3 supprot more zones
            {
                ALOGD_IF(log(HLLFOU),"lcdc only has win2 and win3 supprot more zones");
                return -1;
            }
        }
    }

    //second dispatch stretch win
    j = 0;
    for(i=0;i<4;i++)
    {
        if( sort_stretch[i] == 1)  // strech
        {
            srot_tal[i][1] = win_disphed[j];  // win 0 and win 1 suporot stretch
            win_disphed_flag[j] = 1; // win2 ,win3 is dispatch flag
            ALOGV("stretch zones srot_tal[%d][1]=%d",i,srot_tal[i][1]);
            j++;
            if(j > 2)  // lcdc only has win2 and win3 supprot more zones
            {
                ALOGD_IF(log(HLLFOU),"lcdc only has win0 and win1 supprot stretch");
                return -1;
            }
        }
    }
#ifndef RK_VR
    //third dispatch common zones win
    for (i = 0; i < 4; i++)
    {
        /*had not dispatched and not need scale*/
        if (srot_tal[i][1] == 0 && sort_stretch[i] == 0)
        {
            for(j = 2; j < 4; j++)
            {
                if (win_disphed_flag[j] == 0) // find the win had not dispatched
                    break;
            }
            if (j >= 4)
                break;

            srot_tal[i][1] = win_disphed[j];
            win_disphed_flag[j] = 1;
            ALOGV("srot_tal[%d][1].dispatched=%d",i,srot_tal[i][1]);
        }
    }

    //four dispatch scale zones win but not need scale
    for (i = 0; i < 4; i++)
    {
        if (srot_tal[i][1] == 0)  // had not dispatched
        {
            for (j = 0; j < 2; j++)
            {
                if(win_disphed_flag[j] == 0) // find the win had not dispatched
                    break;
            }
            if (j >= 2)
            {
                ALOGE("4 wins had beed dispatched ");
                return -1;
            }
            srot_tal[i][1] = win_disphed[j];
            win_disphed_flag[j] = 1;
            ALOGV("srot_tal[%d][1].dispatched=%d",i,srot_tal[i][1]);
        }
    }
#else
    // third dispatch common zones win
    for(i=0;i<4;i++)
    {
        if( srot_tal[i][1] == 0)  // had not dispatched
        {
            for(j=0;j<4;j++)
            {
                if(win_disphed_flag[j] == 0) // find the win had not dispatched
                    break;
            }
            if(j>=4)
            {
                ALOGE("4 wins had beed dispatched ");
                return -1;
            }
            srot_tal[i][1] = win_disphed[j];
            win_disphed_flag[j] = 1;
            ALOGV("srot_tal[%d][1].dispatched=%d",i,srot_tal[i][1]);
        }
    }
#endif

    for(i=0;i<pzone_mag->zone_cnt;i++)
    {
         switch(pzone_mag->zone_info[i].sort) {
            case 1:
                pzone_mag->zone_info[i].dispatched = srot_tal[0][1]++;
                break;
            case 2:
                pzone_mag->zone_info[i].dispatched = srot_tal[1][1]++;
                break;
            case 3:
                pzone_mag->zone_info[i].dispatched = srot_tal[2][1]++;
                break;
            case 4:
                pzone_mag->zone_info[i].dispatched = srot_tal[3][1]++;
                break;
            default:
                ALOGE("try_wins_dispatch_hor sort err!");
                return -1;
        }
        ALOGD_IF(log(HLLFIV),"zone[%d].dispatched[%d]=%s,sort=%d", \
        i,pzone_mag->zone_info[i].dispatched,
        compositionTypeName[pzone_mag->zone_info[i].dispatched -1],
        pzone_mag->zone_info[i].sort);

    }

    for(i=0;i<pzone_mag->zone_cnt;i++){
        int disptched = pzone_mag->zone_info[i].dispatched;
        int sct_width = pzone_mag->zone_info[i].src_rect.right
                                            - pzone_mag->zone_info[i].src_rect.left;
        int sct_height = pzone_mag->zone_info[i].src_rect.bottom
                                            - pzone_mag->zone_info[i].src_rect.top;
        int dst_width = pzone_mag->zone_info[i].disp_rect.right
                                            - pzone_mag->zone_info[i].disp_rect.left;
        int dst_height = pzone_mag->zone_info[i].disp_rect.bottom
                                            - pzone_mag->zone_info[i].disp_rect.top;
        /*win2 win3 not support YUV*/
        if(disptched > win1 && is_yuv(pzone_mag->zone_info[i].format))
            return -1;
        /*scal not support whoes source bigger than 2560 to dst 4k*/
        if(disptched <= win1 &&(sct_width > 2160 || sct_height > 2160) &&
            !is_yuv(pzone_mag->zone_info[i].format) && contextAh->mHdmiSI.NeedReDst)
            return -1;
        if(disptched <= win1 && (sct_width > 2560 || dst_width > 2560) &&
                                             !is_yuv(pzone_mag->zone_info[i].format)
                        && (sct_height != dst_height || Context->mResolutionChanged))
            return -1;
    }

#if USE_QUEUE_DDRFREQ
    if(Context->ddrFd > 0)
    {
        for(i=0;i<pzone_mag->zone_cnt;i++)
        {
            int area_no = 0;
            int win_id = 0;
            ALOGD_IF(log(HLLFIV),"Zone[%d]->layer[%d],dispatched=%d,"
            "[%d,%d,%d,%d] =>[%d,%d,%d,%d],"
            "w_h_s_f[%d,%d,%d,%d],tr_rtr_bled[%d,%d,%d],"
            "layer_fd[%d],addr=%x,acq_fence_fd=%d",
            pzone_mag->zone_info[i].zone_index,
            pzone_mag->zone_info[i].layer_index,
            pzone_mag->zone_info[i].dispatched,
            pzone_mag->zone_info[i].src_rect.left,
            pzone_mag->zone_info[i].src_rect.top,
            pzone_mag->zone_info[i].src_rect.right,
            pzone_mag->zone_info[i].src_rect.bottom,
            pzone_mag->zone_info[i].disp_rect.left,
            pzone_mag->zone_info[i].disp_rect.top,
            pzone_mag->zone_info[i].disp_rect.right,
            pzone_mag->zone_info[i].disp_rect.bottom,
            pzone_mag->zone_info[i].width,
            pzone_mag->zone_info[i].height,
            pzone_mag->zone_info[i].stride,
            pzone_mag->zone_info[i].format,
            pzone_mag->zone_info[i].transform,
            pzone_mag->zone_info[i].realtransform,
            pzone_mag->zone_info[i].blend,
            pzone_mag->zone_info[i].layer_fd,
            pzone_mag->zone_info[i].addr,
            pzone_mag->zone_info[i].acq_fence_fd);
            switch(pzone_mag->zone_info[i].dispatched) {
                case win0:
                    bpvinfo.vopinfo[0].state = 1;
                    bpvinfo.vopinfo[0].zone_num ++;
                   break;
                case win1:
                    bpvinfo.vopinfo[1].state = 1;
                    bpvinfo.vopinfo[1].zone_num ++;
                    break;
                case win2_0:
                    bpvinfo.vopinfo[2].state = 1;
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_1:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_2:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_3:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win3_0:
                    bpvinfo.vopinfo[3].state = 1;
                    bpvinfo.vopinfo[3].zone_num ++;
                    break;
                case win3_1:
                    bpvinfo.vopinfo[3].zone_num ++;
                    break;
                case win3_2:
                    bpvinfo.vopinfo[3].zone_num ++;
                    break;
                case win3_3:
                    bpvinfo.vopinfo[3].zone_num ++;
                    break;
                 case win_ext:
                    break;
                default:
                    ALOGE("hwc_dispatch  err!");
                    return -1;
             }
        }
        bpvinfo.bp_size = Context->zone_manager.bp_size;
        bpvinfo.bp_vop_size = Context->zone_manager.bp_size;
        for(i= 0;i<4;i++)
        {
            ALOGD_IF(log(HLLFIV),"RK_QUEDDR_FREQ info win[%d] bo_size=%dMB,bp_vop_size=%dMB,state=%d,num=%d",
                i,bpvinfo.bp_size,bpvinfo.bp_vop_size,bpvinfo.vopinfo[i].state,bpvinfo.vopinfo[i].zone_num);
        }
        if(ioctl(Context->ddrFd, RK_QUEDDR_FREQ, &bpvinfo))
        {
            if(log(HLLTHR))
            {
                for(i= 0;i<4;i++)
                {
                    ALOGD("RK_QUEDDR_FREQ info win[%d] bo_size=%dMB,bp_vop_size=%dMB,state=%d,num=%d",
                        i,bpvinfo.bp_size,bpvinfo.bp_vop_size,bpvinfo.vopinfo[i].state,bpvinfo.vopinfo[i].zone_num);
                }
            }
            return -1;
        }
    }
#endif
    if((large_cnt + bw ) > 5 )
    {
        ALOGD_IF(log(HLLTHR),"data too large ,lcdc not support");
        return -1;
    }
    memcpy(&Context->zone_manager,&zone_m,sizeof(ZoneManager));
    Context->zone_manager.mCmpType = HWC_HOR;
    Context->zone_manager.composter_mode = HWC_LCDC;
    return 0;
}

static int try_wins_dispatch_mix_cross(void * ctx,hwc_display_contents_1_t * list)
{
    int win_disphed_flag[3] = {0,}; // win0, win1, win2, win3 flag which is dispatched
    int win_disphed[3] = {win0,win1,win2_0};
    int i,j;
    int cntfb = 0;
    hwcContext * Context = (hwcContext *)ctx;
    ZoneManager zone_m;
    initLayerCompositionType(Context,list);
    memcpy(&zone_m,&Context->zone_manager,sizeof(ZoneManager));
    ZoneManager* pzone_mag = &zone_m;
    ZoneInfo    zone_info_ty[MaxZones];
    int sort = 1;
    int cnt = 0;
    int srot_tal[3][2] = {0,};
    int sort_stretch[3] = {0};
    int sort_pre;
    int gpu_draw = 0;
    float hfactor_max = 1.0;
    int large_cnt = 0;
    bool isyuv = false;
    int bw = 0;
    BpVopInfo  bpvinfo;
    int tsize = 0;
    int mix_index = 0;
    int iFirstTransformLayer=-1;
    int foundLayer = 0;
    bool intersect = false;
    bool bTransform=false;
    mix_info gMixInfo;

    return -1;
    memset(&bpvinfo,0,sizeof(BpVopInfo));
    char const* compositionTypeName[] = {
            "win0",
            "win1",
            "win2_0",
            "win2_1",
            "win2_2",
            "win2_3",
            "win3_0",
            "win3_1",
            "win3_2",
            "win3_3",
            };
    hwcContext * contextAh = _contextAnchor;
    memset(&zone_info_ty,0,sizeof(zone_info_ty));
    if(Context == _contextAnchor1){
        mix_index = 1;
    }else if(Context == _contextAnchor){
        mix_index = 0;
    }
    if(list->numHwLayers - 1 < 5){
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }

    if(Context->mAlphaError){
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }

    if(contextAh->mHdmiSI.NeedReDst){
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }

#ifdef RK3288_BOX
    if(Context==_contextAnchor && Context->mResolutionChanged && Context->mLcdcNum==2){
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }
#endif

    if (is_primary_and_resolution_changed(Context)) {
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }

    if(Context->Is3D){
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }

    for(int k=1;k<pzone_mag->zone_cnt;k++){
        if(pzone_mag->zone_info[foundLayer].glesPixels <= pzone_mag->zone_info[k].glesPixels){
            foundLayer = k;
        }
    }

    for(int k=foundLayer+1;k<pzone_mag->zone_cnt;k++){
        if(is_x_intersect(&(pzone_mag->zone_info[foundLayer].disp_rect),&(pzone_mag->zone_info[k].disp_rect))){
            intersect = true;
            ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
            return -1;
        }
    }

    memcpy((void*)&gMixInfo,(void*)&gmixinfo[mix_index],sizeof(gMixInfo));
    for(i=0,j=0;i<pzone_mag->zone_cnt;i++){
        //Set the layer which it's layer_index bigger than the first transform layer index to HWC_FRAMEBUFFER or HWC_NODRAW
        if(pzone_mag->zone_info[i].layer_index > 1 && pzone_mag->zone_info[i].layer_index != foundLayer){
            hwc_layer_1_t * layer = &list->hwLayers[pzone_mag->zone_info[i].layer_index];
            if(pzone_mag->zone_info[i].layer_index > 1 && pzone_mag->zone_info[i].layer_index != foundLayer){
                for(int j=2;j<pzone_mag->zone_cnt;j++){
                    layer = &list->hwLayers[j];
                    layer->compositionType = HWC_FRAMEBUFFER;
                }
            }
            cntfb ++;
        }else{
            memcpy(&zone_info_ty[j], &pzone_mag->zone_info[i],sizeof(ZoneInfo));
            zone_info_ty[j].sort = 0;
            j++;
        }
    }
    memcpy(pzone_mag,zone_info_ty,sizeof(zone_info_ty));
    pzone_mag->zone_cnt -= cntfb;
    for(i=0;i< pzone_mag->zone_cnt;i++)
    {
        ALOGD_IF(log(HLLFIV),"%s,%d:Zone[%d]->layer[%d],"
            "[%d,%d,%d,%d] =>[%d,%d,%d,%d],"
            "w_h_s_f[%d,%d,%d,%d],tr_rtr_bled[%d,%d,%d],acq_fence_fd=%d",
            __FUNCTION__,__LINE__,
            Context->zone_manager.zone_info[i].zone_index,
            Context->zone_manager.zone_info[i].layer_index,
            Context->zone_manager.zone_info[i].src_rect.left,
            Context->zone_manager.zone_info[i].src_rect.top,
            Context->zone_manager.zone_info[i].src_rect.right,
            Context->zone_manager.zone_info[i].src_rect.bottom,
            Context->zone_manager.zone_info[i].disp_rect.left,
            Context->zone_manager.zone_info[i].disp_rect.top,
            Context->zone_manager.zone_info[i].disp_rect.right,
            Context->zone_manager.zone_info[i].disp_rect.bottom,
            Context->zone_manager.zone_info[i].width,
            Context->zone_manager.zone_info[i].height,
            Context->zone_manager.zone_info[i].stride,
            Context->zone_manager.zone_info[i].format,
            Context->zone_manager.zone_info[i].transform,
            Context->zone_manager.zone_info[i].realtransform,
            Context->zone_manager.zone_info[i].blend,
            Context->zone_manager.zone_info[i].acq_fence_fd);
    }
    pzone_mag->zone_info[0].sort = sort;
    for(i=0;i<(pzone_mag->zone_cnt-1);)
    {
        pzone_mag->zone_info[i].sort = sort;
        sort_pre  = sort;
        cnt = 0;
        for(j=1;j<4 && (i+j) < pzone_mag->zone_cnt;j++)
        {
            ZoneInfo * next_zf = &(pzone_mag->zone_info[i+j]);
            bool is_combine = false;
            int k;
            for(k=0;k<=cnt;k++)  // compare all sorted_zone info
            {
                ZoneInfo * sorted_zf = &(pzone_mag->zone_info[i+j-1-k]);
                if(is_zone_combine(sorted_zf,next_zf))
                {
                    is_combine = true;
                }
                else
                {
                    is_combine = false;
                    break;
                }
            }
            if(is_combine)
            {
                pzone_mag->zone_info[i+j].sort = sort;
                cnt++;
            }
            else
            {
                sort++;
                pzone_mag->zone_info[i+j].sort = sort;
                cnt++;
                break;
            }
        }
        if( sort_pre == sort && (i+cnt) < (pzone_mag->zone_cnt-1) )  // win2 ,4zones ,win3 4zones,so sort ++,but exit not ++
            sort ++;
        i += cnt;
    }
    if(sort >3)  // lcdc dont support 5 wins
    {
        ALOGD_IF(log(HLLTHR),"lcdc dont support 5 wins sort=%d",sort);
        return -1;
    }
    for(i=0;i<pzone_mag->zone_cnt;i++)
    {
        int factor =1;
        ALOGV("sort[%d].type=%d",i,pzone_mag->zone_info[i].sort);
        if( pzone_mag->zone_info[i].sort == 1){
            srot_tal[0][0]++;
            if(pzone_mag->zone_info[i].is_stretch)
                sort_stretch[0] = 1;
        }
        else if(pzone_mag->zone_info[i].sort == 2){
            srot_tal[1][0]++;
            if(pzone_mag->zone_info[i].is_stretch)
                sort_stretch[1] = 1;
        }
        else if(pzone_mag->zone_info[i].sort == 3){
            srot_tal[2][0]++;
            if(pzone_mag->zone_info[i].is_stretch)
                sort_stretch[2] = 1;
        }
        if(pzone_mag->zone_info[i].hfactor > hfactor_max)
        {
            hfactor_max = pzone_mag->zone_info[i].hfactor;
        }
        if(pzone_mag->zone_info[i].is_large )
        {
            large_cnt ++;
        }
        if(pzone_mag->zone_info[i].format== HAL_PIXEL_FORMAT_YCrCb_NV12)
        {
            isyuv = true;
        }
        if(Context->zone_manager.zone_info[i].hfactor > 1.0)
            factor = 2;
        else
            factor = 1;
        tsize += (Context->zone_manager.zone_info[i].size *factor);
    }
    j = 0;
    for(i=0;i<3;i++)
    {
        if( srot_tal[i][0] >=2)  // > twice zones
        {
            srot_tal[i][1] = win_disphed[j+2];
            win_disphed_flag[j+2] = 1; // win2 ,win3 is dispatch flag
            ALOGV("more twice zones srot_tal[%d][1]=%d",i,srot_tal[i][1]);
            j++;
            if(j > 1)  // lcdc only has win2 and win3 supprot more zones
            {
                ALOGD("lcdc only has win2 and win3 supprot more zones");
                return -1;
            }
        }
    }
    j = 0;
    for(i=0;i<3;i++)
    {
        if( sort_stretch[i] == 1)  // strech
        {
            srot_tal[i][1] = win_disphed[j];  // win 0 and win 1 suporot stretch
            win_disphed_flag[j] = 1; // win0 ,win1 is dispatch flag
            ALOGV("stretch zones srot_tal[%d][1]=%d",i,srot_tal[i][1]);
            j++;
            if(j > 2)  // lcdc only has win0 and win1 supprot stretch
            {
                ALOGD_IF(log(HLLFIV),"lcdc only has win0 and win1 supprot stretch");
                return -1;
            }
        }
    }
    if(hfactor_max >=1.4)
    {
        bw += (j + 1);
    }
    if(isyuv)
    {
        bw +=5;
    }
    ALOGV("large_cnt =%d,bw=%d",large_cnt , bw);

    for(i=0;i<3;i++)
    {
        if( srot_tal[i][1] == 0)  // had not dispatched
        {
            for(j=0;j<3;j++)
            {
                if(win_disphed_flag[j] == 0) // find the win had not dispatched
                    break;
            }
            if(j>=3)
            {
                ALOGE("3 wins had beed dispatched ");
                return -1;
            }
            srot_tal[i][1] = win_disphed[j];
            win_disphed_flag[j] = 1;
            ALOGV("srot_tal[%d][1].dispatched=%d",i,srot_tal[i][1]);
        }
    }

    for(i=0;i<pzone_mag->zone_cnt;i++)
    {
         switch(pzone_mag->zone_info[i].sort) {
            case 1:
                pzone_mag->zone_info[i].dispatched = srot_tal[0][1]++;
                break;
            case 2:
                pzone_mag->zone_info[i].dispatched = srot_tal[1][1]++;
                break;
            case 3:
                pzone_mag->zone_info[i].dispatched = srot_tal[2][1]++;
                break;
            default:
                ALOGE("try_wins_dispatch_mix_vh sort err!");
                return -1;
        }
        ALOGV("zone[%d].dispatched[%d]=%s,sort=%d", \
        i,pzone_mag->zone_info[i].dispatched,
        compositionTypeName[pzone_mag->zone_info[i].dispatched -1],
        pzone_mag->zone_info[i].sort);
    }

    for(i=0;i<pzone_mag->zone_cnt;i++){
        int disptched = pzone_mag->zone_info[i].dispatched;
        int sct_width = pzone_mag->zone_info[i].src_rect.right
                                            - pzone_mag->zone_info[i].src_rect.left;
        int sct_height = pzone_mag->zone_info[i].src_rect.bottom
                                            - pzone_mag->zone_info[i].src_rect.top;
        int dst_width = pzone_mag->zone_info[i].disp_rect.right
                                            - pzone_mag->zone_info[i].disp_rect.left;
        int dst_height = pzone_mag->zone_info[i].disp_rect.bottom
                                            - pzone_mag->zone_info[i].disp_rect.top;
        /*scal not support whoes source bigger than 2560 to dst 4k*/
        if(disptched <= win1 &&(sct_width > 2160 || sct_height > 2160) &&
            !is_yuv(pzone_mag->zone_info[i].format) && contextAh->mHdmiSI.NeedReDst)
            return -1;
        if(disptched <= win1 && (sct_width > 2560 || dst_width > 2560) &&
                                             !is_yuv(pzone_mag->zone_info[i].format)
                        && (sct_height != dst_height || Context->mResolutionChanged))
            return -1;
    }

#if USE_QUEUE_DDRFREQ
    if(Context->ddrFd > 0)
    {
        for(i=0;i<pzone_mag->zone_cnt;i++)
        {
            int area_no = 0;
            int win_id = 0;
            ALOGD_IF(log(HLLFIV),"Zone[%d]->layer[%d],dispatched=%d,"
            "[%d,%d,%d,%d] =>[%d,%d,%d,%d],"
            "w_h_s_f[%d,%d,%d,%d],tr_rtr_bled[%d,%d,%d],"
            "layer_fd[%d],addr=%x,acq_fence_fd=%d",
            pzone_mag->zone_info[i].zone_index,
            pzone_mag->zone_info[i].layer_index,
            pzone_mag->zone_info[i].dispatched,
            pzone_mag->zone_info[i].src_rect.left,
            pzone_mag->zone_info[i].src_rect.top,
            pzone_mag->zone_info[i].src_rect.right,
            pzone_mag->zone_info[i].src_rect.bottom,
            pzone_mag->zone_info[i].disp_rect.left,
            pzone_mag->zone_info[i].disp_rect.top,
            pzone_mag->zone_info[i].disp_rect.right,
            pzone_mag->zone_info[i].disp_rect.bottom,
            pzone_mag->zone_info[i].width,
            pzone_mag->zone_info[i].height,
            pzone_mag->zone_info[i].stride,
            pzone_mag->zone_info[i].format,
            pzone_mag->zone_info[i].transform,
            pzone_mag->zone_info[i].realtransform,
            pzone_mag->zone_info[i].blend,
            pzone_mag->zone_info[i].layer_fd,
            pzone_mag->zone_info[i].addr,
            pzone_mag->zone_info[i].acq_fence_fd);
            switch(pzone_mag->zone_info[i].dispatched) {
                case win0:
                    bpvinfo.vopinfo[0].state = 1;
                    bpvinfo.vopinfo[0].zone_num ++;
                   break;
                case win1:
                    bpvinfo.vopinfo[1].state = 1;
                    bpvinfo.vopinfo[1].zone_num ++;
                    break;
                case win2_0:
                    bpvinfo.vopinfo[2].state = 1;
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_1:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_2:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_3:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                default:
                    ALOGE("hwc_dispatch_mix  err!");
                    return -1;
             }
        }
        bpvinfo.vopinfo[3].state = 1;
        bpvinfo.vopinfo[3].zone_num ++;
        bpvinfo.bp_size = Context->zone_manager.bp_size;
        tsize += Context->fbhandle.width * Context->fbhandle.height*4;
        if(tsize)
            tsize = tsize / (1024 *1024) * 60 ;// MB
        bpvinfo.bp_vop_size = tsize ;
        for(i= 0;i<4;i++)
        {
            ALOGD_IF(log(HLLTHR),"RK_QUEDDR_FREQ mixinfo win[%d] bo_size=%dMB,bp_vop_size=%dMB,state=%d,num=%d",
                i,bpvinfo.bp_size,bpvinfo.bp_vop_size,bpvinfo.vopinfo[i].state,bpvinfo.vopinfo[i].zone_num);
        }
        if(ioctl(Context->ddrFd, RK_QUEDDR_FREQ, &bpvinfo))
        {
            if(log(HLLTHR))
            {
                for(i= 0;i<4;i++)
                {
                    ALOGD("RK_QUEDDR_FREQ mixinfo win[%d] bo_size=%dMB,bp_vop_size=%dMB,state=%d,num=%d",
                        i,bpvinfo.bp_size,bpvinfo.bp_vop_size,bpvinfo.vopinfo[i].state,bpvinfo.vopinfo[i].zone_num);
                }
            }
            return -1;
        }
    }
#endif
    //Mark the composer mode to HWC_MIX
    if (list) {
        list->hwLayers[0].compositionType = HWC_MIX_V2;
        list->hwLayers[1].compositionType = HWC_MIX_V2;
        list->hwLayers[foundLayer].compositionType = HWC_MIX_V2;
    }

    for (unsigned int i = 0; i < list->numHwLayers - 1; i ++) {
        hwc_layer_1_t * layer = &list->hwLayers[i];
        struct private_handle_t* hnd = (struct private_handle_t *)layer->handle;
        if (hnd && layer->compositionType == HWC_FRAMEBUFFER &&
                                  hnd->format == HAL_PIXEL_FORMAT_YCrCb_NV12_10)
            return -1;
    }

    memcpy(&Context->zone_manager,&zone_m,sizeof(ZoneManager));
    Context->zone_manager.mCmpType = HWC_MIX_CROSS;
    Context->zone_manager.composter_mode = HWC_MIX_V2;
    memcpy((void*)&gmixinfo[mix_index],(void*)&gMixInfo,sizeof(gMixInfo));
    return 0;
}


static int try_wins_dispatch_mix_up(void * ctx,hwc_display_contents_1_t * list)
{
    int win_disphed_flag[3] = {0,}; // win0, win1, win2, win3 flag which is dispatched
    int win_disphed[3] = {win0,win1,win2_0};
    int i,j;
    int cntfb = 0;
    hwcContext * Context = (hwcContext *)ctx;
    ZoneManager zone_m;
    initLayerCompositionType(Context,list);
    memcpy(&zone_m,&Context->zone_manager,sizeof(ZoneManager));
    ZoneManager* pzone_mag = &zone_m;
    ZoneInfo    zone_info_ty[MaxZones];
    int sort = 1;
    int cnt = 0;
    int srot_tal[3][2] = {0,};
    int sort_stretch[3] = {0};
    int sort_pre;
    int gpu_draw = 0;
    float hfactor_max = 1.0;
    int large_cnt = 0;
    bool isyuv = false;
    int bw = 0;
    BpVopInfo  bpvinfo;
    int tsize = 0;
    int mix_index = 0;
    int iFirstTransformLayer=-1;
    bool bTransform=false;
    mix_info gMixInfo;

    memset(&bpvinfo,0,sizeof(BpVopInfo));
    char const* compositionTypeName[] = {
            "win0",
            "win1",
            "win2_0",
            "win2_1",
            "win2_2",
            "win2_3",
            "win3_0",
            "win3_1",
            "win3_2",
            "win3_3",
            };
    hwcContext * contextAh = _contextAnchor;
    memset(&zone_info_ty,0,sizeof(zone_info_ty));
    if(Context == _contextAnchor1){
        mix_index = 1;
    }else if(Context == _contextAnchor){
        mix_index = 0;
    }
    if(list->numHwLayers - 1 < 3){
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
    	return -1;
    }

    if(Context->mAlphaError){
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }

    if(contextAh->mHdmiSI.NeedReDst){
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }

#ifdef RK3288_BOX
    if(Context==_contextAnchor && Context->mResolutionChanged && Context->mLcdcNum==2){
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }
#endif

    if (is_primary_and_resolution_changed(Context)) {
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }

#if DUAL_VIEW_MODE
    if(Context != contextAh && Context->mIsDualViewMode) {
        int dpyPw = contextAh->dpyAttr[0].xres;
        int dpyPh = contextAh->dpyAttr[0].yres;
        int dpyEw = Context->dpyAttr[1].xres;
        int dpyEh = Context->dpyAttr[1].yres;
        if(dpyPw != dpyEw || dpyPh != dpyEh) {
            ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
            return -1;
        }
    }
#endif

    for(int k=0;k<2;k++)
    {
        if(pzone_mag->zone_info[k].scale_err || pzone_mag->zone_info[k].toosmall
            || pzone_mag->zone_info[k].zone_err || (pzone_mag->zone_info[k].transform
            && pzone_mag->zone_info[k].format != HAL_PIXEL_FORMAT_YCrCb_NV12 && 0==k)
            || (pzone_mag->zone_info[k].transform && 1 == k)
            || pzone_mag->zone_info[k].skipLayer || pzone_mag->zone_info[k].source_err) {
            ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
            return -1;
        }
    }

    memcpy((void*)&gMixInfo,(void*)&gmixinfo[mix_index],sizeof(gMixInfo));
    for(i=0,j=0;i<pzone_mag->zone_cnt;i++)
    {
        //Set the layer which it's layer_index bigger than the first transform layer index to HWC_FRAMEBUFFER or HWC_NODRAW
        if(pzone_mag->zone_info[i].layer_index > 1)
        {
            hwc_layer_1_t * layer = &list->hwLayers[pzone_mag->zone_info[i].layer_index];
            //Judge the current layer whether backup in gmixinfo[mix_index] or not.
            if(Context->mLastCompType != HWC_MIX_UP
                || gMixInfo.lastZoneCrc[pzone_mag->zone_info[i].layer_index] != pzone_mag->zone_info[i].zoneCrc
                || gMixInfo.gpu_draw_fd[pzone_mag->zone_info[i].layer_index] != pzone_mag->zone_info[i].layer_fd
                || gMixInfo.alpha[pzone_mag->zone_info[i].layer_index] != pzone_mag->zone_info[i].zone_alpha) {
                gpu_draw = 1;
                layer->compositionType = HWC_FRAMEBUFFER;
                gMixInfo.lastZoneCrc[pzone_mag->zone_info[i].layer_index] = pzone_mag->zone_info[i].zoneCrc;
                gMixInfo.gpu_draw_fd[pzone_mag->zone_info[i].layer_index] = pzone_mag->zone_info[i].layer_fd;
                gMixInfo.alpha[pzone_mag->zone_info[i].layer_index] = pzone_mag->zone_info[i].zone_alpha;
            }
            else
            {
                layer->compositionType = HWC_NODRAW;
            }
            if(gpu_draw && i == pzone_mag->zone_cnt-1)
            {
                for(int j=1;j<pzone_mag->zone_cnt;j++)
                {
                    layer = &list->hwLayers[j];
                    layer->compositionType = HWC_FRAMEBUFFER;
                }
                ALOGV(" need draw by gpu");
            }
            cntfb ++;
        }
        else
        {
            memcpy(&zone_info_ty[j], &pzone_mag->zone_info[i],sizeof(ZoneInfo));
            zone_info_ty[j].sort = 0;
            j++;
        }
    }
    memcpy(pzone_mag, &zone_info_ty,sizeof(zone_info_ty));
    pzone_mag->zone_cnt -= cntfb;
    for(i=0;i< pzone_mag->zone_cnt;i++)
    {
        ALOGD_IF(log(HLLFIV),"%s,%d:Zone[%d]->layer[%d],"
            "[%d,%d,%d,%d] =>[%d,%d,%d,%d],"
            "w_h_s_f[%d,%d,%d,%d],tr_rtr_bled[%d,%d,%d],acq_fence_fd=%d",
            __FUNCTION__,__LINE__,
            Context->zone_manager.zone_info[i].zone_index,
            Context->zone_manager.zone_info[i].layer_index,
            Context->zone_manager.zone_info[i].src_rect.left,
            Context->zone_manager.zone_info[i].src_rect.top,
            Context->zone_manager.zone_info[i].src_rect.right,
            Context->zone_manager.zone_info[i].src_rect.bottom,
            Context->zone_manager.zone_info[i].disp_rect.left,
            Context->zone_manager.zone_info[i].disp_rect.top,
            Context->zone_manager.zone_info[i].disp_rect.right,
            Context->zone_manager.zone_info[i].disp_rect.bottom,
            Context->zone_manager.zone_info[i].width,
            Context->zone_manager.zone_info[i].height,
            Context->zone_manager.zone_info[i].stride,
            Context->zone_manager.zone_info[i].format,
            Context->zone_manager.zone_info[i].transform,
            Context->zone_manager.zone_info[i].realtransform,
            Context->zone_manager.zone_info[i].blend,
            Context->zone_manager.zone_info[i].acq_fence_fd);
    }
    pzone_mag->zone_info[0].sort = sort;
    for(i=0;i<(pzone_mag->zone_cnt-1);)
    {
        pzone_mag->zone_info[i].sort = sort;
        sort_pre  = sort;
        cnt = 0;
        for(j=1;j<4 && (i+j) < pzone_mag->zone_cnt;j++)
        {
            ZoneInfo * next_zf = &(pzone_mag->zone_info[i+j]);
            bool is_combine = false;
            int k;
            for(k=0;k<=cnt;k++)  // compare all sorted_zone info
            {
                ZoneInfo * sorted_zf = &(pzone_mag->zone_info[i+j-1-k]);
                if(is_zone_combine(sorted_zf,next_zf))
                {
                    is_combine = true;
                }
                else
                {
                    is_combine = false;
                    break;
                }
            }
            if(is_combine)
            {
                pzone_mag->zone_info[i+j].sort = sort;
                cnt++;
            }
            else
            {
                sort++;
                pzone_mag->zone_info[i+j].sort = sort;
                cnt++;
                break;
            }
        }
        if( sort_pre == sort && (i+cnt) < (pzone_mag->zone_cnt-1) )  // win2 ,4zones ,win3 4zones,so sort ++,but exit not ++
            sort ++;
        i += cnt;
    }
    if(sort >3)  // lcdc dont support 5 wins
    {
        ALOGD_IF(log(HLLTHR),"lcdc dont support 5 wins sort=%d",sort);
        return -1;
    }
    for(i=0;i<pzone_mag->zone_cnt;i++)
    {
        int factor =1;
        ALOGV("sort[%d].type=%d",i,pzone_mag->zone_info[i].sort);
        if( pzone_mag->zone_info[i].sort == 1){
            srot_tal[0][0]++;
            if(pzone_mag->zone_info[i].is_stretch)
                sort_stretch[0] = 1;
        }
        else if(pzone_mag->zone_info[i].sort == 2){
            srot_tal[1][0]++;
            if(pzone_mag->zone_info[i].is_stretch)
                sort_stretch[1] = 1;
        }
        else if(pzone_mag->zone_info[i].sort == 3){
            srot_tal[2][0]++;
            if(pzone_mag->zone_info[i].is_stretch)
                sort_stretch[2] = 1;
        }
        if(pzone_mag->zone_info[i].hfactor > hfactor_max)
        {
            hfactor_max = pzone_mag->zone_info[i].hfactor;
        }
        if(pzone_mag->zone_info[i].is_large )
        {
            large_cnt ++;
        }
        if(pzone_mag->zone_info[i].format== HAL_PIXEL_FORMAT_YCrCb_NV12)
        {
            isyuv = true;
        }
        if(Context->zone_manager.zone_info[i].hfactor > 1.0)
            factor = 2;
        else
            factor = 1;
        tsize += (Context->zone_manager.zone_info[i].size *factor);
    }
    j = 0;
    for(i=0;i<3;i++)
    {
        if( srot_tal[i][0] >=2)  // > twice zones
        {
            srot_tal[i][1] = win_disphed[j+2];
            win_disphed_flag[j+2] = 1; // win2 ,win3 is dispatch flag
            ALOGV("more twice zones srot_tal[%d][1]=%d",i,srot_tal[i][1]);
            j++;
            if(j > 1)  // lcdc only has win2 and win3 supprot more zones
            {
                ALOGD("lcdc only has win2 and win3 supprot more zones");
                return -1;
            }
        }
    }
    j = 0;
    for(i=0;i<3;i++)
    {
        if( sort_stretch[i] == 1)  // strech
        {
            srot_tal[i][1] = win_disphed[j];  // win 0 and win 1 suporot stretch
            win_disphed_flag[j] = 1; // win0 ,win1 is dispatch flag
            ALOGV("stretch zones srot_tal[%d][1]=%d",i,srot_tal[i][1]);
            j++;
            if(j > 2)  // lcdc only has win0 and win1 supprot stretch
            {
                ALOGD_IF(log(HLLFIV),"lcdc only has win0 and win1 supprot stretch");
                return -1;
            }
        }
    }
    if(hfactor_max >=1.4)
    {
        bw += (j + 1);

    }
    if(isyuv)
    {
        bw +=5;
    }
    ALOGV("large_cnt =%d,bw=%d",large_cnt , bw);

    for(i=0;i<3;i++)
    {
        if( srot_tal[i][1] == 0)  // had not dispatched
        {
            for(j=0;j<3;j++)
            {
                if(win_disphed_flag[j] == 0) // find the win had not dispatched
                    break;
            }
            if(j>=3)
            {
                ALOGE("3 wins had beed dispatched ");
                return -1;
            }
            srot_tal[i][1] = win_disphed[j];
            win_disphed_flag[j] = 1;
            ALOGV("srot_tal[%d][1].dispatched=%d",i,srot_tal[i][1]);
        }
    }

    for(i=0;i<pzone_mag->zone_cnt;i++)
    {
         switch(pzone_mag->zone_info[i].sort) {
            case 1:
                pzone_mag->zone_info[i].dispatched = srot_tal[0][1]++;
                break;
            case 2:
                pzone_mag->zone_info[i].dispatched = srot_tal[1][1]++;
                break;
            case 3:
                pzone_mag->zone_info[i].dispatched = srot_tal[2][1]++;
                break;
            default:
                ALOGE("try_wins_dispatch_mix_vh sort err!");
                return -1;
        }
        ALOGV("zone[%d].dispatched[%d]=%s,sort=%d", \
        i,pzone_mag->zone_info[i].dispatched,
        compositionTypeName[pzone_mag->zone_info[i].dispatched -1],
        pzone_mag->zone_info[i].sort);
    }

    for(i=0;i<pzone_mag->zone_cnt;i++){
        int disptched = pzone_mag->zone_info[i].dispatched;
        int sct_width = pzone_mag->zone_info[i].src_rect.right
                                            - pzone_mag->zone_info[i].src_rect.left;
        int sct_height = pzone_mag->zone_info[i].src_rect.bottom
                                            - pzone_mag->zone_info[i].src_rect.top;
        int dst_width = pzone_mag->zone_info[i].disp_rect.right
                                            - pzone_mag->zone_info[i].disp_rect.left;
        int dst_height = pzone_mag->zone_info[i].disp_rect.bottom
                                            - pzone_mag->zone_info[i].disp_rect.top;

        /*scal not support whoes source bigger than 2560 to dst 4k*/
        if(disptched <= win1 &&(sct_width > 2160 || sct_height > 2160) &&
            !is_yuv(pzone_mag->zone_info[i].format) && contextAh->mHdmiSI.NeedReDst)
            return -1;
        if(disptched <= win1 && (sct_width > 2560 || dst_width > 2560) &&
                                             !is_yuv(pzone_mag->zone_info[i].format)
                        && (sct_height != dst_height || Context->mResolutionChanged))
            return -1;
    }

#if USE_QUEUE_DDRFREQ
    if(Context->ddrFd > 0)
    {
        for(i=0;i<pzone_mag->zone_cnt;i++)
        {
            int area_no = 0;
            int win_id = 0;
            ALOGD_IF(log(HLLFIV),"Zone[%d]->layer[%d],dispatched=%d,"
            "[%d,%d,%d,%d] =>[%d,%d,%d,%d],"
            "w_h_s_f[%d,%d,%d,%d],tr_rtr_bled[%d,%d,%d],"
            "layer_fd[%d],addr=%x,acq_fence_fd=%d",
            pzone_mag->zone_info[i].zone_index,
            pzone_mag->zone_info[i].layer_index,
            pzone_mag->zone_info[i].dispatched,
            pzone_mag->zone_info[i].src_rect.left,
            pzone_mag->zone_info[i].src_rect.top,
            pzone_mag->zone_info[i].src_rect.right,
            pzone_mag->zone_info[i].src_rect.bottom,
            pzone_mag->zone_info[i].disp_rect.left,
            pzone_mag->zone_info[i].disp_rect.top,
            pzone_mag->zone_info[i].disp_rect.right,
            pzone_mag->zone_info[i].disp_rect.bottom,
            pzone_mag->zone_info[i].width,
            pzone_mag->zone_info[i].height,
            pzone_mag->zone_info[i].stride,
            pzone_mag->zone_info[i].format,
            pzone_mag->zone_info[i].transform,
            pzone_mag->zone_info[i].realtransform,
            pzone_mag->zone_info[i].blend,
            pzone_mag->zone_info[i].layer_fd,
            pzone_mag->zone_info[i].addr,
            pzone_mag->zone_info[i].acq_fence_fd);
            switch(pzone_mag->zone_info[i].dispatched) {
                case win0:
                    bpvinfo.vopinfo[0].state = 1;
                    bpvinfo.vopinfo[0].zone_num ++;
                   break;
                case win1:
                    bpvinfo.vopinfo[1].state = 1;
                    bpvinfo.vopinfo[1].zone_num ++;
                    break;
                case win2_0:
                    bpvinfo.vopinfo[2].state = 1;
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_1:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_2:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_3:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                default:
                    ALOGE("hwc_dispatch_mix  err!");
                    return -1;
             }
        }
        bpvinfo.vopinfo[3].state = 1;
        bpvinfo.vopinfo[3].zone_num ++;
        bpvinfo.bp_size = Context->zone_manager.bp_size;
        tsize += Context->fbhandle.width * Context->fbhandle.height*4;
        if(tsize)
            tsize = tsize / (1024 *1024) * 60 ;// MB
        bpvinfo.bp_vop_size = tsize ;
        for(i= 0;i<4;i++)
        {
            ALOGD_IF(log(HLLTHR),"RK_QUEDDR_FREQ mixinfo win[%d] bo_size=%dMB,bp_vop_size=%dMB,state=%d,num=%d",
                i,bpvinfo.bp_size,bpvinfo.bp_vop_size,bpvinfo.vopinfo[i].state,bpvinfo.vopinfo[i].zone_num);
        }
        if(ioctl(Context->ddrFd, RK_QUEDDR_FREQ, &bpvinfo))
        {
            if(log(HLLTHR))
            {
                for(i= 0;i<4;i++)
                {
                    ALOGD("RK_QUEDDR_FREQ mixinfo win[%d] bo_size=%dMB,bp_vop_size=%dMB,state=%d,num=%d",
                        i,bpvinfo.bp_size,bpvinfo.bp_vop_size,bpvinfo.vopinfo[i].state,bpvinfo.vopinfo[i].zone_num);
                }
            }
            return -1;
        }
    }
#endif
    //Mark the composer mode to HWC_MIX
    if (list) {
        list->hwLayers[0].compositionType = HWC_MIX_V2;
        list->hwLayers[1].compositionType = HWC_MIX_V2;
    }

    for (unsigned int i = 0; i < list->numHwLayers - 1; i ++) {
        hwc_layer_1_t * layer = &list->hwLayers[i];
        struct private_handle_t* hnd = (struct private_handle_t *)layer->handle;
        if (hnd && layer->compositionType == HWC_FRAMEBUFFER &&
                                  hnd->format == HAL_PIXEL_FORMAT_YCrCb_NV12_10)
            return -1;
    }

    memcpy(&Context->zone_manager,&zone_m,sizeof(ZoneManager));
    Context->mHdmiSI.mix_up = true;
    Context->zone_manager.mCmpType = HWC_MIX_UP;
    Context->zone_manager.composter_mode = HWC_MIX;
    memcpy((void*)&gmixinfo[mix_index],(void*)&gMixInfo,sizeof(gMixInfo));
    return 0;
}

static int try_wins_dispatch_mix_down(void * ctx,hwc_display_contents_1_t * list)
{
    int win_disphed_flag[3] = {0,}; // win0, win1, win2, win3 flag which is dispatched
    int win_disphed[3] = {win0,win1,win2_0};
    int i,j;
    int cntfb = 0;
    int foundLayer = 1;
    hwcContext * Context = (hwcContext *)ctx;
    ZoneManager zone_m;
    initLayerCompositionType(Context,list);
    memcpy(&zone_m,&Context->zone_manager,sizeof(ZoneManager));
    ZoneManager* pzone_mag = &zone_m;

    ZoneInfo    zone_info_ty[MaxZones];
    int sort = 1;
    int cnt = 0;
    int srot_tal[3][2] = {0,};
    int sort_stretch[3] = {0};
    int sort_pre;
    int gpu_draw = 0;
    float hfactor_max = 1.0;
    int large_cnt = 0;
    bool isyuv = false;
    int bw = 0;
    BpVopInfo  bpvinfo;
    int tsize = 0;
    int mix_index = 0;
    mix_info gMixInfo;
    memset(&bpvinfo,0,sizeof(BpVopInfo));
    char const* compositionTypeName[] = {
            "win0",
            "win1",
            "win2_0",
            "win2_1",
            "win2_2",
            "win2_3",
            "win3_0",
            "win3_1",
            "win3_2",
            "win3_3",
            };
    hwcContext * contextAh = _contextAnchor;

    memset(&zone_info_ty,0,sizeof(zone_info_ty));
    if(pzone_mag->zone_cnt < 5 && !Context->mMultiwindow) {
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }
    if(Context == _contextAnchor1) {
        mix_index = 1;
    } else if(Context == _contextAnchor) {
        mix_index = 0;
    }
#if OPTIMIZATION_FOR_TRANSFORM_UI
    //ignore transform ui layer case.
    for(i=0;i<pzone_mag->zone_cnt;i++) {
        if((pzone_mag->zone_info[i].transform != 0)&&
            (pzone_mag->zone_info[i].format != HAL_PIXEL_FORMAT_YCrCb_NV12)
            && (Context->mtrsformcnt!=1 || (Context->mtrsformcnt==1 && pzone_mag->zone_cnt>2))
            ) {
                ALOGD_IF(log(HLLFOU),"Policy out %s,%d ",__FUNCTION__,__LINE__);
                return -1;
            }
    }
#endif
#if DUAL_VIEW_MODE
    if(Context != contextAh && Context->mIsDualViewMode) {
        int dpyPw = contextAh->dpyAttr[0].xres;
        int dpyPh = contextAh->dpyAttr[0].yres;
        int dpyEw = Context->dpyAttr[1].xres;
        int dpyEh = Context->dpyAttr[1].yres;
        if(dpyPw != dpyEw || dpyPh != dpyEh) {
            ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
            return -1;
        }
    }
#endif
    if(Context->Is3D){
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }

#ifdef RK3288_BOX
    if(Context==_contextAnchor && Context->mResolutionChanged && Context->mLcdcNum==2){
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }
#endif

    if (is_primary_and_resolution_changed(Context)) {
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }

    if(contextAh->mHdmiSI.NeedReDst){
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }

TryAgain:
    sort = 1;
    cntfb = 0;
    foundLayer++;
    if(!Context->mMultiwindow && foundLayer>2) {
        ALOGD_IF(log(HLLFOU),"Policy out %s,%d",__FUNCTION__,__LINE__);
        return -1;
    } else if(Context->mMultiwindow) {
        bw = 0;
        tsize = 0;
        isyuv = false;
        large_cnt = 0;
        memset((void*)srot_tal,0,sizeof(srot_tal));
        memset((void*)sort_stretch,0,sizeof(sort_stretch));
        memset((void*)win_disphed_flag,0,sizeof(win_disphed_flag));
        memcpy(&zone_m,&Context->zone_manager,sizeof(ZoneManager));
    }
    if(foundLayer > pzone_mag->zone_cnt - 1) {
        ALOGD_IF(log(HLLFOU),"Policy out %s,%d",__FUNCTION__,__LINE__);
        return -1;
    }

    for(int k=foundLayer;k<pzone_mag->zone_cnt;k++) {
        if(pzone_mag->zone_info[k].scale_err || pzone_mag->zone_info[k].toosmall
            || pzone_mag->zone_info[k].zone_err || pzone_mag->zone_info[k].transform
            || pzone_mag->zone_info[k].skipLayer || pzone_mag->zone_info[k].source_err) {
            ALOGD_IF(log(HLLFOU),"Policy out %s,%d ",__FUNCTION__,__LINE__);
            goto TryAgain;
        }
    }

    memcpy((void*)&gMixInfo,(void*)&gmixinfo[mix_index],sizeof(gMixInfo));
    for(i=0,j=0;i<pzone_mag->zone_cnt;i++) {
        if(pzone_mag->zone_info[i].layer_index < foundLayer) {
            hwc_layer_1_t * layer = &list->hwLayers[pzone_mag->zone_info[i].layer_index];
            if(pzone_mag->zone_info[i].format == HAL_PIXEL_FORMAT_YCrCb_NV12) {
                ALOGD_IF(log(HLLFOU),"Policy out Donot support video ");
                return -1;
            }
            if(Context->mLastCompType != HWC_MIX_DOWN
                || gMixInfo.lastZoneCrc[pzone_mag->zone_info[i].layer_index] != pzone_mag->zone_info[i].zoneCrc
                || gMixInfo.gpu_draw_fd[pzone_mag->zone_info[i].layer_index] != pzone_mag->zone_info[i].layer_fd
                || gMixInfo.alpha[pzone_mag->zone_info[i].layer_index] != pzone_mag->zone_info[i].zone_alpha) {
            	ALOGV("bk fd=%d,cur fd=%d;bk alpha=%x,cur alpha=%x,i=%d,layer_index=%d",gMixInfo.gpu_draw_fd[pzone_mag->zone_info[i].layer_index], \
            	pzone_mag->zone_info[i].layer_fd,gMixInfo.alpha[pzone_mag->zone_info[i].layer_index],\
            	pzone_mag->zone_info[i].zone_alpha, i,pzone_mag->zone_info[i].layer_index);
                gpu_draw = 1;
                layer->compositionType = HWC_FRAMEBUFFER;
                gMixInfo.lastZoneCrc[pzone_mag->zone_info[i].layer_index] = pzone_mag->zone_info[i].zoneCrc;
                gMixInfo.gpu_draw_fd[pzone_mag->zone_info[i].layer_index] = pzone_mag->zone_info[i].layer_fd;
                gMixInfo.alpha[pzone_mag->zone_info[i].layer_index] = pzone_mag->zone_info[i].zone_alpha;
            } else {
                layer->compositionType = HWC_NODRAW;
            }

            if(gpu_draw && pzone_mag->zone_info[i].layer_index == foundLayer - 1){
                for(int i = 0;i < foundLayer ;i++){
                    layer = &list->hwLayers[i];
                    layer->compositionType = HWC_FRAMEBUFFER;
                    ALOGV(" need draw by gpu");
                }
            }
            cntfb ++;
        } else {
           memcpy(&zone_info_ty[j], &pzone_mag->zone_info[i],sizeof(ZoneInfo));
           zone_info_ty[j].sort = 0;
           j++;
        }
    }
    memcpy(pzone_mag, &zone_info_ty,sizeof(zone_info_ty));
    pzone_mag->zone_cnt -= cntfb;
    for(i=0;i< pzone_mag->zone_cnt;i++) {
        ALOGD_IF(log(HLLFIV),"%s,%d:Zone[%d]->layer[%d],"
            "[%d,%d,%d,%d] =>[%d,%d,%d,%d],"
            "w_h_s_f[%d,%d,%d,%d],tr_rtr_bled[%d,%d,%d],acq_fence_fd=%d",
            __FUNCTION__,__LINE__,
            pzone_mag->zone_info[i].zone_index,
            pzone_mag->zone_info[i].layer_index,
            pzone_mag->zone_info[i].src_rect.left,
            pzone_mag->zone_info[i].src_rect.top,
            pzone_mag->zone_info[i].src_rect.right,
            pzone_mag->zone_info[i].src_rect.bottom,
            pzone_mag->zone_info[i].disp_rect.left,
            pzone_mag->zone_info[i].disp_rect.top,
            pzone_mag->zone_info[i].disp_rect.right,
            pzone_mag->zone_info[i].disp_rect.bottom,
            pzone_mag->zone_info[i].width,
            pzone_mag->zone_info[i].height,
            pzone_mag->zone_info[i].stride,
            pzone_mag->zone_info[i].format,
            pzone_mag->zone_info[i].transform,
            pzone_mag->zone_info[i].realtransform,
            pzone_mag->zone_info[i].blend,
            pzone_mag->zone_info[i].acq_fence_fd);
    }
    pzone_mag->zone_info[0].sort = sort;
    for(i=0;i<(pzone_mag->zone_cnt-1);) {
        pzone_mag->zone_info[i].sort = sort;
        sort_pre  = sort;
        cnt = 0;
        for(j=1;j<4 && (i+j) < pzone_mag->zone_cnt;j++) {
            ZoneInfo * next_zf = &(pzone_mag->zone_info[i+j]);
            bool is_combine = false;
            int k;
            for(k=0;k<=cnt;k++) {
                ZoneInfo * sorted_zf = &(pzone_mag->zone_info[i+j-1-k]);
                if(is_zone_combine(sorted_zf,next_zf)) {
                    is_combine = true;
                } else {
                    is_combine = false;
                    break;
                }
            }
            if(is_combine) {
                pzone_mag->zone_info[i+j].sort = sort;
                cnt++;
            } else {
                sort++;
                pzone_mag->zone_info[i+j].sort = sort;
                cnt++;
                break;
            }
        }
        if( sort_pre == sort && (i+cnt) < (pzone_mag->zone_cnt-1) ) {
            sort ++;
        }
        i += cnt;
    }
    if(sort >3) {
        ALOGD_IF(log(HLLTHR),"lcdc dont support 5 wins sort=%d",sort);
        goto TryAgain;
    }
    int count = sort;
    for(i=0;i<pzone_mag->zone_cnt;i++) {
        int factor =1;
        ALOGV("sort[%d].type=%d",i,pzone_mag->zone_info[i].sort);
        if( pzone_mag->zone_info[i].sort == 1) {
            srot_tal[0][0]++;
            if(pzone_mag->zone_info[i].is_stretch)
                sort_stretch[0] = 1;
        } else if (pzone_mag->zone_info[i].sort == 2) {
            srot_tal[1][0]++;
            if(pzone_mag->zone_info[i].is_stretch)
                sort_stretch[1] = 1;
        } else if (pzone_mag->zone_info[i].sort == 3) {
            srot_tal[2][0]++;
            if(pzone_mag->zone_info[i].is_stretch)
                sort_stretch[2] = 1;
        }
        if(pzone_mag->zone_info[i].hfactor > hfactor_max) {
            hfactor_max = pzone_mag->zone_info[i].hfactor;
        }
        if(pzone_mag->zone_info[i].is_large ) {
            large_cnt ++;
        }
        if(pzone_mag->zone_info[i].format== HAL_PIXEL_FORMAT_YCrCb_NV12) {
            isyuv = true;
        }
        if(Context->zone_manager.zone_info[i].hfactor > 1.0){
            factor = 2;
        } else {
            factor = 1;
        }
        tsize += (Context->zone_manager.zone_info[i].size *factor);
    }
    j = 0;
    for(i=0;i<count;i++) {
        if( srot_tal[i][0] >=2) {
            srot_tal[i][1] = win_disphed[j+2];
            win_disphed_flag[j+2] = 1; // win2 ,win3 is dispatch flag
            ALOGV("more twice zones srot_tal[%d][1]=%d",i,srot_tal[i][1]);
            j++;
            if(j > 1) {
                ALOGD_IF(log(HLLFOU),"Policy try again %s,%d ",__FUNCTION__,__LINE__);
                goto TryAgain;
            }
        }
    }
    j = 0;
    for(i=0;i<count;i++) {
        if( sort_stretch[i] == 1) {
            srot_tal[i][1] = win_disphed[j];  // win 0 and win 1 suporot stretch
            win_disphed_flag[j] = 1; // win0 ,win1 is dispatch flag
            ALOGV("stretch zones srot_tal[%d][1]=%d",i,srot_tal[i][1]);
            j++;
            if(j > 2) {
                ALOGD_IF(log(HLLFOU),"Policy try again %s,%d ",__FUNCTION__,__LINE__);
                goto TryAgain;
            }
        }
    }
    if(hfactor_max >=1.4) {
        bw += (j + 1);

    }
    if(isyuv) {
        bw +=5;
    }
    //ALOGD("large_cnt =%d,bw=%d",large_cnt , bw);

    for(i=0;i<count;i++) {
        if( srot_tal[i][1] == 0) {
            for(j=0;j<count;j++) {
                if(win_disphed_flag[j] == 0) // find the win had not dispatched
                    break;
            }
            if(j>=count) {
                ALOGD_IF(log(HLLFOU),"Policy try again %s,%d ",__FUNCTION__,__LINE__);
                goto TryAgain;
            }
            srot_tal[i][1] = win_disphed[j];
            win_disphed_flag[j] = 1;
            ALOGV("srot_tal[%d][1].dispatched=%d",i,srot_tal[i][1]);
        }
    }
    for(i=0;i<pzone_mag->zone_cnt;i++)
    {
         switch(pzone_mag->zone_info[i].sort) {
            case 1:
                pzone_mag->zone_info[i].dispatched = srot_tal[0][1]++;
                break;
            case 2:
                pzone_mag->zone_info[i].dispatched = srot_tal[1][1]++;
                break;
            case 3:
                pzone_mag->zone_info[i].dispatched = srot_tal[2][1]++;
                break;
            default:
                ALOGE("try_wins_dispatch_mix sort err!");
                return -1;
        }
        ALOGV("zone[%d].dispatched[%d]=%s,sort=%d", \
        i,pzone_mag->zone_info[i].dispatched,
        compositionTypeName[pzone_mag->zone_info[i].dispatched -1],
        pzone_mag->zone_info[i].sort);
    }

    for(i=0;i<pzone_mag->zone_cnt;i++){
        int disptched = pzone_mag->zone_info[i].dispatched;
        /*win2 win3 not support YUV*/
        if(disptched > win1 && is_yuv(pzone_mag->zone_info[i].format)){
            ALOGD_IF(log(HLLFOU),"Policy try again %s,%d ",__FUNCTION__,__LINE__);
            goto TryAgain;
        }
    }

#if USE_QUEUE_DDRFREQ
    if(Context->ddrFd > 0)
    {
        for(i=0;i<pzone_mag->zone_cnt;i++)
        {
            int area_no = 0;
            int win_id = 0;
            ALOGD_IF(log(HLLFIV),"Zone[%d]->layer[%d],dispatched=%d,"
            "[%d,%d,%d,%d] =>[%d,%d,%d,%d],"
            "w_h_s_f[%d,%d,%d,%d],tr_rtr_bled[%d,%d,%d],"
            "layer_fd[%d],addr=%x,acq_fence_fd=%d",
            pzone_mag->zone_info[i].zone_index,
            pzone_mag->zone_info[i].layer_index,
            pzone_mag->zone_info[i].dispatched,
            pzone_mag->zone_info[i].src_rect.left,
            pzone_mag->zone_info[i].src_rect.top,
            pzone_mag->zone_info[i].src_rect.right,
            pzone_mag->zone_info[i].src_rect.bottom,
            pzone_mag->zone_info[i].disp_rect.left,
            pzone_mag->zone_info[i].disp_rect.top,
            pzone_mag->zone_info[i].disp_rect.right,
            pzone_mag->zone_info[i].disp_rect.bottom,
            pzone_mag->zone_info[i].width,
            pzone_mag->zone_info[i].height,
            pzone_mag->zone_info[i].stride,
            pzone_mag->zone_info[i].format,
            pzone_mag->zone_info[i].transform,
            pzone_mag->zone_info[i].realtransform,
            pzone_mag->zone_info[i].blend,
            pzone_mag->zone_info[i].layer_fd,
            pzone_mag->zone_info[i].addr,
            pzone_mag->zone_info[i].acq_fence_fd);
            switch(pzone_mag->zone_info[i].dispatched) {
                case win0:
                    bpvinfo.vopinfo[0].state = 1;
                    bpvinfo.vopinfo[0].zone_num ++;
                   break;
                case win1:
                    bpvinfo.vopinfo[1].state = 1;
                    bpvinfo.vopinfo[1].zone_num ++;
                    break;
                case win2_0:
                    bpvinfo.vopinfo[2].state = 1;
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_1:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_2:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_3:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                default:
                    ALOGE("hwc_dispatch_mix  err!");
                    return -1;
             }
        }
        bpvinfo.vopinfo[3].state = 1;
        bpvinfo.vopinfo[3].zone_num ++;
        bpvinfo.bp_size = Context->zone_manager.bp_size;
        tsize += Context->fbhandle.width * Context->fbhandle.height*4;
        if(tsize)
            tsize = tsize / (1024 *1024) * 60 ;// MB
        bpvinfo.bp_vop_size = tsize ;
        for(i= 0;i<4;i++) {
            ALOGD_IF(log(HLLFIV),"RK_QUEDDR_FREQ mixinfo win[%d] bo_size=%dMB,bp_vop_size=%dMB,state=%d,num=%d",
                i,bpvinfo.bp_size,bpvinfo.bp_vop_size,bpvinfo.vopinfo[i].state,bpvinfo.vopinfo[i].zone_num);
        }
        if(ioctl(Context->ddrFd, RK_QUEDDR_FREQ, &bpvinfo)) {
            if(log(HLLTHR)) {
                for(i= 0;i<4;i++) {
                    ALOGW("RK_QUEDDR_FREQ mixinfo win[%d] bo_size=%dMB,bp_vop_size=%dMB,state=%d,num=%d",
                        i,bpvinfo.bp_size,bpvinfo.bp_vop_size,bpvinfo.vopinfo[i].state,bpvinfo.vopinfo[i].zone_num);
                }
            }
            ALOGD_IF(log(HLLFOU),"Policy try again %s,%d ",__FUNCTION__,__LINE__);
            memset(&bpvinfo,0,sizeof(BpVopInfo));
            goto TryAgain;
        }
    }
#endif
    if ((large_cnt + bw) >= 5) {
        ALOGD_IF(log(HLLFOU),"Policy try again %s,%d ",__FUNCTION__,__LINE__);
        goto TryAgain;
    }

    if (gMixInfo.gpu_draw_fd[foundLayer] != 0) {
        for(i=0;i<foundLayer;i++) {
            list->hwLayers[i].compositionType = HWC_FRAMEBUFFER;
        }
        for(i=foundLayer;i<GPUDRAWCNT;i++) {
            gMixInfo.gpu_draw_fd[i] = 0;
        }
    }

    for (unsigned int i = 0; i < list->numHwLayers - 1; i ++) {
        hwc_layer_1_t * layer = &list->hwLayers[i];
        struct private_handle_t* hnd = (struct private_handle_t *)layer->handle;
        if (hnd && layer->compositionType == HWC_FRAMEBUFFER &&
                                  hnd->format == HAL_PIXEL_FORMAT_YCrCb_NV12_10)
            return -1;
    }

    memcpy(&Context->zone_manager,&zone_m,sizeof(ZoneManager));
    Context->zone_manager.mCmpType = HWC_MIX_DOWN;
    Context->zone_manager.composter_mode = HWC_MIX;
    memcpy((void*)&gmixinfo[mix_index],(void*)&gMixInfo,sizeof(gMixInfo));
    return 0;
}

//Refer to try_wins_dispatch_mix to deal with the case which exist ui transform layers.
//Unter the first transform layer,use lcdc to compose,equal or on the top of the transform layer,use gpu to compose
static int try_wins_dispatch_mix_v2 (void * ctx,hwc_display_contents_1_t * list)
{
#if OPTIMIZATION_FOR_TRANSFORM_UI
    int win_disphed_flag[3] = {0,}; // win0, win1, win2, win3 flag which is dispatched
    int win_disphed[3] = {win0,win1,win2_0};
    int i,j;
    int cntfb = 0;
    hwcContext * Context = (hwcContext *)ctx;
    ZoneManager zone_m;
    initLayerCompositionType(Context,list);
    memcpy(&zone_m,&Context->zone_manager,sizeof(ZoneManager));
    ZoneManager* pzone_mag = &zone_m;
    ZoneInfo    zone_info_ty[MaxZones];
    int sort = 1;
    int cnt = 0;
    int srot_tal[3][2] = {0,};
    int sort_stretch[3] = {0};
    int sort_pre;
    int gpu_draw = 0;
    float hfactor_max = 1.0;
    int large_cnt = 0;
    bool isyuv = false;
    int bw = 0;
    BpVopInfo  bpvinfo;
    int tsize = 0;
    int mix_index = 0;
    int mFtrfl = 0;
    int iFirstTransformLayer=-1;
    bool bTransform=false;
    mix_info gMixInfo;

    memset(&bpvinfo,0,sizeof(BpVopInfo));
    char const* compositionTypeName[] = {
            "win0",
            "win1",
            "win2_0",
            "win2_1",
            "win2_2",
            "win2_3",
            "win3_0",
            "win3_1",
            "win3_2",
            "win3_3",
            };
    hwcContext * contextAh = _contextAnchor;
    memset(&zone_info_ty,0,sizeof(zone_info_ty));
    if(Context == _contextAnchor1) {
        mix_index = 1;
    }else if(Context == _contextAnchor) {
        mix_index = 0;
    }
    if(pzone_mag->zone_cnt < 5) {
		ALOGD_IF(log(HLLFOU),"Policy out [%d][%s]",__LINE__,__FUNCTION__);
    	return -1;
    }
    //Find out which layer start transform.
    for(i=0;i<pzone_mag->zone_cnt;i++) {
        if(pzone_mag->zone_info[i].transform != 0) {
            mFtrfl = i;
            iFirstTransformLayer = pzone_mag->zone_info[i].layer_index;
            bTransform = true;
            break;
        }
    }

    if(Context->mAlphaError){
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }

    if(contextAh->mHdmiSI.NeedReDst){
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }

    if(Context->Is3D){
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }

#ifdef RK3288_BOX
    if(Context==_contextAnchor && Context->mResolutionChanged && Context->mLcdcNum==2){
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }
#endif

    if (is_primary_and_resolution_changed(Context)) {
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }

#if DUAL_VIEW_MODE
    if(Context != contextAh && Context->mIsDualViewMode) {
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }
#endif
    for(int k=0;k<mFtrfl;k++) {
        if(pzone_mag->zone_info[k].scale_err || pzone_mag->zone_info[k].toosmall
            || pzone_mag->zone_info[k].zone_err || pzone_mag->zone_info[k].transform
            || pzone_mag->zone_info[k].skipLayer || pzone_mag->zone_info[k].source_err) {
            ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
            return -1;
        }
    }

    //If not exist transform layers,then return.
    if(!bTransform) {
		ALOGD_IF(log(HLLFOU),"Policy out [%d][%s]",__LINE__,__FUNCTION__);
		return -1;
	}

    memcpy((void*)&gMixInfo,(void*)&gmixinfo[mix_index],sizeof(gMixInfo));
    for(i=0,j=0;i<pzone_mag->zone_cnt;i++) {
        //Set the layer which it's layer_index bigger than the first transform layer index to HWC_FRAMEBUFFER or HWC_NODRAW
        if(pzone_mag->zone_info[i].layer_index >= iFirstTransformLayer) {
            hwc_layer_1_t * layer = &list->hwLayers[pzone_mag->zone_info[i].layer_index];
            if(pzone_mag->zone_info[i].format == HAL_PIXEL_FORMAT_YCrCb_NV12) {
                ALOGD_IF(log(HLLTHR),"Donot support video[%d][%s]",__LINE__,__FUNCTION__);
                return -1;
            }
            //Judge the current layer whether backup in gmixinfo[mix_index] or not.
            if(Context->mLastCompType != HWC_MIX_VTWO
                || gMixInfo.lastZoneCrc[pzone_mag->zone_info[i].layer_index] != pzone_mag->zone_info[i].zoneCrc
                || gMixInfo.gpu_draw_fd[pzone_mag->zone_info[i].layer_index] != pzone_mag->zone_info[i].layer_fd
                || gMixInfo.alpha[pzone_mag->zone_info[i].layer_index] != pzone_mag->zone_info[i].zone_alpha) {
                gpu_draw = 1;
                layer->compositionType = HWC_FRAMEBUFFER;
                gMixInfo.lastZoneCrc[pzone_mag->zone_info[i].layer_index] = pzone_mag->zone_info[i].zoneCrc;
                gMixInfo.gpu_draw_fd[pzone_mag->zone_info[i].layer_index] = pzone_mag->zone_info[i].layer_fd;
                gMixInfo.alpha[pzone_mag->zone_info[i].layer_index] = pzone_mag->zone_info[i].zone_alpha;
            } else {
                layer->compositionType = HWC_NODRAW;
            }
            if(gpu_draw && pzone_mag->zone_info[i].layer_index > iFirstTransformLayer) {
                for(int j=iFirstTransformLayer;j<pzone_mag->zone_info[i].layer_index;j++) {
                    layer = &list->hwLayers[j];
                    layer->compositionType = HWC_FRAMEBUFFER;
                }
                ALOGV(" need draw by gpu");
            }
            cntfb ++;
        } else {
            //hwc_layer_1_t * layer = &list->hwLayers[pzone_mag->zone_info[i].layer_index];
            //layer->compositionType = HWC_MIX_V2;
            memcpy(&zone_info_ty[j], &pzone_mag->zone_info[i],sizeof(ZoneInfo));
            zone_info_ty[j].sort = 0;
            j++;
        }
    }
    memcpy(pzone_mag, &zone_info_ty,sizeof(zone_info_ty));
    pzone_mag->zone_cnt -= cntfb;
    for(i=0;i< pzone_mag->zone_cnt;i++)
    {
        ALOGD_IF(log(HLLFIV),"%s,%d:Zone[%d]->layer[%d],"
            "[%d,%d,%d,%d] =>[%d,%d,%d,%d],"
            "w_h_s_f[%d,%d,%d,%d],tr_rtr_bled[%d,%d,%d],acq_fence_fd=%d",
            __FUNCTION__,__LINE__,
            Context->zone_manager.zone_info[i].zone_index,
            Context->zone_manager.zone_info[i].layer_index,
            Context->zone_manager.zone_info[i].src_rect.left,
            Context->zone_manager.zone_info[i].src_rect.top,
            Context->zone_manager.zone_info[i].src_rect.right,
            Context->zone_manager.zone_info[i].src_rect.bottom,
            Context->zone_manager.zone_info[i].disp_rect.left,
            Context->zone_manager.zone_info[i].disp_rect.top,
            Context->zone_manager.zone_info[i].disp_rect.right,
            Context->zone_manager.zone_info[i].disp_rect.bottom,
            Context->zone_manager.zone_info[i].width,
            Context->zone_manager.zone_info[i].height,
            Context->zone_manager.zone_info[i].stride,
            Context->zone_manager.zone_info[i].format,
            Context->zone_manager.zone_info[i].transform,
            Context->zone_manager.zone_info[i].realtransform,
            Context->zone_manager.zone_info[i].blend,
            Context->zone_manager.zone_info[i].acq_fence_fd);
    }
    pzone_mag->zone_info[0].sort = sort;
    for(i=0;i<(pzone_mag->zone_cnt-1);)
    {
        pzone_mag->zone_info[i].sort = sort;
        sort_pre  = sort;
        cnt = 0;
        for(j=1;j<4 && (i+j) < pzone_mag->zone_cnt;j++)
        {
            ZoneInfo * next_zf = &(pzone_mag->zone_info[i+j]);
            bool is_combine = false;
            int k;
            for(k=0;k<=cnt;k++)  // compare all sorted_zone info
            {
                ZoneInfo * sorted_zf = &(pzone_mag->zone_info[i+j-1-k]);
                if(is_zone_combine(sorted_zf,next_zf))
                {
                    is_combine = true;
                }
                else
                {
                    is_combine = false;
                    break;
                }
            }
            if(is_combine)
            {
                pzone_mag->zone_info[i+j].sort = sort;
                cnt++;
            }
            else
            {
                sort++;
                pzone_mag->zone_info[i+j].sort = sort;
                cnt++;
                break;
            }
        }
        if( sort_pre == sort && (i+cnt) < (pzone_mag->zone_cnt-1) )  // win2 ,4zones ,win3 4zones,so sort ++,but exit not ++
            sort ++;
        i += cnt;
    }
    if(sort >3)  // lcdc dont support 5 wins
    {
        ALOGD_IF(log(HLLTHR),"lcdc dont support 5 wins");
        return -1;
    }
    for(i=0;i<pzone_mag->zone_cnt;i++)
    {
        int factor =1;
        ALOGV("sort[%d].type=%d",i,pzone_mag->zone_info[i].sort);
        if( pzone_mag->zone_info[i].sort == 1){
            srot_tal[0][0]++;
            if(pzone_mag->zone_info[i].is_stretch)
                sort_stretch[0] = 1;
        }
        else if(pzone_mag->zone_info[i].sort == 2){
            srot_tal[1][0]++;
            if(pzone_mag->zone_info[i].is_stretch)
                sort_stretch[1] = 1;
        }
        else if(pzone_mag->zone_info[i].sort == 3){
            srot_tal[2][0]++;
            if(pzone_mag->zone_info[i].is_stretch)
                sort_stretch[2] = 1;
        }
        if(pzone_mag->zone_info[i].hfactor > hfactor_max)
        {
            hfactor_max = pzone_mag->zone_info[i].hfactor;
        }
        if(pzone_mag->zone_info[i].is_large )
        {
            large_cnt ++;
        }
        if(pzone_mag->zone_info[i].format== HAL_PIXEL_FORMAT_YCrCb_NV12)
        {
            isyuv = true;
        }
        if(Context->zone_manager.zone_info[i].hfactor > 1.0)
            factor = 2;
        else
            factor = 1;
        tsize += (Context->zone_manager.zone_info[i].size *factor);
    }
    j = 0;
    for(i=0;i<3;i++)
    {
        if( srot_tal[i][0] >=2)  // > twice zones
        {
            srot_tal[i][1] = win_disphed[j+2];
            win_disphed_flag[j+2] = 1; // win2 ,win3 is dispatch flag
            ALOGV("more twice zones srot_tal[%d][1]=%d",i,srot_tal[i][1]);
            j++;
            if(j > 1)  // lcdc only has win2 and win3 supprot more zones
            {
                ALOGD("lcdc only has win2 and win3 supprot more zones");
                return -1;
            }
        }
    }
    j = 0;
    for(i=0;i<3;i++)
    {
        if( sort_stretch[i] == 1)  // strech
        {
            srot_tal[i][1] = win_disphed[j];  // win 0 and win 1 suporot stretch
            win_disphed_flag[j] = 1; // win0 ,win1 is dispatch flag
            ALOGV("stretch zones srot_tal[%d][1]=%d",i,srot_tal[i][1]);
            j++;
            if(j > 2)  // lcdc only has win0 and win1 supprot stretch
            {
                //ALOGD("lcdc only has win0 and win1 supprot stretch");
                return -1;
            }
        }
    }
    if(hfactor_max >=1.4)
    {
        bw += (j + 1);

    }
    if(isyuv)
    {
        bw +=5;
    }
    //ALOGD("large_cnt =%d,bw=%d",large_cnt , bw);

    for(i=0;i<3;i++)
    {
        if( srot_tal[i][1] == 0)  // had not dispatched
        {
            for(j=0;j<3;j++)
            {
                if(win_disphed_flag[j] == 0) // find the win had not dispatched
                    break;
            }
            if(j>=3)
            {
                ALOGE("3 wins had beed dispatched ");
                return -1;
            }
            srot_tal[i][1] = win_disphed[j];
            win_disphed_flag[j] = 1;
            ALOGV("srot_tal[%d][1].dispatched=%d",i,srot_tal[i][1]);
        }
    }
    for(i=0;i<pzone_mag->zone_cnt;i++)
    {
         switch(pzone_mag->zone_info[i].sort) {
            case 1:
                pzone_mag->zone_info[i].dispatched = srot_tal[0][1]++;
                break;
            case 2:
                pzone_mag->zone_info[i].dispatched = srot_tal[1][1]++;
                break;
            case 3:
                pzone_mag->zone_info[i].dispatched = srot_tal[2][1]++;
                break;
            default:
                ALOGE("try_wins_dispatch_mix_v2 sort err!");
                return -1;
        }
        ALOGV("zone[%d].dispatched[%d]=%s,sort=%d", \
        i,pzone_mag->zone_info[i].dispatched,
        compositionTypeName[pzone_mag->zone_info[i].dispatched -1],
        pzone_mag->zone_info[i].sort);
    }

    for(i=0;i<pzone_mag->zone_cnt;i++){
        int disptched = pzone_mag->zone_info[i].dispatched;
        int sct_width = pzone_mag->zone_info[i].src_rect.right
                                            - pzone_mag->zone_info[i].src_rect.left;
        int sct_height = pzone_mag->zone_info[i].src_rect.bottom
                                            - pzone_mag->zone_info[i].src_rect.top;
        int dst_width = pzone_mag->zone_info[i].disp_rect.right
                                            - pzone_mag->zone_info[i].disp_rect.left;
        int dst_height = pzone_mag->zone_info[i].disp_rect.bottom
                                            - pzone_mag->zone_info[i].disp_rect.top;
        /*win2 win3 not support YUV*/
        if(disptched > win1 && is_yuv(pzone_mag->zone_info[i].format))
            return -1;
        /*scal not support whoes source bigger than 2560 to dst 4k*/
        if(disptched <= win1 &&(sct_width > 2160 || sct_height > 2160) &&
            !is_yuv(pzone_mag->zone_info[i].format) && contextAh->mHdmiSI.NeedReDst)
            return -1;
        if(disptched <= win1 && (sct_width > 2560 || dst_width > 2560) &&
                                             !is_yuv(pzone_mag->zone_info[i].format)
                        && (sct_height != dst_height || Context->mResolutionChanged))
            return -1;
    }

#if USE_QUEUE_DDRFREQ
    if(Context->ddrFd > 0)
    {
        for(i=0;i<pzone_mag->zone_cnt;i++)
        {
            int area_no = 0;
            int win_id = 0;
            ALOGD_IF(log(HLLFIV),"Zone[%d]->layer[%d],dispatched=%d,"
            "[%d,%d,%d,%d] =>[%d,%d,%d,%d],"
            "w_h_s_f[%d,%d,%d,%d],tr_rtr_bled[%d,%d,%d],"
            "layer_fd[%d],addr=%x,acq_fence_fd=%d",
            pzone_mag->zone_info[i].zone_index,
            pzone_mag->zone_info[i].layer_index,
            pzone_mag->zone_info[i].dispatched,
            pzone_mag->zone_info[i].src_rect.left,
            pzone_mag->zone_info[i].src_rect.top,
            pzone_mag->zone_info[i].src_rect.right,
            pzone_mag->zone_info[i].src_rect.bottom,
            pzone_mag->zone_info[i].disp_rect.left,
            pzone_mag->zone_info[i].disp_rect.top,
            pzone_mag->zone_info[i].disp_rect.right,
            pzone_mag->zone_info[i].disp_rect.bottom,
            pzone_mag->zone_info[i].width,
            pzone_mag->zone_info[i].height,
            pzone_mag->zone_info[i].stride,
            pzone_mag->zone_info[i].format,
            pzone_mag->zone_info[i].transform,
            pzone_mag->zone_info[i].realtransform,
            pzone_mag->zone_info[i].blend,
            pzone_mag->zone_info[i].layer_fd,
            pzone_mag->zone_info[i].addr,
            pzone_mag->zone_info[i].acq_fence_fd);
            switch(pzone_mag->zone_info[i].dispatched) {
                case win0:
                    bpvinfo.vopinfo[0].state = 1;
                    bpvinfo.vopinfo[0].zone_num ++;
                   break;
                case win1:
                    bpvinfo.vopinfo[1].state = 1;
                    bpvinfo.vopinfo[1].zone_num ++;
                    break;
                case win2_0:
                    bpvinfo.vopinfo[2].state = 1;
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_1:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_2:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_3:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                default:
                    ALOGE("hwc_dispatch_mix  err!");
                    return -1;
             }
        }
        bpvinfo.vopinfo[3].state = 1;
        bpvinfo.vopinfo[3].zone_num ++;
        bpvinfo.bp_size = Context->zone_manager.bp_size;
        tsize += Context->fbhandle.width * Context->fbhandle.height*4;
        if(tsize)
            tsize = tsize / (1024 *1024) * 60 ;// MB
        bpvinfo.bp_vop_size = tsize ;
        for(i= 0;i<4;i++)
        {
            ALOGD_IF(log(HLLFIV),"RK_QUEDDR_FREQ mixinfo win[%d] bo_size=%dMB,bp_vop_size=%dMB,state=%d,num=%d",
                i,bpvinfo.bp_size,bpvinfo.bp_vop_size,bpvinfo.vopinfo[i].state,bpvinfo.vopinfo[i].zone_num);
        }
        if(ioctl(Context->ddrFd, RK_QUEDDR_FREQ, &bpvinfo))
        {
            if(log(HLLTHR))
            {
                for(i= 0;i<4;i++)
                {
                    ALOGD("RK_QUEDDR_FREQ mixinfo win[%d] bo_size=%dMB,bp_vop_size=%dMB,state=%d,num=%d",
                        i,bpvinfo.bp_size,bpvinfo.bp_vop_size,bpvinfo.vopinfo[i].state,bpvinfo.vopinfo[i].zone_num);
                }
            }
            return -1;
        }
    }
#endif
    if ((large_cnt + bw) >= 5) {
        ALOGD_IF(log(HLLTHR),"lagre win > 2,and Scale-down 1.5 multiple,lcdc no support");
        return -1;
    }
    if (list) {
        for(int i=0;i<iFirstTransformLayer;i++) {
            list->hwLayers[i].compositionType = HWC_MIX_V2;
        }
    }

    for (unsigned int i = 0; i < list->numHwLayers - 1; i ++) {
        hwc_layer_1_t * layer = &list->hwLayers[i];
        struct private_handle_t* hnd = (struct private_handle_t *)layer->handle;
        if (hnd && layer->compositionType == HWC_FRAMEBUFFER &&
                                  hnd->format == HAL_PIXEL_FORMAT_YCrCb_NV12_10)
            return -1;
    }

    //Mark the composer mode to HWC_MIX_V2
    memcpy(&Context->zone_manager,&zone_m,sizeof(ZoneManager));
    Context->zone_manager.mCmpType = HWC_MIX_VTWO;
    Context->zone_manager.composter_mode = HWC_MIX_V2;
    memcpy((void*)&gmixinfo[mix_index],(void*)&gMixInfo,sizeof(gMixInfo));
    return 0;
#else
    return -1;
#endif
}

static int try_wins_mix_fp_stereo (void * ctx,hwc_display_contents_1_t * list)
{
    int win_disphed_flag[3] = {0,}; // win0, win1, win2, win3 flag which is dispatched
    int win_disphed[3] = {win0,win1,win2_0};
    int i,j;
    int cntfb = 0;
    ZoneManager zone_m;
    hwcContext * Context = (hwcContext *)ctx;
    memcpy(&zone_m,&Context->zone_manager,sizeof(ZoneManager));
    ZoneManager* pzone_mag = &zone_m;
    ZoneInfo    zone_info_ty[MaxZones];
    int sort = 1;
    int cnt = 0;
    int srot_tal[3][2] = {0,};
    int sort_stretch[3] = {0};
    int sort_pre;
    int gpu_draw = 0;
    float hfactor_max = 1.0;
    int large_cnt = 0;
    bool isyuv = false;
    int bw = 0;
    BpVopInfo  bpvinfo;
    int tsize = 0;
    int nFisrstFPS = -1;
    int nFinalFPS = 10;
    int mix_index = 0;
    int iFirstTransformLayer=-1;
    bool bTransform=false;
    bool isNotSupportOverlay = true;
    bool hasFpsLyaer = false;

    memset(&bpvinfo,0,sizeof(BpVopInfo));
    char const* compositionTypeName[] = {
            "win0",
            "win1",
            "win2_0",
            "win2_1",
            "win2_2",
            "win2_3",
            "win3_0",
            "win3_1",
            "win3_2",
            "win3_3",
            };
    hwcContext * contextAh = _contextAnchor;
    memset(&zone_info_ty,0,sizeof(zone_info_ty));
    if(Context == _contextAnchor1){
        mix_index = 1;
    }else if(Context == _contextAnchor){
        mix_index = 0;
    }
#if DUAL_VIEW_MODE
    if(Context != contextAh && Context->mIsDualViewMode) {
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }
#endif
    if(pzone_mag->zone_info[0].scale_err || pzone_mag->zone_info[0].toosmall
        || pzone_mag->zone_info[0].zone_err || pzone_mag->zone_info[0].transform
        || pzone_mag->zone_info[0].skipLayer || pzone_mag->zone_info[0].source_err) {
        return -1;
    }

    for(i = 0; i < pzone_mag->zone_cnt; i++) {
        if(pzone_mag->zone_info[i].alreadyStereo == 8) {
            hasFpsLyaer = true;
        }
    }

    if(!hasFpsLyaer) {
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }

    if(pzone_mag->zone_cnt > 2) {
        isNotSupportOverlay = true;
    }

    for(i=0;i<pzone_mag->zone_cnt;i++) {
        if(nFisrstFPS == -1 && pzone_mag->zone_info[i].alreadyStereo == 8) {
            nFisrstFPS = i;
        }
    }

    nFinalFPS = pzone_mag->zone_cnt;
    for(i=pzone_mag->zone_cnt-1;i>=0;i--) {
        if(nFinalFPS == pzone_mag->zone_cnt && pzone_mag->zone_info[i].alreadyStereo == 8) {
            nFinalFPS = i;
        }
    }

    if(nFinalFPS == pzone_mag->zone_cnt) {
        nFinalFPS = 0;
    }
    for(i=0,j=0;i<pzone_mag->zone_cnt;i++) {
        if(pzone_mag->zone_info[i].layer_index < nFinalFPS) {
            hwc_layer_1_t * layer = &list->hwLayers[pzone_mag->zone_info[i].layer_index];
            layer->compositionType = HWC_MIX_V2; //NO_DRAW
            cntfb ++;
        } else if (pzone_mag->zone_info[i].alreadyStereo != 8) {
            hwc_layer_1_t * layer = &list->hwLayers[pzone_mag->zone_info[i].layer_index];
            layer->compositionType = HWC_FRAMEBUFFER;
            cntfb ++;
        } else {
            memcpy(&zone_info_ty[j], &pzone_mag->zone_info[i],sizeof(ZoneInfo));
            zone_info_ty[j].sort = 0;
            j++;
        }
    }

    memcpy(pzone_mag, &zone_info_ty,sizeof(zone_info_ty));
    pzone_mag->zone_cnt -= cntfb;
    for(i=0;i< pzone_mag->zone_cnt;i++) {
        ALOGD_IF(log(HLLTWO),"Zone[%d]->layer[%d],"
            "[%d,%d,%d,%d] =>[%d,%d,%d,%d],"
            "w_h_s_f[%d,%d,%d,%d],tr_rtr_bled[%d,%d,%d],acq_fence_fd=%d",
            Context->zone_manager.zone_info[i].zone_index,
            Context->zone_manager.zone_info[i].layer_index,
            Context->zone_manager.zone_info[i].src_rect.left,
            Context->zone_manager.zone_info[i].src_rect.top,
            Context->zone_manager.zone_info[i].src_rect.right,
            Context->zone_manager.zone_info[i].src_rect.bottom,
            Context->zone_manager.zone_info[i].disp_rect.left,
            Context->zone_manager.zone_info[i].disp_rect.top,
            Context->zone_manager.zone_info[i].disp_rect.right,
            Context->zone_manager.zone_info[i].disp_rect.bottom,
            Context->zone_manager.zone_info[i].width,
            Context->zone_manager.zone_info[i].height,
            Context->zone_manager.zone_info[i].stride,
            Context->zone_manager.zone_info[i].format,
            Context->zone_manager.zone_info[i].transform,
            Context->zone_manager.zone_info[i].realtransform,
            Context->zone_manager.zone_info[i].blend,
            Context->zone_manager.zone_info[i].acq_fence_fd);
    }
    pzone_mag->zone_info[0].sort = sort;
    for(i=0;i<(pzone_mag->zone_cnt-1);)
    {
        pzone_mag->zone_info[i].sort = sort;
        sort_pre  = sort;
        cnt = 0;
        for(j=1;j<4 && (i+j) < pzone_mag->zone_cnt;j++)
        {
            ZoneInfo * next_zf = &(pzone_mag->zone_info[i+j]);
            bool is_combine = false;
            int k;
            for(k=0;k<=cnt;k++)  // compare all sorted_zone info
            {
                ZoneInfo * sorted_zf = &(pzone_mag->zone_info[i+j-1-k]);
                if(is_zone_combine(sorted_zf,next_zf))
                {
                    is_combine = true;
                }
                else
                {
                    is_combine = false;
                    break;
                }
            }
            if(is_combine)
            {
                pzone_mag->zone_info[i+j].sort = sort;
                cnt++;
            }
            else
            {
                sort++;
                pzone_mag->zone_info[i+j].sort = sort;
                cnt++;
                break;
            }
        }
        if( sort_pre == sort && (i+cnt) < (pzone_mag->zone_cnt-1) )  // win2 ,4zones ,win3 4zones,so sort ++,but exit not ++
            sort ++;
        i += cnt;
    }
    if(sort >3)  // lcdc dont support 5 wins
    {
        ALOGD_IF(log(HLLTWO),"lcdc dont support 5 wins [%d]",__LINE__);
        return -1;
    }
    for(i=0;i<pzone_mag->zone_cnt;i++)
    {
        int factor =1;
        ALOGV("sort[%d].type=%d",i,pzone_mag->zone_info[i].sort);
        if( pzone_mag->zone_info[i].sort == 1){
            srot_tal[0][0]++;
            if(pzone_mag->zone_info[i].is_stretch)
                sort_stretch[0] = 1;
        }
        else if(pzone_mag->zone_info[i].sort == 2){
            srot_tal[1][0]++;
            if(pzone_mag->zone_info[i].is_stretch)
                sort_stretch[1] = 1;
        }
        else if(pzone_mag->zone_info[i].sort == 3){
            srot_tal[2][0]++;
            if(pzone_mag->zone_info[i].is_stretch)
                sort_stretch[2] = 1;
        }
        if(pzone_mag->zone_info[i].hfactor > hfactor_max)
        {
            hfactor_max = pzone_mag->zone_info[i].hfactor;
        }
        if(pzone_mag->zone_info[i].is_large )
        {
            large_cnt ++;
        }
        if(pzone_mag->zone_info[i].format== HAL_PIXEL_FORMAT_YCrCb_NV12)
        {
            isyuv = true;
        }
        if(Context->zone_manager.zone_info[i].hfactor > 1.0)
            factor = 2;
        else
            factor = 1;
        tsize += (Context->zone_manager.zone_info[i].size *factor);
    }
    j = 0;
    for(i=0;i<3;i++)
    {
        if( srot_tal[i][0] >=2)  // > twice zones
        {
            srot_tal[i][1] = win_disphed[j+2];
            win_disphed_flag[j+2] = 1; // win2 ,win3 is dispatch flag
            ALOGV("more twice zones srot_tal[%d][1]=%d",i,srot_tal[i][1]);
            j++;
            if(j > 1)  // lcdc only has win2 and win3 supprot more zones
            {
                ALOGD("lcdc only has win2 and win3 supprot more zones");
                return -1;
            }
        }
    }
    j = 0;
    for(i=0;i<3;i++)
    {
        if( sort_stretch[i] == 1)  // strech
        {
            srot_tal[i][1] = win_disphed[j];  // win 0 and win 1 suporot stretch
            win_disphed_flag[j] = 1; // win0 ,win1 is dispatch flag
            ALOGV("stretch zones srot_tal[%d][1]=%d",i,srot_tal[i][1]);
            j++;
            if(j > 2)  // lcdc only has win0 and win1 supprot stretch
            {
                ALOGD("lcdc only has win0 and win1 supprot stretch");
                return -1;
            }
        }
    }
    if(hfactor_max >=1.4)
    {
        bw += (j + 1);
    }
    if(isyuv)
    {
        bw +=5;
    }
    ALOGV("large_cnt =%d,bw=%d",large_cnt , bw);
    for(i=0;i<3;i++)
    {
        if( srot_tal[i][1] == 0)  // had not dispatched
        {
            for(j=0;j<3;j++)
            {
                if(win_disphed_flag[j] == 0) // find the win had not dispatched
                    break;
            }
            if(j>=3)
            {
                ALOGE("3 wins had beed dispatched ");
                return -1;
            }
            srot_tal[i][1] = win_disphed[j];
            win_disphed_flag[j] = 1;
            ALOGV("srot_tal[%d][1].dispatched=%d",i,srot_tal[i][1]);
        }
    }

    for(i=0;i<pzone_mag->zone_cnt;i++)
    {
         switch(pzone_mag->zone_info[i].sort) {
            case 1:
                pzone_mag->zone_info[i].dispatched = srot_tal[0][1]++;
                break;
            case 2:
                pzone_mag->zone_info[i].dispatched = srot_tal[1][1]++;
                break;
            case 3:
                pzone_mag->zone_info[i].dispatched = srot_tal[2][1]++;
                break;
            default:
                ALOGE("try_wins_dispatch_mix_vh sort err!");
                return -1;
        }
        ALOGV("zone[%d].dispatched[%d]=%s,sort=%d", \
        i,pzone_mag->zone_info[i].dispatched,
        compositionTypeName[pzone_mag->zone_info[i].dispatched -1],
        pzone_mag->zone_info[i].sort);
    }

    for(i=0;i<pzone_mag->zone_cnt;i++){
        int disptched = pzone_mag->zone_info[i].dispatched;
        int sct_width = pzone_mag->zone_info[i].src_rect.right
                                            - pzone_mag->zone_info[i].src_rect.left;
        int sct_height = pzone_mag->zone_info[i].src_rect.bottom
                                            - pzone_mag->zone_info[i].src_rect.top;
        int dst_width = pzone_mag->zone_info[i].disp_rect.right
                                            - pzone_mag->zone_info[i].disp_rect.left;
        int dst_height = pzone_mag->zone_info[i].disp_rect.bottom
                                            - pzone_mag->zone_info[i].disp_rect.top;
        /*win2 win3 not support YUV*/
        if(disptched > win1 && is_yuv(pzone_mag->zone_info[i].format))
            return -1;
        /*scal not support whoes source bigger than 2560 to dst 4k*/
        if(disptched <= win1 &&(sct_width > 2160 || sct_height > 2160) &&
            !is_yuv(pzone_mag->zone_info[i].format) && contextAh->mHdmiSI.NeedReDst)
            return -1;
        if(disptched <= win1 && (sct_width > 2560 || dst_width > 2560) &&
                                             !is_yuv(pzone_mag->zone_info[i].format)
                        && (sct_height != dst_height || Context->mResolutionChanged))
            return -1;
    }

#if USE_QUEUE_DDRFREQ
    if(Context->ddrFd > 0)
    {
        for(i=0;i<pzone_mag->zone_cnt;i++)
        {
            int area_no = 0;
            int win_id = 0;
            ALOGD_IF(log(HLLTHR),"Zone[%d]->layer[%d],dispatched=%d,"
            "[%d,%d,%d,%d] =>[%d,%d,%d,%d],"
            "w_h_s_f[%d,%d,%d,%d],tr_rtr_bled[%d,%d,%d],"
            "layer_fd[%d],addr=%x,acq_fence_fd=%d",
            pzone_mag->zone_info[i].zone_index,
            pzone_mag->zone_info[i].layer_index,
            pzone_mag->zone_info[i].dispatched,
            pzone_mag->zone_info[i].src_rect.left,
            pzone_mag->zone_info[i].src_rect.top,
            pzone_mag->zone_info[i].src_rect.right,
            pzone_mag->zone_info[i].src_rect.bottom,
            pzone_mag->zone_info[i].disp_rect.left,
            pzone_mag->zone_info[i].disp_rect.top,
            pzone_mag->zone_info[i].disp_rect.right,
            pzone_mag->zone_info[i].disp_rect.bottom,
            pzone_mag->zone_info[i].width,
            pzone_mag->zone_info[i].height,
            pzone_mag->zone_info[i].stride,
            pzone_mag->zone_info[i].format,
            pzone_mag->zone_info[i].transform,
            pzone_mag->zone_info[i].realtransform,
            pzone_mag->zone_info[i].blend,
            pzone_mag->zone_info[i].layer_fd,
            pzone_mag->zone_info[i].addr,
            pzone_mag->zone_info[i].acq_fence_fd);
            switch(pzone_mag->zone_info[i].dispatched) {
                case win0:
                    bpvinfo.vopinfo[0].state = 1;
                    bpvinfo.vopinfo[0].zone_num ++;
                   break;
                case win1:
                    bpvinfo.vopinfo[1].state = 1;
                    bpvinfo.vopinfo[1].zone_num ++;
                    break;
                case win2_0:
                    bpvinfo.vopinfo[2].state = 1;
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_1:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_2:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_3:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                default:
                    ALOGE("hwc_dispatch_mix  err!");
                    return -1;
             }
        }
        bpvinfo.vopinfo[3].state = 1;
        bpvinfo.vopinfo[3].zone_num ++;
        bpvinfo.bp_size = Context->zone_manager.bp_size;
        tsize += Context->fbhandle.width * Context->fbhandle.height*4;
        if(tsize)
            tsize = tsize / (1024 *1024) * 60 ;// MB
        bpvinfo.bp_vop_size = tsize ;
        for(i= 0;i<4;i++)
        {
            ALOGD_IF(log(HLLTHR),"RK_QUEDDR_FREQ mixinfo win[%d] bo_size=%dMB,bp_vop_size=%dMB,state=%d,num=%d",
                i,bpvinfo.bp_size,bpvinfo.bp_vop_size,bpvinfo.vopinfo[i].state,bpvinfo.vopinfo[i].zone_num);
        }
        if(ioctl(Context->ddrFd, RK_QUEDDR_FREQ, &bpvinfo))
        {
            for(i= 0;i<4;i++)
            {
                ALOGD("RK_QUEDDR_FREQ mixinfo win[%d] bo_size=%dMB,bp_vop_size=%dMB,state=%d,num=%d",
                    i,bpvinfo.bp_size,bpvinfo.bp_vop_size,bpvinfo.vopinfo[i].state,bpvinfo.vopinfo[i].zone_num);
            }
            return -1;
        }
    }
#endif
    //Mark the composer mode to HWC_MIX_V2
    if(list){
        list->hwLayers[0].compositionType = HWC_MIX_V2;
    }

    memcpy(&Context->zone_manager,&zone_m,sizeof(ZoneManager));
    Context->zone_manager.mCmpType = HWC_MIX_FPS;
    Context->zone_manager.composter_mode = HWC_MIX_V2;
    return 0;
}

static int try_wins_dispatch_mix_vh (void * ctx,hwc_display_contents_1_t * list)
{
    int win_disphed_flag[3] = {0,}; // win0, win1, win2, win3 flag which is dispatched
    int win_disphed[3] = {win0,win1,win2_0};
    int i,j;
    int cntfb = 0;
    hwcContext * Context = (hwcContext *)ctx;
    hwcContext * ctxp = _contextAnchor;
    hwcContext * ctxe = _contextAnchor1;
    ZoneManager zone_m;
    initLayerCompositionType(Context,list);
    memcpy(&zone_m,&Context->zone_manager,sizeof(ZoneManager));
    ZoneManager* pzone_mag = &zone_m;
    ZoneInfo    zone_info_ty[MaxZones];
    int sort = 1;
    int cnt = 0;
    int srot_tal[3][2] = {0,};
    int sort_stretch[3] = {0};
    int sort_pre;
    int gpu_draw = 0;
    float hfactor_max = 1.0;
    int large_cnt = 0;
    bool isyuv = false;
    int bw = 0;
    BpVopInfo  bpvinfo;
    int tsize = 0;
    int mix_index = 0;
    int iLowerZorderLayer = 0;
    bool bTransform=false;
    mix_info gMixInfo;

    memset(&bpvinfo,0,sizeof(BpVopInfo));
    char const* compositionTypeName[] = {
            "win0",
            "win1",
            "win2_0",
            "win2_1",
            "win2_2",
            "win2_3",
            "win3_0",
            "win3_1",
            "win3_2",
            "win3_3",
            };
    hwcContext * contextAh = _contextAnchor;
    memset(&zone_info_ty,0,sizeof(zone_info_ty));

    if (Context == ctxe)        mix_index = 1;
    else if (Context == ctxp)   mix_index = 0;

    if (Context == ctxe && ctxp->isRk3399 && ctxp->mHdmiSI.NeedReDst) {
        ALOGD_IF(log(HLLFOU),"Policy out %s,%d",__func__,__LINE__);
        return -1;
    }

    if (list->numHwLayers - 1 < 2) {
        ALOGD_IF(log(HLLFOU),"Policy out %s,%d",__func__,__LINE__);
    	return -1;
    }

    if (Context->mAlphaError) {
        ALOGD_IF(log(HLLFOU),"Policy out %s,%d",__func__,__LINE__);
        return -1;
    }

    for(int k = 0; k < 1; k++) {
        if(pzone_mag->zone_info[k].scale_err || pzone_mag->zone_info[k].toosmall
            || pzone_mag->zone_info[k].zone_err || pzone_mag->zone_info[k].transform
            || pzone_mag->zone_info[k].skipLayer || pzone_mag->zone_info[k].source_err) {
            ALOGD_IF(log(HLLFOU),"%s,%d",__func__,__LINE__);
            return -1;
        }
    }

    iLowerZorderLayer = pzone_mag->zone_info[0].layer_index;
    for (int k = 1; k < pzone_mag->zone_cnt; k++) {
        if (iLowerZorderLayer > pzone_mag->zone_info[k].layer_index)
            iLowerZorderLayer = pzone_mag->zone_info[k].layer_index;
    }

    memcpy((void*)&gMixInfo,(void*)&gmixinfo[mix_index],sizeof(gMixInfo));
    for(i = 0,j = 0; i < pzone_mag->zone_cnt; i++)
    {
        //Set the layer which it's layer_index bigger than the first transform layer
        //index to HWC_FRAMEBUFFER or HWC_NODRAW
        if(pzone_mag->zone_info[i].layer_index > iLowerZorderLayer)
        {
            hwc_layer_1_t * layer = &list->hwLayers[pzone_mag->zone_info[i].layer_index];
            //Judge the current layer whether backup in gmixinfo[mix_index] or not.
            if(Context->mLastCompType != HWC_MIX_VH
                || gMixInfo.lastZoneCrc[pzone_mag->zone_info[i].layer_index] != pzone_mag->zone_info[i].zoneCrc
                || gMixInfo.gpu_draw_fd[pzone_mag->zone_info[i].layer_index] != pzone_mag->zone_info[i].layer_fd
                || gMixInfo.alpha[pzone_mag->zone_info[i].layer_index] != pzone_mag->zone_info[i].zone_alpha) {
                gpu_draw = 1;
                layer->compositionType = HWC_FRAMEBUFFER;
                gMixInfo.lastZoneCrc[pzone_mag->zone_info[i].layer_index] = pzone_mag->zone_info[i].zoneCrc;
                gMixInfo.gpu_draw_fd[pzone_mag->zone_info[i].layer_index] = pzone_mag->zone_info[i].layer_fd;
                gMixInfo.alpha[pzone_mag->zone_info[i].layer_index] = pzone_mag->zone_info[i].zone_alpha;
            }
            else
            {
                layer->compositionType = HWC_FRAMEBUFFER;
            }
            if(gpu_draw && pzone_mag->zone_info[i].layer_index > 0)
            {
                for(int j=1;j<pzone_mag->zone_info[i].layer_index;j++)
                {
                    layer = &list->hwLayers[j];
                    layer->compositionType = HWC_FRAMEBUFFER;
                }
                ALOGV(" need draw by gpu");
            }
            cntfb ++;
        }
        else
        {
            memcpy(&zone_info_ty[j], &pzone_mag->zone_info[i],sizeof(ZoneInfo));
            zone_info_ty[j].sort = 0;
            j++;
        }
    }
    memcpy(pzone_mag, &zone_info_ty,sizeof(zone_info_ty));
    pzone_mag->zone_cnt -= cntfb;
    for(i=0;i< pzone_mag->zone_cnt;i++)
    {
        ALOGD_IF(log(HLLFIV),"%s,%d:Zone[%d]->layer[%d],"
            "[%d,%d,%d,%d] =>[%d,%d,%d,%d],"
            "w_h_s_f[%d,%d,%d,%d],tr_rtr_bled[%d,%d,%d],acq_fence_fd=%d",
            __FUNCTION__,__LINE__,
            Context->zone_manager.zone_info[i].zone_index,
            Context->zone_manager.zone_info[i].layer_index,
            Context->zone_manager.zone_info[i].src_rect.left,
            Context->zone_manager.zone_info[i].src_rect.top,
            Context->zone_manager.zone_info[i].src_rect.right,
            Context->zone_manager.zone_info[i].src_rect.bottom,
            Context->zone_manager.zone_info[i].disp_rect.left,
            Context->zone_manager.zone_info[i].disp_rect.top,
            Context->zone_manager.zone_info[i].disp_rect.right,
            Context->zone_manager.zone_info[i].disp_rect.bottom,
            Context->zone_manager.zone_info[i].width,
            Context->zone_manager.zone_info[i].height,
            Context->zone_manager.zone_info[i].stride,
            Context->zone_manager.zone_info[i].format,
            Context->zone_manager.zone_info[i].transform,
            Context->zone_manager.zone_info[i].realtransform,
            Context->zone_manager.zone_info[i].blend,
            Context->zone_manager.zone_info[i].acq_fence_fd);
    }
    pzone_mag->zone_info[0].sort = sort;
    for(i=0;i<(pzone_mag->zone_cnt-1);)
    {
        pzone_mag->zone_info[i].sort = sort;
        sort_pre  = sort;
        cnt = 0;
        for(j=1;j<4 && (i+j) < pzone_mag->zone_cnt;j++)
        {
            ZoneInfo * next_zf = &(pzone_mag->zone_info[i+j]);
            bool is_combine = false;
            int k;
            for(k=0;k<=cnt;k++)  // compare all sorted_zone info
            {
                ZoneInfo * sorted_zf = &(pzone_mag->zone_info[i+j-1-k]);
                if(is_zone_combine(sorted_zf,next_zf))
                {
                    is_combine = true;
                }
                else
                {
                    is_combine = false;
                    break;
                }
            }
            if(is_combine)
            {
                pzone_mag->zone_info[i+j].sort = sort;
                cnt++;
            }
            else
            {
                sort++;
                pzone_mag->zone_info[i+j].sort = sort;
                cnt++;
                break;
            }
        }
        if( sort_pre == sort && (i+cnt) < (pzone_mag->zone_cnt-1) )  // win2 ,4zones ,win3 4zones,so sort ++,but exit not ++
            sort ++;
        i += cnt;
    }
    if(sort >3)  // lcdc dont support 5 wins
    {
        ALOGD_IF(log(HLLTHR),"lcdc dont support 5 wins [%d]",__LINE__);
        return -1;
    }
    for(i=0;i<pzone_mag->zone_cnt;i++)
    {
        int factor =1;
        ALOGV("sort[%d].type=%d",i,pzone_mag->zone_info[i].sort);
        if( pzone_mag->zone_info[i].sort == 1){
            srot_tal[0][0]++;
            if(pzone_mag->zone_info[i].is_stretch)
                sort_stretch[0] = 1;
        }
        else if(pzone_mag->zone_info[i].sort == 2){
            srot_tal[1][0]++;
            if(pzone_mag->zone_info[i].is_stretch)
                sort_stretch[1] = 1;
        }
        else if(pzone_mag->zone_info[i].sort == 3){
            srot_tal[2][0]++;
            if(pzone_mag->zone_info[i].is_stretch)
                sort_stretch[2] = 1;
        }
        if(pzone_mag->zone_info[i].hfactor > hfactor_max)
        {
            hfactor_max = pzone_mag->zone_info[i].hfactor;
        }
        if(pzone_mag->zone_info[i].is_large )
        {
            large_cnt ++;
        }
        if(pzone_mag->zone_info[i].format== HAL_PIXEL_FORMAT_YCrCb_NV12)
        {
            isyuv = true;
        }
        if(Context->zone_manager.zone_info[i].hfactor > 1.0)
            factor = 2;
        else
            factor = 1;
        tsize += (Context->zone_manager.zone_info[i].size *factor);
    }
    j = 0;
    for(i=0;i<3;i++)
    {
        if( srot_tal[i][0] >=2)  // > twice zones
        {
            srot_tal[i][1] = win_disphed[j+2];
            win_disphed_flag[j+2] = 1; // win2 ,win3 is dispatch flag
            ALOGV("more twice zones srot_tal[%d][1]=%d",i,srot_tal[i][1]);
            j++;
            if(j > 1)  // lcdc only has win2 and win3 supprot more zones
            {
                ALOGD("lcdc only has win2 and win3 supprot more zones");
                return -1;
            }
        }
    }
    j = 0;
    for(i=0;i<3;i++)
    {
        if( sort_stretch[i] == 1)  // strech
        {
            srot_tal[i][1] = win_disphed[j];  // win 0 and win 1 suporot stretch
            win_disphed_flag[j] = 1; // win0 ,win1 is dispatch flag
            ALOGV("stretch zones srot_tal[%d][1]=%d",i,srot_tal[i][1]);
            j++;
            if(j > 2)  // lcdc only has win0 and win1 supprot stretch
            {
                ALOGD("lcdc only has win0 and win1 supprot stretch");
                return -1;
            }
        }
    }
    if(hfactor_max >=1.4)
    {
        bw += (j + 1);

    }
    if(isyuv)
    {
        bw +=5;
    }
    ALOGV("large_cnt =%d,bw=%d",large_cnt , bw);

    for(i=0;i<3;i++)
    {
        if( srot_tal[i][1] == 0)  // had not dispatched
        {
            for(j=0;j<3;j++)
            {
                if(win_disphed_flag[j] == 0) // find the win had not dispatched
                    break;
            }
            if(j>=3)
            {
                ALOGE("3 wins had beed dispatched ");
                return -1;
            }
            srot_tal[i][1] = win_disphed[j];
            win_disphed_flag[j] = 1;
            ALOGV("srot_tal[%d][1].dispatched=%d",i,srot_tal[i][1]);
        }
    }

    for(i=0;i<pzone_mag->zone_cnt;i++)
    {
         switch(pzone_mag->zone_info[i].sort) {
            case 1:
                pzone_mag->zone_info[i].dispatched = srot_tal[0][1]++;
                break;
            case 2:
                pzone_mag->zone_info[i].dispatched = srot_tal[1][1]++;
                break;
            case 3:
                pzone_mag->zone_info[i].dispatched = srot_tal[2][1]++;
                break;
            default:
                ALOGE("try_wins_dispatch_mix_vh sort err!");
                return -1;
        }
        ALOGV("zone[%d].dispatched[%d]=%s,sort=%d", \
        i,pzone_mag->zone_info[i].dispatched,
        compositionTypeName[pzone_mag->zone_info[i].dispatched -1],
        pzone_mag->zone_info[i].sort);
    }

    for(i=0;i<pzone_mag->zone_cnt;i++){
        int disptched = pzone_mag->zone_info[i].dispatched;
        int sct_width = pzone_mag->zone_info[i].src_rect.right
                                            - pzone_mag->zone_info[i].src_rect.left;
        int sct_height = pzone_mag->zone_info[i].src_rect.bottom
                                            - pzone_mag->zone_info[i].src_rect.top;
        int dst_width = pzone_mag->zone_info[i].disp_rect.right
                                            - pzone_mag->zone_info[i].disp_rect.left;
        int dst_height = pzone_mag->zone_info[i].disp_rect.bottom
                                            - pzone_mag->zone_info[i].disp_rect.top;
        /*win2 win3 not support YUV*/
        if(disptched > win1 && is_yuv(pzone_mag->zone_info[i].format))
            return -1;
        /*scal not support whoes source bigger than 2560 to dst 4k*/
        if(disptched <= win1 &&(sct_width > 2160 || sct_height > 2160) &&
            !is_yuv(pzone_mag->zone_info[i].format) && contextAh->mHdmiSI.NeedReDst)
            return -1;
        if(disptched <= win1 && (sct_width > 2560 || dst_width > 2560) &&
                                             !is_yuv(pzone_mag->zone_info[i].format)
                        && (sct_height != dst_height || Context->mResolutionChanged))
            return -1;
    }

#if USE_QUEUE_DDRFREQ
    if(Context->ddrFd > 0)
    {
        for(i=0;i<pzone_mag->zone_cnt;i++)
        {
            int area_no = 0;
            int win_id = 0;
            ALOGD_IF(log(HLLFIV),"Zone[%d]->layer[%d],dispatched=%d,"
            "[%d,%d,%d,%d] =>[%d,%d,%d,%d],"
            "w_h_s_f[%d,%d,%d,%d],tr_rtr_bled[%d,%d,%d],"
            "layer_fd[%d],addr=%x,acq_fence_fd=%d",
            pzone_mag->zone_info[i].zone_index,
            pzone_mag->zone_info[i].layer_index,
            pzone_mag->zone_info[i].dispatched,
            pzone_mag->zone_info[i].src_rect.left,
            pzone_mag->zone_info[i].src_rect.top,
            pzone_mag->zone_info[i].src_rect.right,
            pzone_mag->zone_info[i].src_rect.bottom,
            pzone_mag->zone_info[i].disp_rect.left,
            pzone_mag->zone_info[i].disp_rect.top,
            pzone_mag->zone_info[i].disp_rect.right,
            pzone_mag->zone_info[i].disp_rect.bottom,
            pzone_mag->zone_info[i].width,
            pzone_mag->zone_info[i].height,
            pzone_mag->zone_info[i].stride,
            pzone_mag->zone_info[i].format,
            pzone_mag->zone_info[i].transform,
            pzone_mag->zone_info[i].realtransform,
            pzone_mag->zone_info[i].blend,
            pzone_mag->zone_info[i].layer_fd,
            pzone_mag->zone_info[i].addr,
            pzone_mag->zone_info[i].acq_fence_fd);
            switch(pzone_mag->zone_info[i].dispatched) {
                case win0:
                    bpvinfo.vopinfo[0].state = 1;
                    bpvinfo.vopinfo[0].zone_num ++;
                   break;
                case win1:
                    bpvinfo.vopinfo[1].state = 1;
                    bpvinfo.vopinfo[1].zone_num ++;
                    break;
                case win2_0:
                    bpvinfo.vopinfo[2].state = 1;
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_1:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_2:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_3:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                default:
                    ALOGE("hwc_dispatch_mix  err!");
                    return -1;
             }
        }
        bpvinfo.vopinfo[3].state = 1;
        bpvinfo.vopinfo[3].zone_num ++;
        bpvinfo.bp_size = Context->zone_manager.bp_size;
        tsize += Context->fbhandle.width * Context->fbhandle.height*4;
        if(tsize)
            tsize = tsize / (1024 *1024) * 60 ;// MB
        bpvinfo.bp_vop_size = tsize ;
        for(i= 0;i<4;i++)
        {
            ALOGD_IF(log(HLLFIV),"RK_QUEDDR_FREQ mixinfo win[%d] bo_size=%dMB,bp_vop_size=%dMB,state=%d,num=%d",
                i,bpvinfo.bp_size,bpvinfo.bp_vop_size,bpvinfo.vopinfo[i].state,bpvinfo.vopinfo[i].zone_num);
        }
        if(ioctl(Context->ddrFd, RK_QUEDDR_FREQ, &bpvinfo))
        {
            if(log(HLLTHR))
            {
                for(i= 0;i<4;i++)
                {
                    ALOGD("%s,%d:RK_QUEDDR_FREQ mixinfo win[%d] bo_size=%dMB,bp_vop_size=%dMB,state=%d,num=%d",
                        __func__,__LINE__,i,bpvinfo.bp_size,bpvinfo.bp_vop_size,
                            bpvinfo.vopinfo[i].state,bpvinfo.vopinfo[i].zone_num);
                }
            }
            return -1;
        }
    }
#endif
    //Mark the composer mode to HWC_MIX_V2
    if (list) {
        list->hwLayers[0].compositionType = HWC_MIX_V2;
    }

    for (unsigned int i = 0; i < list->numHwLayers - 1; i ++) {
        hwc_layer_1_t * layer = &list->hwLayers[i];
        struct private_handle_t* hnd = (struct private_handle_t *)layer->handle;
        if (hnd && layer->compositionType == HWC_FRAMEBUFFER &&
                                  hnd->format == HAL_PIXEL_FORMAT_YCrCb_NV12_10)
            return -1;
    }

    memcpy(&Context->zone_manager,&zone_m,sizeof(ZoneManager));
    Context->mHdmiSI.mix_vh = true;
    Context->zone_manager.mCmpType = HWC_MIX_VH;
    Context->zone_manager.composter_mode = HWC_MIX_V2;
    memcpy((void*)&gmixinfo[mix_index],(void*)&gMixInfo,sizeof(gMixInfo));
    return 0;
}

static int try_wins_dispatch_win0 (void * ctx,hwc_display_contents_1_t * list)
{
    int win_disphed_flag[2] = {0}; // win0, win2
    int win_disphed[2] = {win0,win2_0};
    int i,j;
    int cntfb = 0;
    hwcContext * Context = (hwcContext *)ctx;
    ZoneManager zone_m;
    initLayerCompositionType(Context,list);
    memcpy(&zone_m,&Context->zone_manager,sizeof(ZoneManager));
    ZoneManager* pzone_mag = &zone_m;
    int sort = 1;
    int cnt = 0;
    int srot_tal[2][2] = {0,};
    int sort_stretch[2] = {0};
    int sort_pre;
    int gpu_draw = 0;
    float hfactor_max = 1.0;
    int large_cnt = 0;
    bool isyuv = false;
    int bw = 0;
    BpVopInfo  bpvinfo;
    int tsize = 0;
    int mix_index = 0;
    int iFirstTransformLayer=-1;
    bool bTransform=false;
    mix_info gMixInfo;

    memset(&bpvinfo,0,sizeof(BpVopInfo));
    char const* compositionTypeName[] = {
            "win0",
            "win1",
            "win2_0",
            "win2_1",
            "win2_2",
            "win2_3",
            "win3_0",
            "win3_1",
            "win3_2",
            "win3_3",
            };
    hwcContext * contextAh = _contextAnchor;
    if (Context == _contextAnchor1) {
        mix_index = 1;
    } else if (Context == _contextAnchor) {
        mix_index = 0;
    }

    if (!Context->mComVop && Context->mIsMipiDualOutMode) {
        ALOGD_IF(log(HLLTHR),"lcdc dont support 5 wins [%d]",__LINE__);
        return -1;
    }

    if (pzone_mag->zone_cnt != 1) {
        ALOGD_IF(log(HLLTHR),"lcdc dont support 5 wins [%d]",__LINE__);
    	return -1;
    }

    if (Context->mAlphaError) {
        ALOGD_IF(log(HLLTHR),"lcdc dont support 5 wins [%d]",__LINE__);
        return -1;
    }

    for (int k = 0; k < 1; k++) {
        if(pzone_mag->zone_info[k].scale_err || pzone_mag->zone_info[k].toosmall
            || pzone_mag->zone_info[k].zone_err || (pzone_mag->zone_info[k].transform
                && (pzone_mag->zone_info[k].format != HAL_PIXEL_FORMAT_YCrCb_NV12))) {
            ALOGD_IF(log(HLLTHR),"lcdc dont support 5 wins [%d]",__LINE__);
            return -1;
        }
    }

    for(i=0;i< pzone_mag->zone_cnt;i++) {
        ALOGD_IF(log(HLLFIV),"%s,%d:Zone[%d]->layer[%d],"
            "[%d,%d,%d,%d] =>[%d,%d,%d,%d],"
            "w_h_s_f[%d,%d,%d,%d],tr_rtr_bled[%d,%d,%d],acq_fence_fd=%d",
            __FUNCTION__,__LINE__,
            Context->zone_manager.zone_info[i].zone_index,
            Context->zone_manager.zone_info[i].layer_index,
            Context->zone_manager.zone_info[i].src_rect.left,
            Context->zone_manager.zone_info[i].src_rect.top,
            Context->zone_manager.zone_info[i].src_rect.right,
            Context->zone_manager.zone_info[i].src_rect.bottom,
            Context->zone_manager.zone_info[i].disp_rect.left,
            Context->zone_manager.zone_info[i].disp_rect.top,
            Context->zone_manager.zone_info[i].disp_rect.right,
            Context->zone_manager.zone_info[i].disp_rect.bottom,
            Context->zone_manager.zone_info[i].width,
            Context->zone_manager.zone_info[i].height,
            Context->zone_manager.zone_info[i].stride,
            Context->zone_manager.zone_info[i].format,
            Context->zone_manager.zone_info[i].transform,
            Context->zone_manager.zone_info[i].realtransform,
            Context->zone_manager.zone_info[i].blend,
            Context->zone_manager.zone_info[i].acq_fence_fd);
    }
    pzone_mag->zone_info[0].sort = 1;
    sort = 1;

    if (sort > 3) {
        ALOGD_IF(log(HLLTHR),"lcdc dont support 5 wins [%d]",__LINE__);
        return -1;
    }

    for(i = 0; i < pzone_mag->zone_cnt; i++) {
        int factor =1;
        ALOGV("sort[%d].type=%d",i,pzone_mag->zone_info[i].sort);
        if( pzone_mag->zone_info[i].sort == 1){
            srot_tal[0][0]++;
            if(pzone_mag->zone_info[i].is_stretch)
                sort_stretch[0] = 1;
        }
        else if(pzone_mag->zone_info[i].sort == 2){
            srot_tal[1][0]++;
            if(pzone_mag->zone_info[i].is_stretch)
                sort_stretch[1] = 1;
        }

        if(pzone_mag->zone_info[i].hfactor > hfactor_max)
            hfactor_max = pzone_mag->zone_info[i].hfactor;

        if(pzone_mag->zone_info[i].is_large)
            large_cnt ++;

        if(pzone_mag->zone_info[i].format== HAL_PIXEL_FORMAT_YCrCb_NV12)
            isyuv = true;

        if(Context->zone_manager.zone_info[i].hfactor > 1.0)
            factor = 2;
        else
            factor = 1;
        tsize += (Context->zone_manager.zone_info[i].size *factor);
    }

    for (i = 0,j = 0; i < 1; i++) {
        {
            srot_tal[i][1] = win_disphed[0];  // win 0 and win 1 suporot stretch
            win_disphed_flag[i] = 1; // win0 ,win1 is dispatch flag
            ALOGV("stretch zones srot_tal[%d][1]=%d",i,srot_tal[i][1]);
            j++;
            if(j > 1) {
                ALOGD_IF(log(HLLTWO),"lcdc only has win0 and win1 supprot stretch");
                return -1;
            }
        }
    }

    // third dispatch common zones win
    for (i = 0; i < 1; i++) {
        if (srot_tal[i][1] == 0)  // had not dispatched
        {
            ALOGD_IF(log(HLLTHR),"has layer can not beed dispatched [%d]",__LINE__);
            return -1;
        }
    }

    if (hfactor_max >= 1.4)
        bw += (j + 1);

    if (isyuv)
        bw +=5;

    for(i=0;i<pzone_mag->zone_cnt;i++) {
         switch(pzone_mag->zone_info[i].sort) {
            case 1:
                pzone_mag->zone_info[i].dispatched = srot_tal[0][1]++;
                break;
            case 2:
                pzone_mag->zone_info[i].dispatched = srot_tal[1][1]++;
                break;
            default:
                ALOGE("try_wins_dispatch_mix_vh sort err!");
                return -1;
        }
        ALOGV("zone[%d].dispatched[%d]=%s,sort=%d", \
        i,pzone_mag->zone_info[i].dispatched,
        compositionTypeName[pzone_mag->zone_info[i].dispatched -1],
        pzone_mag->zone_info[i].sort);
    }

    //Mark the composer mode to HWC_MIX_V2
    memcpy(&Context->zone_manager,&zone_m,sizeof(ZoneManager));

    Context->zone_manager.mCmpType = HWC_HOR;
    Context->zone_manager.composter_mode = HWC_LCDC;
    return 0;
}

static int try_wins_dispatch_win02 (void * ctx,hwc_display_contents_1_t * list)
{
    int win_disphed_flag[2] = {0,}; // win0, win2
    int win_disphed[2] = {win0,win2_0};
    int i,j;
    int cntfb = 0;
    hwcContext * Context = (hwcContext *)ctx;
    ZoneManager zone_m;
    initLayerCompositionType(Context,list);
    memcpy(&zone_m,&Context->zone_manager,sizeof(ZoneManager));
    ZoneManager* pzone_mag = &zone_m;
    ZoneInfo    zone_info_ty[MaxZones];
    int sort = 1;
    int cnt = 0;
    int srot_tal[2][2] = {0,};
    int sort_stretch[2] = {0};
    int sort_pre;
    int gpu_draw = 0;
    float hfactor_max = 1.0;
    int large_cnt = 0;
    bool isyuv = false;
    int bw = 0;
    BpVopInfo  bpvinfo;
    int tsize = 0;
    int mix_index = 0;
    int iFirstTransformLayer=-1;
    bool bTransform=false;
    mix_info gMixInfo;

    memset(&bpvinfo,0,sizeof(BpVopInfo));
    char const* compositionTypeName[] = {
            "win0",
            "win1",
            "win2_0",
            "win2_1",
            "win2_2",
            "win2_3",
            "win3_0",
            "win3_1",
            "win3_2",
            "win3_3",
            };
    hwcContext * contextAh = _contextAnchor;
    memset(&zone_info_ty,0,sizeof(zone_info_ty));
    if(Context == _contextAnchor1){
        mix_index = 1;
    }else if(Context == _contextAnchor){
        mix_index = 0;
    }

    if (Context->mAlphaError) {
        return -1;
    }

    for (int k = 0; k < pzone_mag->zone_cnt; k++) {
        if(pzone_mag->zone_info[k].scale_err || pzone_mag->zone_info[k].toosmall
            || pzone_mag->zone_info[k].zone_err || (pzone_mag->zone_info[k].transform
                && (pzone_mag->zone_info[k].format != HAL_PIXEL_FORMAT_YCrCb_NV12)))
            return -1;
    }

    for (i = 0; i< pzone_mag->zone_cnt; i++) {
        ALOGD_IF(log(HLLFIV),"%s,%d:Zone[%d]->layer[%d],"
            "[%d,%d,%d,%d] =>[%d,%d,%d,%d],"
            "w_h_s_f[%d,%d,%d,%d],tr_rtr_bled[%d,%d,%d],acq_fence_fd=%d",
            __FUNCTION__,__LINE__,
            Context->zone_manager.zone_info[i].zone_index,
            Context->zone_manager.zone_info[i].layer_index,
            Context->zone_manager.zone_info[i].src_rect.left,
            Context->zone_manager.zone_info[i].src_rect.top,
            Context->zone_manager.zone_info[i].src_rect.right,
            Context->zone_manager.zone_info[i].src_rect.bottom,
            Context->zone_manager.zone_info[i].disp_rect.left,
            Context->zone_manager.zone_info[i].disp_rect.top,
            Context->zone_manager.zone_info[i].disp_rect.right,
            Context->zone_manager.zone_info[i].disp_rect.bottom,
            Context->zone_manager.zone_info[i].width,
            Context->zone_manager.zone_info[i].height,
            Context->zone_manager.zone_info[i].stride,
            Context->zone_manager.zone_info[i].format,
            Context->zone_manager.zone_info[i].transform,
            Context->zone_manager.zone_info[i].realtransform,
            Context->zone_manager.zone_info[i].blend,
            Context->zone_manager.zone_info[i].acq_fence_fd);
        pzone_mag->zone_info[i].sort = i + 1;
    }

    sort = pzone_mag->zone_cnt;

    if (sort > 3) {
        ALOGD_IF(log(HLLTHR),"lcdc dont support 5 wins [%d]",__LINE__);
        return -1;
    }

    for (i = 0; i < pzone_mag->zone_cnt; i++) {
        int factor =1;
        ALOGV("sort[%d].type=%d",i,pzone_mag->zone_info[i].sort);
        if (pzone_mag->zone_info[i].sort == 1) {
            srot_tal[0][0]++;
            if(pzone_mag->zone_info[i].is_stretch)
                sort_stretch[0] = 1;
        } else if (pzone_mag->zone_info[i].sort == 2) {
            srot_tal[1][0]++;
            if(pzone_mag->zone_info[i].is_stretch)
                sort_stretch[1] = 1;
        }

        if(pzone_mag->zone_info[i].hfactor > hfactor_max)
            hfactor_max = pzone_mag->zone_info[i].hfactor;

        if(pzone_mag->zone_info[i].is_large)
            large_cnt ++;

        if(pzone_mag->zone_info[i].format== HAL_PIXEL_FORMAT_YCrCb_NV12)
            isyuv = true;

        if(Context->zone_manager.zone_info[i].hfactor > 1.0)
            factor = 2;
        else
            factor = 1;
        tsize += (Context->zone_manager.zone_info[i].size *factor);
    }

    for(i = 0,j = 0; i < 2; i++) {
        if (sort_stretch[i] == 0) {
            if (j > 0)
                break;
            srot_tal[i][1] = win_disphed[1];
            win_disphed_flag[1] = 1; // win2 ,win3 is dispatch flag
            j++;
        }
    }

    for (i = 0, j = 0; i < 2; i++) {
        if (sort_stretch[i] == 1) {
            if (j > 0)
                break;
            srot_tal[i][1] = win_disphed[0];  // win 0 and win 1 suporot stretch
            win_disphed_flag[0] = 1; // win0 ,win1 is dispatch flag
            j++;
        }
    }

    for (i = 0; i < pzone_mag->zone_cnt; i++) {
        if (srot_tal[i][1] == 0) {  // had not dispatched
            ALOGD_IF(log(HLLTHR),"has not win to dispatch[%d]",__LINE__);
            return -1;
        }
    }

    if(hfactor_max >=1.4)
    {
        bw += (j + 1);

    }
    if(isyuv)
    {
        bw +=5;
    }

    for (i = 0; i < pzone_mag->zone_cnt; i++) {
         switch(pzone_mag->zone_info[i].sort) {
            case 1:
                pzone_mag->zone_info[i].dispatched = srot_tal[0][1]++;
                break;
            case 2:
                pzone_mag->zone_info[i].dispatched = srot_tal[1][1]++;
                break;
            default:
                ALOGE("%s:%d sort err!", __func__, __LINE__);
                return -1;
        }
        ALOGV("zone[%d].dispatched[%d]=%s,sort=%d", \
        i,pzone_mag->zone_info[i].dispatched,
        compositionTypeName[pzone_mag->zone_info[i].dispatched -1],
        pzone_mag->zone_info[i].sort);
    }

#if USE_QUEUE_DDRFREQ
    if(Context->ddrFd > 0)
    {
        for(i=0;i<pzone_mag->zone_cnt;i++)
        {
            int area_no = 0;
            int win_id = 0;
            ALOGD_IF(log(HLLFIV),"Zone[%d]->layer[%d],dispatched=%d,"
            "[%d,%d,%d,%d] =>[%d,%d,%d,%d],"
            "w_h_s_f[%d,%d,%d,%d],tr_rtr_bled[%d,%d,%d],"
            "layer_fd[%d],addr=%x,acq_fence_fd=%d",
            pzone_mag->zone_info[i].zone_index,
            pzone_mag->zone_info[i].layer_index,
            pzone_mag->zone_info[i].dispatched,
            pzone_mag->zone_info[i].src_rect.left,
            pzone_mag->zone_info[i].src_rect.top,
            pzone_mag->zone_info[i].src_rect.right,
            pzone_mag->zone_info[i].src_rect.bottom,
            pzone_mag->zone_info[i].disp_rect.left,
            pzone_mag->zone_info[i].disp_rect.top,
            pzone_mag->zone_info[i].disp_rect.right,
            pzone_mag->zone_info[i].disp_rect.bottom,
            pzone_mag->zone_info[i].width,
            pzone_mag->zone_info[i].height,
            pzone_mag->zone_info[i].stride,
            pzone_mag->zone_info[i].format,
            pzone_mag->zone_info[i].transform,
            pzone_mag->zone_info[i].realtransform,
            pzone_mag->zone_info[i].blend,
            pzone_mag->zone_info[i].layer_fd,
            pzone_mag->zone_info[i].addr,
            pzone_mag->zone_info[i].acq_fence_fd);
            switch(pzone_mag->zone_info[i].dispatched) {
                case win0:
                    bpvinfo.vopinfo[0].state = 1;
                    bpvinfo.vopinfo[0].zone_num ++;
                   break;
                case win1:
                    bpvinfo.vopinfo[1].state = 1;
                    bpvinfo.vopinfo[1].zone_num ++;
                    break;
                case win2_0:
                    bpvinfo.vopinfo[2].state = 1;
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_1:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_2:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_3:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                default:
                    ALOGE("hwc_dispatch_mix  err!");
                    return -1;
             }
        }
        bpvinfo.vopinfo[3].state = 1;
        bpvinfo.vopinfo[3].zone_num ++;
        bpvinfo.bp_size = Context->zone_manager.bp_size;
        tsize += Context->fbhandle.width * Context->fbhandle.height*4;
        if(tsize)
            tsize = tsize / (1024 *1024) * 60 ;// MB
        bpvinfo.bp_vop_size = tsize ;
        for(i= 0;i<4;i++)
        {
            ALOGD_IF(log(HLLFIV),"RK_QUEDDR_FREQ mixinfo win[%d] bo_size=%dMB,bp_vop_size=%dMB,state=%d,num=%d",
                i,bpvinfo.bp_size,bpvinfo.bp_vop_size,bpvinfo.vopinfo[i].state,bpvinfo.vopinfo[i].zone_num);
        }
        if(ioctl(Context->ddrFd, RK_QUEDDR_FREQ, &bpvinfo))
        {
            if(log(HLLTHR))
            {
                for(i= 0;i<4;i++)
                {
                    ALOGD("RK_QUEDDR_FREQ mixinfo win[%d] bo_size=%dMB,bp_vop_size=%dMB,state=%d,num=%d",
                        i,bpvinfo.bp_size,bpvinfo.bp_vop_size,bpvinfo.vopinfo[i].state,bpvinfo.vopinfo[i].zone_num);
                }
            }
            return -1;
        }
    }
#endif
    //Mark the composer mode to HWC_MIX_V2
    memcpy(&Context->zone_manager,&zone_m,sizeof(ZoneManager));

    Context->zone_manager.mCmpType = HWC_HOR;
    Context->zone_manager.composter_mode = HWC_LCDC;
    return 0;
}

static int try_wins_dispatch_mix_win02 (void * ctx,hwc_display_contents_1_t * list)
{
    int win_disphed_flag[3] = {0,}; // win0, win1, win2, win3 flag which is dispatched
    int win_disphed[3] = {win0,win1,win2_0};
    int i,j;
    int cntfb = 0;
    hwcContext * Context = (hwcContext *)ctx;
    ZoneManager zone_m;
    initLayerCompositionType(Context,list);
    memcpy(&zone_m,&Context->zone_manager,sizeof(ZoneManager));
    ZoneManager* pzone_mag = &zone_m;
    ZoneInfo    zone_info_ty[MaxZones];
    int sort = 1;
    int cnt = 0;
    int srot_tal[3][2] = {0,};
    int sort_stretch[3] = {0};
    int sort_pre;
    int gpu_draw = 0;
    float hfactor_max = 1.0;
    int large_cnt = 0;
    bool isyuv = false;
    int bw = 0;
    BpVopInfo  bpvinfo;
    int tsize = 0;
    int mix_index = 0;
    int iFirstTransformLayer=-1;
    bool bTransform=false;
    mix_info gMixInfo;

    memset(&bpvinfo,0,sizeof(BpVopInfo));
    char const* compositionTypeName[] = {
            "win0",
            "win1",
            "win2_0",
            "win2_1",
            "win2_2",
            "win2_3",
            "win3_0",
            "win3_1",
            "win3_2",
            "win3_3",
            };
    hwcContext * contextAh = _contextAnchor;
    memset(&zone_info_ty,0,sizeof(zone_info_ty));
    if(Context == _contextAnchor1){
        mix_index = 1;
    }else if(Context == _contextAnchor){
        mix_index = 0;
    }

    if (list->numHwLayers - 1 < 2) {
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }

    if (Context->mAlphaError) {
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }

    if (contextAh->mHdmiSI.NeedReDst) {
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }

#ifdef RK3288_BOX
    if (Context==_contextAnchor && Context->mResolutionChanged && Context->mLcdcNum==2) {
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }
#endif
#if DUAL_VIEW_MODE
    if (Context != contextAh && Context->mIsDualViewMode) {
        int dpyPw = contextAh->dpyAttr[0].xres;
        int dpyPh = contextAh->dpyAttr[0].yres;
        int dpyEw = Context->dpyAttr[1].xres;
        int dpyEh = Context->dpyAttr[1].yres;
        if(dpyPw != dpyEw || dpyPh != dpyEh) {
            ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
            return -1;
        }
    }
#endif

    for (int k=0; k < 1; k++) {
        if(pzone_mag->zone_info[k].scale_err || pzone_mag->zone_info[k].toosmall
            || pzone_mag->zone_info[k].zone_err || (pzone_mag->zone_info[k].transform
                && pzone_mag->zone_info[k].format != HAL_PIXEL_FORMAT_YCrCb_NV12 && 0==k)
                    || (pzone_mag->zone_info[k].transform && 1 == k)) {
            ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
            return -1;
        }
    }

    memcpy((void*)&gMixInfo,(void*)&gmixinfo[mix_index],sizeof(gMixInfo));
    for (i = 0, j = 0; i < pzone_mag->zone_cnt; i++) {
        //Set the layer which it's layer_index bigger than the first transform layer index to HWC_FRAMEBUFFER or HWC_NODRAW
        if (pzone_mag->zone_info[i].layer_index > 0) {
            hwc_layer_1_t * layer = &list->hwLayers[pzone_mag->zone_info[i].layer_index];
            //Judge the current layer whether backup in gmixinfo[mix_index] or not.
            if (Context->mLastCompType != HWC_MIX_UP
                || gMixInfo.lastZoneCrc[pzone_mag->zone_info[i].layer_index] != pzone_mag->zone_info[i].zoneCrc
                || gMixInfo.gpu_draw_fd[pzone_mag->zone_info[i].layer_index] != pzone_mag->zone_info[i].layer_fd
                || gMixInfo.alpha[pzone_mag->zone_info[i].layer_index] != pzone_mag->zone_info[i].zone_alpha) {
                gpu_draw = 1;
                layer->compositionType = HWC_FRAMEBUFFER;
                gMixInfo.lastZoneCrc[pzone_mag->zone_info[i].layer_index] = pzone_mag->zone_info[i].zoneCrc;
                gMixInfo.gpu_draw_fd[pzone_mag->zone_info[i].layer_index] = pzone_mag->zone_info[i].layer_fd;
                gMixInfo.alpha[pzone_mag->zone_info[i].layer_index] = pzone_mag->zone_info[i].zone_alpha;
            } else {
                layer->compositionType = HWC_NODRAW;
            }
            if(gpu_draw && i == pzone_mag->zone_cnt-1) {
                for (int j = 1; j < pzone_mag->zone_cnt; j++) {
                    layer = &list->hwLayers[j];
                    layer->compositionType = HWC_FRAMEBUFFER;
                }
                ALOGV(" need draw by gpu");
            }
            cntfb ++;
        } else {
            memcpy(&zone_info_ty[j], &pzone_mag->zone_info[i],sizeof(ZoneInfo));
            zone_info_ty[j].sort = 0;
            j++;
        }
    }
    memcpy(pzone_mag, &zone_info_ty,sizeof(zone_info_ty));
    pzone_mag->zone_cnt -= cntfb;
    for (i = 0; i < pzone_mag->zone_cnt; i++) {
        ALOGD_IF(log(HLLFIV),"%s,%d:Zone[%d]->layer[%d],"
            "[%d,%d,%d,%d] =>[%d,%d,%d,%d],"
            "w_h_s_f[%d,%d,%d,%d],tr_rtr_bled[%d,%d,%d],acq_fence_fd=%d",
            __FUNCTION__,__LINE__,
            Context->zone_manager.zone_info[i].zone_index,
            Context->zone_manager.zone_info[i].layer_index,
            Context->zone_manager.zone_info[i].src_rect.left,
            Context->zone_manager.zone_info[i].src_rect.top,
            Context->zone_manager.zone_info[i].src_rect.right,
            Context->zone_manager.zone_info[i].src_rect.bottom,
            Context->zone_manager.zone_info[i].disp_rect.left,
            Context->zone_manager.zone_info[i].disp_rect.top,
            Context->zone_manager.zone_info[i].disp_rect.right,
            Context->zone_manager.zone_info[i].disp_rect.bottom,
            Context->zone_manager.zone_info[i].width,
            Context->zone_manager.zone_info[i].height,
            Context->zone_manager.zone_info[i].stride,
            Context->zone_manager.zone_info[i].format,
            Context->zone_manager.zone_info[i].transform,
            Context->zone_manager.zone_info[i].realtransform,
            Context->zone_manager.zone_info[i].blend,
            Context->zone_manager.zone_info[i].acq_fence_fd);
    }
    pzone_mag->zone_info[0].sort = sort;
    for (i = 0; i < (pzone_mag->zone_cnt-1);) {
        pzone_mag->zone_info[i].sort = sort;
        sort_pre  = sort;
        cnt = 0;
        for (j = 1; j < 4 && (i+j) < pzone_mag->zone_cnt; j++) {
            ZoneInfo * next_zf = &(pzone_mag->zone_info[i+j]);
            bool is_combine = false;
            int k;
            for (k = 0; k <= cnt; k++) { // compare all sorted_zone info
                ZoneInfo * sorted_zf = &(pzone_mag->zone_info[i+j-1-k]);
                if (is_zone_combine(sorted_zf,next_zf)) {
                    is_combine = true;
                } else {
                    is_combine = false;
                    break;
                }
            }
            if(is_combine) {
                pzone_mag->zone_info[i+j].sort = sort;
                cnt++;
            } else {
                sort++;
                pzone_mag->zone_info[i+j].sort = sort;
                cnt++;
                break;
            }
        }
        if (sort_pre == sort && (i+cnt) < (pzone_mag->zone_cnt-1))  // win2 ,4zones ,win3 4zones,so sort ++,but exit not ++
            sort ++;
        i += cnt;
    }
    if (sort > 3) {// lcdc dont support 5 wins
        ALOGD_IF(log(HLLTHR),"lcdc dont support 5 wins sort=%d",sort);
        return -1;
    }
    for (i = 0; i < pzone_mag->zone_cnt; i++) {
        int factor =1;
        ALOGV("sort[%d].type=%d",i,pzone_mag->zone_info[i].sort);
        if (pzone_mag->zone_info[i].sort == 1) {
            srot_tal[0][0]++;
            if(pzone_mag->zone_info[i].is_stretch)
                sort_stretch[0] = 1;
        } else if (pzone_mag->zone_info[i].sort == 2) {
            srot_tal[1][0]++;
            if(pzone_mag->zone_info[i].is_stretch)
                sort_stretch[1] = 1;
        } else if (pzone_mag->zone_info[i].sort == 3) {
            srot_tal[2][0]++;
            if(pzone_mag->zone_info[i].is_stretch)
                sort_stretch[2] = 1;
        }
        if(pzone_mag->zone_info[i].hfactor > hfactor_max)
        {
            hfactor_max = pzone_mag->zone_info[i].hfactor;
        }
        if (pzone_mag->zone_info[i].is_large ) {
            large_cnt ++;
        }
        if(pzone_mag->zone_info[i].format== HAL_PIXEL_FORMAT_YCrCb_NV12) {
            isyuv = true;
        }
        if(Context->zone_manager.zone_info[i].hfactor > 1.0)
            factor = 2;
        else
            factor = 1;
        tsize += (Context->zone_manager.zone_info[i].size *factor);
    }
    j = 0;
    for (i = 0; i < 3; i++) {
        if (srot_tal[i][0] >= 2) {
            srot_tal[i][1] = win_disphed[j+2];
            win_disphed_flag[j+2] = 1; // win2 ,win3 is dispatch flag
            ALOGV("more twice zones srot_tal[%d][1]=%d",i,srot_tal[i][1]);
            j++;
            if(j > 1) {
                ALOGD("lcdc only has win2 and win3 supprot more zones");
                return -1;
            }
        }
    }
    j = 0;
    for (i = 0; i < 3; i++) {
        if (sort_stretch[i] == 1) {
            srot_tal[i][1] = win_disphed[j];  // win 0 and win 1 suporot stretch
            win_disphed_flag[j] = 1; // win0 ,win1 is dispatch flag
            ALOGV("stretch zones srot_tal[%d][1]=%d",i,srot_tal[i][1]);
            j++;
            if(j > 2) {
                ALOGD_IF(log(HLLFIV),"lcdc only has win0 and win1 supprot stretch");
                return -1;
            }
        }
    }
    if(hfactor_max >=1.4)
    {
        bw += (j + 1);

    }
    if(isyuv)
    {
        bw +=5;
    }
    ALOGV("large_cnt =%d,bw=%d",large_cnt , bw);

    for(i=0;i<3;i++)
    {
        if( srot_tal[i][1] == 0)  // had not dispatched
        {
            for(j=0;j<3;j++)
            {
                if(win_disphed_flag[j] == 0) // find the win had not dispatched
                    break;
            }
            if(j>=3)
            {
                ALOGE("3 wins had beed dispatched ");
                return -1;
            }
            srot_tal[i][1] = win_disphed[j];
            win_disphed_flag[j] = 1;
            ALOGV("srot_tal[%d][1].dispatched=%d",i,srot_tal[i][1]);
        }
    }

    for(i=0;i<pzone_mag->zone_cnt;i++)
    {
         switch(pzone_mag->zone_info[i].sort) {
            case 1:
                pzone_mag->zone_info[i].dispatched = srot_tal[0][1]++;
                break;
            case 2:
                pzone_mag->zone_info[i].dispatched = srot_tal[1][1]++;
                break;
            case 3:
                pzone_mag->zone_info[i].dispatched = srot_tal[2][1]++;
                break;
            default:
                ALOGE("try_wins_dispatch_mix_vh sort err!");
                return -1;
        }
        ALOGV("zone[%d].dispatched[%d]=%s,sort=%d", \
        i,pzone_mag->zone_info[i].dispatched,
        compositionTypeName[pzone_mag->zone_info[i].dispatched -1],
        pzone_mag->zone_info[i].sort);
    }

    for(i=0;i<pzone_mag->zone_cnt;i++){
        int disptched = pzone_mag->zone_info[i].dispatched;
        int sct_width = pzone_mag->zone_info[i].src_rect.right
                                            - pzone_mag->zone_info[i].src_rect.left;
        int sct_height = pzone_mag->zone_info[i].src_rect.bottom
                                            - pzone_mag->zone_info[i].src_rect.top;
        int dst_width = pzone_mag->zone_info[i].disp_rect.right
                                            - pzone_mag->zone_info[i].disp_rect.left;
        int dst_height = pzone_mag->zone_info[i].disp_rect.bottom
                                            - pzone_mag->zone_info[i].disp_rect.top;
        /*win2 win3 not support YUV*/
        if(disptched > win1 && is_yuv(pzone_mag->zone_info[i].format))
            return -1;
        /*scal not support whoes source bigger than 2560 to dst 4k*/
        if(disptched <= win1 &&(sct_width > 2160 || sct_height > 2160) &&
            !is_yuv(pzone_mag->zone_info[i].format) && contextAh->mHdmiSI.NeedReDst)
            return -1;
        if(disptched <= win1 && (sct_width > 2560 || dst_width > 2560) &&
                                             !is_yuv(pzone_mag->zone_info[i].format)
                        && (sct_height != dst_height || Context->mResolutionChanged))
            return -1;
    }

#if USE_QUEUE_DDRFREQ
    if(Context->ddrFd > 0)
    {
        for(i=0;i<pzone_mag->zone_cnt;i++)
        {
            int area_no = 0;
            int win_id = 0;
            ALOGD_IF(log(HLLFIV),"Zone[%d]->layer[%d],dispatched=%d,"
            "[%d,%d,%d,%d] =>[%d,%d,%d,%d],"
            "w_h_s_f[%d,%d,%d,%d],tr_rtr_bled[%d,%d,%d],"
            "layer_fd[%d],addr=%x,acq_fence_fd=%d",
            pzone_mag->zone_info[i].zone_index,
            pzone_mag->zone_info[i].layer_index,
            pzone_mag->zone_info[i].dispatched,
            pzone_mag->zone_info[i].src_rect.left,
            pzone_mag->zone_info[i].src_rect.top,
            pzone_mag->zone_info[i].src_rect.right,
            pzone_mag->zone_info[i].src_rect.bottom,
            pzone_mag->zone_info[i].disp_rect.left,
            pzone_mag->zone_info[i].disp_rect.top,
            pzone_mag->zone_info[i].disp_rect.right,
            pzone_mag->zone_info[i].disp_rect.bottom,
            pzone_mag->zone_info[i].width,
            pzone_mag->zone_info[i].height,
            pzone_mag->zone_info[i].stride,
            pzone_mag->zone_info[i].format,
            pzone_mag->zone_info[i].transform,
            pzone_mag->zone_info[i].realtransform,
            pzone_mag->zone_info[i].blend,
            pzone_mag->zone_info[i].layer_fd,
            pzone_mag->zone_info[i].addr,
            pzone_mag->zone_info[i].acq_fence_fd);
            switch(pzone_mag->zone_info[i].dispatched) {
                case win0:
                    bpvinfo.vopinfo[0].state = 1;
                    bpvinfo.vopinfo[0].zone_num ++;
                   break;
                case win1:
                    bpvinfo.vopinfo[1].state = 1;
                    bpvinfo.vopinfo[1].zone_num ++;
                    break;
                case win2_0:
                    bpvinfo.vopinfo[2].state = 1;
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_1:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_2:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_3:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                default:
                    ALOGE("hwc_dispatch_mix  err!");
                    return -1;
             }
        }
        bpvinfo.vopinfo[3].state = 1;
        bpvinfo.vopinfo[3].zone_num ++;
        bpvinfo.bp_size = Context->zone_manager.bp_size;
        tsize += Context->fbhandle.width * Context->fbhandle.height*4;
        if(tsize)
            tsize = tsize / (1024 *1024) * 60 ;// MB
        bpvinfo.bp_vop_size = tsize ;
        for(i= 0;i<4;i++)
        {
            ALOGD_IF(log(HLLTHR),"RK_QUEDDR_FREQ mixinfo win[%d] bo_size=%dMB,bp_vop_size=%dMB,state=%d,num=%d",
                i,bpvinfo.bp_size,bpvinfo.bp_vop_size,bpvinfo.vopinfo[i].state,bpvinfo.vopinfo[i].zone_num);
        }
        if(ioctl(Context->ddrFd, RK_QUEDDR_FREQ, &bpvinfo))
        {
            if(log(HLLTHR))
            {
                for(i= 0;i<4;i++)
                {
                    ALOGD("RK_QUEDDR_FREQ mixinfo win[%d] bo_size=%dMB,bp_vop_size=%dMB,state=%d,num=%d",
                        i,bpvinfo.bp_size,bpvinfo.bp_vop_size,bpvinfo.vopinfo[i].state,bpvinfo.vopinfo[i].zone_num);
                }
            }
            return -1;
        }
    }
#endif
    //Mark the composer mode to HWC_MIX
    if(list){
        list->hwLayers[0].compositionType = HWC_MIX_V2;
    }

    for (unsigned int i = 0; i < list->numHwLayers - 1; i ++) {
        hwc_layer_1_t * layer = &list->hwLayers[i];
        struct private_handle_t* hnd = (struct private_handle_t *)layer->handle;
        if (hnd && layer->compositionType == HWC_FRAMEBUFFER &&
                                  hnd->format == HAL_PIXEL_FORMAT_YCrCb_NV12_10)
            return -1;
    }

    memcpy(&Context->zone_manager,&zone_m,sizeof(ZoneManager));
    Context->mHdmiSI.mix_up = true;
    Context->zone_manager.mCmpType = HWC_MIX_UP;
    Context->zone_manager.composter_mode = HWC_MIX;
    memcpy((void*)&gmixinfo[mix_index],(void*)&gMixInfo,sizeof(gMixInfo));
    return 0;
}

static int try_rga_vop_gpu_policy (void * ctx, hwc_display_contents_1_t * list)
{
    int win_disphed_flag[3] = {0,}; // win0, win1, win2, win3 flag which is dispatched
    int win_disphed[3] = {win0,win1,win2_0};
    int i,j;
    int cntfb = 0;
    hwcContext * Context = (hwcContext *)ctx;
    ZoneManager zone_m;
    initLayerCompositionType(Context,list);
    memcpy(&zone_m,&Context->zone_manager,sizeof(ZoneManager));
    ZoneManager* pzone_mag = &zone_m;
    ZoneInfo    zone_info_ty[MaxZones];
    int sort = 1;
    int cnt = 0;
    int srot_tal[3][2] = {0,};
    int sort_stretch[3] = {0};
    int sort_pre;
    int gpu_draw = 0;
    float hfactor_max = 1.0;
    int large_cnt = 0;
    bool isyuv = false;
    int bw = 0;
    BpVopInfo  bpvinfo;
    int tsize = 0;
    int mix_index = 0;
    int iFirstTransformLayer=-1;
    int yuvTenBitIndex = 0;
    bool bTransform=false;
    mix_info gMixInfo;

    memset(&bpvinfo,0,sizeof(BpVopInfo));
    char const* compositionTypeName[] = {
            "win0",
            "win1",
            "win2_0",
            "win2_1",
            "win2_2",
            "win2_3",
            "win3_0",
            "win3_1",
            "win3_2",
            "win3_3",
            };
    hwcContext * contextAh = _contextAnchor;
    memset(&zone_info_ty,0,sizeof(zone_info_ty));
    if(Context == _contextAnchor1){
        mix_index = 1;
    }else if(Context == _contextAnchor){
        mix_index = 0;
    }

    if (!Context->mHasYuvTenBit) {
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }

    yuvTenBitIndex = hwc_rga_fix_zones_for_yuv_ten_bit(pzone_mag);

    if (list && yuvTenBitIndex >= (int)list->numHwLayers - 1) {
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }

    if (yuvTenBitIndex > MaxBlitNum - 1) {
        ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
        return -1;
    }

    ALOGD("yuvTenBitIndex=%d", yuvTenBitIndex);

#if DUAL_VIEW_MODE
    if(Context != contextAh && Context->mIsDualViewMode) {
        int dpyPw = contextAh->dpyAttr[0].xres;
        int dpyPh = contextAh->dpyAttr[0].yres;
        int dpyEw = Context->dpyAttr[1].xres;
        int dpyEh = Context->dpyAttr[1].yres;
        if(dpyPw != dpyEw || dpyPh != dpyEh) {
            ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
            return -1;
        }
    }
#endif

    for(int k = 0; k < 1; k++) {
        if(pzone_mag->zone_info[k].scale_err || pzone_mag->zone_info[k].toosmall
            || pzone_mag->zone_info[k].zone_err || (pzone_mag->zone_info[k].transform
                && pzone_mag->zone_info[k].format != HAL_PIXEL_FORMAT_YCrCb_NV12 && 0==k)
                    || (pzone_mag->zone_info[k].transform && 1 == k)) {
            ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
            return -1;
        }
    }

    memcpy((void*)&gMixInfo,(void*)&gmixinfo[mix_index],sizeof(gMixInfo));
    for(i=0,j=0;i<pzone_mag->zone_cnt;i++) {
        //Set the layer which it's layer_index bigger than the first transform layer index to HWC_FRAMEBUFFER or HWC_NODRAW
        if(pzone_mag->zone_info[i].layer_index > yuvTenBitIndex) {
            hwc_layer_1_t * layer = &list->hwLayers[pzone_mag->zone_info[i].layer_index];
            //Judge the current layer whether backup in gmixinfo[mix_index] or not.
            if(Context->mLastCompType != HWC_RGA_VOP_GPU
                || gMixInfo.lastZoneCrc[pzone_mag->zone_info[i].layer_index] != pzone_mag->zone_info[i].zoneCrc
                || gMixInfo.gpu_draw_fd[pzone_mag->zone_info[i].layer_index] != pzone_mag->zone_info[i].layer_fd
                || gMixInfo.alpha[pzone_mag->zone_info[i].layer_index] != pzone_mag->zone_info[i].zone_alpha) {
                gpu_draw = 1;
                layer->compositionType = HWC_FRAMEBUFFER;
                gMixInfo.lastZoneCrc[pzone_mag->zone_info[i].layer_index] = pzone_mag->zone_info[i].zoneCrc;
                gMixInfo.gpu_draw_fd[pzone_mag->zone_info[i].layer_index] = pzone_mag->zone_info[i].layer_fd;
                gMixInfo.alpha[pzone_mag->zone_info[i].layer_index] = pzone_mag->zone_info[i].zone_alpha;
            } else
                layer->compositionType = HWC_NODRAW;

            if(gpu_draw && i == pzone_mag->zone_cnt - 1) {
                for(int j = 1; j < pzone_mag->zone_cnt; j++) {
                    layer = &list->hwLayers[j];
                    layer->compositionType = HWC_FRAMEBUFFER;
                }
                ALOGV(" need draw by gpu");
            }
            cntfb ++;
        } else {
            memcpy(&zone_info_ty[j], &pzone_mag->zone_info[i],sizeof(ZoneInfo));
            zone_info_ty[j].sort = 0;
            j++;
        }
    }

    memcpy(pzone_mag, &zone_info_ty,sizeof(zone_info_ty));
    pzone_mag->zone_cnt -= cntfb;
    for (i = 0; i < pzone_mag->zone_cnt; i++) {
        ALOGD_IF(log(HLLFIV),"%s,%d:Zone[%d]->layer[%d],"
            "[%d,%d,%d,%d] =>[%d,%d,%d,%d],"
            "w_h_s_f[%d,%d,%d,%d],tr_rtr_bled[%d,%d,%d],acq_fence_fd=%d",
            __FUNCTION__,__LINE__,
            Context->zone_manager.zone_info[i].zone_index,
            Context->zone_manager.zone_info[i].layer_index,
            Context->zone_manager.zone_info[i].src_rect.left,
            Context->zone_manager.zone_info[i].src_rect.top,
            Context->zone_manager.zone_info[i].src_rect.right,
            Context->zone_manager.zone_info[i].src_rect.bottom,
            Context->zone_manager.zone_info[i].disp_rect.left,
            Context->zone_manager.zone_info[i].disp_rect.top,
            Context->zone_manager.zone_info[i].disp_rect.right,
            Context->zone_manager.zone_info[i].disp_rect.bottom,
            Context->zone_manager.zone_info[i].width,
            Context->zone_manager.zone_info[i].height,
            Context->zone_manager.zone_info[i].stride,
            Context->zone_manager.zone_info[i].format,
            Context->zone_manager.zone_info[i].transform,
            Context->zone_manager.zone_info[i].realtransform,
            Context->zone_manager.zone_info[i].blend,
            Context->zone_manager.zone_info[i].acq_fence_fd);
    }

    pzone_mag->zone_info[0].sort = sort;
    for(i = 0; i < (pzone_mag->zone_cnt-1);) {
        pzone_mag->zone_info[i].sort = sort;
        sort_pre  = sort;
        cnt = 0;
        for (j = 1; j < 4 && (i+j) < pzone_mag->zone_cnt; j++) {
            ZoneInfo * next_zf = &(pzone_mag->zone_info[i+j]);
            bool is_combine = false;
            int k;
            for (k = 0;k <= cnt; k++)  {
                ZoneInfo * sorted_zf = &(pzone_mag->zone_info[i+j-1-k]);
                if(is_zone_combine(sorted_zf,next_zf))
                    is_combine = true;
                else {
                    is_combine = false;
                    break;
                }
            }
            if (is_combine) {
                pzone_mag->zone_info[i+j].sort = sort;
                cnt++;
            } else {
                sort++;
                pzone_mag->zone_info[i+j].sort = sort;
                cnt++;
                break;
            }
        }
        if(sort_pre == sort && (i+cnt) < (pzone_mag->zone_cnt-1))  // win2 ,4zones ,win3 4zones,so sort ++,but exit not ++
            sort ++;
        i += cnt;
    }
    if (sort >3) {
        ALOGD_IF(log(HLLTHR),"lcdc dont support 5 wins sort=%d",sort);
        return -1;
    }
    for(i = 0; i < pzone_mag->zone_cnt; i++) {
        int factor =1;
        ALOGV("sort[%d].type=%d",i,pzone_mag->zone_info[i].sort);
        if (pzone_mag->zone_info[i].sort == 1) {
            srot_tal[0][0]++;
            if(pzone_mag->zone_info[i].is_stretch)
                sort_stretch[0] = 1;
        } else if (pzone_mag->zone_info[i].sort == 2) {
            srot_tal[1][0]++;
            if (pzone_mag->zone_info[i].is_stretch)
                sort_stretch[1] = 1;
        } else if (pzone_mag->zone_info[i].sort == 3) {
            srot_tal[2][0]++;
            if (pzone_mag->zone_info[i].is_stretch)
                sort_stretch[2] = 1;
        }
        if(pzone_mag->zone_info[i].hfactor > hfactor_max)
            hfactor_max = pzone_mag->zone_info[i].hfactor;

        if(pzone_mag->zone_info[i].is_large )
            large_cnt ++;

        if(pzone_mag->zone_info[i].format== HAL_PIXEL_FORMAT_YCrCb_NV12)
            isyuv = true;

        if(Context->zone_manager.zone_info[i].hfactor > 1.0)
            factor = 2;
        else
            factor = 1;
        tsize += (Context->zone_manager.zone_info[i].size *factor);
    }

    j = 0;
    for (i = 0; i < 3; i++) {
        if (sort_stretch[i] == 1) {
            srot_tal[i][1] = win_disphed[j];  // win 0 and win 1 suporot stretch
            win_disphed_flag[j] = 1; // win0 ,win1 is dispatch flag
            ALOGV("stretch zones srot_tal[%d][1]=%d",i,srot_tal[i][1]);
            j++;
            if (j > 2) {
                ALOGD_IF(log(HLLFIV),"lcdc only has win0 and win1 supprot stretch");
                return -1;
            }
        }
    }

    if (hfactor_max >=1.4)
        bw += (j + 1);

    if (isyuv)
        bw +=5;

    ALOGV("large_cnt =%d,bw=%d",large_cnt , bw);

    for (i = 0; i < 3; i++) {
        if (srot_tal[i][1] == 0) {
            for (j = 0; j < 3; j++) {
                if(win_disphed_flag[j] == 0) // find the win had not dispatched
                    break;
            }
            if (j >= 3) {
                ALOGE("3 wins had beed dispatched ");
                return -1;
            }
            srot_tal[i][1] = win_disphed[j];
            win_disphed_flag[j] = 1;
            ALOGV("srot_tal[%d][1].dispatched=%d",i,srot_tal[i][1]);
        }
    }

    for (i = 0; i < pzone_mag->zone_cnt; i++) {
         switch(pzone_mag->zone_info[i].sort) {
            case 1:
                pzone_mag->zone_info[i].dispatched = srot_tal[0][1]++;
                break;
            case 2:
                pzone_mag->zone_info[i].dispatched = srot_tal[1][1]++;
                break;
            case 3:
                pzone_mag->zone_info[i].dispatched = srot_tal[2][1]++;
                break;
            default:
                ALOGE("try_wins_dispatch_mix_vh sort err!");
                return -1;
        }
        ALOGV("zone[%d].dispatched[%d]=%s,sort=%d", \
        i,pzone_mag->zone_info[i].dispatched,
        compositionTypeName[pzone_mag->zone_info[i].dispatched -1],
        pzone_mag->zone_info[i].sort);
    }

    for (i = 0; i < pzone_mag->zone_cnt; i++) {
        int disptched = pzone_mag->zone_info[i].dispatched;
        int sct_width = pzone_mag->zone_info[i].src_rect.right
                                            - pzone_mag->zone_info[i].src_rect.left;
        int sct_height = pzone_mag->zone_info[i].src_rect.bottom
                                            - pzone_mag->zone_info[i].src_rect.top;
        int dst_width = pzone_mag->zone_info[i].disp_rect.right
                                            - pzone_mag->zone_info[i].disp_rect.left;
        int dst_height = pzone_mag->zone_info[i].disp_rect.bottom
                                            - pzone_mag->zone_info[i].disp_rect.top;
        /*win2 win3 not support YUV*/
        if(disptched > win1 && is_yuv(pzone_mag->zone_info[i].format)) {
            ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
            return -1;
        }
        /*scal not support whoes source bigger than 2560 to dst 4k*/
        if(disptched <= win1 &&(sct_width > 2160 || sct_height > 2160) &&
            !is_yuv(pzone_mag->zone_info[i].format) && contextAh->mHdmiSI.NeedReDst) {
            ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
            return -1;
        }
        if(disptched <= win1 && (sct_width > 2560 || dst_width > 2560) &&
                                             !is_yuv(pzone_mag->zone_info[i].format)
                        && (sct_height != dst_height || Context->mResolutionChanged)) {
            ALOGD_IF(log(HLLFOU),"Policy out:%s,%d",__FUNCTION__,__LINE__);
            return -1;
        }
    }

#if USE_QUEUE_DDRFREQ
    if (Context->ddrFd > 0) {
        for (i = 0; i < pzone_mag->zone_cnt; i++) {
            int area_no = 0;
            int win_id = 0;
            ALOGD_IF(log(HLLFIV),"Zone[%d]->layer[%d],dispatched=%d,"
            "[%d,%d,%d,%d] =>[%d,%d,%d,%d],"
            "w_h_s_f[%d,%d,%d,%d],tr_rtr_bled[%d,%d,%d],"
            "layer_fd[%d],addr=%x,acq_fence_fd=%d",
            pzone_mag->zone_info[i].zone_index,
            pzone_mag->zone_info[i].layer_index,
            pzone_mag->zone_info[i].dispatched,
            pzone_mag->zone_info[i].src_rect.left,
            pzone_mag->zone_info[i].src_rect.top,
            pzone_mag->zone_info[i].src_rect.right,
            pzone_mag->zone_info[i].src_rect.bottom,
            pzone_mag->zone_info[i].disp_rect.left,
            pzone_mag->zone_info[i].disp_rect.top,
            pzone_mag->zone_info[i].disp_rect.right,
            pzone_mag->zone_info[i].disp_rect.bottom,
            pzone_mag->zone_info[i].width,
            pzone_mag->zone_info[i].height,
            pzone_mag->zone_info[i].stride,
            pzone_mag->zone_info[i].format,
            pzone_mag->zone_info[i].transform,
            pzone_mag->zone_info[i].realtransform,
            pzone_mag->zone_info[i].blend,
            pzone_mag->zone_info[i].layer_fd,
            pzone_mag->zone_info[i].addr,
            pzone_mag->zone_info[i].acq_fence_fd);
            switch(pzone_mag->zone_info[i].dispatched) {
                case win0:
                    bpvinfo.vopinfo[0].state = 1;
                    bpvinfo.vopinfo[0].zone_num ++;
                   break;
                case win1:
                    bpvinfo.vopinfo[1].state = 1;
                    bpvinfo.vopinfo[1].zone_num ++;
                    break;
                case win2_0:
                    bpvinfo.vopinfo[2].state = 1;
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_1:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_2:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                case win2_3:
                    bpvinfo.vopinfo[2].zone_num ++;
                    break;
                default:
                    ALOGE("hwc_dispatch_mix  err!");
                    return -1;
             }
        }
        bpvinfo.vopinfo[3].state = 1;
        bpvinfo.vopinfo[3].zone_num ++;
        bpvinfo.bp_size = Context->zone_manager.bp_size;
        tsize += Context->fbhandle.width * Context->fbhandle.height*4;
        if(tsize)
            tsize = tsize / (1024 *1024) * 60 ;// MB
        bpvinfo.bp_vop_size = tsize ;
        for (i = 0; i < 4; i++) {
            ALOGD_IF(log(HLLTHR),"RK_QUEDDR_FREQ mixinfo win[%d] bo_size=%dMB,bp_vop_size=%dMB,state=%d,num=%d",
                i,bpvinfo.bp_size,bpvinfo.bp_vop_size,bpvinfo.vopinfo[i].state,bpvinfo.vopinfo[i].zone_num);
        }
        if (ioctl(Context->ddrFd, RK_QUEDDR_FREQ, &bpvinfo)) {
            if(log(HLLTHR)) {
                for(i= 0;i<4;i++) {
                    ALOGD("RK_QUEDDR_FREQ mixinfo win[%d] bo_size=%dMB,bp_vop_size=%dMB,state=%d,num=%d",
                        i,bpvinfo.bp_size,bpvinfo.bp_vop_size,bpvinfo.vopinfo[i].state,bpvinfo.vopinfo[i].zone_num);
                }
            }
            return -1;
        }
    }
#endif
    //Mark the composer mode to HWC_MIX
    if (list) {
        list->hwLayers[yuvTenBitIndex].compositionType = HWC_MIX_V2;
        for (int i = 0; i < yuvTenBitIndex; i ++)
            list->hwLayers[i].compositionType = HWC_BLITTER;
    }

    memcpy(&Context->zone_manager,&zone_m,sizeof(ZoneManager));
    Context->mHdmiSI.mix_vh = true;
    Context->zone_manager.mCmpType = HWC_RGA_VOP_GPU;
    Context->zone_manager.composter_mode = HWC_MIX_V2;
    memcpy((void*)&gmixinfo[mix_index],(void*)&gMixInfo,sizeof(gMixInfo));

    return 0;
}

// return 0: suess
// return -1: fail
static int try_wins_dispatch_ver(void * ctx,hwc_display_contents_1_t * list)
{
    int win_disphed_flag[4] = {0,}; // win0, win1, win2, win3 flag which is dispatched
    int win_disphed[4] = {win0,win1,win2_0,win3_0};
    int i,j;
    hwcContext * Context = (hwcContext *)ctx;
    ZoneManager zone_m;
    initLayerCompositionType(Context,list);
    memcpy(&zone_m,&Context->zone_manager,sizeof(ZoneManager));
    ZoneManager* pzone_mag = &zone_m;
    // try dispatch stretch wins
    char const* compositionTypeName[] = {
            "win0",
            "win1",
            "win2_0",
            "win2_1",
            "win2_2",
            "win2_3",
            "win3_0",
            "win3_1",
            "win3_2",
            "win3_3",
            "win_ext"
            };

    // first dispatch stretch win
    if(pzone_mag->zone_cnt <=4)
    {
        for(i=0,j=0;i<pzone_mag->zone_cnt;i++)
        {
            if(pzone_mag->zone_info[i].is_stretch == true
                && pzone_mag->zone_info[i].dispatched == 0)
            {
                pzone_mag->zone_info[i].dispatched = win_disphed[j];  // win 0 and win 1 suporot stretch
                win_disphed_flag[j] = 1; // win2 ,win3 is dispatch flag
                ALOGV("stretch zones [%d]=%d",i,pzone_mag->zone_info[i].dispatched);
                j++;
                if(j > 2)  // lcdc only has win2 and win3 supprot more zones
                {
                    //ALOGD("lcdc only has win0 and win1 supprot stretch");
                    return -1;
                }
            }
        }
        // second dispatch common zones win
        for(i=0,j=0;i<pzone_mag->zone_cnt;i++)
        {
            if( pzone_mag->zone_info[i].dispatched == 0)  // had not dispatched
            {
                for(j=0;j<4;j++)
                {
                    if(win_disphed_flag[j] == 0) // find the win had not dispatched
                        break;
                }
                if(j>=4)
                {
                    ALOGE("4 wins had beed dispatched ");
                    return -1;
                }
                pzone_mag->zone_info[i].dispatched  = win_disphed[j];
                win_disphed_flag[j] = 1;
                ALOGV("zone[%d][1].dispatched=%d",i,pzone_mag->zone_info[i].dispatched);
            }
        }

    }
    else
    {
        for(i=0,j=0;i<pzone_mag->zone_cnt;i++)
        {
            if( pzone_mag->zone_info[i].dispatched == 0)  // had not dispatched
            {
                for(j=0;j<4;j++)
                {
                    if(win_disphed_flag[j] == 0) // find the win had not dispatched
                        break;
                }
                if(j>=4)
                {
                    bool isLandScape = ( (0==pzone_mag->zone_info[i].realtransform) \
                                || (HWC_TRANSFORM_ROT_180==pzone_mag->zone_info[i].realtransform) );
                    bool isSmallRect = (isLandScape && (pzone_mag->zone_info[i].height< (unsigned int)Context->fbhandle.height/4))  \
                                ||(!isLandScape && (pzone_mag->zone_info[i].width < (unsigned int)Context->fbhandle.width/4)) ;
                    if(isSmallRect)
                        pzone_mag->zone_info[i].dispatched  = win_ext;
                    else
                        return -1;  // too large
                }
                else
                {
                    pzone_mag->zone_info[i].dispatched  = win_disphed[j];
                    win_disphed_flag[j] = 1;
                    ALOGV("zone[%d].dispatched=%d",i,pzone_mag->zone_info[i].dispatched);
                }
            }
        }
    }

    Context->zone_manager.composter_mode = HWC_LCDC;
    return 0;
}

static int try_wins_dispatch_skip(void * ctx,hwc_display_contents_1_t * list)
{
    return -1;
}

#if G6110_SUPPORT_FBDC
static int check_zone(hwcContext * Context)
{
    ZoneManager* pzone_mag = &(Context->zone_manager);
    int iCountFBDC = 0;
    int win_id = -1,old_win_id = -1;
    static gralloc_module_t *psHal = NULL;

    if(Context == NULL)
    {
        LOGE("Context is null");
        return -1;
    }


    for(int i=0;i<pzone_mag->zone_cnt;i++)
    {
        switch(pzone_mag->zone_info[i].dispatched) {
            case win0:
                win_id = 0;
                break;
            case win1:
                win_id = 1;
                break;
            case win2_0:
            case win2_1:
            case win2_2:
            case win2_3:
                win_id = 2;
                break;
            case win3_0:
            case win3_1:
            case win3_2:
            case win3_3:
                win_id = 3;
                break;
            case win_ext:
                break;
            default:
                ALOGE("win err!");
                return -1;
         }

        //Count layers which used fbdc
        if(HALPixelFormatGetCompression(pzone_mag->zone_info[i].format) != HAL_FB_COMPRESSION_NONE
            && win_id != old_win_id)
        {
            iCountFBDC++;
        }

        old_win_id = win_id;

        /*if(iCountFBDC > 1 && HALPixelFormatGetCompression(pzone_mag->zone_info[i].format) != HAL_FB_COMPRESSION_NONE)
        {
            psHal->perform(psHal,
                                  GRALLOC_MODULE_SET_BUFFER_FORMAT_HINT_IMG,
                                  pzone_mag->zone_info[i].handle,
                                  HAL_PIXEL_FORMAT_RGBA_8888);
            ALOGD("IMG reset format to RGBA_8888");
            pzone_mag->zone_info[i].format = HAL_PIXEL_FORMAT_RGBA_8888;
        }*/

    }

    //If FBDC layers bigger than one,then go into GPU composition.
    if(iCountFBDC > 1)
    {
        return -1;
    }

    return 0;
}
#endif

static hwcSTATUS
hwcCheckFormat(
    IN  struct private_handle_t * Handle,
    OUT RgaSURF_FORMAT * Format

    )
{
    struct private_handle_t *handle = Handle;
    if (Format != NULL)
    {
        switch (GPU_FORMAT)
        {
        case HAL_PIXEL_FORMAT_RGB_565:
        case HAL_PIXEL_FORMAT_RGB_888:
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_RGBX_8888:
        case HAL_PIXEL_FORMAT_BGRA_8888:
        case HAL_PIXEL_FORMAT_YCrCb_NV12:
        case HAL_PIXEL_FORMAT_YCrCb_NV12_VIDEO:
#if G6110_SUPPORT_FBDC
        case FBDC_BGRA_8888:
        case FBDC_RGBA_8888:
#endif
             return hwcSTATUS_OK;
        default:
            ALOGE("%s:line=%d invalid format=0x%x",__func__,__LINE__,GPU_FORMAT);
            return hwcSTATUS_INVALID_ARGUMENT;
        }
    }


    return hwcSTATUS_OK;
}

static int skip_count = 0;
static uint32_t
check_layer(
    hwcContext * Context,
    uint32_t Count,
    uint32_t Index,
    bool videomode,
    hwc_layer_1_t * Layer
    )
{
    struct private_handle_t * handle =
        (struct private_handle_t *) Layer->handle;
    //(void) Context;

    (void) Count;
    (void) Index;

    if(0 == Index && (Layer->blending >> 16) < 250
        && ((Layer->blending & 0xffff) == HWC_BLENDING_PREMULT)){
        Context->mAlphaError = true;
    }else{
        Context->mAlphaError = false;
    }

    if(handle != NULL && Context->mVideoMode && Layer->transform != 0)
        Context->mVideoRotate=true;
    else
        Context->mVideoRotate=false;
    if (
#if !(defined(GPU_G6110) || defined(RK3288_BOX) || defined(RK3399_BOX))
        skip_count < 10 ||
#endif
        (handle && handle->type == 1 && !_contextAnchor->iommuEn)) {
        /* We are forbidden to handle this layer. */
        if(log(HLLTHR))
        {
            if(handle)
            {
                LOGD("Will not handle format=%x,handle_type=%d",GPU_FORMAT,handle->type);
            }
        }
        Layer->compositionType = HWC_FRAMEBUFFER;
        if (skip_count<10)
        {
        	skip_count++;
        }
        ALOGD_IF(log(HLLFOU),"Policy out [%d][%s]",__LINE__,__FUNCTION__);
        return HWC_FRAMEBUFFER;
    }
    // Force 4K transform video go into GPU
#if 0
    int w=0,h=0;
    w =  Layer->sourceCrop.right - Layer->sourceCrop.left;
    h =  Layer->sourceCrop.bottom - Layer->sourceCrop.top;

    if(Context->mVideoMode && (w>=3840 || h>=2160) && Layer->transform)
    {
        ALOGV("4K video transform=%d,w=%d,h=%d go into GPU",Layer->transform,w,h);
        return HWC_FRAMEBUFFER;
    }
#endif

#if !ENABLE_LCDC_IN_NV12_TRANSFORM
        if(Context->mGtsStatus)
#endif
        {
            ALOGV("In gts status,go into lcdc when rotate video");
            if(Layer->transform && handle && handle->format == HAL_PIXEL_FORMAT_YCrCb_NV12)
            {
                Context->mTrsfrmbyrga = true;
                LOGV("zxl:layer->transform=%d",Layer->transform );
            }
        }

    do
    {
        RgaSURF_FORMAT format = RK_FORMAT_UNKNOWN;

        /* TODO: I BELIEVE YOU CAN HANDLE SUCH LAYER!. */
        /* At least surfaceflinger can handle this layer. */
        Layer->compositionType = HWC_FRAMEBUFFER;

        /* Get format. */
		//zxl: remove hwcGetFormat,or it will let fbdc format return gpu.
        if(  /*hwcCheckFormat(handle, &format) != hwcSTATUS_OK
            ||*/ (LayerZoneCheck(Layer,Context == _contextAnchor ? HWC_DISPLAY_PRIMARY : HWC_DISPLAY_EXTERNAL) != 0))
        {
             return HWC_FRAMEBUFFER;
        }

        Layer->compositionType = HWC_LCDC;
        //ALOGD("win 0");
        break;


    }while (0);
    /* Return last composition type. */
    return Layer->compositionType;
}


/*****************************************************************************/

#if hwcDEBUG
static void
_Dump(
    hwc_display_contents_1_t* list
    )
{
    size_t i, j;

    for (i = 0; list && (i < (list->numHwLayers - 1)); i++)
    {
        hwc_layer_1_t const * l = &list->hwLayers[i];
        struct private_handle_t * handle = (struct private_handle_t *) (l->handle);
        if(l->flags & HWC_SKIP_LAYER)
        {
            LOGD("layer %p skipped", l);
        }
        else
        {
            for (j = 0; j < l->visibleRegionScreen.numRects; j++)
            {
                LOGD("\trect%d: {%d,%d,%d,%d}", j,
                     l->visibleRegionScreen.rects[j].left,
                     l->visibleRegionScreen.rects[j].top,
                     l->visibleRegionScreen.rects[j].right,
                     l->visibleRegionScreen.rects[j].bottom);
            }
        }
    }
}
#endif

#if hwcDumpSurface
static void
_DumpSurface(
    hwc_display_contents_1_t* list
    )
{
    size_t i;
    static int DumpSurfaceCount = 0;

    char pro_value[PROPERTY_VALUE_MAX];
    property_get("sys.dump",pro_value,0);
    //LOGI(" sys.dump value :%s",pro_value);
    if(!strcmp(pro_value,"true"))
    {
        for (i = 0; list && (i < (list->numHwLayers - 1)); i++)
        {
            hwc_layer_1_t const * l = &list->hwLayers[i];

            if(l->flags & HWC_SKIP_LAYER)
            {
                LOGI("layer %p skipped", l);
            }
            else
            {
                struct private_handle_t * handle_pre = (struct private_handle_t *) l->handle;
                int32_t SrcStride ;
                FILE * pfile = NULL;
                char layername[100] ;

                if( handle_pre == NULL || handle_pre->format == HAL_PIXEL_FORMAT_YCrCb_NV12_VIDEO)
                    continue;

                SrcStride = hwcGetBytePerPixelFromAndroidFromat(handle_pre->format);

                memset(layername,0,sizeof(layername));
                system("mkdir /data/dump/ && chmod /data/dump/ 777 ");
                //mkdir( "/data/dump/",777);
                sprintf(layername,"/data/dump/dmlayer%d_%d_%d_%d.bin",DumpSurfaceCount,handle_pre->stride,handle_pre->height,SrcStride);
                DumpSurfaceCount ++;
                pfile = fopen(layername,"wb");
                if(pfile)
                {
#ifdef GPU_G6110
                    fwrite((const void *)(handle_pre->pvBase),(size_t)(handle_pre->size),1,pfile);
#else
                    fwrite((const void *)(handle_pre->base),(size_t)(SrcStride * handle_pre->stride*handle_pre->height),1,pfile);
#endif
                    fclose(pfile);
                    LOGI(" dump surface layername %s,w:%d,h:%d,formatsize :%d",layername,handle_pre->width,handle_pre->height,SrcStride);
                }
            }
        }
    }
    property_set("sys.dump","false");
}
#endif

static void
hwcDumpArea(
    IN hwcArea * Area
    )
{
    hwcArea * area = Area;

    while (area != NULL)
    {
        char buf[128];
        char digit[8];
        bool first = true;

        sprintf(buf,
                "Area[%d,%d,%d,%d] owners=%08x:",
                area->rect.left,
                area->rect.top,
                area->rect.right,
                area->rect.bottom,
                area->owners);

        for (int i = 0; i < 32; i++)
        {
            /* Build decimal layer indices. */
            if (area->owners & (1U << i))
            {
                if (first)
                {
                    sprintf(digit, " %d", i);
                    strcat(buf, digit);
                    first = false;
                }
                else
                {
                    sprintf(digit, ",%d", i);
                    strcat(buf, digit);
                }
            }

            if (area->owners < (1 << i))
            {
                break;
            }
        }

        LOGD("%s", buf);

        /* Advance to next area. */
        area = area->next;
    }
}
static int CompareLines(int *da,int w)
{
    int i,j;
    for(i = 0;i<1;i++) // compare 4 lins
    {
        for(j= 0;j<w;j+=8)
        {
            if((unsigned int)*da != 0xff000000 && (unsigned int)*da != 0x0)
            {
                return 1;
            }
            da +=8;

        }
    }
    return 0;
}
static int CompareVers(int *da,int w,int h)
{
    int i,j;
    int *data ;
    for(i = 0;i<1;i++) // compare 4 lins
    {
        data = da + i;
        for(j= 0;j<h;j+=4)
        {
            if((unsigned int)*data != 0xff000000 && (unsigned int)*data != 0x0 )
            {
                return 1;
            }
            data +=4*w;
        }
    }
    return 0;
}

static int DetectValidData(int *data,int w,int h)
{
    int i,j;
    int *da;
    int ret;
    /*  detect model
    -------------------------
    |   |   |    |    |      |
    |   |   |    |    |      |
    |------------------------|
    |   |   |    |    |      |
    |   |   |    |    |      |
    |   |   |    |    |      |
    |------------------------|
    |   |   |    |    |      |
    |   |   |    |    |      |
    |------------------------|
    |   |   |    |    |      |
    |   |   |    |    |      |
    |------------------------|
    |   |   |    |    |      |
    --------------------------

    */
    if(data == NULL)
        return 1;
    for(i = 2;i<h;i+= 8)
    {
        da = data +  i *w;
        if(CompareLines(da,w))
            return 1;
    }
    //for(i = 8;i<w;i+= 8)
    //{
    //    da = data +  i ;
    //    if(CompareVers(da,w,h))
    //        return 1;
    //}

    return 0;

}

/**
 * @brief Sort by pos (positive-order)
 *
 * @param pos           0:ypos  1:xpos
 * @param win_id 		Win index
 * @param p_fb_info 	Win config data
 * @return 				Errno no
 */

static int  sort_area_by_pos(int pos,int win_id,struct rk_fb_win_cfg_data* p_fb_info)
{
    int i,j,k;
    bool bSwitch;
	if((win_id !=2 && win_id !=3) || p_fb_info==NULL || (pos != 0 && pos != 1))
	{
		ALOGW("%s(%d):invalid param",__FUNCTION__,__LINE__);
		return -1;
	}

	struct rk_fb_area_par tmp_fb_area;
	for(i=0;i<4;i++)
	{
		if(p_fb_info->win_par[i].win_id == win_id)
		{
		    for(j=0;j<3;j++)
		    {
		        bSwitch=false;
                for(k=RK_WIN_MAX_AREA-1;k>j;k--)
                {
                    if((p_fb_info->win_par[i].area_par[k].ion_fd || p_fb_info->win_par[i].area_par[k].phy_addr)  &&
                        (p_fb_info->win_par[i].area_par[k-1].ion_fd || p_fb_info->win_par[i].area_par[k-1].phy_addr) )
                        {
                            if(((pos == 0) && (p_fb_info->win_par[i].area_par[k].ypos < p_fb_info->win_par[i].area_par[k-1].ypos)) ||
                                ((pos == 1) && (p_fb_info->win_par[i].area_par[k].xpos < p_fb_info->win_par[i].area_par[k-1].xpos)) )
                            {
                                //switch
                                memcpy(&tmp_fb_area,&(p_fb_info->win_par[i].area_par[k-1]),sizeof(struct rk_fb_area_par));
                                memcpy(&(p_fb_info->win_par[i].area_par[k-1]),&(p_fb_info->win_par[i].area_par[k]),sizeof(struct rk_fb_area_par));
                                memcpy(&(p_fb_info->win_par[i].area_par[k]),&tmp_fb_area,sizeof(struct rk_fb_area_par));
                                bSwitch=true;
                            }
                        }
                }
                if(!bSwitch)    //end advance
                    return 0;
            }
            break;
        }
    }
	return 0;
}

//extern "C" void *blend(uint8_t *dst, uint8_t *src, int dst_w, int src_w, int src_h);
static int hwc_control_3dmode(int num,int flag)
{
    hwcContext * context = _contextAnchor1;
    if(context->fd_3d < 0 || !_contextAnchor1)
        return -1;

    int ret = 0;
    ssize_t err;
    char buf[200];
    int fd = context->fd_3d;
    switch(flag){
    case 0:
        memset(buf,0,sizeof(buf));
        lseek(fd,0,SEEK_SET);
        err = read(fd, buf, sizeof(buf));
        if(err <= 0)
            ALOGW("read hdmi 3dmode err=%d",err);

        int mode,hdmi3dmode;
        //ALOGI("line %d,buf[%s]",__LINE__,buf);
        sscanf(buf,"3dmodes=%d cur3dmode=%d",&mode,&hdmi3dmode);
        ALOGI_IF(log(HLLTWO),"hdmi3dmode=%d,mode=%d",hdmi3dmode,mode);

        if(8==hdmi3dmode)
            ret = 1;
        else if(6==hdmi3dmode)
            ret = 2;
        else if(0==hdmi3dmode)
            ret = 8;
        else
            ret = 0;
        break;

    case 1:
        lseek(fd,0,SEEK_SET);
        if(1==num)
            ret = write(fd,"8",2);
        else if(2==num)
            ret = write(fd,"6",2);
        else if(8==num)
            ret = write(fd,"0",2);
        else if(0==num)
            ret = write(fd,"-1",3);
        if(ret < 0)
            ALOGW("change 3dmode to %d err is %s",num,strerror(errno));
        break;

    default:
        break;
    }
    return ret;
}

int init_thread_pamaters(threadPamaters* mThreadPamaters)
{
    if(mThreadPamaters) {
        mThreadPamaters->count = 0;
        pthread_mutex_init(&mThreadPamaters->mtx, NULL);
        pthread_mutex_init(&mThreadPamaters->mlk, NULL);
        pthread_cond_init(&mThreadPamaters->cond, NULL);
    } else {
        ALOGE("{%s}%d,mThreadPamaters is NULL",__FUNCTION__,__LINE__);
    }
    return 0;
}

int free_thread_pamaters(threadPamaters* mThreadPamaters)
{
    if(mThreadPamaters) {
        pthread_mutex_destroy(&mThreadPamaters->mtx);
        pthread_mutex_destroy(&mThreadPamaters->mlk);
        pthread_cond_destroy(&mThreadPamaters->cond);
    } else {
        ALOGE("{%s}%d,mThreadPamaters is NULL",__FUNCTION__,__LINE__);
    }
    return 0;
}

void* hwc_control_3dmode_thread(void *arg)
{
    int ret = -1;
    int needStereo = 0;
    hwcContext *contextp = _contextAnchor;
    ALOGD("hwc_control_3dmode_thread creat");
    pthread_cond_wait(&contextp->mControlStereo.cond,&contextp->mControlStereo.mtx);
    while(true) {
        pthread_mutex_lock(&contextp->mControlStereo.mlk);
        needStereo = contextp->mControlStereo.count;
        pthread_mutex_unlock(&contextp->mControlStereo.mlk);
        ret = hwc_control_3dmode(2,0);
        if(needStereo != ret) {
            hwc_control_3dmode(needStereo,1);
            ALOGI_IF(log(HLLONE),"change stereo mode %d to %d",ret,needStereo);
        }
        ALOGD_IF(log(HLLTWO),"mControlStereo.count=%d",needStereo);
        pthread_cond_wait(&contextp->mControlStereo.cond,&contextp->mControlStereo.mtx);
    }
    ALOGD("hwc_control_3dmode_thread exit");
    pthread_exit(NULL);
    return NULL;
}

static int hwc_reset_rga_blit_rects(hwcContext * context, hwc_display_contents_1_t *list)
{
    int i,w,h,s,ret,relw,relh;
    float wscale,hscale;

    hwcContext *ctxp = _contextAnchor;

    if (!context)
        return -EINVAL;

    ret = i = w = h = s = relw = relh = 0;
    if (context->mContextIndex == 0) {
        w = context->dpyAttr[0].xres;
        h = context->dpyAttr[0].yres;
        relw = context->dpyAttr[0].relxres;
        relh = context->dpyAttr[0].relyres;
    } else if (context->mContextIndex == 1) {
        w = ctxp->dpyAttr[1].xres;
        h = ctxp->dpyAttr[1].xres;
        relw = context->dpyAttr[1].xres;
        relh = context->dpyAttr[1].yres;
    }

    wscale = (float)relw / w;
    hscale = (float)relh / h;

    for (unsigned int j = 0; j < list->numHwLayers; j++,i++){
        hwc_layer_1_t * layer = &list->hwLayers[i];

        if (layer->compositionType != HWC_BLITTER)
            continue;

        hwc_region_t * regions = &layer->visibleRegionScreen;

        if (regions->numRects > 30)
            regions->numRects = 30;

        hwc_rect_t * dstRect = &layer->displayFrame;

        if (i > MaxBlitNum) {
            ALOGE("RGA blit layers err");
            continue;
        }

        hwc_rect_t * rects = context->mRgaBlitRects[i];

        if (!rects) {
            ALOGE("RGA blit layers rects err");
            continue;
        }

        dstRect->left = (int)(dstRect->left * wscale);
        dstRect->right = (int)(dstRect->right * wscale);
        dstRect->top = (int)(dstRect->top * hscale);
        dstRect->bottom = (int)(dstRect->bottom * hscale);

        for (unsigned int j = 0; j < regions->numRects; j++) {
            rects[j].left = (int)(regions->rects[j].left * wscale);
            rects[j].right = (int)(regions->rects[j].right * wscale);
            rects[j].top = (int)(regions->rects[j].top * hscale);
            rects[j].bottom = (int)(regions->rects[j].bottom * hscale);
        }

        layer->visibleRegionScreen.rects = (hwc_rect_t const *)rects;
   }
   return 0;
}

int hwc_alloc_rga_blit_buffers(hwcContext * context)
{
    int i,w,h,s,fmt,usage,ret;

    if (!context)
        return -EINVAL;

    ret = i = w = h = s = fmt = usage = 0;
    if (context->mContextIndex == 0) {
        usage = GRALLOC_USAGE_HW_COMPOSER|GRALLOC_USAGE_HW_RENDER;
        fmt = context->fbhandle.format;
        w = context->dpyAttr[0].relxres;
        h = context->dpyAttr[0].relyres;
    } else if (context->mContextIndex == 1) {
        usage = GRALLOC_USAGE_HW_COMPOSER|GRALLOC_USAGE_HW_RENDER;
        fmt = context->fbhandle.format;
        w = context->dpyAttr[1].xres;
        h = context->dpyAttr[1].yres;
    }

    if (context->mCurRgaBlitBufferSize != w * h) {
        hwc_free_rga_blit_buffers(context);
        context->mCurRgaBlitBufferSize = w * h;
    }

    for (i = 0; i < MaxVideoBackBuffers; i++) {
        if (context->mRgaBlitBuffers[i] == NULL) {
            ret = hwc_alloc_buffer(&context->mRgaBlitBuffers[i],
                                                        w, h, &s, fmt, usage);
            if (ret)
                break;
        }
    }

    if (ret)
        hwc_free_rga_blit_buffers(context);

    return ret;
}

int hwc_free_rga_blit_buffers(hwcContext * context)
{
    int i,ret;

    if (!context)
        return -EINVAL;

    for (i = 0; i < MaxVideoBackBuffers; i++) {
        if (context->mRgaBlitBuffers[i] != NULL)
            ret = hwc_free_buffer(context->mRgaBlitBuffers[i]);
    }

    return 0;
}

buffer_handle_t hwc_rga_blit_get_next_buffer(hwcContext * context)
{
    int index;

    if (!context)
        return NULL;

    index = (context->mCurRgaBlitIndex + 1) % MaxVideoBackBuffers;

    context->mCurRgaBlitIndex = index;

    return context->mRgaBlitBuffers[index];
}

buffer_handle_t hwc_rga_blit_get_current_buffer(hwcContext * context)
{
    if (!context)
        return NULL;

    return context->mRgaBlitBuffers[context->mCurRgaBlitIndex];
}

static int dump_config_info(struct rk_fb_win_cfg_data fb_info ,hwcContext * context, int flag)
{
    bool listIsNull = true;
    char poutbuf[20];
    char eoutbuf[20];
    bool isLogOut = flag == 3;
    isLogOut = isLogOut || (log(HLLONE));

    if(!isLogOut){
        return 0;
    }

    switch(flag){
    case 0:
        strcpy(poutbuf,"Primary set:");
        strcpy(eoutbuf,"External set:");
        break;

    case 1:
        strcpy(poutbuf,"MIX Primary set:");
        strcpy(eoutbuf,"MIX External set:");
        break;

    case 2:
        strcpy(poutbuf,"Primary post:");
        strcpy(eoutbuf,"External post:");
        break;

    case 3:
        strcpy(poutbuf,"PCfg error:");
        strcpy(eoutbuf,"ECfg error:");
        break;

    case 4:
        strcpy(poutbuf,"last config:");
        strcpy(eoutbuf,"last config:");
        break;

    default:
        strcpy(poutbuf,"default:");
        strcpy(eoutbuf,"default:");
        break;
    }
    for(int i = 0;i<4;i++)
    {
        for(int j=0;j<4;j++)
        {
            if(fb_info.win_par[i].area_par[j].ion_fd || fb_info.win_par[i].area_par[j].phy_addr)
            {
                listIsNull = false;
                ALOGD("%s win[%d],area[%d],z_win[%d,%d],[%d,%d,%d,%d]=>[%d,%d,%d,%d],w_h_f[%d,%d,%d],fd=%d,addr=%x,fbFd=%d,fenceFd=%d",
                    context==_contextAnchor ? poutbuf : eoutbuf,
                    i,j,
                    fb_info.win_par[i].z_order,
                    fb_info.win_par[i].win_id,
                    fb_info.win_par[i].area_par[j].x_offset,
                    fb_info.win_par[i].area_par[j].y_offset,
                    fb_info.win_par[i].area_par[j].xact,
                    fb_info.win_par[i].area_par[j].yact,
                    fb_info.win_par[i].area_par[j].xpos,
                    fb_info.win_par[i].area_par[j].ypos,
                    fb_info.win_par[i].area_par[j].xsize,
                    fb_info.win_par[i].area_par[j].ysize,
                    fb_info.win_par[i].area_par[j].xvir,
                    fb_info.win_par[i].area_par[j].yvir,
                    fb_info.win_par[i].area_par[j].data_format,
                    fb_info.win_par[i].area_par[j].ion_fd,
                    fb_info.win_par[i].area_par[j].phy_addr,
                    context->fbFd,
                    fb_info.win_par[i].area_par[j].acq_fence_fd);
            }
        }
    }
    ALOGD_IF(listIsNull,"fbinfo is null when collect config");

    return 0;
}

static int init_every_to_skip_policy(hwcContext *context)
{
    for (int i =0; i < HWC_POLICY_NUM; i++)
        context->fun_policy[i] = try_wins_dispatch_skip;

    return 0;
}

static int init_common_policy(hwcContext *context)
{
    context->fun_policy[HWC_HOR] = try_wins_dispatch_hor;
    context->fun_policy[HWC_MIX_VTWO] = try_wins_dispatch_mix_v2;
    context->fun_policy[HWC_MIX_UP] = try_wins_dispatch_mix_up;
    context->fun_policy[HWC_MIX_DOWN] = try_wins_dispatch_mix_down;
    context->fun_policy[HWC_MIX_CROSS] = try_wins_dispatch_mix_cross;
    context->fun_policy[HWC_MIX_FPS] = try_wins_dispatch_skip;
    context->fun_policy[HWC_MIX_VH] = try_wins_dispatch_mix_vh;
    context->fun_policy[HWC_RGA_VOP_GPU] = try_rga_vop_gpu_policy;

    return 0;
}

static int init_big_vop_mipi_dual_out_policy(hwcContext *context)
{
    context->fun_policy[HWC_HOR] = try_wins_dispatch_win02;
    context->fun_policy[HWC_MIX_VTWO] = try_wins_dispatch_skip;
    context->fun_policy[HWC_MIX_UP] = try_wins_dispatch_skip;
    context->fun_policy[HWC_MIX_DOWN] = try_wins_dispatch_skip;
    context->fun_policy[HWC_MIX_CROSS] = try_wins_dispatch_skip;
    context->fun_policy[HWC_MIX_FPS] = try_wins_dispatch_skip;
    context->fun_policy[HWC_MIX_VH] = try_wins_dispatch_mix_win02;

    return 0;
}

static int init_lite_vop_mipi_dual_out_policy(hwcContext *context)
{
    context->fun_policy[HWC_HOR] = try_wins_dispatch_win0;
    context->fun_policy[HWC_MIX_VTWO] = try_wins_dispatch_skip;
    context->fun_policy[HWC_MIX_UP] = try_wins_dispatch_skip;
    context->fun_policy[HWC_MIX_DOWN] = try_wins_dispatch_skip;
    context->fun_policy[HWC_MIX_CROSS] = try_wins_dispatch_skip;
    context->fun_policy[HWC_MIX_FPS] = try_wins_dispatch_skip;
    context->fun_policy[HWC_MIX_VH] = try_wins_dispatch_skip;

    return 0;
}

static int init_lite_vop_policy(hwcContext *context)
{
    context->fun_policy[HWC_HOR] = try_wins_dispatch_win0;
    context->fun_policy[HWC_MIX_DOWN] = try_wins_dispatch_skip;
    context->fun_policy[HWC_MIX_CROSS] = try_wins_dispatch_skip;
    context->fun_policy[HWC_MIX_VTWO] = try_wins_dispatch_skip;
    context->fun_policy[HWC_MIX_FPS] = try_wins_dispatch_skip;
    context->fun_policy[HWC_MIX_UP] = try_wins_dispatch_skip;
    context->fun_policy[HWC_MIX_VH] = try_wins_dispatch_mix_vh;

    return 0;
}

static int hwc_collect_acquire_fence_fd(hwcContext * ctx,hwc_display_contents_1_t *list)
{
    ZoneManager* pzone_mag = NULL;
    int i,j,fd,layerIndex,num;
    char value[20] = "merge-acq";

    if (!ctx || !list)
        return -1;

    num = list->numHwLayers;
    pzone_mag = &(ctx->zone_manager);

#if USE_HWC_FENCE
    for(i = 0; i < pzone_mag->zone_cnt; i++) {
        fd = -1;
        layerIndex = pzone_mag->zone_info[i].layer_index;
        if (layerIndex < num)
            fd = list->hwLayers[layerIndex].acquireFenceFd;

        pzone_mag->zone_info[i].acq_fence_fd = fd;
    }
#endif

    return 0;
}

static int hwc_add_fbinfo(hwcContext * context, hwc_display_contents_1_t *list,struct rk_fb_win_cfg_data *fbinfo)
{
    hwcContext *contextp = _contextAnchor;
    char buf[100];
    int m = 0;
    int winid = 0;
    int xres = 0;
    int yres = 0;
    int w_dst = 0;
    int h_dst = 0;
    int hoffset = 0;
    int zorder = 0;
    int fd = contextp->screenFd;
    lseek(fd,0,SEEK_SET);
    if(read(fd,buf,sizeof(buf)) < 0){
        ALOGE("error reading fb screen_info: %s", strerror(errno));
        return -1;
    }
	sscanf(buf,"xres:%d yres:%d",&w_dst,&h_dst);
    ALOGD_IF(log(HLLONE),"width=%d,height=%d",w_dst,h_dst);
    xres = contextp->dpyAttr[HWC_DISPLAY_EXTERNAL].xres;
    yres = contextp->dpyAttr[HWC_DISPLAY_EXTERNAL].yres;

    hoffset = h_dst - 2 * yres;
    if(hoffset < 0) {
        ALOGW_IF(log(HLLTHR),"Is in frame package stereo %s,%d",__FUNCTION__,__LINE__);
        return -1;
    }

    for(int i = 1;i>=0;i--) {
        for(int j=3;j>=0;j--) {
            bool isNeedCopy = fbinfo->win_par[i].area_par[j].ion_fd != 0;
            isNeedCopy = isNeedCopy || fbinfo->win_par[i].area_par[j].phy_addr;
            isNeedCopy = isNeedCopy && fbinfo->win_par[i].area_par[j].reserved0 == HWCRFPS;
            if(isNeedCopy) {
                if(false) {
                    m = 1;
                    winid = 0;
                    zorder = 0;
                } else {
                    m = 2;
                    winid = 2;
                    zorder = fbinfo->win_par[i].z_order+1;
                }
                fbinfo->win_par[m].win_id                    = winid;
                fbinfo->win_par[m].z_order                   = zorder;
                fbinfo->win_par[m].area_par[0].x_offset      = fbinfo->win_par[i].area_par[j].x_offset;
                fbinfo->win_par[m].area_par[0].y_offset      = fbinfo->win_par[i].area_par[j].y_offset;
                fbinfo->win_par[m].area_par[0].xact          = fbinfo->win_par[i].area_par[j].xact;
                fbinfo->win_par[m].area_par[0].yact          = fbinfo->win_par[i].area_par[j].yact;
                fbinfo->win_par[m].area_par[0].xpos          = fbinfo->win_par[i].area_par[j].xpos;
                fbinfo->win_par[m].area_par[0].ypos          = fbinfo->win_par[i].area_par[j].ypos + hoffset + yres;
                fbinfo->win_par[m].area_par[0].xsize         = fbinfo->win_par[i].area_par[j].xsize;
                fbinfo->win_par[m].area_par[0].ysize         = fbinfo->win_par[i].area_par[j].ysize;
                fbinfo->win_par[m].area_par[0].reserved0     = fbinfo->win_par[i].area_par[j].reserved0;
                fbinfo->win_par[m].area_par[0].acq_fence_fd  = fbinfo->win_par[i].area_par[j].acq_fence_fd;
                fbinfo->win_par[m].area_par[0].ion_fd        = fbinfo->win_par[i].area_par[j].ion_fd;
                fbinfo->win_par[m].area_par[0].xvir          = fbinfo->win_par[i].area_par[j].xvir;
                fbinfo->win_par[m].area_par[0].yvir          = fbinfo->win_par[i].area_par[j].yvir;
                fbinfo->win_par[m].area_par[0].data_format   = fbinfo->win_par[i].area_par[j].data_format;
                ALOGD_IF(log(HLLONE),"Add new layer from[%d,%d,%d,%d]=>[%d,%d,%d,%d][m=%d]",
                    fbinfo->win_par[i].area_par[j].xpos,fbinfo->win_par[i].area_par[j].ypos,
                    fbinfo->win_par[i].area_par[j].xsize,fbinfo->win_par[i].area_par[j].ysize,
                    fbinfo->win_par[m].area_par[0].xpos,fbinfo->win_par[m].area_par[0].ypos,
                    fbinfo->win_par[m].area_par[0].xsize,fbinfo->win_par[m].area_par[0].ysize,m);
            }
        }
    }
    return 0;
}

static int hwc_add_write_back(hwcContext * context, buffer_handle_t *hnd,
    struct rk_fb_win_cfg_data *fbinfo1, struct rk_fb_win_cfg_data *fbinfo2)
{
#if 0
    int w,h,s,fmt,usage;
    w = context->fbhandle.width;
    h = context->fbhandle.height;
    fmt = HAL_PIXEL_FORMAT_BGRA_8888;

    usage = GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_HW_RENDER;

    int ret = hwc_alloc_buffer(hnd,w,h,&s,fmt,usage);

    if (ret) {
        ALOGE("alloc buffer fail");
        return ret;
    }

    struct private_handle_t*  handle = (struct private_handle_t*)*hnd;
    if (!handle) {
		ALOGE("hanndle=NULL at line %d",__LINE__);
        return -EINVAL;
    }

    fbinfo1->wb_cfg.data_format = fmt;
    fbinfo1->wb_cfg.ion_fd = handle->share_fd;
    fbinfo1->wb_cfg.phy_addr = 0;
	fbinfo1->wb_cfg.xsize = w;
    fbinfo1->wb_cfg.ysize = h;

    memset(fbinfo2,0,sizeof(struct rk_fb_win_cfg_data));
    fbinfo2->ret_fence_fd = -1;
    for(int i=0;i<RK_MAX_BUF_NUM;i++) {
        fbinfo2->rel_fence_fd[i] = -1;
    }

    fbinfo2->win_par[0].area_par[0].data_format = fmt;

    fbinfo2->win_par[0].win_id = 0;
    fbinfo2->win_par[0].z_order = 0;
    fbinfo2->win_par[0].area_par[0].ion_fd = handle->share_fd;
    fbinfo2->win_par[0].area_par[0].acq_fence_fd = -1;

    fbinfo2->win_par[0].area_par[0].x_offset = 0;
    fbinfo2->win_par[0].area_par[0].y_offset = 0;
    fbinfo2->win_par[0].area_par[0].xpos = 0;
    fbinfo2->win_par[0].area_par[0].ypos = 0;
    fbinfo2->win_par[0].area_par[0].xsize = w;
    fbinfo2->win_par[0].area_par[0].ysize = h;
    fbinfo2->win_par[0].area_par[0].xact = w;
    fbinfo2->win_par[0].area_par[0].yact = h;
    fbinfo2->win_par[0].area_par[0].xvir = w;
    fbinfo2->win_par[0].area_par[0].yvir = h;
	fbinfo2->wait_fs = 0;

    ret = 0;

    return ret;
#else
    return 0;
#endif
}


static bool hwc_check_cfg(hwcContext * ctx,struct rk_fb_win_cfg_data fb_info)
{
    bool ret = true;
    bool z_ret = false;
    if(!ctx) {
        ret = false;
        return ret;
    }
    if (ctx->isVr)
	return true;
    int width,height;
    hwcContext * context = _contextAnchor;
    if(ctx==context) {
        width  = context->dpyAttr[HWCP].xres;
        height = context->dpyAttr[HWCP].yres;
    } else {
        width  = context->dpyAttr[HWCE].xres;
        height = context->dpyAttr[HWCE].yres;
    }
    if(ctx->zone_manager.mCmpType == HWC_MIX_DOWN) {
        z_ret = true;
    }
    for(int i = 0;i<4;i++){
        for(int j=0;j<4;j++){
            if(fb_info.win_par[i].area_par[j].ion_fd || fb_info.win_par[i].area_par[j].phy_addr){
                int z_order = fb_info.win_par[i].z_order;
                int win_id = fb_info.win_par[i].win_id;
                int x_offset = fb_info.win_par[i].area_par[j].x_offset;
                int y_offset = fb_info.win_par[i].area_par[j].y_offset;
                int xact = fb_info.win_par[i].area_par[j].xact;
                int yact = fb_info.win_par[i].area_par[j].yact;
                int xpos = fb_info.win_par[i].area_par[j].xpos;
                int ypos = fb_info.win_par[i].area_par[j].ypos;
                int xsize = fb_info.win_par[i].area_par[j].xsize;
                int ysize = fb_info.win_par[i].area_par[j].ysize;
                int xvir = fb_info.win_par[i].area_par[j].xvir;
                int yvir = fb_info.win_par[i].area_par[j].yvir;
                int data_format = fb_info.win_par[i].area_par[j].data_format;

                z_ret = z_ret || (z_order == 0);//z_order At least once to 0
                ret = ret && (x_offset + xact <= xvir);
                ret = ret && (y_offset + yact <= yvir);
                ret = ret && (xpos + xsize <= width);
                ret = ret && (ypos + ysize <= height);
                if(win_id >= 2){
                    ret = ret && (xact == xsize);
                    ret = ret && (yact == ysize);
#ifndef USE_AFBC_LAYER
                    ret = ret && (data_format <= 7);
#endif
                }

                if(!ret){
                    ALOGW("%s[%d,%d]w[%d],a[%d],z_win[%d,%d],[%d,%d,%d,%d]=>[%d,%d,%d,%d],w_h_f[%d,%d,%d]",
                          ctx==_contextAnchor ? "PCfg err" : "ECfg err",width,height,i,j,z_order,win_id,
                          x_offset,y_offset,xact,yact,xpos,ypos,xsize,ysize,xvir,yvir,data_format);
                    break;
                }
            }
        }
        if(!ret){
            break;
        }
    }

    ret = ret && z_ret;
    return ret;
}

static int hwc_collect_cfg(hwcContext * context, hwc_display_contents_1_t *list,struct hwc_fb_info *hfi,int mix_flag,bool mix_prepare)
{
    ZoneManager* pzone_mag = &(context->zone_manager);
    hwcContext * ctxp = _contextAnchor;
    int dpyID = 0;
    int i,j;
    int z_order = 0;
    int win_no = 0;
    bool isRealyMix = mix_flag;
    int is_spewin = is_special_wins(context);
    struct rk_fb_win_cfg_data fb_info;
    int comType = pzone_mag->mCmpType;

    dpyID = (ctxp == context) ? 0 : 1;
    memset((void*)&fb_info,0,sizeof(fb_info));
    fb_info.ret_fence_fd = -1;
    for(i=0;i<RK_MAX_BUF_NUM;i++) {
        hfi->pRelFenceFd[i] = 0;
        fb_info.rel_fence_fd[i] = -1;
    }

    if(mix_flag == 1 && !context->mHdmiSI.mix_up){
        z_order ++;
    }
    for(i=0;i<pzone_mag->zone_cnt;i++){
        hwc_rect_t * psrc_rect = &(pzone_mag->zone_info[i].src_rect);
        hwc_rect_t * pdisp_rect = &(pzone_mag->zone_info[i].disp_rect);
        int area_no = 0;
        int win_id = 0;
        int raw_format=hwChangeFormatandroidL(pzone_mag->zone_info[i].format);

        ALOGD_IF(log(HLLONE),"hwc_set_lcdc Zone[%d]->layer[%d],dispatched=%d,"
        "[%d,%d,%d,%d] =>[%d,%d,%d,%d],"
        "w_h_s_f[%d,%d,%d,0x%x],tr_rtr_bled[%d,%d,%d],"
        "layer_fd[%d],addr=%x,acq_fence_fd=%d",
        pzone_mag->zone_info[i].zone_index,
        pzone_mag->zone_info[i].layer_index,
        pzone_mag->zone_info[i].dispatched,
        pzone_mag->zone_info[i].src_rect.left,
        pzone_mag->zone_info[i].src_rect.top,
        pzone_mag->zone_info[i].src_rect.right,
        pzone_mag->zone_info[i].src_rect.bottom,
        pzone_mag->zone_info[i].disp_rect.left,
        pzone_mag->zone_info[i].disp_rect.top,
        pzone_mag->zone_info[i].disp_rect.right,
        pzone_mag->zone_info[i].disp_rect.bottom,
        pzone_mag->zone_info[i].width,
        pzone_mag->zone_info[i].height,
        pzone_mag->zone_info[i].stride,
        pzone_mag->zone_info[i].format,
        pzone_mag->zone_info[i].transform,
        pzone_mag->zone_info[i].realtransform,
        pzone_mag->zone_info[i].blend,
        pzone_mag->zone_info[i].layer_fd,
        pzone_mag->zone_info[i].addr,
        pzone_mag->zone_info[i].acq_fence_fd);

        switch(pzone_mag->zone_info[i].dispatched) {
            case win0:
                win_no ++;
                win_id = 0;
                area_no = 0;
                ALOGV("[%d]dispatched=%d,z_order=%d",i,pzone_mag->zone_info[i].dispatched,z_order);
                z_order++;
                break;
            case win1:
                ALOGV("[%d]dispatched=%d,z_order=%d",i,pzone_mag->zone_info[i].dispatched,z_order);
                win_no ++;
                win_id = 1;
                area_no = 0;
                z_order++;
                break;
            case win2_0:
                ALOGV("[%d]dispatched=%d,z_order=%d",i,pzone_mag->zone_info[i].dispatched,z_order);
                win_no ++;
                win_id = 2;
                area_no = 0;
                z_order++;
                break;
            case win2_1:
                win_id = 2;
                area_no = 1;
                ALOGV("[%d]dispatched=%d,z_order=%d",i,pzone_mag->zone_info[i].dispatched,z_order);
                break;
            case win2_2:
                win_id = 2;
                area_no = 2;
                ALOGV("[%d]dispatched=%d,z_order=%d",i,pzone_mag->zone_info[i].dispatched,z_order);
                break;
            case win2_3:
                win_id = 2;
                area_no = 3;
                ALOGV("[%d]dispatched=%d,z_order=%d",i,pzone_mag->zone_info[i].dispatched,z_order);
                break;
            case win3_0:
                win_no ++;
                win_id = 3;
                area_no = 0;
                ALOGV("[%d]dispatched=%d,z_order=%d",i,pzone_mag->zone_info[i].dispatched,z_order);
                z_order++;
                break;
            case win3_1:
                win_id = 3;
                area_no = 1;
                ALOGV("[%d]dispatched=%d,z_order=%d",i,pzone_mag->zone_info[i].dispatched,z_order);
                break;
            case win3_2:
                win_id = 3;
                area_no = 2;
                ALOGV("[%d]dispatched=%d,z_order=%d",i,pzone_mag->zone_info[i].dispatched,z_order);
                break;
            case win3_3:
                win_id = 3;
                area_no = 3;
                ALOGV("[%d]dispatched=%d,z_order=%d",i,pzone_mag->zone_info[i].dispatched,z_order);
                break;
             case win_ext:
                break;
            default:
                ALOGE("hwc_set_lcdc  err!");
                return -1;
         }

#if G6110_SUPPORT_FBDC
    if(HALPixelFormatGetCompression(pzone_mag->zone_info[i].format) != HAL_FB_COMPRESSION_NONE){
        raw_format = HALPixelFormatGetRawFormat(pzone_mag->zone_info[i].format);
        switch(raw_format){
            case HAL_PIXEL_FORMAT_RGB_565:
                raw_format = FBDC_RGB_565;
                fb_info.win_par[win_no-1].area_par[area_no].fbdc_data_format = FBDC_RGB_565;
                fb_info.win_par[win_no-1].area_par[area_no].fbdc_en= 1;
                fb_info.win_par[win_no-1].area_par[area_no].fbdc_cor_en = 0;
                break;
            case HAL_PIXEL_FORMAT_BGRA_8888:
            case HAL_PIXEL_FORMAT_RGBA_8888:
                raw_format = FBDC_ABGR_888;
                fb_info.win_par[win_no-1].area_par[area_no].fbdc_data_format = FBDC_ABGR_888;
                fb_info.win_par[win_no-1].area_par[area_no].fbdc_en= 1;
                fb_info.win_par[win_no-1].area_par[area_no].fbdc_cor_en = 0;
                break;
            default:
                ALOGE("Unsupport format 0x%x",raw_format);
                break;
        }
    }
#endif

        if(win_no ==1 && !mix_flag)         {
            if(raw_format ==  HAL_PIXEL_FORMAT_RGBA_8888){
                fb_info.win_par[win_no-1].area_par[area_no].data_format = HAL_PIXEL_FORMAT_RGBX_8888;
            }else{
                fb_info.win_par[win_no-1].area_par[area_no].data_format =  raw_format;
            }
        }else{
            fb_info.win_par[win_no-1].area_par[area_no].data_format =  raw_format;
        }
        fb_info.win_par[win_no-1].win_id = win_id;
        fb_info.win_par[win_no-1].alpha_mode = AB_SRC_OVER;
        fb_info.win_par[win_no-1].g_alpha_val =  pzone_mag->zone_info[i].zone_alpha;
        fb_info.win_par[win_no-1].z_order = z_order-1;
        fb_info.win_par[win_no-1].area_par[area_no].ion_fd = \
                        pzone_mag->zone_info[i].direct_fd ? \
                        pzone_mag->zone_info[i].direct_fd: pzone_mag->zone_info[i].layer_fd;
        fb_info.win_par[win_no-1].area_par[area_no].phy_addr = pzone_mag->zone_info[i].addr;
#if USE_HWC_FENCE
        if (context->isRk3399)
            fb_info.win_par[win_no-1].area_par[area_no].acq_fence_fd = pzone_mag->zone_info[i].acq_fence_fd;
        else
            fb_info.win_par[win_no-1].area_par[area_no].acq_fence_fd = -1;
#else
        fb_info.win_par[win_no-1].area_par[area_no].acq_fence_fd = -1;
#endif
        fb_info.win_par[win_no-1].area_par[area_no].x_offset = hwcMAX(psrc_rect->left, 0);
        fb_info.win_par[win_no-1].area_par[area_no].y_offset = hwcMAX(psrc_rect->top, 0);
        fb_info.win_par[win_no-1].area_par[area_no].xpos =  hwcMAX(pdisp_rect->left, 0);
        fb_info.win_par[win_no-1].area_par[area_no].ypos = hwcMAX(pdisp_rect->top , 0);
        fb_info.win_par[win_no-1].area_par[area_no].xsize = pdisp_rect->right - pdisp_rect->left;
        fb_info.win_par[win_no-1].area_par[area_no].ysize = pdisp_rect->bottom - pdisp_rect->top;
        hfi->pRelFenceFd[i] = pzone_mag->zone_info[i].pRelFenceFd;
        if(context->zone_manager.mCmpType == HWC_MIX_FPS) {
            if(pzone_mag->zone_info[i].alreadyStereo != 8) {
                fb_info.win_par[win_no-1].area_par[area_no].reserved0 = HWCRFPS;
            }
        }
        if(pzone_mag->zone_info[i].transform == HWC_TRANSFORM_ROT_90
            || pzone_mag->zone_info[i].transform == HWC_TRANSFORM_ROT_270){

            if(!context->mNV12_VIDEO_VideoMode){
                fb_info.win_par[win_no-1].area_par[area_no].xact = psrc_rect->right- psrc_rect->left;
                fb_info.win_par[win_no-1].area_par[area_no].yact = psrc_rect->bottom - psrc_rect->top;
            }else{
                //Only for NV12_VIDEO
                fb_info.win_par[win_no-1].area_par[area_no].xact = psrc_rect->bottom - psrc_rect->top;
                fb_info.win_par[win_no-1].area_par[area_no].yact = psrc_rect->right- psrc_rect->left;
            }

            fb_info.win_par[win_no-1].area_par[area_no].xvir = pzone_mag->zone_info[i].height ;
            fb_info.win_par[win_no-1].area_par[area_no].yvir = pzone_mag->zone_info[i].stride;
        }else{
            fb_info.win_par[win_no-1].area_par[area_no].xact = psrc_rect->right- psrc_rect->left;
            fb_info.win_par[win_no-1].area_par[area_no].yact = psrc_rect->bottom - psrc_rect->top;
            fb_info.win_par[win_no-1].area_par[area_no].xvir = pzone_mag->zone_info[i].stride;
            fb_info.win_par[win_no-1].area_par[area_no].yvir = pzone_mag->zone_info[i].height;
        }

#ifdef USE_AFBC_LAYER
        {
            uint64_t internal_format = pzone_mag->zone_info[i].internal_format;
            D("internal_format of zone_%d: 0x%llx.", i, internal_format);

            if ( isAfbcInternalFormat(internal_format) )
            {
                uint64_t index_of_arm_hal_format = internal_format & GRALLOC_ARM_INTFMT_FMT_MASK;

                switch (  index_of_arm_hal_format )
                {
                    case GRALLOC_ARM_HAL_FORMAT_INDEXED_RGBA_8888:
                        D("to set afbc_config for rgba_888.");
                        // fb_info.win_par[win_no-1].area_par[area_no].fbdc_data_format = FBDC_ARGB_888;
                        fb_info.win_par[win_no-1].area_par[area_no].fbdc_data_format = 0x27;
                        fb_info.win_par[win_no-1].area_par[area_no].fbdc_en = 1;
                        fb_info.win_par[win_no-1].area_par[area_no].fbdc_cor_en = 0;

                        fb_info.win_par[win_no-1].area_par[area_no].data_format = 0x27;
                        break;

                    case GRALLOC_ARM_HAL_FORMAT_INDEXED_RGBX_8888:
                        D("to set afbc_config for rgbx_888.");
                        // fb_info.win_par[win_no-1].area_par[area_no].fbdc_data_format = FBDC_ARGB_888;
                        fb_info.win_par[win_no-1].area_par[area_no].fbdc_data_format = 0x27;
                        fb_info.win_par[win_no-1].area_par[area_no].fbdc_en = 1;
                        fb_info.win_par[win_no-1].area_par[area_no].fbdc_cor_en = 0;

                        fb_info.win_par[win_no-1].area_par[area_no].data_format = 0x27;
                        break;

                    default:
                        E("unsupported index_of_arm_hal_format : 0x%llx.", index_of_arm_hal_format);
                        break;
                }
            }
        }
#endif

#if G6110_SUPPORT_FBDC
        //zxl: xact need 16bytes aligned and yact need 4bytes aligned in FBDC area.
        if(!mix_prepare && fb_info.win_par[win_no-1].area_par[area_no].fbdc_en == 1)
        {
            if(!IS_ALIGN(fb_info.win_par[win_no-1].area_par[area_no].yact,4))
            {
                fb_info.win_par[win_no-1].area_par[area_no].yact=ALIGN(fb_info.win_par[win_no-1].area_par[area_no].yact,4)-4;
            }
            if(!IS_ALIGN(fb_info.win_par[win_no-1].area_par[area_no].xact,16))
            {
                fb_info.win_par[win_no-1].area_par[area_no].xact=ALIGN(fb_info.win_par[win_no-1].area_par[area_no].yact,16)-16;
            }
        }
#endif
    }


#ifndef GPU_G6110
    //win2 & win3 need sort by ypos (positive-order)
    sort_area_by_pos(0,2,&fb_info);
    sort_area_by_pos(0,3,&fb_info);
#else
    //win2 & win3 need sort by xpos (positive-order)
    sort_area_by_pos(1,2,&fb_info);
    sort_area_by_pos(1,3,&fb_info);
#endif

#if VIDEO_UI_OPTIMATION
    if((fb_info.win_par[0].area_par[0].data_format == HAL_PIXEL_FORMAT_YCrCb_NV12_OLD
        || fb_info.win_par[0].area_par[0].data_format ==  HAL_PIXEL_FORMAT_YCrCb_NV12_10_OLD)
        && list->numHwLayers == 3)  // @ video & 2 layers
    {
        bool IsDiff = true;
        int ret;
        hwc_layer_1_t * layer = &list->hwLayers[1];
        if(layer){
            struct private_handle_t* handle = (struct private_handle_t *) layer->handle;
            if(handle && (handle->format == HAL_PIXEL_FORMAT_RGBA_8888 ||
                    handle->format == HAL_PIXEL_FORMAT_RGBX_8888 ||
                    handle->format == HAL_PIXEL_FORMAT_BGRA_8888)){
                IsDiff = handle->share_fd != context->vui_fd;
            }
            if(IsDiff){
                context->vui_hide = 0;
            }else if(!context->vui_hide){
                ret = DetectValidData((int *)(GPU_BASE),handle->width,handle->height);
                if(!ret){
                    context->vui_hide = 1;
                    ALOGD(" @video UI close");
                }
            }
            // close UI win:external always do it
            if(context->vui_hide == 1){
                for(i = 1;i<4;i++){
                    for(j=0;j<4;j++){
                        fb_info.win_par[i].area_par[j].ion_fd = 0;
                        fb_info.win_par[i].area_par[j].phy_addr = 0;
                    }
                }
            }
            if (handle)
                context->vui_fd = handle->share_fd;
            else
                context->vui_fd = -1;
        }
    }
#endif

#if DEBUG_CHECK_WIN_CFG_DATA
    for(i = 0;i<4;i++){
        for(j=0;j<4;j++){
            if(fb_info.win_par[i].area_par[j].ion_fd || fb_info.win_par[i].area_par[j].phy_addr){
                #if 1
                if(fb_info.win_par[i].z_order<0 ||
                fb_info.win_par[i].win_id < 0 || fb_info.win_par[i].win_id > 4 ||
                fb_info.win_par[i].g_alpha_val < 0 || fb_info.win_par[i].g_alpha_val > 0xFF ||
                fb_info.win_par[i].area_par[j].x_offset < 0 || fb_info.win_par[i].area_par[j].y_offset < 0 ||
                fb_info.win_par[i].area_par[j].x_offset > 4096 || fb_info.win_par[i].area_par[j].y_offset > 4096 ||
                fb_info.win_par[i].area_par[j].xact < 0 || fb_info.win_par[i].area_par[j].yact < 0 ||
                fb_info.win_par[i].area_par[j].xact > 4096 || fb_info.win_par[i].area_par[j].yact > 4096 ||
                fb_info.win_par[i].area_par[j].xpos < 0 || fb_info.win_par[i].area_par[j].ypos < 0 ||
                fb_info.win_par[i].area_par[j].xpos >4096 || fb_info.win_par[i].area_par[j].ypos > 4096 ||
                fb_info.win_par[i].area_par[j].xsize < 0 || fb_info.win_par[i].area_par[j].ysize < 0 ||
                fb_info.win_par[i].area_par[j].xsize > 4096 || fb_info.win_par[i].area_par[j].ysize > 4096 ||
                fb_info.win_par[i].area_par[j].xvir < 0 ||  fb_info.win_par[i].area_par[j].yvir < 0 ||
                fb_info.win_par[i].area_par[j].xvir > 4096 || fb_info.win_par[i].area_par[j].yvir > 4096 ||
                fb_info.win_par[i].area_par[j].ion_fd < 0)
                #endif
                ALOGE("%s:line=%d,par[%d],area[%d],z_win_galp[%d,%d,%x],[%d,%d,%d,%d]=>[%d,%d,%d,%d],w_h_f[%d,%d,%d],acq_fence_fd=%d,fd=%d,addr=%x",
                        __func__,__LINE__,i,j,
                        fb_info.win_par[i].z_order,
                        fb_info.win_par[i].win_id,
                        fb_info.win_par[i].g_alpha_val,
                        fb_info.win_par[i].area_par[j].x_offset,
                        fb_info.win_par[i].area_par[j].y_offset,
                        fb_info.win_par[i].area_par[j].xact,
                        fb_info.win_par[i].area_par[j].yact,
                        fb_info.win_par[i].area_par[j].xpos,
                        fb_info.win_par[i].area_par[j].ypos,
                        fb_info.win_par[i].area_par[j].xsize,
                        fb_info.win_par[i].area_par[j].ysize,
                        fb_info.win_par[i].area_par[j].xvir,
                        fb_info.win_par[i].area_par[j].yvir,
                        fb_info.win_par[i].area_par[j].data_format,
                        fb_info.win_par[i].area_par[j].acq_fence_fd,
                        fb_info.win_par[i].area_par[j].ion_fd,
                        fb_info.win_par[i].area_par[j].phy_addr);
              }
        }

    }
#endif

#if USE_HWC_FENCE
#if SYNC_IN_VIDEO
    if(context->mVideoMode && !context->mIsMediaView && !g_hdmi_mode)
        fb_info.wait_fs=1;
    else
#endif
        fb_info.wait_fs=0;  //not wait acquire fence temp(wait in hwc)
#endif
    if(comType == HWC_MIX_FPS) {
        isRealyMix = false;
        for(unsigned int k=0;k<list->numHwLayers - 1;k++) {
            hwc_layer_1_t * layer = &list->hwLayers[k];
            if(layer->compositionType == HWC_FRAMEBUFFER) {
                isRealyMix = true;
            }
        }
    }
    //if primary the y_offset will be n times of height
    if((mix_flag && isRealyMix)&& !mix_prepare){
        int numLayers = list->numHwLayers;
        int format = -1;
        hwc_layer_1_t *fbLayer = &list->hwLayers[numLayers - 1];
        if (!fbLayer){
            ALOGE("fbLayer=NULL");
            return -1;
        }
        struct private_handle_t*  handle = (struct private_handle_t*)fbLayer->handle;
        if (!handle){
            ALOGD_IF(log(HLLFOU),"hanndle=NULL at line %d",__LINE__);
            return -1;
        }

        win_no ++;
        if(mix_flag == 1 && !context->mHdmiSI.mix_up){
            format = context->fbhandle.format;
            z_order=1;
        }else if(mix_flag == 2 || (mix_flag == 1 && context->mHdmiSI.mix_up)){
            format = HAL_PIXEL_FORMAT_RGBA_8888;
            z_order++;
        }

#if G6110_SUPPORT_FBDC
        if(context->fbhandle.format == FBDC_ABGR_888)
        {
            format = context->fbhandle.format;
            fb_info.win_par[win_no-1].area_par[0].fbdc_data_format = FBDC_ABGR_888;
            fb_info.win_par[win_no-1].area_par[0].fbdc_en= 1;
            fb_info.win_par[win_no-1].area_par[0].fbdc_cor_en = 0;
        }
#endif

        ALOGV("mix_flag=%d,win_no =%d,z_order = %d",mix_flag,win_no,z_order);
        unsigned int offset = handle->offset;
        fb_info.win_par[win_no-1].area_par[0].data_format = format;
        if(context->mHdmiSI.mix_vh && mix_flag == 2){
            fb_info.win_par[win_no-1].win_id = 1;
        }else{
            fb_info.win_par[win_no-1].win_id = 3;
        }
        fb_info.win_par[win_no-1].z_order = z_order-1;
        if (pzone_mag->mCmpType == HWC_MIX_CROSS) {
            fb_info.win_par[win_no-1].z_order = z_order-2;
            fb_info.win_par[win_no-2].z_order = z_order-1;
        }
        if (!context->mComVop && context->mHdmiSI.mix_vh) {
            fb_info.win_par[win_no-1].win_id = 2;
            fb_info.win_par[win_no-1].z_order = 2;
        }
        fb_info.win_par[win_no-1].area_par[0].ion_fd = handle->share_fd;
#if USE_HWC_FENCE
        if (context->isRk3399)
            fb_info.win_par[win_no-1].area_par[0].acq_fence_fd = fbLayer->acquireFenceFd;
        else
            fb_info.win_par[win_no-1].area_par[0].acq_fence_fd = -1;
#else
        fb_info.win_par[win_no-1].area_par[0].acq_fence_fd = -1;
#endif
        fb_info.win_par[win_no-1].area_par[0].x_offset = 0;
        fb_info.win_par[win_no-1].area_par[0].y_offset = offset/context->fbStride;
        fb_info.win_par[win_no-1].area_par[0].xpos = 0;
        fb_info.win_par[win_no-1].area_par[0].ypos = 0;
        fb_info.win_par[win_no-1].area_par[0].xsize = handle->width;
        fb_info.win_par[win_no-1].area_par[0].ysize = handle->height;
        fb_info.win_par[win_no-1].area_par[0].xact = handle->width;
        fb_info.win_par[win_no-1].area_par[0].yact = handle->height;
        fb_info.win_par[win_no-1].area_par[0].xvir = handle->stride;
        fb_info.win_par[win_no-1].area_par[0].yvir = handle->height;
        hfi->pRelFenceFd[pzone_mag->zone_cnt] = &(fbLayer->releaseFenceFd);
        if(comType == HWC_MIX_FPS) {
            fb_info.win_par[win_no-1].area_par[0].reserved0 = HWCRFPS;
        }
#if USE_HWC_FENCE
#if SYNC_IN_VIDEO
    if(context->mVideoMode && !context->mIsMediaView && !g_hdmi_mode)
        fb_info.wait_fs=1;
    else
#endif
        fb_info.wait_fs=0;
#endif
    }
    memcpy((void*)(&(hfi->fb_info)),(void*)&fb_info,sizeof(rk_fb_win_cfg_data));
    return 0;
}

static int hwc_pre_prepare(hwc_display_contents_1_t** displays, int flag)
{
    int forceStereop = 0;
    int forceStereoe = 0;
    hwcContext * contextp = _contextAnchor;
    hwcContext * contexte = _contextAnchor1;
    contextp->Is3D = false;
    if(contexte!=NULL){
        contexte->Is3D = false;
    }

    if(contextp->mLcdcNum == 2 || contextp->isRk3399 || contextp->isRk3366){
        int xres = contextp->dpyAttr[HWC_DISPLAY_PRIMARY].xres;
        int yres = contextp->dpyAttr[HWC_DISPLAY_PRIMARY].yres;
        int relxres = contextp->dpyAttr[HWC_DISPLAY_PRIMARY].relxres;
        int relyres = contextp->dpyAttr[HWC_DISPLAY_PRIMARY].relyres;
        if (xres != relxres || yres != relyres) {
            contextp->mResolutionChanged = true;
        } else {
            contextp->mResolutionChanged = false;
        }
    }

#if (defined(GPU_G6110) || defined(RK3288_BOX) || defined(RK3399_BOX))
#ifdef RK3288_BOX
    if(contextp->mLcdcNum == 2){
        return 0;
    }
#endif
#if USE_WM_SIZE
    contextp->mHdmiSI.hdmi_anm = 0;
    contextp->mHdmiSI.anroidSt = false;
#endif
#endif
    return 0;
}

static int hwc_try_policy(hwcContext * context,hwc_display_contents_1_t * list,int dpyID)
{
    int ret;
    for(int i = 0;i < HWC_POLICY_NUM;i++){
#if G6110_SUPPORT_FBDC
        //zxl: if in mix mode and it has fbdc layer before,then go into GPU compose.
        if (i > HWC_HOR && context->bFbdc)
        {
            return -1;
        }
#endif
        if (context->mIsLargeVideo && context->mIsMediaView)
            return -1;

        ret = context->fun_policy[i]((void*)context,list);
        if(!ret){
            break; // find the Policy
        }
    }
    return ret;
}

static int hwc_prepare_virtual(hwc_composer_device_1_t * dev, hwc_display_contents_1_t  *list)
{
	if (list==NULL)
	{
		return -1;
	}

    hwcContext * context = _contextAnchor;

    context->wfdRgaBlit = false;
	for (size_t j = 0; j <(list->numHwLayers - 1); j++)
	{
		struct private_handle_t * handle = (struct private_handle_t *)list->hwLayers[j].handle;

		if (handle && GPU_FORMAT==HAL_PIXEL_FORMAT_YCrCb_NV12_VIDEO)
		{
			ALOGV("WFD rga_video_copybit,%x,w=%d,h=%d",\
				GPU_BASE,GPU_WIDTH,GPU_HEIGHT);
			if (context->wfdOptimize==0)
			{
				rga_video_copybit(handle,0,0,0,handle->share_fd,RK_FORMAT_YCbCr_420_SP,0,HWCV,false);
			}
		}
	}
#if VIRTUAL_RGA_BLIT
    unsigned int  i ;
    bool mBlit = true;
    int  pixelSize  = 0;

    /*if wfdOptimize,than return*/
    if(context->wfdOptimize > 0)
        return 0;

    for (  i = 0; i < (list->numHwLayers - 1); i++)
    {
        hwc_layer_1_t * layer = &list->hwLayers[i];
        struct private_handle_t * handle = (struct private_handle_t *)layer->handle;
        if ((layer->flags & HWC_SKIP_LAYER) || (handle == NULL))
        {
            return 0;
        }
    }

    for (i = 0; i < (list->numHwLayers - 1); i++)
    {
        hwc_layer_1_t * layer = &list->hwLayers[i];
        struct private_handle_t * handle = (struct private_handle_t *)layer->handle;

        pixelSize += ((layer->sourceCrop.bottom - layer->sourceCrop.top) * \
                        (layer->sourceCrop.right - layer->sourceCrop.left));
        if(pixelSize > 4718592)  // pixel too large,RGA done use more time
        {
            mBlit = false;
            break;
        }
        layer->compositionType = HWC_BLITTER;
    }

    if(!mBlit)
    {
        for (i = 0; i < (list->numHwLayers - 1); i++)
        {
            hwc_layer_1_t * layer = &list->hwLayers[i];
            layer->compositionType = HWC_FRAMEBUFFER;
        }
    }else
        context->wfdRgaBlit = true;
#endif
	return 0;
}

static int hwc_prepare_screen(hwc_composer_device_1 *dev, hwc_display_contents_1_t *list, int dpyID)
{
    ATRACE_CALL();
    size_t i;
    size_t j;

    hwcContext * ctxp = _contextAnchor;
    hwcContext * context = _contextAnchor;
    if(dpyID == HWCE){
        context = _contextAnchor1;
    }

    int ret;
    int err;
    bool vertical = false;
    struct private_handle_t * handles[MAX_VIDEO_SOURCE];
    int index=0;
    int video_sources=0;
    int iVideoSources;
    int m,n;
    int vinfo_cnt = 0;
    int video_cnt = 0;
    int mix_index = dpyID;
    int transformcnt = 0;
    bool bIsMediaView=false;
    char gts_status[PROPERTY_VALUE_MAX];

    /* Check layer list. */
    if (list == NULL || (list->numHwLayers  == 0)/*||!(list->flags & HWC_GEOMETRY_CHANGED)*/){
        ALOGD_IF(log(HLLTWO),"dpyID=%d list null",dpyID);
        return 0;
    }

    if(is_gpu_or_nodraw(list,dpyID)){
        return 0;
    }

#if (defined(GPU_G6110) || defined(RK3288_BOX))
#if USE_WM_SIZE
    if(_contextAnchor->mHdmiSI.anroidSt){
        goto GpuComP;
    }
#endif
#endif

    LOGV("%s(%d):>>> hwc_prepare_primary %d layers <<<",
         __FUNCTION__,
         __LINE__,
         list->numHwLayers -1);

    if(ctxp->mBootCnt < BOOTCOUNT)
    {
        hwc_list_nodraw(list);
        return 0;
    }
#if GET_VPU_INTO_FROM_HEAD
    //init handles,reset bMatch
    for (i = 0; i < MAX_VIDEO_SOURCE; i++){
        handles[i]=NULL;
        context->video_info[i].bMatch=false;
    }
#endif

    for (i = 0; i < (list->numHwLayers - 1); i++){
        struct private_handle_t * handle = (struct private_handle_t *) list->hwLayers[i].handle;
        if(handle && GPU_FORMAT == HAL_PIXEL_FORMAT_YCrCb_NV12_VIDEO){
            video_cnt ++;
        }
#ifdef GPU_G6110
        if(handle && handle->share_fd != handle->fd[0]) {
            handle->share_fd = handle->fd[0];
        }
#endif

        if ( handle && !(is_hal_format_supported_by_vop(GPU_FORMAT) ) )
        {
            ALOGD_IF(log(HLLFOU),"preset gles_composition for vop unsupported hal_format 0x%x.", GPU_FORMAT);
            goto GpuComP;
        }
    }
    context->mVideoMode = false;
    context->mVideoRotate = false;
    context->mIsMediaView = false;
    context->mHdmiSI.mix_up = false;
    context->mHdmiSI.mix_vh = false;
    context->mNeedRgaTransform = false;
    context->mNV12_VIDEO_VideoMode = false;
    context->mHasYuvTenBit = false;
    context->mSecureLayer = 0;

#if G6110_SUPPORT_FBDC
    context->bFbdc = false;
#endif
#if OPTIMIZATION_FOR_DIMLAYER
    context->bHasDimLayer = false;
#endif
    context->mtrsformcnt  = 0;
    for (i = 0; i < (list->numHwLayers - 1); i++){
        struct private_handle_t * handle = (struct private_handle_t *) list->hwLayers[i].handle;

        if(list->hwLayers[i].transform != 0){
            context->mtrsformcnt ++;
        }

        if(handle && GPU_FORMAT == HAL_PIXEL_FORMAT_YCrCb_NV12){
            context->mVideoMode = true;
        }

        if(handle && GPU_FORMAT == HAL_PIXEL_FORMAT_YCrCb_NV12_VIDEO){
            tVPU_FRAME vpu_hd;

            context->mVideoMode = true;
            context->mNV12_VIDEO_VideoMode = true;

            ALOGV("video");
#if GET_VPU_INTO_FROM_HEAD
            for(m=0;m<MAX_VIDEO_SOURCE;m++){
                ALOGV("m=%d,[%p,%p],[%p,%p]",m,context->video_info[m].video_hd,handle,
                    context->video_info[m].video_base,(void*)handle->base);
                if( (context->video_info[m].video_hd == handle)&& handle->video_width != 0
                    && (context->video_info[m].video_base == (void*)handle->base)){
                    //match video,but handle info been update
                    context->video_info[m].bMatch=true;
                    break;
                }

            }

            //if can't find any match video in back video source,then update handle
            if(m == MAX_VIDEO_SOURCE )
#endif
            {
#if GET_VPU_INTO_FROM_HEAD
                memcpy(&vpu_hd,(void*)(GPU_BASE),sizeof(tVPU_FRAME));
#else
#if defined(__arm64__) || defined(__aarch64__)
                memcpy(&vpu_hd,(void*)((unsigned long)GPU_BASE+2*handle->stride*handle->height),sizeof(tVPU_FRAME));
#else
                memcpy(&vpu_hd,(void*)((unsigned int)GPU_BASE+2*handle->stride*handle->height),sizeof(tVPU_FRAME));
#endif
#endif
                //if find invalid params,then increase iVideoSources and try again.
                if(vpu_hd.FrameWidth>8192 || vpu_hd.FrameWidth <=0 || \
                    vpu_hd.FrameHeight>8192 || vpu_hd.FrameHeight<=0){
                    ALOGE("invalid video(w=%d,h=%d)",vpu_hd.FrameWidth,vpu_hd.FrameHeight);
                }

                handle->video_addr = vpu_hd.FrameBusAddr[0];
                handle->video_width = vpu_hd.FrameWidth;
                handle->video_height = vpu_hd.FrameHeight;
                handle->video_disp_width = vpu_hd.DisplayWidth;
                handle->video_disp_height = vpu_hd.DisplayHeight;

#if WRITE_VPU_FRAME_DATA
                if(hwc_get_int_property("sys.hwc.write_vpu_frame_data","0")){
                    static FILE* pOutFile = NULL;
                    VPUMemLink(&vpu_hd.vpumem);
                    pOutFile = fopen("/data/raw.yuv", "wb");
                    if (pOutFile) {
                        ALOGE("pOutFile open ok!");
                    } else {
                        ALOGE("pOutFile open fail!");
                    }
                    fwrite(vpu_hd.vpumem.vir_addr,1, vpu_hd.FrameWidth*vpu_hd.FrameHeight*3/2, pOutFile);
                 }
#endif

#if GET_VPU_INTO_FROM_HEAD
                //record handle in handles
                handles[index]=handle;
                index++;
#endif
                ALOGV("prepare [%x,%dx%d] active[%d,%d]",handle->video_addr,handle->video_width,\
                    handle->video_height,vpu_hd.DisplayWidth,vpu_hd.DisplayHeight);

                context->video_fmt = vpu_hd.OutputWidth;
                if(context->video_fmt !=HAL_PIXEL_FORMAT_YCrCb_NV12
                    && context->video_fmt !=HAL_PIXEL_FORMAT_YCrCb_NV12_10)
                    context->video_fmt = HAL_PIXEL_FORMAT_YCrCb_NV12;   // Compatible old sf lib
                ALOGV("context->video_fmt =%d",context->video_fmt);
            }
        }
    }

#if GET_VPU_INTO_FROM_HEAD
    for (i = 0; i < index; i++){
        struct private_handle_t * handle = handles[i];
        if(handle == NULL)
            continue;

        for(m=0;m<MAX_VIDEO_SOURCE;m++){
            //save handle into video_info which doesn't match before.
            if(!context->video_info[m].bMatch){
                ALOGV("save handle=%p,base=%p,w=%d,h=%d",handle,GPU_BASE,handle->video_width,handle->video_height);
                context->video_info[m].video_hd = handle ;
                context->video_info[m].video_base = (void*)(GPU_BASE);
                context->video_info[m].bMatch=true;
                vinfo_cnt++;
                break;
            }
         }
    }

    for(m=0;m<MAX_VIDEO_SOURCE;m++){
        //clear handle into video_info which doesn't match before.
        ALOGV("cancel m=%d,handle=%p,base=%p",m,context->video_info[m].video_hd,context->video_info[m].video_base);
        if(!context->video_info[m].bMatch)
        {
            context->video_info[m].video_hd = NULL;
            context->video_info[m].video_base = NULL;
        }
    }
#endif

    if(video_cnt >1){
        // more videos goto gpu cmp
        ALOGW("more2 video=%d goto GpuComP",video_cnt);
		ALOGD_IF(log(HLLFOU),"Policy out [%d][%s]",__LINE__,__FUNCTION__);
        goto GpuComP;
    }
    //Get gts staus,save in context->mGtsStatus
    hwc_get_string_property("sys.cts_gts.status","false",gts_status);
    if(!strcmp(gts_status,"true")){
        context->mGtsStatus = true;
    }else{
        context->mGtsStatus = false;
    }

    //context->mTrsfrmbyrga = false;
    /* Check all layers: tag with different compositionType. */
    for (i = 0; i < (list->numHwLayers - 1); i++){
        hwc_layer_1_t * layer = &list->hwLayers[i];
        if(layer->transform){
            transformcnt ++;
        }
        uint32_t compositionType =
             check_layer(context, list->numHwLayers - 1, i,context->mVideoMode, layer);

        if (compositionType == HWC_FRAMEBUFFER){
            break;
        }
    }

    if(hwc_get_int_property("sys.hwc.disable","0")== 1){
		ALOGD_IF(log(HLLFOU),"Policy out [%d][%s]",__LINE__,__FUNCTION__);
        goto GpuComP;
	}
    /* Roll back to FRAMEBUFFER if any layer can not be handled. */
    if (i != (list->numHwLayers - 1) || (list->numHwLayers==1) /*|| context->mtrsformcnt > 1*/){
		ALOGD_IF(log(HLLFOU),"Policy out [%d][%s]",__LINE__,__FUNCTION__);
        goto GpuComP;
    }

#if !ENABLE_LCDC_IN_NV12_TRANSFORM
    if(!context->mGtsStatus){
        if(context->mVideoMode &&  !context->mNV12_VIDEO_VideoMode && context->mtrsformcnt>0){
            ALOGV("Go into GLES,in nv12 transform case");
			ALOGD_IF(log(HLLFOU),"Policy out [%d][%s]",__LINE__,__FUNCTION__);
            goto GpuComP;
        }
    }
#endif

    for (i = 0; i < (list->numHwLayers - 1); i++){
        struct private_handle_t * handle = (struct private_handle_t *) list->hwLayers[i].handle;
        int stride_gr;
        int video_w=0,video_h=0;

        if (handle && GPU_FORMAT == HAL_PIXEL_FORMAT_YCrCb_NV12_VIDEO) {
            video_w = handle->video_width;
            video_h = handle->video_height;
        } else if (handle && GPU_FORMAT == HAL_PIXEL_FORMAT_YCrCb_NV12) {
            video_w = handle->width;
            video_h = handle->height;
        }

        //alloc video gralloc buffer in video mode
        if (context->fd_video_bk[0] == -1 && context->mTrsfrmbyrga) {
            ALOGD_IF(log(HLLFOU),"mNV12_VIDEO_VideoMode=%d,mTrsfrmbyrga=%d,w=%d,h=%d",
                context->mNV12_VIDEO_VideoMode,context->mTrsfrmbyrga,video_w,video_h);
            for(j=0;j<MaxVideoBackBuffers;j++){
                err = context->mAllocDev->alloc(context->mAllocDev, RWIDTH,RHEIGHT,HAL_PIXEL_FORMAT_YCrCb_NV12, \
                    GRALLOC_USAGE_HW_COMPOSER|GRALLOC_USAGE_HW_RENDER|GRALLOC_USAGE_HW_VIDEO_ENCODER, \
                    (buffer_handle_t*)(&(context->pbvideo_bk[j])),&stride_gr);
                if(!err){
                    struct private_handle_t*handle = (struct private_handle_t*)context->pbvideo_bk[j];
                    context->fd_video_bk[j] = handle->share_fd;
#if defined(__arm64__) || defined(__aarch64__)
                    context->base_video_bk[j]= (long)(GPU_BASE);
#else
                    context->base_video_bk[j]= (int)(GPU_BASE);
#endif
                    context->relFenceFd[j] = -1;
                    ALOGD_IF(log(HLLTWO),"video alloc fd [%dx%d,f=%d],fd=%d,%p",
                        handle->width,handle->height,handle->format,handle->share_fd,context->base_video_bk[j]);

                }else {
                    ALOGE("video alloc faild video(w=%d,h=%d,format=0x%x,error=%s)",handle->video_width,
                        handle->video_height,context->fbhandle.format,strerror(errno));
					ALOGD_IF(log(HLLFOU),"Policy out [%d][%s]",__LINE__,__FUNCTION__);
					for(size_t k=0;k<j;k++){
					    if(context->fd_video_bk[k] != -1){
                            err = context->mAllocDev->free(context->mAllocDev,context->pbvideo_bk[k]);
                            if(err){
                                ALOGW("free back buff error %s,%d,%d",strerror(errno),j,k);
                            }
                            context->fd_video_bk[k] = -1;
                        }
					}
					context->fd_video_bk[j] != -1;
					goto GpuComP;
                }
            }
        }
    }


    // free video gralloc buffer in ui mode
    if(context->fd_video_bk[0] > 0 &&
        (/*!context->mVideoMode*/(
#if USE_VIDEO_BACK_BUFFERS
        !context->mNV12_VIDEO_VideoMode &&
#endif
        !context->mTrsfrmbyrga) || (video_cnt >1))){
        err = 0;
        for(i=0;i<MaxVideoBackBuffers;i++){
            ALOGD_IF(log(HLLTWO),"dpyID=%d,free video fd=%d,base=%p,%p",
                dpyID,context->fd_video_bk[i],context->base_video_bk[i],context->pbvideo_bk[i]);
            if(context->pbvideo_bk[i] != NULL)
                err = context->mAllocDev->free(context->mAllocDev, context->pbvideo_bk[i]);
            if(!err){
                context->fd_video_bk[i] = -1;
                context->base_video_bk[i] = 0;
                context->pbvideo_bk[i] = NULL;
            }
            if(context->relFenceFd[i] > 0) {
                close(context->relFenceFd[i]);
                context->relFenceFd[i] = -1;
            }
            ALOGW_IF(err, "free(...) failed %d (%s)", err, strerror(-err));
        }
    }

    //G6110 FBDC: only let video case continue.
#if 0
    if(!context->mVideoMode){
        goto GpuComP;
    }
#endif

    ret = collect_all_zones(context,list);

    //if (context->mHasYuvTenBit)
    //    property_set("sys.hwc.log", "511");
    //else
    //    property_set("sys.hwc.log", "0");

    hwc_sprite_replace(context,list);
    if(ret !=0 ){
		ALOGD_IF(log(HLLFOU),"Policy out [%d][%s]",__LINE__,__FUNCTION__);
        goto GpuComP;
    }

    //if(vertical == true)
    ret = hwc_try_policy(context,list,dpyID);
    if(list->hwLayers[context->mRgaTBI.index].compositionType == HWC_FRAMEBUFFER) {
        context->mNeedRgaTransform = false;
    } else if(context->mNeedRgaTransform) {
        int ret = 0;
        int w_valid = context->mRgaTBI.w_valid;
        int h_valid = context->mRgaTBI.h_valid;
        int layer_fd = context->mRgaTBI.layer_fd;
        int lastfd = context->mRgaTBI.transform;
        uint32_t transform = context->mRgaTBI.transform;
        bool trsfrmbyrga = context->mRgaTBI.trsfrmbyrga;
        struct private_handle_t* hdl = context->mRgaTBI.hdl;
        int Dstfmt = trsfrmbyrga ? hwChangeRgaFormat(hdl->format) : RK_FORMAT_YCbCr_420_SP;
        if (context->mRgaTBI.type == 1) {
            Dstfmt = RK_FORMAT_YCbCr_420_SP;
            ret = rgaRotateScale(context,transform,layer_fd,Dstfmt,true);
        } else if (context->mRgaTBI.type == 0)
            ret = rga_video_copybit(hdl,transform,w_valid,h_valid,layer_fd,Dstfmt,
                                                            trsfrmbyrga,dpyID,true);
        if (ret) {
            ALOGD_IF(log(HLLONE),"T:RGA copyblit fail");
            context->mRgaTBI.lastfd = 0;
            int index_v = context->mCurVideoIndex % MaxVideoBackBuffers;
            if (context->relFenceFd[index_v] > 0)
                close(context->relFenceFd[index_v]);
            context->relFenceFd[index_v] = -1;
            goto GpuComP;
        }
    }
    if(ret !=0 ){
		ALOGD_IF(log(HLLFOU),"Policy out [%d][%s]",__LINE__,__FUNCTION__);
        goto GpuComP;
    }
    if(context->zone_manager.composter_mode != HWC_MIX) {
        for(i = 0;i<GPUDRAWCNT;i++){
            gmixinfo[mix_index].gpu_draw_fd[i] = 0;
        }
    }

#if G6110_SUPPORT_FBDC
    if(check_zone(context) && dpyID == 0){
        ALOGD_IF(log(HLLFOU),"Policy out [%d][%s]",__LINE__,__FUNCTION__);
        goto GpuComP;
    }
#endif

    //before composition:do overlay no error???
    struct hwc_fb_info hfi;
    if(context->zone_manager.composter_mode == HWC_LCDC){
        err = hwc_collect_cfg(context,list,&hfi,0,true);
    }else if(context->zone_manager.composter_mode == HWC_MIX){
        err = hwc_collect_cfg(context,list,&hfi,1,true);
    }else if(context->zone_manager.composter_mode == HWC_MIX_V2){
        err = hwc_collect_cfg(context,list,&hfi,2,true);
    }
    if(err){
        ALOGD_IF(log(HLLFOU),"Policy out [%d][%s]",__LINE__,__FUNCTION__);
        goto GpuComP;
    }else{
        if(!hwc_check_cfg(context,hfi.fb_info)){
            ALOGD_IF(log(HLLFOU),"Policy out [%d][%s]",__LINE__,__FUNCTION__);
            dump_config_info(hfi.fb_info,context,3);
            goto GpuComP;
        }
    }
#if (defined(RK3368_BOX) || defined(RK3288_BOX))
#ifdef RK3288_BOX
	if(_contextAnchor->mLcdcNum == 1)
#endif
    {
        if(!hwcPrimaryToExternalCheckConfig(context,hfi.fb_info)){
            ALOGD_IF(log(HLLONE),"Policy out [%d][%s]",__LINE__,__FUNCTION__);
            goto GpuComP;
        }
    }
#endif

    if (context->zone_manager.mCmpType == HWC_RGA_VOP_GPU) {
        if (hwc_alloc_rga_blit_buffers(context))
            goto GpuComP;
    } else if (context->mLastCompType == HWC_RGA_VOP_GPU)
        hwc_free_rga_blit_buffers(context);

    context->mLastCompType = context->zone_manager.mCmpType;

    if (context->mOneWinOpt && !context->mHasYuvTenBit && !context->mSecureLayer)
        goto GpuComP;

    return 0;
GpuComP   :
    for (i = 0; i < (list->numHwLayers - 1); i++) {
        hwc_layer_1_t * layer = &list->hwLayers[i];

        layer->compositionType = HWC_FRAMEBUFFER;
    }
    for(i = 0; i < GPUDRAWCNT; i++) {
        gmixinfo[mix_index].gpu_draw_fd[i] = 0;
    }
    context->mNeedRgaTransform = false;
    for (j = 0; j <(list->numHwLayers - 1); j++) {
        struct private_handle_t * handle = (struct private_handle_t *)list->hwLayers[j].handle;

        list->hwLayers[j].compositionType = HWC_FRAMEBUFFER;

        if (handle && GPU_FORMAT==HAL_PIXEL_FORMAT_YCrCb_NV12_VIDEO) {
            ALOGV("rga_video_copybit,handle=%x,base=%x,w=%d,h=%d,video(w=%d,h=%d)",\
                  handle,GPU_BASE,GPU_WIDTH,GPU_HEIGHT,handle->video_width,handle->video_height);
            rga_video_copybit(handle,0,0,0,handle->share_fd,RK_FORMAT_YCbCr_420_SP,0,dpyID,false);
        }
        if (handle && GPU_FORMAT==HAL_PIXEL_FORMAT_YCrCb_NV12_10) {
            list->hwLayers[j].compositionType = HWC_NODRAW;
        }
    }
    context->zone_manager.composter_mode = HWC_FRAMEBUFFER;
    context->mLastCompType = -1;
    context->mOneWinOpt = false;
    return 0;

}

int
hwc_prepare(
    hwc_composer_device_1_t * dev,
    size_t numDisplays,
    hwc_display_contents_1_t** displays
    )
{
    ATRACE_CALL();
    hwcContext * context = _contextAnchor;
    int ret = 0;
    size_t i;
    hwc_display_contents_1_t* list = displays[0];  // ignore displays beyond the first

    /* Check device handle. */
    if (context == NULL|| &context->device.common != (hw_device_t *)dev){
        LOGE("%s(%d):Invalid device!", __FUNCTION__, __LINE__);
        return HWC_EGL_ERROR;
    }

    init_log_level();
    hwc_pre_prepare(displays,0);

#if hwcDumpSurface
    _DumpSurface(list);
#endif
    void* zone_m = (void *)&context->zone_manager;
    memset(zone_m,0,sizeof(ZoneManager));
    context->zone_manager.composter_mode = HWC_FRAMEBUFFER;

    if(_contextAnchor1 != NULL){
        zone_m = (void *)&_contextAnchor1->zone_manager;
        memset(zone_m,0,sizeof(ZoneManager));
        _contextAnchor1->zone_manager.composter_mode = HWC_FRAMEBUFFER;
    }

    /* Roll back to FRAMEBUFFER if any layer can not be handled. */
    if(hwc_get_int_property("sys.hwc.compose_policy","0") <= 0 ){
        for (i = 0; i < (list->numHwLayers - 1); i++){
            list->hwLayers[i].compositionType = HWC_FRAMEBUFFER;
        }
        hwc_display_contents_1_t* list_e = displays[1];
        if(list_e){
            for (i = 0; i < (list_e->numHwLayers - 1); i++){
                list_e->hwLayers[i].compositionType = HWC_FRAMEBUFFER;
            }
        }
#if (defined(GPU_G6110) || defined(RK3288_BOX))
#ifdef RK3288_BOX
        if(context->mLcdcNum == 2){
            return 0;
        }
#endif
        if(!hdmi_noready && getHdmiMode() == 1){
            for (unsigned int i = 0; i < (list->numHwLayers - 1); i++){
                hwc_layer_1_t * layer = &list->hwLayers[i];
                layer->compositionType = HWC_NODRAW;
            }
        }
#endif
        ALOGD_IF(log(HLLFOU),"Policy out [%d][%s]",__LINE__,__FUNCTION__);
        return 0;
    }

#if hwcDEBUG
    if(log(HLLEIG)){
        LOGD("%s(%d):Layers to set:", __FUNCTION__, __LINE__);
        _Dump(list);
    }
#endif

    for (size_t i = 0; i < numDisplays; i++) {
        hwc_display_contents_1_t *list = displays[i];
        switch(i) {
            case HWC_DISPLAY_PRIMARY:
            case HWC_DISPLAY_EXTERNAL:
                ret = hwc_prepare_screen(dev, list, i);
                break;
            case HWC_DISPLAY_VIRTUAL:
                if(list) {
                    ret = hwc_prepare_virtual(dev, list);
                }
                break;
            default:
                ret = -EINVAL;
        }
    }
    return ret;
}

int hwc_blank(struct hwc_composer_device_1 *dev, int dpy, int blank)
{
    // We're using an older method of screen blanking based on
    // early_suspend in the kernel.  No need to do anything here.
    ATRACE_CALL();
    ALOGI("hwc_blank dpy[%d],blank[%d]",dpy,blank);
    // return 0;
    switch (dpy) {
    case HWC_DISPLAY_PRIMARY: {
        int fb_blank = blank ? FB_BLANK_POWERDOWN : FB_BLANK_UNBLANK;
        int err = ioctl(_contextAnchor->fbFd, FBIOBLANK, fb_blank);
        if (err < 0) {
            if (errno == EBUSY)
                ALOGD("%sblank ioctl failed (display already %sblanked)",
                        blank ? "" : "un", blank ? "" : "un");
            else
                ALOGE("%sblank ioctl failed: %s", blank ? "" : "un",
                        strerror(errno));
            return -errno;
        }
        else
        {
            _contextAnchor->fb_blanked = blank;
        }
        break;
    }

    case HWC_DISPLAY_EXTERNAL:{
#if HWC_EXTERNAL
		if(blank == 0)
		    hdmi_noready = false;
		_contextAnchor1->fb_blanked = blank;
        int fb_blank = blank ? FB_BLANK_POWERDOWN : FB_BLANK_UNBLANK;
#endif
        break;
    }

    default:
        return -EINVAL;

    }

    return 0;
}

int hwc_query(struct hwc_composer_device_1* dev,int what, int* value)
{

    hwcContext * context = _contextAnchor;

    switch (what) {
    case HWC_BACKGROUND_LAYER_SUPPORTED:
        // we support the background layer
        value[0] = 1;
        break;
    case HWC_VSYNC_PERIOD:
        // vsync period in nanosecond
        value[0] = 1e9 / context->fb_fps;
        break;
    default:
        // unsupported query
        return -EINVAL;
    }
    return 0;
}

static int display_commit( int dpy)
{
    return 0;
}

static int hwc_Post( hwcContext * context,hwc_display_contents_1_t* list)
{
    ATRACE_CALL();
    hwcContext * ctxp = _contextAnchor;

    int dpyID = 0;
    int winID = 2;

    if (list == NULL) {
        return -1;
    }

    if (context!=_contextAnchor)
        dpyID = 1;

    if(!is_need_post(list,dpyID,1)) {
        return -1;
    }

    if (context->isBox && !dpyID && !context->isRk3399)
        winID = 0;

    if (context->mResolutionChanged)
	    winID = 0;

    if (context->isVr)
	    winID = 0;

    if (context->mIsMipiDualOutMode && !context->mComVop)
        winID = 0;

    if (ctxp->mHdmiSI.NeedReDst && dpyID)
        winID = 0;

    if (context->isRk3368 && context->isMid && dpyID == 0 &&
                (ctxp->mHdmiSI.CvbsOn || ctxp->mHdmiSI.HdmiOn))
        winID = 0;

    //if (context->fbFd>0 && !context->fb_blanked)
#if defined(RK3288_MID)
    if(dpyID == 0 || (dpyID == 1 && !context->fb_blanked))
#else
#ifdef RK3288_BOX
    int lcdcNum = _contextAnchor->mLcdcNum;
    bool isPost = dpyID == 0 || (dpyID == 1 && !context->fb_blanked);
    if(lcdcNum==1 || (lcdcNum==2 && isPost))
#else
    if(true)
#endif
#endif
    {
        struct fb_var_screeninfo info;
        struct rk_fb_win_cfg_data fb_info;
        memset(&fb_info,0,sizeof(fb_info));
        fb_info.ret_fence_fd = -1;
        for(int i=0;i<RK_MAX_BUF_NUM;i++) {
            fb_info.rel_fence_fd[i] = -1;
        }
        int numLayers = list->numHwLayers;
        hwc_layer_1_t *fbLayer = &list->hwLayers[numLayers - 1];
        if (!fbLayer)
        {
            ALOGE("fbLayer=NULL");
            return -1;
        }
        info = context->info;
        struct private_handle_t*  handle = (struct private_handle_t*)fbLayer->handle;
        if (!handle)
        {
			ALOGE("hanndle=NULL at line %d",__LINE__);
            return -1;
        }

        ALOGV("hwc_primary_Post num=%d,ion=%d",numLayers,handle->share_fd);

        unsigned int offset = handle->offset;
#if G6110_SUPPORT_FBDC
        //fix splash bug when reboot system.
        if(numLayers == 1)
            fb_info.win_par[0].area_par[0].data_format = HAL_PIXEL_FORMAT_RGBA_8888;
        else
#endif
            fb_info.win_par[0].area_par[0].data_format = context->fbhandle.format;
        fb_info.win_par[0].win_id = winID;
        fb_info.win_par[0].z_order = 0;
        fb_info.win_par[0].area_par[0].ion_fd = handle->share_fd;
#if USE_HWC_FENCE
        if (context->isRk3399)
            fb_info.win_par[0].area_par[0].acq_fence_fd = fbLayer->acquireFenceFd;
        else
            fb_info.win_par[0].area_par[0].acq_fence_fd = -1;
#else
        fb_info.win_par[0].area_par[0].acq_fence_fd = -1;
#endif
        fb_info.win_par[0].area_par[0].x_offset = 0;
        fb_info.win_par[0].area_par[0].y_offset = offset/context->fbStride;
        fb_info.win_par[0].area_par[0].xpos = 0;
        fb_info.win_par[0].area_par[0].ypos = 0;
        fb_info.win_par[0].area_par[0].xsize = handle->width;
        fb_info.win_par[0].area_par[0].ysize = handle->height;
        fb_info.win_par[0].area_par[0].xact = handle->width;
        fb_info.win_par[0].area_par[0].yact = handle->height;
        fb_info.win_par[0].area_par[0].xvir = handle->stride;
        fb_info.win_par[0].area_par[0].yvir = handle->height;
#if USE_HWC_FENCE
#if SYNC_IN_VIDEO
    if(context->mVideoMode && !context->mIsMediaView && !g_hdmi_mode)
        fb_info.wait_fs=1;
    else
#endif
	    fb_info.wait_fs=0;
#endif
        if(context == _contextAnchor1){
            if(_contextAnchor->mHdmiSI.NeedReDst){
                if(hotplug_reset_dstposition(&fb_info,0)){
                    ALOGW("reset_dst fail [%d]",__LINE__);
                }
            }
         }else{
#if (defined(GPU_G6110) || defined(RK3288_BOX))
#ifdef RK3288_BOX
            if(_contextAnchor->mLcdcNum==1)
#endif
#ifdef RK3368_MID
            if(context->mHdmiSI.CvbsOn || context->mHdmiSI.HdmiOn)
#endif
            {
                if(hotplug_reset_dstposition(&fb_info,1)){
                    ALOGW("reset_dst fail [%d]",__LINE__);
                }
            }
#endif
        }
#ifdef RK3288_BOX
        if(context==_contextAnchor && context->mResolutionChanged && context->mLcdcNum==2){
            hotplug_reset_dstposition(&fb_info,2);
        }
#endif

        if (is_primary_and_resolution_changed(context)) {
            hotplug_reset_dstposition(&fb_info,2);
        }

#if DUAL_VIEW_MODE
        if(context != _contextAnchor && context->mIsDualViewMode) {
            dual_view_vop_config(&fb_info);
            goto UseFence;
        }
#endif

        if (context->mIsMipiDualOutMode)
            mipi_dual_vop_config(context, &fb_info);

#ifdef USE_AFBC_LAYER
        uint64_t internal_format = handle->internal_format;
        D("internal_format of fb_target_layer : 0x%llx.", internal_format);

        if ( isAfbcInternalFormat(internal_format) )
        {
            uint64_t index_of_arm_hal_format = internal_format & GRALLOC_ARM_INTFMT_FMT_MASK;
            struct rk_fb_area_par* pAreaPar = &(fb_info.win_par[0].area_par[0] );

            switch (  index_of_arm_hal_format )
            {
                case GRALLOC_ARM_HAL_FORMAT_INDEXED_RGBA_8888:
                    D("to set afbc_format for rgba_8888.");
                    // pAreaPar->fbdc_data_format =  FBDC_ARGB_888;
                    pAreaPar->fbdc_data_format = 0x27;
                    pAreaPar->fbdc_en = 1;
                    pAreaPar->fbdc_cor_en = 0;

                    pAreaPar->data_format = 0x27;
                    break;

                case GRALLOC_ARM_HAL_FORMAT_INDEXED_RGBX_8888:
                    D("to set afbc_format for rgbx_8888.");
                    // pAreaPar->fbdc_data_format =  FBDC_ARGB_888;
                    pAreaPar->fbdc_data_format = 0x27;
                    pAreaPar->fbdc_en = 1;
                    pAreaPar->fbdc_cor_en = 0;

                    pAreaPar->data_format = 0x27;
                    break;
                case GRALLOC_ARM_HAL_FORMAT_INDEXED_BGRA_8888:
                    D("to set afbc_format for bgra_8888.");
                    // pAreaPar->fbdc_data_format =  FBDC_ARGB_888;
                    pAreaPar->fbdc_data_format = 0x27;
                    pAreaPar->fbdc_en = 1;
                    pAreaPar->fbdc_cor_en = 0;

                    pAreaPar->data_format = 0x27;
                    break;

                case GRALLOC_ARM_HAL_FORMAT_INDEXED_RGB_888:
                    D("to set afbc_format for rgb_888.");
                    pAreaPar->fbdc_data_format = 0x28;
                    pAreaPar->fbdc_en = 1;
                    pAreaPar->fbdc_cor_en = 0;

                    pAreaPar->data_format = 0x28;
                    break;

                case GRALLOC_ARM_HAL_FORMAT_INDEXED_RGB_565:
                    D("to set afbc_format for rgb_565.");
                    pAreaPar->fbdc_data_format = 0x26;
                    pAreaPar->fbdc_en = 1;
                    pAreaPar->fbdc_cor_en = 0;

                    pAreaPar->data_format = 0x26;
                    break;

                default:
                    E("unsupported index_of_arm_hal_format : 0x%llx.", index_of_arm_hal_format);
                    break;
            }
        }
#endif

        if(ioctl(context->fbFd, RK_FBIOSET_CONFIG_DONE, &fb_info)){
            ALOGE("ID=%d:ioctl fail:%s",context!=_contextAnchor,strerror(errno));
            dump_config_info(fb_info,context,3);
        }else{
#if ONLY_USE_ONE_VOP
#ifdef RK3288_BOX
            if(_contextAnchor->mLcdcNum == 1)
#endif
            {
                memcpy(&_contextAnchor->fb_info,&fb_info,sizeof(rk_fb_win_cfg_data));
            }
#endif
            ALOGD_IF(log(HLLONE),"ID=%d:",context!=_contextAnchor);
        }
        dump_config_info(fb_info,context,2);
#if DUAL_VIEW_MODE
UseFence:
#endif
#if USE_HWC_FENCE
        for(int k=0;k<RK_MAX_BUF_NUM;k++)
        {
            if (context->mIsMipiDualOutMode) {
                char fname[15] = "hwc_fb_target";
                if(fb_info.rel_fence_fd[k] >= 0) {
                    fbLayer->releaseFenceFd = fence_merge(fname,
                        fbLayer->releaseFenceFd, fb_info.rel_fence_fd[k]);
                }
            } else {
                if(fb_info.rel_fence_fd[k] >= 0) {
                    fbLayer->releaseFenceFd = fb_info.rel_fence_fd[k];
                }
            }
        }
		if(fb_info.ret_fence_fd >= 0)
        	list->retireFenceFd = fb_info.ret_fence_fd;
#else
        for(int k=0;k<RK_MAX_BUF_NUM;k++)
        {
            if(fb_info.rel_fence_fd[k] >=0 )
                close(fb_info.rel_fence_fd[k]);
        }
        fbLayer->releaseFenceFd=-1;

        if(fb_info.ret_fence_fd >= 0)
        {
            close(fb_info.ret_fence_fd);
        }
        list->retireFenceFd=-1;
#endif
    }
    return 0;
}

static int hwc_set_lcdc(hwcContext * context, hwc_display_contents_1_t *list,int mix_flag)
{
    ATRACE_CALL();
    buffer_handle_t handle = 0;
    struct rk_fb_win_cfg_data fbwb;
    struct hwc_fb_info hfi;
    android::String8 result;

    int fd1 = -1;
    int fd2 = -1;
    int dpyID = 0;
    unsigned int j = 0;
    if(context == _contextAnchor1) {
        dpyID = 1;
    }
    if(!is_need_post(list,dpyID,2)) {
        return -1;
    }

    //struct rk_fb_win_cfg_data fb_info;
    int comType = context->zone_manager.mCmpType;
    hwc_collect_cfg(context,list,&hfi,mix_flag,false);

    //if(!context->fb_blanked)
    if(true) {
    //This will lead nenamark fps go down in rk3368 and will error for 3366
#if (!defined(GPU_G6110) && !defined(TARGET_BOARD_PLATFORM_RK3366) && !defined(TARGET_BOARD_PLATFORM_RK3399))
        if(context != _contextAnchor1) {
            hwc_display_t dpy = NULL;
            hwc_surface_t surf = NULL;
            dpy = eglGetCurrentDisplay();
            surf = eglGetCurrentSurface(EGL_DRAW);
            eglSwapBuffers((EGLDisplay) dpy, (EGLSurface) surf);
        }
#endif
        if(context == _contextAnchor1) {
            if(_contextAnchor->mHdmiSI.NeedReDst) {
                if(hotplug_reset_dstposition(&(hfi.fb_info),0)) {
                    ALOGW("reset_dst fail [%d]",__LINE__);
                }
            }
        } else {
#if (defined(GPU_G6110) || defined(RK3288_BOX))
#ifdef RK3288_BOX
            if(_contextAnchor->mLcdcNum==1)
#endif
#if RK3368_MID
            if(context->mHdmiSI.CvbsOn || context->mHdmiSI.HdmiOn)
#endif
            {
                if(hotplug_reset_dstposition(&(hfi.fb_info),1)){
                    ALOGW("reset_dst fail [%d]",__LINE__);
                }
            }
#endif
        }

#ifdef RK3288_BOX
        if(context==_contextAnchor && context->mResolutionChanged && context->mLcdcNum==2) {
            hotplug_reset_dstposition(&(hfi.fb_info),2);
        }
#endif
        if (is_primary_and_resolution_changed(context)) {
            hotplug_reset_dstposition(&(hfi.fb_info),2);
        }

        if (context->mLastCompType == HWC_RGA_VOP_GPU) {
            hwc_add_rga_blit_fbinfo(context, &hfi);
        }

#if DUAL_VIEW_MODE
        if(context != _contextAnchor && context->mIsDualViewMode) {
            dual_view_vop_config(&hfi.fb_info);
            goto UseFence;
        }
#endif

        if (context->mIsMipiDualOutMode)
            mipi_dual_vop_config(context, &hfi.fb_info);

        if(ioctl(context->fbFd, RK_FBIOSET_CONFIG_DONE, &(hfi.fb_info))) {
            ALOGE("ID=%d:ioctl fail:%s",dpyID,strerror(errno));
            dump_config_info(hfi.fb_info,context,3);
        } else {
            ALOGD_IF(log(HLLONE),"ID=%d:",dpyID);
        }
        if (mix_flag) {
            dump_config_info(hfi.fb_info,context,1);
        } else {
            dump_config_info(hfi.fb_info,context,0);
        }
#if DUAL_VIEW_MODE
UseFence:
#endif
#if USE_HWC_FENCE
        result.appendFormat("rel_fence_fd:");
        for(unsigned int i=0;i<RK_MAX_BUF_NUM;i++) {
            result.appendFormat("fd" "[%d]=%d ", i, hfi.fb_info.rel_fence_fd[i]);
            if(hfi.fb_info.rel_fence_fd[i] >= 0) {
                if(hfi.pRelFenceFd[i]) {
                    if(*(hfi.pRelFenceFd[i]) == 0) {
                        *(hfi.pRelFenceFd[i]) = -1;
                        ALOGW("bug:%s,%d,prff can not be 0",__func__,__LINE__);
                    }
                    fd1 = *(hfi.pRelFenceFd[i]);
                    fd2 = hfi.fb_info.rel_fence_fd[i];
                    if(fd1 < 0)
                        *(hfi.pRelFenceFd[i]) = fd2 = hfi.fb_info.rel_fence_fd[i];
                    if(*(hfi.pRelFenceFd[i]) < 0) {
                        *(hfi.pRelFenceFd[i]) = -1;
                    }
                } else {
                    close(hfi.fb_info.rel_fence_fd[i]);
                }
            }
        }
        result.appendFormat("\nLayer relFenceFd:");
        for(unsigned int i=0;i< (list->numHwLayers);i++) {
            result.appendFormat("Layer" "[%d].relFd=%d ", i,list->hwLayers[i].releaseFenceFd);
        }
        result.appendFormat("\nback buffer:");
        for(unsigned int i=0;i< MaxVideoBackBuffers;i++) {
            result.appendFormat("bk buffer" "[%d].relFenceFd=%d ",i,context->relFenceFd[i]);
        }
        ALOGD_IF(log(HLLONE),"%s", result.string());
        if(list->retireFenceFd > 0) {
            close(list->retireFenceFd);
            list->retireFenceFd = -1;
        }
		if(hfi.fb_info.ret_fence_fd >= 0) {
            list->retireFenceFd = hfi.fb_info.ret_fence_fd;
        }
#else
        for(int i=0;i<RK_MAX_BUF_NUM;i++) {
            if(hfi.fb_info.rel_fence_fd[i] >= 0 ) {
                if(i< (int)(list->numHwLayers -1)) {
                    list->hwLayers[i].releaseFenceFd = -1;
                    close(hfi.fb_info.rel_fence_fd[i]);
                } else {
                    close(hfi.fb_info.rel_fence_fd[i]);
                }
             }
    	}
        list->retireFenceFd = -1;
        if(hfi.fb_info.ret_fence_fd >= 0) {
            close(hfi.fb_info.ret_fence_fd);
        }
        //list->retireFenceFd = fb_info.ret_fence_fd;
#endif
    } else {
        for(unsigned int i=0;i< (list->numHwLayers -1);i++) {
            list->hwLayers[i].releaseFenceFd = -1;
    	}
        list->retireFenceFd = -1;
    }

#if 0
    if (context->mOneWinOpt) {
        context->mOneWinOpt = false;
        if(ioctl(context->fbFd, RK_FBIOSET_CONFIG_DONE, &fbwb)) {
            ALOGE("ID=%d:ioctl fail fbwb:%s",dpyID,strerror(errno));
            dump_config_info(fbwb,context,3);
        } else {
            ALOGD_IF(1,"vop write back config ID=%d:",dpyID);
            dump_config_info(fbwb,context,5);
        }
        hwc_free_buffer(handle);
        for(int i = 0; i < RK_MAX_BUF_NUM; i++) {
            if(fbwb.rel_fence_fd[i] >= 0 ) {
                close(fbwb.rel_fence_fd[i]);
    	    }
    	}
        if(fbwb.ret_fence_fd >= 0) {
            close(fbwb.ret_fence_fd);
        }
    }
#endif
    return 0;
}

static int hwc_rga_blit( hwcContext * context, hwc_display_contents_1_t *list)
{
#if RGA_BLIT
    hwcSTATUS status = hwcSTATUS_OK;
    unsigned int i;
    unsigned int index = 0;

#if hwcUseTime
    struct timeval tpend1, tpend2;
    long usec1 = 0;
#endif
#if hwcBlitUseTime
    struct timeval tpendblit1, tpendblit2;
    long usec2 = 0;
#endif

    hwc_layer_1_t *fbLayer = NULL;
    struct private_handle_t * fbhandle = NULL;
    bool bNeedFlush = false;
    FenceMangrRga RgaFenceMg;

#if hwcUseTime
    gettimeofday(&tpend1, NULL);
#endif
    memset(&RgaFenceMg,0,sizeof(FenceMangrRga));

    ALOGD("%s(%d):>>> Set  %d layers <<<",
         __FUNCTION__,
         __LINE__,
         list->numHwLayers);
    /* Prepare. */
    if (context->mLastCompType == HWC_RGA_VOP_GPU)
        hwc_reset_rga_blit_rects(context, list);

    for (i = 0; i < (list->numHwLayers - 1); i++)
    {
        /* Check whether this composition can be handled by hwcomposer. */
        if (list->hwLayers[i].compositionType >= HWC_BLITTER)
        {
#if FENCE_TIME_USE
            struct timeval tstart, tend;
            gettimeofday(&tstart, NULL);
#endif

            #if 0
            if(context->membk_fence_acqfd[context->membk_index] > 0)
            {
                sync_wait(context->membk_fence_acqfd[context->membk_index], 500);
                close(context->membk_fence_acqfd[context->membk_index]);
                context->membk_fence_acqfd[context->membk_index] = -1;
                //ALOGD("close0 rga acq_fd=%d",fb_info.win_par[0].area_par[0].acq_fence_fd);
            }
            #endif
#if FENCE_TIME_USE
            gettimeofday(&tend, NULL);
            if(((tend.tv_sec - tstart.tv_sec)*1000 + (tend.tv_usec - tstart.tv_usec)/1000) > 16)
            {
                ALOGW("wait for LCDC fence too long ,spent t = %ld ms",((tend.tv_sec - tstart.tv_sec)*1000 + (tend.tv_usec - tstart.tv_usec)/1000));
            }
#endif

#if ENABLE_HWC_WORMHOLE
            hwcRECT FbRect;
            hwcArea * area;
            hwc_region_t holeregion;
#endif
            bNeedFlush = true;

            if (context->mLastCompType == HWC_RGA_VOP_GPU) {
                buffer_handle_t rgaHnd = hwc_rga_blit_get_next_buffer(context);
                fbhandle = (struct private_handle_t*)rgaHnd;
            } else {
                fbLayer = &list->hwLayers[list->numHwLayers - 1];
                ALOGV("fbLyaer = %x,num=%d",fbLayer,list->numHwLayers);
                if (fbLayer == NULL)
                {
                    ALOGE("fbLayer is null");
                    hwcONERROR(hwcSTATUS_INVALID_ARGUMENT);
                }
                fbhandle = (struct private_handle_t*)fbLayer->handle;
            }

            if (fbhandle == NULL)
            {
                ALOGE("fbhandle is null");
                hwcONERROR(hwcSTATUS_INVALID_ARGUMENT);
            }
            ALOGV("i=%d,tpye=%d,hanlde=%p",i,list->hwLayers[i].compositionType,fbhandle);
#if ENABLE_HWC_WORMHOLE
            /* Reset allocated areas. */
            if (context->compositionArea != NULL)
            {
                ZoneFree(context, context->compositionArea);

                context->compositionArea = NULL;
            }

            FbRect.left = 0;
            FbRect.top = 0;
            FbRect.right = fbhandle->width;
            FbRect.bottom = fbhandle->height;

            /* Generate new areas. */
            /* Put a no-owner area with screen size, this is for worm hole,
             * and is needed for clipping. */
            context->compositionArea = zone_alloc(context,
                                       NULL,
                                       &FbRect,
                                       0U);

            /* Split areas: go through all regions. */
            for (unsigned int k = 0; k < list->numHwLayers - 1; k++)
            {
                int owner = 1U << k;
                hwc_layer_1_t *  hwLayer = &list->hwLayers[k];
                hwc_region_t * region  = &hwLayer->visibleRegionScreen;
                //struct private_handle_t* srchnd = (struct private_handle_t *) hwLayer->handle;

                if((hwLayer->blending & 0xFFFF) != HWC_BLENDING_NONE)
                {
                    ALOGV("ignore alpha layer");
                    continue;
                }
                /* Now go through all rectangles to split areas. */
                for (int j = 0; j < region->numRects; j++)
                {
                    /* Assume the region will never go out of dest surface. */
                    DivArea(context,
                               context->compositionArea,
                               (hwcRECT *) &region->rects[j],
                               owner);

                }

            }
#if DUMP_SPLIT_AREA
            LOGV("SPLITED AREA:");
            hwcDumpArea(context->compositionArea);
#endif

            area = context->compositionArea;

            while (area != NULL)
            {
                /* Check worm hole first. */
                if (area->owners == 0U)
                {

                    holeregion.numRects = 1;
                    holeregion.rects = (hwc_rect_t const*) & area->rect;
                    /* Setup worm hole source. */
                    LOGV(" WormHole [%d,%d,%d,%d]",
                         area->rect.left,
                         area->rect.top,
                         area->rect.right,
                         area->rect.bottom
                        );

                    hwcClear(context,
                             0xFF000000,
                             &list->hwLayers[i],
                             fbhandle,
                             (hwc_rect_t *)&area->rect,
                             &holeregion
                            );

                    /* Advance to next area. */
                }
                area = area->next;
            }
#endif
            /* Done. */
            break;
        }
        else if (list->hwLayers[i].compositionType == HWC_FRAMEBUFFER)
        {
            /* Previous swap rectangle is gone. */
            break;

        }
    }
    /* Go through the layer list one-by-one blitting each onto the FB */

#if RGA_USE_FENCE
    for(i = 0;i< RGA_REL_FENCE_NUM;i++)
    {
        context->rga_fence_relfd[i] = -1;
    }
    if(context->composer_mode == HWC_RGA)
        RgaFenceMg.use_fence = true;
#endif

    for (i = 0; i < list->numHwLayers -1; i++)
    {
        switch (list->hwLayers[i].compositionType)
        {
            case HWC_BLITTER:
                /* Do the blit. */

#if hwcBlitUseTime
                gettimeofday(&tpendblit1, NULL);
#endif
                hwcONERROR(
                    hwcBlit(context,
                            &list->hwLayers[i],
                            fbhandle,
                            &list->hwLayers[i].sourceCrop,
                            &list->hwLayers[i].displayFrame,
                            &list->hwLayers[i].visibleRegionScreen,
                            &RgaFenceMg,index));

#if hwcBlitUseTime
                gettimeofday(&tpendblit2, NULL);
                usec2 = 1000 * (tpendblit2.tv_sec - tpendblit1.tv_sec) + (tpendblit2.tv_usec - tpendblit1.tv_usec) / 1000;
#endif
                index++;
                break;

            case HWC_CLEAR_HOLE:
                LOGV("%s(%d):Layer %d is CLEAR_HOLE", __FUNCTION__, __LINE__, i);
                /* Do the clear, color = (0, 0, 0, 1). */
                /* TODO: Only clear holes on screen.
                 * See Layer::onDraw() of surfaceflinger. */
                if (i != 0) break;

                hwcONERROR(
                    hwcClear(context,
                             0xFF000000,
                             &list->hwLayers[i],
                             fbhandle,
                             &list->hwLayers[i].displayFrame,
                             &list->hwLayers[i].visibleRegionScreen));
                break;

            case HWC_DIM:
                LOGV("%s(%d):Layer %d is DIM", __FUNCTION__, __LINE__, i);
                if (i == 0)
                {
                    /* Use clear instead of dim for the first layer. */
                    hwcONERROR(
                        hwcClear(context,
                                 ((list->hwLayers[0].blending & 0xFF0000) << 8),
                                 &list->hwLayers[i],
                                 fbhandle,
                                 &list->hwLayers[i].displayFrame,
                                 &list->hwLayers[i].visibleRegionScreen));
                }
                else
                {
                    /* Do the dim. */
                    hwcONERROR(
                        hwcDim(context,
                               &list->hwLayers[i],
                               fbhandle,
                               &list->hwLayers[i].displayFrame,
                               &list->hwLayers[i].visibleRegionScreen));
                }
                break;

            case HWC_OVERLAY:
                /* TODO: HANDLE OVERLAY LAYERS HERE. */
                LOGV("%s(%d):Layer %d is OVERLAY", __FUNCTION__, __LINE__, i);
                break;
            }

    }

#if hwcUseTime
    gettimeofday(&tpend2, NULL);
    usec1 = 1000 * (tpend2.tv_sec - tpend1.tv_sec) + (tpend2.tv_usec - tpend1.tv_usec) / 1000;
    LOGV("hwcBlit compositer %d layers use time=%ld ms", list->numHwLayers -1, usec1);
#endif

    if (context->mLastCompType == HWC_RGA_VOP_GPU)
        hwc_reset_rga_blit_rects(context, list);

    return 0; //? 0 : HWC_EGL_ERROR;
OnError:
    if (context->mLastCompType == HWC_RGA_VOP_GPU)
        hwc_reset_rga_blit_rects(context, list);
    LOGE("%s(%d):Failed!", __FUNCTION__, __LINE__);
    return HWC_EGL_ERROR;
#else
    return 0;
#endif
}

static void hwc_static_screen_opt_handler(int sig)
{
#if HTGFORCEREFRESH
    hwcContext * ctxp = _contextAnchor;
    hwcContext * ctxe = _contextAnchor1;
    if (sig == SIGALRM) {
        ctxp->mOneWinOpt = true;
        if (ctxe) ctxe->mOneWinOpt = true;
        pthread_mutex_lock(&ctxp->mRefresh.mlk);
        ctxp->mRefresh.count = 100;
        ALOGD_IF(log(HLLTWO),"Htg:mRefresh.count=%d",ctxp->mRefresh.count);
        pthread_mutex_unlock(&ctxp->mRefresh.mlk);
        pthread_cond_signal(&ctxp->mRefresh.cond);
    }
#endif

    return;
}

static int hwc_static_screen_opt_set()
{
    hwcContext * context = _contextAnchor;

    if (context->isVr)
	return 0;

    struct itimerval tv = {{0,0},{0,0}};
    if (-1 != context->mLastCompType) {
        int msec = 0;
        char value[PROPERTY_VALUE_MAX];
        property_get("sys.vwb.time",value,"2500");
        msec = atoi(value);
        if (msec > 5000)
            msec = 5000;
        if (msec < 250)
            msec = 250;
        tv.it_value.tv_usec = (msec % 1000)*1000;
        tv.it_value.tv_sec = msec / 1000;
        setitimer(ITIMER_REAL, &tv, NULL);
        //ALOGD("reset timer!");
    } else {
        tv.it_value.tv_usec = 0;
        setitimer(ITIMER_REAL, &tv, NULL);
        ALOGD_IF(log(HLLTWO),"close timer!");
    }
    return 0;
}

static int hwc_alloc_buffer(buffer_handle_t *hnd, int w,int h,int *s,int fmt,int usage)
{
    int stride_gr = 0;
    hwcContext * context = _contextAnchor;

    int err = context->mAllocDev->alloc(context->mAllocDev, w, h, fmt, usage, hnd,
                                                                       &stride_gr);
    if (!err) {
        struct private_handle_t*handle = (struct private_handle_t*)hnd;
        ALOGD("Dim buffer alloc fd [%dx%d,f=%d],fd=%d ",handle->width,
                            handle->height,handle->format,handle->share_fd);
    } else
        ALOGE("Dim buffer alloc faild");

    return err;
}

static int hwc_free_buffer(buffer_handle_t hnd)
{
    hwcContext * context = _contextAnchor;
    int err = 0;
	if (context && hnd) {
        err = context->mAllocDev->free(context->mAllocDev, hnd);
        ALOGW_IF(err,"free mDimHandle failed %d (%s)", err, strerror(-err));
	}
    return err;
}

static int hwc_check_fencefd(size_t numDisplays,hwc_display_contents_1_t  ** displays)
{
    for (size_t i = 0; i < numDisplays; i++) {
        hwc_display_contents_1_t *list = displays[i];
        if(list){
            int numLayers = list->numHwLayers;
            for(int j = 0;j<numLayers;j++){
                hwc_layer_1_t *layer = &list->hwLayers[j];
                if(layer && layer->acquireFenceFd>0){
                    ALOGW("Foce to close aqFenceFd,%d,%d",i,j);
                    close(layer->acquireFenceFd);
                    layer->acquireFenceFd = -1;
                }
                if(layer){
                    ALOGD_IF(log(HLLSIX),"i=%d,j=%d,rff=%d",i,j,layer->releaseFenceFd);
                }
            }
        }
    }
    return 0;
}

static int hwc_set_screen(hwc_composer_device_1 *dev, hwc_display_contents_1_t *list,int dpyID)
{
    ATRACE_CALL();
    if(!is_need_post(list,dpyID,0)){
        return -1;
    }
    hwcContext * ctxp = _contextAnchor;
    hwcContext * context = _contextAnchor;
    if(dpyID == HWCE){
        context = _contextAnchor1;
    }

#if hwcUseTime
    struct timeval tpend1, tpend2;
    long usec1 = 0;
#endif

    hwc_display_t dpy = NULL;
    hwc_surface_t surf = NULL;

    if (list != NULL) {
        dpy = list->dpy;
        surf = list->sur;
    }

    //if (context && !context->isRk3399)
    hwc_sync(list);

    /* Check device handle. */
    if (dpyID == 0 && (context == NULL ||
        &_contextAnchor->device.common != (hw_device_t *) dev)) {
        LOGE("%s(%d): Invalid device!", __FUNCTION__, __LINE__);
        return HWC_EGL_ERROR;
    }

    /* Check layer list. */
    if ((list == NULL  || list->numHwLayers == 0) && dpyID == 0) {
        ALOGE("(%d):list=NULL,Layers =%d",__LINE__,list->numHwLayers);
        /* Reset swap rectangles. */
        return -1;
    } else if(list == NULL) {
        return -1;
    }

    //if (context && context->isRk3399)
    //    hwc_collect_acquire_fence_fd(context, list);

    if(ctxp->mBootCnt < BOOTCOUNT) {
        int offset = 0;
        int numLayers = 0;
        hwc_layer_1_t *fbLayer = NULL;
        struct private_handle_t * fbhandle = NULL;
        hwc_sync_release(list);
        if(0 == dpyID) ctxp->mBootCnt++;
        numLayers = list->numHwLayers;
        if(numLayers > 0) {
            fbLayer = &list->hwLayers[numLayers - 1];
            fbhandle = (struct private_handle_t*)fbLayer->handle;
            offset = fbhandle ? fbhandle->offset : -1;
        }
        ALOGW("hwc skip,numLayers=%d,offset=%d",list->numHwLayers,offset);
        return 0;
    }

    LOGV("%s(%d):>>> Set start %d layers <<<,mode=%d",
         __FUNCTION__,
         __LINE__,
         list->numHwLayers,context->zone_manager.composter_mode);

#if hwcDEBUG
    if(log(HLLEIG)){
        LOGD("%s(%d):Layers to set:", __FUNCTION__, __LINE__);
        _Dump(list);
    }
#endif
#if hwcUseTime
    gettimeofday(&tpend1,NULL);
#endif
    int ret = -1;
    if(context->mNeedRgaTransform) {
        int w_valid = context->mRgaTBI.w_valid;
        int h_valid = context->mRgaTBI.h_valid;
        int layer_fd = context->mRgaTBI.layer_fd;
        int lastfd = context->mRgaTBI.transform;
        uint32_t transform = context->mRgaTBI.transform;
        bool trsfrmbyrga = context->mRgaTBI.trsfrmbyrga;
        struct private_handle_t* hdl = context->mRgaTBI.hdl;
        int Dstfmt = trsfrmbyrga ? hwChangeRgaFormat(hdl->format) : RK_FORMAT_YCbCr_420_SP;
        if (context->mRgaTBI.type == 1) {
            Dstfmt = RK_FORMAT_YCbCr_420_SP;
            rgaRotateScale(context,transform,layer_fd,Dstfmt,false);
        } else
            rga_video_copybit(hdl,transform,w_valid,h_valid,layer_fd,Dstfmt,
                                                    trsfrmbyrga,dpyID,false);
#if USE_VIDEO_BACK_BUFFERS
        context->mCurVideoIndex++;  //update video buffer index
#else
        if(trsfrmbyrga)
            context->mCurVideoIndex++;  //update video buffer index
#endif
    }

    if (context->mLastCompType == HWC_RGA_VOP_GPU) {
        hwc_rga_blit(context, list);
    }

    if(context->zone_manager.composter_mode == HWC_LCDC) {
        ret = hwc_set_lcdc(context,list,0);
    } else if (context->zone_manager.composter_mode == HWC_FRAMEBUFFER) {
        ret = hwc_Post(context,list);
    } else if (context->zone_manager.composter_mode == HWC_MIX) {
        ret = hwc_set_lcdc(context,list,1);
    } else if (context->zone_manager.composter_mode == HWC_MIX_V2) {
        ret = hwc_set_lcdc(context,list,2);
    }

#if !(defined(GPU_G6110) || defined(RK3288_BOX) || defined(RK3399_BOX))
    if(dpyID == HWCP)
#endif
    {
        static int frame_cnt = 0;
        char value[PROPERTY_VALUE_MAX];
        property_get("sys.glib.state", value, "0");
        int skipcnt = atoi(value);
        if(skipcnt > 0) {
            if(((++frame_cnt)%skipcnt) == 0) {
                eglSwapBuffers((EGLDisplay) NULL, (EGLSurface) NULL);
            }
        } else {
            frame_cnt = 0;
        }
    }

#if hwcUseTime
    gettimeofday(&tpend2,NULL);
    usec1 = 1000*(tpend2.tv_sec - tpend1.tv_sec) + (tpend2.tv_usec- tpend1.tv_usec)/1000;
    LOGD("hwcBlit compositer %d layers use time=%ld ms",list->numHwLayers,usec1);
#endif
    //close(Context->fbFd1);
#ifdef ENABLE_HDMI_APP_LANDSCAP_TO_PORTRAIT
    if (list != NULL && getHdmiMode()>0){
        if (bootanimFinish==0){
        bootanimFinish = hwc_get_int_property("service.bootanim.exit","0")
            if (bootanimFinish > 0){
                usleep(1000000);
            }
        }
    }
#endif

    //ALOGD("set end");
    return ret; //? 0 : HWC_EGL_ERROR;
}

static int hwc_set_virtual(hwc_composer_device_1_t * dev, hwc_display_contents_1_t  **contents, unsigned int rga_fb_addr)
{
    ATRACE_CALL();
	hwc_display_contents_1_t* list_pri = contents[0];
	hwc_display_contents_1_t* list_wfd = contents[2];
	hwc_layer_1_t *  fbLayer = &list_pri->hwLayers[list_pri->numHwLayers - 1];
	hwc_layer_1_t *  wfdLayer = &list_wfd->hwLayers[list_wfd->numHwLayers - 1];
	hwcContext * context = _contextAnchor;
	struct timeval tpend1, tpend2;
	long usec1 = 0;
	gettimeofday(&tpend1,NULL);
	if (list_wfd)
	{
		hwc_sync(list_wfd);
	}
	if (fbLayer==NULL || wfdLayer==NULL)
	{
		return -1;
	}

	if ((context->wfdOptimize>0) && wfdLayer->handle)
	{
		hwc_cfg_t cfg;
		memset(&cfg, 0, sizeof(hwc_cfg_t));
		cfg.src_handle = (struct private_handle_t *)fbLayer->handle;
		cfg.dst_handle = (struct private_handle_t *)wfdLayer->handle;
		cfg.src_rect.left = (int)fbLayer->displayFrame.left;
		cfg.src_rect.top = (int)fbLayer->displayFrame.top;
		cfg.src_rect.right = (int)fbLayer->displayFrame.right;
		cfg.src_rect.bottom = (int)fbLayer->displayFrame.bottom;
		//cfg.src_format = cfg.src_handle->format;

		cfg.rga_fbAddr = rga_fb_addr;
		cfg.dst_rect.left = (int)wfdLayer->displayFrame.left;
		cfg.dst_rect.top = (int)wfdLayer->displayFrame.top;
		cfg.dst_rect.right = (int)wfdLayer->displayFrame.right;
		cfg.dst_rect.bottom = (int)wfdLayer->displayFrame.bottom;
		//cfg.dst_format = cfg.dst_handle->format;
		set_rga_cfg(&cfg);
		do_rga_transform_and_scale();
	}
#if VIRTUAL_RGA_BLIT
	else if(context->wfdRgaBlit)
	{
	    hwcContext * ctx = _contextAnchor2;
	    hwc_rga_blit(ctx, list_wfd);
	}
#endif

	gettimeofday(&tpend2,NULL);
	usec1 = 1000*(tpend2.tv_sec - tpend1.tv_sec) + (tpend2.tv_usec- tpend1.tv_usec)/1000;
	ALOGV("hwc use time=%ld ms",usec1);
	return 0;
}

int
hwc_set(
    hwc_composer_device_1_t * dev,
    size_t numDisplays,
    hwc_display_contents_1_t  ** displays
    )
{
    ATRACE_CALL();
    hwcContext * ctxp = _contextAnchor;
    int ret[4] = {0,0,0,0};
#if (defined(GPU_G6110) || defined(RK_BOX))
#ifdef RK3288_BOX
    if(_contextAnchor->mLcdcNum==1) {
        if(getHdmiMode() == 1 || _contextAnchor->mHdmiSI.CvbsOn) {
            hotplug_set_overscan(0);
        }
    } else {
        hotplug_set_overscan(0);
    }
#else
    if(getHdmiMode() == 1 || _contextAnchor->mHdmiSI.CvbsOn) {
        hotplug_set_overscan(0);
    }
#endif
#endif
    for (uint32_t i = 0; i < numDisplays; i++) {
        hwc_display_contents_1_t* list = displays[i];
        switch(i) {
            case HWC_DISPLAY_PRIMARY:
            case HWC_DISPLAY_EXTERNAL:
                ret[i] = hwc_set_screen(dev, list, i);
                break;
            case HWC_DISPLAY_VIRTUAL:
                if (list){
                    unsigned int fb_addr = 0;
                    // fb_addr = context->hwc_ion.pion->phys + context->hwc_ion.last_offset;
                    ret[2] = hwc_set_virtual(dev, displays,fb_addr);
                }
                break;
            default:
                ret[3] = -EINVAL;
        }
    }
    for (uint32_t i = 0; i < numDisplays; i++) {
        hwc_display_contents_1_t* list = displays[i];
        if (list)
            hwc_sync_release(list);

        if (list && ctxp && ctxp->isVr)
            hwc_single_buffer_close_rel_fence(list);
    }
    hwc_check_fencefd(numDisplays,displays);

#if HWC_DELAY_TIME_TEST
    while (hwc_get_int_property("sys.hwc.test","0")) {
	    usleep(HWC_DELAY_TIME_TEST);
    }
#endif
    if (ret[0])
        ALOGW("Why is the set error-----------------------------------------------");

    hwc_static_screen_opt_set();
    return 0;
}

static void hwc_registerProcs(struct hwc_composer_device_1* dev,
                                    hwc_procs_t const* procs)
{
    hwcContext * context = _contextAnchor;

    context->procs =  (hwc_procs_t *)procs;
}


static int hwc_event_control(struct hwc_composer_device_1* dev,
        int dpy, int event, int enabled)
{

    hwcContext * context = _contextAnchor;

    bool isLog = log(HLLFIV);
    ALOGD_IF(isLog,"D_EN[%d,%d]",dpy,enabled);

    if (context && context->isVr) {
        ALOGD_IF(isLog,"D_EN[%d,%d] vr return",dpy,enabled);
	    return 0;
    }

    if (dpy==1 && _contextAnchor1) {
        context = _contextAnchor1;
        if (context->fbFd <= 0) {
            ALOGW("D_EN[%d,%d] ERROR",dpy,enabled);
            return 0;
        }
    }

    switch (event) {
    case HWC_EVENT_VSYNC:
    {
        int val = !!enabled;
        int err;

        err = ioctl(context->fbFd, RK_FBIOSET_VSYNC_ENABLE, &val);
        if (err < 0)
        {
            LOGE(" RK_FBIOSET_VSYNC_ENABLE err=%d",err);
            return -1;
        }
        return 0;
    }
    default:
        return -1;
    }
}

static void handle_vsync_event(hwcContext * context )
{

    if (!context->procs)
        return;

    int err = lseek(context->vsync_fd, 0, SEEK_SET);
    if (err < 0) {
        ALOGE("error seeking to vsync timestamp: %s", strerror(errno));
        return;
    }

    char buf[4096];
    err = read(context->vsync_fd, buf, sizeof(buf));
    if (err < 0) {
        ALOGE("error reading vsync timestamp: %s", strerror(errno));
        return;
    }
    buf[sizeof(buf) - 1] = '\0';

    //errno = 0;
    uint64_t timestamp = strtoull(buf, NULL, 0) ;/*+ (uint64_t)(1e9 / context->fb_fps)  ;*/
    if(context->timestamp != timestamp){
        context->timestamp = timestamp;
        context->procs->vsync(context->procs, 0, timestamp);
    }
/*
    uint64_t mNextFakeVSync = timestamp + (uint64_t)(1e9 / context->fb_fps);
    struct timespec spec;
    spec.tv_sec  = mNextFakeVSync / 1000000000;
    spec.tv_nsec = mNextFakeVSync % 1000000000;

    do {
        err = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &spec, NULL);
    } while (err<0 && errno == EINTR);


    if (err==0)
    {
        context->procs->vsync(context->procs, 0, mNextFakeVSync );
        //ALOGD(" timestamp=%lld ms,preid=%lld us",mNextFakeVSync/1000000,(uint64_t)(1e6 / context->fb_fps) );
    }
    else
    {
        ALOGE(" clock_nanosleep ERR!!!");
    }
*/
}

static void hwc_primary_screen_query() {
    hwcContext * context = _contextAnchor;
    if (context) {
        char buf[100];
        int width = 0;
        int height = 0;
        int fd = -1;
        fd = open("/sys/class/graphics/fb0/screen_info", O_RDONLY);
        if (fd < 0) {
            ALOGE("hwc_change_config:open fb0 screen_info error,fd=%d",fd);
            return;
        }
        if (read(fd,buf,sizeof(buf)) < 0) {
            ALOGE("error reading fb0 screen_info: %s", strerror(errno));
            return;
        }
        close(fd);
        sscanf(buf,"xres:%d yres:%d",&width,&height);
        ALOGD("hwc_change_config:width=%d,height=%d",width,height);
        if (context->mIsMipiDualOutMode) {
            context->dpyAttr[HWC_DISPLAY_PRIMARY].relxres = width / 2;
            context->dpyAttr[HWC_DISPLAY_PRIMARY].relyres = height * 2;
        } else {
            context->dpyAttr[HWC_DISPLAY_PRIMARY].relxres = width;
            context->dpyAttr[HWC_DISPLAY_PRIMARY].relyres = height;
        }
    }

    return;
}

void hwc_change_screen_config(int dpy, int fb, int state) {
    hwcContext * context = _contextAnchor;
    if (context) {
        char buf[100];
        int width = 0;
        int height = 0;
        int fd = -1;
        fd = open("/sys/class/graphics/fb0/screen_info", O_RDONLY);
        if(fd < 0)
        {
            ALOGE("hwc_change_config:open fb0 screen_info error,fd=%d",fd);
            return;
        }
        if(read(fd,buf,sizeof(buf)) < 0)
        {
            ALOGE("error reading fb0 screen_info: %s", strerror(errno));
            return;
        }
        close(fd);
        sscanf(buf,"xres:%d yres:%d",&width,&height);
        ALOGD("hwc_change_config:width=%d,height=%d",width,height);
        context->dpyAttr[HWC_DISPLAY_PRIMARY].relxres = width;
        context->dpyAttr[HWC_DISPLAY_PRIMARY].relyres = height;
#if HTGFORCEREFRESH
        pthread_mutex_lock(&context->mRefresh.mlk);
        context->mRefresh.count = 0;
        ALOGD_IF(log(HLLTWO),"Htg:mRefresh.count=%d",context->mRefresh.count);
        pthread_mutex_unlock(&context->mRefresh.mlk);
        pthread_cond_signal(&context->mRefresh.cond);
#endif
    }

    return;
}

void hwc_change_config(){
#ifdef RK3288_BOX
    hwcContext * context = _contextAnchor;
    if(context->mLcdcNum == 2){
        char buf[100];
        int width = 0;
        int height = 0;
        int fd = -1;
        fd = open("/sys/class/graphics/fb0/screen_info", O_RDONLY);
        if(fd < 0)
    	{
    	    ALOGE("hwc_change_config:open fb0 screen_info error,fd=%d",fd);
            return;
    	}
    	if(read(fd,buf,sizeof(buf)) < 0)
        {
            ALOGE("error reading fb0 screen_info: %s", strerror(errno));
            return;
        }
        close(fd);
    	sscanf(buf,"xres:%d yres:%d",&width,&height);
        ALOGD("hwc_change_config:width=%d,height=%d",width,height);
    	context->dpyAttr[HWC_DISPLAY_PRIMARY].relxres = width;
        context->dpyAttr[HWC_DISPLAY_PRIMARY].relyres = height;
#if HTGFORCEREFRESH
        pthread_mutex_lock(&context->mRefresh.mlk);
        context->mRefresh.count = 0;
        ALOGD_IF(log(HLLTWO),"Htg:mRefresh.count=%d",context->mRefresh.count);
        pthread_mutex_unlock(&context->mRefresh.mlk);
        pthread_cond_signal(&context->mRefresh.cond);
#endif
    }
#endif
    return;
}

void handle_hotplug_event(int hdmi_mode ,int flag )
{
    hwcContext * context = _contextAnchor;
    if (!context->procs){
        return;
    }

    if (hdmi_mode == -1 && flag == -1) {
        context->procs->invalidate(context->procs);
        return;
    }

    bool isNeedRemove = true;
#if (defined(GPU_G6110) || defined(RK3288_BOX))
#ifdef RK3288_BOX
    if(context->mLcdcNum == 1){
#endif
        if(!context->mIsBootanimExit){
            if(hdmi_mode){
                if(6 == flag){
                    context->mHdmiSI.HdmiOn = true;
                    context->mHdmiSI.CvbsOn = false;
                }else if(1 == flag){
                    context->mHdmiSI.CvbsOn = true;
                    context->mHdmiSI.HdmiOn = false;
                }
                hotplug_free_dimbuffer();
                hotplug_get_config(1);
                hotplug_set_config();
            }
            return;
        }
        if(context->mIsFirstCallbackToHotplug){
            isNeedRemove = false;
            context->mIsFirstCallbackToHotplug = false;
        }
#ifdef RK3288_BOX
    }
#endif
#endif
    if(isNeedRemove && (context->mHdmiSI.CvbsOn || context->mHdmiSI.HdmiOn)){
        int count = 0;
        if(context->mHdmiSI.NeedReDst){
            context->mHdmiSI.NeedReDst = false;
        }
        while(_contextAnchor1 && _contextAnchor1->fb_blanked){
            count++;
            usleep(10000);
            if(300==count){
                ALOGW("wait for unblank");
                break;
            }
        }
        hdmi_noready = true;
        hotplug_free_dimbuffer();
        if(context->mHdmiSI.CvbsOn){
            context->mHdmiSI.CvbsOn = false;
        }else{
            context->mHdmiSI.HdmiOn = false;
        }
        if(_contextAnchor1){
            _contextAnchor1->fb_blanked = 1;
        }
#if !defined(GPU_G6110) && (defined(RK_MID) || defined(RK_VR))
        hotplug_set_frame(context,0);
#endif
#ifdef RK3288_BOX
        if(context->mLcdcNum == 2){
            hotplug_set_frame(context,0);
        }
#endif

        if(context->isRk3399 || context->isRk3366){
            hotplug_set_frame(context,0);
        }

        context->dpyAttr[HWC_DISPLAY_EXTERNAL].connected = false;
        context->procs->hotplug(context->procs, HWC_DISPLAY_EXTERNAL, 0);
#if (defined(GPU_G6110) || defined(RK3288_BOX) || defined(RK3399_BOX))
    if(context->mLcdcNum == 1){
        hotplug_set_overscan(1);
    }
#endif
        ALOGI("remove hotplug device [%d,%d,%d]",__LINE__,hdmi_mode,flag);
    }
    if(hdmi_mode){
        hotplug_free_dimbuffer();
        hotplug_get_config(1);
        hotplug_set_config();
        if(6 == flag){
            context->mHdmiSI.HdmiOn = true;
            context->mHdmiSI.CvbsOn = false;
        }else if(1 == flag){
            context->mHdmiSI.CvbsOn = true;
            context->mHdmiSI.HdmiOn = false;
        }
#if !defined(GPU_G6110) && (defined(RK_MID) || defined(RK_VR))
        hotplug_set_frame(context,0);
#endif
#ifdef RK3288_BOX
        if(context->mLcdcNum == 2){
            hotplug_set_frame(context,0);
        }
#endif
        if(context->isRk3399 || context->isRk3366){
            hotplug_set_frame(context,0);
        }

        context->procs->hotplug(context->procs, HWC_DISPLAY_EXTERNAL, 1);
        ALOGI("connet to hotplug device [%d,%d,%d]",__LINE__,hdmi_mode,flag);
#if HTGFORCEREFRESH
        pthread_mutex_lock(&context->mRefresh.mlk);
        context->mRefresh.count = 0;
        ALOGD_IF(log(HLLTWO),"Htg:mRefresh.count=%d",context->mRefresh.count);
        pthread_mutex_unlock(&context->mRefresh.mlk);
        pthread_cond_signal(&context->mRefresh.cond);
#endif
#if (defined(GPU_G6110) || defined(RK3288_BOX) || defined(RK3399_BOX))
    if(context->mLcdcNum == 1){
        hotplug_set_overscan(0);
    }
#endif
    }

    return;
}


static void *hwc_thread(void *data)
{
    prctl(PR_SET_NAME,"HWC_Vsync");
    hwcContext * context = _contextAnchor;

#if 0
    uint64_t timestamp = 0;
    nsecs_t now = 0;
    nsecs_t next_vsync = 0;
    nsecs_t sleep;
    const nsecs_t period = nsecs_t(1e9 / 50.0);
    struct timespec spec;
   // int err;
    do
    {

        now = systemTime(CLOCK_MONOTONIC);
        next_vsync = context->mNextFakeVSync;

        sleep = next_vsync - now;
        if (sleep < 0) {
            // we missed, find where the next vsync should be
            sleep = (period - ((now - next_vsync) % period));
            next_vsync = now + sleep;
        }
        context->mNextFakeVSync = next_vsync + period;

        spec.tv_sec  = next_vsync / 1000000000;
        spec.tv_nsec = next_vsync % 1000000000;

        do
        {
            err = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &spec, NULL);
        } while (err<0 && errno == EINTR);

        if (err == 0)
        {
            if (context->procs && context->procs->vsync)
            {
                context->procs->vsync(context->procs, 0, next_vsync);

                ALOGD(" hwc_thread next_vsync=%lld ",next_vsync);
            }

        }

    } while (1);
#endif

  //    char uevent_desc[4096];
   // memset(uevent_desc, 0, sizeof(uevent_desc));



    char temp[4096];

    int err = read(context->vsync_fd, temp, sizeof(temp));
    if (err < 0) {
        ALOGE("error reading vsync timestamp: %s", strerror(errno));
        return NULL;
    }

    struct pollfd fds[1];
    fds[0].fd = context->vsync_fd;
    fds[0].events = POLLPRI;
    //fds[1].fd = uevent_get_fd();
    //fds[1].events = POLLIN;

    while (true) {
        int err = poll(fds, 1, -1);
        if (err > 0) {
            if (fds[0].revents & POLLPRI) {
                handle_vsync_event(context);
            }

        }
        else if (err == -1) {
            if (errno == EINTR)
                break;
            ALOGE("error in vsync thread: %s", strerror(errno));
        }
    }

    return NULL;
}



int
hwc_device_close(
    struct hw_device_t *dev
    )
{
    int i;
    int err=0;
    hwcContext * context = _contextAnchor;

    LOGD("%s(%d):Close hwc device in thread=%d",
         __FUNCTION__, __LINE__, gettid());
    ALOGD("hwc_device_close ----------------------");
    /* Check device. */
    if (context == NULL
    || &context->device.common != (hw_device_t *) dev
    )
    {
        LOGE("%s(%d):Invalid device!", __FUNCTION__, __LINE__);

        return -EINVAL;
    }

    if (--context->reference > 0)
    {
        /* Dereferenced only. */
        return 0;
    }

    if(context->engine_fd > -1)
        close(context->engine_fd);
    /* Clean context. */
    if(context->vsync_fd > 0)
        close(context->vsync_fd);
    if(context->fbFd > 0)
    {
        close(context->fbFd );

    }
    if(context->fbFd1 > 0)
    {
        close(context->fbFd1 );
    }

//#if  (ENABLE_TRANSFORM_BY_RGA | ENABLE_LCDC_IN_NV12_TRANSFORM)
    if(bkupmanage.phd_drt)
    {
        err = context->mAllocDev->free(context->mAllocDev, bkupmanage.phd_drt);
        ALOGW_IF(err, "free bkupmanage.phd_drt (...) failed %d (%s)", err, strerror(-err));
    }
//#endif


    // free video gralloc buffer
    for(i=0;i<MaxVideoBackBuffers;i++)
    {
        if(context->pbvideo_bk[i] != NULL)
            err = context->mAllocDev->free(context->mAllocDev, context->pbvideo_bk[i]);
        if(!err)
        {
            context->fd_video_bk[i] = -1;
            context->base_video_bk[i] = 0;
            context->pbvideo_bk[i] = NULL;
        }
        ALOGW_IF(err, "free pbvideo_bk (...) failed %d (%s)", err, strerror(-err));
    }

#if OPTIMIZATION_FOR_DIMLAYER
    if(context->mDimHandle)
    {
        err = context->mAllocDev->free(context->mAllocDev, context->mDimHandle);
        ALOGW_IF(err, "free mDimHandle (...) failed %d (%s)", err, strerror(-err));
    }
#endif

    hwc_rga_blit_free_rects(context);

    pthread_mutex_destroy(&context->lock);
    free(context);

    _contextAnchor = NULL;

    return 0;
}

static int hwc_getDisplayConfigs(struct hwc_composer_device_1* dev, int disp,
            			uint32_t* configs, size_t* numConfigs)
{
   int ret = 0;
   hwcContext * pdev = ( hwcContext  *)dev;
    //in 1.1 there is no way to choose a config, report as config id # 0
    //This config is passed to getDisplayAttributes. Ignore for now.
    switch(disp) {

        case HWC_DISPLAY_PRIMARY:
            if(*numConfigs > 0) {
                configs[0] = 0;
                *numConfigs = 1;
            }
            ret = 0; //NO_ERROR
            break;
        case HWC_DISPLAY_EXTERNAL:
            ret = -1; //Not connected
            if(pdev->dpyAttr[HWC_DISPLAY_EXTERNAL].connected) {
                ret = 0; //NO_ERROR
                if(*numConfigs > 0) {
                    configs[0] = 0;
                    *numConfigs = 1;
                }
            }
            break;
    }
   return 0;
}

static int hwc_getDisplayAttributes(struct hwc_composer_device_1* dev, int disp,
            			 uint32_t config, const uint32_t* attributes, int32_t* values)
{

    hwcContext  *pdev = (hwcContext  *)dev;
    //If hotpluggable displays are inactive return error
    if(disp == HWC_DISPLAY_EXTERNAL && !pdev->dpyAttr[disp].connected) {
        return -1;
    }
    static  uint32_t DISPLAY_ATTRIBUTES[] = {
        HWC_DISPLAY_VSYNC_PERIOD,
        HWC_DISPLAY_WIDTH,
        HWC_DISPLAY_HEIGHT,
        HWC_DISPLAY_DPI_X,
        HWC_DISPLAY_DPI_Y,
        HWC_DISPLAY_NO_ATTRIBUTE,
     };
    //From HWComposer

    const int NUM_DISPLAY_ATTRIBUTES = (sizeof(DISPLAY_ATTRIBUTES)/sizeof(DISPLAY_ATTRIBUTES)[0]);

    for (size_t i = 0; i < NUM_DISPLAY_ATTRIBUTES - 1; i++) {
        switch (attributes[i]) {
        case HWC_DISPLAY_VSYNC_PERIOD:
            values[i] = pdev->dpyAttr[disp].vsync_period;
            break;
        case HWC_DISPLAY_WIDTH:
            values[i] = pdev->dpyAttr[disp].xres;
            ALOGD("%s disp = %d, width = %d",__FUNCTION__, disp,
                    pdev->dpyAttr[disp].xres);
            break;
        case HWC_DISPLAY_HEIGHT:
            values[i] = pdev->dpyAttr[disp].yres;
            ALOGD("%s disp = %d, height = %d",__FUNCTION__, disp,
                    pdev->dpyAttr[disp].yres);
            break;
        case HWC_DISPLAY_DPI_X:
            values[i] = (int32_t) (pdev->dpyAttr[disp].xdpi*1000.0);
            break;
        case HWC_DISPLAY_DPI_Y:
            values[i] = (int32_t) (pdev->dpyAttr[disp].ydpi*1000.0);
            break;
        case HWC_DISPLAY_NO_ATTRIBUTE:
            break;
        default:
            ALOGE("Unknown display attribute %d",
                    attributes[i]);
            return -EINVAL;
        }
    }

   return 0;
}

static int is_surport_wfd_optimize()
{
   char value[PROPERTY_VALUE_MAX];
   memset(value,0,PROPERTY_VALUE_MAX);
   property_get("drm.service.enabled", value, "false");
   if (!strcmp(value,"false"))
   {
     return false;
   }
   else
   {
     return true;
   }
}

static void hwc_dump(struct hwc_composer_device_1* dev, char *buff, int buff_len)
{
  // return 0;
}

int
hwc_device_open(
    const struct hw_module_t * module,
    const char * name,
    struct hw_device_t ** device
    )
{
    int  status    = 0;
    int rel;
    hwcContext * context = NULL;
    struct fb_fix_screeninfo fixInfo;
    struct fb_var_screeninfo info;
    int refreshRate = 0;
    float xdpi;
    float ydpi;
    uint32_t vsync_period;
    hw_module_t const* module_gr;
    int err;
    int stride_gr;
    int i;
    int xxx_w,xxx_h;
    LOGD("%s(%d):Open hwc device in thread=%d",
         __FUNCTION__, __LINE__, gettid());

    *device = NULL;

    if (strcmp(name, HWC_HARDWARE_COMPOSER) != 0)
    {
        LOGE("%s(%d):Invalid device name!", __FUNCTION__, __LINE__);
        return -EINVAL;
    }

    /* Get context. */
    context = _contextAnchor;

    /* Return if already initialized. */
    if (context != NULL)
    {
        /* Increament reference count. */
        context->reference++;

        *device = &context->device.common;
        return 0;
    }


    /* Allocate memory. */
    context = (hwcContext *) malloc(sizeof (hwcContext));

    if(context == NULL)
    {
        LOGE("%s(%d):malloc Failed!", __FUNCTION__, __LINE__);
        return -EINVAL;
    }
    memset(context, 0, sizeof (hwcContext));

#if 0//def TARGET_BOARD_PLATFORM_RK3399
    if(vop_init_devices(&context->vopctx))
        hwcONERROR(hwcRGA_OPEN_ERR);

    vop_dump(context->vopctx);
#endif
    context->fbFd = open("/dev/graphics/fb0", O_RDWR, 0);
    if(context->fbFd < 0)
    {
         hwcONERROR(hwcSTATUS_IO_ERR);
    }
#if USE_QUEUE_DDRFREQ
    context->ddrFd = open("/dev/ddr_freq", O_RDWR, 0);
    if(context->ddrFd < 0)
    {
         ALOGE("/dev/ddr_freq open failed !!!!!");
        // hwcONERROR(hwcSTATUS_IO_ERR);
    }
    else
    {
        ALOGD("context->ddrFd ok");
    }
#endif
    rel = ioctl(context->fbFd, RK_FBIOGET_IOMMU_STA, &context->iommuEn);
    if (rel != 0)
    {
         hwcONERROR(hwcSTATUS_IO_ERR);
    }

    rel = ioctl(context->fbFd, FBIOGET_FSCREENINFO, &fixInfo);
    if (rel != 0)
    {
         hwcONERROR(hwcSTATUS_IO_ERR);
    }

    if (ioctl(context->fbFd, FBIOGET_VSCREENINFO, &info) == -1)
    {
         hwcONERROR(hwcSTATUS_IO_ERR);
    }
    if (int(info.width) <= 0 || int(info.height) <= 0)
	{
		// the driver doesn't return that information
		// default to 160 dpi
		info.width  = ((info.xres * 25.4f)/160.0f + 0.5f);
		info.height = ((info.yres * 25.4f)/160.0f + 0.5f);
	}
    xdpi =  (info.xres * 25.4f) / info.width;
    ydpi =  (info.yres * 25.4f) / info.height;

    refreshRate = 1000000000000LLU /
    (
       uint64_t( info.upper_margin + info.lower_margin + info.yres )
       * ( info.left_margin  + info.right_margin + info.xres )
       * info.pixclock
     );

    if (refreshRate == 0) {
        ALOGW("invalid refresh rate, assuming 60 Hz");
        refreshRate = 60*1000;
    }

    vsync_period  = 1000000000 / refreshRate;

    context->fb_blanked = 1;
    context->dpyAttr[HWC_DISPLAY_PRIMARY].fd = context->fbFd;
    //xres, yres may not be 32 aligned
    context->dpyAttr[HWC_DISPLAY_PRIMARY].stride = fixInfo.line_length /(info.xres/8);
    context->dpyAttr[HWC_DISPLAY_PRIMARY].xres = info.xres;
    context->dpyAttr[HWC_DISPLAY_PRIMARY].yres = info.yres;
    context->dpyAttr[HWC_DISPLAY_PRIMARY].relxres = info.xres;
    context->dpyAttr[HWC_DISPLAY_PRIMARY].relyres = info.yres;
    context->dpyAttr[HWC_DISPLAY_PRIMARY].xdpi = xdpi;
    context->dpyAttr[HWC_DISPLAY_PRIMARY].ydpi = ydpi;
    context->dpyAttr[HWC_DISPLAY_PRIMARY].vsync_period = vsync_period;
    context->dpyAttr[HWC_DISPLAY_PRIMARY].connected = true;
    context->info = info;

    /* Initialize variables. */

    context->device.common.tag = HARDWARE_DEVICE_TAG;
    context->device.common.version = HWC_DEVICE_API_VERSION_1_3;

    context->device.common.module  = (hw_module_t *) module;

    /* initialize the procs */
    context->device.common.close   = hwc_device_close;
    context->device.prepare        = hwc_prepare;
    context->device.set            = hwc_set;
   // context->device.common.version = HWC_DEVICE_API_VERSION_1_0;
    context->device.blank          = hwc_blank;
    context->device.query          = hwc_query;
    context->device.eventControl   = hwc_event_control;

    context->device.registerProcs  = hwc_registerProcs;

    context->device.getDisplayConfigs = hwc_getDisplayConfigs;
    context->device.getDisplayAttributes = hwc_getDisplayAttributes;

    context->device.dump = hwc_dump;

    /* initialize params of video buffers*/
    for(i=0;i<MaxVideoBackBuffers;i++)
    {
        context->fd_video_bk[i] = -1;
        context->base_video_bk[i] = 0;
        context->pbvideo_bk[i] = NULL;
    }
    context->mCurVideoIndex= 0;

    context->mBootCnt = 0;
    context->mSkipFlag = 0;
    context->mVideoMode = false;
    context->mNV12_VIDEO_VideoMode = false;
    context->mIsMediaView = false;
    context->mVideoRotate = false;
    context->mGtsStatus   = false;
    context->mTrsfrmbyrga = false;
    context->mOneWinOpt = false;
    context->mLastCompType = -1;
    context->mContextIndex = 0;
#if GET_VPU_INTO_FROM_HEAD
    /* initialize params of video source info*/
    for(i=0;i<MAX_VIDEO_SOURCE;i++)
    {
        context->video_info[i].video_base = NULL;
        context->video_info[i].video_hd = NULL;
        context->video_info[i].bMatch=false;
    }
#endif

    /* Get gco2D object pointer. */
    context->engine_fd = open("/dev/rga",O_RDWR,0);
    if( context->engine_fd < 0)
    {
        //hwcONERROR(hwcRGA_OPEN_ERR);
        ALOGE("rga open err!");
    }

#if ENABLE_WFD_OPTIMIZE
	 property_set("sys.enable.wfd.optimize","1");
#endif
    if(context->engine_fd > -1)
    {
        int type = hwc_get_int_property("sys.enable.wfd.optimize","0");
        context->wfdOptimize = type;
        init_rga_cfg(context->engine_fd);
        if (type>0 && !is_surport_wfd_optimize())
        {
           property_set("sys.enable.wfd.optimize","0");
        }
    }

    /* Initialize pmem and frameubffer stuff. */
    // context->fbFd         = 0;
    // context->fbPhysical   = ~0U;
    // context->fbStride     = 0;

    if ( info.pixclock > 0 )
    {
        refreshRate = 1000000000000000LLU /
        (
            uint64_t( info.vsync_len + info.upper_margin + info.lower_margin + info.yres )
            * ( info.hsync_len + info.left_margin  + info.right_margin + info.xres )
            * info.pixclock
        );
    }
    else
    {
        ALOGW("fbdev pixclock is zero");
    }

    if (refreshRate == 0)
    {
        refreshRate = 60*1000;  // 60 Hz
    }

    context->fb_fps = refreshRate / 1000.0f;

    context->fbPhysical = fixInfo.smem_start;
    context->fbStride   = fixInfo.line_length;
	context->fbhandle.width = info.xres;
	context->fbhandle.height = info.yres;
#ifdef GPU_G6110
    #if G6110_SUPPORT_FBDC
    context->fbhandle.format = FBDC_ABGR_888;
    #else
    context->fbhandle.format = HAL_PIXEL_FORMAT_RGBA_8888;
    #endif
#else
    context->fbhandle.format = info.nonstd & 0xff;
#endif //end of GPU_G6110
    context->fbhandle.stride = (info.xres+ 31) & (~31);
    context->pmemPhysical = ~0U;
    context->pmemLength   = 0;
	//hwc_get_int_property("ro.rk.soc", "0");
    context->fbSize = info.xres*info.yres*4*3;
    context->lcdSize = info.xres*info.yres*4;

    mUsedVopNum = 1;
    context->mLcdcNum = 1;
    context->mHdmiSI.HdmiOn = false;
    context->mHdmiSI.NeedReDst = false;
    context->mHdmiSI.vh_flag = false;
    context->mIsBootanimExit = false;
    context->mIsFirstCallbackToHotplug = false;
    context->mIsDualViewMode = false;
#if DUAL_VIEW_MODE
    context->mIsDualViewMode = true;
#endif
    context->mComVop = true;
    context->mIsMipiDualOutMode = false;

#if HTGFORCEREFRESH
    init_thread_pamaters(&context->mRefresh);
#endif
    init_thread_pamaters(&context->mControlStereo);

#ifdef RK3288_BOX
    {
        int fd = -1;
        int ret = -1;
        char name[64];
        char value[10];
        const char node[] = "/sys/class/graphics/fb%u/lcdcid";
        for(unsigned int i = 0;i < 8 && context->mLcdcNum == 1;i++){
            snprintf(name, 64, node, i);
            fd = open(name,O_RDONLY,0);
            if(fd > 0){
                ret = read(fd,value,sizeof(value));
                if(ret < 0){
                    ALOGW("Get fb%d lcdcid fail:%s",i,strerror(errno));
                }else{
                    if(atoi(value)==1){
                        context->mLcdcNum = 2;
                        ALOGI("Get fb%d lcdcid=%d",i,atoi(value));
                    }
                }
                close(fd);
            }else{
                ALOGW("Open fb%d lcdcid fail:%s",i,strerror(errno));
            }
        }
    }
    if(context->mLcdcNum == 2){
        mUsedVopNum = 2;
    }
#endif
#if (defined(GPU_G6110) || defined(RK3288_BOX) || defined(RK3399_BOX))
    if(context->mLcdcNum == 1){
        context->screenFd = open("/sys/class/graphics/fb0/screen_info", O_RDONLY);
        if(context->screenFd <= 0){
            ALOGW("fb0 screen_info open fail for:%s",strerror(errno));
        }
    }else{
        context->screenFd = -1;
    }
#else
    context->screenFd = -1;
#endif
    err = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &module_gr);
    ALOGE_IF(err, "FATAL: can't find the %s module", GRALLOC_HARDWARE_MODULE_ID);
    if (err == 0) {
        gralloc_open(module_gr, &context->mAllocDev);

        memset(&bkupmanage,0,sizeof(hwbkupmanage));
        bkupmanage.dstwinNo = 0xff;
        bkupmanage.direct_fd=0;
    }
	else
	{
	    ALOGE(" GRALLOC_HARDWARE_MODULE_ID failed");
	}

#if OPTIMIZATION_FOR_DIMLAYER
    context->bHasDimLayer = false;
    err = context->mAllocDev->alloc(context->mAllocDev, context->fbhandle.width, \
                                    context->fbhandle.height,HAL_PIXEL_FORMAT_RGB_565, \
                                    GRALLOC_USAGE_HW_COMPOSER|GRALLOC_USAGE_HW_RENDER, \
                                    (buffer_handle_t*)(&(context->mDimHandle)),&stride_gr);
    if(!err){
        struct private_handle_t* handle = (struct private_handle_t*)context->mDimHandle;
        context->mDimFd = handle->share_fd;
#if defined(__arm64__) || defined(__aarch64__)
        context->mDimBase = (long)(GPU_BASE);
#else
        context->mDimBase = (int)(GPU_BASE);
#endif
        ALOGD("Dim buffer alloc fd [%dx%d,f=%d],fd=%d ",context->fbhandle.width,
            context->fbhandle.height,HAL_PIXEL_FORMAT_RGB_565,handle->share_fd);
    }
    else{
            ALOGE("Dim buffer alloc faild");
            goto OnError;
    }

    memset((void*)context->mDimBase,0x0,context->fbhandle.width*context->fbhandle.height*2);
#endif

    err = context->mAllocDev->alloc(context->mAllocDev, 32,32,HAL_PIXEL_FORMAT_RGB_565, \
                                    GRALLOC_USAGE_HW_COMPOSER|GRALLOC_USAGE_HW_RENDER, \
                                    (buffer_handle_t*)(&(context->mHdmiSI.FrameHandle)),&stride_gr);
    if(!err){
        struct private_handle_t* handle = (struct private_handle_t*)context->mHdmiSI.FrameHandle;
        context->mHdmiSI.FrameFd = handle->share_fd;
#if defined(__arm64__) || defined(__aarch64__)
        context->mHdmiSI.FrameBase = (long)(GPU_BASE);
#else
        context->mHdmiSI.FrameBase = (int)(GPU_BASE);
#endif
        ALOGD("Frame buffer alloc fd [32x32,f=%d],fd=%d ",HAL_PIXEL_FORMAT_RGB_565,handle->share_fd);
    }
    else{
        ALOGE("Frame buffer alloc faild");
        goto OnError;
    }
    memset((void*)context->mHdmiSI.FrameBase,0x00,32*32*2);

#if SPRITEOPTIMATION
    /*sprite*/
    for(i=0;i<3;i++)
    {
        err = context->mAllocDev->alloc(context->mAllocDev, BufferSize,BufferSize,HAL_PIXEL_FORMAT_RGBA_8888,\
                    GRALLOC_USAGE_HW_COMPOSER|GRALLOC_USAGE_HW_RENDER,\
                    (buffer_handle_t*)(&context->mSrBI.handle[i]),&stride_gr);
        if(!err){
            struct private_handle_t*handle = (struct private_handle_t*)context->mSrBI.handle[i];
            context->mSrBI.fd[i] = handle->share_fd;
#if defined(__arm64__) || defined(__aarch64__)
            context->mSrBI.hd_base[i] = (long)(GPU_BASE);
#else
            context->mSrBI.hd_base[i] = (int)(GPU_BASE);
#endif
            ALOGD("@hwc alloc[%d] [%dx%d,f=%d],[hande->type=%d],fd=%d",
                i,handle->width,handle->height,handle->format,handle->type,handle->share_fd);
        }else{
            ALOGE("hwc alloc[%d] faild",i);
            goto OnError;
        }
    }
#endif

#if defined(TARGET_BOARD_PLATFORM_RK3399) || defined(TARGET_BOARD_PLATFORM_RK3366)
    context->mComVop = is_common_vop(0/*hwc_device_open is open index of 0*/);
    if (context->mComVop)
        ALOGI("Primary is big vop type!!!!!");
#endif

    /* Increment reference count. */
    context->reference++;

    initPlatform(context);

    queryVopScreenMode(context);

    context->mIsMipiDualOutMode = context->vopDispMode == 2;

    init_every_to_skip_policy(context);

    if ((context->isRk3399 || context->isRk3366)) {
        if (context->mComVop && context->mIsMipiDualOutMode)
            init_big_vop_mipi_dual_out_policy(context);
        else if (!context->mComVop && context->mIsMipiDualOutMode)
            init_lite_vop_mipi_dual_out_policy(context);
        else if (context->mComVop)
            init_common_policy(context);
        else
            init_lite_vop_policy(context);
    } else {
        if (context->mIsMipiDualOutMode)
            init_big_vop_mipi_dual_out_policy(context);
        else
            init_common_policy(context);
    }

    if (context->isRk3399 || context->isRk3366)
        hwc_rga_blit_alloc_rects(context);

    _contextAnchor = context;
#if VIRTUAL_RGA_BLIT
    _contextAnchor2 = (hwcContext *) malloc(sizeof (hwcContext));
    memcpy((void*)_contextAnchor2,(void*)context,sizeof(hwcContext));
#endif
    if (context->fbhandle.width > context->fbhandle.height)
    {
        property_set("sys.display.oritation","0");
    }
    else
    {
        property_set("sys.display.oritation","2");
    }

#if USE_HW_VSYNC

    context->vsync_fd = open("/sys/class/graphics/fb0/vsync", O_RDONLY, 0);
    //context->vsync_fd = open("/sys/devices/platform/rk30-lcdc.0/vsync", O_RDONLY);
    if (context->vsync_fd < 0) {
        hwcONERROR(hwcSTATUS_IO_ERR);
    }

    if (pthread_mutex_init(&context->lock, NULL))
    {
        hwcONERROR(hwcMutex_ERR);
    }

    if (pthread_create(&context->hdmi_thread, NULL, hwc_thread, context))
    {
        hwcONERROR(hwcTHREAD_ERR);
    }
#endif

    /* Return device handle. */
    *device = &context->device.common;

    LOGD("RGA HWComposer verison%s\n"
         "Device:               %p\n"
         "fb_fps=%f",
         "1.0.0",
         context,
         context->fb_fps);

    hwc_init_version();

    char Version[32];

    if(context->engine_fd > 0) {
        memset(Version,0,sizeof(Version));
        if(ioctl(context->engine_fd, RGA_GET_VERSION, Version) == 0)
        {
            property_set("sys.grga.version",Version);
            LOGD(" rga version =%s",Version);

        }
    }
#ifdef TARGET_BOARD_PLATFORM_RK3368
    if(0 == hwc_get_int_property("ro.rk.soc", "0"))
        property_set("sys.rk.soc","rk3368");
#endif
    /*
    context->ippDev = new ipp_device_t();
    rel = ipp_open(context->ippDev);
    if (rel < 0)
    {
        delete context->ippDev;
        context->ippDev = NULL;
        ALOGE("Open ipp device fail.");
    }
    */
    init_hdmi_mode();

    pthread_t t;
    if (pthread_create(&t, NULL, rk_hwc_hdmi_thread, NULL))
    {
        LOGD("Create readHdmiMode thread error .");
    }
#if HWC_EXTERNAL
	pthread_t t0;
	if (pthread_create(&t0, NULL, hotplug_try_register, NULL))
    {
        LOGD("Create hotplug_try_register thread error .");
    }
#endif
#ifdef RK3288_BOX
    if(context->mLcdcNum == 2){
        hwc_change_config();
    }
#endif
    initCrcTable();
#if HTGFORCEREFRESH
    pthread_t t1;
    if (pthread_create(&t1, NULL, hotplug_invalidate_refresh, NULL))
    {
        LOGD("Create hotplug_invalidate_refresh thread error .");
    }
#endif
    signal(SIGALRM, hwc_static_screen_opt_handler);

    xxx_w =  hwc_get_int_property("sys.xxx.x_w","0");
    xxx_h =  hwc_get_int_property("sys.xxx.x_h","0");

    if(xxx_w && xxx_h)
        hwc_primary_screen_query();

    if (context->mIsMipiDualOutMode)
        hwc_primary_screen_query();

    return 0;

OnError:
#ifdef TARGET_BOARD_PLATFORM_RK3399
    if(context->vopctx)
    {
        vop_free_devices(&(context->vopctx));
    }
#endif
    if (context->vsync_fd > 0)
    {
        close(context->vsync_fd);
    }
    if(context->fbFd > 0)
    {
        close(context->fbFd );

    }
    if(context->fbFd1 > 0)
    {
        close(context->fbFd1 );
    }
    pthread_mutex_destroy(&context->lock);
#if HTGFORCEREFRESH
    free_thread_pamaters(&context->mRefresh);
#endif
    free_thread_pamaters(&context->mControlStereo);
    /* Error roll back. */
    if (context != NULL)
    {
        if (context->engine_fd > -1)
        {
            close(context->engine_fd);
        }
        free(context);

    }

    *device = NULL;
    LOGE("%s(%d):Failed!", __FUNCTION__, __LINE__);

    return -EINVAL;
}

int  getHdmiMode()
{
#if 0
    char pro_value[PROPERTY_VALUE_MAX];
    property_get("sys.hdmi.mode",pro_value,0);
    int mode = atoi(pro_value);
    return mode;
#else
    // LOGD("g_hdmi_mode=%d",g_hdmi_mode);
#endif
    // LOGD("g_hdmi_mode=%d",g_hdmi_mode);
    return g_hdmi_mode;
}

static int hwc_read_node(const char *intValue,char *outValue,int flag)
{
    int fd = -1;
    int ret = -1;
    size_t size = sizeof(outValue);
    fd = open(intValue, O_RDONLY);
    memset(outValue, 0, size);
    int err = read(fd, outValue, sizeof(outValue));
    if (err < 0){
        ALOGE("error reading vsync timestamp: %s", strerror(errno));
        return ret;
    }
    ret = atoi(outValue);
    close(fd);
    return ret;
}

void init_hdmi_mode()
{
#ifdef RK3288_BOX
    if(_contextAnchor->mLcdcNum == 2){
        int index = -1;
        int connect = -1;
        char outputValue[100];
        char inputValue[100] = "/sys/devices/virtual/display/HDMI/connect";
        connect = hwc_read_node(inputValue,outputValue,0);
        if(connect >= 0){
            memset(inputValue, 0, sizeof(inputValue));
            strcpy(inputValue,"/sys/devices/virtual/display/HDMI/property");
            index = hwc_read_node(inputValue,outputValue,0);
            ALOGD("%d,index=%d,connect=%d,hdmi=%d",__LINE__,index,connect,g_hdmi_mode);
            if(index == 1 && connect == 1){
                g_hdmi_mode = 1;
            }else if(index == 1){
                g_hdmi_mode = 0;
            }
        }
        index = -1;
        connect = -1;
        memset(inputValue, 0, sizeof(inputValue));
        strcpy(inputValue,"/sys/devices/virtual/display/HDMI1/connect");
        connect = hwc_read_node(inputValue,outputValue,0);
        if(connect >= 0){
            memset(inputValue, 0, sizeof(inputValue));
            strcpy(inputValue,"/sys/devices/virtual/display/HDMI1/property");
            index = hwc_read_node(inputValue,outputValue,0);
            ALOGD("%d,index=%d,connect=%d,hdmi=%d",__LINE__,index,connect,g_hdmi_mode);
            if(index == 1 && connect == 1){
                g_hdmi_mode = 1;
            }else if(index == 1){
                g_hdmi_mode = 0;
            }
        }
    }else {
        int fd = open("/sys/devices/virtual/switch/hdmi/state", O_RDONLY);
        if (fd > 0){
            char statebuf[100];
            memset(statebuf, 0, sizeof(statebuf));
            int err = read(fd, statebuf, sizeof(statebuf));
            if (err < 0){
                ALOGE("error reading vsync timestamp: %s", strerror(errno));
                return;
            }
            close(fd);
            g_hdmi_mode = atoi(statebuf);
        }else{
            LOGE("Open hdmi mode error.");
        }
    }
#else
    int fd = open("/sys/devices/virtual/switch/hdmi/state", O_RDONLY);
    if (fd > 0)
    {
        char statebuf[100];
        memset(statebuf, 0, sizeof(statebuf));
        int err = read(fd, statebuf, sizeof(statebuf));

        if (err < 0)
        {
            ALOGE("error reading vsync timestamp: %s", strerror(errno));
            return;
        }
        close(fd);
        g_hdmi_mode = atoi(statebuf);
        /* if (g_hdmi_mode==0)
        {
        property_set("sys.hdmi.mode", "0");
        }
        else
        {
        property_set("sys.hdmi.mode", "1");
        }*/
    }
    else
    {
        LOGE("Open hdmi mode error.");
    }

    if(g_hdmi_mode == 1)
    {
#ifdef GPU_G6110
        //hotplug_free_dimbuffer();
        //hotplug_get_config(0);
        //hotplug_set_config();
#endif
    }
#endif
}
int closeFb(int fd)
{
    if (fd > 0)
    {
        int disable = 0;

        if (ioctl(fd, 0x5019, &disable) == -1)
        {
            LOGE("close fb[%d] fail.",fd);
            return -1;
        }
        ALOGD("fb1 realy close!");
        return (close(fd));
    }
    return -1;
}
int hotplug_get_config(int flag){
    /*flag:0 hdmi;1 cvbs*/
    ALOGD("enter %s", __FUNCTION__);
    //memset(values, 0, sizeof(values));
    int fd;
    int err;
    int stride_gr;
    hwcContext *context = NULL;
    context = _contextAnchor1;
    if(context == NULL ){
        context = (hwcContext *) malloc(sizeof (hwcContext));
        if(context==NULL){
            ALOGE("hotplug_get_config:Alloc context fail");
            return -1;
        }
        memset(context, 0, sizeof (hwcContext));
    }
    struct fb_var_screeninfo info = _contextAnchor->info;
    int outX = 0;
    int outY = 0;
    hotplug_parse_mode(&outX, &outY);
#if defined(TARGET_BOARD_PLATFORM_RK3399) || defined(TARGET_BOARD_PLATFORM_RK3366)
    hwc_parse_screen_info(&outX, &outY);
#endif
    info.xres = outX;
    info.yres = outY;
    info.yres_virtual = info.yres * 3;
    info.xres_virtual = info.xres;
    info.grayscale = 0;
    info.grayscale |= info.xres<< 8;
    info.grayscale |= info.yres<<20;
#if (defined(GPU_G6110) || defined(RK3288_BOX))
#if defined(RK3288_BOX)
    if(_contextAnchor->mLcdcNum == 1){
	if(_contextAnchor->fbFd > 0){
            fd  =  _contextAnchor->fbFd;
        }else{
            fd  =  open("/dev/graphics/fb0", O_RDWR, 0);
        }
    }else{
    	if(context->fbFd > 0){
    	    fd  =  context->fbFd;
        }else{
            fd  =  open("/dev/graphics/fb4", O_RDWR, 0);
        }
    }
#else
    if(_contextAnchor->fbFd > 0){
        fd  =  _contextAnchor->fbFd;
    }else{
        fd  =  open("/dev/graphics/fb0", O_RDWR, 0);
    }
#endif
#else
#if defined(TARGET_BOARD_PLATFORM_RK3399) || defined(TARGET_BOARD_PLATFORM_RK3366)
    if(context->fbFd > 0){
	    fd  =  context->fbFd;
    }else{
        fd  =  open("/dev/graphics/fb5", O_RDWR, 0);
    }
#else
	if(context->fbFd > 0){
	    fd  =  context->fbFd;
    }else{
        fd  =  open("/dev/graphics/fb4", O_RDWR, 0);
    }
#endif
#endif
	if (fd < 0){
	    ALOGE("hotplug_get_config:open /dev/graphics/fb4 fail");
        return -errno;
	}
#if !(defined(GPU_G6110) || defined(TARGET_BOARD_PLATFORM_RK3399) || defined(TARGET_BOARD_PLATFORM_RK3366))
#ifdef RK3288_BOX
    if(_contextAnchor->mLcdcNum == 2){
        info.reserved[3] |= 1;
#endif
        info.reserved[3] |= 1;
    	if (ioctl(fd, FBIOPUT_VSCREENINFO, &info)){
    	    ALOGE("hotplug_get_config:FBIOPUT_VSCREENINFO error,hdmifd=%d",fd);
            return -errno;
    	}
#ifdef RK3288_BOX
    }
#endif
#endif
    context->fd_3d = _contextAnchor->fd_3d;
    if(context->fd_3d<=0){
        context->fd_3d = open("/sys/class/display/HDMI/3dmode", O_RDWR, 0);
        if(context->fd_3d < 0){
            ALOGE("open /sys/class/display/HDMI/3dmode fail");
        }
        _contextAnchor->fd_3d = context->fd_3d;
    }

#if RK_BOX
    if (flag == 1) {
        char buf[100];
        int width = 0;
        int height = 0;
        int fdExternal = -1;
#ifdef RK3288_BOX
        if(_contextAnchor->mLcdcNum == 2){
            fdExternal = open("/sys/class/graphics/fb4/screen_info", O_RDONLY);
        }else{
            fdExternal = open("/sys/class/graphics/fb0/screen_info", O_RDONLY);
        }
#elif defined(TARGET_BOARD_PLATFORM_RK3399) || defined(TARGET_BOARD_PLATFORM_RK3366)
        fdExternal = open("/sys/class/graphics/fb5/screen_info", O_RDONLY);
#else
        fdExternal = open("/sys/class/graphics/fb0/screen_info", O_RDONLY);
#endif
        if(fdExternal < 0){
            ALOGE("hotplug_get_config:open fb screen_info error,cvbsfd=%d",fdExternal);
            return -errno;
    	}
        if(read(fdExternal,buf,sizeof(buf)) < 0){
            ALOGE("error reading fb screen_info: %s", strerror(errno));
            return -1;
        }
        close(fdExternal);
		sscanf(buf,"xres:%d yres:%d",&width,&height);
        ALOGD("hotplug_get_config:width=%d,height=%d",width,height);
    	info.xres = width;
    	info.yres = height;
    }
#endif

#if USE_QUEUE_DDRFREQ
    context->ddrFd = _contextAnchor->fbFd;
#endif
    int refreshRate = 0;
	if ( info.pixclock > 0 ){
		refreshRate = 1000000000000000LLU /
		(
			uint64_t( info.vsync_len + info.upper_margin + info.lower_margin + info.yres )
			* ( info.hsync_len + info.left_margin  + info.right_margin + info.xres )
			* info.pixclock
		);
	}else{
		ALOGD( "fbdev pixclock is zero for fd: %d", fd );
	}

	if (refreshRate == 0){
		refreshRate = 60*1000;  // 60 Hz
	}
	struct fb_fix_screeninfo finfo;
	if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == -1){
	    ALOGE("FBIOGET_FSCREENINFO,hdmifd=%d",fd);
		return -errno;
	}
	if (int(info.width) <= 0 || int(info.height) <= 0){
		// the driver doesn't return that information
		// default to 160 dpi
		info.width  = ((info.xres * 25.4f)/160.0f + 0.5f);
		info.height = ((info.yres * 25.4f)/160.0f + 0.5f);
	}

	float xdpi = (info.xres * 25.4f) / info.width;
	float ydpi = (info.yres * 25.4f) / info.height;
	unsigned int vsync_period  = 1000000000 / refreshRate;

	context->dpyAttr[HWC_DISPLAY_EXTERNAL].fd = context->fbFd = fd;
    //xres, yres may not be 32 aligned
    context->dpyAttr[HWC_DISPLAY_EXTERNAL].stride = finfo.line_length /(info.xres/8);
    context->dpyAttr[HWC_DISPLAY_EXTERNAL].xres = info.xres;
    context->dpyAttr[HWC_DISPLAY_EXTERNAL].yres = info.yres;
    context->dpyAttr[HWC_DISPLAY_EXTERNAL].xdpi = xdpi;
    context->dpyAttr[HWC_DISPLAY_EXTERNAL].ydpi = ydpi;
    context->dpyAttr[HWC_DISPLAY_EXTERNAL].vsync_period = vsync_period;
    context->dpyAttr[HWC_DISPLAY_EXTERNAL].connected = true;
    context->info = info;
    context->mAllocDev = _contextAnchor->mAllocDev;

    /* initialize params of video buffers*/
    for(int i=0;i<MaxVideoBackBuffers;i++){
        context->fd_video_bk[i] = -1;
        context->base_video_bk[i] = 0;
        context->pbvideo_bk[i] = NULL;
    }
    context->mCurVideoIndex= 0;

	context->fb_blanked = 1;
    context->mSkipFlag = 0;
    context->mVideoMode = false;
    context->mNV12_VIDEO_VideoMode = false;
    context->mIsMediaView = false;
    context->mVideoRotate = false;
    context->mGtsStatus   = false;
    context->mTrsfrmbyrga = false;
    context->mOneWinOpt = false;
    context->mLastCompType = -1;
    context->mContextIndex = 1;

    context->fb_fps = refreshRate / 1000.0f;

    context->fbPhysical = finfo.smem_start;
    context->fbStride   = finfo.line_length;
	context->fbhandle.width = info.xres;
	context->fbhandle.height = info.yres;
#ifdef GPU_G6110
    #if G6110_SUPPORT_FBDC
    context->fbhandle.format = FBDC_ABGR_888;
    #else
    context->fbhandle.format = HAL_PIXEL_FORMAT_RGBA_8888;
    #endif
#else
    context->fbhandle.format = info.nonstd & 0xff;
#endif
    context->fbhandle.stride = (info.xres+ 31) & (~31);
    context->pmemPhysical = ~0U;
    context->pmemLength   = 0;
	//hwc_get_int_property("ro.rk.soc", "0");
    context->fbSize = info.xres*info.yres*4*3;
    context->lcdSize = info.xres*info.yres*4;

#if OPTIMIZATION_FOR_DIMLAYER
    err = _contextAnchor->mAllocDev->alloc(_contextAnchor->mAllocDev, context->fbhandle.width, \
                                    context->fbhandle.height,HAL_PIXEL_FORMAT_RGB_565, \
                                    GRALLOC_USAGE_HW_COMPOSER|GRALLOC_USAGE_HW_RENDER, \
                                    (buffer_handle_t*)(&(context->mDimHandle)),&stride_gr);
    if(!err){
        struct private_handle_t*handle = (struct private_handle_t*)context->mDimHandle;
        context->mDimFd = handle->share_fd;
#if defined(__arm64__) || defined(__aarch64__)
        context->mDimBase = (long)(GPU_BASE);
#else
        context->mDimBase = (int)(GPU_BASE);
#endif
        ALOGD("Dim buffer alloc fd [%dx%d,f=%d],fd=%d ",context->fbhandle.width,context->fbhandle.height,HAL_PIXEL_FORMAT_RGB_565,handle->share_fd);

    }else{
        ALOGE("Dim buffer alloc faild");
        goto OnError;
    }

    memset((void*)context->mDimBase,0x0,context->fbhandle.width*context->fbhandle.height*2);
#endif
#if SPRITEOPTIMATION
    /*sprite*/
    for(int i=0;i<MaxSpriteBNUM;i++){
        if(_contextAnchor->mSrBI.handle[i]){
            context->mSrBI.fd[i]      = _contextAnchor->mSrBI.fd[i];
            context->mSrBI.hd_base[i] = _contextAnchor->mSrBI.hd_base[i];
            context->mSrBI.handle[i]  = _contextAnchor->mSrBI.handle[i];
        }else{
            err = context->mAllocDev->alloc(context->mAllocDev, BufferSize,BufferSize,HAL_PIXEL_FORMAT_RGBA_8888,\
                GRALLOC_USAGE_HW_COMPOSER|GRALLOC_USAGE_HW_RENDER,\
                (buffer_handle_t*)(&context->mSrBI.handle[i]),&stride_gr);
            if(!err){
                struct private_handle_t*handle = (struct private_handle_t*)context->mSrBI.handle[i];
                context->mSrBI.fd[i] = handle->share_fd;
#if defined(__arm64__) || defined(__aarch64__)
                context->mSrBI.hd_base[i] = (long)(GPU_BASE);
#else
                context->mSrBI.hd_base[i] = (int)(GPU_BASE);
#endif
                _contextAnchor->mSrBI.fd[i]      = context->mSrBI.fd[i];
                _contextAnchor->mSrBI.hd_base[i] = context->mSrBI.hd_base[i];
                _contextAnchor->mSrBI.handle[i]  = context->mSrBI.handle[i];
                ALOGD("@hwc alloc[%d] [%dx%d,f=%d],fd=%d ",
                    i,handle->width,handle->height,handle->format,handle->share_fd);
            }else{
                ALOGE("hwc alloc[%d] faild",i);
                goto OnError;
            }
        }
    }
    context->mSrBI.mCurIndex = 0;
#endif

    context->mComVop = true;
#if defined(TARGET_BOARD_PLATFORM_RK3399) || defined(TARGET_BOARD_PLATFORM_RK3366)
    context->mComVop = is_common_vop(1/*hwc_device_open is open index of 0*/);
    if (context->mComVop)
        ALOGI("Externel is big vop type!!!!!");
#endif

    initPlatform(context);

    queryVopScreenMode(context);

    context->mIsMipiDualOutMode = context->vopDispMode == 2;

    init_every_to_skip_policy(context);

    if ((context->isRk3399 || context->isRk3366)) {
        if (context->mComVop && context->mIsMipiDualOutMode)
            init_big_vop_mipi_dual_out_policy(context);
        else if (!context->mComVop && context->mIsMipiDualOutMode)
            init_lite_vop_mipi_dual_out_policy(context);
        else if (context->mComVop)
            init_common_policy(context);
        else
            init_lite_vop_policy(context);
    } else {
        if (context->mIsMipiDualOutMode)
            init_big_vop_mipi_dual_out_policy(context);
        else
            init_common_policy(context);
    }

    if (context->isRk3399 || context->isRk3366)
        hwc_rga_blit_alloc_rects(context);

    _contextAnchor1 = context;
#if !(defined(GPU_G6110) || defined(RK3399_BOX))
#ifdef RK3288_BOX
    if(_contextAnchor->mLcdcNum == 2)
#endif
    {
        hotplug_set_frame(_contextAnchor,0);
    }
#endif

    return 1;

OnError:

    if (context->vsync_fd > 0)
    {
        close(context->vsync_fd);
    }
    if(context->fbFd > 0)
    {
        close(context->fbFd );
    }
    if(context->fbFd1 > 0)
    {
        close(context->fbFd1 );
    }

#if OPTIMIZATION_FOR_DIMLAYER
    if(context->mDimHandle)
    {
        err = _contextAnchor->mAllocDev->free(_contextAnchor->mAllocDev, context->mDimHandle);
        ALOGW_IF(err, "free mDimHandle (...) failed %d (%s)", err, strerror(-err));
    }
#endif
    pthread_mutex_destroy(&context->lock);

    /* Error roll back. */
    if (context != NULL)
    {
        if (context->engine_fd > -1)
        {
            close(context->engine_fd);
        }
        free(context);
    }
    _contextAnchor1 = NULL;
    LOGE("%s(%d):Failed!", __FUNCTION__, __LINE__);

    return -EINVAL;

}

int hwc_parse_screen_info(int *outX, int *outY)
{
    char buf[100];
    int width = 0;
    int height = 0;
    int fdExternal = -1;
	fdExternal = open("/sys/class/graphics/fb5/screen_info", O_RDONLY);
    if(fdExternal < 0){
        ALOGE("hotplug_get_config:open fb screen_info error,cvbsfd=%d",fdExternal);
        return -errno;
	}
    if(read(fdExternal,buf,sizeof(buf)) < 0){
        ALOGE("error reading fb screen_info: %s", strerror(errno));
        return -1;
    }
    close(fdExternal);
	sscanf(buf,"xres:%d yres:%d",&width,&height);
    ALOGD("hotplug_get_config:width=%d,height=%d",width,height);
	*outX = width;
	*outY = height;
	return 0;
}

int hotplug_parse_mode(int *outX, int *outY)
{
   int fd = open("/sys/class/display/HDMI/mode", O_RDONLY);
   ALOGD("enter %s", __FUNCTION__);

   if (fd > 0) {
        char statebuf[100];
        memset(statebuf, 0, sizeof(statebuf));
        int err = read(fd, statebuf, sizeof(statebuf));
        if (err < 0) {
            ALOGE("error reading hdmi mode: %s", strerror(errno));
            return -1;
        }
        //ALOGD("statebuf=%s",statebuf);
        close(fd);
        char xres[10];
        char yres[10];
        int temp = 0;
        memset(xres, 0, sizeof(xres));
        memset(yres, 0, sizeof(yres));
        for (unsigned int i=0; i<strlen(statebuf); i++) {
            if (statebuf[i] >= '0' && statebuf[i] <= '9') {
                xres[i] = statebuf[i];
            } else {
                temp = i;
                break;
            }
        }
        int m = 0;
        for (unsigned int j=temp+1; j<strlen(statebuf);j++) {
            if (statebuf[j] >= '0' && statebuf[j] <= '9') {
                yres[m] = statebuf[j];
                m++;
            } else {
                break;
            }
        }
        *outX = atoi(xres);
        *outY = atoi(yres);
        close(fd);
        return 0;
    } else {
        close(fd);
        ALOGE("Get HDMI mode fail");
        return -1;
    }
}

int hotplug_set_config()
{
    int dType = HWC_DISPLAY_EXTERNAL;
    hwcContext * ctxp = _contextAnchor;
    hwcContext * ctxe = _contextAnchor1;
    if (ctxe != NULL) {
        ctxp->dpyAttr[dType].fd           = ctxe->dpyAttr[dType].fd;
        ctxp->dpyAttr[dType].stride       = ctxe->dpyAttr[dType].stride;
        ctxp->dpyAttr[dType].xres         = ctxe->dpyAttr[dType].xres;
        ctxp->dpyAttr[dType].yres         = ctxe->dpyAttr[dType].yres;
        ctxp->dpyAttr[dType].xdpi         = ctxe->dpyAttr[dType].xdpi;
        ctxp->dpyAttr[dType].ydpi         = ctxe->dpyAttr[dType].ydpi;
        ctxp->dpyAttr[dType].vsync_period = ctxe->dpyAttr[dType].vsync_period;
        ctxp->dpyAttr[dType].connected    = true;
        if (ctxp->mIsDualViewMode) {
            ctxp->dpyAttr[dType].xres = ctxe->dpyAttr[dType].xres * 2;
            ctxp->dpyAttr[dType].yres = ctxe->dpyAttr[dType].yres;
            ctxe->mIsDualViewMode = true;
            LOGV("w_s,h_s,w_d,h_d = [%d,%d,%d,%d]",
                ctxp->dpyAttr[dType].xres,ctxp->dpyAttr[dType].yres,
                ctxe->dpyAttr[dType].xres,ctxe->dpyAttr[dType].yres);
	    } else if (ctxe->mIsMipiDualOutMode) {
            ctxp->dpyAttr[dType].xres /= 2;
            ctxp->dpyAttr[dType].yres *= 2;
            LOGV("w_s,h_s,w_d,h_d = [%d,%d,%d,%d]",
                ctxp->dpyAttr[dType].xres,ctxp->dpyAttr[dType].yres,
                ctxe->dpyAttr[dType].xres,ctxe->dpyAttr[dType].yres);
        } else if (ctxp->dpyAttr[dType].yres > 1080 && ctxp->dpyAttr[dType].xres > ctxp->dpyAttr[dType].yres) {
            ctxp->dpyAttr[dType].yres = 1080;
            ctxp->dpyAttr[dType].xres = 1920 * ctxp->dpyAttr[dType].yres / ctxe->dpyAttr[dType].yres;
            ctxp->mHdmiSI.NeedReDst = true;
            LOGV("w_s,h_s,w_d,h_d = [%d,%d,%d,%d]",
                ctxp->dpyAttr[dType].xres,ctxp->dpyAttr[dType].yres,
                ctxe->dpyAttr[dType].xres,ctxe->dpyAttr[dType].yres);
        } else {
            ctxp->mIsDualViewMode = false;
            ctxp->mHdmiSI.NeedReDst = false;
        }
        return 0;
    } else {
        ctxp->dpyAttr[dType].connected = false;
        ctxp->mIsDualViewMode = false;
        ALOGE("hotplug_set_config fail,ctxe is null");
        return -ENODEV;
    }
}

void hotplug_get_resolution(int* w,int* h)
{
    *w = int(_contextAnchor->dpyAttr[HWC_DISPLAY_EXTERNAL].xres);
    *h = int(_contextAnchor->dpyAttr[HWC_DISPLAY_EXTERNAL].yres);
}

static int hotplug_close_device()
{
    int i;
    int err=0;
    hwcContext * context = _contextAnchor1;

    if(context->engine_fd > -1)
        close(context->engine_fd);
    /* Clean context. */
    if(context->vsync_fd > 0)
        close(context->vsync_fd);
    if(context->fbFd > 0)
    {
        close(context->fbFd );
    }
    if(context->fbFd1 > 0)
    {
        close(context->fbFd1 );
    }

    // free video gralloc buffer
    for(i=0;i<MaxVideoBackBuffers;i++)
    {
        if(context->pbvideo_bk[i] != NULL)
            err = context->mAllocDev->free(context->mAllocDev, context->pbvideo_bk[i]);
        if(!err)
        {
            context->fd_video_bk[i] = -1;
            context->base_video_bk[i] = 0;
            context->pbvideo_bk[i] = NULL;
        }
        ALOGW_IF(err, "free pbvideo_bk (...) failed %d (%s)", err, strerror(-err));
    }

#if OPTIMIZATION_FOR_DIMLAYER
    if(context->mDimHandle)
    {
        err = context->mAllocDev->free(context->mAllocDev, context->mDimHandle);
        ALOGW_IF(err, "free mDimHandle (...) failed %d (%s)", err, strerror(-err));
    }
#endif

    hwc_rga_blit_free_rects(context);

    pthread_mutex_destroy(&context->lock);
    free(context);
    _contextAnchor1 = NULL;
    return 0;
}

void *hotplug_try_register(void *)
{
    prctl(PR_SET_NAME,"HWC_htg1");
    hwcContext * context = _contextAnchor;
    int count = 0;

    if (context->isRk3399/* && context->isBox*/) {
        hwc_change_screen_config(0, 0, 1);
        if (is_vop_connected(HWC_DISPLAY_EXTERNAL/**/))
            handle_hotplug_event(1, 6);
        goto READY;
    }

#ifndef RK3368_BOX
#if RK3288_BOX
    if(context->mLcdcNum == 2)
#endif
    {
        if(getHdmiMode() == 1){
            hotplug_free_dimbuffer();
            hotplug_get_config(0);
        }
    }
#endif
    while(context->fb_blanked){
        count++;
        usleep(10000);
        if(300==count){
            ALOGW("wait for primary unblank");
            break;
        }
    }
    if(getHdmiMode() == 1){
        handle_hotplug_event(1, 6);
		ALOGI("hotplug_try_register at line = %d",__LINE__);
    }else{
#if (defined(RK3368_BOX) || defined(RK3288_BOX) || defined(RK3399_BOX))
#if RK3288_BOX
        if(context->mLcdcNum == 1){
            handle_hotplug_event(1, 1);
            ALOGI("hotplug_try_register at line = %d",__LINE__);
        }
#else
        handle_hotplug_event(1, 1);
        ALOGI("hotplug_try_register at line = %d",__LINE__);
#endif
#endif
    }
#if (defined(GPU_G6110) || defined(RK3288_BOX) || defined(RK3399_BOX))
#if RK3288_BOX
    if(context->mLcdcNum == 2){
        goto READY;
    }
#endif
    while(!context->mIsBootanimExit){
        int i = 0;
        char value[PROPERTY_VALUE_MAX];
        property_get("service.bootanim.exit",value,"0");
        i = atoi(value);
        if(1==i){
            context->mIsBootanimExit = true;
            context->mIsFirstCallbackToHotplug = true;
        }else{
            usleep(30000);
        }
    }
    if(getHdmiMode() == 1){
        handle_hotplug_event(1, 6);
		ALOGI("hotplug_try_register at line = %d",__LINE__);
#ifndef RK3368_MID
    }else{
        handle_hotplug_event(1, 1);
		ALOGI("hotplug_try_register at line = %d",__LINE__);
#endif
    }
#endif

READY:
    pthread_exit(NULL);
    return NULL;
}

int hotplug_set_overscan(int flag)
{
    char new_valuep[PROPERTY_VALUE_MAX];
    char new_valuee[PROPERTY_VALUE_MAX];

    switch(flag){
    case 0:
        property_get("persist.sys.overscan.main", new_valuep, "false");
        property_get("persist.sys.overscan.aux",  new_valuee, "false");
        break;

    case 1:
        strcpy(new_valuep,"overscan 100,100,100,100");
        strcpy(new_valuee,"overscan 100,100,100,100");
        break;

    default:
        break;
    }

    int fdp = open("/sys/class/graphics/fb0/scale",O_RDWR);
    if(fdp > 0){
        int ret = write(fdp,new_valuep,sizeof(new_valuep));
        if(ret != sizeof(new_valuep)){
            ALOGE("write /sys/class/graphics/fb0/scale fail");
            close(fdp);
            return -1;
        }
        ALOGV("new_valuep=[%s]",new_valuep);
        close(fdp);
    }
#ifdef RK3288_BOX
    if(_contextAnchor->mLcdcNum == 2){
        int fde = open("/sys/class/graphics/fb4/scale",O_RDWR);
        if(fde > 0){
            int ret = write(fde,new_valuee,sizeof(new_valuee));
            if(ret != sizeof(new_valuee)){
                ALOGE("write /sys/class/graphics/fb4/scale fail");
                close(fde);
                return -1;
            }
            ALOGV("new_valuep=[%s]",new_valuee);
            close(fde);
        }
    }
#endif
    return 0;
}

int hotplug_reset_dstposition(struct rk_fb_win_cfg_data * fb_info,int flag)
{
    /*flag:HDMI hotplug has two situation
    *1:
    *0:mHdmiSI.NeedReDst case hotplug 1080p when 4k
    */
    hwcContext *context = _contextAnchor;
    char buf[100];
    int fd = context->screenFd;
    unsigned int w_source = 0;
    unsigned int h_source = 0;
    unsigned int w_dst = 0;
    unsigned int h_dst = 0;
    unsigned int w_hotplug = 0;
    unsigned int h_hotplug = 0;
    if (fb_info == NULL) {
        return -1;
    }

    if (flag != 2 && _contextAnchor1 == NULL) {
        return -1;
    }

    //ALOGD("%s,%d",__FUNCTION__,__LINE__);
    switch(flag){
    case 0:
        w_source = context->dpyAttr[HWC_DISPLAY_EXTERNAL].xres;
        h_source = context->dpyAttr[HWC_DISPLAY_EXTERNAL].yres;
        w_dst    = _contextAnchor1->dpyAttr[HWC_DISPLAY_EXTERNAL].xres;
        h_dst    = _contextAnchor1->dpyAttr[HWC_DISPLAY_EXTERNAL].yres;
        break;

    case 1:
        w_source = context->dpyAttr[HWC_DISPLAY_PRIMARY].xres;
        h_source = context->dpyAttr[HWC_DISPLAY_PRIMARY].yres;
        w_hotplug = context->dpyAttr[HWC_DISPLAY_EXTERNAL].xres;
        h_hotplug = context->dpyAttr[HWC_DISPLAY_EXTERNAL].yres;
        lseek(fd,0,SEEK_SET);
        if (read(fd,buf,sizeof(buf)) < 0) {
            ALOGE("error reading fb screen_info:%d,%s",fd,strerror(errno));
            return -1;
        }
		sscanf(buf,"xres:%d yres:%d",&w_dst,&h_dst);
        ALOGD_IF(log(HLLONE),"width=%d,height=%d",w_dst,h_dst);
        break;

    case 2:
        w_source = context->dpyAttr[HWC_DISPLAY_PRIMARY].xres;
        h_source = context->dpyAttr[HWC_DISPLAY_PRIMARY].yres;
        w_dst    = context->dpyAttr[HWC_DISPLAY_PRIMARY].relxres;
        h_dst    = context->dpyAttr[HWC_DISPLAY_PRIMARY].relyres;
        break;

    default:
        break;
    }

    float w_scale = (float)w_dst / w_source;
    float h_scale = (float)h_dst / h_source;

    if (h_source != h_dst) {
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                bool isNeedReset = fb_info->win_par[i].area_par[j].ion_fd != 0;
                isNeedReset = isNeedReset || fb_info->win_par[i].area_par[j].phy_addr;
                isNeedReset = isNeedReset && fb_info->win_par[i].area_par[j].reserved0 == 0;

                if (isNeedReset && fb_info->win_par[i].win_id > 1) {
                    ALOGD_IF(log(HLLONE),"Is changing %d",__LINE__);
                    continue;
		}

                if (isNeedReset) {
                    fb_info->win_par[i].area_par[j].xpos  =
                        (unsigned short)(fb_info->win_par[i].area_par[j].xpos * w_scale);
                    fb_info->win_par[i].area_par[j].ypos  =
                        (unsigned short)(fb_info->win_par[i].area_par[j].ypos * h_scale);
                    fb_info->win_par[i].area_par[j].xsize =
                        (unsigned short)(fb_info->win_par[i].area_par[j].xsize * w_scale);
                    fb_info->win_par[i].area_par[j].ysize =
                        (unsigned short)(fb_info->win_par[i].area_par[j].ysize * h_scale);
                    ALOGD_IF(log(HLLONE),"Adjust dst to => [%d,%d,%d,%d][%d,%d,%d,%d]",
                        fb_info->win_par[i].area_par[j].xpos,fb_info->win_par[i].area_par[j].ypos,
                        fb_info->win_par[i].area_par[j].xsize,fb_info->win_par[i].area_par[j].ysize,
                        w_source, h_source, w_dst, h_dst);
                }
            }
        }
    }
    return 0;
}

int hotplug_set_frame(hwcContext* context,int flag)
{
    int ret = 0;
    struct rk_fb_win_cfg_data fb_info;
    memset(&fb_info,0,sizeof(fb_info));
    fb_info.ret_fence_fd = -1;
    for(int i=0;i<RK_MAX_BUF_NUM;i++) {
        fb_info.rel_fence_fd[i] = -1;
    }

    fb_info.win_par[0].area_par[0].data_format = HAL_PIXEL_FORMAT_RGB_565;
    fb_info.win_par[0].win_id = 0;
    fb_info.win_par[0].z_order = 0;
    fb_info.win_par[0].area_par[0].ion_fd = context->mHdmiSI.FrameFd;
    fb_info.win_par[0].area_par[0].acq_fence_fd = -1;
    fb_info.win_par[0].area_par[0].x_offset = 0;
    fb_info.win_par[0].area_par[0].y_offset = 0;
    fb_info.win_par[0].area_par[0].xpos = 0;
    fb_info.win_par[0].area_par[0].ypos = 0;
    fb_info.win_par[0].area_par[0].xsize = 32;
    fb_info.win_par[0].area_par[0].ysize = 32;
    fb_info.win_par[0].area_par[0].xact = 32;
    fb_info.win_par[0].area_par[0].yact = 32;
    fb_info.win_par[0].area_par[0].xvir = 32;
    fb_info.win_par[0].area_par[0].yvir = 32;
    fb_info.wait_fs = 0;

    if(ioctl(_contextAnchor1->fbFd, RK_FBIOSET_CONFIG_DONE, &fb_info) == -1){
        ALOGE("%s,%d,RK_FBIOSET_CONFIG_DONE fail",__FUNCTION__,__LINE__);
    }else{
        ALOGD_IF(log(HLLONE),"hotplug_set_frame");
    }

    for(int k=0;k<RK_MAX_BUF_NUM;k++){
        //ALOGD("%s,%d,fb_info.rel_fence_fd[%d]=%d",
            //__FUNCTION__,__LINE__,k,fb_info.rel_fence_fd[k]);
        if(fb_info.rel_fence_fd[k] >=0 ){
            ret = 1;
            close(fb_info.rel_fence_fd[k]);
        }
    }
    //ALOGD("%s,%d,fb_info.ret_fence_fd=%d",
        //__FUNCTION__,__LINE__,fb_info.ret_fence_fd);
    if(fb_info.ret_fence_fd >= 0){
        ret = 1;
        close(fb_info.ret_fence_fd);
    }
    return ret;
}

void  *hotplug_invalidate_refresh(void *arg)
{
    int count = 0;
    int nMaxCnt = 25;
    unsigned int nSleepTime = 200;
    hwcContext *contextp = _contextAnchor;
    ALOGD("hotplug_invalidate_refresh creat");
#if HTGFORCEREFRESH
    pthread_cond_wait(&contextp->mRefresh.cond,&contextp->mRefresh.mtx);
    while(true) {
        for(count = 0; count < nMaxCnt; count++) {
            usleep(nSleepTime*1000);
            pthread_mutex_lock(&contextp->mRefresh.mlk);
            count = contextp->mRefresh.count;
            contextp->mRefresh.count ++;
            ALOGD_IF(log(HLLTWO),"mRefresh.count=%d",contextp->mRefresh.count);
            pthread_mutex_unlock(&contextp->mRefresh.mlk);
            contextp->procs->invalidate(contextp->procs);
        }
        pthread_cond_wait(&contextp->mRefresh.cond,&contextp->mRefresh.mtx);
        count = 0;
    }
#endif
    ALOGD("hotplug_invalidate_refresh exit");
    pthread_exit(NULL);
    return NULL;
}

int hwc_sprite_replace(hwcContext * Context,hwc_display_contents_1_t * list)
{
#if SPRITEOPTIMATION
#if (defined(RK3368_BOX))// || defined(RK3288_BOX))
    ATRACE_CALL();
    if(Context == _contextAnchor)
        return 0;

    ZoneInfo mZoneInfo;
    ZoneManager* pzone_mag = &Context->zone_manager;
    int i = pzone_mag->zone_cnt-1;//Must be Sprite if has
    memcpy(&mZoneInfo,&pzone_mag->zone_info[i],sizeof(ZoneInfo));

    if(pzone_mag->zone_info[i].zone_err || pzone_mag->zone_info[i].transform)
        return -1;

    int mSize = 64;
    if(_contextAnchor->mHdmiSI.NeedReDst){
        mSize = 128;
        return -1;
    }

    int width=0,height=0;
	int xpos,ypos;
	int x_offset,y_offset;
    RECT clip;
    int Rotation = 0;
    unsigned char RotateMode = 1;
    struct rga_req  Rga_Request;
    int SrcVirW,SrcVirH,SrcActW,SrcActH;
    int DstVirW,DstVirH,DstActW,DstActH;
    int xoffset;
    int yoffset;
    int fd_dst = Context->mSrBI.fd[Context->mSrBI.mCurIndex];
    int Dstfmt = hwChangeRgaFormat(HAL_PIXEL_FORMAT_RGBA_8888);
    int rga_fd = _contextAnchor->engine_fd;
    hwcContext * context = _contextAnchor;

    if (!rga_fd)
        return -1;

    DstActW = mZoneInfo.disp_rect.right  - mZoneInfo.disp_rect.left;
    DstActH = mZoneInfo.disp_rect.bottom - mZoneInfo.disp_rect.top;

    if(mSize < DstActW || mSize < DstActH)
        mSize = 128;

    if(mSize < DstActW || mSize < DstActH)
        return -1;

    DstVirW = mSize;
    DstVirH = mSize;

    if(Context==_contextAnchor){
        width  = Context->dpyAttr[0].xres;
        height = Context->dpyAttr[0].yres;
    }else if(Context==_contextAnchor1){
		width  = Context->dpyAttr[1].xres;
        height = Context->dpyAttr[1].yres;
	}

    struct private_handle_t *handle = mZoneInfo.handle;
    if (!handle)
        return -1;

    if(mZoneInfo.disp_rect.left <= width - mZoneInfo.disp_rect.right)
        xpos = mZoneInfo.disp_rect.left;
    else
		xpos = mZoneInfo.disp_rect.right - mSize;
	if(mZoneInfo.disp_rect.top <= height - mZoneInfo.disp_rect.bottom)
        ypos = mZoneInfo.disp_rect.top;
    else
		ypos = mZoneInfo.disp_rect.bottom - mSize;

	xoffset = mZoneInfo.disp_rect.left - xpos;
	yoffset = mZoneInfo.disp_rect.top  - ypos;

    x_offset = mZoneInfo.src_rect.left;
    y_offset = mZoneInfo.src_rect.top;

    SrcVirW = handle->stride;
    SrcVirH = handle->height;
    SrcActW = mZoneInfo.src_rect.right - mZoneInfo.src_rect.left;
    SrcActH = mZoneInfo.src_rect.bottom - mZoneInfo.src_rect.top;
    SrcActW = SrcActW<16?(SrcActW+SrcActW%2):(SrcActW);
    SrcActH = SrcActH<16?(SrcActH+SrcActH%2):(SrcActH);

    mZoneInfo.stride = (mSize + 31)&(~31);
    mZoneInfo.width = mSize;
    mZoneInfo.height = mSize;
    mZoneInfo.disp_rect.left   = xpos;
    mZoneInfo.disp_rect.right  = xpos + mSize;
    mZoneInfo.disp_rect.top    = ypos;
    mZoneInfo.disp_rect.bottom = ypos + mSize;
    mZoneInfo.src_rect.left    = 0;
    mZoneInfo.src_rect.right   = mSize;
    mZoneInfo.src_rect.top     = 0;
    mZoneInfo.src_rect.bottom  = mSize;
    mZoneInfo.layer_fd = Context->mSrBI.fd[Context->mSrBI.mCurIndex];

    if(SrcVirW<=0 || SrcVirH<=0 || SrcActW<=0 || SrcActH<=0)
        return -1;

    if(DstVirW<=0 || DstVirH<=0 || DstActW<=0 || DstActH<=0)
        return -1;
    memcpy(&pzone_mag->zone_info[i],&mZoneInfo,sizeof(ZoneInfo));
    memset((void*)(Context->mSrBI.hd_base[Context->mSrBI.mCurIndex]),0x0,mSize*mSize*4);
    ALOGD_IF(log(HLLTWO),"Sprite Zone[%d]->layer[%d],"
        "[%d,%d,%d,%d] =>[%d,%d,%d,%d],"
        "w_h_s_f[%d,%d,%d,%d],tr_rtr_bled[%d,%d,%d],acq_fence_fd=%d,",
        Context->zone_manager.zone_info[i].zone_index,
        Context->zone_manager.zone_info[i].layer_index,
        Context->zone_manager.zone_info[i].src_rect.left,
        Context->zone_manager.zone_info[i].src_rect.top,
        Context->zone_manager.zone_info[i].src_rect.right,
        Context->zone_manager.zone_info[i].src_rect.bottom,
        Context->zone_manager.zone_info[i].disp_rect.left,
        Context->zone_manager.zone_info[i].disp_rect.top,
        Context->zone_manager.zone_info[i].disp_rect.right,
        Context->zone_manager.zone_info[i].disp_rect.bottom,
        Context->zone_manager.zone_info[i].width,
        Context->zone_manager.zone_info[i].height,
        Context->zone_manager.zone_info[i].stride,
        Context->zone_manager.zone_info[i].format,
        Context->zone_manager.zone_info[i].transform,
        Context->zone_manager.zone_info[i].realtransform,
        Context->zone_manager.zone_info[i].blend,
        Context->zone_manager.zone_info[i].acq_fence_fd);

    memset(&Rga_Request, 0x0, sizeof(Rga_Request));

    clip.xmin = 0;
    clip.xmax = mSize-1;
    clip.ymin = 0;
    clip.ymax = mSize-1;

    ALOGD_IF(log(HLLTWO),"src addr=[%x],handle type=[%d],w-h[%d,%d],act[%d,%d],off[%d,%d][f=%d]",
        handle->share_fd,handle->type,SrcVirW, SrcVirH,SrcActW,SrcActH,x_offset,y_offset,hwChangeRgaFormat(handle->format));
    ALOGD_IF(log(HLLTWO),"dst fd=[%x],w-h[%d,%d],act[%d,%d],off[%d,%d][f=%d],rot=%d,rot_mod=%d",
        fd_dst, DstVirW, DstVirH,DstActW,DstActH,xoffset,yoffset,Dstfmt,Rotation,RotateMode);

    RGA_set_src_vir_info(&Rga_Request, handle->share_fd, 0, 0,SrcVirW, SrcVirH, hwChangeRgaFormat(handle->format), 0);
    RGA_set_dst_vir_info(&Rga_Request, fd_dst, 0, 0,DstVirW,DstVirH,&clip, Dstfmt, 0);
    RGA_set_bitblt_mode(&Rga_Request, 0, RotateMode,Rotation,0,0,0);
    RGA_set_src_act_info(&Rga_Request,SrcActW,SrcActH, x_offset,y_offset);
    RGA_set_dst_act_info(&Rga_Request,DstActW,DstActH, xoffset,yoffset);

    if( handle->type == 1 )
    {
#if defined(__arm64__) || defined(__aarch64__)
        RGA_set_dst_vir_info(&Rga_Request, fd_dst,(unsigned long)(GPU_BASE), 0,DstVirW,DstVirH,&clip, Dstfmt, 0);
#else
        RGA_set_dst_vir_info(&Rga_Request, fd_dst,(unsigned int)(GPU_BASE), 0,DstVirW,DstVirH,&clip, Dstfmt, 0);
#endif
        RGA_set_mmu_info(&Rga_Request, 1, 0, 0, 0, 0, 2);
        Rga_Request.mmu_info.mmu_flag |= (1<<31) | (1<<10) | (1<<8);
    }

    if(ioctl(rga_fd, RGA_BLIT_SYNC, &Rga_Request) != 0) {
        LOGE(" %s(%d) RGA_BLIT fail",__FUNCTION__, __LINE__);
    }

    Context->mSrBI.mCurIndex = (Context->mSrBI.mCurIndex + 1)%MaxSpriteBNUM;
    //gettimeofday(&te,NULL);
    //ALOGD("SPRITE USE TIME T = %ld",(te.tv_sec-ts.tv_sec)*1000000+te.tv_usec-ts.tv_usec);
#endif
    return 0;
#else
    return 0;
#endif
}


bool hotplug_free_dimbuffer()
{
#if OPTIMIZATION_FOR_DIMLAYER
    hwcContext * context = _contextAnchor;
	if(_contextAnchor1 && _contextAnchor1->mDimHandle){
	    buffer_handle_t mhandle = _contextAnchor1->mDimHandle;
        int err = context->mAllocDev->free(context->mAllocDev, mhandle);
        ALOGW_IF(err,"free mDimHandle failed %d (%s)", err, strerror(-err));
        _contextAnchor1->mDimHandle = 0;
	}
#endif
    return true;
}

bool hwcPrimaryToExternalCheckConfig(hwcContext * ctx,struct rk_fb_win_cfg_data fb_info)
{
    hwcContext * context = _contextAnchor;
#ifdef RK3288_BOX
    if(context->mLcdcNum == 2){
        return true;
    }
#endif
    if(ctx != _contextAnchor){
        return true;
    }

    int compostMode = ctx->zone_manager.composter_mode;
    if(ctx->mHdmiSI.mix_vh){
        return true;
    }else if(compostMode != HWC_LCDC){
        return false;
    }

    bool ret = true;
    bool isSameResolution = false;
    bool isLargeHdmi = context->mHdmiSI.NeedReDst;
    int widthPrimary  = context->dpyAttr[HWCP].xres;
    int heightPrimary = context->dpyAttr[HWCP].yres;
    int widthExternal  = context->dpyAttr[HWCE].xres;
    int heightExternal = context->dpyAttr[HWCE].yres;

    isSameResolution = (widthPrimary == widthExternal && heightPrimary == heightExternal);
    for(int i = 0;i<4;i++){
        for(int j=0;j<4;j++){
            if(fb_info.win_par[i].area_par[j].ion_fd || fb_info.win_par[i].area_par[j].phy_addr){
                int win_id = fb_info.win_par[i].win_id;
                if(win_id >= 2){
                    ret = ret && isSameResolution && !isLargeHdmi;
                }
            }
        }
        if(!ret){
            break;
        }
    }
    return ret;
}

static unsigned int createCrc32(unsigned int crc,unsigned const char *buffer, unsigned int size)
{
	unsigned int i;
	for (i = 0; i < size; i++) {
		crc = crcTable[(crc ^ buffer[i]) & 0xff] ^ (crc >> 8);
	}
	return crc ;
}

static void initCrcTable(void)
{
	unsigned int c;
	unsigned int i, j;

	for (i = 0; i < 256; i++) {
		c = (unsigned int)i;
		for (j = 0; j < 8; j++) {
			if (c & 1) {
				c = 0xedb88320L ^ (c >> 1);
			} else {
			    c = c >> 1;
			}
		}
		crcTable[i] = c;
	}
}

static int fence_merge(char* value,int fd1,int fd2)
{
    int ret = -1;
    if(fd1 >= 0 && fd2 >= 0) {
        ret = sync_merge(value, fd1, fd2);
        close(fd1);close(fd2);
    } else if (fd1 >= 0) {
        ret = sync_merge(value, fd1, fd1);
        close(fd1);
    } else if (fd2 >= 0) {
        ret = sync_merge(value, fd2, fd2);
        close(fd2);
    }
    if(ret < 0) {
        ALOGW("%s:merge[%d,%d]:%s",value,fd1,fd2,strerror(errno));
    }
    ALOGD_IF(log(HLLSIX),"merge fd[%d,%d] to fd=%d",fd1,fd2,ret);
    return ret;
}

static int mipi_dual_vop_config(hwcContext *ctx,
                                    struct rk_fb_win_cfg_data * fbinfo)
{
    int ret = -1;
    int xvir = 0;
    int yvir = 0;
    int xact = 0;
    int yact = 0;
    int xpos = 0;
    int ypos = 0;
    int xsize = 0;
    int ysize = 0;
    int x_offset = 0;
    int y_offset = 0;
    int win_id = 0;
    int ion_fd = 0;
    int z_order = 0;
    int format = 0;
    int z_order_offset = 0;

    int index = 0;
    int acq_fence_fd = -1;

    int dpyPw = 0;
    int dpyPh = 0;

    struct rk_fb_win_cfg_data fbpinfo;
    memcpy(&fbpinfo,fbinfo,sizeof(fbpinfo));

    if (!ctx)
        return 0;

    if (ctx->mContextIndex == 0) {
        dpyPw = ctx->dpyAttr[0].relxres;
        dpyPh = ctx->dpyAttr[0].relyres;
    } else if (ctx->mContextIndex == 1) {
        dpyPw = ctx->dpyAttr[1].xres / 2;
        dpyPh = ctx->dpyAttr[1].yres * 2;
    }

    //if (!ctx->mComVop)
        //dump_config_info(*fbinfo,ctx,3);
    ALOGD_IF(log(HLLONE),"pri[%d,%d]",dpyPw,dpyPh);
    for(int i = 0;i < 4;i++) {
        for(int j = 0;j < 4;j++) {
            if(fbinfo->win_par[i].area_par[j].phy_addr || fbinfo->win_par[i].area_par[j].ion_fd) {
                 x_offset    = fbinfo->win_par[i].area_par[j].x_offset;
                 y_offset    = fbinfo->win_par[i].area_par[j].y_offset;
                 xact        = fbinfo->win_par[i].area_par[j].xact;
                 yact        = fbinfo->win_par[i].area_par[j].yact;
                 xpos        = fbinfo->win_par[i].area_par[j].xpos;
                 ypos        = fbinfo->win_par[i].area_par[j].ypos;
                 xsize       = fbinfo->win_par[i].area_par[j].xsize;
                 ysize       = fbinfo->win_par[i].area_par[j].ysize;
                 xvir        = fbinfo->win_par[i].area_par[j].xvir;
                 yvir        = fbinfo->win_par[i].area_par[j].yvir;
                 win_id      = fbinfo->win_par[i].win_id;
                 z_order     = fbinfo->win_par[i].z_order;
                 ion_fd      = fbinfo->win_par[i].area_par[j].ion_fd;
                 format      = fbinfo->win_par[i].area_par[j].data_format;
                 acq_fence_fd= fbinfo->win_par[i].area_par[j].acq_fence_fd;

                if (win_id < 2) {
                    fbpinfo.win_par[index].win_id                    = 0;
                    fbpinfo.win_par[index].area_par[0].xvir          = xvir;
                    fbpinfo.win_par[index].area_par[0].yvir          = yvir;
                    fbpinfo.win_par[index].area_par[0].x_offset      = x_offset;
                    fbpinfo.win_par[index].area_par[0].y_offset      = y_offset;
                    fbpinfo.win_par[index].area_par[0].xact          = xact;
                    fbpinfo.win_par[index].area_par[0].yact          = yact;
                    fbpinfo.win_par[index].area_par[0].xpos          = xpos;
                    fbpinfo.win_par[index].area_par[0].ypos          = ypos;
                    fbpinfo.win_par[index].area_par[0].xsize         = xsize;
                    fbpinfo.win_par[index].area_par[0].ysize         = ysize;
                    fbpinfo.win_par[index].area_par[0].acq_fence_fd  = acq_fence_fd;
                    fbpinfo.win_par[index].area_par[0].data_format   = format;
                    fbpinfo.win_par[index].area_par[0].ion_fd        = ion_fd;
                    fbpinfo.win_par[index].z_order                   = z_order + z_order_offset;

                    if (dpyPh - ypos > ysize) {
                        index++;
                        continue;
                    }

                    fbpinfo.win_par[index].area_par[0].ysize         = dpyPh / 2 - ypos;
                    fbpinfo.win_par[index].area_par[0].yact          = yact * (dpyPh / 2 - ypos) / ysize;

                    index++;
                    z_order_offset++;

                    if (ctx->mComVop)
                        fbpinfo.win_par[index].win_id                = 1;
                    else
                        fbpinfo.win_par[index].win_id                = 2;
                    fbpinfo.win_par[index].area_par[0].xvir          = xvir;
                    fbpinfo.win_par[index].area_par[0].yvir          = yvir;
                    fbpinfo.win_par[index].area_par[0].x_offset      = x_offset;
                    fbpinfo.win_par[index].area_par[0].y_offset      = y_offset + fbpinfo.win_par[index - 1].area_par[0].yact;
                    fbpinfo.win_par[index].area_par[0].xact          = xact;
                    fbpinfo.win_par[index].area_par[0].yact          = yact - fbpinfo.win_par[index - 1].area_par[0].yact;
                    fbpinfo.win_par[index].area_par[0].xpos          = xpos + dpyPw;
                    fbpinfo.win_par[index].area_par[0].ypos          = ypos;
                    fbpinfo.win_par[index].area_par[0].xsize         = xsize;
                    fbpinfo.win_par[index].area_par[0].ysize         = ysize - fbpinfo.win_par[index - 1].area_par[0].ysize;
                    fbpinfo.win_par[index].area_par[0].acq_fence_fd  = -1;
                    fbpinfo.win_par[index].area_par[0].data_format   = format;
                    fbpinfo.win_par[index].area_par[0].ion_fd        = ion_fd;
                    if (ctx->mComVop)
                        fbpinfo.win_par[index].z_order               = z_order + z_order_offset;
                    else
                        fbpinfo.win_par[index].z_order               = z_order + z_order_offset + 1;

                    index ++;
                } else {
                    fbpinfo.win_par[index].win_id                    = 2;
                    fbpinfo.win_par[index].area_par[0].xvir          = xvir;
                    fbpinfo.win_par[index].area_par[0].yvir          = yvir;
                    fbpinfo.win_par[index].area_par[0].x_offset      = x_offset;
                    fbpinfo.win_par[index].area_par[0].y_offset      = y_offset;
                    fbpinfo.win_par[index].area_par[0].xact          = xact;
                    fbpinfo.win_par[index].area_par[0].yact          = yact;
                    fbpinfo.win_par[index].area_par[0].xpos          = xpos;
                    fbpinfo.win_par[index].area_par[0].ypos          = ypos;
                    fbpinfo.win_par[index].area_par[0].xsize         = xsize;
                    fbpinfo.win_par[index].area_par[0].ysize         = ysize;
                    fbpinfo.win_par[index].area_par[0].acq_fence_fd  = acq_fence_fd;
                    fbpinfo.win_par[index].area_par[0].data_format   = format;
                    fbpinfo.win_par[index].area_par[0].ion_fd        = ion_fd;
                    fbpinfo.win_par[index].z_order                   = z_order + z_order_offset;

                    if (dpyPh - ypos > ysize) {
                        index++;
                        continue;
                    }

                    fbpinfo.win_par[index].area_par[0].ysize         = dpyPh / 2 - ypos;
                    fbpinfo.win_par[index].area_par[0].yact          = yact * (dpyPh / 2 - ypos) / ysize;
                    if (fbpinfo.win_par[index].area_par[0].ysize != fbpinfo.win_par[index].area_par[0].yact)
                        fbpinfo.win_par[index].area_par[0].yact = fbpinfo.win_par[index].area_par[0].ysize;

                    index++;
                    z_order_offset++;

                    fbpinfo.win_par[index].win_id                    = 3;
                    fbpinfo.win_par[index].area_par[0].xvir          = xvir;
                    fbpinfo.win_par[index].area_par[0].yvir          = yvir;
                    fbpinfo.win_par[index].area_par[0].x_offset      = x_offset;
                    fbpinfo.win_par[index].area_par[0].y_offset      = y_offset + fbpinfo.win_par[index - 1].area_par[0].yact;
                    fbpinfo.win_par[index].area_par[0].xact          = xact;
                    fbpinfo.win_par[index].area_par[0].yact          = yact - fbpinfo.win_par[index - 1].area_par[0].yact;
                    fbpinfo.win_par[index].area_par[0].xpos          = xpos + dpyPw;
                    fbpinfo.win_par[index].area_par[0].ypos          = ypos;
                    fbpinfo.win_par[index].area_par[0].xsize         = xsize;
                    fbpinfo.win_par[index].area_par[0].ysize         = ysize - fbpinfo.win_par[index - 1].area_par[0].ysize;
                    fbpinfo.win_par[index].area_par[0].acq_fence_fd  = -1;
                    fbpinfo.win_par[index].area_par[0].data_format   = format;
                    fbpinfo.win_par[index].area_par[0].ion_fd        = ion_fd;
                    fbpinfo.win_par[index].z_order                   = z_order + z_order_offset;
                    index ++;
                }
            }
        }
    }
    //dump_config_info(fbpinfo,ctx,10);
    memcpy(fbinfo,&fbpinfo,sizeof(fbpinfo));
    return 0;
}


static int dual_view_vop_config(struct rk_fb_win_cfg_data * fbinfo)
{
    int ret = -1;
    int xvir = 0;
    int yvir = 0;
    int xact = 0;
    int yact = 0;
    int xpos = 0;
    int ypos = 0;
    int xsize = 0;
    int ysize = 0;
    int x_offset = 0;
    int y_offset = 0;
    struct rk_fb_win_cfg_data fbpinfo;
    memset(&fbpinfo,0,sizeof(fbpinfo));
    fbpinfo.ret_fence_fd = -1;
    for(size_t i=0;i<RK_MAX_BUF_NUM;i++) {
        fbpinfo.rel_fence_fd[i] = -1;
    }
    struct rk_fb_win_cfg_data fbeinfo = fbpinfo;
    hwcContext * ctxp = _contextAnchor;
    hwcContext * ctxe = _contextAnchor1;
    if(!ctxe) {
        return false;
    }
    int dpyPw = ctxp->dpyAttr[0].xres;
    int dpyPh = ctxp->dpyAttr[0].yres;
    int dpyEw = ctxe->dpyAttr[1].xres;
    int dpyEh = ctxe->dpyAttr[1].yres;
    int width = ctxp->dpyAttr[1].xres;
    int height= ctxp->dpyAttr[1].yres;
    float w_scale = (float)dpyPw / dpyEw;
    float h_scale = (float)dpyPh / dpyEh;
    dump_config_info(*fbinfo,ctxp,3);
    ALOGD_IF(log(HLLONE),"pri[%d,%d],ext[%d,%d],dpy[%d,%d]",dpyPw,dpyPh,dpyEw,dpyEh,width,height);
    for(int i = 0;i < 4;i++) {
        for(int j = 0;j < 4;j++) {
            if(fbinfo->win_par[i].area_par[j].phy_addr || fbinfo->win_par[i].area_par[j].ion_fd) {
                x_offset    = fbinfo->win_par[i].area_par[j].x_offset;
                y_offset    = fbinfo->win_par[i].area_par[j].y_offset;
                xact        = fbinfo->win_par[i].area_par[j].xact;
                yact        = fbinfo->win_par[i].area_par[j].yact;
                xpos        = fbinfo->win_par[i].area_par[j].xpos;
                ypos        = fbinfo->win_par[i].area_par[j].ypos;
                xsize       = fbinfo->win_par[i].area_par[j].xsize;
                ysize       = fbinfo->win_par[i].area_par[j].ysize;
                xvir        = fbinfo->win_par[i].area_par[j].xvir;
                yvir        = fbinfo->win_par[i].area_par[j].yvir;


                fbpinfo.win_par[i].win_id                    = fbinfo->win_par[i].win_id;
                fbpinfo.win_par[i].z_order                   = fbinfo->win_par[i].z_order;
                fbpinfo.win_par[i].area_par[j].ion_fd        = fbinfo->win_par[i].area_par[j].ion_fd;
                fbpinfo.win_par[i].area_par[j].xvir          = xvir;
                fbpinfo.win_par[i].area_par[j].yvir          = yvir;
                fbpinfo.win_par[i].area_par[j].yact          = yact;
                fbpinfo.win_par[i].area_par[j].ysize         = ysize;
                fbpinfo.win_par[i].area_par[j].acq_fence_fd  = fbinfo->win_par[i].area_par[j].acq_fence_fd;
                fbpinfo.win_par[i].area_par[j].data_format   = fbinfo->win_par[i].area_par[j].data_format;

                fbeinfo.win_par[i].win_id                    = fbinfo->win_par[i].win_id;
                fbeinfo.win_par[i].z_order                   = fbinfo->win_par[i].z_order;
                fbeinfo.win_par[i].area_par[j].ion_fd        = fbinfo->win_par[i].area_par[j].ion_fd;
                fbeinfo.win_par[i].area_par[j].xvir          = xvir;
                fbeinfo.win_par[i].area_par[j].yvir          = yvir;
                fbeinfo.win_par[i].area_par[j].yact          = yact;
                fbeinfo.win_par[i].area_par[j].ysize         = ysize;
                fbeinfo.win_par[i].area_par[j].acq_fence_fd  = fbinfo->win_par[i].area_par[j].acq_fence_fd;
                fbeinfo.win_par[i].area_par[j].data_format   = fbinfo->win_par[i].area_par[j].data_format;

                fbpinfo.win_par[i].area_par[j].x_offset      = x_offset;
                fbpinfo.win_par[i].area_par[j].y_offset      = y_offset;
                fbpinfo.win_par[i].area_par[j].xact          = xact * (dpyEw - xpos) / xsize;
                fbpinfo.win_par[i].area_par[j].xpos          = xpos;
                fbpinfo.win_par[i].area_par[j].ypos          = ypos;
                fbpinfo.win_par[i].area_par[j].xsize         = dpyEw - xpos;

                fbeinfo.win_par[i].area_par[j].x_offset      = x_offset + fbpinfo.win_par[i].area_par[j].xact;
                fbeinfo.win_par[i].area_par[j].y_offset      = y_offset;
                fbeinfo.win_par[i].area_par[j].xact          = xact - fbpinfo.win_par[i].area_par[j].xact;
                fbeinfo.win_par[i].area_par[j].xpos          = 0;
                fbeinfo.win_par[i].area_par[j].ypos          = y_offset;
                fbeinfo.win_par[i].area_par[j].xsize         = xsize - dpyEw + xpos;

                if(dpyPw != dpyEw || dpyPh != dpyEh) {
                    fbpinfo.win_par[i].area_par[j].xpos  = w_scale * xpos;
                    fbpinfo.win_par[i].area_par[j].ypos  = h_scale * ypos;
                    fbpinfo.win_par[i].area_par[j].xsize = w_scale * (dpyEw - xpos);
                    fbpinfo.win_par[i].area_par[j].ysize = h_scale * ysize;
                }
            }
        }
    }
    dump_config_info(fbpinfo,ctxp,10);
    dump_config_info(fbeinfo,ctxp,10);
    if(ioctl(ctxp->fbFd, RK_FBIOSET_CONFIG_DONE, &fbpinfo)){
        ALOGE("dual view p:ioctl fail:%s",strerror(errno));
        dump_config_info(fbpinfo,ctxp,3);
    }else{
        ALOGD_IF(log(HLLONE),"ID=dual view p");
    }
    if(ioctl(ctxe->fbFd, RK_FBIOSET_CONFIG_DONE, &fbeinfo)){
        ALOGE("dual view e:ioctl fail:%s",strerror(errno));
        dump_config_info(fbeinfo,ctxp,3);
    }else{
        ALOGD_IF(log(HLLONE),"ID=dual view e");
    }
	for(size_t i=0;i<RK_MAX_BUF_NUM;i++) {
        if(fbinfo->rel_fence_fd[i] > -1) {
            close(fbinfo->rel_fence_fd[i]);
            fbinfo->rel_fence_fd[i] = -1;
        }
    }
    if(fbinfo->ret_fence_fd > -1) {
        close(fbinfo->ret_fence_fd);
        fbinfo->ret_fence_fd = -1;
    }
	for(size_t i=0;i<RK_MAX_BUF_NUM;i++) {
	    char value[20] = "dual-view-rel";
        if(fbpinfo.rel_fence_fd[i] >= 0 || fbeinfo.rel_fence_fd[i] >= 0) {
            ret = fence_merge(value,fbpinfo.rel_fence_fd[i],fbeinfo.rel_fence_fd[i]);
            if(ret >= 0) {
                fbinfo->rel_fence_fd[i] = ret;
            } else {
                fbinfo->rel_fence_fd[i] = -1;
                ALOGE("dual-view-ret merge fail [%d,%d]",fbpinfo.rel_fence_fd[i],fbpinfo.rel_fence_fd[i]);
            }
        } else {
            fbinfo->rel_fence_fd[i] = -1;
        }
    }
    if(fbpinfo.ret_fence_fd >= 0 || fbeinfo.ret_fence_fd >= 0) {
        char value[20] = "dual-view-ret";
        ret = fence_merge(value,fbpinfo.ret_fence_fd,fbeinfo.ret_fence_fd);
        if(ret >= 0) {
            fbinfo->ret_fence_fd = ret;
        } else {
            fbinfo->ret_fence_fd = -1;
            ALOGE("dual-view-ret merge fail [%d,%d]",fbpinfo.ret_fence_fd,fbpinfo.ret_fence_fd);
        }
    } else {
        fbinfo->ret_fence_fd = -1;
    }
    return 0;
}

int hwc_rga_fix_zones_for_yuv_ten_bit(ZoneManager* pZones)
{
    ZoneManager zone_m;
    int i,j,k,yuvTenBitIndex,zoneCnt;

    zoneCnt = pZones->zone_cnt;

    yuvTenBitIndex = 0;
    memcpy(&zone_m, pZones, sizeof(ZoneManager));
    memset(pZones, 0, sizeof(ZoneManager));

    for (k = 0; k < zone_m.zone_cnt; k++) {
        if (zone_m.zone_info[k].format == HAL_PIXEL_FORMAT_YCrCb_NV12_10) {
            yuvTenBitIndex = k;
        }
        ALOGD("i=%d,format=%d",k,zone_m.zone_info[k].format);
    }

    for (i = yuvTenBitIndex, j = 0; i < zone_m.zone_cnt; i++,j++)
        memcpy(&pZones->zone_info[j], &zone_m.zone_info[i], sizeof(ZoneInfo));

    pZones->zone_cnt = zone_m.zone_cnt - yuvTenBitIndex;
    return yuvTenBitIndex;
}

int hwc_add_rga_blit_fbinfo(hwcContext * ctx, struct hwc_fb_info *hfi)
{
    int addIndex = 0;
    buffer_handle_t hnd = hwc_rga_blit_get_current_buffer(ctx);
    struct rk_fb_win_par win_par[RK30_MAX_LAYER_SUPPORT];
    struct rk_fb_win_cfg_data *fbinfo = NULL;

    if (!hfi) {
        ALOGE("There is no hfi for %s:%d", __func__, __LINE__);
        return -EINVAL;
    }

    fbinfo = &hfi->fb_info;

    for(int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            if(fbinfo->win_par[i].area_par[j].ion_fd ||
                                           fbinfo->win_par[i].area_par[j].phy_addr) {
                fbinfo->win_par[i].z_order ++;
                addIndex = i;
            }
        }
    }

    addIndex ++;

    struct private_handle_t*  handle = (struct private_handle_t*)hnd;
    if (!handle) {
        ALOGE("There is no handle for %s:%d", __func__, __LINE__);
        return -EINVAL;
    }

    if (ctx->fbhandle.format == HAL_PIXEL_FORMAT_RGBA_8888)
        fbinfo->win_par[addIndex].area_par[0].data_format = HAL_PIXEL_FORMAT_RGBX_8888;
    else
        fbinfo->win_par[addIndex].area_par[0].data_format = ctx->fbhandle.format;

    fbinfo->win_par[addIndex].win_id = 2;
    fbinfo->win_par[addIndex].z_order = 0;
    fbinfo->win_par[addIndex].area_par[0].ion_fd = handle->share_fd;
    fbinfo->win_par[addIndex].area_par[0].acq_fence_fd = -1;
    fbinfo->win_par[addIndex].area_par[0].x_offset = 0;
    fbinfo->win_par[addIndex].area_par[0].y_offset = 0;
    fbinfo->win_par[addIndex].area_par[0].xpos = 0;
    fbinfo->win_par[addIndex].area_par[0].ypos = 0;
    fbinfo->win_par[addIndex].area_par[0].xsize = handle->width;
    fbinfo->win_par[addIndex].area_par[0].ysize = handle->height;
    fbinfo->win_par[addIndex].area_par[0].xact = handle->width;
    fbinfo->win_par[addIndex].area_par[0].yact = handle->height;
    fbinfo->win_par[addIndex].area_par[0].xvir = handle->stride;
    fbinfo->win_par[addIndex].area_par[0].yvir = handle->height;
    /*for (int i = 0; i < RK_MAX_BUF_NUM; i++) {
        if (!hfi->pRelFenceFd[i] && ctx->mRgaBlitRelFd[ctx->mCurRgaBlitIndex] == -1) {
            hfi->pRelFenceFd[i] = &ctx->mRgaBlitRelFd[ctx->mCurRgaBlitIndex];
            break;
        }
    }*/

    return 0;
}

int hwc_rga_blit_alloc_rects(hwcContext * context)
{
    for (int i = 0; i < MaxBlitNum; i++) {
        if (context->mRgaBlitRects[i] == NULL) {
            size_t size = 30 * sizeof(hwc_rect_t);
            context->mRgaBlitRects[i] = (hwc_rect_t *)malloc(size);
        }
    }

    return 0;
}

int hwc_rga_blit_free_rects(hwcContext * context)
{
    for (int i = 0; i < MaxBlitNum; i++) {
        if (context->mRgaBlitRects[i] == NULL) {
            free(context->mRgaBlitRects[i]);
        }
    }

    return 0;
}

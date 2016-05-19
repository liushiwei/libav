/*
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <windows.h>

#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#define DXVA2API_USE_BITFIELDS
#define COBJMACROS

#include <d3d9.h>
#include <dxva2api.h>
#include <initguid.h>

#include "common.h"
#include "hwcontext.h"
#include "hwcontext_dxva2.h"
#include "hwcontext_internal.h"
#include "imgutils.h"
#include "pixdesc.h"
#include "pixfmt.h"

typedef struct DXVA2FramesContext {
    IDirect3DSurface9 **surfaces_internal;
    int              nb_surfaces_used;

    HANDLE  device_handle;
    IDirectXVideoAccelerationService *service;

    D3DFORMAT format;
} DXVA2FramesContext;

static const struct {
    D3DFORMAT d3d_format;
    enum AVPixelFormat pix_fmt;
} supported_formats[] = {
    { MKTAG('N', 'V', '1', '2'), AV_PIX_FMT_NV12 },
};

DEFINE_GUID(video_decoder_service,   0xfc51a551, 0xd5e7, 0x11d9, 0xaf, 0x55, 0x00, 0x05, 0x4e, 0x43, 0xff, 0x02);
DEFINE_GUID(video_processor_service, 0xfc51a552, 0xd5e7, 0x11d9, 0xaf, 0x55, 0x00, 0x05, 0x4e, 0x43, 0xff, 0x02);

static void dxva2_frames_uninit(AVHWFramesContext *ctx)
{
    AVDXVA2DeviceContext *device_hwctx = ctx->device_ctx->hwctx;
    AVDXVA2FramesContext *frames_hwctx = ctx->hwctx;
    DXVA2FramesContext *s = ctx->internal->priv;
    int i;

    if (frames_hwctx->decoder_to_release)
        IDirectXVideoDecoder_Release(frames_hwctx->decoder_to_release);

    if (s->surfaces_internal) {
        for (i = 0; i < frames_hwctx->nb_surfaces; i++) {
            if (s->surfaces_internal[i])
                IDirect3DSurface9_Release(s->surfaces_internal[i]);
        }
    }
    av_freep(&s->surfaces_internal);

    if (s->service) {
        IDirectXVideoAccelerationService_Release(s->service);
        s->service = NULL;
    }

    if (s->device_handle != INVALID_HANDLE_VALUE) {
        IDirect3DDeviceManager9_CloseDeviceHandle(device_hwctx->devmgr, s->device_handle);
        s->device_handle = INVALID_HANDLE_VALUE;
    }
}

static AVBufferRef *dxva2_pool_alloc(void *opaque, int size)
{
    AVHWFramesContext      *ctx = (AVHWFramesContext*)opaque;
    DXVA2FramesContext       *s = ctx->internal->priv;
    AVDXVA2FramesContext *hwctx = ctx->hwctx;

    if (s->nb_surfaces_used < hwctx->nb_surfaces) {
        s->nb_surfaces_used++;
        return av_buffer_create((uint8_t*)s->surfaces_internal[s->nb_surfaces_used - 1],
                                sizeof(*hwctx->surfaces), NULL, 0, 0);
    }

    return NULL;
}

static int dxva2_init_pool(AVHWFramesContext *ctx)
{
    AVDXVA2FramesContext *frames_hwctx = ctx->hwctx;
    AVDXVA2DeviceContext *device_hwctx = ctx->device_ctx->hwctx;
    DXVA2FramesContext              *s = ctx->internal->priv;
    int decode = (frames_hwctx->surface_type == DXVA2_VideoDecoderRenderTarget);

    int i;
    HRESULT hr;

    if (ctx->initial_pool_size <= 0)
        return 0;

    hr = IDirect3DDeviceManager9_OpenDeviceHandle(device_hwctx->devmgr, &s->device_handle);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to open device handle\n");
        return AVERROR_UNKNOWN;
    }

    hr = IDirect3DDeviceManager9_GetVideoService(device_hwctx->devmgr,
                                                 s->device_handle,
                                                 decode ? &video_decoder_service : &video_processor_service,
                                                 (void **)&s->service);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create the video service\n");
        return AVERROR_UNKNOWN;
    }

    for (i = 0; i < FF_ARRAY_ELEMS(supported_formats); i++) {
        if (ctx->sw_format == supported_formats[i].pix_fmt) {
            s->format = supported_formats[i].d3d_format;
            break;
        }
    }
    if (i == FF_ARRAY_ELEMS(supported_formats)) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported pixel format: %s\n",
               av_get_pix_fmt_name(ctx->sw_format));
        return AVERROR(EINVAL);
    }

    s->surfaces_internal = av_mallocz_array(ctx->initial_pool_size,
                                            sizeof(*s->surfaces_internal));
    if (!s->surfaces_internal)
        return AVERROR(ENOMEM);

    hr = IDirectXVideoAccelerationService_CreateSurface(s->service,
                                                        ctx->width, ctx->height,
                                                        ctx->initial_pool_size - 1,
                                                        s->format, D3DPOOL_DEFAULT, 0,
                                                        frames_hwctx->surface_type,
                                                        s->surfaces_internal, NULL);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Could not create the surfaces\n");
        return AVERROR_UNKNOWN;
    }

    ctx->internal->pool_internal = av_buffer_pool_init2(sizeof(*s->surfaces_internal),
                                                        ctx, dxva2_pool_alloc, NULL);
    if (!ctx->internal->pool_internal)
        return AVERROR(ENOMEM);

    frames_hwctx->surfaces    = s->surfaces_internal;
    frames_hwctx->nb_surfaces = ctx->initial_pool_size;

    return 0;
}

static int dxva2_frames_init(AVHWFramesContext *ctx)
{
    AVDXVA2FramesContext *hwctx = ctx->hwctx;
    DXVA2FramesContext       *s = ctx->internal->priv;
    int ret;

    if (hwctx->surface_type != DXVA2_VideoDecoderRenderTarget &&
        hwctx->surface_type != DXVA2_VideoProcessorRenderTarget) {
        av_log(ctx, AV_LOG_ERROR, "Unknown surface type: %lu\n",
               hwctx->surface_type);
        return AVERROR(EINVAL);
    }

    s->device_handle = INVALID_HANDLE_VALUE;

    /* init the frame pool if the caller didn't provide one */
    if (!ctx->pool) {
        ret = dxva2_init_pool(ctx);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Error creating an internal frame pool\n");
            return ret;
        }
    }

    return 0;
}

static int dxva2_get_buffer(AVHWFramesContext *ctx, AVFrame *frame)
{
    frame->buf[0] = av_buffer_pool_get(ctx->pool);
    if (!frame->buf[0])
        return AVERROR(ENOMEM);

    frame->data[3] = frame->buf[0]->data;
    frame->format  = AV_PIX_FMT_DXVA2_VLD;
    frame->width   = ctx->width;
    frame->height  = ctx->height;

    return 0;
}

static int dxva2_transfer_get_formats(AVHWFramesContext *ctx,
                                      enum AVHWFrameTransferDirection dir,
                                      enum AVPixelFormat **formats)
{
    enum AVPixelFormat *fmts;

    fmts = av_malloc_array(2, sizeof(*fmts));
    if (!fmts)
        return AVERROR(ENOMEM);

    fmts[0] = ctx->sw_format;
    fmts[1] = AV_PIX_FMT_NONE;

    *formats = fmts;

    return 0;
}

static int dxva2_transfer_data(AVHWFramesContext *ctx, AVFrame *dst,
                               const AVFrame *src)
{
    IDirect3DSurface9 *surface;
    D3DSURFACE_DESC    surfaceDesc;
    D3DLOCKED_RECT     LockedRect;
    HRESULT            hr;

    int download = !!src->hw_frames_ctx;

    surface = (IDirect3DSurface9*)(download ? src->data[3] : dst->data[3]);

    hr = IDirect3DSurface9_GetDesc(surface, &surfaceDesc);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Error getting a surface description\n");
        return AVERROR_UNKNOWN;
    }

    hr = IDirect3DSurface9_LockRect(surface, &LockedRect, NULL,
                                    download ? D3DLOCK_READONLY : D3DLOCK_DISCARD);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Unable to lock DXVA2 surface\n");
        return AVERROR_UNKNOWN;
    }

    if (download) {
        av_image_copy_plane(dst->data[0], dst->linesize[0],
                            (uint8_t*)LockedRect.pBits, LockedRect.Pitch,
                            src->width, src->height);
        av_image_copy_plane(dst->data[1], dst->linesize[1],
                            (uint8_t*)LockedRect.pBits + LockedRect.Pitch * surfaceDesc.Height,
                            LockedRect.Pitch, src->width, src->height / 2);
    } else {
        av_image_copy_plane((uint8_t*)LockedRect.pBits, LockedRect.Pitch,
                            dst->data[0], dst->linesize[0],
                            src->width, src->height);
        av_image_copy_plane((uint8_t*)LockedRect.pBits + LockedRect.Pitch * surfaceDesc.Height,
                            LockedRect.Pitch, dst->data[1], dst->linesize[1],
                            src->width, src->height / 2);
    }

    IDirect3DSurface9_UnlockRect(surface);

    return 0;
}

const HWContextType ff_hwcontext_type_dxva2 = {
    .type                 = AV_HWDEVICE_TYPE_DXVA2,
    .name                 = "DXVA2",

    .device_hwctx_size    = sizeof(AVDXVA2DeviceContext),
    .frames_hwctx_size    = sizeof(AVDXVA2FramesContext),
    .frames_priv_size     = sizeof(DXVA2FramesContext),

    .frames_init          = dxva2_frames_init,
    .frames_uninit        = dxva2_frames_uninit,
    .frames_get_buffer    = dxva2_get_buffer,
    .transfer_get_formats = dxva2_transfer_get_formats,
    .transfer_data_to     = dxva2_transfer_data,
    .transfer_data_from   = dxva2_transfer_data,

    .pix_fmts             = (const enum AVPixelFormat[]){ AV_PIX_FMT_DXVA2_VLD, AV_PIX_FMT_NONE },
};
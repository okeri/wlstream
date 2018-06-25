#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 199309L
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <pulse/pulseaudio.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>
#include <libdrm/drm_fourcc.h>
#include "wlr-export-dmabuf-unstable-v1-client-protocol.h"

struct wayland_output {
	struct wl_list link;
	uint32_t id;
	struct wl_output *output;
	char *make;
	char *model;
	int width;
	int height;
	AVRational framerate;
};

struct fifo_buffer {
	AVFrame **queued_frames;
	int num_queued_frames;
	int max_queued_frames;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	pthread_mutex_t cond_lock;
};

struct capture_context {
    AVClass *class; /* For pretty logging */
    struct wl_display *display;
    struct wl_registry *registry;
    struct zwlr_export_dmabuf_manager_v1 *export_manager;

    struct wl_list output_list;

    /* Target */
    struct wl_output *target_output;

    /* Main frame callback */
    struct zwlr_export_dmabuf_frame_v1 *frame_callback;

    /* If something happens during capture */
    int err;
    bool quit;

    /* Lavf */
    AVFormatContext *avf;
    pthread_mutex_t avf_lock;

    /* Video */
    pthread_t video_thread;
    int64_t vid_start_pts;
    AVFrame *current_frame;
    struct fifo_buffer video_frames;
    AVCodecContext *video_avctx;
    AVBufferRef *drm_device_ref;
    AVBufferRef *drm_frames_ref;
    AVBufferRef *mapped_device_ref;
    AVBufferRef *mapped_frames_ref;

    /* Audio */
    pa_sample_spec pa_spec;
    pthread_t audio_thread;
    int64_t audio_start_pts;
    AVCodecContext *audio_avctx;
    SwrContext *swr_ctx;
    pa_context *pa_context;
    pa_threaded_mainloop *pa_mainloop;
    pa_mainloop_api *pa_mainloop_api;
    struct fifo_buffer audio_frames;

    /* Config */
    char *out_filename;

    char *video_encoder;
    float video_bitrate;
    int video_frame_queue;
    char *hardware_device;
    bool is_software_encoder;
    enum AVPixelFormat video_sw_format;
    enum AVHWDeviceType hw_device_type;
    AVDictionary *video_encoder_opts;

    char *audio_encoder;
    float audio_bitrate;
    int audio_samplerate;
    int audio_frame_queue;
    AVDictionary *audio_encoder_opts;
};

static int init_fifo(struct fifo_buffer *buf, int max_queued_frames)
{
    pthread_mutex_init(&buf->lock, NULL);
    pthread_cond_init(&buf->cond, NULL);
    pthread_mutex_init(&buf->cond_lock, NULL);
    buf->num_queued_frames = 0;
    buf->max_queued_frames = max_queued_frames;
    buf->queued_frames = av_mallocz(buf->max_queued_frames * sizeof(AVFrame *));
    return !buf->queued_frames ? AVERROR(ENOMEM) : 0;
}

static int get_fifo_size(struct fifo_buffer *buf)
{
    pthread_mutex_lock(&buf->lock);
    int ret = buf->num_queued_frames;
    pthread_mutex_unlock(&buf->lock);
    return ret;
}

static int push_to_fifo(struct fifo_buffer *buf, AVFrame *f)
{
	int ret;
    pthread_mutex_lock(&buf->lock);
    if ((buf->num_queued_frames + 1) > buf->max_queued_frames) {
        av_frame_free(&f);
        ret = 1;
    } else {
        buf->queued_frames[buf->num_queued_frames++] = f;
        ret = 0;
    }
    pthread_mutex_unlock(&buf->lock);
    pthread_cond_signal(&buf->cond);
    return ret;
}

static AVFrame *pop_from_fifo(struct fifo_buffer *buf)
{
    pthread_mutex_lock(&buf->lock);

    if (!buf->num_queued_frames) {
        pthread_mutex_unlock(&buf->lock);
        pthread_cond_wait(&buf->cond, &buf->cond_lock);
        pthread_mutex_lock(&buf->lock);
    }

    AVFrame *rf = buf->queued_frames[0];
    for (int i = 1; i < buf->num_queued_frames; i++)
        buf->queued_frames[i - 1] = buf->queued_frames[i];

    buf->num_queued_frames--;
    buf->queued_frames[buf->num_queued_frames] = NULL;

    pthread_mutex_unlock(&buf->lock);
    return rf;
}

static void free_fifo(struct fifo_buffer *buf)
{
    pthread_mutex_lock(&buf->lock);
    if (buf->num_queued_frames)
        for (int i = 0; i < buf->num_queued_frames; i++)
            av_frame_free(&buf->queued_frames[i]);
    av_freep(&buf->queued_frames);
    pthread_mutex_unlock(&buf->lock);
}

static void output_handle_geometry(void *data, struct wl_output *wl_output,
                                   int32_t x, int32_t y, int32_t phys_width,
                                   int32_t phys_height, int32_t subpixel,
                                   const char *make, const char *model,
                                   int32_t transform)
{
    struct wayland_output *output = data;
    output->make = av_strdup(make);
    output->model = av_strdup(model);
}

static void output_handle_mode(void *data, struct wl_output *wl_output,
                               uint32_t flags, int32_t width, int32_t height,
                               int32_t refresh)
{
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        struct wayland_output *output = data;
        output->width = width;
        output->height = height;
        output->framerate = (AVRational){ refresh, 1000 };
    }
}

static void output_handle_done(void *data, struct wl_output *wl_output)
{
	/* Nothing to do */
}

static void output_handle_scale(void *data, struct wl_output *wl_output,
                                int32_t factor)
{
	/* Nothing to do */
}

static const struct wl_output_listener output_listener = {
    .geometry = output_handle_geometry,
    .mode = output_handle_mode,
    .done = output_handle_done,
    .scale = output_handle_scale,
};

static void registry_handle_add(void *data, struct wl_registry *reg,
                                uint32_t id, const char *interface,
                                uint32_t ver)
{
    struct capture_context *ctx = data;

    if (!strcmp(interface, wl_output_interface.name)) {
        struct wayland_output *output = av_mallocz(sizeof(*output));

        output->id = id;
        output->output = wl_registry_bind(reg, id, &wl_output_interface, 1);

        wl_output_add_listener(output->output, &output_listener, output);
        wl_list_insert(&ctx->output_list, &output->link);
    }

    if (!strcmp(interface, zwlr_export_dmabuf_manager_v1_interface.name)) {
        const struct wl_interface *i = &zwlr_export_dmabuf_manager_v1_interface;
        ctx->export_manager = wl_registry_bind(reg, id, i, 1);
	}
}

static void remove_output(struct wayland_output *out)
{
    wl_list_remove(&out->link);
    av_free(out->make);
    av_free(out->model);
    av_free(out);
}

static struct wayland_output *find_output(struct capture_context *ctx,
                                          struct wl_output *out, uint32_t id)
{
	struct wayland_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &ctx->output_list, link)
		if ((output->output == out) || (output->id == id))
			return output;
	return NULL;
}

static void registry_handle_remove(void *data, struct wl_registry *reg,
                                   uint32_t id)
{
    remove_output(find_output((struct capture_context *)data, NULL, id));
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_handle_add,
	.global_remove = registry_handle_remove,
};

static void frame_free(void *opaque, uint8_t *data)
{
    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)data;

    for (int i = 0; i < desc->nb_objects; ++i)
        close(desc->objects[i].fd);

    zwlr_export_dmabuf_frame_v1_destroy(opaque);

    av_free(data);
}

static void frame_start(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
		                uint32_t width, uint32_t height, uint32_t offset_x,
		                uint32_t offset_y, uint32_t buffer_flags, uint32_t flags,
		                uint32_t format, uint32_t mod_high, uint32_t mod_low,
		                uint32_t num_objects)
{
    struct capture_context *ctx = data;
    int err = 0;

	/* Allocate DRM specific struct */
	AVDRMFrameDescriptor *desc = av_mallocz(sizeof(*desc));
	if (!desc) {
		err = AVERROR(ENOMEM);
		goto fail;
	}

	desc->nb_objects = num_objects;
	desc->objects[0].format_modifier = ((uint64_t)mod_high << 32) | mod_low;

	desc->nb_layers = 1;
	desc->layers[0].format = format;

	/* Allocate a frame */
	AVFrame *f = av_frame_alloc();
	if (!f) {
		err = AVERROR(ENOMEM);
		goto fail;
	}

	/* Set base frame properties */
	ctx->current_frame = f;
	f->width = width;
	f->height = height;
	f->format = AV_PIX_FMT_DRM_PRIME;

	/* Set the frame data to the DRM specific struct */
	f->buf[0] = av_buffer_create((uint8_t*)desc, sizeof(*desc),
			&frame_free, frame, 0);
	if (!f->buf[0]) {
		err = AVERROR(ENOMEM);
		goto fail;
	}

	f->data[0] = (uint8_t*)desc;

	return;

fail:
	ctx->err = err;
	frame_free(frame, (uint8_t *)desc);
}

static void frame_object(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
                         uint32_t index, int32_t fd, uint32_t size,
                         uint32_t offset, uint32_t stride, uint32_t plane_index)
{
    struct capture_context *ctx = data;
    AVFrame *f = ctx->current_frame;
    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)f->data[0];

    desc->objects[index].fd = fd;
    desc->objects[index].size = size;

    desc->layers[0].planes[plane_index].object_index = index;
    desc->layers[0].planes[plane_index].offset = offset;
    desc->layers[0].planes[plane_index].pitch = stride;
}

static enum AVPixelFormat drm_fmt_to_pixfmt(uint32_t fmt) {
    switch (fmt) {
    case DRM_FORMAT_NV12: return AV_PIX_FMT_NV12;
    case DRM_FORMAT_ARGB8888: return AV_PIX_FMT_BGRA;
    case DRM_FORMAT_XRGB8888: return AV_PIX_FMT_BGR0;
    case DRM_FORMAT_ABGR8888: return AV_PIX_FMT_RGBA;
    case DRM_FORMAT_XBGR8888: return AV_PIX_FMT_RGB0;
    case DRM_FORMAT_RGBA8888: return AV_PIX_FMT_ABGR;
    case DRM_FORMAT_RGBX8888: return AV_PIX_FMT_0BGR;
    case DRM_FORMAT_BGRA8888: return AV_PIX_FMT_ARGB;
    case DRM_FORMAT_BGRX8888: return AV_PIX_FMT_0RGB;
    default: return AV_PIX_FMT_NONE;
    };
}

static int attach_drm_frames_ref(struct capture_context *ctx, AVFrame *f,
		enum AVPixelFormat sw_format) {
	int err = 0;
	AVHWFramesContext *hwfc;

	if (ctx->drm_frames_ref) {
		hwfc = (AVHWFramesContext*)ctx->drm_frames_ref->data;
		if (hwfc->width == f->width && hwfc->height == f->height &&
				hwfc->sw_format == sw_format) {
			goto attach;
		}
		av_buffer_unref(&ctx->drm_frames_ref);
	}

	ctx->drm_frames_ref = av_hwframe_ctx_alloc(ctx->drm_device_ref);
	if (!ctx->drm_frames_ref) {
		err = AVERROR(ENOMEM);
		goto fail;
	}

	hwfc = (AVHWFramesContext*)ctx->drm_frames_ref->data;

	hwfc->format = f->format;
	hwfc->sw_format = sw_format;
	hwfc->width = f->width;
	hwfc->height = f->height;

	err = av_hwframe_ctx_init(ctx->drm_frames_ref);
	if (err) {
		av_log(ctx, AV_LOG_ERROR, "AVHWFramesContext init failed: %s!\n",
				av_err2str(err));
		goto fail;
	}

attach:
	/* Set frame hardware context referencce */
	f->hw_frames_ctx = av_buffer_ref(ctx->drm_frames_ref);
	if (!f->hw_frames_ctx) {
		err = AVERROR(ENOMEM);
		goto fail;
	}

	return 0;

fail:
	av_buffer_unref(&ctx->drm_frames_ref);
	return err;
}

static void register_cb(struct capture_context *ctx);

static void frame_ready(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
		uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
	struct capture_context *ctx = data;
	AVFrame *f = ctx->current_frame;
	AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)f->data[0];
	enum AVPixelFormat pix_fmt = drm_fmt_to_pixfmt(desc->layers[0].format);
	int err = 0;

	/* Timestamp, nanoseconds timebase */
	f->pts = ((((uint64_t)tv_sec_hi) << 32) | tv_sec_lo) * 1000000000 + tv_nsec;

	if (!ctx->vid_start_pts) {
		ctx->vid_start_pts = f->pts;
	}

	f->pts = av_rescale_q(f->pts - ctx->vid_start_pts, (AVRational){ 1, 1000000000 },
			ctx->video_avctx->time_base);

	/* Attach the hardware frame context to the frame */
	err = attach_drm_frames_ref(ctx, f, pix_fmt);
	if (err) {
		goto end;
	}

	/* TODO: support multiplane stuff */
	desc->layers[0].nb_planes = av_pix_fmt_count_planes(pix_fmt);

	AVFrame *mapped_frame = av_frame_alloc();
	if (!mapped_frame) {
		err = AVERROR(ENOMEM);
		goto end;
	}

	AVHWFramesContext *mapped_hwfc;
	mapped_hwfc = (AVHWFramesContext *)ctx->mapped_frames_ref->data;
	mapped_frame->format = mapped_hwfc->format;
	mapped_frame->pts = f->pts;

	/* Set frame hardware context referencce */
	mapped_frame->hw_frames_ctx = av_buffer_ref(ctx->mapped_frames_ref);
	if (!mapped_frame->hw_frames_ctx) {
		err = AVERROR(ENOMEM);
		goto end;
	}

    err = av_hwframe_map(mapped_frame, f, 0);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Error mapping: %s!\n", av_err2str(err));
        goto end;
    }

    if (push_to_fifo(&ctx->video_frames, mapped_frame))
        av_log(ctx, AV_LOG_WARNING, "Dropped video frame!\n");

    if (!ctx->quit && !ctx->err)
        register_cb(ctx);

end:
    ctx->err = err;
    av_frame_free(&ctx->current_frame);
}

static void frame_cancel(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
		uint32_t reason) {
	struct capture_context *ctx = data;
	av_log(ctx, AV_LOG_WARNING, "Frame cancelled!\n");
	av_frame_free(&ctx->current_frame);
	if (reason == ZWLR_EXPORT_DMABUF_FRAME_V1_CANCEL_REASON_PERMANENT) {
		av_log(ctx, AV_LOG_ERROR, "Permanent failure, exiting\n");
		ctx->err = true;
	} else {
		register_cb(ctx);
	}
}

static const struct zwlr_export_dmabuf_frame_v1_listener frame_listener = {
	.frame = frame_start,
	.object = frame_object,
	.ready = frame_ready,
	.cancel = frame_cancel,
};

static void register_cb(struct capture_context *ctx) {
	ctx->frame_callback = zwlr_export_dmabuf_manager_v1_capture_output(
			ctx->export_manager, 0, ctx->target_output);

	zwlr_export_dmabuf_frame_v1_add_listener(ctx->frame_callback,
			&frame_listener, ctx);
}

void *video_encode_thread(void *arg) {
	int err = 0;
	struct capture_context *ctx = arg;

	do {
		AVFrame *f = NULL;
		if (get_fifo_size(&ctx->video_frames) || !ctx->quit)
			f = pop_from_fifo(&ctx->video_frames);

		if (ctx->is_software_encoder && f) {
			AVFrame *soft_frame = av_frame_alloc();
			av_hwframe_transfer_data(soft_frame, f, 0);
			soft_frame->pts = f->pts;
			av_frame_free(&f);
			f = soft_frame;
		}

		err = avcodec_send_frame(ctx->video_avctx, f);

		av_frame_free(&f);

		if (err) {
			av_log(ctx, AV_LOG_ERROR, "Error encoding: %s!\n", av_err2str(err));
			goto end;
		}

		while (1) {
			AVPacket pkt;
			av_init_packet(&pkt);

			int ret = avcodec_receive_packet(ctx->video_avctx, &pkt);
			if (ret == AVERROR(EAGAIN)) {
				break;
			} else if (ret == AVERROR_EOF) {
				av_log(ctx, AV_LOG_INFO, "Encoder flushed (video)!\n");
				goto end;
			} else if (ret) {
				av_log(ctx, AV_LOG_ERROR, "Error encoding: %s!\n",
						av_err2str(ret));
				err = ret;
				goto end;
			}

			pkt.stream_index = 0;

			pthread_mutex_lock(&ctx->avf_lock);
			err = av_interleaved_write_frame(ctx->avf, &pkt);
			pthread_mutex_unlock(&ctx->avf_lock);

			av_packet_unref(&pkt);

			if (err) {
				av_log(ctx, AV_LOG_ERROR, "Writing video packet fail: %s!\n",
						av_err2str(err));
				goto end;
			}
		};

		av_log(ctx, AV_LOG_INFO, "Encoded video frame %i (%i in queue)\n",
				ctx->video_avctx->frame_number, get_fifo_size(&ctx->video_frames));

	} while (!ctx->err);

end:
	if (!ctx->err)
		ctx->err = err;
	return NULL;
}

static int init_lavu_hwcontext(struct capture_context *ctx) {
	/* DRM hwcontext */
	ctx->drm_device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_DRM);
	if (!ctx->drm_device_ref)
		return AVERROR(ENOMEM);

	AVHWDeviceContext *ref_data = (AVHWDeviceContext*)ctx->drm_device_ref->data;
	AVDRMDeviceContext *hwctx = ref_data->hwctx;

	/* We don't need a device (we don't even know it and can't open it) */
	hwctx->fd = -1;

	av_hwdevice_ctx_init(ctx->drm_device_ref);

	/* Mapped hwcontext */
	int err = av_hwdevice_ctx_create(&ctx->mapped_device_ref,
			ctx->hw_device_type, ctx->hardware_device, NULL, 0);
	if (err < 0) {
		av_log(ctx, AV_LOG_ERROR, "Failed to create a hardware device: %s\n",
				av_err2str(err));
		return err;
	}

	return 0;
}

static int set_hwframe_ctx(struct capture_context *ctx,
                           AVBufferRef *hw_device_ctx)
{
	AVHWFramesContext *frames_ctx = NULL;
	int err = 0;

	if (!(ctx->mapped_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx)))
		return AVERROR(ENOMEM);

	AVHWFramesConstraints *cst;
	cst = av_hwdevice_get_hwframe_constraints(ctx->mapped_device_ref, NULL);
	if (!cst) {
		av_log(ctx, AV_LOG_ERROR, "Failed to get hw device constraints!\n");
		av_buffer_unref(&ctx->mapped_frames_ref);
		return AVERROR(ENOMEM);
	}

	frames_ctx = (AVHWFramesContext *)(ctx->mapped_frames_ref->data);
	frames_ctx->format = cst->valid_hw_formats[0];
	frames_ctx->sw_format = ctx->video_avctx->pix_fmt;
	frames_ctx->width = ctx->video_avctx->width;
	frames_ctx->height = ctx->video_avctx->height;

	av_hwframe_constraints_free(&cst);

	if ((err = av_hwframe_ctx_init(ctx->mapped_frames_ref))) {
		av_log(ctx, AV_LOG_ERROR, "Failed to initialize hw frame context: %s!\n",
				av_err2str(err));
		av_buffer_unref(&ctx->mapped_frames_ref);
		return err;
	}

	if (!ctx->is_software_encoder) {
		ctx->video_avctx->pix_fmt = frames_ctx->format;
		ctx->video_avctx->hw_frames_ctx = av_buffer_ref(ctx->mapped_frames_ref);
		if (!ctx->video_avctx->hw_frames_ctx) {
			av_buffer_unref(&ctx->mapped_frames_ref);
			err = AVERROR(ENOMEM);
		}
	}

	return err;
}

static int setup_video_avctx(struct capture_context *ctx) {
    AVCodec *codec = avcodec_find_encoder_by_name(ctx->video_encoder);
    if (!codec) {
        av_log(ctx, AV_LOG_ERROR, "Codec not found (not compiled in lavc?)!\n");
        return AVERROR(EINVAL);
    }
    ctx->avf->oformat->video_codec = codec->id;
    ctx->is_software_encoder = !(codec->capabilities & AV_CODEC_CAP_HARDWARE);

    ctx->video_avctx = avcodec_alloc_context3(codec);
    if (!ctx->video_avctx)
        return 1;

    ctx->video_avctx->opaque            = ctx;
    ctx->video_avctx->bit_rate          = (int)ctx->video_bitrate*1000000.0f;
    ctx->video_avctx->pix_fmt           = ctx->video_sw_format;
    ctx->video_avctx->time_base         = (AVRational){ 1, 1000 };
    ctx->video_avctx->compression_level = 7;
    ctx->video_avctx->width             = find_output(ctx, ctx->target_output, 0)->width;
    ctx->video_avctx->height            = find_output(ctx, ctx->target_output, 0)->height;

	if (ctx->avf->oformat->flags & AVFMT_GLOBALHEADER) {
		ctx->video_avctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	/* Init hw frames context */
	int err = set_hwframe_ctx(ctx, ctx->mapped_device_ref);
	if (err) {
		return err;
	}

	err = avcodec_open2(ctx->video_avctx, codec, &ctx->video_encoder_opts);
	if (err) {
		av_log(ctx, AV_LOG_ERROR, "Cannot open encoder: %s!\n",
				av_err2str(err));
		return err;
	}

	return 0;
}

static uint64_t get_codec_channel_layout(AVCodec *codec)
{
    int i = 0;
    if (!codec->channel_layouts)
        return AV_CH_LAYOUT_STEREO;
    while (1) {
        if (!codec->channel_layouts[i])
            break;
        if (codec->channel_layouts[i] == AV_CH_LAYOUT_STEREO)
            return codec->channel_layouts[i];
        i++;
    }
    return codec->channel_layouts[0];
}

static enum AVSampleFormat get_codec_sample_fmt(AVCodec *codec)
{
    int i = 0;
    if (!codec->sample_fmts)
        return AV_SAMPLE_FMT_S16;
    while (1) {
        if (codec->sample_fmts[i] == -1)
            break;
        if (av_get_bytes_per_sample(codec->sample_fmts[i]) >= 2)
            return codec->sample_fmts[i];
        i++;
    }
    return codec->sample_fmts[0];
}

static int setup_audio_avctx(struct capture_context *ctx)
{
	int err;
	AVCodec *codec = avcodec_find_encoder_by_name(ctx->audio_encoder);
	if (!codec) {
		av_log(ctx, AV_LOG_ERROR, "Codec not found (not compiled in lavc?)!\n");
		return AVERROR(EINVAL);
	}
	ctx->avf->oformat->audio_codec = codec->id;

    ctx->audio_avctx = avcodec_alloc_context3(codec);
    if (!ctx->audio_avctx)
        return AVERROR(ENOMEM);

    ctx->audio_avctx->opaque            = ctx;
    ctx->audio_avctx->bit_rate          = lrintf(ctx->audio_bitrate*1000.0f);
    ctx->audio_avctx->sample_fmt        = get_codec_sample_fmt(codec);
    ctx->audio_avctx->channel_layout    = get_codec_channel_layout(codec);
    ctx->audio_avctx->sample_rate       = ctx->audio_samplerate;
    ctx->audio_avctx->time_base         = (AVRational){ 1, 1000 };
    ctx->audio_avctx->channels          = av_get_channel_layout_nb_channels(ctx->audio_avctx->channel_layout);

    if (ctx->avf->oformat->flags & AVFMT_GLOBALHEADER)
        ctx->audio_avctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    err = avcodec_open2(ctx->audio_avctx, codec, &ctx->audio_encoder_opts);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Cannot open encoder: %s!\n",
               av_err2str(err));
        return err;
    }

    return 0;
}

static int init_encoding(struct capture_context *ctx) {
	int err;

	/* lavf init */
    err = avformat_alloc_output_context2(&ctx->avf, NULL, NULL,
	                                     ctx->out_filename);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Unable to init lavf context!\n");
        return err;
    }

	{ /* Video output stream */
        AVStream *st = avformat_new_stream(ctx->avf, NULL);
        if (!st) {
            av_log(ctx, AV_LOG_ERROR, "Unable to alloc stream!\n");
            return 1;
        }

        if ((err = setup_video_avctx(ctx)))
            return err;

        st->id = 0;
        st->time_base = ctx->video_avctx->time_base;
        st->avg_frame_rate = find_output(ctx, ctx->target_output, 0)->framerate;

        err = avcodec_parameters_from_context(st->codecpar, ctx->video_avctx);
		if (err) {
			av_log(ctx, AV_LOG_ERROR, "Couldn't copy codec params: %s!\n",
					av_err2str(err));
			return err;
		}
	}

    { /* Audio output stream */
        AVStream *st = avformat_new_stream(ctx->avf, NULL);
		if (!st) {
			av_log(ctx, AV_LOG_ERROR, "Unable to alloc stream!\n");
			return 1;
		}

        if ((err = setup_audio_avctx(ctx)))
            return err;

        st->id = 0;
		st->time_base = ctx->audio_avctx->time_base;

        err = avcodec_parameters_from_context(st->codecpar, ctx->audio_avctx);
		if (err) {
			av_log(ctx, AV_LOG_ERROR, "Couldn't copy codec params: %s!\n",
					av_err2str(err));
			return err;
		}
    }

    /* Debug print */
    av_dump_format(ctx->avf, 0, ctx->out_filename, 1);

	/* Open for writing */
    err = avio_open(&ctx->avf->pb, ctx->out_filename, AVIO_FLAG_WRITE);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Couldn't open %s: %s!\n", ctx->out_filename,
               av_err2str(err));
        return err;
    }

	err = avformat_write_header(ctx->avf, NULL);
	if (err) {
		av_log(ctx, AV_LOG_ERROR, "Couldn't write header: %s!\n", av_err2str(err));
		return err;
	}

    pthread_mutex_init(&ctx->avf_lock, NULL);

	return err;
}

void *audio_encode_thread(void *arg)
{
	struct capture_context *ctx = arg;
	int err = 0;

	do {
	    AVFrame *in_f = NULL;
	    if (get_fifo_size(&ctx->audio_frames) || !ctx->quit) {
	        in_f = pop_from_fifo(&ctx->audio_frames);
	        int os = swr_get_out_samples(ctx->swr_ctx, in_f->nb_samples);
	        if ((os < ctx->audio_avctx->frame_size) && !ctx->quit) {
	            swr_convert_frame(ctx->swr_ctx, NULL, in_f);
	            av_frame_free(&in_f);
	            continue;
	        }
        }

        /* Resampled frame */
        AVFrame *out_f = av_frame_alloc();
        out_f->format = ctx->audio_avctx->sample_fmt;
        out_f->sample_rate = ctx->audio_avctx->sample_rate;
        out_f->channel_layout = ctx->audio_avctx->channel_layout;
        out_f->pts = ROUNDED_DIV(swr_next_pts(ctx->swr_ctx, INT64_MIN), 24000);
        if (in_f) { /* Last frame */
            out_f->nb_samples = ctx->audio_avctx->frame_size;
            av_frame_get_buffer(out_f, 0);
        }

        /* Resample */
    	swr_convert_frame(ctx->swr_ctx, out_f, in_f);
    	av_frame_free(&in_f);

        /* Flush */
        if (!out_f->nb_samples)
            av_frame_free(&out_f);

        /* Encode */
        err = avcodec_send_frame(ctx->audio_avctx, out_f);
        av_frame_free(&out_f);

        if (err) {
            av_log(ctx, AV_LOG_ERROR, "Error encoding: %s!\n", av_err2str(err));
            goto end;
        }

		while (1) {
			AVPacket pkt;
			av_init_packet(&pkt);

			int ret = avcodec_receive_packet(ctx->audio_avctx, &pkt);
			if (ret == AVERROR(EAGAIN)) {
				break;
			} else if (ret == AVERROR_EOF) {
				av_log(ctx, AV_LOG_INFO, "Encoder flushed (audio)!\n");
				goto end;
			} else if (ret) {
				av_log(ctx, AV_LOG_ERROR, "Error encoding: %s!\n",
						av_err2str(ret));
				err = ret;
				goto end;
			}

			pkt.stream_index = 1;

            pthread_mutex_lock(&ctx->avf_lock);
			err = av_interleaved_write_frame(ctx->avf, &pkt);
			pthread_mutex_unlock(&ctx->avf_lock);

			av_packet_unref(&pkt);

			if (err) {
				av_log(ctx, AV_LOG_ERROR, "Writing audio packet fail: %s!\n",
						av_err2str(err));
				goto end;
			}
		};

		av_log(ctx, AV_LOG_INFO, "Encoded audio frame %i (%i in queue)\n",
				ctx->audio_avctx->frame_number, get_fifo_size(&ctx->audio_frames));
    } while (!ctx->err);

    return NULL;

end:
    if (!ctx->err)
        ctx->err = err;
    return NULL;
}

static void pulse_stream_read_cb(pa_stream *stream, size_t size,
                                 void *data)
{
    struct capture_context *ctx = data;

    uint8_t *buffer;
    pa_stream_peek(stream, (const void **)&buffer, &size);
    pa_stream_drop(stream);

    AVFrame *f = av_frame_alloc();
    f->sample_rate    = ctx->audio_samplerate;
    f->format         = AV_SAMPLE_FMT_FLT;
    f->channel_layout = AV_CH_LAYOUT_STEREO;
    f->data[0]        = buffer;
    f->nb_samples     = size >> 3;

    av_log(ctx, AV_LOG_WARNING, "Got %i samples (%i)!\n", f->nb_samples, get_fifo_size(&ctx->audio_frames));

    if (push_to_fifo(&ctx->audio_frames, f))
        av_log(ctx, AV_LOG_WARNING, "Dropped audio frame!\n");

    if (ctx->quit)
        pa_stream_disconnect(stream);
}

static void pulse_stream_status_cb(pa_stream *stream, void *data)
{
    struct capture_context *ctx = data;

    switch (pa_stream_get_state(stream)) {
    case PA_STREAM_UNCONNECTED:
        av_log(ctx, AV_LOG_INFO, "PulseAudio stream currently unconnected!\n");
        break;
    case PA_STREAM_CREATING:
        av_log(ctx, AV_LOG_INFO, "PulseAudio stream being created!\n");
        break;
    case PA_STREAM_READY:
        av_log(ctx, AV_LOG_INFO, "PulseAudio stream now ready!\n");
        break;
    case PA_STREAM_FAILED:
        av_log(ctx, AV_LOG_INFO, "PulseAudio stream connection failed!\n");
        break;
    case PA_STREAM_TERMINATED:
        av_log(ctx, AV_LOG_INFO, "PulseAudio stream terminated!\n");
        pa_context_disconnect(ctx->pa_context);
        break;
    }
}

static void pulse_sink_info_cb(pa_context *context, const pa_sink_info *info,
                               int is_last, void *data)
{
    struct capture_context *ctx = data;

    if (is_last)
        return;

    av_log(ctx, AV_LOG_INFO, "Starting streaming from \"%s\"\n",
           info->description);

    pa_stream *stream;
    pa_buffer_attr attr;
    pa_channel_map map;

    ctx->pa_spec.format   = PA_SAMPLE_FLOAT32LE;
    ctx->pa_spec.rate     = ctx->audio_samplerate;
    ctx->pa_spec.channels = 2;
    pa_channel_map_init_stereo(&map);

    attr.maxlength = 1024*32;
    attr.fragsize  = -1;

    stream = pa_stream_new(context, "dmabuf-capture", &ctx->pa_spec, &map);

    /* Set stream callbacks */
    pa_stream_set_state_callback(stream, pulse_stream_status_cb, ctx);
    pa_stream_set_read_callback(stream, pulse_stream_read_cb, ctx);

    /* Start stream */
    pa_stream_connect_record(stream, info->monitor_source_name, &attr,
                             PA_STREAM_ADJUST_LATENCY |
                             PA_STREAM_FIX_RATE |
                             PA_STREAM_FIX_CHANNELS);
}

static void pulse_server_info_cb(pa_context *context, const pa_server_info *info,
                                 void *data)
{
    struct capture_context *ctx = data;

    pa_operation *op;
    op = pa_context_get_sink_info_by_name(context, info->default_sink_name,
                                          pulse_sink_info_cb, ctx);
    pa_operation_unref(op);
}

static void pulse_state_cb(pa_context *context, void *data)
{
    struct capture_context *ctx = data;

    switch (pa_context_get_state(context)) {
    case PA_CONTEXT_UNCONNECTED:
        av_log(ctx, AV_LOG_INFO, "PulseAudio reports it is unconnected!\n");
        break;
    case PA_CONTEXT_CONNECTING:
        av_log(ctx, AV_LOG_INFO, "Connecting to PulseAudio!\n");
        break;
    case PA_CONTEXT_AUTHORIZING:
        av_log(ctx, AV_LOG_INFO, "Authorizing PulseAudio connection!\n");
        break;
    case PA_CONTEXT_SETTING_NAME:
        av_log(ctx, AV_LOG_INFO, "Sending client name!\n");
        break;
    case PA_CONTEXT_FAILED:
        av_log(ctx, AV_LOG_INFO, "PulseAudio connection failed!\n");
        break;
    case PA_CONTEXT_TERMINATED:
        av_log(ctx, AV_LOG_INFO, "PulseAudio connection terminated!\n");
        break;
    case PA_CONTEXT_READY:
        av_log(ctx, AV_LOG_INFO, "PulseAudio connection ready!\n");
        pa_operation_unref(pa_context_get_server_info(context,
                           pulse_server_info_cb, ctx));
        break;
    }
}

static int init_pulse(struct capture_context *ctx)
{
    ctx->pa_mainloop = pa_threaded_mainloop_new();
    ctx->pa_mainloop_api = pa_threaded_mainloop_get_api(ctx->pa_mainloop);

    ctx->pa_context = pa_context_new(ctx->pa_mainloop_api, "dmabuf-capture");
    pa_context_set_state_callback(ctx->pa_context, pulse_state_cb, ctx);
    pa_context_connect(ctx->pa_context, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL);

    return 0;
}

static int init_swr(struct capture_context *ctx)
{
    ctx->swr_ctx = swr_alloc();
    if (!ctx->swr_ctx) {
        av_log(ctx, AV_LOG_ERROR, "Could not alloc swr context!\n");
        return AVERROR(ENOMEM);
    }

    av_opt_set_int           (ctx->swr_ctx, "in_sample_rate",     ctx->audio_samplerate,            0);
    av_opt_set_channel_layout(ctx->swr_ctx, "in_channel_layout",  AV_CH_LAYOUT_STEREO,              0);
    av_opt_set_sample_fmt    (ctx->swr_ctx, "in_sample_fmt",      AV_SAMPLE_FMT_FLT,                0);

    av_opt_set_int           (ctx->swr_ctx, "out_sample_rate",    ctx->audio_avctx->sample_rate,    0);
    av_opt_set_channel_layout(ctx->swr_ctx, "out_channel_layout", ctx->audio_avctx->channel_layout, 0);
    av_opt_set_sample_fmt    (ctx->swr_ctx, "out_sample_fmt",     ctx->audio_avctx->sample_fmt,     0);

    int err = swr_init(ctx->swr_ctx);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Could not init swr context: %s!\n",
               av_err2str(err));
        return err;
    }

    return 0;
}

static int init_audio_capture(struct capture_context *ctx)
{
    int err;

    if ((err = init_pulse(ctx)))
        return err;

    if ((err = init_swr(ctx)))
        return err;

    if ((err = init_fifo(&ctx->audio_frames, ctx->audio_frame_queue)))
        return err;

    pthread_create(&ctx->audio_thread, NULL, audio_encode_thread, ctx);
    pa_threaded_mainloop_start(ctx->pa_mainloop);

    return 0;
}

struct capture_context *q_ctx = NULL;

void on_quit_signal(int signo) {
	printf("\r");
	av_log(q_ctx, AV_LOG_WARNING, "Quitting!\n");
	q_ctx->quit = true;
}

static int main_loop(struct capture_context *ctx) {
	int err;

	q_ctx = ctx;

	if (signal(SIGINT, on_quit_signal) == SIG_ERR) {
		av_log(ctx, AV_LOG_ERROR, "Unable to install signal handler!\n");
		return AVERROR(EINVAL);
	}

    if ((err = init_lavu_hwcontext(ctx)))
        return err;

    if ((err = init_encoding(ctx)))
        return err;

    if ((err = init_audio_capture(ctx)))
        return err;

    if ((err = init_fifo(&ctx->video_frames, ctx->video_frame_queue)))
        return err;

    /* Start video/audio encoding threads */
    pthread_create(&ctx->video_thread, NULL, video_encode_thread, ctx);

    /* Start the frame callback */
    register_cb(ctx);

    /* Run capture */
    while (wl_display_dispatch(ctx->display) != -1 && !ctx->err && !ctx->quit);

    /* Join with encoder threads */
    if (ctx->video_thread)
        pthread_join(ctx->video_thread, NULL);
    if (ctx->audio_thread)
        pthread_join(ctx->audio_thread, NULL);

    if ((err = av_write_trailer(ctx->avf))) {
        av_log(ctx, AV_LOG_ERROR, "Error writing trailer: %s!\n",
               av_err2str(err));
        return err;
    }

    av_log(ctx, AV_LOG_INFO, "Wrote trailer!\n");

    return ctx->err;
}

static int init(struct capture_context *ctx)
{
    ctx->display = wl_display_connect(NULL);
    if (!ctx->display) {
        av_log(ctx, AV_LOG_ERROR, "Failed to connect to display!\n");
        return AVERROR(EINVAL);
    }

    wl_list_init(&ctx->output_list);

    ctx->registry = wl_display_get_registry(ctx->display);
    wl_registry_add_listener(ctx->registry, &registry_listener, ctx);

    wl_display_roundtrip(ctx->display);
    wl_display_dispatch(ctx->display);

    if (!ctx->export_manager) {
        av_log(ctx, AV_LOG_ERROR, "Compositor doesn't support %s!\n",
               zwlr_export_dmabuf_manager_v1_interface.name);
        return AVERROR(ENOSYS);
    }

    return 0;
}

static void uninit(struct capture_context *ctx);

int main(int argc, char *argv[])
{
	int err;
	struct capture_context ctx = { 0 };
	ctx.class = &((AVClass) {
		.class_name = "dmabuf-capture",
		.item_name  = av_default_item_name,
		.version    = LIBAVUTIL_VERSION_INT,
	});

	err = init(&ctx);
	if (err) {
		goto end;
	}

	struct wayland_output *o, *tmp_o;
	wl_list_for_each_reverse_safe(o, tmp_o, &ctx.output_list, link) {
		printf("Capturable output: %s Model: %s: ID: %i\n",
				o->make, o->model, o->id);
	}

	if (argc != 8) {
		printf("Invalid number of arguments! Usage and example:\n"
				"./dmabuf-capture <source id> <hardware device type> <device> "
				"<encoder name> <pixel format> <bitrate in Mbps> <file path>\n"
				"./dmabuf-capture 0 vaapi /dev/dri/renderD129 libx264 nv12 12 "
				"dmabuf_recording_01.mkv\n");
		return 1;
	}

	const int o_id = strtol(argv[1], NULL, 10);
	o = find_output(&ctx, NULL, o_id);
	if (!o) {
		printf("Unable to find output with ID %i!\n", o_id);
		return 1;
	}

    ctx.out_filename = argv[7];
	ctx.target_output = o->output;

	ctx.video_encoder = argv[4];
	ctx.video_bitrate = strtof(argv[6], NULL);
	ctx.video_sw_format = av_get_pix_fmt(argv[5]);
	ctx.hw_device_type = av_hwdevice_find_type_by_name(argv[2]);
	ctx.hardware_device = argv[3];
	ctx.video_frame_queue = 16;

    ctx.audio_encoder = "aac";
    ctx.audio_bitrate = 128.0f;
    ctx.audio_samplerate = 48000;
    ctx.audio_frame_queue = 128;

	av_dict_set(&ctx.video_encoder_opts, "preset", "veryfast", 0);

	err = main_loop(&ctx);
	if (err) {
		goto end;
	}

end:
	uninit(&ctx);
	return err;
}

static void uninit(struct capture_context *ctx)
{
    struct wayland_output *output, *tmp_o;
    wl_list_for_each_safe(output, tmp_o, &ctx->output_list, link)
        remove_output(output);

    if (ctx->export_manager)
        zwlr_export_dmabuf_manager_v1_destroy(ctx->export_manager);

    if (ctx->pa_mainloop) {
        pa_threaded_mainloop_stop(ctx->pa_mainloop);
        pa_threaded_mainloop_free(ctx->pa_mainloop);
    }

    free_fifo(&ctx->video_frames);
    free_fifo(&ctx->audio_frames);

    av_buffer_unref(&ctx->drm_frames_ref);
    av_buffer_unref(&ctx->drm_device_ref);
    av_buffer_unref(&ctx->mapped_frames_ref);
    av_buffer_unref(&ctx->mapped_device_ref);

    av_dict_free(&ctx->video_encoder_opts);
    av_dict_free(&ctx->audio_encoder_opts);

    avcodec_close(ctx->video_avctx);
    avcodec_close(ctx->audio_avctx);

    pthread_mutex_lock(&ctx->avf_lock);
    if (ctx->avf)
        avio_closep(&ctx->avf->pb);
    avformat_free_context(ctx->avf);
    pthread_mutex_unlock(&ctx->avf_lock);
}
/* libvpx decoder API (compat/vpx/vpx_decoder.h) on Android MediaCodec, for
 * TOTK's VP9 cutscenes (games/totk/runtime/movie_host.c). The movie host
 * decodes synchronously, one input at a time, and takes at most one frame per
 * input into a free output slot. MediaCodec is asynchronous with some latency,
 * so each decode call queues the input, waits briefly for output, keeps every
 * frame that arrived (copied out as I420) and hands back the oldest one.
 * Decoder: c2.qti.vp9.decoder.low_latency (the Thor's hardware VP9), any VP9
 * decoder as fallback, or TOTK_MOVIE_CODEC=<codec name>. */
#include "vpx/vp8dx.h"
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIME "video/x-vnd.on2.vp9"
#define QUEUED 8
enum { COLOR_PLANAR = 19, COLOR_SEMIPLANAR = 21, COLOR_QCOM_SEMIPLANAR_32M = 0x7fa30c04, COLOR_FLEXIBLE = 0x7f420888 };

typedef struct { uint8_t *data; int64_t pts; } Frame;
typedef struct {
    AMediaCodec *codec; char name[96];
    int w, h, color, stride, slice, crop_l, crop_t;
    int64_t pts; int first;
    Frame q[QUEUED]; int nq;
    uint8_t *cur; vpx_image_t img;
} Dec;

static struct vpx_codec_iface { int unused; } vp9;
vpx_codec_iface_t *vpx_codec_vp9_dx(void) { return &vp9; }
const char *vpx_codec_version_str(void) { return "Android MediaCodec"; }
const char *vpx_codec_err_to_string(vpx_codec_err_t e) {
    return e == VPX_CODEC_OK ? "ok" : e == VPX_CODEC_MEM_ERROR ? "out of memory" : e == VPX_CODEC_UNSUP_BITSTREAM ? "unsupported" : "MediaCodec error";
}
const char *vpx_codec_error_detail(const vpx_codec_ctx_t *ctx) { return ctx->err_detail; }

static vpx_codec_err_t fail(vpx_codec_ctx_t *ctx, const char *what, int status) {
    static char text[160];
    snprintf(text, sizeof text, "%s (media status %d)", what, status);
    ctx->err_detail = text;
    fprintf(stderr, "[movie] MediaCodec: %s\n", text);
    return VPX_CODEC_ERROR;
}

vpx_codec_err_t vpx_codec_dec_init(vpx_codec_ctx_t *ctx, vpx_codec_iface_t *iface, const vpx_codec_dec_cfg_t *cfg, long flags) {
    (void)iface; (void)flags;
    memset(ctx, 0, sizeof *ctx);
    Dec *d = calloc(1, sizeof *d);
    if (!d) return VPX_CODEC_MEM_ERROR;
    const char *want = getenv("TOTK_MOVIE_CODEC");
    snprintf(d->name, sizeof d->name, "%s", want && *want ? want : "c2.qti.vp9.decoder.low_latency");
    d->codec = AMediaCodec_createCodecByName(d->name);
    if (!d->codec) { snprintf(d->name, sizeof d->name, "default %s decoder", MIME); d->codec = AMediaCodec_createDecoderByType(MIME); }
    if (!d->codec) { free(d); return fail(ctx, "no VP9 decoder", 0); }
    d->w = (int)cfg->w; d->h = (int)cfg->h; d->first = 1;
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, MIME);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, d->w);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, d->h);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, COLOR_FLEXIBLE);
    AMediaFormat_setInt32(f, "low-latency", 1);
    media_status_t st = AMediaCodec_configure(d->codec, f, NULL, NULL, 0);
    AMediaFormat_delete(f);
    if (st == AMEDIA_OK) st = AMediaCodec_start(d->codec);
    if (st != AMEDIA_OK) { AMediaCodec_delete(d->codec); free(d); return fail(ctx, "configure/start", st); }
    ctx->priv = d;
    fprintf(stderr, "[movie] VP9 %dx%d on %s\n", d->w, d->h, d->name);
    return VPX_CODEC_OK;
}

static void read_format(Dec *d) {
    AMediaFormat *f = AMediaCodec_getOutputFormat(d->codec);
    int32_t v, l = 0, t = 0;
    if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, &v)) d->color = v;
    d->stride = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_STRIDE, &v) && v > 0 ? v : d->w;
    d->slice = AMediaFormat_getInt32(f, "slice-height", &v) && v > 0 ? v : d->h;
    if (AMediaFormat_getInt32(f, "crop-left", &l) && AMediaFormat_getInt32(f, "crop-top", &t)) { d->crop_l = l; d->crop_t = t; }
    fprintf(stderr, "[movie] output: color format %#x, stride %d, slice height %d, crop %d,%d\n", d->color, d->stride, d->slice, d->crop_l, d->crop_t);
    AMediaFormat_delete(f);
}

/* One decoded buffer -> a packed I420 frame (w x h). */
static int keep(Dec *d, const uint8_t *buf, size_t size, int64_t pts) {
    if (d->nq == QUEUED) return 0;
    size_t ysize = (size_t)d->w * d->h, csize = (size_t)(d->w / 2) * (d->h / 2);
    uint8_t *out = malloc(ysize + 2 * csize);
    if (!out) return 0;
    const uint8_t *y = buf + (size_t)d->crop_t * d->stride + d->crop_l;
    size_t yplane = (size_t)d->stride * d->slice;
    for (int r = 0; r < d->h; ++r) memcpy(out + (size_t)r * d->w, y + (size_t)r * d->stride, (size_t)d->w);
    uint8_t *u = out + ysize, *v = u + csize;
    int cw = d->w / 2, ch = d->h / 2;
    if (d->color == COLOR_PLANAR) {
        size_t cstride = (size_t)d->stride / 2, cplane = cstride * (d->slice / 2);
        const uint8_t *su = buf + yplane + (size_t)(d->crop_t / 2) * cstride + d->crop_l / 2, *sv = su + cplane;
        if (yplane + 2 * cplane > size) return 0;
        for (int r = 0; r < ch; ++r) { memcpy(u + (size_t)r * cw, su + r * cstride, (size_t)cw); memcpy(v + (size_t)r * cw, sv + r * cstride, (size_t)cw); }
    } else {   /* semi-planar: interleaved U, V after the Y plane */
        const uint8_t *uv = buf + yplane + (size_t)(d->crop_t / 2) * d->stride + d->crop_l;
        if (yplane + (size_t)d->stride * (d->slice / 2) > size) { free(out); return 0; }
        for (int r = 0; r < ch; ++r) {
            const uint8_t *s = uv + (size_t)r * d->stride;
            for (int c = 0; c < cw; ++c) { u[(size_t)r * cw + c] = s[2 * c]; v[(size_t)r * cw + c] = s[2 * c + 1]; }
        }
    }
    d->q[d->nq++] = (Frame){out, pts};
    return 1;
}

/* Collect output: wait up to timeout_us for the first buffer, then take whatever else is ready. */
static void drain(vpx_codec_ctx_t *ctx, Dec *d, int64_t timeout_us) {
    for (;;) {
        AMediaCodecBufferInfo info;
        ssize_t i = AMediaCodec_dequeueOutputBuffer(d->codec, &info, timeout_us);
        if (i == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) { read_format(d); continue; }
        if (i == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (i < 0) return;
        size_t cap; const uint8_t *buf = AMediaCodec_getOutputBuffer(d->codec, (size_t)i, &cap);
        if (!d->color) read_format(d);
        if (buf && info.size > 0 && !keep(d, buf + info.offset, (size_t)info.size, info.presentationTimeUs))
            fail(ctx, d->nq == QUEUED ? "decoded frames piling up" : "unexpected output buffer layout", (int)info.size);
        AMediaCodec_releaseOutputBuffer(d->codec, (size_t)i, false);
        timeout_us = 0;
    }
}

vpx_codec_err_t vpx_codec_decode(vpx_codec_ctx_t *ctx, const uint8_t *data, unsigned int size, void *user, long deadline) {
    (void)user; (void)deadline;
    Dec *d = ctx->priv;
    if (!d) return VPX_CODEC_ERROR;
    ssize_t i = AMediaCodec_dequeueInputBuffer(d->codec, 500000);
    if (i < 0) return fail(ctx, "no input buffer", (int)i);
    size_t cap; uint8_t *buf = AMediaCodec_getInputBuffer(d->codec, (size_t)i, &cap);
    if (!buf || cap < size) return fail(ctx, "input buffer too small", (int)cap);
    memcpy(buf, data, size);
    media_status_t st = AMediaCodec_queueInputBuffer(d->codec, (size_t)i, 0, size, (uint64_t)(d->pts += 33333), 0);
    if (st != AMEDIA_OK) return fail(ctx, "queue input", st);
    drain(ctx, d, d->nq ? 0 : d->first ? 300000 : 40000);
    if (d->nq) d->first = 0;
    return VPX_CODEC_OK;
}

vpx_image_t *vpx_codec_get_frame(vpx_codec_ctx_t *ctx, vpx_codec_iter_t *iter) {
    Dec *d = ctx->priv;
    if (!d || *iter || !d->nq) return NULL;
    free(d->cur);
    d->cur = d->q[0].data;
    memmove(d->q, d->q + 1, (size_t)--d->nq * sizeof *d->q);
    d->img = (vpx_image_t){.fmt = VPX_IMG_FMT_I420, .w = (unsigned)d->w, .h = (unsigned)d->h, .d_w = (unsigned)d->w, .d_h = (unsigned)d->h,
        .planes = {d->cur, d->cur + (size_t)d->w * d->h, d->cur + (size_t)d->w * d->h + (size_t)(d->w / 2) * (d->h / 2)},
        .stride = {d->w, d->w / 2, d->w / 2}};
    *iter = d;
    return &d->img;
}

vpx_codec_err_t vpx_codec_destroy(vpx_codec_ctx_t *ctx) {
    Dec *d = ctx->priv;
    if (!d) return VPX_CODEC_OK;
    AMediaCodec_stop(d->codec); AMediaCodec_delete(d->codec);
    for (int i = 0; i < d->nq; ++i) free(d->q[i].data);
    free(d->cur); free(d);
    ctx->priv = NULL;
    return VPX_CODEC_OK;
}

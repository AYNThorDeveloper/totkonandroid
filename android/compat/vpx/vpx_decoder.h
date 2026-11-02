/* The part of libvpx's decoder API that games/totk/runtime/movie_host.c uses,
 * implemented on Android's own VP9 decoder (NDK MediaCodec) by
 * android/android_vpx_mediacodec.c. Decoded frames come back as I420. */
#ifndef ANDROID_COMPAT_VPX_DECODER_H
#define ANDROID_COMPAT_VPX_DECODER_H
#include <stdint.h>

typedef enum { VPX_CODEC_OK = 0, VPX_CODEC_ERROR = 1, VPX_CODEC_MEM_ERROR = 2, VPX_CODEC_UNSUP_BITSTREAM = 3 } vpx_codec_err_t;
typedef enum { VPX_IMG_FMT_I420 = 0x102 } vpx_img_fmt_t;
enum { VPX_PLANE_Y = 0, VPX_PLANE_U = 1, VPX_PLANE_V = 2 };
typedef struct { vpx_img_fmt_t fmt; unsigned int w, h, d_w, d_h; unsigned char *planes[4]; int stride[4]; } vpx_image_t;
typedef const void *vpx_codec_iter_t;
typedef struct vpx_codec_iface vpx_codec_iface_t;
typedef struct { unsigned int threads, w, h; } vpx_codec_dec_cfg_t;
typedef struct { void *priv; const char *err_detail; } vpx_codec_ctx_t;

vpx_codec_err_t vpx_codec_dec_init(vpx_codec_ctx_t *ctx, vpx_codec_iface_t *iface, const vpx_codec_dec_cfg_t *cfg, long flags);
vpx_codec_err_t vpx_codec_decode(vpx_codec_ctx_t *ctx, const uint8_t *data, unsigned int size, void *user, long deadline);
vpx_image_t *vpx_codec_get_frame(vpx_codec_ctx_t *ctx, vpx_codec_iter_t *iter);
vpx_codec_err_t vpx_codec_destroy(vpx_codec_ctx_t *ctx);
const char *vpx_codec_error_detail(const vpx_codec_ctx_t *ctx);
const char *vpx_codec_err_to_string(vpx_codec_err_t err);
const char *vpx_codec_version_str(void);
#endif

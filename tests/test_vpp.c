/*
 * test_vpp.c — VAEntrypointVideoProc blit coverage.
 *
 * Chromium reaches this path through VaapiImageProcessorBackend whenever the
 * decoded NV12/P010 surface cannot be imported into EGL directly: it then asks
 * for AR24/BGR4 output, and it asks for it *with* a source rectangle and a
 * different output size. The driver used to reject any request whose source and
 * destination regions were not identical and origin-aligned, returning
 * VA_STATUS_ERROR_OPERATION_FAILED — at which point Chromium drops the whole
 * stream to software decode rather than retrying differently.
 *
 * These tests cover the three shapes a client actually asks for:
 *   1. 1:1 blit (the fast path, must not regress)
 *   2. crop — a sub-rectangle of the source, same scale
 *   3. scale — a different output size, with and without a crop
 *
 * Correctness of the colour conversion itself is covered by the existing
 * decode/ffmpeg tests; what is checked here is that the geometry is honoured,
 * by painting a known pattern and reading back specific pixels.
 */

#include "test_common.h"
#include <va/va_drmcommon.h>

/* Fill an NV12 surface with a horizontal luma ramp: Y = x * 255 / (w-1),
 * flat chroma. A ramp makes it possible to tell where in the source a given
 * output pixel was sampled from. */
static bool paint_luma_ramp(VASurfaceID surface, uint32_t w, uint32_t h) {
    VAImageFormat fmt = { .fourcc = VA_FOURCC_NV12 };
    VAImage image;
    if (vaCreateImage(g_dpy, &fmt, w, h, &image) != VA_STATUS_SUCCESS) {
        return false;
    }
    unsigned char *base = NULL;
    if (vaMapBuffer(g_dpy, image.buf, (void **) &base) != VA_STATUS_SUCCESS) {
        vaDestroyImage(g_dpy, image.image_id);
        return false;
    }
    unsigned char *y = base + image.offsets[0];
    for (uint32_t row = 0; row < h; row++) {
        for (uint32_t col = 0; col < w; col++) {
            y[(size_t) row * image.pitches[0] + col] =
                (unsigned char) ((col * 255u) / (w > 1 ? w - 1 : 1));
        }
    }
    unsigned char *uv = base + image.offsets[1];
    for (uint32_t row = 0; row < h / 2; row++) {
        memset(uv + (size_t) row * image.pitches[1], 128, w);
    }
    vaUnmapBuffer(g_dpy, image.buf);

    VAStatus st = vaPutImage(g_dpy, surface, image.image_id,
                             0, 0, w, h, 0, 0, w, h);
    vaDestroyImage(g_dpy, image.image_id);
    return st == VA_STATUS_SUCCESS;
}

/* Read the ARGB destination back and return the R channel of one pixel. */
static bool read_argb_pixel(VASurfaceID surface, uint32_t w, uint32_t h,
                            uint32_t px, uint32_t py, unsigned char *outLuma) {
    VAImageFormat fmt = { .fourcc = VA_FOURCC_ARGB };
    VAImage image;
    if (vaCreateImage(g_dpy, &fmt, w, h, &image) != VA_STATUS_SUCCESS) {
        return false;
    }
    if (vaGetImage(g_dpy, surface, 0, 0, w, h, image.image_id) != VA_STATUS_SUCCESS) {
        vaDestroyImage(g_dpy, image.image_id);
        return false;
    }
    unsigned char *base = NULL;
    if (vaMapBuffer(g_dpy, image.buf, (void **) &base) != VA_STATUS_SUCCESS) {
        vaDestroyImage(g_dpy, image.image_id);
        return false;
    }
    /* Grey input (flat chroma) means R == G == B, so any channel works as a
     * stand-in for luma. Sample the green byte, which is at the same offset in
     * every 8888 permutation this driver emits. */
    const unsigned char *px4 = base + (size_t) py * image.pitches[0] + (size_t) px * 4;
    *outLuma = px4[1];
    vaUnmapBuffer(g_dpy, image.buf);
    vaDestroyImage(g_dpy, image.image_id);
    return true;
}

/* Run one VPP blit. srcRect/dstRect may be NULL to mean "whole surface". */
static VAStatus run_blit(VASurfaceID src, VASurfaceID dst,
                         const VARectangle *srcRect, const VARectangle *dstRect) {
    VAConfigID config;
    VAStatus st = vaCreateConfig(g_dpy, VAProfileNone, VAEntrypointVideoProc,
                                 NULL, 0, &config);
    if (st != VA_STATUS_SUCCESS) return st;

    VAContextID context;
    st = vaCreateContext(g_dpy, config, 0, 0, VA_PROGRESSIVE, &dst, 1, &context);
    if (st != VA_STATUS_SUCCESS) { vaDestroyConfig(g_dpy, config); return st; }

    VAProcPipelineParameterBuffer pipeline;
    memset(&pipeline, 0, sizeof(pipeline));
    pipeline.surface = src;
    pipeline.surface_region = srcRect;
    pipeline.output_region = dstRect;
    pipeline.filter_flags = VA_FRAME_PICTURE;

    VABufferID buf;
    st = vaCreateBuffer(g_dpy, context, VAProcPipelineParameterBufferType,
                        sizeof(pipeline), 1, &pipeline, &buf);
    if (st != VA_STATUS_SUCCESS) goto out;

    st = vaBeginPicture(g_dpy, context, dst);
    if (st == VA_STATUS_SUCCESS) {
        st = vaRenderPicture(g_dpy, context, &buf, 1);
        VAStatus endSt = vaEndPicture(g_dpy, context);
        if (st == VA_STATUS_SUCCESS) st = endSt;
    }
    vaDestroyBuffer(g_dpy, buf);

out:
    vaDestroyContext(g_dpy, context);
    vaDestroyConfig(g_dpy, config);
    return st;
}

static void test_vpp_identity_blit(void) {
    TEST_START("VPP 1:1 NV12->ARGB blit (fast path must not regress)");

    const uint32_t W = 256, H = 128;
    VASurfaceID src, dst;
    EXPECT_STATUS(vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420, W, H, &src, 1, NULL, 0));
    EXPECT_STATUS(vaCreateSurfaces(g_dpy, VA_RT_FORMAT_RGB32, W, H, &dst, 1, NULL, 0));
    EXPECT_TRUE(paint_luma_ramp(src, W, H), "could not paint source");

    VAStatus st = run_blit(src, dst, NULL, NULL);
    if (st != VA_STATUS_SUCCESS) {
        vaDestroySurfaces(g_dpy, &src, 1);
        vaDestroySurfaces(g_dpy, &dst, 1);
        EXPECT_STATUS(st);
        return;
    }

    unsigned char left = 0, right = 0;
    bool ok = read_argb_pixel(dst, W, H, 4, H / 2, &left) &&
              read_argb_pixel(dst, W, H, W - 5, H / 2, &right);
    vaDestroySurfaces(g_dpy, &src, 1);
    vaDestroySurfaces(g_dpy, &dst, 1);

    EXPECT_TRUE(ok, "could not read back destination");
    EXPECT_TRUE(right > left + 100, "luma ramp not reproduced across the blit");
    TEST_PASS();
}

static void test_vpp_crop(void) {
    TEST_START("VPP crop: source sub-rectangle, same scale");

    const uint32_t W = 256, H = 128;
    const uint32_t CROP_W = 128, CROP_H = 64;
    VASurfaceID src, dst;
    EXPECT_STATUS(vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420, W, H, &src, 1, NULL, 0));
    EXPECT_STATUS(vaCreateSurfaces(g_dpy, VA_RT_FORMAT_RGB32, CROP_W, CROP_H, &dst, 1, NULL, 0));
    EXPECT_TRUE(paint_luma_ramp(src, W, H), "could not paint source");

    /* Take the right half of the ramp. Its left edge should therefore start
     * around the mid-grey value rather than at black. */
    VARectangle srcRect = { .x = 128, .y = 32, .width = CROP_W, .height = CROP_H };
    VARectangle dstRect = { .x = 0, .y = 0, .width = CROP_W, .height = CROP_H };

    VAStatus st = run_blit(src, dst, &srcRect, &dstRect);
    if (st != VA_STATUS_SUCCESS) {
        vaDestroySurfaces(g_dpy, &src, 1);
        vaDestroySurfaces(g_dpy, &dst, 1);
        char msg[96];
        snprintf(msg, sizeof(msg), "crop blit rejected with status %d", st);
        TEST_FAIL(msg);
        return;
    }

    unsigned char left = 0;
    bool ok = read_argb_pixel(dst, CROP_W, CROP_H, 2, CROP_H / 2, &left);
    vaDestroySurfaces(g_dpy, &src, 1);
    vaDestroySurfaces(g_dpy, &dst, 1);

    EXPECT_TRUE(ok, "could not read back destination");
    /* Source x=128 of a 0..255 ramp over 256 px is ~128. Allow generous slack
     * for the YUV->RGB round trip; the point is that it is not ~0, which is
     * what we would read if the crop origin were ignored. */
    EXPECT_TRUE(left > 80, "crop origin ignored — left edge is still black");
    TEST_PASS();
}

static void test_vpp_scale_down(void) {
    TEST_START("VPP scale: 256x128 -> 64x32");

    const uint32_t W = 256, H = 128;
    const uint32_t DW = 64, DH = 32;
    VASurfaceID src, dst;
    EXPECT_STATUS(vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420, W, H, &src, 1, NULL, 0));
    EXPECT_STATUS(vaCreateSurfaces(g_dpy, VA_RT_FORMAT_RGB32, DW, DH, &dst, 1, NULL, 0));
    EXPECT_TRUE(paint_luma_ramp(src, W, H), "could not paint source");

    VARectangle srcRect = { .x = 0, .y = 0, .width = W, .height = H };
    VARectangle dstRect = { .x = 0, .y = 0, .width = DW, .height = DH };

    VAStatus st = run_blit(src, dst, &srcRect, &dstRect);
    if (st != VA_STATUS_SUCCESS) {
        vaDestroySurfaces(g_dpy, &src, 1);
        vaDestroySurfaces(g_dpy, &dst, 1);
        char msg[96];
        snprintf(msg, sizeof(msg), "scaling blit rejected with status %d", st);
        TEST_FAIL(msg);
        return;
    }

    unsigned char left = 0, mid = 0, right = 0;
    bool ok = read_argb_pixel(dst, DW, DH, 1, DH / 2, &left) &&
              read_argb_pixel(dst, DW, DH, DW / 2, DH / 2, &mid) &&
              read_argb_pixel(dst, DW, DH, DW - 2, DH / 2, &right);
    vaDestroySurfaces(g_dpy, &src, 1);
    vaDestroySurfaces(g_dpy, &dst, 1);

    EXPECT_TRUE(ok, "could not read back destination");
    /* The ramp must survive the downscale monotonically: a scaler that ignored
     * the size difference would read garbage or a constant. */
    EXPECT_TRUE(mid > left + 50, "downscaled ramp not increasing (left->mid)");
    EXPECT_TRUE(right > mid + 50, "downscaled ramp not increasing (mid->right)");
    TEST_PASS();
}

static void test_vpp_scale_with_crop(void) {
    TEST_START("VPP crop + scale together");

    const uint32_t W = 256, H = 128;
    const uint32_t DW = 96, DH = 48;
    VASurfaceID src, dst;
    EXPECT_STATUS(vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420, W, H, &src, 1, NULL, 0));
    EXPECT_STATUS(vaCreateSurfaces(g_dpy, VA_RT_FORMAT_RGB32, DW, DH, &dst, 1, NULL, 0));
    EXPECT_TRUE(paint_luma_ramp(src, W, H), "could not paint source");

    /* Right half, scaled to a smaller output. */
    VARectangle srcRect = { .x = 128, .y = 0, .width = 128, .height = 128 };
    VARectangle dstRect = { .x = 0, .y = 0, .width = DW, .height = DH };

    VAStatus st = run_blit(src, dst, &srcRect, &dstRect);
    if (st != VA_STATUS_SUCCESS) {
        vaDestroySurfaces(g_dpy, &src, 1);
        vaDestroySurfaces(g_dpy, &dst, 1);
        char msg[96];
        snprintf(msg, sizeof(msg), "crop+scale blit rejected with status %d", st);
        TEST_FAIL(msg);
        return;
    }

    unsigned char left = 0, right = 0;
    bool ok = read_argb_pixel(dst, DW, DH, 1, DH / 2, &left) &&
              read_argb_pixel(dst, DW, DH, DW - 2, DH / 2, &right);
    vaDestroySurfaces(g_dpy, &src, 1);
    vaDestroySurfaces(g_dpy, &dst, 1);

    EXPECT_TRUE(ok, "could not read back destination");
    EXPECT_TRUE(left > 80, "crop origin ignored in crop+scale");
    EXPECT_TRUE(right > left + 40, "ramp not increasing across crop+scale");
    TEST_PASS();
}

int main(void) {
    test_global_setup();

    printf("\n=== nvidia-vaapi-driver VideoProc tests ===\n");
    printf("Driver: %s\n\n", vaQueryVendorString(g_dpy));

    if (!test_has_entrypoint(g_dpy, VAProfileNone, VAEntrypointVideoProc)) {
        printf("  VideoProc entrypoint unavailable — skipping suite\n");
        test_global_teardown();
        return 77;
    }

    printf("Geometry:\n");
    test_vpp_identity_blit();
    test_vpp_crop();
    test_vpp_scale_down();
    test_vpp_scale_with_crop();

    test_print_summary("VideoProc tests");
    test_global_teardown();
    return g_fail > 0 ? 1 : 0;
}

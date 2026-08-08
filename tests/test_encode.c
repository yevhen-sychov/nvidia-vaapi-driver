/*
 * test_encode.c — Encode path integration tests for nvidia-vaapi-driver.
 *
 * Build:
 *   gcc -o test_encode test_encode.c -lva -lva-drm -lm
 *
 * Run:
 *   ./test_encode           # all tests
 *   ./test_encode h264      # H.264 tests only
 *   ./test_encode hevc      # HEVC tests only
 *
 * Exit code: 0 = all pass, 1 = failure
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>
#include <va/va_enc_av1.h>
#define DRM_DEVICE "/dev/dri/renderD128"

static int pass_count = 0;
static int fail_count = 0;

#define TEST_START(name) \
    printf("  %-50s ", name); fflush(stdout);

#define TEST_PASS() do { \
    printf("\033[32mPASS\033[0m\n"); pass_count++; \
} while (0)

#define TEST_FAIL(reason) do { \
    printf("\033[31mFAIL\033[0m (%s)\n", reason); fail_count++; \
} while (0)

#define TEST_SKIP(reason) do { \
    printf("\033[33mSKIP\033[0m (%s)\n", reason); \
} while (0)

#define TEST_ASSERT(cond, reason) do { \
    if (!(cond)) { TEST_FAIL(reason); return; } \
} while (0)

static VADisplay dpy;
static int drm_fd;

static void setup(void)
{
    drm_fd = open(DRM_DEVICE, O_RDWR);
    if (drm_fd < 0) {
        fprintf(stderr, "Cannot open %s\n", DRM_DEVICE);
        exit(1);
    }
    dpy = vaGetDisplayDRM(drm_fd);
    if (!dpy) {
        fprintf(stderr, "vaGetDisplayDRM failed\n");
        exit(1);
    }
    int major, minor;
    VAStatus st = vaInitialize(dpy, &major, &minor);
    if (st != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaInitialize failed: %d\n", st);
        exit(1);
    }
}

static void teardown(void)
{
    vaTerminate(dpy);
    close(drm_fd);
}

/* --- Test: Entrypoints --- */

static void test_entrypoints_h264(void)
{
    TEST_START("H.264 EncSlice entrypoint exists");
    int ne = vaMaxNumEntrypoints(dpy);
    VAEntrypoint *eps = calloc(ne, sizeof(VAEntrypoint));
    int n = 0;
    vaQueryConfigEntrypoints(dpy, VAProfileH264High, eps, &n);
    bool found = false;
    for (int i = 0; i < n; i++) {
        if (eps[i] == VAEntrypointEncSlice) found = true;
    }
    free(eps);
    TEST_ASSERT(found, "VAEntrypointEncSlice not found for H264High");
    TEST_PASS();
}

static void test_entrypoints_hevc(void)
{
    TEST_START("HEVC EncSlice entrypoint exists");
    int ne = vaMaxNumEntrypoints(dpy);
    VAEntrypoint *eps = calloc(ne, sizeof(VAEntrypoint));
    int n = 0;
    vaQueryConfigEntrypoints(dpy, VAProfileHEVCMain, eps, &n);
    bool found = false;
    for (int i = 0; i < n; i++) {
        if (eps[i] == VAEntrypointEncSlice) found = true;
    }
    free(eps);
    TEST_ASSERT(found, "VAEntrypointEncSlice not found for HEVCMain");
    TEST_PASS();
}

/* --- Test: Config attributes --- */

static void test_config_attributes(void)
{
    TEST_START("Encode config attributes (RTFormat, RateControl)");
    VAConfigAttrib attribs[3] = {
        { .type = VAConfigAttribRTFormat },
        { .type = VAConfigAttribRateControl },
        { .type = VAConfigAttribEncMaxRefFrames },
    };
    VAStatus st = vaGetConfigAttributes(dpy, VAProfileH264High,
                                         VAEntrypointEncSlice, attribs, 3);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaGetConfigAttributes failed");
    TEST_ASSERT(attribs[0].value & VA_RT_FORMAT_YUV420, "no YUV420 RTFormat");
    TEST_ASSERT(attribs[1].value & VA_RC_CQP, "no CQP rate control");
    TEST_ASSERT(attribs[1].value & VA_RC_CBR, "no CBR rate control");
    TEST_ASSERT(attribs[1].value & VA_RC_VBR, "no VBR rate control");
    TEST_PASS();
}

/* --- Test: Create/destroy config+surfaces+context --- */

static void test_create_destroy(void)
{
    TEST_START("Create and destroy encode config/surfaces/context");

    VAConfigAttrib attrib = { .type = VAConfigAttribRTFormat,
                               .value = VA_RT_FORMAT_YUV420 };
    VAConfigID config;
    VAStatus st = vaCreateConfig(dpy, VAProfileH264High,
                                  VAEntrypointEncSlice, &attrib, 1, &config);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateConfig failed");

    VASurfaceID surfaces[4];
    st = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 320, 240,
                           surfaces, 4, NULL, 0);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateSurfaces failed");

    VAContextID context;
    st = vaCreateContext(dpy, config, 320, 240, VA_PROGRESSIVE,
                          surfaces, 4, &context);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateContext failed");

    st = vaDestroyContext(dpy, context);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaDestroyContext failed");
    st = vaDestroySurfaces(dpy, surfaces, 4);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaDestroySurfaces failed");
    st = vaDestroyConfig(dpy, config);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaDestroyConfig failed");
    TEST_PASS();
}

/* --- Test: Full encode cycle (1 frame) --- */

static void test_encode_one_frame(VAProfile profile, const char *codec_name)
{
    char name[64];
    snprintf(name, sizeof(name), "%s encode 1 frame (320x240)", codec_name);
    TEST_START(name);

    VAConfigAttrib attrib = { .type = VAConfigAttribRTFormat,
                               .value = VA_RT_FORMAT_YUV420 };
    VAConfigID config;
    VAStatus st = vaCreateConfig(dpy, profile, VAEntrypointEncSlice,
                                  &attrib, 1, &config);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "config");

    VASurfaceID surface;
    st = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 320, 240,
                           &surface, 1, NULL, 0);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "surface");

    VAContextID context;
    st = vaCreateContext(dpy, config, 320, 240, VA_PROGRESSIVE,
                          &surface, 1, &context);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "context");

    /* Coded buffer */
    VABufferID coded_buf;
    st = vaCreateBuffer(dpy, context, VAEncCodedBufferType, 320 * 240,
                         1, NULL, &coded_buf);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "coded_buf");

    /* Create NV12 image and fill with gray */
    VAImageFormat fmt = { .fourcc = VA_FOURCC_NV12 };
    VAImage image;
    st = vaCreateImage(dpy, &fmt, 320, 240, &image);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "image");
    void *img_data;
    vaMapBuffer(dpy, image.buf, &img_data);
    memset(img_data, 128, image.data_size);
    vaUnmapBuffer(dpy, image.buf);
    vaPutImage(dpy, surface, image.image_id, 0, 0, 320, 240, 0, 0, 320, 240);

    /* Sequence params */
    VABufferID seq_buf;
    if (profile == VAProfileH264High || profile == VAProfileH264Main ||
        profile == VAProfileH264ConstrainedBaseline) {
        VAEncSequenceParameterBufferH264 seq = {
            .picture_width_in_mbs = 320 / 16,
            .picture_height_in_mbs = 240 / 16,
            .intra_period = 30, .ip_period = 1,
        };
        vaCreateBuffer(dpy, context, VAEncSequenceParameterBufferType,
                        sizeof(seq), 1, &seq, &seq_buf);
    } else {
        VAEncSequenceParameterBufferHEVC seq = {
            .pic_width_in_luma_samples = 320,
            .pic_height_in_luma_samples = 240,
            .intra_period = 30, .ip_period = 1,
        };
        vaCreateBuffer(dpy, context, VAEncSequenceParameterBufferType,
                        sizeof(seq), 1, &seq, &seq_buf);
    }

    /* Picture params */
    VABufferID pic_buf;
    if (profile == VAProfileH264High || profile == VAProfileH264Main ||
        profile == VAProfileH264ConstrainedBaseline) {
        VAEncPictureParameterBufferH264 pic = {
            .coded_buf = coded_buf,
            .pic_fields.bits.idr_pic_flag = 1,
        };
        vaCreateBuffer(dpy, context, VAEncPictureParameterBufferType,
                        sizeof(pic), 1, &pic, &pic_buf);
    } else {
        VAEncPictureParameterBufferHEVC pic = {
            .coded_buf = coded_buf,
            .pic_fields.bits.idr_pic_flag = 1,
        };
        vaCreateBuffer(dpy, context, VAEncPictureParameterBufferType,
                        sizeof(pic), 1, &pic, &pic_buf);
    }

    /* Slice params */
    VABufferID slice_buf;
    if (profile == VAProfileH264High || profile == VAProfileH264Main ||
        profile == VAProfileH264ConstrainedBaseline) {
        VAEncSliceParameterBufferH264 slice = { .slice_type = 2 };
        vaCreateBuffer(dpy, context, VAEncSliceParameterBufferType,
                        sizeof(slice), 1, &slice, &slice_buf);
    } else {
        VAEncSliceParameterBufferHEVC slice = { .slice_type = 2 };
        vaCreateBuffer(dpy, context, VAEncSliceParameterBufferType,
                        sizeof(slice), 1, &slice, &slice_buf);
    }

    /* Encode */
    st = vaBeginPicture(dpy, context, surface);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaBeginPicture");
    VABufferID bufs[] = { seq_buf, pic_buf, slice_buf };
    st = vaRenderPicture(dpy, context, bufs, 3);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaRenderPicture");
    st = vaEndPicture(dpy, context);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaEndPicture");

    st = vaSyncSurface(dpy, surface);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaSyncSurface");

    /* Map coded buffer and check output */
    VACodedBufferSegment *seg = NULL;
    st = vaMapBuffer(dpy, coded_buf, (void **)&seg);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaMapBuffer");
    TEST_ASSERT(seg != NULL, "coded segment is NULL");
    TEST_ASSERT(seg->buf != NULL, "coded data is NULL");
    TEST_ASSERT(seg->size > 0, "coded size is 0");

    /* Check for valid NAL start code */
    unsigned char *bs = (unsigned char *)seg->buf;
    bool has_start_code = (bs[0] == 0 && bs[1] == 0 && bs[2] == 0 && bs[3] == 1);
    TEST_ASSERT(has_start_code, "no NAL start code 00 00 00 01");

    vaUnmapBuffer(dpy, coded_buf);

    /* Cleanup */
    vaDestroyBuffer(dpy, coded_buf);
    vaDestroyBuffer(dpy, seq_buf);
    vaDestroyBuffer(dpy, pic_buf);
    vaDestroyBuffer(dpy, slice_buf);
    vaDestroyImage(dpy, image.image_id);
    vaDestroyContext(dpy, context);
    vaDestroySurfaces(dpy, &surface, 1);
    vaDestroyConfig(dpy, config);
    TEST_PASS();
}

static void test_hevc_main10_one_frame(void)
{
    TEST_START("HEVC Main10 encode 1 frame (10-bit P010)");

    VAConfigAttrib attrib = { .type = VAConfigAttribRTFormat,
                               .value = VA_RT_FORMAT_YUV420_10 };
    VAConfigID config;
    VAStatus st = vaCreateConfig(dpy, VAProfileHEVCMain10, VAEntrypointEncSlice,
                                  &attrib, 1, &config);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "config");

    VASurfaceID surface;
    st = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420_10, 320, 240,
                           &surface, 1, NULL, 0);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "surface");

    VAContextID context;
    st = vaCreateContext(dpy, config, 320, 240, VA_PROGRESSIVE,
                          &surface, 1, &context);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "context");

    VABufferID coded_buf;
    st = vaCreateBuffer(dpy, context, VAEncCodedBufferType, 320 * 240,
                         1, NULL, &coded_buf);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "coded_buf");

    /* Create P010 image and fill with gray */
    VAImageFormat fmt = { .fourcc = VA_FOURCC_P010 };
    VAImage image;
    st = vaCreateImage(dpy, &fmt, 320, 240, &image);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "image");
    void *img_data;
    vaMapBuffer(dpy, image.buf, &img_data);
    memset(img_data, 128, image.data_size);
    vaUnmapBuffer(dpy, image.buf);
    vaPutImage(dpy, surface, image.image_id, 0, 0, 320, 240, 0, 0, 320, 240);

    VAEncSequenceParameterBufferHEVC seq = {
        .pic_width_in_luma_samples = 320,
        .pic_height_in_luma_samples = 240,
        .intra_period = 30, .ip_period = 1,
    };
    VABufferID seq_buf;
    vaCreateBuffer(dpy, context, VAEncSequenceParameterBufferType,
                    sizeof(seq), 1, &seq, &seq_buf);

    VAEncPictureParameterBufferHEVC pic = {
        .coded_buf = coded_buf,
        .pic_fields.bits.idr_pic_flag = 1,
    };
    VABufferID pic_buf;
    vaCreateBuffer(dpy, context, VAEncPictureParameterBufferType,
                    sizeof(pic), 1, &pic, &pic_buf);

    VAEncSliceParameterBufferHEVC slice = { .slice_type = 2 };
    VABufferID slice_buf;
    vaCreateBuffer(dpy, context, VAEncSliceParameterBufferType,
                    sizeof(slice), 1, &slice, &slice_buf);

    st = vaBeginPicture(dpy, context, surface);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaBeginPicture");
    VABufferID bufs[] = { seq_buf, pic_buf, slice_buf };
    st = vaRenderPicture(dpy, context, bufs, 3);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaRenderPicture");
    st = vaEndPicture(dpy, context);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaEndPicture");

    st = vaSyncSurface(dpy, surface);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaSyncSurface");

    VACodedBufferSegment *seg = NULL;
    st = vaMapBuffer(dpy, coded_buf, (void **)&seg);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaMapBuffer");
    TEST_ASSERT(seg != NULL, "coded segment is NULL");
    TEST_ASSERT(seg->size > 0, "coded size is 0");
    vaUnmapBuffer(dpy, coded_buf);

    vaDestroyBuffer(dpy, coded_buf);
    vaDestroyBuffer(dpy, seq_buf);
    vaDestroyBuffer(dpy, pic_buf);
    vaDestroyBuffer(dpy, slice_buf);
    vaDestroyImage(dpy, image.image_id);
    vaDestroyContext(dpy, context);
    vaDestroySurfaces(dpy, &surface, 1);
    vaDestroyConfig(dpy, config);
    TEST_PASS();
}

static void test_av1_one_frame(void)
{
    TEST_START("AV1 encode 1 frame");

    VAConfigAttrib attrib = { .type = VAConfigAttribRTFormat,
                               .value = VA_RT_FORMAT_YUV420 };
    VAConfigID config;
    VAStatus st = vaCreateConfig(dpy, VAProfileAV1Profile0, VAEntrypointEncSlice,
                                  &attrib, 1, &config);
    if (st != VA_STATUS_SUCCESS) {
        TEST_SKIP("AV1 encoding not supported by hardware");
        return;
    }

    VASurfaceID surface;
    st = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 320, 240,
                           &surface, 1, NULL, 0);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "surface");

    VAContextID context;
    st = vaCreateContext(dpy, config, 320, 240, VA_PROGRESSIVE,
                          &surface, 1, &context);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "context");

    VABufferID coded_buf;
    st = vaCreateBuffer(dpy, context, VAEncCodedBufferType, 320 * 240,
                         1, NULL, &coded_buf);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "coded_buf");

    VAEncSequenceParameterBufferAV1 seq = {
        .intra_period = 30, .ip_period = 1,
    };
    VABufferID seq_buf;
    vaCreateBuffer(dpy, context, VAEncSequenceParameterBufferType,
                    sizeof(seq), 1, &seq, &seq_buf);

    VAEncPictureParameterBufferAV1 pic = {
        .coded_buf = coded_buf,
        .frame_width_minus_1 = 319,
        .frame_height_minus_1 = 239,
        .picture_flags.bits.frame_type = 0, /* KEY_FRAME */
    };
    VABufferID pic_buf;
    vaCreateBuffer(dpy, context, VAEncPictureParameterBufferType,
                    sizeof(pic), 1, &pic, &pic_buf);

    st = vaBeginPicture(dpy, context, surface);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaBeginPicture");
    VABufferID bufs[] = { seq_buf, pic_buf };
    st = vaRenderPicture(dpy, context, bufs, 2);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaRenderPicture");
    st = vaEndPicture(dpy, context);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaEndPicture");

    st = vaSyncSurface(dpy, surface);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaSyncSurface");

    VACodedBufferSegment *seg = NULL;
    st = vaMapBuffer(dpy, coded_buf, (void **)&seg);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaMapBuffer");
    TEST_ASSERT(seg != NULL, "coded segment is NULL");
    TEST_ASSERT(seg->size > 0, "coded size is 0");
    vaUnmapBuffer(dpy, coded_buf);

    vaDestroyConfig(dpy, config);
    TEST_PASS();
}

static void test_rate_control_params(void)
{
    TEST_START("Rate control parameter propagation (CBR)");
    VAConfigAttrib attribs[2] = {
        { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 },
        { .type = VAConfigAttribRateControl, .value = VA_RC_CBR }
    };
    VAConfigID config;
    VAStatus st = vaCreateConfig(dpy, VAProfileH264High, VAEntrypointEncSlice,
                                  attribs, 2, &config);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "config");

    VASurfaceID surface;
    vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 320, 240, &surface, 1, NULL, 0);
    VAContextID context;
    vaCreateContext(dpy, config, 320, 240, VA_PROGRESSIVE, &surface, 1, &context);

    /* Misc param: Rate Control */
    VABufferID rc_buf;
    unsigned int misc_rc_size = sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterRateControl);
    VAEncMiscParameterBuffer *misc_rc;
    vaCreateBuffer(dpy, context, VAEncMiscParameterBufferType, misc_rc_size, 1, NULL, &rc_buf);
    vaMapBuffer(dpy, rc_buf, (void **)&misc_rc);
    misc_rc->type = VAEncMiscParameterTypeRateControl;
    VAEncMiscParameterRateControl *rc = (VAEncMiscParameterRateControl *)misc_rc->data;
    rc->bits_per_second = 1000000; /* 1 Mbps */
    rc->target_percentage = 90;
    vaUnmapBuffer(dpy, rc_buf);

    /* Basic encode setup */
    VABufferID coded;
    vaCreateBuffer(dpy, context, VAEncCodedBufferType, 320 * 240, 1, NULL, &coded);
    VAEncSequenceParameterBufferH264 seq = { .picture_width_in_mbs = 20, .picture_height_in_mbs = 15 };
    VAEncPictureParameterBufferH264 pic = { .coded_buf = coded, .pic_fields.bits.idr_pic_flag = 1 };
    VABufferID seq_buf, pic_buf;
    vaCreateBuffer(dpy, context, VAEncSequenceParameterBufferType, sizeof(seq), 1, &seq, &seq_buf);
    vaCreateBuffer(dpy, context, VAEncPictureParameterBufferType, sizeof(pic), 1, &pic, &pic_buf);

    vaBeginPicture(dpy, context, surface);
    VABufferID bufs[] = { seq_buf, pic_buf, rc_buf };
    vaRenderPicture(dpy, context, bufs, 3);
    st = vaEndPicture(dpy, context);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaEndPicture");

    vaDestroyBuffer(dpy, coded);
    vaDestroyBuffer(dpy, seq_buf);
    vaDestroyBuffer(dpy, pic_buf);
    vaDestroyBuffer(dpy, rc_buf);
    vaDestroyContext(dpy, context);
    vaDestroySurfaces(dpy, &surface, 1);
    vaDestroyConfig(dpy, config);
    TEST_PASS();
}

static void test_quality_level_param(void)
{
    TEST_START("Quality level parameter propagation");
    VAConfigAttrib attrib = { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 };
    VAConfigID config;
    vaCreateConfig(dpy, VAProfileH264High, VAEntrypointEncSlice, &attrib, 1, &config);
    VASurfaceID surface;
    vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 320, 240, &surface, 1, NULL, 0);
    VAContextID context;
    vaCreateContext(dpy, config, 320, 240, VA_PROGRESSIVE, &surface, 1, &context);

    /* Misc param: Quality Level */
    VABufferID ql_buf;
    unsigned int misc_ql_size = sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterBufferQualityLevel);
    VAEncMiscParameterBuffer *misc_ql;
    vaCreateBuffer(dpy, context, VAEncMiscParameterBufferType, misc_ql_size, 1, NULL, &ql_buf);
    vaMapBuffer(dpy, ql_buf, (void **)&misc_ql);
    misc_ql->type = VAEncMiscParameterTypeQualityLevel;
    VAEncMiscParameterBufferQualityLevel *ql = (VAEncMiscParameterBufferQualityLevel *)misc_ql->data;
    ql->quality_level = 1; /* P1 (Highest performance) */
    vaUnmapBuffer(dpy, ql_buf);

    VABufferID coded;
    vaCreateBuffer(dpy, context, VAEncCodedBufferType, 320 * 240, 1, NULL, &coded);
    VAEncSequenceParameterBufferH264 seq = { .picture_width_in_mbs = 20, .picture_height_in_mbs = 15 };
    VAEncPictureParameterBufferH264 pic = { .coded_buf = coded, .pic_fields.bits.idr_pic_flag = 1 };
    VABufferID seq_buf, pic_buf;
    vaCreateBuffer(dpy, context, VAEncSequenceParameterBufferType, sizeof(seq), 1, &seq, &seq_buf);
    vaCreateBuffer(dpy, context, VAEncPictureParameterBufferType, sizeof(pic), 1, &pic, &pic_buf);

    vaBeginPicture(dpy, context, surface);
    VABufferID bufs[] = { seq_buf, pic_buf, ql_buf };
    vaRenderPicture(dpy, context, bufs, 3);
    VAStatus st = vaEndPicture(dpy, context);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaEndPicture");

    vaDestroyBuffer(dpy, coded);
    vaDestroyBuffer(dpy, seq_buf);
    vaDestroyBuffer(dpy, pic_buf);
    vaDestroyBuffer(dpy, ql_buf);
    vaDestroyContext(dpy, context);
    vaDestroySurfaces(dpy, &surface, 1);
    vaDestroyConfig(dpy, config);
    TEST_PASS();
}

static void test_av1_temporal_layers(void)
{
    TEST_START("AV1 temporal layers (temporal_id propagation)");
    VAConfigAttrib attrib = { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 };
    VAConfigID config;
    VAStatus st = vaCreateConfig(dpy, VAProfileAV1Profile0, VAEntrypointEncSlice, &attrib, 1, &config);
    if (st != VA_STATUS_SUCCESS) { TEST_SKIP("AV1 not supported"); return; }

    VASurfaceID surface;
    vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 320, 240, &surface, 1, NULL, 0);
    VAContextID context;
    vaCreateContext(dpy, config, 320, 240, VA_PROGRESSIVE, &surface, 1, &context);
    VABufferID coded;
    vaCreateBuffer(dpy, context, VAEncCodedBufferType, 320 * 240, 1, NULL, &coded);

    for (int i = 0; i < 4; i++) {
        VAEncSequenceParameterBufferAV1 seq = { .intra_period = 30 };
        VAEncPictureParameterBufferAV1 pic = {
            .coded_buf = coded,
            .frame_width_minus_1 = 319, .frame_height_minus_1 = 239,
            .temporal_id = i % 3,
            .picture_flags.bits.frame_type = (i == 0) ? 0 : 1,
        };
        VABufferID seq_buf, pic_buf;
        vaCreateBuffer(dpy, context, VAEncSequenceParameterBufferType, sizeof(seq), 1, &seq, &seq_buf);
        vaCreateBuffer(dpy, context, VAEncPictureParameterBufferType, sizeof(pic), 1, &pic, &pic_buf);

        vaBeginPicture(dpy, context, surface);
        VABufferID bufs[] = { seq_buf, pic_buf };
        vaRenderPicture(dpy, context, bufs, 2);
        st = vaEndPicture(dpy, context);
        TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaEndPicture");

        vaDestroyBuffer(dpy, seq_buf);
        vaDestroyBuffer(dpy, pic_buf);
    }

    vaDestroyBuffer(dpy, coded);
    vaDestroyContext(dpy, context);
    vaDestroySurfaces(dpy, &surface, 1);
    vaDestroyConfig(dpy, config);
    TEST_PASS();
}

/* The driver advertises the AV1 encode RTFormat as the combined mask
 * VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10. A client that echoes that
 * queried mask into vaCreateConfig and then feeds 8-bit NV12 frames must still
 * encode successfully. Previously the driver treated any config whose mask had
 * the 10-bit bit set as a 10-bit encode, so the 8-bit input copy failed and
 * vaEndPicture returned an error — the sort of hard NVENC failure that crashes
 * a browser's GPU process (dropping *decode* to software too). */
static void test_av1_combined_rtformat_encode(void)
{
    TEST_START("AV1 encode 8-bit frame with advertised RTFormat mask");

    VAConfigAttrib q = { .type = VAConfigAttribRTFormat };
    VAStatus st = vaGetConfigAttributes(dpy, VAProfileAV1Profile0, VAEntrypointEncSlice, &q, 1);
    if (st != VA_STATUS_SUCCESS) { TEST_SKIP("AV1 not supported"); return; }
    TEST_ASSERT(q.value & VA_RT_FORMAT_YUV420, "8-bit YUV420 not advertised for AV1 encode");

    /* Use the advertised mask verbatim (may contain both 8-bit and 10-bit). */
    VAConfigAttrib attrib = { .type = VAConfigAttribRTFormat, .value = q.value };
    VAConfigID config;
    st = vaCreateConfig(dpy, VAProfileAV1Profile0, VAEntrypointEncSlice, &attrib, 1, &config);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "config");

    VASurfaceID surface;
    st = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 320, 240, &surface, 1, NULL, 0);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "surface");

    VAContextID context;
    st = vaCreateContext(dpy, config, 320, 240, VA_PROGRESSIVE, &surface, 1, &context);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "context");

    VABufferID coded;
    st = vaCreateBuffer(dpy, context, VAEncCodedBufferType, 320 * 240, 1, NULL, &coded);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "coded_buf");

    VAImageFormat fmt = { .fourcc = VA_FOURCC_NV12 };
    VAImage image;
    vaCreateImage(dpy, &fmt, 320, 240, &image);
    void *img_data;
    vaMapBuffer(dpy, image.buf, &img_data);
    memset(img_data, 128, image.data_size);
    vaUnmapBuffer(dpy, image.buf);
    vaPutImage(dpy, surface, image.image_id, 0, 0, 320, 240, 0, 0, 320, 240);

    VAEncSequenceParameterBufferAV1 seq = { .intra_period = 30 };
    VAEncPictureParameterBufferAV1 pic = {
        .coded_buf = coded,
        .frame_width_minus_1 = 319, .frame_height_minus_1 = 239,
        .picture_flags.bits.frame_type = 0,
    };
    VABufferID seq_buf, pic_buf;
    vaCreateBuffer(dpy, context, VAEncSequenceParameterBufferType, sizeof(seq), 1, &seq, &seq_buf);
    vaCreateBuffer(dpy, context, VAEncPictureParameterBufferType, sizeof(pic), 1, &pic, &pic_buf);

    st = vaBeginPicture(dpy, context, surface);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaBeginPicture");
    VABufferID bufs[] = { seq_buf, pic_buf };
    st = vaRenderPicture(dpy, context, bufs, 2);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaRenderPicture");
    st = vaEndPicture(dpy, context);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaEndPicture");
    st = vaSyncSurface(dpy, surface);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaSyncSurface");

    VACodedBufferSegment *seg = NULL;
    st = vaMapBuffer(dpy, coded, (void **)&seg);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaMapBuffer");
    TEST_ASSERT(seg != NULL && seg->size > 0, "empty coded buffer");
    vaUnmapBuffer(dpy, coded);

    vaDestroyBuffer(dpy, coded);
    vaDestroyBuffer(dpy, seq_buf);
    vaDestroyBuffer(dpy, pic_buf);
    vaDestroyImage(dpy, image.image_id);
    vaDestroyContext(dpy, context);
    vaDestroySurfaces(dpy, &surface, 1);
    vaDestroyConfig(dpy, config);
    TEST_PASS();
}

/* Reproduces Chrome/WebRTC L1T2 screenshare: the encoder is configured with
 * VAConfigAttribEncRateControlExt (temporal layers) and driven with a
 * VAEncMiscParameterTemporalLayerStructure (number_of_layers=2) plus per-frame
 * temporal_id. This is the exact path that must produce a valid hardware AV1
 * stream — if the driver mis-programs NVENC temporal SVC here, Chrome's GPU
 * process fails and *all* hardware acceleration (including decode) falls back
 * to software. */
static void test_av1_temporal_svc_encode(void)
{
    TEST_START("AV1 temporal SVC L1T2 encode (misc layer structure)");

    VAConfigAttrib attribs[3] = {
        { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 },
        { .type = VAConfigAttribRateControl, .value = VA_RC_CBR },
        { .type = VAConfigAttribEncRateControlExt },
    };
    /* Query what the driver advertises for temporal layers, like Chrome does. */
    VAStatus st = vaGetConfigAttributes(dpy, VAProfileAV1Profile0,
                                        VAEntrypointEncSlice, attribs, 3);
    if (st != VA_STATUS_SUCCESS) { TEST_SKIP("AV1 not supported"); return; }
    TEST_ASSERT(attribs[2].value != VA_ATTRIB_NOT_SUPPORTED,
                "VAConfigAttribEncRateControlExt not advertised for AV1");
    {
        VAConfigAttribValEncRateControlExt v = { .value = attribs[2].value };
        TEST_ASSERT(v.bits.max_num_temporal_layers_minus1 >= 1,
                    "driver advertises fewer than 2 temporal layers");
    }

    /* Create the config the way Chrome does: request a single concrete
     * RTFormat (8-bit YUV420), not the full capability mask returned by the
     * query above. */
    VAConfigAttrib cfg_attribs[3] = {
        { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 },
        { .type = VAConfigAttribRateControl, .value = VA_RC_CBR },
        { .type = VAConfigAttribEncRateControlExt, .value = attribs[2].value },
    };
    VAConfigID config;
    st = vaCreateConfig(dpy, VAProfileAV1Profile0, VAEntrypointEncSlice,
                        cfg_attribs, 3, &config);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "config");

    VASurfaceID surface;
    st = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 320, 240, &surface, 1, NULL, 0);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "surface");

    VAContextID context;
    st = vaCreateContext(dpy, config, 320, 240, VA_PROGRESSIVE, &surface, 1, &context);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "context");

    VABufferID coded;
    st = vaCreateBuffer(dpy, context, VAEncCodedBufferType, 320 * 240, 1, NULL, &coded);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "coded_buf");

    /* Fill surface with gray. */
    VAImageFormat fmt = { .fourcc = VA_FOURCC_NV12 };
    VAImage image;
    vaCreateImage(dpy, &fmt, 320, 240, &image);
    void *img_data;
    vaMapBuffer(dpy, image.buf, &img_data);
    memset(img_data, 128, image.data_size);
    vaUnmapBuffer(dpy, image.buf);
    vaPutImage(dpy, surface, image.image_id, 0, 0, 320, 240, 0, 0, 320, 240);

    /* L1T2 temporal pattern: layer 0,1,0,1,... */
    const int temporal_ids[8] = { 0, 1, 0, 1, 0, 1, 0, 1 };

    for (int i = 0; i < 8; i++) {
        /* Misc: temporal layer structure (only meaningful on first frame, but
         * Chrome resends it; the driver must tolerate it). */
        VABufferID tl_buf;
        unsigned int tl_size = sizeof(VAEncMiscParameterBuffer) +
                               sizeof(VAEncMiscParameterTemporalLayerStructure);
        VAEncMiscParameterBuffer *misc_tl;
        vaCreateBuffer(dpy, context, VAEncMiscParameterBufferType, tl_size, 1, NULL, &tl_buf);
        vaMapBuffer(dpy, tl_buf, (void **)&misc_tl);
        misc_tl->type = VAEncMiscParameterTypeTemporalLayerStructure;
        VAEncMiscParameterTemporalLayerStructure *tl =
            (VAEncMiscParameterTemporalLayerStructure *)misc_tl->data;
        tl->number_of_layers = 2;
        tl->periodicity = 2;
        tl->layer_id[0] = 0;
        tl->layer_id[1] = 1;
        vaUnmapBuffer(dpy, tl_buf);

        /* Misc: rate control (CBR). */
        VABufferID rc_buf;
        unsigned int rc_size = sizeof(VAEncMiscParameterBuffer) +
                               sizeof(VAEncMiscParameterRateControl);
        VAEncMiscParameterBuffer *misc_rc;
        vaCreateBuffer(dpy, context, VAEncMiscParameterBufferType, rc_size, 1, NULL, &rc_buf);
        vaMapBuffer(dpy, rc_buf, (void **)&misc_rc);
        misc_rc->type = VAEncMiscParameterTypeRateControl;
        VAEncMiscParameterRateControl *rc = (VAEncMiscParameterRateControl *)misc_rc->data;
        rc->bits_per_second = 2000000;
        rc->target_percentage = 90;
        vaUnmapBuffer(dpy, rc_buf);

        VAEncSequenceParameterBufferAV1 seq = { .intra_period = 60 };
        VAEncPictureParameterBufferAV1 pic = {
            .coded_buf = coded,
            .frame_width_minus_1 = 319, .frame_height_minus_1 = 239,
            .temporal_id = temporal_ids[i],
            .picture_flags.bits.frame_type = (i == 0) ? 0 : 1,
        };
        VABufferID seq_buf, pic_buf;
        vaCreateBuffer(dpy, context, VAEncSequenceParameterBufferType, sizeof(seq), 1, &seq, &seq_buf);
        vaCreateBuffer(dpy, context, VAEncPictureParameterBufferType, sizeof(pic), 1, &pic, &pic_buf);

        st = vaBeginPicture(dpy, context, surface);
        TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaBeginPicture");
        VABufferID bufs[] = { tl_buf, rc_buf, seq_buf, pic_buf };
        st = vaRenderPicture(dpy, context, bufs, 4);
        TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaRenderPicture");
        st = vaEndPicture(dpy, context);
        TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaEndPicture");
        st = vaSyncSurface(dpy, surface);
        TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaSyncSurface");

        VACodedBufferSegment *seg = NULL;
        st = vaMapBuffer(dpy, coded, (void **)&seg);
        TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaMapBuffer");
        TEST_ASSERT(seg != NULL && seg->size > 0, "empty coded buffer for SVC frame");
        vaUnmapBuffer(dpy, coded);

        vaDestroyBuffer(dpy, tl_buf);
        vaDestroyBuffer(dpy, rc_buf);
        vaDestroyBuffer(dpy, seq_buf);
        vaDestroyBuffer(dpy, pic_buf);
    }

    vaDestroyBuffer(dpy, coded);
    vaDestroyImage(dpy, image.image_id);
    vaDestroyContext(dpy, context);
    vaDestroySurfaces(dpy, &surface, 1);
    vaDestroyConfig(dpy, config);
    TEST_PASS();
}

static void test_av1_main10_one_frame(void)
{
    TEST_START("AV1 Main10 encode 1 frame (10-bit P010)");

    VAConfigAttrib attrib = { .type = VAConfigAttribRTFormat,
                               .value = VA_RT_FORMAT_YUV420_10 };
    VAConfigID config;
    VAStatus st = vaCreateConfig(dpy, VAProfileAV1Profile0, VAEntrypointEncSlice,
                                  &attrib, 1, &config);
    if (st != VA_STATUS_SUCCESS) {
        TEST_SKIP("AV1 10-bit encoding not supported");
        return;
    }

    VASurfaceID surface;
    st = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420_10, 320, 240,
                           &surface, 1, NULL, 0);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "surface");

    VAContextID context;
    st = vaCreateContext(dpy, config, 320, 240, VA_PROGRESSIVE,
                          &surface, 1, &context);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "context");

    VABufferID coded_buf;
    st = vaCreateBuffer(dpy, context, VAEncCodedBufferType, 320 * 240,
                         1, NULL, &coded_buf);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "coded_buf");

    /* Create P010 image and fill with gray */
    VAImageFormat fmt = { .fourcc = VA_FOURCC_P010 };
    VAImage image;
    st = vaCreateImage(dpy, &fmt, 320, 240, &image);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "image");
    void *img_data;
    vaMapBuffer(dpy, image.buf, &img_data);
    memset(img_data, 128, image.data_size);
    vaUnmapBuffer(dpy, image.buf);
    vaPutImage(dpy, surface, image.image_id, 0, 0, 320, 240, 0, 0, 320, 240);

    VAEncSequenceParameterBufferAV1 seq = { .intra_period = 30 };
    VABufferID seq_buf;
    vaCreateBuffer(dpy, context, VAEncSequenceParameterBufferType,
                    sizeof(seq), 1, &seq, &seq_buf);

    VAEncPictureParameterBufferAV1 pic = {
        .coded_buf = coded_buf,
        .frame_width_minus_1 = 319,
        .frame_height_minus_1 = 239,
        .picture_flags.bits.frame_type = 0, /* KEY_FRAME */
    };
    VABufferID pic_buf;
    vaCreateBuffer(dpy, context, VAEncPictureParameterBufferType,
                    sizeof(pic), 1, &pic, &pic_buf);

    st = vaBeginPicture(dpy, context, surface);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaBeginPicture");
    VABufferID bufs[] = { seq_buf, pic_buf };
    st = vaRenderPicture(dpy, context, bufs, 2);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaRenderPicture");
    st = vaEndPicture(dpy, context);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaEndPicture");

    st = vaSyncSurface(dpy, surface);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaSyncSurface");

    VACodedBufferSegment *seg = NULL;
    st = vaMapBuffer(dpy, coded_buf, (void **)&seg);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaMapBuffer");
    TEST_ASSERT(seg != NULL, "coded segment is NULL");
    TEST_ASSERT(seg->size > 0, "coded size is 0");
    vaUnmapBuffer(dpy, coded_buf);

    vaDestroyBuffer(dpy, coded_buf);
    vaDestroyBuffer(dpy, seq_buf);
    vaDestroyBuffer(dpy, pic_buf);
    vaDestroyImage(dpy, image.image_id);
    vaDestroyContext(dpy, context);
    vaDestroySurfaces(dpy, &surface, 1);
    vaDestroyConfig(dpy, config);
    TEST_PASS();
}

static void test_dynamic_resolution(void)
{
    TEST_START("Dynamic resolution (320x240 → 640x480 in same context)");
    VAConfigAttrib attrib = { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 };
    VAConfigID config;
    vaCreateConfig(dpy, VAProfileH264High, VAEntrypointEncSlice, &attrib, 1, &config);

    VASurfaceID surfaces[2];
    vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 320, 240, &surfaces[0], 1, NULL, 0);
    vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 640, 480, &surfaces[1], 1, NULL, 0);

    /* Create context with max size 640x480 */
    VAContextID context;
    VAStatus st = vaCreateContext(dpy, config, 640, 480, VA_PROGRESSIVE, surfaces, 2, &context);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateContext");

    VABufferID coded;
    vaCreateBuffer(dpy, context, VAEncCodedBufferType, 640 * 480, 1, NULL, &coded);

    /* Frame 1: 320x240 */
    {
        VAEncSequenceParameterBufferH264 seq = { .picture_width_in_mbs = 20, .picture_height_in_mbs = 15 };
        VAEncPictureParameterBufferH264 pic = { .coded_buf = coded, .pic_fields.bits.idr_pic_flag = 1 };
        VABufferID seq_buf, pic_buf;
        vaCreateBuffer(dpy, context, VAEncSequenceParameterBufferType, sizeof(seq), 1, &seq, &seq_buf);
        vaCreateBuffer(dpy, context, VAEncPictureParameterBufferType, sizeof(pic), 1, &pic, &pic_buf);
        vaBeginPicture(dpy, context, surfaces[0]);
        VABufferID bufs[] = { seq_buf, pic_buf };
        vaRenderPicture(dpy, context, bufs, 2);
        st = vaEndPicture(dpy, context);
        TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaEndPicture 320x240");
        vaDestroyBuffer(dpy, seq_buf);
        vaDestroyBuffer(dpy, pic_buf);
    }

    /* Frame 2: 640x480 */
    {
        VAEncSequenceParameterBufferH264 seq = { .picture_width_in_mbs = 40, .picture_height_in_mbs = 30 };
        VAEncPictureParameterBufferH264 pic = { .coded_buf = coded, .pic_fields.bits.idr_pic_flag = 1 };
        VABufferID seq_buf, pic_buf;
        vaCreateBuffer(dpy, context, VAEncSequenceParameterBufferType, sizeof(seq), 1, &seq, &seq_buf);
        vaCreateBuffer(dpy, context, VAEncPictureParameterBufferType, sizeof(pic), 1, &pic, &pic_buf);
        vaBeginPicture(dpy, context, surfaces[1]);
        VABufferID bufs[] = { seq_buf, pic_buf };
        vaRenderPicture(dpy, context, bufs, 2);
        st = vaEndPicture(dpy, context);
        TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaEndPicture 640x480");
        vaDestroyBuffer(dpy, seq_buf);
        vaDestroyBuffer(dpy, pic_buf);
    }

    vaDestroyBuffer(dpy, coded);
    vaDestroyContext(dpy, context);
    vaDestroySurfaces(dpy, surfaces, 2);
    vaDestroyConfig(dpy, config);
    TEST_PASS();
}

/* --- Test: Sequential encodes (leak check) --- */

static void test_sequential_encodes(void)
{
    TEST_START("10 sequential H.264 encodes (leak check)");

    for (int run = 0; run < 10; run++) {
        VAConfigAttrib attrib = { .type = VAConfigAttribRTFormat,
                                   .value = VA_RT_FORMAT_YUV420 };
        VAConfigID config;
        vaCreateConfig(dpy, VAProfileH264High, VAEntrypointEncSlice,
                        &attrib, 1, &config);
        VASurfaceID surface;
        vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 320, 240, &surface, 1, NULL, 0);
        VAContextID context;
        vaCreateContext(dpy, config, 320, 240, VA_PROGRESSIVE, &surface, 1, &context);
        VABufferID coded;
        vaCreateBuffer(dpy, context, VAEncCodedBufferType, 320 * 240, 1, NULL, &coded);

        VAEncSequenceParameterBufferH264 seq = {
            .picture_width_in_mbs = 20, .picture_height_in_mbs = 15,
            .intra_period = 30, .ip_period = 1,
        };
        VAEncPictureParameterBufferH264 pic = {
            .coded_buf = coded, .pic_fields.bits.idr_pic_flag = 1,
        };
        VAEncSliceParameterBufferH264 slice = { .slice_type = 2 };
        VABufferID bufs[3];
        vaCreateBuffer(dpy, context, VAEncSequenceParameterBufferType,
                        sizeof(seq), 1, &seq, &bufs[0]);
        vaCreateBuffer(dpy, context, VAEncPictureParameterBufferType,
                        sizeof(pic), 1, &pic, &bufs[1]);
        vaCreateBuffer(dpy, context, VAEncSliceParameterBufferType,
                        sizeof(slice), 1, &slice, &bufs[2]);

        vaBeginPicture(dpy, context, surface);
        vaRenderPicture(dpy, context, bufs, 3);
        VAStatus st = vaEndPicture(dpy, context);
        if (st != VA_STATUS_SUCCESS) {
            TEST_FAIL("vaEndPicture failed in sequential run");
            return;
        }

        vaDestroyBuffer(dpy, coded);
        vaDestroyBuffer(dpy, bufs[0]);
        vaDestroyBuffer(dpy, bufs[1]);
        vaDestroyBuffer(dpy, bufs[2]);
        vaDestroyContext(dpy, context);
        vaDestroySurfaces(dpy, &surface, 1);
        vaDestroyConfig(dpy, config);
    }
    TEST_PASS();
}

/* --- Test: Coded buffer reuse across frames --- */

static void test_coded_buffer_reuse(void)
{
    TEST_START("Coded buffer reuse across 5 frames");

    VAConfigAttrib attrib = { .type = VAConfigAttribRTFormat,
                               .value = VA_RT_FORMAT_YUV420 };
    VAConfigID config;
    vaCreateConfig(dpy, VAProfileH264High, VAEntrypointEncSlice,
                    &attrib, 1, &config);
    VASurfaceID surface;
    vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 320, 240, &surface, 1, NULL, 0);
    VAContextID context;
    vaCreateContext(dpy, config, 320, 240, VA_PROGRESSIVE, &surface, 1, &context);
    VABufferID coded;
    vaCreateBuffer(dpy, context, VAEncCodedBufferType, 320 * 240, 1, NULL, &coded);

    for (int frame = 0; frame < 5; frame++) {
        VAEncSequenceParameterBufferH264 seq = {
            .picture_width_in_mbs = 20, .picture_height_in_mbs = 15,
            .intra_period = 30, .ip_period = 1,
        };
        VAEncPictureParameterBufferH264 pic = {
            .coded_buf = coded,
            .pic_fields.bits.idr_pic_flag = (frame == 0) ? 1 : 0,
        };
        VAEncSliceParameterBufferH264 slice = {
            .slice_type = (frame == 0) ? 2 : 0,
        };
        VABufferID bufs[3];
        vaCreateBuffer(dpy, context, VAEncSequenceParameterBufferType,
                        sizeof(seq), 1, &seq, &bufs[0]);
        vaCreateBuffer(dpy, context, VAEncPictureParameterBufferType,
                        sizeof(pic), 1, &pic, &bufs[1]);
        vaCreateBuffer(dpy, context, VAEncSliceParameterBufferType,
                        sizeof(slice), 1, &slice, &bufs[2]);

        vaBeginPicture(dpy, context, surface);
        vaRenderPicture(dpy, context, bufs, 3);
        VAStatus st = vaEndPicture(dpy, context);
        if (st != VA_STATUS_SUCCESS) {
            TEST_FAIL("vaEndPicture failed");
            goto cleanup;
        }

        VACodedBufferSegment *seg;
        vaMapBuffer(dpy, coded, (void **)&seg);
        if (!seg || !seg->buf || seg->size == 0) {
            TEST_FAIL("empty coded buffer");
            vaUnmapBuffer(dpy, coded);
            goto cleanup;
        }
        vaUnmapBuffer(dpy, coded);

        vaDestroyBuffer(dpy, bufs[0]);
        vaDestroyBuffer(dpy, bufs[1]);
        vaDestroyBuffer(dpy, bufs[2]);
    }
    TEST_PASS();

cleanup:
    vaDestroyBuffer(dpy, coded);
    vaDestroyContext(dpy, context);
    vaDestroySurfaces(dpy, &surface, 1);
    vaDestroyConfig(dpy, config);
}

/* --- Test: many frames within a single long-running encode session --- */

/* Regression test for a crash reported after a few minutes of real-world
 * WebRTC H.264 encoding: the driver used to allocate/register a fresh CUDA
 * linear staging buffer + NVENC resource on every single frame and free/
 * unregister it again afterwards. That per-frame churn (hundreds/thousands
 * of allocations over a real call) destabilized the encode session and
 * crashed the process. The fix reuses one persistent buffer/registration for
 * the whole session. This test drives many more frames through a single
 * context than the other stress tests to exercise that reuse path. */
static void test_long_running_single_session(void)
{
    TEST_START("200 frames in a single long-running H.264 session (crash regression)");

    VAConfigAttrib attrib = { .type = VAConfigAttribRTFormat,
                               .value = VA_RT_FORMAT_YUV420 };
    VAConfigID config;
    vaCreateConfig(dpy, VAProfileH264High, VAEntrypointEncSlice,
                    &attrib, 1, &config);
    VASurfaceID surface;
    vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 1920, 1088, &surface, 1, NULL, 0);
    VAContextID context;
    vaCreateContext(dpy, config, 1920, 1088, VA_PROGRESSIVE, &surface, 1, &context);
    VABufferID coded;
    vaCreateBuffer(dpy, context, VAEncCodedBufferType, 1920 * 1088, 1, NULL, &coded);

    const int numFrames = 200;
    for (int frame = 0; frame < numFrames; frame++) {
        VAEncSequenceParameterBufferH264 seq = {
            .picture_width_in_mbs = 120, .picture_height_in_mbs = 68,
            .intra_period = 0, .ip_period = 1,
        };
        VAEncPictureParameterBufferH264 pic = {
            .coded_buf = coded,
            .pic_fields.bits.idr_pic_flag = (frame == 0) ? 1 : 0,
        };
        VAEncSliceParameterBufferH264 slice = {
            .slice_type = (frame == 0) ? 2 : 0,
        };
        VAEncMiscParameterBuffer *miscBuf;
        VABufferID bufs[4];
        vaCreateBuffer(dpy, context, VAEncSequenceParameterBufferType,
                        sizeof(seq), 1, &seq, &bufs[0]);
        vaCreateBuffer(dpy, context, VAEncPictureParameterBufferType,
                        sizeof(pic), 1, &pic, &bufs[1]);
        vaCreateBuffer(dpy, context, VAEncSliceParameterBufferType,
                        sizeof(slice), 1, &slice, &bufs[2]);

        /* Mimic real-world Chrome behaviour: resend rate-control/framerate
         * misc params on every single frame, matching the reported crash log. */
        size_t miscSize = sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterRateControl);
        vaCreateBuffer(dpy, context, VAEncMiscParameterBufferType,
                        miscSize, 1, NULL, &bufs[3]);
        vaMapBuffer(dpy, bufs[3], (void **)&miscBuf);
        miscBuf->type = VAEncMiscParameterTypeRateControl;
        VAEncMiscParameterRateControl *rc = (VAEncMiscParameterRateControl*) miscBuf->data;
        rc->bits_per_second = 4000000;
        rc->target_percentage = 100;
        vaUnmapBuffer(dpy, bufs[3]);

        vaBeginPicture(dpy, context, surface);
        vaRenderPicture(dpy, context, bufs, 4);
        VAStatus st = vaEndPicture(dpy, context);
        if (st != VA_STATUS_SUCCESS) {
            TEST_FAIL("vaEndPicture failed on frame");
            goto cleanup;
        }

        VACodedBufferSegment *seg;
        vaMapBuffer(dpy, coded, (void **)&seg);
        if (!seg || !seg->buf || seg->size == 0) {
            TEST_FAIL("empty coded buffer");
            vaUnmapBuffer(dpy, coded);
            goto cleanup;
        }
        vaUnmapBuffer(dpy, coded);

        for (int i = 0; i < 4; i++) vaDestroyBuffer(dpy, bufs[i]);
    }
    TEST_PASS();

cleanup:
    vaDestroyBuffer(dpy, coded);
    vaDestroyContext(dpy, context);
    vaDestroySurfaces(dpy, &surface, 1);
    vaDestroyConfig(dpy, config);
}

/* Fill an NV12 image with "natural-looking" content: a smoothly translating
 * gradient (compressible under a generous bit budget, i.e. small frame sizes
 * at high target bitrate) with a moving high-contrast block that shifts each
 * frame (real motion and detail for P-frames to code, which balloons frame
 * sizes when the rate controller is starved and cannot afford quality
 * anymore). Pure random noise is uncompressible so CBR just outputs at the
 * quality floor regardless of target bps -- useless for distinguishing
 * "reconfigure applied" from "reconfigure ignored". A smooth+detail mix
 * gives the rate controller headroom to actually differentiate. */
static void fill_nv12_image_variable(VAImage *img, int frame)
{
    unsigned char *base = NULL;
    if (vaMapBuffer(dpy, img->buf, (void **)&base) != VA_STATUS_SUCCESS) {
        return;
    }
    unsigned char *y = base + img->offsets[0];
    const uint32_t detailPos = (uint32_t)(frame * 3) % (img->width - 128);
    for (uint32_t row = 0; row < img->height; row++) {
        unsigned char *rowp = y + (size_t)row * img->pitches[0];
        for (uint32_t col = 0; col < img->width; col++) {
            /* Smooth gradient base */
            unsigned int v = (col + row + frame * 2) & 0xff;
            /* Sharp detail block that moves each frame */
            if (col >= detailPos && col < detailPos + 128 &&
                row >= img->height / 4 && row < img->height * 3 / 4) {
                v ^= ((col ^ row ^ frame) * 131) & 0xff;
            }
            rowp[col] = (unsigned char)v;
        }
    }
    unsigned char *uv = base + img->offsets[1];
    for (uint32_t row = 0; row < img->height / 2; row++) {
        unsigned char *rowp = uv + (size_t)row * img->pitches[1];
        for (uint32_t col = 0; col < img->width / 2; col++) {
            rowp[col * 2 + 0] = (unsigned char)((col + frame) & 0xff);
            rowp[col * 2 + 1] = (unsigned char)((row + frame * 2) & 0xff);
        }
    }
    vaUnmapBuffer(dpy, img->buf);
}

/* --- Test: bitrate change mid-session is applied via nvEncReconfigureEncoder --- */

/* Regression test for a WebRTC BWE issue observed in Chrome: the codec-specific
 * misc-param handlers recorded new bits_per_second into NVENCContext but never
 * applied it to the running encoder, so BWE bitrate reductions were silently
 * ignored. The encoder kept emitting at the initial rate (~19x the requested
 * one in a real trace), saturating the peer connection and freezing playback.
 *
 * This test drives the same VA-API sequence a browser would: encode N frames
 * at a high bitrate, push a rate-control misc-param that drops the target
 * roughly 10x, then encode another N frames. If the reconfigure actually
 * happens, the second batch's average frame size is meaningfully smaller than
 * the first batch's. If it didn't, both averages would land in the same range.
 */
static void test_bitrate_reconfigure_mid_session(void)
{
    TEST_START("Bitrate change mid-session reconfigures NVENC (BWE regression)");

    VAConfigAttrib attribs[2] = {
        { .type = VAConfigAttribRTFormat,   .value = VA_RT_FORMAT_YUV420 },
        { .type = VAConfigAttribRateControl, .value = VA_RC_CBR },
    };
    VAConfigID config;
    VAStatus st = vaCreateConfig(dpy, VAProfileH264High, VAEntrypointEncSlice,
                                  attribs, 2, &config);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "config");

    const uint32_t W = 1280, H = 720;
    VASurfaceID surface;
    vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, W, H, &surface, 1, NULL, 0);
    VAContextID context;
    vaCreateContext(dpy, config, W, H, VA_PROGRESSIVE, &surface, 1, &context);
    VABufferID coded;
    vaCreateBuffer(dpy, context, VAEncCodedBufferType, W * H, 1, NULL, &coded);

    /* Reusable NV12 upload staging image: without varying pixel content the
     * encoder happily codes an all-zero frame down to ~40 bytes at any bitrate
     * and the assertions below become vacuous. */
    VAImageFormat fmt = { .fourcc = VA_FOURCC_NV12 };
    VAImage image;
    vaCreateImage(dpy, &fmt, W, H, &image);

    /* Warm-up + high-bitrate window, then low-bitrate window. Warm-up frames
     * absorb the initial IDR + rate-control convergence so their sizes don't
     * skew the averages. */
    const int WARMUP = 5;
    const int WINDOW = 25;
    const uint32_t HIGH_BPS = 8000000; /* 8 Mbps */
    const uint32_t LOW_BPS  = 400000;  /* 0.4 Mbps -- 20x drop */

    uint64_t highSum = 0, lowSum = 0;
    int highSamples = 0, lowSamples = 0;
    const int totalFrames = WARMUP + WINDOW + WINDOW;

    for (int frame = 0; frame < totalFrames; frame++) {
        fill_nv12_image_variable(&image, frame);
        vaPutImage(dpy, surface, image.image_id, 0, 0, W, H, 0, 0, W, H);

        VAEncSequenceParameterBufferH264 seq = {
            .picture_width_in_mbs = W / 16, .picture_height_in_mbs = H / 16,
            .intra_period = 0, .ip_period = 1,
            .bits_per_second = HIGH_BPS,
        };
        VAEncPictureParameterBufferH264 pic = {
            .coded_buf = coded,
            .pic_fields.bits.idr_pic_flag = (frame == 0) ? 1 : 0,
        };
        VAEncSliceParameterBufferH264 slice = {
            .slice_type = (frame == 0) ? 2 : 0,
        };

        /* Switch to low bitrate as the transition frame between the two
         * windows -- Chrome resends rate-control every frame in practice. */
        const uint32_t bps = (frame < WARMUP + WINDOW) ? HIGH_BPS : LOW_BPS;

        VABufferID bufs[4];
        vaCreateBuffer(dpy, context, VAEncSequenceParameterBufferType,
                        sizeof(seq), 1, &seq, &bufs[0]);
        vaCreateBuffer(dpy, context, VAEncPictureParameterBufferType,
                        sizeof(pic), 1, &pic, &bufs[1]);
        vaCreateBuffer(dpy, context, VAEncSliceParameterBufferType,
                        sizeof(slice), 1, &slice, &bufs[2]);

        size_t miscSize = sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterRateControl);
        vaCreateBuffer(dpy, context, VAEncMiscParameterBufferType,
                        miscSize, 1, NULL, &bufs[3]);
        VAEncMiscParameterBuffer *miscBuf;
        vaMapBuffer(dpy, bufs[3], (void **)&miscBuf);
        miscBuf->type = VAEncMiscParameterTypeRateControl;
        VAEncMiscParameterRateControl *rc = (VAEncMiscParameterRateControl*) miscBuf->data;
        rc->bits_per_second = bps;
        rc->target_percentage = 100;
        vaUnmapBuffer(dpy, bufs[3]);

        vaBeginPicture(dpy, context, surface);
        vaRenderPicture(dpy, context, bufs, 4);
        VAStatus est = vaEndPicture(dpy, context);
        if (est != VA_STATUS_SUCCESS) {
            TEST_FAIL("vaEndPicture failed mid-session");
            goto cleanup;
        }

        VACodedBufferSegment *seg;
        vaMapBuffer(dpy, coded, (void **)&seg);
        if (!seg || !seg->buf || seg->size == 0) {
            TEST_FAIL("empty coded buffer");
            vaUnmapBuffer(dpy, coded);
            goto cleanup;
        }

        if (frame >= WARMUP && frame < WARMUP + WINDOW) {
            highSum += seg->size;
            highSamples++;
        } else if (frame >= WARMUP + WINDOW) {
            lowSum += seg->size;
            lowSamples++;
        }
        vaUnmapBuffer(dpy, coded);

        for (int i = 0; i < 4; i++) vaDestroyBuffer(dpy, bufs[i]);
    }

    if (highSamples == 0 || lowSamples == 0) {
        TEST_FAIL("no samples collected");
        goto cleanup;
    }

    const double highAvg = (double)highSum / highSamples;
    const double lowAvg  = (double)lowSum  / lowSamples;

    /* At a 20x target-bitrate drop we expect the low-window average to be at
     * least 3x smaller than the high-window average. This tolerance survives
     * rate-control convergence noise and quality-preserving VBV behaviour but
     * catches the "reconfigure never happens" regression, where both averages
     * would land in the same order of magnitude. */
    if (lowAvg > highAvg / 3.0) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "bitrate change not applied: highAvg=%.0f B, lowAvg=%.0f B (need lowAvg < highAvg/3)",
                 highAvg, lowAvg);
        TEST_FAIL(msg);
        goto cleanup;
    }

    TEST_PASS();

cleanup:
    vaDestroyImage(dpy, image.image_id);
    vaDestroyBuffer(dpy, coded);
    vaDestroyContext(dpy, context);
    vaDestroySurfaces(dpy, &surface, 1);
    vaDestroyConfig(dpy, config);
}

/* --- Test: AV1 per-frame base_qindex actually reaches the encoder --- */

/* Chromium runs AV1 encode in CQP mode (kEncodeConstantQuantizationParameter)
 * and does rate control itself in software, pushing the result as
 * VAEncPictureParameterBufferAV1::base_qindex on every frame. It sends no
 * VAEncMiscParameterTypeRateControl for AV1 at all -- so base_qindex is the
 * entire rate-control channel.
 *
 * The driver used to read only temporal_id out of that buffer and drop
 * base_qindex on the floor, which meant the browser's bitrate target had no
 * effect on the output whatsoever. This encodes the same moving content at a
 * low qindex (high quality, large frames) and then a high one (low quality,
 * small frames) and requires the coded sizes to respond. With the field
 * ignored, both windows come out the same size and this fails. */
static void test_av1_base_qindex_applied(void)
{
    TEST_START("AV1 base_qindex drives coded size (Chromium CQP path)");

    VAConfigAttrib attribs[2] = {
        { .type = VAConfigAttribRTFormat,    .value = VA_RT_FORMAT_YUV420 },
        { .type = VAConfigAttribRateControl, .value = VA_RC_CQP },
    };
    VAConfigID config;
    VAStatus st = vaCreateConfig(dpy, VAProfileAV1Profile0, VAEntrypointEncSlice,
                                 attribs, 2, &config);
    if (st != VA_STATUS_SUCCESS) { TEST_SKIP("AV1 encoding not supported"); return; }

    const uint32_t W = 1280, H = 720;
    VASurfaceID surface;
    vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, W, H, &surface, 1, NULL, 0);
    VAContextID context;
    vaCreateContext(dpy, config, W, H, VA_PROGRESSIVE, &surface, 1, &context);
    VABufferID coded;
    vaCreateBuffer(dpy, context, VAEncCodedBufferType, W * H, 1, NULL, &coded);

    VAImageFormat fmt = { .fourcc = VA_FOURCC_NV12 };
    VAImage image;
    vaCreateImage(dpy, &fmt, W, H, &image);

    /* AV1 qindex is 0-255. Keep both values well inside the range and far
     * apart so the comparison is not fighting quantiser granularity. */
    const uint8_t LOW_QINDEX  = 60;   /* high quality -> big frames */
    const uint8_t HIGH_QINDEX = 220;  /* low quality  -> small frames */
    const int WARMUP = 4;
    const int WINDOW = 16;

    uint64_t lowQSum = 0, highQSum = 0;
    int lowQSamples = 0, highQSamples = 0;
    const int totalFrames = WARMUP + WINDOW + WINDOW;

    for (int frame = 0; frame < totalFrames; frame++) {
        fill_nv12_image_variable(&image, frame);
        vaPutImage(dpy, surface, image.image_id, 0, 0, W, H, 0, 0, W, H);

        const uint8_t qindex = (frame < WARMUP + WINDOW) ? LOW_QINDEX : HIGH_QINDEX;

        VAEncSequenceParameterBufferAV1 seq = { .intra_period = 0 };
        VAEncPictureParameterBufferAV1 pic = {
            .coded_buf = coded,
            .frame_width_minus_1 = (uint16_t)(W - 1),
            .frame_height_minus_1 = (uint16_t)(H - 1),
            .base_qindex = qindex,
            .picture_flags.bits.frame_type = (frame == 0) ? 0 : 1,
        };

        VABufferID bufs[2];
        vaCreateBuffer(dpy, context, VAEncSequenceParameterBufferType,
                       sizeof(seq), 1, &seq, &bufs[0]);
        vaCreateBuffer(dpy, context, VAEncPictureParameterBufferType,
                       sizeof(pic), 1, &pic, &bufs[1]);

        vaBeginPicture(dpy, context, surface);
        vaRenderPicture(dpy, context, bufs, 2);
        VAStatus est = vaEndPicture(dpy, context);
        if (est != VA_STATUS_SUCCESS) {
            TEST_FAIL("vaEndPicture failed");
            for (int i = 0; i < 2; i++) vaDestroyBuffer(dpy, bufs[i]);
            goto cleanup;
        }

        VACodedBufferSegment *seg;
        vaMapBuffer(dpy, coded, (void **)&seg);
        if (!seg || !seg->buf || seg->size == 0) {
            TEST_FAIL("empty coded buffer");
            vaUnmapBuffer(dpy, coded);
            for (int i = 0; i < 2; i++) vaDestroyBuffer(dpy, bufs[i]);
            goto cleanup;
        }
        if (frame >= WARMUP && frame < WARMUP + WINDOW) {
            lowQSum += seg->size;
            lowQSamples++;
        } else if (frame >= WARMUP + WINDOW) {
            highQSum += seg->size;
            highQSamples++;
        }
        vaUnmapBuffer(dpy, coded);

        for (int i = 0; i < 2; i++) vaDestroyBuffer(dpy, bufs[i]);
    }

    if (lowQSamples == 0 || highQSamples == 0) {
        TEST_FAIL("no samples collected");
        goto cleanup;
    }

    const double lowQAvg  = (double) lowQSum  / lowQSamples;   /* qindex 60  */
    const double highQAvg = (double) highQSum / highQSamples;  /* qindex 220 */

    /* A ~3.7x qindex increase should shrink frames by far more than 1.5x. The
     * loose factor keeps this robust across NVENC preset/driver revisions while
     * still failing hard if base_qindex is ignored, in which case the two
     * averages are essentially identical. */
    if (highQAvg > lowQAvg / 1.5) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "base_qindex ignored: qindex=%u avg=%.0f B vs qindex=%u avg=%.0f B "
                 "(need high-qindex avg < low/1.5)",
                 LOW_QINDEX, lowQAvg, HIGH_QINDEX, highQAvg);
        TEST_FAIL(msg);
        goto cleanup;
    }

    TEST_PASS();

cleanup:
    vaDestroyImage(dpy, image.image_id);
    vaDestroyBuffer(dpy, coded);
    vaDestroyContext(dpy, context);
    vaDestroySurfaces(dpy, &surface, 1);
    vaDestroyConfig(dpy, config);
}

/* --- Test: repeated bitrate changes each get applied --- */

/* Chrome pushes a fresh rate-control misc-param on every single frame in the
 * traces we've captured, and BWE frequently ramps the target up and down. This
 * verifies each direction of change is honoured -- not just "one change once".
 * If reconfigure only worked on the very first change (e.g. a stale-value
 * check compared the wrong field) the ramp-up would fail this test. */
static void test_bitrate_reconfigure_ramp_down_and_up(void)
{
    TEST_START("Repeated bitrate changes (down then up) each take effect");

    VAConfigAttrib attribs[2] = {
        { .type = VAConfigAttribRTFormat,   .value = VA_RT_FORMAT_YUV420 },
        { .type = VAConfigAttribRateControl, .value = VA_RC_CBR },
    };
    VAConfigID config;
    VAStatus st = vaCreateConfig(dpy, VAProfileH264High, VAEntrypointEncSlice,
                                  attribs, 2, &config);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "config");

    const uint32_t W = 1280, H = 720;
    VASurfaceID surface;
    vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, W, H, &surface, 1, NULL, 0);
    VAContextID context;
    vaCreateContext(dpy, config, W, H, VA_PROGRESSIVE, &surface, 1, &context);
    VABufferID coded;
    vaCreateBuffer(dpy, context, VAEncCodedBufferType, W * H, 1, NULL, &coded);

    VAImageFormat fmt = { .fourcc = VA_FOURCC_NV12 };
    VAImage image;
    vaCreateImage(dpy, &fmt, W, H, &image);

    const int WARMUP = 5;
    const int WINDOW = 20;
    const uint32_t HIGH_BPS = 6000000;
    const uint32_t LOW_BPS  = 500000;

    uint64_t sum[3] = {0, 0, 0};    /* high, low, high-again */
    int      cnt[3] = {0, 0, 0};

    /* Three windows: high, low, high. All prefixed by WARMUP frames each. */
    const int perStage = WARMUP + WINDOW;
    const int totalFrames = 3 * perStage;

    for (int frame = 0; frame < totalFrames; frame++) {
        fill_nv12_image_variable(&image, frame);
        vaPutImage(dpy, surface, image.image_id, 0, 0, W, H, 0, 0, W, H);

        const int stage = frame / perStage;
        const int stageFrame = frame % perStage;
        const uint32_t bps = (stage == 1) ? LOW_BPS : HIGH_BPS;

        VAEncSequenceParameterBufferH264 seq = {
            .picture_width_in_mbs = W / 16, .picture_height_in_mbs = H / 16,
            .intra_period = 0, .ip_period = 1,
        };
        VAEncPictureParameterBufferH264 pic = {
            .coded_buf = coded,
            .pic_fields.bits.idr_pic_flag = (frame == 0) ? 1 : 0,
        };
        VAEncSliceParameterBufferH264 slice = {
            .slice_type = (frame == 0) ? 2 : 0,
        };

        VABufferID bufs[4];
        vaCreateBuffer(dpy, context, VAEncSequenceParameterBufferType,
                        sizeof(seq), 1, &seq, &bufs[0]);
        vaCreateBuffer(dpy, context, VAEncPictureParameterBufferType,
                        sizeof(pic), 1, &pic, &bufs[1]);
        vaCreateBuffer(dpy, context, VAEncSliceParameterBufferType,
                        sizeof(slice), 1, &slice, &bufs[2]);

        size_t miscSize = sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterRateControl);
        vaCreateBuffer(dpy, context, VAEncMiscParameterBufferType,
                        miscSize, 1, NULL, &bufs[3]);
        VAEncMiscParameterBuffer *miscBuf;
        vaMapBuffer(dpy, bufs[3], (void **)&miscBuf);
        miscBuf->type = VAEncMiscParameterTypeRateControl;
        VAEncMiscParameterRateControl *rc = (VAEncMiscParameterRateControl*) miscBuf->data;
        rc->bits_per_second = bps;
        rc->target_percentage = 100;
        vaUnmapBuffer(dpy, bufs[3]);

        vaBeginPicture(dpy, context, surface);
        vaRenderPicture(dpy, context, bufs, 4);
        VAStatus est = vaEndPicture(dpy, context);
        if (est != VA_STATUS_SUCCESS) {
            TEST_FAIL("vaEndPicture failed during ramp");
            goto cleanup;
        }

        VACodedBufferSegment *seg;
        vaMapBuffer(dpy, coded, (void **)&seg);
        if (!seg || !seg->buf || seg->size == 0) {
            TEST_FAIL("empty coded buffer during ramp");
            vaUnmapBuffer(dpy, coded);
            goto cleanup;
        }
        if (stageFrame >= WARMUP) {
            sum[stage] += seg->size;
            cnt[stage]++;
        }
        vaUnmapBuffer(dpy, coded);

        for (int i = 0; i < 4; i++) vaDestroyBuffer(dpy, bufs[i]);
    }

    if (cnt[0] == 0 || cnt[1] == 0 || cnt[2] == 0) {
        TEST_FAIL("missing samples in ramp windows");
        goto cleanup;
    }

    const double highAvg     = (double)sum[0] / cnt[0];
    const double lowAvg      = (double)sum[1] / cnt[1];
    const double highAgainAvg = (double)sum[2] / cnt[2];

    /* Ramp-down must halve at least: catches "reconfigure ignored". */
    if (lowAvg > highAvg / 2.0) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "ramp-down not applied: high=%.0f low=%.0f", highAvg, lowAvg);
        TEST_FAIL(msg);
        goto cleanup;
    }
    /* Ramp-up must recover to at least 2x the low-window average: catches
     * "reconfigure only takes the first change". */
    if (highAgainAvg < lowAvg * 2.0) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "ramp-up not applied: low=%.0f high-again=%.0f", lowAvg, highAgainAvg);
        TEST_FAIL(msg);
        goto cleanup;
    }

    TEST_PASS();

cleanup:
    vaDestroyImage(dpy, image.image_id);
    vaDestroyBuffer(dpy, coded);
    vaDestroyContext(dpy, context);
    vaDestroySurfaces(dpy, &surface, 1);
    vaDestroyConfig(dpy, config);
}

/* --- Test: framerate change mid-session doesn't break encode --- */

/* Chrome also pushes VAEncMiscParameterTypeFrameRate when its capture rate
 * changes (e.g. tab visibility, throttled camera). Even if the perceptual
 * effect is subtle, we should hand the new framerate to NVENC so its rate-
 * control math is against the right frame budget. A regression that made
 * this call error out would surface as vaEndPicture failing, which this
 * test catches deterministically. */
static void test_framerate_reconfigure_mid_session(void)
{
    TEST_START("Framerate change mid-session doesn't break encode");

    VAConfigAttrib attribs[2] = {
        { .type = VAConfigAttribRTFormat,   .value = VA_RT_FORMAT_YUV420 },
        { .type = VAConfigAttribRateControl, .value = VA_RC_CBR },
    };
    VAConfigID config;
    VAStatus st = vaCreateConfig(dpy, VAProfileH264High, VAEntrypointEncSlice,
                                  attribs, 2, &config);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "config");

    const uint32_t W = 640, H = 480;
    VASurfaceID surface;
    vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, W, H, &surface, 1, NULL, 0);
    VAContextID context;
    vaCreateContext(dpy, config, W, H, VA_PROGRESSIVE, &surface, 1, &context);
    VABufferID coded;
    vaCreateBuffer(dpy, context, VAEncCodedBufferType, W * H, 1, NULL, &coded);

    VAImageFormat fmt = { .fourcc = VA_FOURCC_NV12 };
    VAImage image;
    vaCreateImage(dpy, &fmt, W, H, &image);

    const uint32_t rates[] = { 30, 15, 24, 60, 12 };
    const int perStage = 10;
    int frameCounter = 0;

    for (size_t r = 0; r < sizeof(rates)/sizeof(rates[0]); r++) {
        const uint32_t fps = rates[r];
        for (int frame = 0; frame < perStage; frame++) {
            fill_nv12_image_variable(&image, frameCounter);
            vaPutImage(dpy, surface, image.image_id, 0, 0, W, H, 0, 0, W, H);
            frameCounter++;

            const bool isFirstFrameEver = (r == 0 && frame == 0);
            VAEncSequenceParameterBufferH264 seq = {
                .picture_width_in_mbs = W / 16, .picture_height_in_mbs = H / 16,
                .intra_period = 0, .ip_period = 1,
            };
            VAEncPictureParameterBufferH264 pic = {
                .coded_buf = coded,
                .pic_fields.bits.idr_pic_flag = isFirstFrameEver ? 1 : 0,
            };
            VAEncSliceParameterBufferH264 slice = {
                .slice_type = isFirstFrameEver ? 2 : 0,
            };

            VABufferID bufs[5];
            vaCreateBuffer(dpy, context, VAEncSequenceParameterBufferType,
                            sizeof(seq), 1, &seq, &bufs[0]);
            vaCreateBuffer(dpy, context, VAEncPictureParameterBufferType,
                            sizeof(pic), 1, &pic, &bufs[1]);
            vaCreateBuffer(dpy, context, VAEncSliceParameterBufferType,
                            sizeof(slice), 1, &slice, &bufs[2]);

            size_t rcSize = sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterRateControl);
            vaCreateBuffer(dpy, context, VAEncMiscParameterBufferType,
                            rcSize, 1, NULL, &bufs[3]);
            VAEncMiscParameterBuffer *rcMisc;
            vaMapBuffer(dpy, bufs[3], (void **)&rcMisc);
            rcMisc->type = VAEncMiscParameterTypeRateControl;
            VAEncMiscParameterRateControl *rc = (VAEncMiscParameterRateControl*) rcMisc->data;
            rc->bits_per_second = 1500000;
            rc->target_percentage = 100;
            vaUnmapBuffer(dpy, bufs[3]);

            size_t frSize = sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterFrameRate);
            vaCreateBuffer(dpy, context, VAEncMiscParameterBufferType,
                            frSize, 1, NULL, &bufs[4]);
            VAEncMiscParameterBuffer *frMisc;
            vaMapBuffer(dpy, bufs[4], (void **)&frMisc);
            frMisc->type = VAEncMiscParameterTypeFrameRate;
            VAEncMiscParameterFrameRate *fr = (VAEncMiscParameterFrameRate*) frMisc->data;
            fr->framerate = fps; /* den=1 encoded implicitly */
            vaUnmapBuffer(dpy, bufs[4]);

            vaBeginPicture(dpy, context, surface);
            vaRenderPicture(dpy, context, bufs, 5);
            VAStatus est = vaEndPicture(dpy, context);
            if (est != VA_STATUS_SUCCESS) {
                char msg[64];
                snprintf(msg, sizeof(msg), "vaEndPicture failed at fps=%u", fps);
                TEST_FAIL(msg);
                for (int i = 0; i < 5; i++) vaDestroyBuffer(dpy, bufs[i]);
                goto cleanup;
            }

            VACodedBufferSegment *seg;
            vaMapBuffer(dpy, coded, (void **)&seg);
            bool empty = (!seg || !seg->buf || seg->size == 0);
            vaUnmapBuffer(dpy, coded);
            if (empty) {
                TEST_FAIL("empty coded buffer after framerate change");
                for (int i = 0; i < 5; i++) vaDestroyBuffer(dpy, bufs[i]);
                goto cleanup;
            }

            for (int i = 0; i < 5; i++) vaDestroyBuffer(dpy, bufs[i]);
        }
    }

    TEST_PASS();

cleanup:
    vaDestroyImage(dpy, image.image_id);
    vaDestroyBuffer(dpy, coded);
    vaDestroyContext(dpy, context);
    vaDestroySurfaces(dpy, &surface, 1);
    vaDestroyConfig(dpy, config);
}

/* --- Test: automatic descriptor mode (NVD_DESCRIPTOR_MODE unset/auto) --- */

/* With NVD_DESCRIPTOR_MODE left unset (the default AUTO mode), surfaces that
 * belong to an encode context should be exported as a single combined-fourcc
 * layer (e.g. NV12 with 2 planes), since that's the layout Chrome's
 * WebGL/canvas "video-processing" worker path needs to render the local
 * capture/preview into before it's handed to NVENC. This must happen without
 * the user having to set NVD_DESCRIPTOR_MODE=combined manually. */
static void test_encode_surface_export_auto_combined(void)
{
    TEST_START("Encode surface export defaults to combined layer (auto mode)");

    VAConfigAttrib attrib = { .type = VAConfigAttribRTFormat,
                               .value = VA_RT_FORMAT_YUV420 };
    VAConfigID config;
    VAStatus st = vaCreateConfig(dpy, VAProfileH264High, VAEntrypointEncSlice,
                                  &attrib, 1, &config);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "config");

    VASurfaceID surface;
    st = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 320, 240, &surface, 1, NULL, 0);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "surface");

    VAContextID context;
    st = vaCreateContext(dpy, config, 320, 240, VA_PROGRESSIVE, &surface, 1, &context);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "context");

    VABufferID coded_buf;
    st = vaCreateBuffer(dpy, context, VAEncCodedBufferType, 320 * 240, 1, NULL, &coded_buf);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "coded_buf");

    /* vaBeginPicture() is what associates the surface with its (encode)
     * context internally; the export decision relies on that association
     * being in place, matching how Chrome always drives a real session. */
    st = vaBeginPicture(dpy, context, surface);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaBeginPicture");

    VADRMPRIMESurfaceDescriptor desc;
    st = vaExportSurfaceHandle(dpy, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaExportSurfaceHandle failed");
    TEST_ASSERT(desc.num_objects == 1, "encode surface should export a single DMA-BUF object");
    TEST_ASSERT(desc.num_layers == 1,
                "encode surface should default to a single combined layer, not split per-plane");
    for (int i = 0; i < desc.num_objects; i++) close(desc.objects[i].fd);

    /* Skip vaRenderPicture/vaEndPicture — this test only exercises the
     * export-layout decision, not a full encode cycle (that's covered by
     * test_encode_one_frame() and friends elsewhere). */
    vaDestroyBuffer(dpy, coded_buf);
    vaDestroyContext(dpy, context);
    vaDestroySurfaces(dpy, &surface, 1);
    vaDestroyConfig(dpy, config);
    TEST_PASS();
}

/* --- Test: Decode regression --- */

static void test_decode_still_works(void)
{
    TEST_START("Decode entrypoints still present (VLD)");
    int ne = vaMaxNumEntrypoints(dpy);
    VAEntrypoint *eps = calloc(ne, sizeof(VAEntrypoint));
    int n = 0;
    vaQueryConfigEntrypoints(dpy, VAProfileH264High, eps, &n);
    bool found_vld = false;
    bool found_enc = false;
    for (int i = 0; i < n; i++) {
        if (eps[i] == VAEntrypointVLD) found_vld = true;
        if (eps[i] == VAEntrypointEncSlice) found_enc = true;
    }
    free(eps);
    TEST_ASSERT(found_vld, "VAEntrypointVLD missing");
    TEST_ASSERT(found_enc, "VAEntrypointEncSlice missing");
    TEST_PASS();
}

static void test_encode_mismatch_dimensions(void)
{
    TEST_START("Encode 1920x1080 surface with 1920x1088 context");

    VAConfigAttrib attrib = { .type = VAConfigAttribRTFormat,
                               .value = VA_RT_FORMAT_YUV420 };
    VAConfigID config;
    VAStatus st = vaCreateConfig(dpy, VAProfileH264High, VAEntrypointEncSlice,
                                  &attrib, 1, &config);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "config");

    VASurfaceID surface;
    /* Source surface is 1920x1080 */
    st = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 1920, 1080,
                           &surface, 1, NULL, 0);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "surface");

    VAContextID context;
    /* Context is 1920x1088 (mismatch) */
    st = vaCreateContext(dpy, config, 1920, 1088, VA_PROGRESSIVE,
                          &surface, 1, &context);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "context");

    VABufferID coded_buf;
    st = vaCreateBuffer(dpy, context, VAEncCodedBufferType, 1920 * 1088,
                         1, NULL, &coded_buf);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "coded_buf");

    VAEncSequenceParameterBufferH264 seq = {
        .picture_width_in_mbs = 120, /* 1920 / 16 */
        .picture_height_in_mbs = 68,  /* 1088 / 16 */
        .intra_period = 30, .ip_period = 1,
    };
    VABufferID seq_buf;
    vaCreateBuffer(dpy, context, VAEncSequenceParameterBufferType,
                    sizeof(seq), 1, &seq, &seq_buf);

    VAEncPictureParameterBufferH264 pic = {
        .coded_buf = coded_buf,
        .pic_fields.bits.idr_pic_flag = 1,
    };
    VABufferID pic_buf;
    vaCreateBuffer(dpy, context, VAEncPictureParameterBufferType,
                    sizeof(pic), 1, &pic, &pic_buf);

    VAEncSliceParameterBufferH264 slice = { .slice_type = 2 };
    VABufferID slice_buf;
    vaCreateBuffer(dpy, context, VAEncSliceParameterBufferType,
                    sizeof(slice), 1, &slice, &slice_buf);

    st = vaBeginPicture(dpy, context, surface);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaBeginPicture");
    VABufferID bufs[] = { seq_buf, pic_buf, slice_buf };
    st = vaRenderPicture(dpy, context, bufs, 3);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaRenderPicture");
    st = vaEndPicture(dpy, context);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaEndPicture");

    st = vaSyncSurface(dpy, surface);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaSyncSurface");

    vaDestroyBuffer(dpy, coded_buf);
    vaDestroyBuffer(dpy, seq_buf);
    vaDestroyBuffer(dpy, pic_buf);
    vaDestroyBuffer(dpy, slice_buf);
    vaDestroyContext(dpy, context);
    vaDestroySurfaces(dpy, &surface, 1);
    vaDestroyConfig(dpy, config);
    TEST_PASS();
}

static void test_h264_b_frame(void)
{
    TEST_START("H.264 B-frame encode (manual reordering)");

    VAConfigAttrib attrib = { .type = VAConfigAttribRTFormat,
                               .value = VA_RT_FORMAT_YUV420 };
    VAConfigID config;
    vaCreateConfig(dpy, VAProfileH264High, VAEntrypointEncSlice, &attrib, 1, &config);

    VASurfaceID surface;
    vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 320, 240, &surface, 1, NULL, 0);

    VAContextID context;
    vaCreateContext(dpy, config, 320, 240, VA_PROGRESSIVE, &surface, 1, &context);

    VABufferID coded_buf;
    vaCreateBuffer(dpy, context, VAEncCodedBufferType, 320 * 240, 1, NULL, &coded_buf);

    /* Encode order: I, P, B */
    int slice_types[] = { 2 /* I */, 0 /* P */, 1 /* B */ };

    for (int i = 0; i < 3; i++) {
        VAEncSequenceParameterBufferH264 seq = {
            .picture_width_in_mbs = 320 / 16,
            .picture_height_in_mbs = 240 / 16,
            .intra_period = 30, .ip_period = 2, /* 1 B-frame */
        };
        VABufferID seq_buf;
        vaCreateBuffer(dpy, context, VAEncSequenceParameterBufferType, sizeof(seq), 1, &seq, &seq_buf);

        VAEncPictureParameterBufferH264 pic = { .coded_buf = coded_buf };
        if (i == 0) pic.pic_fields.bits.idr_pic_flag = 1;
        VABufferID pic_buf;
        vaCreateBuffer(dpy, context, VAEncPictureParameterBufferType, sizeof(pic), 1, &pic, &pic_buf);

        VAEncSliceParameterBufferH264 slice = { .slice_type = slice_types[i] };
        VABufferID slice_buf;
        vaCreateBuffer(dpy, context, VAEncSliceParameterBufferType, sizeof(slice), 1, &slice, &slice_buf);

        vaBeginPicture(dpy, context, surface);
        VABufferID bufs[] = { seq_buf, pic_buf, slice_buf };
        vaRenderPicture(dpy, context, bufs, 3);
        vaEndPicture(dpy, context);
        vaSyncSurface(dpy, surface);

        VACodedBufferSegment *seg;
        vaMapBuffer(dpy, coded_buf, (void**)&seg);
        TEST_ASSERT(seg->size > 0, "packet should not be empty");
        vaUnmapBuffer(dpy, coded_buf);

        vaDestroyBuffer(dpy, seq_buf);
        vaDestroyBuffer(dpy, pic_buf);
        vaDestroyBuffer(dpy, slice_buf);
    }

    vaDestroyBuffer(dpy, coded_buf);
    vaDestroyContext(dpy, context);
    vaDestroySurfaces(dpy, &surface, 1);
    vaDestroyConfig(dpy, config);
    TEST_PASS();
}

/* --- Main --- */

int main(int argc, char **argv)
{
    bool run_h264 = true, run_hevc = true, run_av1 = true;
    if (argc > 1) {
        if (strcmp(argv[1], "h264") == 0) { run_hevc = false; run_av1 = false; }
        else if (strcmp(argv[1], "hevc") == 0) { run_h264 = false; run_av1 = false; }
        else if (strcmp(argv[1], "av1") == 0) { run_h264 = false; run_hevc = false; }
    }

    setup();

    printf("\n=== nvidia-vaapi-driver encode tests ===\n");
    printf("Driver: %s\n\n", vaQueryVendorString(dpy));

    printf("Entrypoints:\n");
    test_entrypoints_h264();
    test_entrypoints_hevc();

    printf("\nConfig:\n");
    test_config_attributes();
    test_rate_control_params();
    test_quality_level_param();

    printf("\nLifecycle:\n");
    test_create_destroy();
    test_dynamic_resolution();

    if (run_h264) {
        printf("\nH.264 Encode:\n");
        test_encode_one_frame(VAProfileH264High, "H.264 High");
        test_encode_one_frame(VAProfileH264Main, "H.264 Main");
        test_encode_one_frame(VAProfileH264ConstrainedBaseline, "H.264 CB");
        test_h264_b_frame();
    }

    if (run_hevc) {
        printf("\nHEVC Encode:\n");
        test_encode_one_frame(VAProfileHEVCMain, "HEVC Main");
        test_hevc_main10_one_frame();
    }

    if (run_av1) {
        printf("\nAV1 Encode:\n");
        test_av1_one_frame();
        test_av1_main10_one_frame();
        test_av1_combined_rtformat_encode();
        test_av1_temporal_layers();
        test_av1_temporal_svc_encode();
        test_av1_base_qindex_applied();
    }

    printf("\nStress:\n");
    test_sequential_encodes();
    test_coded_buffer_reuse();
    test_long_running_single_session();

    printf("\nDynamic reconfigure (BWE / camera rate changes):\n");
    test_bitrate_reconfigure_mid_session();
    test_bitrate_reconfigure_ramp_down_and_up();
    test_framerate_reconfigure_mid_session();

    printf("\nDescriptor mode (auto):\n");
    test_encode_surface_export_auto_combined();

    printf("\nRegression:\n");
    test_decode_still_works();
    test_encode_mismatch_dimensions();

    printf("\n=== Results: %d passed, %d failed ===\n\n",
           pass_count, fail_count);

    teardown();
    return fail_count > 0 ? 1 : 0;
}

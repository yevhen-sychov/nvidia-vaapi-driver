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
        test_av1_temporal_layers();
    }

    printf("\nStress:\n");
    test_sequential_encodes();
    test_coded_buffer_reuse();

    printf("\nRegression:\n");
    test_decode_still_works();
    test_encode_mismatch_dimensions();

    printf("\n=== Results: %d passed, %d failed ===\n\n",
           pass_count, fail_count);

    teardown();
    return fail_count > 0 ? 1 : 0;
}

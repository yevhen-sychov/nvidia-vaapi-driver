/*
 * test_encode_only.c — the CUDA-less encode-only path.
 *
 * This is the mode a 32-bit client lands in: cuInit() fails in-process (that is
 * how Steam's 32-bit client sees a Blackwell card), the driver reports
 * "IPC encode-only", and every encode is delegated to the 64-bit nvenc-helper.
 * Nothing else in the suite covers it, which is how the driver came to report
 * zero encode entrypoints there while every CUDA-side test stayed green.
 *
 * Driven by tests/test_encode_only.sh, which forces the mode with
 * CUDA_VISIBLE_DEVICES="" and controls whether a helper is reachable.
 *
 * Modes:
 *   ./test_encode_only helper    — helper reachable: real capabilities over IPC,
 *                                  plus an end-to-end encode through it.
 *   ./test_encode_only fallback  — no helper: the driver must still advertise
 *                                  its built-in encode profiles rather than
 *                                  concluding the GPU cannot encode.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>

#define DRM_DEVICE "/dev/dri/renderD128"

static int pass_count = 0;
static int fail_count = 0;

#define TEST_START(name) \
    printf("  %-55s ", name); fflush(stdout);

#define TEST_PASS() do { \
    printf("\033[32mPASS\033[0m\n"); pass_count++; \
} while (0)

#define TEST_FAIL(reason) do { \
    printf("\033[31mFAIL\033[0m (%s)\n", reason); fail_count++; \
} while (0)

#define TEST_ASSERT(cond, reason) do { \
    if (!(cond)) { TEST_FAIL(reason); return; } \
} while (0)

static VADisplay dpy;
static int drm_fd;

static bool has_entrypoint(VAProfile profile, VAEntrypoint want)
{
    int max = vaMaxNumEntrypoints(dpy);
    VAEntrypoint *eps = calloc(max, sizeof(VAEntrypoint));
    int n = 0;
    vaQueryConfigEntrypoints(dpy, profile, eps, &n);
    bool found = false;
    for (int i = 0; i < n; i++) {
        if (eps[i] == want) { found = true; break; }
    }
    free(eps);
    return found;
}

static bool has_profile(VAProfile want)
{
    int max = vaMaxNumConfigAttributes(dpy); /* unused, keeps libva happy on old headers */
    (void) max;
    int nprofiles = vaMaxNumProfiles(dpy);
    VAProfile *profiles = calloc(nprofiles, sizeof(VAProfile));
    int n = 0;
    vaQueryConfigProfiles(dpy, profiles, &n);
    bool found = false;
    for (int i = 0; i < n; i++) {
        if (profiles[i] == want) { found = true; break; }
    }
    free(profiles);
    return found;
}

/* --- The mode itself --- */

static void test_is_encode_only(void)
{
    TEST_START("driver reports encode-only mode");
    const char *vendor = vaQueryVendorString(dpy);
    TEST_ASSERT(vendor != NULL, "no vendor string");
    TEST_ASSERT(strstr(vendor, "encode-only") != NULL, vendor);
    TEST_PASS();
}

static void test_no_decode_entrypoint(void)
{
    TEST_START("no VLD entrypoint without CUDA");
    TEST_ASSERT(!has_entrypoint(VAProfileH264High, VAEntrypointVLD),
                "VLD advertised with no CUDA context to decode with");
    TEST_PASS();
}

/* The regression this suite exists for: a capability probe that cannot run
 * must not be read as "this GPU encodes nothing". When it was, ffmpeg walked
 * away with "No usable encoding entrypoint found" for every profile and Steam
 * fell back to x264 on the CPU. */
static void test_base_profiles_advertised(void)
{
    static const struct { VAProfile profile; const char *name; } base[] = {
        { VAProfileH264ConstrainedBaseline, "H264 ConstrainedBaseline" },
        { VAProfileH264Main,                "H264 Main" },
        { VAProfileH264High,                "H264 High" },
        { VAProfileHEVCMain,                "HEVC Main" },
    };

    for (size_t i = 0; i < sizeof(base)/sizeof(base[0]); i++) {
        char name[80];
        snprintf(name, sizeof(name), "%s listed as an encode profile", base[i].name);
        TEST_START(name);
        if (!has_profile(base[i].profile)) {
            TEST_FAIL("missing from vaQueryConfigProfiles");
            continue;
        }
        if (!has_entrypoint(base[i].profile, VAEntrypointEncSlice)) {
            TEST_FAIL("profile listed but no EncSlice entrypoint");
            continue;
        }
        TEST_PASS();
    }
}

static void test_config_attributes(void)
{
    TEST_START("H.264 encode config attributes are answered");
    VAConfigAttrib attribs[3] = {
        { .type = VAConfigAttribRTFormat },
        { .type = VAConfigAttribMaxPictureWidth },
        { .type = VAConfigAttribMaxPictureHeight },
    };
    VAStatus st = vaGetConfigAttributes(dpy, VAProfileH264High, VAEntrypointEncSlice,
                                        attribs, 3);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaGetConfigAttributes failed");
    TEST_ASSERT((attribs[0].value & VA_RT_FORMAT_YUV420) != 0, "no YUV420 RT format");
    TEST_ASSERT(attribs[1].value != VA_ATTRIB_NOT_SUPPORTED && attribs[1].value >= 1920,
                "max encode width below 1080p");
    TEST_ASSERT(attribs[2].value != VA_ATTRIB_NOT_SUPPORTED && attribs[2].value >= 1080,
                "max encode height below 1080p");
    TEST_PASS();
}

/* --- End-to-end encode through the helper --- */

static void test_encode_one_frame(VAProfile profile, const char *codec_name)
{
    const int w = 320, h = 240;
    char name[80];
    snprintf(name, sizeof(name), "%s encode one frame via IPC helper", codec_name);
    TEST_START(name);

    bool is_h264 = (profile != VAProfileHEVCMain);

    VAConfigAttrib attrib = { .type = VAConfigAttribRTFormat,
                              .value = VA_RT_FORMAT_YUV420 };
    VAConfigID config;
    VAStatus st = vaCreateConfig(dpy, profile, VAEntrypointEncSlice, &attrib, 1, &config);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateConfig");

    VASurfaceID surface;
    st = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, w, h, &surface, 1, NULL, 0);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateSurfaces");

    VAContextID context;
    st = vaCreateContext(dpy, config, w, h, VA_PROGRESSIVE, &surface, 1, &context);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateContext");

    VABufferID coded_buf;
    st = vaCreateBuffer(dpy, context, VAEncCodedBufferType, w * h, 1, NULL, &coded_buf);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateBuffer(coded)");

    /* Fill the surface the way a CUDA-less client has to: derive a host image
     * and write pixels into it. This is Steam's path -- there is no CUDA in
     * this process to upload with. */
    VAImage image;
    st = vaDeriveImage(dpy, surface, &image);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaDeriveImage");
    TEST_ASSERT(image.format.fourcc == VA_FOURCC_NV12, "derived image is not NV12");
    void *img_data = NULL;
    st = vaMapBuffer(dpy, image.buf, &img_data);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaMapBuffer(image)");
    memset(img_data, 128, image.data_size);        /* mid-gray luma + neutral chroma */
    memset((char*)img_data + image.offsets[0], 60, (size_t) image.pitches[0] * h / 2);
    vaUnmapBuffer(dpy, image.buf);
    vaDestroyImage(dpy, image.image_id);

    VABufferID seq_buf, pic_buf, slice_buf;
    if (is_h264) {
        VAEncSequenceParameterBufferH264 seq = {
            .picture_width_in_mbs = w / 16, .picture_height_in_mbs = h / 16,
            .intra_period = 30, .ip_period = 1,
        };
        vaCreateBuffer(dpy, context, VAEncSequenceParameterBufferType,
                       sizeof(seq), 1, &seq, &seq_buf);
        VAEncPictureParameterBufferH264 pic = {
            .coded_buf = coded_buf, .pic_fields.bits.idr_pic_flag = 1,
        };
        vaCreateBuffer(dpy, context, VAEncPictureParameterBufferType,
                       sizeof(pic), 1, &pic, &pic_buf);
        VAEncSliceParameterBufferH264 slice = { .slice_type = 2 };
        vaCreateBuffer(dpy, context, VAEncSliceParameterBufferType,
                       sizeof(slice), 1, &slice, &slice_buf);
    } else {
        VAEncSequenceParameterBufferHEVC seq = {
            .pic_width_in_luma_samples = w, .pic_height_in_luma_samples = h,
            .intra_period = 30, .ip_period = 1,
        };
        vaCreateBuffer(dpy, context, VAEncSequenceParameterBufferType,
                       sizeof(seq), 1, &seq, &seq_buf);
        VAEncPictureParameterBufferHEVC pic = {
            .coded_buf = coded_buf, .pic_fields.bits.idr_pic_flag = 1,
        };
        vaCreateBuffer(dpy, context, VAEncPictureParameterBufferType,
                       sizeof(pic), 1, &pic, &pic_buf);
        VAEncSliceParameterBufferHEVC slice = { .slice_type = 2 };
        vaCreateBuffer(dpy, context, VAEncSliceParameterBufferType,
                       sizeof(slice), 1, &slice, &slice_buf);
    }

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
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaMapBuffer(coded)");
    TEST_ASSERT(seg != NULL && seg->buf != NULL, "no coded segment");
    TEST_ASSERT(seg->size > 0, "coded size is 0");
    unsigned char *bs = (unsigned char *) seg->buf;
    TEST_ASSERT(bs[0] == 0 && bs[1] == 0 && bs[2] == 0 && bs[3] == 1,
                "no NAL start code 00 00 00 01");
    vaUnmapBuffer(dpy, coded_buf);

    vaDestroyBuffer(dpy, coded_buf);
    vaDestroyBuffer(dpy, seq_buf);
    vaDestroyBuffer(dpy, pic_buf);
    vaDestroyBuffer(dpy, slice_buf);
    vaDestroyContext(dpy, context);
    vaDestroySurfaces(dpy, &surface, 1);
    vaDestroyConfig(dpy, config);
    TEST_PASS();
}

int main(int argc, char **argv)
{
    bool with_helper = (argc > 1 && strcmp(argv[1], "helper") == 0);

    drm_fd = open(DRM_DEVICE, O_RDWR);
    if (drm_fd < 0) {
        fprintf(stderr, "Cannot open %s\n", DRM_DEVICE);
        return 1;
    }
    dpy = vaGetDisplayDRM(drm_fd);
    if (!dpy) {
        fprintf(stderr, "vaGetDisplayDRM failed\n");
        return 1;
    }
    int major, minor;
    VAStatus st = vaInitialize(dpy, &major, &minor);
    if (st != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaInitialize failed: %d\n", st);
        return 1;
    }

    printf("\n=== encode-only mode (%s) ===\n", with_helper ? "helper reachable"
                                                            : "no helper");
    printf("Driver: %s\n\n", vaQueryVendorString(dpy));

    const char *vendor = vaQueryVendorString(dpy);
    if (vendor == NULL || strstr(vendor, "encode-only") == NULL) {
        /* CUDA came up despite CUDA_VISIBLE_DEVICES="" -- the mode under test
         * does not exist on this box, and passing the CUDA-path tests here
         * would prove nothing. */
        printf("  CUDA initialised anyway; encode-only mode unreachable. Skipping.\n\n");
        vaTerminate(dpy);
        close(drm_fd);
        return 77; /* meson: skipped */
    }

    printf("Mode:\n");
    test_is_encode_only();
    test_no_decode_entrypoint();

    printf("\nAdvertised encode surface:\n");
    test_base_profiles_advertised();
    test_config_attributes();

    if (with_helper) {
        printf("\nEncode through the helper:\n");
        test_encode_one_frame(VAProfileH264High, "H.264 High");
        test_encode_one_frame(VAProfileHEVCMain, "HEVC Main");
    }

    printf("\n=== Results: %d passed, %d failed ===\n\n", pass_count, fail_count);

    vaTerminate(dpy);
    close(drm_fd);
    return fail_count > 0 ? 1 : 0;
}

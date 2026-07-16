
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

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

static void test_av1_decode_init(void)
{
    TEST_START("AV1 decoding initialization (8-bit)");
    VAConfigID config_id;
    VAConfigAttrib rt_attr = { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 };
    VAStatus st = vaCreateConfig(dpy, VAProfileAV1Profile0, VAEntrypointVLD, &rt_attr, 1, &config_id);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateConfig failed");

    VAContextID context_id;
    st = vaCreateContext(dpy, config_id, 1920, 1080, VA_PROGRESSIVE, NULL, 0, &context_id);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateContext failed");

    VASurfaceID surfaces[1];
    st = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 1920, 1080, surfaces, 1, NULL, 0);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateSurfaces failed");

    vaDestroySurfaces(dpy, surfaces, 1);
    vaDestroyContext(dpy, context_id);
    vaDestroyConfig(dpy, config_id);
    TEST_PASS();
}

static void test_av1_decode_init_10bit(void)
{
    TEST_START("AV1 decoding initialization (10-bit)");
    VAConfigID config_id;
    VAConfigAttrib rt_attr = { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420_10 };
    VAStatus st = vaCreateConfig(dpy, VAProfileAV1Profile0, VAEntrypointVLD, &rt_attr, 1, &config_id);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateConfig failed");

    VAContextID context_id;
    st = vaCreateContext(dpy, config_id, 1920, 1080, VA_PROGRESSIVE, NULL, 0, &context_id);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateContext failed");

    VASurfaceID surfaces[1];
    st = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420_10, 1920, 1080, surfaces, 1, NULL, 0);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateSurfaces failed");

    vaDestroySurfaces(dpy, surfaces, 1);
    vaDestroyContext(dpy, context_id);
    vaDestroyConfig(dpy, config_id);
    TEST_PASS();
}

static void test_av1_decode_export(void)
{
    TEST_START("AV1 surface export (separate layers)");
    VAConfigID config_id;
    VAConfigAttrib rt_attr = { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 };
    vaCreateConfig(dpy, VAProfileAV1Profile0, VAEntrypointVLD, &rt_attr, 1, &config_id);

    VAContextID context_id;
    vaCreateContext(dpy, config_id, 1920, 1080, VA_PROGRESSIVE, NULL, 0, &context_id);

    VASurfaceID surfaces[1];
    vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 1920, 1080, surfaces, 1, NULL, 0);

    /* Chromium's zero-copy import path exports surfaces with separate layers
     * (one layer per plane). This is the layout the NVIDIA direct backend
     * produces and the mode we must support for browser decode. */
    VADRMPRIMESurfaceDescriptor desc;
    VAStatus st = vaExportSurfaceHandle(dpy, surfaces[0], VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                       VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaExportSurfaceHandle failed");

    TEST_ASSERT(desc.num_layers == 2, "Expected 2 layers for NV12 separate export");
    TEST_ASSERT(desc.layers[0].num_planes == 1, "Expected 1 plane per layer");
    TEST_ASSERT(desc.layers[1].num_planes == 1, "Expected 1 plane per layer");

    for (int i = 0; i < desc.num_objects; i++) close(desc.objects[i].fd);

    vaDestroySurfaces(dpy, surfaces, 1);
    vaDestroyContext(dpy, context_id);
    vaDestroyConfig(dpy, config_id);
    TEST_PASS();
}

int main()
{
    printf("\n=== nvidia-vaapi-driver decoding tests ===\n\n");
    setup();
    
    test_av1_decode_init();
    test_av1_decode_init_10bit();
    test_av1_decode_export();

    teardown();
    printf("\n=== Results: %d passed, %d failed ===\n\n", pass_count, fail_count);
    return (fail_count > 0);
}

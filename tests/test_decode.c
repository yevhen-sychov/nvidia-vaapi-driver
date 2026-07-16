#define _DEFAULT_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <drm_fourcc.h>

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

/* The driver advertises AV1 Profile0 decode RTFormat as the combined mask
 * VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10. A client that echoes that
 * queried mask back into vaCreateConfig (and does not pass render targets to
 * vaCreateContext) must still get a usable 8-bit decoder — otherwise the
 * decoder is silently mis-configured for 10-bit and decode fails, forcing a
 * software fallback in the browser. */
static void test_av1_decode_combined_rtformat(void)
{
    TEST_START("AV1 decode with advertised RTFormat mask (8-bit)");

    /* Query the advertised RTFormat, exactly as a browser does. */
    VAConfigAttrib q = { .type = VAConfigAttribRTFormat };
    VAStatus st = vaGetConfigAttributes(dpy, VAProfileAV1Profile0, VAEntrypointVLD, &q, 1);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaGetConfigAttributes failed");
    TEST_ASSERT(q.value & VA_RT_FORMAT_YUV420, "8-bit YUV420 not advertised for AV1 decode");

    /* Create the config with the full advertised mask. */
    VAConfigID config_id;
    st = vaCreateConfig(dpy, VAProfileAV1Profile0, VAEntrypointVLD, &q, 1, &config_id);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateConfig failed");

    /* 8-bit NV12 render targets, as a browser supplies for an 8-bit stream. */
    VASurfaceID surfaces[1];
    st = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 1920, 1080, surfaces, 1, NULL, 0);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateSurfaces (8-bit) failed");

    VAContextID context_id;
    st = vaCreateContext(dpy, config_id, 1920, 1080, VA_PROGRESSIVE, surfaces, 1, &context_id);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateContext failed");

    /* Exporting the surface must yield an 8-bit NV12 layout (2 planes, 1 byte
     * per luma sample). If the combined-mask config forced a 10-bit (P010)
     * decoder, the exported DRM format would be 16-bit (R16/GR1616). */
    VADRMPRIMESurfaceDescriptor desc;
    st = vaExportSurfaceHandle(dpy, surfaces[0], VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                               VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaExportSurfaceHandle failed");
    TEST_ASSERT(desc.fourcc == VA_FOURCC_NV12,
                "surface is not 8-bit NV12 (decoder mis-configured for 10-bit)");
    for (int i = 0; i < desc.num_objects; i++) close(desc.objects[i].fd);

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

/* NVD_DESCRIPTOR_MODE=combined should export YUV surfaces as a single layer
 * carrying the combined DRM fourcc (e.g. NV12) with multiple planes, instead
 * of one split single-channel layer per plane (R8 + GR88). This is the
 * workaround for EGL/ANGLE DMA-BUF importers that reject the split layout
 * with EGL_BAD_MATCH. The mode is read once per driver instance, so this
 * test uses its own display separate from the shared `dpy` used above. */
static void test_av1_decode_combined_descriptor_mode(void)
{
    TEST_START("AV1 decode export with NVD_DESCRIPTOR_MODE=combined");

    setenv("NVD_DESCRIPTOR_MODE", "combined", 1);
    int local_drm_fd = open(DRM_DEVICE, O_RDWR);
    TEST_ASSERT(local_drm_fd >= 0, "Cannot open DRM device");
    VADisplay local_dpy = vaGetDisplayDRM(local_drm_fd);
    TEST_ASSERT(local_dpy != NULL, "vaGetDisplayDRM failed");
    int major, minor;
    VAStatus st = vaInitialize(local_dpy, &major, &minor);
    unsetenv("NVD_DESCRIPTOR_MODE");
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaInitialize failed");

    VAConfigID config_id;
    VAConfigAttrib rt_attr = { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 };
    st = vaCreateConfig(local_dpy, VAProfileAV1Profile0, VAEntrypointVLD, &rt_attr, 1, &config_id);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateConfig failed");

    VASurfaceID surfaces[1];
    st = vaCreateSurfaces(local_dpy, VA_RT_FORMAT_YUV420, 1920, 1080, surfaces, 1, NULL, 0);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateSurfaces failed");

    VAContextID context_id;
    st = vaCreateContext(local_dpy, config_id, 1920, 1080, VA_PROGRESSIVE, surfaces, 1, &context_id);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateContext failed");

    VADRMPRIMESurfaceDescriptor desc;
    st = vaExportSurfaceHandle(local_dpy, surfaces[0], VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                               VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaExportSurfaceHandle failed");

    TEST_ASSERT(desc.num_objects == 1, "combined mode should export a single DMA-BUF object");
    TEST_ASSERT(desc.num_layers == 1, "combined mode should export a single combined layer");
    TEST_ASSERT(desc.layers[0].drm_format == DRM_FORMAT_NV12,
                "combined layer should carry the combined NV12 fourcc, not a split plane format");
    TEST_ASSERT(desc.layers[0].num_planes == 2, "combined NV12 layer should carry both planes");

    for (int i = 0; i < desc.num_objects; i++) close(desc.objects[i].fd);

    vaDestroySurfaces(local_dpy, surfaces, 1);
    vaDestroyContext(local_dpy, context_id);
    vaDestroyConfig(local_dpy, config_id);
    vaTerminate(local_dpy);
    close(local_drm_fd);
    TEST_PASS();
}

/* AUTO mode (the default) picks the export layout per-surface based on
 * context type: encode-context surfaces get the combined layer, decode-
 * context surfaces get the split layer — except a decode surface created
 * at exactly the same resolution as a currently-active local encode
 * context, which is treated as a self-preview (Chrome re-decoding its own
 * outgoing stream for a thumbnail) and also gets the combined layer. A
 * decode surface at any other resolution is genuine remote video and must
 * keep the split layout the browser's normal decode-display importer
 * needs. */
static void test_decode_auto_mode_self_preview_combined(void)
{
    TEST_START("AUTO mode: decode surface matching active encode res gets combined layer (self-preview)");

    VAConfigAttrib enc_attr = { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 };
    VAConfigID enc_config;
    VAStatus st = vaCreateConfig(dpy, VAProfileH264High, VAEntrypointEncSlice, &enc_attr, 1, &enc_config);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateConfig (encode) failed");

    VASurfaceID enc_surface;
    st = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 640, 480, &enc_surface, 1, NULL, 0);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateSurfaces (encode) failed");

    VAContextID enc_context;
    st = vaCreateContext(dpy, enc_config, 640, 480, VA_PROGRESSIVE, &enc_surface, 1, &enc_context);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateContext (encode) failed");

    /* vaBeginPicture() associates the surface with the (encode) context,
     * matching how a real browser session drives it. The encode context
     * is kept alive (not destroyed) for the rest of this test to simulate
     * an active local capture/preview session. */
    st = vaBeginPicture(dpy, enc_context, enc_surface);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaBeginPicture (encode) failed");

    VAConfigAttrib dec_attr = { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 };
    VAConfigID dec_config;
    st = vaCreateConfig(dpy, VAProfileAV1Profile0, VAEntrypointVLD, &dec_attr, 1, &dec_config);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateConfig (decode) failed");

    /* Decode surface at the SAME resolution as the active encode context
     * above — this is the self-preview case and should get the combined
     * (single-layer) export, just like an encode surface would. */
    VASurfaceID self_preview_surface;
    st = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 640, 480, &self_preview_surface, 1, NULL, 0);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateSurfaces (self-preview decode) failed");

    VAContextID self_preview_context;
    st = vaCreateContext(dpy, dec_config, 640, 480, VA_PROGRESSIVE, &self_preview_surface, 1, &self_preview_context);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateContext (self-preview decode) failed");

    VADRMPRIMESurfaceDescriptor desc;
    st = vaExportSurfaceHandle(dpy, self_preview_surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                               VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaExportSurfaceHandle (self-preview) failed");
    TEST_ASSERT(desc.num_layers == 1,
                "decode surface matching an active encode context's resolution should use the combined layer");
    for (int i = 0; i < desc.num_objects; i++) close(desc.objects[i].fd);

    /* Decode surface at a DIFFERENT resolution — genuine remote video —
     * must keep the split per-plane layout. */
    VASurfaceID remote_surface;
    st = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 1280, 720, &remote_surface, 1, NULL, 0);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateSurfaces (remote decode) failed");

    VAContextID remote_context;
    st = vaCreateContext(dpy, dec_config, 1280, 720, VA_PROGRESSIVE, &remote_surface, 1, &remote_context);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaCreateContext (remote decode) failed");

    VADRMPRIMESurfaceDescriptor remote_desc;
    st = vaExportSurfaceHandle(dpy, remote_surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                               VA_EXPORT_SURFACE_SEPARATE_LAYERS, &remote_desc);
    TEST_ASSERT(st == VA_STATUS_SUCCESS, "vaExportSurfaceHandle (remote) failed");
    TEST_ASSERT(remote_desc.num_layers == 2,
                "decode surface at a resolution not matching any active encode context must keep the split layout");
    for (int i = 0; i < remote_desc.num_objects; i++) close(remote_desc.objects[i].fd);

    vaDestroySurfaces(dpy, &remote_surface, 1);
    vaDestroyContext(dpy, remote_context);
    vaDestroySurfaces(dpy, &self_preview_surface, 1);
    vaDestroyContext(dpy, self_preview_context);
    vaDestroyConfig(dpy, dec_config);
    vaDestroyContext(dpy, enc_context);
    vaDestroySurfaces(dpy, &enc_surface, 1);
    vaDestroyConfig(dpy, enc_config);

    TEST_PASS();
}

int main()
{
    printf("\n=== nvidia-vaapi-driver decoding tests ===\n\n");
    setup();
    
    test_av1_decode_init();
    test_av1_decode_init_10bit();
    test_av1_decode_combined_rtformat();
    test_av1_decode_export();
    test_av1_decode_combined_descriptor_mode();
    test_decode_auto_mode_self_preview_combined();

    teardown();
    printf("\n=== Results: %d passed, %d failed ===\n\n", pass_count, fail_count);
    return (fail_count > 0);
}

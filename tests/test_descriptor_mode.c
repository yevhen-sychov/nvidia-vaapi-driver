/*
 * test_descriptor_mode.c — Regression tests for the AUTO descriptor-mode
 * heuristic.
 *
 * Background: the AUTO descriptor mode picks the DMA-BUF export layout on a
 * per-surface basis. Encode-context surfaces (local capture/preview, imported
 * by Chrome's WebGL/canvas worker path) get COMBINED; decode-context surfaces
 * (remote peer video, imported by Chrome's normal video-frame zero-copy path)
 * get SINGLE. Historically, AUTO also treated a decode surface whose
 * resolution matched an active encode context as a "self-preview" and gave it
 * COMBINED — that misfired on every real WebRTC call (peers negotiate to your
 * own resolution) and rendered remote peers with green macroblock corruption.
 *
 * These tests pin the current behavior:
 *   1. A pure decode surface always exports as SINGLE (num_layers == numPlanes).
 *   2. Even with a concurrent encode context at the same resolution, a decode
 *      surface still exports as SINGLE — the resolution-match heuristic is
 *      off by default in AUTO.
 */

#include "test_common.h"
#include <va/va_drmcommon.h>
#include <drm_fourcc.h>

static void test_decode_only_exports_single(void) {
    TEST_START("Decode-only surface → SINGLE layout (2 layers for NV12)");
    VASurfaceID surface_id;
    VAStatus st = vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420, 1280, 720,
                                    &surface_id, 1, NULL, 0);
    EXPECT_STATUS(st);

    VADRMPRIMESurfaceDescriptor desc;
    st = vaExportSurfaceHandle(g_dpy, surface_id,
                               VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                               VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc);
    EXPECT_STATUS(st);

    EXPECT_TRUE(desc.num_layers == 2,
                "expected 2 split layers (SINGLE), got combined");
    EXPECT_TRUE(desc.layers[0].drm_format == DRM_FORMAT_R8,
                "layer 0 not R8 → not split-layer NV12");
    EXPECT_TRUE(desc.layers[1].drm_format == DRM_FORMAT_RG88,
                "layer 1 not RG88 → not split-layer NV12");

    for (int i = 0; i < (int)desc.num_objects; i++)
        close(desc.objects[i].fd);
    vaDestroySurfaces(g_dpy, &surface_id, 1);
    TEST_PASS();
}

/* The regression case that produced green macroblocks on remote peers:
 * an encode context is live at the same resolution as a decode surface.
 * Older AUTO code returned a combined layer for the decode surface here,
 * which Chrome's normal decode-display importer rendered as green MB
 * corruption. New AUTO ignores resolution and returns SINGLE. */
static void test_decode_with_active_encode_at_same_res(void) {
    TEST_START("Decode surface + concurrent encode @ same 1280x720 → SINGLE");

    /* This test pins the DEFAULT AUTO behavior — when the legacy opt-in is
     * active, the opposite behavior is expected and covered by
     * test_opt_in_self_preview_combined() instead. Skip here to avoid a
     * spurious failure in the opt-in test-suite pass. */
    const char *opt_in = getenv("NVD_SELF_PREVIEW_COMBINED");
    if (opt_in != NULL && strcmp(opt_in, "1") == 0) {
        TEST_SKIP("NVD_SELF_PREVIEW_COMBINED=1 covered by opt-in test");
        return;
    }

    /* Stand up a live H.264 encode context at 1280x720 (matches Chrome's
     * default WebRTC HD camera constraint). If encode is unsupported, skip. */
    if (!test_has_entrypoint(g_dpy, VAProfileH264High, VAEntrypointEncSlice)) {
        TEST_SKIP("H264 encode entrypoint unavailable");
        return;
    }

    VAConfigAttrib attr = { .type = VAConfigAttribRTFormat,
                            .value = VA_RT_FORMAT_YUV420 };
    VAConfigID enc_config;
    VAStatus st = vaCreateConfig(g_dpy, VAProfileH264High, VAEntrypointEncSlice,
                                  &attr, 1, &enc_config);
    EXPECT_STATUS(st);

    VASurfaceID enc_surface;
    st = vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420, 1280, 720,
                          &enc_surface, 1, NULL, 0);
    EXPECT_STATUS(st);

    VAContextID enc_context;
    st = vaCreateContext(g_dpy, enc_config, 1280, 720, VA_PROGRESSIVE,
                         &enc_surface, 1, &enc_context);
    EXPECT_STATUS(st);

    /* Now the decode-side surface at the same 1280x720. This is what a remote
     * peer's incoming H.264 track would decode into. */
    VASurfaceID dec_surface;
    st = vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420, 1280, 720,
                          &dec_surface, 1, NULL, 0);
    EXPECT_STATUS(st);

    VADRMPRIMESurfaceDescriptor desc;
    st = vaExportSurfaceHandle(g_dpy, dec_surface,
                               VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                               VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc);
    EXPECT_STATUS(st);

    /* The regression signal: an old AUTO with the resolution-match heuristic
     * would return num_layers == 1 here (combined). We want SINGLE (== 2). */
    if (desc.num_layers != 2) {
        TEST_FAIL("decode surface exported as COMBINED — resolution-match "
                  "heuristic still active (this causes green MBs on peers)");
        for (int i = 0; i < (int)desc.num_objects; i++)
            close(desc.objects[i].fd);
        vaDestroySurfaces(g_dpy, &dec_surface, 1);
        vaDestroyContext(g_dpy, enc_context);
        vaDestroySurfaces(g_dpy, &enc_surface, 1);
        vaDestroyConfig(g_dpy, enc_config);
        return;
    }
    EXPECT_TRUE(desc.layers[0].drm_format == DRM_FORMAT_R8,
                "layer 0 not R8 after decode-export");
    EXPECT_TRUE(desc.layers[1].drm_format == DRM_FORMAT_RG88,
                "layer 1 not RG88 after decode-export");

    for (int i = 0; i < (int)desc.num_objects; i++)
        close(desc.objects[i].fd);
    vaDestroySurfaces(g_dpy, &dec_surface, 1);
    vaDestroyContext(g_dpy, enc_context);
    vaDestroySurfaces(g_dpy, &enc_surface, 1);
    vaDestroyConfig(g_dpy, enc_config);
    TEST_PASS();
}

/* Opt-in escape hatch for the small set of users who rely on Chrome's
 * decode-back self-preview importer. When NVD_SELF_PREVIEW_COMBINED=1 is set,
 * the resolution-match heuristic is re-enabled and the decode surface goes
 * back to COMBINED when an encode context of the same size exists. */
static void test_opt_in_self_preview_combined(void) {
    TEST_START("NVD_SELF_PREVIEW_COMBINED=1 → decode+match falls back to COMBINED");

    const char *env = getenv("NVD_SELF_PREVIEW_COMBINED");
    if (env == NULL || strcmp(env, "1") != 0) {
        TEST_SKIP("NVD_SELF_PREVIEW_COMBINED not set — run with =1 to cover");
        return;
    }

    if (!test_has_entrypoint(g_dpy, VAProfileH264High, VAEntrypointEncSlice)) {
        TEST_SKIP("H264 encode entrypoint unavailable");
        return;
    }

    VAConfigAttrib attr = { .type = VAConfigAttribRTFormat,
                            .value = VA_RT_FORMAT_YUV420 };
    VAConfigID enc_config;
    EXPECT_STATUS(vaCreateConfig(g_dpy, VAProfileH264High, VAEntrypointEncSlice,
                                  &attr, 1, &enc_config));

    VASurfaceID enc_surface;
    EXPECT_STATUS(vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420, 640, 480,
                                    &enc_surface, 1, NULL, 0));

    VAContextID enc_context;
    EXPECT_STATUS(vaCreateContext(g_dpy, enc_config, 640, 480, VA_PROGRESSIVE,
                                    &enc_surface, 1, &enc_context));

    VASurfaceID dec_surface;
    EXPECT_STATUS(vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420, 640, 480,
                                    &dec_surface, 1, NULL, 0));

    VADRMPRIMESurfaceDescriptor desc;
    EXPECT_STATUS(vaExportSurfaceHandle(g_dpy, dec_surface,
                                          VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                          VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                                          &desc));

    EXPECT_TRUE(desc.num_layers == 1,
                "opt-in should force COMBINED (num_layers == 1) for decode "
                "surface matching a live encode resolution");

    for (int i = 0; i < (int)desc.num_objects; i++)
        close(desc.objects[i].fd);
    vaDestroySurfaces(g_dpy, &dec_surface, 1);
    vaDestroyContext(g_dpy, enc_context);
    vaDestroySurfaces(g_dpy, &enc_surface, 1);
    vaDestroyConfig(g_dpy, enc_config);
    TEST_PASS();
}

int main(void) {
    test_global_setup();

    printf("\n=== nvidia-vaapi-driver descriptor-mode tests ===\n");
    printf("Driver: %s\n\n", vaQueryVendorString(g_dpy));

    printf("AUTO default:\n");
    test_decode_only_exports_single();
    test_decode_with_active_encode_at_same_res();

    printf("\nOpt-in fallback:\n");
    test_opt_in_self_preview_combined();

    test_print_summary("Descriptor-mode tests");
    test_global_teardown();
    return g_fail > 0 ? 1 : 0;
}

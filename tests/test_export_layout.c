/*
 * test_export_layout.c — DMA-BUF export layout of YUV surfaces.
 *
 * Multi-plane YUV surfaces export one split single-channel layer per plane
 * (R8 + RG88 for NV12), each plane in its own dma-buf object by default, or
 * all planes in one shared object under NVD_SINGLE_BUFFER=1. The layout no
 * longer depends on whether the surface belongs to a decode or an encode
 * context.
 *
 * These tests pin:
 *   1. A decode surface exports as split layers (num_layers == numPlanes).
 *   2. A live encode context at the same resolution does not change that.
 *      (An old heuristic exported such surfaces as one combined NV12 layer,
 *      which rendered remote WebRTC peers with green macroblocks.)
 *   3. Chromium's uniform-modifier contract, under both layouts.
 */

#include "test_common.h"
#include <va/va_drmcommon.h>
#include <drm_fourcc.h>

static void test_decode_only_exports_split_layers(void) {
    TEST_START("Decode-only surface → split layers (2 layers for NV12)");
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
                "expected 2 split layers, got combined");
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
 * An old heuristic returned a combined layer for the decode surface here,
 * which Chrome's normal decode-display importer rendered as green MB
 * corruption. */
static void test_decode_with_active_encode_at_same_res(void) {
    TEST_START("Decode surface + concurrent encode @ same 1280x720 → split layers");

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

    /* The regression signal: the old resolution-match heuristic returned
     * num_layers == 1 here (combined). We want split layers (== 2). */
    if (desc.num_layers != 2) {
        TEST_FAIL("decode surface exported as one combined layer — this causes "
                  "green MBs on peers");
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

/*
 * Chromium's DMA-BUF import contract (media/gpu/vaapi/vaapi_video_decoder.cc,
 * ExportVASurfaceAsNativePixmapDmaBufUnwrapped):
 *
 *     handle.modifier = descriptor.objects[0].drm_format_modifier;
 *     for (each object) CHECK_EQ(handle.modifier, object.drm_format_modifier);
 *
 * That is a CHECK, i.e. a GPU-process abort — not an error path Chromium can
 * recover from.
 *
 * On NVIDIA the block-linear modifier encodes the GOB block height, which is
 * chosen from the *plane* height. The chroma plane of a 4:2:0 surface is half
 * height, so below a threshold it lands in a smaller block-height bucket than
 * luma and the two planes get genuinely different modifiers. Measured on this
 * driver the two converge at exactly height 172 (width-independent, identical
 * for NV12 and P010).
 *
 * Below that threshold there is no descriptor we can emit that is both correct
 * and Chromium-safe:
 *   - one object per plane (the default) reports the true per-plane
 *     modifiers => Chromium CHECK_EQ aborts.
 *   - one shared object (NVD_SINGLE_BUFFER=1) holds both planes and carries
 *     exactly one modifier => the chroma plane is described with luma's
 *     tiling => silently wrong pixels (green macroblocks) rather than a crash.
 *
 * So the driver advertises a minimum surface height at or above the threshold
 * and clients simply never allocate into the divergent regime. These tests pin
 * both halves of that contract.
 */
#define MODIFIER_CONVERGENCE_HEIGHT 172

static void test_advertised_min_height_avoids_modifier_split(void) {
    TEST_START("Decode MinHeight >= modifier convergence height");

    if (!test_has_entrypoint(g_dpy, VAProfileH264High, VAEntrypointVLD)) {
        TEST_SKIP("H264 decode entrypoint unavailable");
        return;
    }

    VAConfigAttrib attr = { .type = VAConfigAttribRTFormat,
                            .value = VA_RT_FORMAT_YUV420 };
    VAConfigID config;
    EXPECT_STATUS(vaCreateConfig(g_dpy, VAProfileH264High, VAEntrypointVLD,
                                 &attr, 1, &config));

    unsigned int num_attribs = 0;
    VAStatus st = vaQuerySurfaceAttributes(g_dpy, config, NULL, &num_attribs);
    if (st != VA_STATUS_SUCCESS || num_attribs == 0) {
        vaDestroyConfig(g_dpy, config);
        TEST_FAIL("vaQuerySurfaceAttributes returned no attributes");
        return;
    }

    VASurfaceAttrib *attribs = calloc(num_attribs, sizeof(*attribs));
    st = vaQuerySurfaceAttributes(g_dpy, config, attribs, &num_attribs);
    if (st != VA_STATUS_SUCCESS) {
        free(attribs);
        vaDestroyConfig(g_dpy, config);
        EXPECT_STATUS(st);
        return;
    }

    int min_height = -1;
    for (unsigned int i = 0; i < num_attribs; i++) {
        if (attribs[i].type == VASurfaceAttribMinHeight)
            min_height = attribs[i].value.value.i;
    }
    free(attribs);
    vaDestroyConfig(g_dpy, config);

    if (min_height < 0) {
        TEST_FAIL("driver did not report VASurfaceAttribMinHeight");
        return;
    }
    if (min_height < MODIFIER_CONVERGENCE_HEIGHT) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "MinHeight=%d < %d — clients can allocate surfaces whose luma "
                 "and chroma planes have different block-linear modifiers",
                 min_height, MODIFIER_CONVERGENCE_HEIGHT);
        TEST_FAIL(msg);
        return;
    }
    TEST_PASS();
}

/* Sweep a range of surface sizes at and above the advertised minimum and assert
 * every exported descriptor satisfies Chromium's uniform-modifier CHECK. Runs
 * under whatever layout the suite was invoked with, so the meson
 * `export_layout_single_buffer` variant covers NVD_SINGLE_BUFFER=1 too. */
static void test_exported_modifiers_are_uniform(void) {
    TEST_START("Exported objects share one drm_format_modifier");

    static const struct { int w, h; } sizes[] = {
        { 320, 176 }, { 352, 288 }, { 640, 360 }, { 640, 480 },
        { 848, 480 }, { 1280, 720 }, { 1920, 1080 }, { 3840, 2160 },
    };

    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        VASurfaceID surface_id;
        VAStatus st = vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420,
                                       sizes[i].w, sizes[i].h,
                                       &surface_id, 1, NULL, 0);
        if (st != VA_STATUS_SUCCESS)
            continue; /* size unsupported on this GPU — not what we're testing */

        VADRMPRIMESurfaceDescriptor desc;
        memset(&desc, 0, sizeof(desc));
        st = vaExportSurfaceHandle(g_dpy, surface_id,
                                   VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                   VA_EXPORT_SURFACE_READ_ONLY |
                                       VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                                   &desc);
        if (st != VA_STATUS_SUCCESS) {
            vaDestroySurfaces(g_dpy, &surface_id, 1);
            EXPECT_STATUS(st);
            return;
        }

        for (unsigned o = 1; o < desc.num_objects; o++) {
            if (desc.objects[o].drm_format_modifier !=
                desc.objects[0].drm_format_modifier) {
                char msg[192];
                snprintf(msg, sizeof(msg),
                         "%dx%d: object[0] mod 0x%llx != object[%u] mod 0x%llx "
                         "— Chromium CHECK_EQ would abort the GPU process",
                         sizes[i].w, sizes[i].h,
                         (unsigned long long) desc.objects[0].drm_format_modifier,
                         o,
                         (unsigned long long) desc.objects[o].drm_format_modifier);
                for (unsigned c = 0; c < desc.num_objects; c++)
                    close(desc.objects[c].fd);
                vaDestroySurfaces(g_dpy, &surface_id, 1);
                TEST_FAIL(msg);
                return;
            }
        }

        for (unsigned o = 0; o < desc.num_objects; o++)
            close(desc.objects[o].fd);
        vaDestroySurfaces(g_dpy, &surface_id, 1);
    }
    TEST_PASS();
}

int main(void) {
    test_global_setup();

    printf("\n=== nvidia-vaapi-driver export layout tests ===\n");
    printf("Driver: %s\n\n", vaQueryVendorString(g_dpy));

    printf("Layer layout:\n");
    test_decode_only_exports_split_layers();
    test_decode_with_active_encode_at_same_res();

    printf("\nChromium import contract:\n");
    test_advertised_min_height_avoids_modifier_split();
    test_exported_modifiers_are_uniform();

    test_print_summary("Export layout tests");
    test_global_teardown();
    return g_fail > 0 ? 1 : 0;
}

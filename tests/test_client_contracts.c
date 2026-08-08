/*
 * test_client_contracts.c — Pins the low-level VA-API behaviours that Chromium,
 * Firefox and FFmpeg depend on.
 *
 * These are not features; they are contracts. Each one is small, easy to break
 * accidentally while refactoring, and produces a confusing downstream failure
 * (silent software fallback, a wrong-looking error, or a browser-side abort)
 * rather than an obvious one. Chromium binds only 44 libva symbols and Firefox
 * only two, so what those clients actually rely on is a narrow and very
 * specific surface — that is what is checked here.
 */

#include "test_common.h"
#include <va/va_drmcommon.h>

/* An attribute this driver does not implement for a decode config. The VA-API
 * contract is that the driver writes VA_ATTRIB_NOT_SUPPORTED; leaving the
 * caller's value untouched makes a client that pre-fills the array read its own
 * request back and conclude the attribute is supported. Chromium happens to
 * zero the array first (vaapi_wrapper.cc, AreAttribsSupported) so it is immune,
 * but nothing in the spec requires a client to do that. */
static void test_unhandled_config_attrib_is_marked_unsupported(void) {
    TEST_START("vaGetConfigAttributes marks unknown attribs unsupported");

    if (!test_has_entrypoint(g_dpy, VAProfileH264High, VAEntrypointVLD)) {
        TEST_SKIP("H264 decode entrypoint unavailable");
        return;
    }

    /* Deliberately pre-fill with a bogus non-zero value: if the driver ignores
     * the attribute we will read this back unchanged. */
    VAConfigAttrib attribs[2] = {
        { .type = VAConfigAttribRTFormat, .value = 0xdeadbeef },
        { .type = VAConfigAttribEncROI,   .value = 0xdeadbeef },
    };
    EXPECT_STATUS(vaGetConfigAttributes(g_dpy, VAProfileH264High,
                                        VAEntrypointVLD, attribs, 2));

    EXPECT_TRUE(attribs[0].value != 0xdeadbeef,
                "RTFormat left untouched — driver did not answer it at all");
    EXPECT_TRUE(attribs[1].value == VA_ATTRIB_NOT_SUPPORTED,
                "unhandled attribute did not come back as VA_ATTRIB_NOT_SUPPORTED");
    TEST_PASS();
}

/* Same contract on the VideoProc path, which Chromium reaches through
 * VaapiImageProcessorBackend. */
static void test_unhandled_vpp_config_attrib_is_marked_unsupported(void) {
    TEST_START("vaGetConfigAttributes (VPP) marks unknown attribs unsupported");

    if (!test_has_entrypoint(g_dpy, VAProfileNone, VAEntrypointVideoProc)) {
        TEST_SKIP("VideoProc entrypoint unavailable");
        return;
    }

    VAConfigAttrib attribs[1] = {
        { .type = VAConfigAttribEncROI, .value = 0xdeadbeef },
    };
    EXPECT_STATUS(vaGetConfigAttributes(g_dpy, VAProfileNone,
                                        VAEntrypointVideoProc, attribs, 1));
    EXPECT_TRUE(attribs[0].value == VA_ATTRIB_NOT_SUPPORTED,
                "unhandled VPP attribute did not come back as VA_ATTRIB_NOT_SUPPORTED");
    TEST_PASS();
}

/* Chromium probes every profile a second time with zero attributes
 * (FillSupportedProfileInfos) and drops the profile if that call fails.
 * FFmpeg's ff_vaapi_decode_make_config does the same, and so does the
 * surface-alignment probe on libva >= 1.21. */
static void test_create_config_with_no_attributes(void) {
    TEST_START("vaCreateConfig succeeds with zero attributes");

    if (!test_has_entrypoint(g_dpy, VAProfileH264High, VAEntrypointVLD)) {
        TEST_SKIP("H264 decode entrypoint unavailable");
        return;
    }

    VAConfigID config;
    EXPECT_STATUS(vaCreateConfig(g_dpy, VAProfileH264High, VAEntrypointVLD,
                                 NULL, 0, &config));
    vaDestroyConfig(g_dpy, config);
    TEST_PASS();
}

/* vaDeriveImage must fail with exactly VA_STATUS_ERROR_OPERATION_FAILED.
 *
 * Chromium's UploadVideoFrameToSurface tries vaDeriveImage first and only falls
 * back to vaCreateImage + vaPutImage when it sees that specific status — any
 * other error is treated as fatal and kills hardware encode. FFmpeg's
 * vaapi_frames_init also probes vaDeriveImage on a scratch surface at every
 * decoder init and needs the failure to be clean. */
static void test_derive_image_fails_with_operation_failed(void) {
    TEST_START("vaDeriveImage fails with OPERATION_FAILED (not another error)");

    VASurfaceID surface_id;
    EXPECT_STATUS(vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420, 640, 480,
                                   &surface_id, 1, NULL, 0));

    VAImage image;
    VAStatus st = vaDeriveImage(g_dpy, surface_id, &image);
    if (st == VA_STATUS_SUCCESS) {
        /* If a future change makes derive work, that is fine for Chromium —
         * but the image must then be NV12, which is all it accepts. */
        uint32_t fourcc = image.format.fourcc;
        vaDestroyImage(g_dpy, image.image_id);
        vaDestroySurfaces(g_dpy, &surface_id, 1);
        EXPECT_TRUE(fourcc == VA_FOURCC_NV12,
                    "vaDeriveImage succeeded but not as NV12 — Chromium rejects it");
        TEST_PASS();
        return;
    }
    vaDestroySurfaces(g_dpy, &surface_id, 1);

    if (st != VA_STATUS_ERROR_OPERATION_FAILED) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "got status %d, Chromium only falls back on %d",
                 st, VA_STATUS_ERROR_OPERATION_FAILED);
        TEST_FAIL(msg);
        return;
    }
    TEST_PASS();
}

/* Firefox exports a surface and only *then* calls vaSyncSurface, and it ignores
 * the sync result (FFmpegVideoDecoder.cpp). So vaExportSurfaceHandle has to
 * return a fully resolved surface on its own — any driver logic that finalizes
 * a frame inside vaSyncSurface would race. */
static void test_export_without_prior_sync(void) {
    TEST_START("vaExportSurfaceHandle resolves without a prior vaSyncSurface");

    VASurfaceID surface_id;
    EXPECT_STATUS(vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420, 1280, 720,
                                   &surface_id, 1, NULL, 0));

    VADRMPRIMESurfaceDescriptor desc;
    memset(&desc, 0, sizeof(desc));
    VAStatus st = vaExportSurfaceHandle(g_dpy, surface_id,
                                        VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                        VA_EXPORT_SURFACE_READ_ONLY |
                                            VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                                        &desc);
    if (st != VA_STATUS_SUCCESS) {
        vaDestroySurfaces(g_dpy, &surface_id, 1);
        EXPECT_STATUS(st);
        return;
    }

    /* Sync afterwards, as Firefox does — it must not fail. */
    VAStatus sync_st = vaSyncSurface(g_dpy, surface_id);

    for (unsigned o = 0; o < desc.num_objects; o++)
        close(desc.objects[o].fd);
    vaDestroySurfaces(g_dpy, &surface_id, 1);

    EXPECT_TRUE(desc.num_objects > 0, "export produced no DMA-BUF objects");
    EXPECT_STATUS(sync_st);
    TEST_PASS();
}

/* Firefox's DMABufSurfaceYUV::ImportPRIMESurfaceDescriptor only understands
 * NV12, YV12, P010 and P016, and reads layers[i] as one plane each. Anything
 * else is silently mis-imported rather than rejected. */
static void test_exported_fourcc_is_importable(void) {
    TEST_START("Exported descriptor fourcc is one Firefox can import");

    static const struct { unsigned rt; const char *name; } formats[] = {
        { VA_RT_FORMAT_YUV420,    "YUV420" },
        { VA_RT_FORMAT_YUV420_10, "YUV420_10" },
        { VA_RT_FORMAT_YUV420_12, "YUV420_12" },
    };

    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        VASurfaceID surface_id;
        if (vaCreateSurfaces(g_dpy, formats[i].rt, 1280, 720,
                             &surface_id, 1, NULL, 0) != VA_STATUS_SUCCESS)
            continue; /* format unsupported on this GPU */

        VADRMPRIMESurfaceDescriptor desc;
        memset(&desc, 0, sizeof(desc));
        VAStatus st = vaExportSurfaceHandle(g_dpy, surface_id,
                                            VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                            VA_EXPORT_SURFACE_READ_ONLY |
                                                VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                                            &desc);
        if (st != VA_STATUS_SUCCESS) {
            vaDestroySurfaces(g_dpy, &surface_id, 1);
            continue;
        }

        const uint32_t fourcc = desc.fourcc;
        const uint32_t num_layers = desc.num_layers;
        const uint32_t num_objects = desc.num_objects;
        for (unsigned o = 0; o < desc.num_objects; o++)
            close(desc.objects[o].fd);
        vaDestroySurfaces(g_dpy, &surface_id, 1);

        if (fourcc != VA_FOURCC_NV12 && fourcc != VA_FOURCC_P010 &&
            fourcc != VA_FOURCC_P016 && fourcc != VA_FOURCC_YV12) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "%s exported as fourcc '%.4s' — Firefox only imports "
                     "NV12/YV12/P010/P016",
                     formats[i].name, (const char *) &fourcc);
            TEST_FAIL(msg);
            return;
        }
        if (num_layers > 4 || num_objects > 4) {
            TEST_FAIL("more than 4 layers/objects — beyond Firefox's import limit");
            return;
        }
    }
    TEST_PASS();
}

int main(void) {
    test_global_setup();

    printf("\n=== nvidia-vaapi-driver client-contract tests ===\n");
    printf("Driver: %s\n\n", vaQueryVendorString(g_dpy));

    printf("Config attribute contract:\n");
    test_unhandled_config_attrib_is_marked_unsupported();
    test_unhandled_vpp_config_attrib_is_marked_unsupported();
    test_create_config_with_no_attributes();

    printf("\nChromium encode-upload contract:\n");
    test_derive_image_fails_with_operation_failed();

    printf("\nFirefox import contract:\n");
    test_export_without_prior_sync();
    test_exported_fourcc_is_importable();

    test_print_summary("Client-contract tests");
    test_global_teardown();
    return g_fail > 0 ? 1 : 0;
}

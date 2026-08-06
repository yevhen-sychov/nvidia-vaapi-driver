/*
 * test_encode_config.c — Config and capability tests.
 * Tests profile/entrypoint validation, config attributes, and error paths.
 *
 * Build: gcc -o test_encode_config tests/test_encode_config.c -lva -lva-drm
 * Run:   ./test_encode_config
 */

#include "test_common.h"
#include <va/va_drmcommon.h>
#include <drm_fourcc.h>

/* --- Profile/Entrypoint matrix --- */

typedef struct {
    VAProfile profile;
    const char *name;
    bool expect_encode;
    bool expect_decode;
} ProfileTest;

static const ProfileTest profile_tests[] = {
    { VAProfileH264ConstrainedBaseline, "H264 CB",   true,  true  },
    { VAProfileH264Main,               "H264 Main", true,  true  },
    { VAProfileH264High,               "H264 High", true,  true  },
    { VAProfileHEVCMain,               "HEVC Main", true,  true  },
    { VAProfileHEVCMain10,             "HEVC M10",  true,  true  },
    { VAProfileVP8Version0_3,          "VP8",       false, true  },
    { VAProfileMPEG2Simple,            "MPEG2",     false, true  },
    { VAProfileVP9Profile0,            "VP9 P0",    false, false }, /* VP9 requires gstreamer-codecparsers */
    { VAProfileAV1Profile0,            "AV1 P0",    true,  true  },
    { VAProfileJPEGBaseline,           "JPEG",      false, true  },
};
#define NUM_PROFILE_TESTS (sizeof(profile_tests) / sizeof(profile_tests[0]))

static void test_encode_entrypoints(void) {
    for (int i = 0; i < (int)NUM_PROFILE_TESTS; i++) {
        char name[64];
        snprintf(name, sizeof(name), "EncSlice for %-10s → %s",
                 profile_tests[i].name,
                 profile_tests[i].expect_encode ? "present" : "absent");
        TEST_START(name);

        bool has = test_has_entrypoint(g_dpy, profile_tests[i].profile,
                                        VAEntrypointEncSlice);
        if (has == profile_tests[i].expect_encode) {
            TEST_PASS();
        } else {
            TEST_FAIL(has ? "unexpected EncSlice" : "missing EncSlice");
        }
    }
}

static void test_decode_entrypoints(void) {
    for (int i = 0; i < (int)NUM_PROFILE_TESTS; i++) {
        char name[64];
        snprintf(name, sizeof(name), "VLD for %-10s → %s",
                 profile_tests[i].name,
                 profile_tests[i].expect_decode ? "present" : "absent");
        TEST_START(name);

        bool has = test_has_entrypoint(g_dpy, profile_tests[i].profile,
                                        VAEntrypointVLD);
        if (has == profile_tests[i].expect_decode) {
            TEST_PASS();
        } else if (profile_tests[i].profile == VAProfileVP9Profile0 && has) {
            /* VP9 is present (optional dependency found) even though we didn't strictly expect it */
            TEST_PASS();
        } else {
            TEST_FAIL(has ? "unexpected VLD" : "missing VLD");
        }
    }
}

/* --- Config attribute validation --- */

static void test_config_rtformat(void) {
    TEST_START("H264 High RTFormat includes YUV420");
    VAConfigAttrib a = { .type = VAConfigAttribRTFormat };
    EXPECT_STATUS(vaGetConfigAttributes(g_dpy, VAProfileH264High,
                                         VAEntrypointEncSlice, &a, 1));
    EXPECT_TRUE(a.value & VA_RT_FORMAT_YUV420, "no YUV420");
    TEST_PASS();
}

static void test_hevc_main10_rtformat(void) {
    TEST_START("HEVC Main10 RTFormat includes YUV420_10");
    VAConfigAttrib a = { .type = VAConfigAttribRTFormat };
    EXPECT_STATUS(vaGetConfigAttributes(g_dpy, VAProfileHEVCMain10,
                                         VAEntrypointEncSlice, &a, 1));
    EXPECT_TRUE(a.value & VA_RT_FORMAT_YUV420_10, "no YUV420_10");
    TEST_PASS();
}

static void test_av1_rtformat(void) {
    TEST_START("AV1 P0 RTFormat includes YUV420_10");
    VAConfigAttrib a = { .type = VAConfigAttribRTFormat };
    EXPECT_STATUS(vaGetConfigAttributes(g_dpy, VAProfileAV1Profile0,
                                         VAEntrypointEncSlice, &a, 1));
    EXPECT_TRUE(a.value & VA_RT_FORMAT_YUV420_10, "no YUV420_10");
    TEST_PASS();
}

static void test_av1_attributes(void) {
#if VA_CHECK_VERSION(1, 12, 0)
    TEST_START("AV1 P0 config attributes supported");
    VAConfigAttrib a[3] = {
        { .type = VAConfigAttribEncAV1 },
        { .type = VAConfigAttribEncAV1Ext1 },
        { .type = VAConfigAttribEncAV1Ext2 }
    };
    EXPECT_STATUS(vaGetConfigAttributes(g_dpy, VAProfileAV1Profile0,
                                         VAEntrypointEncSlice, a, 3));
    EXPECT_TRUE(a[0].value != VA_ATTRIB_NOT_SUPPORTED, "EncAV1 unsupported");
    EXPECT_TRUE(a[1].value != VA_ATTRIB_NOT_SUPPORTED, "EncAV1Ext1 unsupported");
    EXPECT_TRUE(a[2].value != VA_ATTRIB_NOT_SUPPORTED, "EncAV1Ext2 unsupported");

    VAConfigAttribValEncAV1Ext2 v2;
    v2.value = a[2].value;
    EXPECT_TRUE(v2.bits.tx_mode_support != 0, "AV1 tx_mode_support must be non-zero");
    EXPECT_TRUE(v2.bits.tx_mode_support & 0x02, "AV1 TX_MODE_LARGEST should be supported");
    TEST_PASS();
#endif
}

static void test_hevc_attributes(void) {
    TEST_START("HEVC config attributes supported");
    VAConfigAttrib a[2] = {
        { .type = VAConfigAttribEncHEVCFeatures },
        { .type = VAConfigAttribEncHEVCBlockSizes }
    };
    EXPECT_STATUS(vaGetConfigAttributes(g_dpy, VAProfileHEVCMain,
                                         VAEntrypointEncSlice, a, 2));
    EXPECT_TRUE(a[0].value != VA_ATTRIB_NOT_SUPPORTED, "EncHEVCFeatures unsupported");
    EXPECT_TRUE(a[0].value != 0, "EncHEVCFeatures should be non-zero");
    EXPECT_TRUE(a[1].value != VA_ATTRIB_NOT_SUPPORTED, "EncHEVCBlockSizes unsupported");
    EXPECT_TRUE(a[1].value != 0, "EncHEVCBlockSizes should be non-zero");
    TEST_PASS();
}

static void test_config_ratecontrol(void) {
    TEST_START("Rate control: CQP + CBR + VBR supported");
    VAConfigAttrib a = { .type = VAConfigAttribRateControl };
    EXPECT_STATUS(vaGetConfigAttributes(g_dpy, VAProfileH264High,
                                         VAEntrypointEncSlice, &a, 1));
    EXPECT_TRUE(a.value & VA_RC_CQP, "no CQP");
    EXPECT_TRUE(a.value & VA_RC_CBR, "no CBR");
    EXPECT_TRUE(a.value & VA_RC_VBR, "no VBR");
    TEST_PASS();
}

static void test_config_packed_headers(void) {
    TEST_START("Packed headers: SEQ + PIC advertised");
    VAConfigAttrib a = { .type = VAConfigAttribEncPackedHeaders };
    EXPECT_STATUS(vaGetConfigAttributes(g_dpy, VAProfileH264High,
                                         VAEntrypointEncSlice, &a, 1));
    EXPECT_TRUE(a.value & VA_ENC_PACKED_HEADER_SEQUENCE, "no SEQ");
    EXPECT_TRUE(a.value & VA_ENC_PACKED_HEADER_PICTURE, "no PIC");
    TEST_PASS();
}

static void test_config_max_ref_frames(void) {
    TEST_START("Max ref frames reported (H.264) → B-frames disabled");
    VAConfigAttrib a = { .type = VAConfigAttribEncMaxRefFrames };
    EXPECT_STATUS(vaGetConfigAttributes(g_dpy, VAProfileH264High,
                                         VAEntrypointEncSlice, &a, 1));
    EXPECT_TRUE(a.value != VA_ATTRIB_NOT_SUPPORTED, "not supported");
    EXPECT_TRUE((a.value & 0xffff) >= 1, "L0 refs < 1");
    EXPECT_TRUE((a.value >> 16) == 0, "L1 refs should be 0 (B-frames disabled)");
    TEST_PASS();
}

static void test_av1_max_ref_frames(void) {
    TEST_START("Max ref frames reported (AV1) → B-frames disabled");
    VAConfigAttrib a = { .type = VAConfigAttribEncMaxRefFrames };
    EXPECT_STATUS(vaGetConfigAttributes(g_dpy, VAProfileAV1Profile0,
                                         VAEntrypointEncSlice, &a, 1));
    EXPECT_TRUE(a.value != VA_ATTRIB_NOT_SUPPORTED, "not supported");
    EXPECT_TRUE((a.value & 0xffff) >= 1, "L0 refs < 1");
    EXPECT_TRUE((a.value >> 16) == 0, "L1 refs should be 0 for AV1 (B-frames disabled)");
    TEST_PASS();
}

static void test_config_max_dimensions(void) {
    TEST_START("Max dimensions reported");
    VAConfigAttrib a[2] = {
        { .type = VAConfigAttribMaxPictureWidth },
        { .type = VAConfigAttribMaxPictureHeight }
    };
    EXPECT_STATUS(vaGetConfigAttributes(g_dpy, VAProfileH264High,
                                         VAEntrypointEncSlice, a, 2));
    EXPECT_TRUE(a[0].value != VA_ATTRIB_NOT_SUPPORTED, "Width unsupported");
    EXPECT_TRUE(a[0].value >= 4096, "Width too small");
    EXPECT_TRUE(a[1].value != VA_ATTRIB_NOT_SUPPORTED, "Height unsupported");
    EXPECT_TRUE(a[1].value >= 4096, "Height too small");
    TEST_PASS();
}

static void test_config_quality_range(void) {
    TEST_START("Quality range attribute reported");
    VAConfigAttrib a = { .type = VAConfigAttribEncQualityRange };
    EXPECT_STATUS(vaGetConfigAttributes(g_dpy, VAProfileH264High,
                                         VAEntrypointEncSlice, &a, 1));
    EXPECT_TRUE(a.value != VA_ATTRIB_NOT_SUPPORTED, "not supported");
    EXPECT_TRUE(a.value >= 1, "quality range < 1");
    TEST_PASS();
}

/* --- Capability-dependent profile tests --- */

/* Attempt vaCreateConfig for a "high-tier" profile. If the driver reports
 * unsupported (VA_STATUS_ERROR_UNSUPPORTED_PROFILE) that's an honest SKIP:
 * the runtime capability probe determined this NVENC/NVDEC combination
 * does not accept it. If it returns SUCCESS, confirm the advertised
 * VAConfigAttribRTFormat mask matches the expected chroma layout for the
 * profile — otherwise the config is misadvertised and clients will fail
 * at surface-alloc time. */
static void test_capability_gated_profile(VAProfile profile, const char *name,
                                           uint32_t expected_rtformat) {
    char label[64];
    snprintf(label, sizeof(label), "cap-gated encode: %-14s", name);
    TEST_START(label);

    VAConfigAttrib attr = { .type = VAConfigAttribRTFormat, .value = expected_rtformat };
    VAConfigID config;
    VAStatus st = vaCreateConfig(g_dpy, profile, VAEntrypointEncSlice,
                                  &attr, 1, &config);
    if (st == VA_STATUS_ERROR_UNSUPPORTED_PROFILE ||
        st == VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT ||
        st == VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT) {
        TEST_SKIP("gated off by NVENC caps on this hardware");
        return;
    }
    if (st != VA_STATUS_SUCCESS) {
        char msg[64]; snprintf(msg, sizeof(msg), "vaCreateConfig=%d", st);
        TEST_FAIL(msg);
        return;
    }
    /* Now check the RTFormat we advertise back matches what encode needs. */
    VAConfigAttrib rt = { .type = VAConfigAttribRTFormat };
    EXPECT_STATUS(vaGetConfigAttributes(g_dpy, profile, VAEntrypointEncSlice,
                                         &rt, 1));
    if ((rt.value & expected_rtformat) == 0) {
        vaDestroyConfig(g_dpy, config);
        TEST_FAIL("advertised RTFormat missing expected chroma");
        return;
    }
    vaDestroyConfig(g_dpy, config);
    TEST_PASS();
}

static void test_high_tier_profiles(void) {
    test_capability_gated_profile(VAProfileH264High10, "H264 High10",
                                   VA_RT_FORMAT_YUV420_10);
    test_capability_gated_profile(VAProfileHEVCMain422_10, "HEVC 422_10",
                                   VA_RT_FORMAT_YUV422_10);
    test_capability_gated_profile(VAProfileHEVCMain444, "HEVC Main444",
                                   VA_RT_FORMAT_YUV444);
    test_capability_gated_profile(VAProfileHEVCMain444_10, "HEVC 444_10",
                                   VA_RT_FORMAT_YUV444_10);
}

/* --- QP handling smoke test ---
 *
 * These do NOT validate the actual encoded QP in the bitstream (that would
 * require decoding the H.264 stream back and inspecting slice headers,
 * which is well beyond a smoke test). Instead they exercise the code paths
 * that parse initial_qp / min_qp / max_qp and pic_init_qp — a misaligned
 * struct or bad field name will make the test crash or fail to encode. */
static void test_qp_misc_param_parses(void) {
    TEST_START("Encode with initial_qp/min_qp/max_qp in CBR misc-param");
    VAConfigAttrib attr[2] = {
        { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 },
        { .type = VAConfigAttribRateControl, .value = VA_RC_CBR },
    };
    VAConfigID cfg;
    EXPECT_STATUS(vaCreateConfig(g_dpy, VAProfileH264High, VAEntrypointEncSlice,
                                  attr, 2, &cfg));
    VASurfaceID surf;
    EXPECT_STATUS(vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420, 320, 240,
                                    &surf, 1, NULL, 0));
    VAContextID ctx;
    EXPECT_STATUS(vaCreateContext(g_dpy, cfg, 320, 240, VA_PROGRESSIVE,
                                    &surf, 1, &ctx));

    /* Build a misc rate-control param populated with QP bounds. */
    VAEncMiscParameterBuffer *misc_hdr = NULL;
    VABufferID misc_buf;
    EXPECT_STATUS(vaCreateBuffer(g_dpy, ctx, VAEncMiscParameterBufferType,
                                  sizeof(VAEncMiscParameterBuffer) +
                                    sizeof(VAEncMiscParameterRateControl),
                                  1, NULL, &misc_buf));
    EXPECT_STATUS(vaMapBuffer(g_dpy, misc_buf, (void**)&misc_hdr));
    misc_hdr->type = VAEncMiscParameterTypeRateControl;
    VAEncMiscParameterRateControl *rc =
        (VAEncMiscParameterRateControl*)misc_hdr->data;
    memset(rc, 0, sizeof(*rc));
    rc->bits_per_second = 2000000;
    rc->target_percentage = 90;
    rc->initial_qp = 24;
    rc->min_qp = 20;
    rc->max_qp = 40;
    EXPECT_STATUS(vaUnmapBuffer(g_dpy, misc_buf));

    /* Send it in a Begin/Render/End cycle just to exercise the parser;
     * skip the actual encode-picture cycle (needs full seq/pic/slice
     * setup) since we only care that the misc-param path doesn't die. */
    EXPECT_STATUS(vaBeginPicture(g_dpy, ctx, surf));
    EXPECT_STATUS(vaRenderPicture(g_dpy, ctx, &misc_buf, 1));
    /* Deliberately DON'T call vaEndPicture — that would trigger the real
     * encode and require seq/pic/slice params. The parse happens inside
     * vaRenderPicture handlers. */

    vaDestroyBuffer(g_dpy, misc_buf);
    vaDestroyContext(g_dpy, ctx);
    vaDestroySurfaces(g_dpy, &surf, 1);
    vaDestroyConfig(g_dpy, cfg);
    TEST_PASS();
}

/* --- Error path tests --- */

static void test_invalid_entrypoint(void) {
    TEST_START("vaCreateConfig with invalid entrypoint → error");
    VAConfigAttrib a = { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 };
    VAConfigID config;
    /* Use a valid profile but wrong entrypoint type (0xFF) */
    VAStatus st = vaCreateConfig(g_dpy, VAProfileH264High, (VAEntrypoint)0xFF,
                                  &a, 1, &config);
    EXPECT_TRUE(st != VA_STATUS_SUCCESS, "should fail for invalid entrypoint");
    TEST_PASS();
}

static void test_encode_on_decode_only_profile(void) {
    TEST_START("vaCreateConfig encode on MPEG2 (decode-only) → error");
    VAConfigAttrib a = { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 };
    VAConfigID config;
    VAStatus st = vaCreateConfig(g_dpy, VAProfileMPEG2Simple,
                                  VAEntrypointEncSlice, &a, 1, &config);
    EXPECT_TRUE(st != VA_STATUS_SUCCESS, "should fail for decode-only profile");
    TEST_PASS();
}

static void test_create_config_all_encode_profiles(void) {
    VAProfile profiles[] = {
        VAProfileH264ConstrainedBaseline, VAProfileH264Main, VAProfileH264High,
        VAProfileHEVCMain, VAProfileHEVCMain10, VAProfileAV1Profile0,
    };
    for (int i = 0; i < 6; i++) {
        char name[64];
        snprintf(name, sizeof(name), "vaCreateConfig for encode profile %d", profiles[i]);
        TEST_START(name);

        VAConfigAttrib a = { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 };
        VAConfigID config;
        VAStatus st = vaCreateConfig(g_dpy, profiles[i], VAEntrypointEncSlice,
                                      &a, 1, &config);
        EXPECT_STATUS(st);
        st = vaDestroyConfig(g_dpy, config);
        EXPECT_STATUS(st);
        TEST_PASS();
    }
}

/* --- Surface creation tests --- */

static void test_surface_nv12(void) {
    TEST_START("Create NV12 surface 1920x1080");
    VASurfaceID surface;
    VAStatus st = vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420, 1920, 1080,
                                    &surface, 1, NULL, 0);
    EXPECT_STATUS(st);
    vaDestroySurfaces(g_dpy, &surface, 1);
    TEST_PASS();
}

static void test_surface_p010(void) {
    TEST_START("Create P010 surface 1920x1080 (10-bit)");
    VASurfaceID surface;
    VAStatus st = vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420_10, 1920, 1080,
                                    &surface, 1, NULL, 0);
    if (st != VA_STATUS_SUCCESS) {
        TEST_SKIP("10-bit surfaces not supported");
        return;
    }
    vaDestroySurfaces(g_dpy, &surface, 1);
    TEST_PASS();
}

static void test_surface_multiple(void) {
    TEST_START("Create 16 surfaces simultaneously");
    VASurfaceID surfaces[16];
    VAStatus st = vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420, 640, 480,
                                    surfaces, 16, NULL, 0);
    EXPECT_STATUS(st);
    vaDestroySurfaces(g_dpy, surfaces, 16);
    TEST_PASS();
}

static void test_surface_small(void) {
    TEST_START("Create tiny surface 16x16");
    VASurfaceID surface;
    VAStatus st = vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420, 16, 16,
                                    &surface, 1, NULL, 0);
    EXPECT_STATUS(st);
    vaDestroySurfaces(g_dpy, &surface, 1);
    TEST_PASS();
}

static void test_surface_4k(void) {
    TEST_START("Create 4K surface 3840x2160");
    VASurfaceID surface;
    VAStatus st = vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420, 3840, 2160,
                                    &surface, 1, NULL, 0);
    EXPECT_STATUS(st);
    vaDestroySurfaces(g_dpy, &surface, 1);
    TEST_PASS();
}

/* --- Main --- */

static void test_export_surface_descriptor(void) {
    TEST_START("Export NV12 surface descriptor (separate layers)");
    VASurfaceID surface_id;
    VAStatus st = vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420, 128, 128, &surface_id, 1, NULL, 0);
    EXPECT_STATUS(st);

    VADRMPRIMESurfaceDescriptor desc;
    /* The NVIDIA direct backend exports multi-planar surfaces as separate
     * layers (one layer per plane). This is what Chromium's zero-copy import
     * path expects for browser decode. */
    st = vaExportSurfaceHandle(g_dpy, surface_id, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, 
                               VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc);
    EXPECT_STATUS(st);

    EXPECT_TRUE(desc.num_layers == 2, "expected 2 layers for NV12");
    EXPECT_TRUE(desc.layers[0].num_planes == 1, "expected 1 plane per layer");
    EXPECT_TRUE(desc.layers[1].num_planes == 1, "expected 1 plane per layer");
    EXPECT_TRUE(desc.layers[0].drm_format == DRM_FORMAT_R8, "expected DRM_FORMAT_R8");
    EXPECT_TRUE(desc.layers[1].drm_format == DRM_FORMAT_RG88, "expected DRM_FORMAT_RG88");

    for (int i = 0; i < (int)desc.num_objects; i++) {
        close(desc.objects[i].fd);
    }
    vaDestroySurfaces(g_dpy, &surface_id, 1);
    TEST_PASS();
}

static void test_export_surface_descriptor_p010(void) {
    TEST_START("Export P010 surface descriptor (separate layers)");
    VASurfaceID surface_id;
    VAStatus st = vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420_10, 128, 128, &surface_id, 1, NULL, 0);
    EXPECT_STATUS(st);

    VADRMPRIMESurfaceDescriptor desc;
    st = vaExportSurfaceHandle(g_dpy, surface_id, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, 
                               VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc);
    EXPECT_STATUS(st);

    EXPECT_TRUE(desc.num_layers == 2, "expected 2 layers for P010");
    EXPECT_TRUE(desc.layers[0].num_planes == 1, "expected 1 plane per layer");
    EXPECT_TRUE(desc.layers[1].num_planes == 1, "expected 1 plane per layer");
    EXPECT_TRUE(desc.layers[0].drm_format == DRM_FORMAT_R16, "expected DRM_FORMAT_R16");
    EXPECT_TRUE(desc.layers[1].drm_format == DRM_FORMAT_RG1616, "expected DRM_FORMAT_RG1616");

    for (int i = 0; i < (int)desc.num_objects; i++) {
        close(desc.objects[i].fd);
    }
    vaDestroySurfaces(g_dpy, &surface_id, 1);
    TEST_PASS();
}

int main(void)
{
    test_global_setup();

    printf("\n=== nvidia-vaapi-driver config & capability tests ===\n");
    printf("Driver: %s\n\n", vaQueryVendorString(g_dpy));

    printf("Encode entrypoints:\n");
    test_encode_entrypoints();

    printf("\nDecode entrypoints:\n");
    test_decode_entrypoints();

    printf("\nConfig attributes:\n");
    test_config_rtformat();
    test_hevc_main10_rtformat();
    test_av1_rtformat();
    test_av1_attributes();
    test_hevc_attributes();
    test_config_ratecontrol();
    test_config_packed_headers();
    test_config_max_ref_frames();
    test_av1_max_ref_frames();
    test_config_max_dimensions();
    test_config_quality_range();

    printf("\nError paths:\n");
    test_invalid_entrypoint();
    test_encode_on_decode_only_profile();

    printf("\nConfig creation:\n");
    test_create_config_all_encode_profiles();

    printf("\nSurface creation:\n");
    test_surface_nv12();
    test_surface_p010();
    test_surface_multiple();
    test_surface_small();
    test_surface_4k();

    printf("\nSurface export:\n");
    test_export_surface_descriptor();
    test_export_surface_descriptor_p010();

    printf("\nHigh-tier profile capability gating:\n");
    test_high_tier_profiles();

    printf("\nQP handling:\n");
    test_qp_misc_param_parses();

    test_print_summary("Config tests");
    test_global_teardown();
    return g_fail > 0 ? 1 : 0;
}

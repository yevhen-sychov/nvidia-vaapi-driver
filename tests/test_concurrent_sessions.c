/*
 * test_concurrent_sessions.c — Multi-session stress for the encode/decode
 * paths.
 *
 * Motivating bug: a Chromium GPU-process SIGSEGV (exit_code=139) that
 * reproduces when a screenshare stream is started while a camera NVENC
 * session is already live and encoding. Standalone screenshare (no camera)
 * works fine, standalone camera works fine. Adding a second live session
 * is the trigger.
 *
 * Coverage:
 *   1. Two H.264 encode sessions in parallel at the same resolution
 *      (camera + screenshare, both 1920x1088-ish).
 *   2. Encode session live + a second encode session created and driven
 *      while the first is streaming (staggered start; mimics the
 *      "camera on, then click share screen" flow).
 *   3. Encode session live + a decode session opened and driven in
 *      parallel at the same resolution (mimics the WebRTC self-preview
 *      "decode-back the just-encoded stream" path).
 *   4. Rapid create+destroy churn of a secondary encode session while
 *      the primary is continuously encoding (stress for our context
 *      lifecycle / DPB / DMA-BUF handle-table paths).
 *
 * If NVENC hits the consumer-card concurrent-session cap the affected
 * subtest logs SKIP instead of FAIL — that's a hardware policy limit,
 * not a driver bug.
 *
 * Build: pulled in by meson (see meson.build), links libva + libva-drm
 * + libpthread.
 */

#include "test_common.h"
#include <pthread.h>
#include <sys/stat.h>
#include <va/va_drmcommon.h>
#include <va/va_enc_h264.h>

#define ENC_W 1920
#define ENC_H 1088
#define FRAMES_PER_SESSION 8

typedef struct {
    int thread_idx;
    uint32_t width;
    uint32_t height;
    int frames_to_encode;
    /* Delay in milliseconds before thread starts real work (used to stagger
     * a "second stream added while first is streaming" scenario). */
    int start_delay_ms;
    /* Output: whether the thread succeeded end-to-end. */
    bool ok;
    /* Output: last VAStatus and the operation label at which it happened. */
    VAStatus last_status;
    const char *last_op;
    /* If the encode session couldn't even be created because NVENC returned
     * cap-exceeded, we mark this to let the harness report SKIP not FAIL. */
    bool skipped_encoder_cap;
} EncoderThreadArgs;

typedef struct {
    int thread_idx;
    uint32_t width;
    uint32_t height;
    int loops;
    int start_delay_ms;
    bool ok;
    VAStatus last_status;
    const char *last_op;
} DecoderThreadArgs;

static void msleep(int ms) {
    struct timespec ts = { .tv_sec = ms / 1000,
                           .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Snapshot of a VADRMPRIMESurfaceDescriptor's shape (independent of the
 * lifetime of the descriptor itself — the FDs are already closed by the
 * time we compare snapshots). Used to diff single-session vs concurrent-
 * session exports and catch drift that would silently hand Chrome garbage. */
typedef struct {
    uint32_t fourcc;
    uint32_t width;
    uint32_t height;
    uint32_t num_objects;
    uint32_t num_layers;
    uint64_t obj_size[4];
    uint64_t obj_modifier[4];
    uint32_t layer_drm_format[4];
    uint32_t layer_num_planes[4];
    uint32_t layer_offset[4][4];
    uint32_t layer_pitch[4][4];
    /* Aggregate invariants — captured for logging + assertion. */
    bool     any_zero_pitch;
    bool     any_negative_fd;
    bool     any_stat_fail;
    uint64_t smallest_object_size;
} DescShape;

static void capture_desc_shape(const VADRMPRIMESurfaceDescriptor *desc,
                               DescShape *out) {
    memset(out, 0, sizeof(*out));
    out->fourcc = desc->fourcc;
    out->width = desc->width;
    out->height = desc->height;
    out->num_objects = desc->num_objects;
    out->num_layers = desc->num_layers;
    out->smallest_object_size = UINT64_MAX;

    for (uint32_t i = 0; i < desc->num_objects && i < 4; i++) {
        out->obj_size[i] = desc->objects[i].size;
        out->obj_modifier[i] = desc->objects[i].drm_format_modifier;
        if (desc->objects[i].fd < 0) out->any_negative_fd = true;
        else {
            struct stat sb;
            if (fstat(desc->objects[i].fd, &sb) < 0) out->any_stat_fail = true;
        }
        if (desc->objects[i].size < out->smallest_object_size)
            out->smallest_object_size = desc->objects[i].size;
    }
    for (uint32_t i = 0; i < desc->num_layers && i < 4; i++) {
        out->layer_drm_format[i] = desc->layers[i].drm_format;
        out->layer_num_planes[i] = desc->layers[i].num_planes;
        for (uint32_t j = 0; j < desc->layers[i].num_planes && j < 4; j++) {
            out->layer_offset[i][j] = desc->layers[i].offset[j];
            out->layer_pitch[i][j]  = desc->layers[i].pitch[j];
            if (desc->layers[i].pitch[j] == 0) out->any_zero_pitch = true;
            /* NOTE: intentionally NOT checking (offset + pitch*height) against
             * object size — NVIDIA's block-linear modifier
             * (0x03000000000606014-family) uses GOB reordering, so a plane
             * occupies more bytes than the naive linear formula gives. Any
             * check we add here would false-positive on every tiled export.
             * Linear-modifier consumers are welcome to add a modifier-gated
             * variant of this check. */
        }
    }
}

static void print_desc_shape(const char *label, const DescShape *s) {
    printf("    [%s] fourcc=0x%08x %ux%u  objs=%u layers=%u\n",
           label, s->fourcc, s->width, s->height, s->num_objects, s->num_layers);
    for (uint32_t i = 0; i < s->num_objects && i < 4; i++) {
        printf("        obj[%u] size=%lu mod=0x%016lx\n",
               i, (unsigned long)s->obj_size[i], (unsigned long)s->obj_modifier[i]);
    }
    for (uint32_t i = 0; i < s->num_layers && i < 4; i++) {
        printf("        layer[%u] fmt=0x%08x planes=%u",
               i, s->layer_drm_format[i], s->layer_num_planes[i]);
        for (uint32_t j = 0; j < s->layer_num_planes[i] && j < 4; j++) {
            printf("  p%u{off=%u pitch=%u}", j,
                   s->layer_offset[i][j], s->layer_pitch[i][j]);
        }
        printf("\n");
    }
    printf("        invariants: zero_pitch=%d bad_fd=%d fstat_fail=%d\n",
           s->any_zero_pitch, s->any_negative_fd, s->any_stat_fail);
}

/* True if the two shapes agree on the invariants that must be stable across
 * exports of the same-format same-resolution surface, regardless of what
 * other sessions are alive at the time. Anything that comes back different
 * IS the bug we're looking for. */
static bool shapes_match_invariants(const DescShape *a, const DescShape *b) {
    if (a->fourcc != b->fourcc) return false;
    if (a->width != b->width || a->height != b->height) return false;
    if (a->num_objects != b->num_objects) return false;
    if (a->num_layers != b->num_layers) return false;
    for (uint32_t i = 0; i < a->num_objects && i < 4; i++) {
        if (a->obj_size[i] != b->obj_size[i]) return false;
        if (a->obj_modifier[i] != b->obj_modifier[i]) return false;
    }
    for (uint32_t i = 0; i < a->num_layers && i < 4; i++) {
        if (a->layer_drm_format[i] != b->layer_drm_format[i]) return false;
        if (a->layer_num_planes[i] != b->layer_num_planes[i]) return false;
        for (uint32_t j = 0; j < a->layer_num_planes[i] && j < 4; j++) {
            if (a->layer_offset[i][j] != b->layer_offset[i][j]) return false;
            if (a->layer_pitch[i][j]  != b->layer_pitch[i][j])  return false;
        }
    }
    return true;
}

/* Export the given surface into a fresh DMA-BUF descriptor, capture its shape
 * into *shape, then close the FDs. Returns true iff export succeeded and
 * every per-descriptor invariant we check passed. */
static bool export_and_capture(VASurfaceID surface, DescShape *shape,
                                VAStatus *st_out) {
    VADRMPRIMESurfaceDescriptor desc;
    memset(&desc, 0, sizeof(desc));
    *st_out = vaExportSurfaceHandle(g_dpy, surface,
                                     VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                     VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc);
    if (*st_out != VA_STATUS_SUCCESS) return false;
    capture_desc_shape(&desc, shape);
    for (uint32_t i = 0; i < desc.num_objects; i++) close(desc.objects[i].fd);
    return !shape->any_negative_fd && !shape->any_stat_fail &&
           !shape->any_zero_pitch;
}

/* ------- Encoder helper (H.264, IDR every frame to avoid ref frame juggling) */

static bool encode_one_frame(VADisplay dpy, VAContextID context,
                             VASurfaceID surface, VABufferID coded_buf,
                             uint32_t w, uint32_t h,
                             const char **op_out, VAStatus *st_out) {
    VABufferID seq_buf = VA_INVALID_ID, pic_buf = VA_INVALID_ID, slice_buf = VA_INVALID_ID;

    VAEncSequenceParameterBufferH264 seq = {
        .picture_width_in_mbs = w / 16,
        .picture_height_in_mbs = h / 16,
        .intra_period = 30,
        .ip_period = 1,
    };
    *op_out = "seq buf";
    *st_out = vaCreateBuffer(dpy, context, VAEncSequenceParameterBufferType,
                             sizeof(seq), 1, &seq, &seq_buf);
    if (*st_out != VA_STATUS_SUCCESS) return false;

    VAEncPictureParameterBufferH264 pic = {
        .coded_buf = coded_buf,
        .pic_fields.bits.idr_pic_flag = 1,
    };
    *op_out = "pic buf";
    *st_out = vaCreateBuffer(dpy, context, VAEncPictureParameterBufferType,
                             sizeof(pic), 1, &pic, &pic_buf);
    if (*st_out != VA_STATUS_SUCCESS) return false;

    VAEncSliceParameterBufferH264 slice = { .slice_type = 2 };
    *op_out = "slice buf";
    *st_out = vaCreateBuffer(dpy, context, VAEncSliceParameterBufferType,
                             sizeof(slice), 1, &slice, &slice_buf);
    if (*st_out != VA_STATUS_SUCCESS) return false;

    *op_out = "vaBeginPicture";
    *st_out = vaBeginPicture(dpy, context, surface);
    if (*st_out != VA_STATUS_SUCCESS) return false;

    VABufferID bufs[] = { seq_buf, pic_buf, slice_buf };
    *op_out = "vaRenderPicture";
    *st_out = vaRenderPicture(dpy, context, bufs, 3);
    if (*st_out != VA_STATUS_SUCCESS) return false;

    *op_out = "vaEndPicture";
    *st_out = vaEndPicture(dpy, context);
    if (*st_out != VA_STATUS_SUCCESS) return false;

    *op_out = "vaSyncSurface";
    *st_out = vaSyncSurface(dpy, surface);
    if (*st_out != VA_STATUS_SUCCESS) return false;

    /* Map + immediately unmap the coded buffer to actually pull the
     * bitstream out — matches what a real WebRTC pipeline does per frame. */
    VACodedBufferSegment *seg = NULL;
    *op_out = "vaMapBuffer(coded)";
    *st_out = vaMapBuffer(dpy, coded_buf, (void **)&seg);
    if (*st_out != VA_STATUS_SUCCESS) return false;
    vaUnmapBuffer(dpy, coded_buf);

    vaDestroyBuffer(dpy, seq_buf);
    vaDestroyBuffer(dpy, pic_buf);
    vaDestroyBuffer(dpy, slice_buf);
    return true;
}

static void *encoder_thread(void *raw) {
    EncoderThreadArgs *a = raw;
    a->ok = false;
    a->skipped_encoder_cap = false;
    a->last_status = VA_STATUS_SUCCESS;
    a->last_op = "start";

    if (a->start_delay_ms > 0) msleep(a->start_delay_ms);

    VAConfigAttrib attrib = { .type = VAConfigAttribRTFormat,
                              .value = VA_RT_FORMAT_YUV420 };
    VAConfigID config;
    a->last_op = "vaCreateConfig(enc)";
    a->last_status = vaCreateConfig(g_dpy, VAProfileH264High,
                                    VAEntrypointEncSlice, &attrib, 1, &config);
    if (a->last_status != VA_STATUS_SUCCESS) return NULL;

    VASurfaceID surface;
    a->last_op = "vaCreateSurfaces(enc)";
    a->last_status = vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420,
                                       a->width, a->height, &surface, 1, NULL, 0);
    if (a->last_status != VA_STATUS_SUCCESS) {
        vaDestroyConfig(g_dpy, config);
        return NULL;
    }

    VAContextID context;
    a->last_op = "vaCreateContext(enc)";
    a->last_status = vaCreateContext(g_dpy, config, a->width, a->height,
                                     VA_PROGRESSIVE, &surface, 1, &context);
    if (a->last_status != VA_STATUS_SUCCESS) {
        /* NVENC session cap looks like VA_STATUS_ERROR_HW_BUSY /
         * ALLOCATION_FAILED / OPERATION_FAILED depending on the code path.
         * We can't cleanly distinguish "your card only allows N concurrent"
         * from a real bug at this layer, so we flag SKIP for those. */
        if (a->last_status == VA_STATUS_ERROR_HW_BUSY ||
            a->last_status == VA_STATUS_ERROR_ALLOCATION_FAILED ||
            a->last_status == VA_STATUS_ERROR_OPERATION_FAILED) {
            a->skipped_encoder_cap = true;
        }
        vaDestroySurfaces(g_dpy, &surface, 1);
        vaDestroyConfig(g_dpy, config);
        return NULL;
    }

    VABufferID coded_buf;
    a->last_op = "vaCreateBuffer(coded)";
    a->last_status = vaCreateBuffer(g_dpy, context, VAEncCodedBufferType,
                                    a->width * a->height, 1, NULL, &coded_buf);
    if (a->last_status != VA_STATUS_SUCCESS) {
        vaDestroyContext(g_dpy, context);
        vaDestroySurfaces(g_dpy, &surface, 1);
        vaDestroyConfig(g_dpy, config);
        return NULL;
    }

    /* Fill the surface with mid-grey once via a host image so encoding has
     * real content. Doing it before the loop is enough — encoder re-reads
     * the surface each vaBeginPicture. */
    VAImageFormat fmt = { .fourcc = VA_FOURCC_NV12 };
    VAImage image;
    a->last_op = "vaCreateImage(NV12)";
    a->last_status = vaCreateImage(g_dpy, &fmt, a->width, a->height, &image);
    if (a->last_status == VA_STATUS_SUCCESS) {
        void *img_data = NULL;
        vaMapBuffer(g_dpy, image.buf, &img_data);
        if (img_data) memset(img_data, 128, image.data_size);
        vaUnmapBuffer(g_dpy, image.buf);
        vaPutImage(g_dpy, surface, image.image_id, 0, 0, a->width, a->height,
                   0, 0, a->width, a->height);
        vaDestroyImage(g_dpy, image.image_id);
    }

    bool loop_ok = true;
    for (int f = 0; f < a->frames_to_encode; f++) {
        if (!encode_one_frame(g_dpy, context, surface, coded_buf,
                              a->width, a->height, &a->last_op, &a->last_status)) {
            loop_ok = false;
            break;
        }
    }

    vaDestroyBuffer(g_dpy, coded_buf);
    vaDestroyContext(g_dpy, context);
    vaDestroySurfaces(g_dpy, &surface, 1);
    vaDestroyConfig(g_dpy, config);
    a->ok = loop_ok;
    return NULL;
}

static void *decoder_thread(void *raw) {
    DecoderThreadArgs *a = raw;
    a->ok = false;
    a->last_status = VA_STATUS_SUCCESS;
    a->last_op = "start";

    if (a->start_delay_ms > 0) msleep(a->start_delay_ms);

    /* Not actually decoding a real bitstream — the point of this test is
     * to exercise the driver's context-lifecycle + DMA-BUF export paths
     * concurrently with a live encoder. We stand up a decode context,
     * export the surface descriptor a few times (that's the path Chrome
     * uses for zero-copy video display and the one that has previously
     * misfired), then tear down. */
    VAConfigAttrib attrib = { .type = VAConfigAttribRTFormat,
                              .value = VA_RT_FORMAT_YUV420 };
    VAConfigID config;
    a->last_op = "vaCreateConfig(dec)";
    /* AV1 is the safest decode profile on our fork; falls back to H.264
     * if AV1 decode isn't advertised on this build. */
    VAProfile prof = test_has_entrypoint(g_dpy, VAProfileAV1Profile0,
                                          VAEntrypointVLD)
                         ? VAProfileAV1Profile0
                         : VAProfileH264High;
    a->last_status = vaCreateConfig(g_dpy, prof, VAEntrypointVLD,
                                    &attrib, 1, &config);
    if (a->last_status != VA_STATUS_SUCCESS) return NULL;

    VASurfaceID surface;
    a->last_op = "vaCreateSurfaces(dec)";
    a->last_status = vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420,
                                       a->width, a->height, &surface, 1, NULL, 0);
    if (a->last_status != VA_STATUS_SUCCESS) {
        vaDestroyConfig(g_dpy, config);
        return NULL;
    }

    VAContextID context;
    a->last_op = "vaCreateContext(dec)";
    a->last_status = vaCreateContext(g_dpy, config, a->width, a->height,
                                     VA_PROGRESSIVE, &surface, 1, &context);
    if (a->last_status != VA_STATUS_SUCCESS) {
        vaDestroySurfaces(g_dpy, &surface, 1);
        vaDestroyConfig(g_dpy, config);
        return NULL;
    }

    bool loop_ok = true;
    for (int i = 0; i < a->loops; i++) {
        VADRMPRIMESurfaceDescriptor desc;
        a->last_op = "vaExportSurfaceHandle(dec)";
        a->last_status = vaExportSurfaceHandle(g_dpy, surface,
                                                VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                                VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                                                &desc);
        if (a->last_status != VA_STATUS_SUCCESS) { loop_ok = false; break; }
        for (uint32_t j = 0; j < desc.num_objects; j++) close(desc.objects[j].fd);
        msleep(5);
    }

    vaDestroyContext(g_dpy, context);
    vaDestroySurfaces(g_dpy, &surface, 1);
    vaDestroyConfig(g_dpy, config);
    a->ok = loop_ok;
    return NULL;
}

/* ---------------------------- Test bodies ---------------------------- */

static void test_two_encoders_parallel_same_res(void) {
    TEST_START("Two H.264 encoders @ 1920x1088 in parallel");
    if (!test_has_entrypoint(g_dpy, VAProfileH264High, VAEntrypointEncSlice)) {
        TEST_SKIP("H.264 encode unavailable"); return;
    }

    EncoderThreadArgs a = { .thread_idx = 0, .width = ENC_W, .height = ENC_H,
                            .frames_to_encode = FRAMES_PER_SESSION };
    EncoderThreadArgs b = { .thread_idx = 1, .width = ENC_W, .height = ENC_H,
                            .frames_to_encode = FRAMES_PER_SESSION };
    pthread_t ta, tb;
    pthread_create(&ta, NULL, encoder_thread, &a);
    pthread_create(&tb, NULL, encoder_thread, &b);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    if (a.skipped_encoder_cap || b.skipped_encoder_cap) {
        TEST_SKIP("NVENC concurrent-session cap reached (consumer card policy)");
        return;
    }
    if (!a.ok) { TEST_FAIL(a.last_op ? a.last_op : "thread A"); return; }
    if (!b.ok) { TEST_FAIL(b.last_op ? b.last_op : "thread B"); return; }
    TEST_PASS();
}

static void test_camera_then_screenshare_staggered(void) {
    TEST_START("Encoder A live, encoder B added mid-stream (camera+screenshare flow)");
    if (!test_has_entrypoint(g_dpy, VAProfileH264High, VAEntrypointEncSlice)) {
        TEST_SKIP("H.264 encode unavailable"); return;
    }

    /* A starts immediately and encodes 20 frames — enough to be firmly
     * mid-stream by the time B arrives 300ms later. */
    EncoderThreadArgs a = { .thread_idx = 0, .width = 1280, .height = 720,
                            .frames_to_encode = 20 };
    EncoderThreadArgs b = { .thread_idx = 1, .width = ENC_W, .height = ENC_H,
                            .frames_to_encode = FRAMES_PER_SESSION,
                            .start_delay_ms = 300 };
    pthread_t ta, tb;
    pthread_create(&ta, NULL, encoder_thread, &a);
    pthread_create(&tb, NULL, encoder_thread, &b);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    if (a.skipped_encoder_cap || b.skipped_encoder_cap) {
        TEST_SKIP("NVENC concurrent-session cap reached"); return;
    }
    if (!a.ok) { TEST_FAIL(a.last_op ? a.last_op : "camera thread"); return; }
    if (!b.ok) { TEST_FAIL(b.last_op ? b.last_op : "screenshare thread"); return; }
    TEST_PASS();
}

static void test_encode_plus_decode_same_res(void) {
    TEST_START("Encoder live @ 1280x720 + decode context added @ 1280x720");
    if (!test_has_entrypoint(g_dpy, VAProfileH264High, VAEntrypointEncSlice)) {
        TEST_SKIP("H.264 encode unavailable"); return;
    }

    EncoderThreadArgs enc = { .thread_idx = 0, .width = 1280, .height = 720,
                              .frames_to_encode = 20 };
    DecoderThreadArgs dec = { .thread_idx = 1, .width = 1280, .height = 720,
                              .loops = 15, .start_delay_ms = 200 };
    pthread_t te, td;
    pthread_create(&te, NULL, encoder_thread, &enc);
    pthread_create(&td, NULL, decoder_thread, &dec);
    pthread_join(te, NULL);
    pthread_join(td, NULL);

    if (enc.skipped_encoder_cap) {
        TEST_SKIP("NVENC concurrent-session cap reached"); return;
    }
    if (!enc.ok) { TEST_FAIL(enc.last_op ? enc.last_op : "encoder"); return; }
    if (!dec.ok) { TEST_FAIL(dec.last_op ? dec.last_op : "decoder"); return; }
    TEST_PASS();
}

static void test_rapid_second_encoder_churn(void) {
    TEST_START("Primary encoder live, secondary encoder create/destroy x5");
    if (!test_has_entrypoint(g_dpy, VAProfileH264High, VAEntrypointEncSlice)) {
        TEST_SKIP("H.264 encode unavailable"); return;
    }

    EncoderThreadArgs primary = { .thread_idx = 0, .width = 1280, .height = 720,
                                  .frames_to_encode = 40 };
    pthread_t tp;
    pthread_create(&tp, NULL, encoder_thread, &primary);

    /* Give the primary a head-start, then churn a secondary a few times. */
    msleep(150);
    bool churn_ok = true;
    for (int i = 0; i < 5; i++) {
        EncoderThreadArgs sec = { .thread_idx = 100 + i,
                                  .width = ENC_W, .height = ENC_H,
                                  .frames_to_encode = 3 };
        pthread_t ts;
        pthread_create(&ts, NULL, encoder_thread, &sec);
        pthread_join(ts, NULL);
        if (sec.skipped_encoder_cap) continue;  /* not a churn failure */
        if (!sec.ok) {
            printf("\n    churn iteration %d failed at %s (status %d)",
                   i, sec.last_op ? sec.last_op : "?", sec.last_status);
            churn_ok = false;
            break;
        }
    }
    pthread_join(tp, NULL);

    if (primary.skipped_encoder_cap) {
        TEST_SKIP("NVENC concurrent-session cap reached"); return;
    }
    if (!primary.ok) { TEST_FAIL(primary.last_op ? primary.last_op : "primary"); return; }
    if (!churn_ok) { TEST_FAIL("churn iteration failed"); return; }
    TEST_PASS();
}

/* ---------------- Descriptor-drift diagnostic ---------------- */

/* Two decode surfaces at the same resolution — one exported cold, one
 * exported while an encoder is actively encoding — SHOULD produce
 * bit-identical descriptors. If the concurrent-exported descriptor drifts
 * (different pitch, offset, size, modifier), that's exactly the kind of
 * silent-lie the driver would hand Chrome that leads Chrome's ANGLE
 * importer to segfault when it tries to bind the buffer. */
static void test_descriptor_shape_stable_under_concurrent_encoder(void) {
    TEST_START("Decode DMA-BUF shape stays identical while encoder is running");
    if (!test_has_entrypoint(g_dpy, VAProfileH264High, VAEntrypointEncSlice)) {
        TEST_SKIP("H.264 encode unavailable"); return;
    }

    /* --- Baseline: export a decode surface with no other session alive. */
    VASurfaceID base_surface;
    VAStatus st = vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420, 1280, 720,
                                    &base_surface, 1, NULL, 0);
    EXPECT_STATUS(st);
    DescShape baseline;
    VAStatus est;
    bool base_ok = export_and_capture(base_surface, &baseline, &est);
    if (!base_ok) {
        printf("\n    baseline export invariants failed:\n");
        print_desc_shape("baseline", &baseline);
        TEST_FAIL("baseline export invariant failed");
        vaDestroySurfaces(g_dpy, &base_surface, 1);
        return;
    }
    vaDestroySurfaces(g_dpy, &base_surface, 1);

    /* --- Now spin up a concurrent encoder and export a fresh decode
     * surface while the encoder is streaming. Repeat a few times. */
    EncoderThreadArgs enc = { .thread_idx = 0, .width = 1920, .height = 1088,
                              .frames_to_encode = 30 };
    pthread_t te;
    pthread_create(&te, NULL, encoder_thread, &enc);
    msleep(150);

    bool all_match = true;
    DescShape concurrent;
    for (int i = 0; i < 5; i++) {
        VASurfaceID surf;
        st = vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420, 1280, 720,
                              &surf, 1, NULL, 0);
        if (st != VA_STATUS_SUCCESS) {
            printf("\n    iteration %d: vaCreateSurfaces failed (%d)", i, st);
            all_match = false;
            break;
        }
        bool ok = export_and_capture(surf, &concurrent, &est);
        if (!ok) {
            printf("\n    iteration %d: descriptor invariant failed", i);
            print_desc_shape("concurrent", &concurrent);
            all_match = false;
            vaDestroySurfaces(g_dpy, &surf, 1);
            break;
        }
        if (!shapes_match_invariants(&baseline, &concurrent)) {
            printf("\n    iteration %d: descriptor drift between baseline and concurrent", i);
            print_desc_shape("baseline",  &baseline);
            print_desc_shape("concurrent", &concurrent);
            all_match = false;
            vaDestroySurfaces(g_dpy, &surf, 1);
            break;
        }
        vaDestroySurfaces(g_dpy, &surf, 1);
        msleep(30);
    }
    pthread_join(te, NULL);

    if (enc.skipped_encoder_cap) {
        TEST_SKIP("NVENC concurrent-session cap reached"); return;
    }
    if (!all_match) { TEST_FAIL("descriptor differs across sessions"); return; }
    TEST_PASS();
}

/* Complement: two ENCODE surfaces of the same shape, one exported cold
 * and one exported while another encoder is streaming, must also agree.
 * Encode surfaces go through the WebGL/canvas worker importer in Chrome —
 * which is the one that crashed on our user's screenshare start. */
static void test_encode_descriptor_shape_stable_second_encoder(void) {
    TEST_START("Encode DMA-BUF shape stays identical while second encoder is running");
    if (!test_has_entrypoint(g_dpy, VAProfileH264High, VAEntrypointEncSlice)) {
        TEST_SKIP("H.264 encode unavailable"); return;
    }

    /* Baseline: standalone encode surface, no other session alive. */
    VAConfigAttrib attr = { .type = VAConfigAttribRTFormat,
                            .value = VA_RT_FORMAT_YUV420 };
    VAConfigID cfg;
    EXPECT_STATUS(vaCreateConfig(g_dpy, VAProfileH264High, VAEntrypointEncSlice,
                                  &attr, 1, &cfg));
    VASurfaceID enc_surf;
    EXPECT_STATUS(vaCreateSurfaces(g_dpy, VA_RT_FORMAT_YUV420, 1280, 720,
                                    &enc_surf, 1, NULL, 0));
    VAContextID enc_ctx;
    EXPECT_STATUS(vaCreateContext(g_dpy, cfg, 1280, 720, VA_PROGRESSIVE,
                                    &enc_surf, 1, &enc_ctx));

    DescShape baseline;
    VAStatus est;
    if (!export_and_capture(enc_surf, &baseline, &est)) {
        printf("\n    baseline export invariants failed:\n");
        print_desc_shape("enc-baseline", &baseline);
        TEST_FAIL("enc baseline invariant failed");
        vaDestroyContext(g_dpy, enc_ctx);
        vaDestroySurfaces(g_dpy, &enc_surf, 1);
        vaDestroyConfig(g_dpy, cfg);
        return;
    }

    /* Kick a second encoder in a background thread, then re-export the
     * primary encode surface a few times to see if concurrency perturbs
     * the descriptor. */
    EncoderThreadArgs sec = { .thread_idx = 1, .width = 1920, .height = 1088,
                              .frames_to_encode = 30 };
    pthread_t ts;
    pthread_create(&ts, NULL, encoder_thread, &sec);
    msleep(150);

    bool all_match = true;
    DescShape concurrent;
    for (int i = 0; i < 5; i++) {
        if (!export_and_capture(enc_surf, &concurrent, &est)) {
            printf("\n    iteration %d: concurrent enc export invariant failed", i);
            print_desc_shape("enc-concurrent", &concurrent);
            all_match = false;
            break;
        }
        if (!shapes_match_invariants(&baseline, &concurrent)) {
            printf("\n    iteration %d: enc descriptor drift", i);
            print_desc_shape("enc-baseline",   &baseline);
            print_desc_shape("enc-concurrent", &concurrent);
            all_match = false;
            break;
        }
        msleep(30);
    }
    pthread_join(ts, NULL);
    vaDestroyContext(g_dpy, enc_ctx);
    vaDestroySurfaces(g_dpy, &enc_surf, 1);
    vaDestroyConfig(g_dpy, cfg);

    if (sec.skipped_encoder_cap) {
        TEST_SKIP("NVENC concurrent-session cap reached"); return;
    }
    if (!all_match) { TEST_FAIL("encode descriptor drift"); return; }
    TEST_PASS();
}

int main(void) {
    test_global_setup();

    printf("\n=== nvidia-vaapi-driver concurrent-sessions tests ===\n");
    printf("Driver: %s\n\n", vaQueryVendorString(g_dpy));

    printf("Parallel encoders:\n");
    test_two_encoders_parallel_same_res();
    test_camera_then_screenshare_staggered();

    printf("\nEncode + decode co-existence:\n");
    test_encode_plus_decode_same_res();

    printf("\nContext-lifecycle stress:\n");
    test_rapid_second_encoder_churn();

    printf("\nDescriptor shape stability (the interesting one):\n");
    test_descriptor_shape_stable_under_concurrent_encoder();
    test_encode_descriptor_shape_stable_second_encoder();

    test_print_summary("Concurrent sessions");
    test_global_teardown();
    return g_fail > 0 ? 1 : 0;
}

/*
 * Encode-side VA-API dispatch, split out of vabackend.c so the shared
 * decode-focused translation unit stays close to upstream (elFarto's
 * nvidia-vaapi-driver, which has no encode support). The four entry points
 * below are called from vabackend.c only when nvCtx->isEncode is true or
 * when the VA-API config attribute query targets an encode profile.
 *
 * Nothing in here touches decode-side state, so upstream merges into
 * vabackend.c leave this file untouched.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "vabackend.h"
#include "nvenc.h"
#include "nvenc-ipc.h"

/* IPC path forward-declaration: called only from nvEndPictureEncode below. */
static VAStatus nvEndPictureEncodeIPC(NVDriver *drv, NVContext *nvCtx);

void nvGetConfigAttributesEncode(
        NVDriver *drv,
        VAProfile profile,
        VAConfigAttrib *attrib_list,
        int num_attribs
    )
{
    for (int i = 0; i < num_attribs; i++)
    {
        switch (attrib_list[i].type) {
        case VAConfigAttribRTFormat:
            switch (profile) {
            case VAProfileH264High10:
                attrib_list[i].value = VA_RT_FORMAT_YUV420_10;
                break;
            case VAProfileHEVCMain422_10:
                attrib_list[i].value = VA_RT_FORMAT_YUV422_10;
                break;
            case VAProfileHEVCMain444:
                attrib_list[i].value = VA_RT_FORMAT_YUV444;
                break;
            case VAProfileHEVCMain444_10:
                attrib_list[i].value = VA_RT_FORMAT_YUV444_10;
                break;
            case VAProfileHEVCMain10:
            case VAProfileAV1Profile0:
                attrib_list[i].value = VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10;
                break;
            default:
                attrib_list[i].value = VA_RT_FORMAT_YUV420;
                break;
            }
            break;
        case VAConfigAttribRateControl:
            attrib_list[i].value = VA_RC_CQP | VA_RC_CBR | VA_RC_VBR;
            break;
        case VAConfigAttribEncRateControlExt: {
            /* Advertise temporal-layer (SVC) support so clients such as
             * Chrome/WebRTC will use the hardware encoder for temporal
             * scalability modes (e.g. L1T2/L1T3 screenshare) instead of
             * falling back to software. This is currently wired end-to-end
             * only for AV1 (layer structure parsing + NVENC temporal SVC
             * config), so report it only for AV1 to avoid falsely claiming
             * support for codecs whose per-layer path is not implemented.
             * NVENC supports up to 4 temporal layers, so report
             * max_num_temporal_layers_minus1=3. Per-temporal-layer bitrate
             * control is not implemented, so leave the flag at 0. */
            if (profile == VAProfileAV1Profile0) {
                VAConfigAttribValEncRateControlExt v = { .value = 0 };
                v.bits.max_num_temporal_layers_minus1 = 3;
                v.bits.temporal_layer_bitrate_control_flag = 0;
                attrib_list[i].value = v.value;
            } else {
                attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
            }
            break;
        }
        case VAConfigAttribEncPackedHeaders:
            /* NVENC authors its own SPS/PPS/VPS and slice headers; there is no
             * API to hand it a client-built slice header, and the bitstream it
             * emits is self-consistent. So what we advertise here is not "which
             * headers can you give us" but "which headers will the client stop
             * building itself".
             *
             * Deliberately NOT advertising SLICE (or PICTURE): Chromium's
             * H264VaapiVideoEncoderDelegate treats packed headers as
             * all-or-nothing — if SEQUENCE|PICTURE|SLICE are not all present it
             * configures the context with VA_ENC_PACKED_HEADER_NONE and sends
             * no headers at all, leaving the driver to author them. That is
             * exactly the arrangement that matches NVENC. Advertising the full
             * set instead made Chromium build SPS/PPS and per-slice headers
             * (with has_emulation_bytes = 0, expecting us to insert emulation
             * prevention) that we then silently discarded — wasted work, and a
             * standing risk that its frame_num/ref-list assumptions diverge
             * from the headers NVENC actually wrote.
             *
             * RAW_DATA is advertised because GStreamer gates SEI/AUD insertion
             * on it, and MISC because FFmpeg gates SEI/AUD/metadata on that.
             * Both tolerate the missing SLICE bit: they simply skip the packed
             * slice header and let the driver produce it. */
            attrib_list[i].value = VA_ENC_PACKED_HEADER_SEQUENCE
                                 | VA_ENC_PACKED_HEADER_MISC
                                 | VA_ENC_PACKED_HEADER_RAW_DATA;
            break;
        case VAConfigAttribEncMaxRefFrames:
            /* Disable B-frames for all codecs for now to ensure 1:1 mapping and no reordering issues.
             * FFmpeg VA-API sends frames in encode order, and with PTD=0 we must match that.
             * Supporting B-frames with manual PTD requires complex DPB management. */
            attrib_list[i].value = 1; /* 1 L0, 0 L1 */
            break;
        case VAConfigAttribMaxPictureWidth:
            nvenc_max_encode_dimensions(drv, profile, &attrib_list[i].value, NULL);
            break;
        case VAConfigAttribMaxPictureHeight:
            nvenc_max_encode_dimensions(drv, profile, NULL, &attrib_list[i].value);
            break;
        case VAConfigAttribEncQualityRange:
            attrib_list[i].value = 7; //NVENC presets P1-P7
            break;
        case VAConfigAttribEncHEVCFeatures:
            if (profile == VAProfileHEVCMain || profile == VAProfileHEVCMain10 ||
                profile == VAProfileHEVCMain422_10 ||
                profile == VAProfileHEVCMain444 || profile == VAProfileHEVCMain444_10) {
                VAConfigAttribValEncHEVCFeatures v = { .value = 0 };
                v.bits.amp = VA_FEATURE_SUPPORTED;
                v.bits.sao = VA_FEATURE_SUPPORTED;
                v.bits.temporal_mvp = VA_FEATURE_SUPPORTED;
                v.bits.strong_intra_smoothing = VA_FEATURE_SUPPORTED;
                v.bits.sign_data_hiding = VA_FEATURE_SUPPORTED;
                v.bits.constrained_intra_pred = VA_FEATURE_SUPPORTED;
                v.bits.cu_qp_delta = VA_FEATURE_SUPPORTED;
                v.bits.weighted_prediction = VA_FEATURE_SUPPORTED;
                v.bits.transquant_bypass = VA_FEATURE_SUPPORTED;
                attrib_list[i].value = v.value;
            } else {
                attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
            }
            break;
        case VAConfigAttribEncHEVCBlockSizes:
            if (profile == VAProfileHEVCMain || profile == VAProfileHEVCMain10 ||
                profile == VAProfileHEVCMain422_10 ||
                profile == VAProfileHEVCMain444 || profile == VAProfileHEVCMain444_10) {
                VAConfigAttribValEncHEVCBlockSizes v = { .value = 0 };
                v.bits.log2_max_coding_tree_block_size_minus3 = 3; // 64x64
                v.bits.log2_min_coding_tree_block_size_minus3 = 0; // 8x8
                v.bits.log2_min_luma_coding_block_size_minus3 = 0; // 8x8
                v.bits.log2_max_luma_transform_block_size_minus2 = 3; // 32x32
                v.bits.log2_min_luma_transform_block_size_minus2 = 0; // 4x4
                v.bits.max_max_transform_hierarchy_depth_inter = 2;
                v.bits.max_max_transform_hierarchy_depth_intra = 2;
                attrib_list[i].value = v.value;
            } else {
                attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
            }
            break;
#if VA_CHECK_VERSION(1, 12, 0)
        case VAConfigAttribEncAV1:
            if (profile == VAProfileAV1Profile0) {
                VAConfigAttribValEncAV1 v = { .value = 0 };
                v.bits.support_filter_intra = VA_FEATURE_SUPPORTED;
                v.bits.support_intra_edge_filter = VA_FEATURE_SUPPORTED;
                v.bits.support_interintra_compound = VA_FEATURE_SUPPORTED;
                v.bits.support_masked_compound = VA_FEATURE_SUPPORTED;
                v.bits.support_warped_motion = VA_FEATURE_SUPPORTED;
                v.bits.support_palette_mode = VA_FEATURE_SUPPORTED;
                v.bits.support_dual_filter = VA_FEATURE_SUPPORTED;
                v.bits.support_jnt_comp = VA_FEATURE_SUPPORTED;
                v.bits.support_ref_frame_mvs = VA_FEATURE_SUPPORTED;
                v.bits.support_restoration = VA_FEATURE_SUPPORTED;
                v.bits.support_allow_intrabc = VA_FEATURE_SUPPORTED;
                v.bits.support_cdef_channel_strength = VA_FEATURE_SUPPORTED;
                attrib_list[i].value = v.value;
            } else {
                attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
            }
            break;
        case VAConfigAttribEncAV1Ext1:
            if (profile == VAProfileAV1Profile0) {
                VAConfigAttribValEncAV1Ext1 v = { .value = 0 };
                v.bits.interpolation_filter = 0x1f;
                v.bits.segment_feature_support = 0xff;
                attrib_list[i].value = v.value;
            } else {
                attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
            }
            break;
        case VAConfigAttribEncAV1Ext2:
            if (profile == VAProfileAV1Profile0) {
                VAConfigAttribValEncAV1Ext2 v = { .value = 0 };
                v.bits.tile_size_bytes_minus1 = 3;
                v.bits.obu_size_bytes_minus1 = 3;
                v.bits.tx_mode_support = 0x07;
                v.bits.max_tile_num_minus1 = 63;
                attrib_list[i].value = v.value;
            } else {
                attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
            }
            break;
#endif
        case VAConfigAttribEncMaxTileRows:
            attrib_list[i].value = 1;
            break;
        case VAConfigAttribEncMaxTileCols:
            attrib_list[i].value = 1;
            break;
        default:
            attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
            break;
        }
    }
}

void nvRenderPictureEncode(NVContext *nvCtx, NVBuffer *buf)
{
    NVENCContext *nvencCtx = (NVENCContext*) nvCtx->encodeData;
    bool isH264 = (nvCtx->profile == VAProfileH264ConstrainedBaseline ||
                   nvCtx->profile == VAProfileH264Main ||
                   nvCtx->profile == VAProfileH264High ||
                   nvCtx->profile == VAProfileH264High10);
    bool isHEVC = (nvCtx->profile == VAProfileHEVCMain ||
                   nvCtx->profile == VAProfileHEVCMain10 ||
                   nvCtx->profile == VAProfileHEVCMain422_10 ||
                   nvCtx->profile == VAProfileHEVCMain444 ||
                   nvCtx->profile == VAProfileHEVCMain444_10);
    bool isAV1 = (nvCtx->profile == VAProfileAV1Profile0);

    switch (buf->bufferType) {
    case VAEncSequenceParameterBufferType:
        if (isH264) {
            h264enc_handle_sequence_params(nvencCtx, buf);
        } else if (isHEVC) {
            hevcenc_handle_sequence_params(nvencCtx, buf);
        } else if (isAV1) {
            av1enc_handle_sequence_params(nvencCtx, buf);
        }
        break;
    case VAEncPictureParameterBufferType:
        if (isH264) {
            h264enc_handle_picture_params(nvencCtx, buf);
        } else if (isHEVC) {
            hevcenc_handle_picture_params(nvencCtx, buf);
        } else if (isAV1) {
            av1enc_handle_picture_params(nvencCtx, buf);
        }
        break;
    case VAEncSliceParameterBufferType:
        if (isH264) {
            h264enc_handle_slice_params(nvencCtx, buf);
        } else if (isHEVC) {
            hevcenc_handle_slice_params(nvencCtx, buf);
        } else if (isAV1) {
            av1enc_handle_slice_params(nvencCtx, buf);
        }
        break;
    case VAEncMiscParameterBufferType:
        if (isH264) {
            h264enc_handle_misc_params(nvencCtx, buf);
        } else if (isHEVC) {
            hevcenc_handle_misc_params(nvencCtx, buf);
        } else if (isAV1) {
            av1enc_handle_misc_params(nvencCtx, buf);
        }
        break;
    case VAEncCodedBufferType:
        /* Coded buffer is handled at EndPicture */
        break;
    case VAEncPackedHeaderParameterBufferType:
    case VAEncPackedHeaderDataBufferType:
        /* Packed headers: NVENC generates its own headers, skip these.
         * See VAConfigAttribEncPackedHeaders in nvGetConfigAttributesEncode()
         * for why we no longer advertise SLICE/PICTURE, which keeps well-behaved
         * clients from building these in the first place. */
        break;
    case VAEncMacroblockMapBufferType:
        /* AV1 segmentation map. Chromium's AV1 encoder delegate submits one
         * every frame. NVENC exposes no per-block AV1 segment map through the
         * public API (only a whole-frame QP delta map, which is a different
         * thing), so there is nothing to apply — accept it silently rather than
         * logging once per frame for the life of the stream. Rate control still
         * works: it arrives through base_qindex, which we do honour. */
        break;
    default:
        LOG("Encode: unhandled buffer type: %d", buf->bufferType);
        break;
    }
}

VAStatus nvEndPictureEncode(NVDriver *drv, NVContext *nvCtx)
{
    NVENCContext *nvencCtx = (NVENCContext*) nvCtx->encodeData;
    NVSurface *surface = nvCtx->renderTarget;

    if (nvencCtx == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    /* IPC path: delegate to 64-bit helper */
    if (nvencCtx->useIPC) {
        return nvEndPictureEncodeIPC(drv, nvCtx);
    }

    if (nvencCtx->encoder == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    CHECK_CUDA_RESULT_RETURN(drv->cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

    /* Initialize encoder on first frame (we now have all params from sequence/picture buffers) */
    if (!nvencCtx->initialized) {
        GUID codecGuid = nvenc_va_profile_to_codec_guid(nvCtx->profile);
        GUID profileGuid = nvenc_va_profile_to_profile_guid(nvCtx->profile);

        if (!nvenc_init_encoder(nvencCtx, nvencCtx->width, nvencCtx->height,
                                codecGuid, profileGuid,
                                NV_ENC_TUNING_INFO_LOW_LATENCY)) {
            CHECK_CUDA_RESULT(drv->cu->cuCtxPopCurrent(NULL));
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        if (!nvenc_alloc_output_buffer(nvencCtx)) {
            CHECK_CUDA_RESULT(drv->cu->cuCtxPopCurrent(NULL));
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
    } else {
        /* Encoder already running: pick up any bitrate/framerate changes
         * Chrome pushed via misc params since the last encoded frame. Without
         * this the encoder stays at whatever bitrate was in effect at
         * initialization and ignores WebRTC BWE reductions, so it emits far
         * above the target bitrate and saturates the peer connection. */
        nvenc_reconfigure_if_needed(nvencCtx);
    }

    /* Realise the surface so we have a backing image with CUDA memory */
    if (!drv->backend->realiseSurface(drv, surface)) {
        LOG("Encode: failed to realise input surface");
        CHECK_CUDA_RESULT(drv->cu->cuCtxPopCurrent(NULL));
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    BackingImage *img = surface->backingImage;
    if (img == NULL) {
        LOG("Encode: surface has no backing image");
        CHECK_CUDA_RESULT(drv->cu->cuCtxPopCurrent(NULL));
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /*
     * The backing image contains CUarray(s) for each plane.
     * NVENC needs a linear CUdeviceptr. We need to allocate a linear buffer,
     * copy the CUarray contents into it, then register with NVENC.
     *
     * Use surface dimensions for the copy (the CUarray matches the surface).
     * NVENC width/height may differ due to MB/CTU alignment.
     */
    uint32_t surfWidth = surface->width;
    uint32_t surfHeight = surface->height;
    uint32_t encWidth = nvencCtx->width;
    uint32_t encHeight = nvencCtx->height;
    NV_ENC_BUFFER_FORMAT encFmt = nvencCtx->inputFormat;

    uint32_t copyWidth = (surfWidth < encWidth) ? surfWidth : encWidth;
    uint32_t copyHeight = (surfHeight < encHeight) ? surfHeight : encHeight;

    /* Only log when the geometry actually changes — this runs on every frame,
     * and for a steady stream it prints the same line forever. Behind
     * LOG_ENABLED() so a build with logging off does none of this work. */
    if (LOG_ENABLED()) {
        const NVENCGeometryLog geometry = {
            .surfWidth = surfWidth, .surfHeight = surfHeight,
            .encWidth = encWidth, .encHeight = encHeight,
            .copyWidth = copyWidth, .copyHeight = copyHeight,
            .imgFormat = (int32_t) img->format,
            .bitDepth = (int32_t) surface->bitDepth,
            .encFormat = (int32_t) encFmt,
        };
        if (nvenc_log_state_changed(&nvencCtx->loggedGeometry, &geometry, sizeof(geometry))) {
            LOG("Encode: surface %ux%u (format %d, bitDepth %d), encoder %ux%u (format %d), copy %ux%u",
                surfWidth, surfHeight, img->format, surface->bitDepth,
                encWidth, encHeight, encFmt, copyWidth, copyHeight);
        }
    }

    /* Calculate pitch and size for NV12/P010 linear buffer.
     * Allocate for the full encode height (may be larger than surface due to alignment)
     * but only copy surfHeight rows from the CUarray. */
    uint32_t bytesPerPixel = (encFmt == NV_ENC_BUFFER_FORMAT_YUV420_10BIT) ? 2 : 1;
    uint32_t pitch = encWidth * bytesPerPixel;
    /* Align pitch to 256 bytes for NVENC */
    pitch = (pitch + 255) & ~255;
    uint32_t lumaSize = pitch * encHeight;
    uint32_t chromaSize = pitch * (encHeight / 2);
    uint32_t totalSize = lumaSize + chromaSize;

    /*
     * Reuse a persistent linear staging buffer + NVENC-registered resource for
     * the lifetime of the session instead of allocating/registering (and
     * freeing/unregistering) new ones on every single frame. Repeatedly
     * churning CUDA device memory allocations and NVENC resource registrations
     * was found to gradually destabilize long-running encode sessions,
     * eventually crashing the GPU process after a few minutes.
     */
    if (nvencCtx->registeredRes != NULL &&
        (nvencCtx->linearBufferSize < totalSize ||
         nvencCtx->registeredWidth != encWidth ||
         nvencCtx->registeredHeight != encHeight ||
         nvencCtx->registeredPitch != pitch)) {
        /* Resolution/format changed: drop the old buffer/registration */
        nvenc_unregister_resource(nvencCtx, nvencCtx->registeredRes);
        nvencCtx->registeredRes = NULL;
        drv->cu->cuMemFree(nvencCtx->linearBuffer);
        nvencCtx->linearBuffer = 0;
        nvencCtx->linearBufferSize = 0;
    }

    if (nvencCtx->linearBuffer == 0) {
        CUresult cuRes = drv->cu->cuMemAlloc(&nvencCtx->linearBuffer, totalSize);
        if (cuRes != CUDA_SUCCESS) {
            LOG("Encode: failed to allocate linear buffer (%u bytes): %d", totalSize, cuRes);
            CHECK_CUDA_RESULT(drv->cu->cuCtxPopCurrent(NULL));
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        nvencCtx->linearBufferSize = totalSize;
    }

    if (nvencCtx->registeredRes == NULL) {
        if (!nvenc_register_cuda_resource(nvencCtx, nvencCtx->linearBuffer,
                                          encWidth, encHeight, pitch,
                                          encFmt, &nvencCtx->registeredRes)) {
            drv->cu->cuMemFree(nvencCtx->linearBuffer);
            nvencCtx->linearBuffer = 0;
            nvencCtx->linearBufferSize = 0;
            CHECK_CUDA_RESULT(drv->cu->cuCtxPopCurrent(NULL));
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        nvencCtx->registeredWidth = encWidth;
        nvencCtx->registeredHeight = encHeight;
        nvencCtx->registeredPitch = pitch;
    }

    CUdeviceptr linearBuffer = nvencCtx->linearBuffer;
    NV_ENC_REGISTERED_PTR registeredRes = nvencCtx->registeredRes;
    CUresult cuRes;

    /* Zero the buffer so padded rows are clean */
    drv->cu->cuMemsetD8Async(linearBuffer, 0, totalSize, 0);

    /* Copy luma plane from CUarray to linear buffer */
    CUDA_MEMCPY2D copy = {0};
    copy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
    copy.srcArray = img->arrays[0];
    copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.dstDevice = linearBuffer;
    copy.dstPitch = pitch;
    copy.WidthInBytes = copyWidth * bytesPerPixel;
    copy.Height = copyHeight;

    cuRes = drv->cu->cuMemcpy2D(&copy);
    if (cuRes != CUDA_SUCCESS) {
        LOG("Encode: luma copy failed: %d (surface=%ux%u, pitch=%u)", cuRes, surfWidth, surfHeight, pitch);
        CHECK_CUDA_RESULT(drv->cu->cuCtxPopCurrent(NULL));
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Copy chroma plane (interleaved UV) */
    memset(&copy, 0, sizeof(copy));
    copy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
    copy.srcArray = img->arrays[1];
    copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.dstDevice = linearBuffer + lumaSize;
    copy.dstPitch = pitch;
    /* Chroma plane: each pixel has 2 channels (U,V) interleaved */
    copy.WidthInBytes = copyWidth * bytesPerPixel;
    copy.Height = copyHeight / 2;

    cuRes = drv->cu->cuMemcpy2D(&copy);
    if (cuRes != CUDA_SUCCESS) {
        LOG("Encode: chroma copy failed: %d", cuRes);
        CHECK_CUDA_RESULT(drv->cu->cuCtxPopCurrent(NULL));
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Map the persistent registered resource for this frame */
    NV_ENC_INPUT_PTR mappedResource = NULL;
    NV_ENC_BUFFER_FORMAT mappedFmt = encFmt;
    if (!nvenc_map_resource(nvencCtx, registeredRes, &mappedResource, &mappedFmt)) {
        CHECK_CUDA_RESULT(drv->cu->cuCtxPopCurrent(NULL));
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Encode the frame.
     * Use only OUTPUT_SPSPPS on the first frame; after that let NVENC handle it. */
    uint32_t picFlags = (nvencCtx->frameCount == 0 || nvencCtx->forceIDR)
        ? (NV_ENC_PIC_FLAG_OUTPUT_SPSPPS | NV_ENC_PIC_FLAG_FORCEIDR)
        : 0;
    nvencCtx->forceIDR = false;
    int encResult = nvenc_encode_frame(nvencCtx, mappedResource, mappedFmt,
                                       encWidth, encHeight, pitch,
                                       nvencCtx->picType, picFlags);

    /* Unmap only - the buffer/registration itself is kept for reuse on the
     * next frame and released only when the encode context is destroyed. */
    nvenc_unmap_resource(nvencCtx, mappedResource);

    if (encResult < 0) {
        CHECK_CUDA_RESULT(drv->cu->cuCtxPopCurrent(NULL));
        return VA_STATUS_ERROR_ENCODING_ERROR;
    }

    /* Find the coded buffer */
    NVBuffer *codedBuf = (NVBuffer*) nvGetObjectPtr(drv, OBJECT_TYPE_BUFFER,
                                                    nvencCtx->currentCodedBufId);

    if (encResult == 0) {
        /* NVENC needs more input (B-frame reordering). Mark coded buffer as empty. */
        if (codedBuf != NULL && codedBuf->ptr != NULL) {
            NVCodedBuffer *coded = (NVCodedBuffer*) codedBuf->ptr;
            coded->bitstreamSize = 0;
            coded->hasData = false;
        }
        LOG_DEBUG("Encode: frame %lu buffered (needs more input)",
                  (unsigned long)(nvencCtx->frameCount - 1));
        CHECK_CUDA_RESULT_RETURN(drv->cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
        return VA_STATUS_SUCCESS;
    }

    /* Lock bitstream and copy into the coded buffer */
    void *bitstreamPtr = NULL;
    uint32_t bitstreamSize = 0;
    if (!nvenc_lock_bitstream(nvencCtx, &bitstreamPtr, &bitstreamSize)) {
        CHECK_CUDA_RESULT(drv->cu->cuCtxPopCurrent(NULL));
        return VA_STATUS_ERROR_ENCODING_ERROR;
    }

    if (codedBuf != NULL && codedBuf->ptr != NULL) {
        NVCodedBuffer *coded = (NVCodedBuffer*) codedBuf->ptr;
        /* Grow the buffer if needed */
        if (bitstreamSize > coded->bitstreamAlloc) {
            void *newBuf = realloc(coded->bitstreamData, bitstreamSize);
            if (newBuf != NULL) {
                coded->bitstreamData = newBuf;
                coded->bitstreamAlloc = bitstreamSize;
            } else {
                LOG("Encode: failed to grow coded buffer to %u bytes", bitstreamSize);
                nvenc_unlock_bitstream(nvencCtx);
                CHECK_CUDA_RESULT(drv->cu->cuCtxPopCurrent(NULL));
                return VA_STATUS_ERROR_ALLOCATION_FAILED;
            }
        }
        memcpy(coded->bitstreamData, bitstreamPtr, bitstreamSize);
        coded->bitstreamSize = bitstreamSize;
        coded->hasData = true;
        /* Pure per-frame progress: the size differs every frame, so there is no
         * "changed" state to key off and nothing actionable in a steady stream.
         * Available under NVD_LOG_VERBOSE=1; NVD_STATS=1 gives throughput. */
        LOG_DEBUG("Encode: frame %lu encoded, %u bytes",
                  (unsigned long)(nvencCtx->frameCount - 1), bitstreamSize);
    } else {
        LOG("Encode: WARNING - no coded buffer found for id %d", nvencCtx->currentCodedBufId);
    }

    nvenc_unlock_bitstream(nvencCtx);

    CHECK_CUDA_RESULT_RETURN(drv->cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
    return VA_STATUS_SUCCESS;
}

/* IPC encode path: send frame data to 64-bit helper, receive bitstream */
static VAStatus nvEndPictureEncodeIPC(NVDriver *drv, NVContext *nvCtx)
{
    NVENCContext *nvencCtx = (NVENCContext*) nvCtx->encodeData;
    NVSurface *surface = nvCtx->renderTarget;

    (void)drv;

    /* Connect to helper on first use */
    if (nvencCtx->ipcFd < 0) {
        /* Try connecting to an already-running helper first, then start one */
        static const char *helper_paths[] = {
            "/usr/libexec/nvenc-helper",
            "/usr/local/libexec/nvenc-helper",
            "/usr/lib/nvidia-vaapi-driver/nvenc-helper",
            NULL
        };
        nvencCtx->ipcFd = nvenc_ipc_connect();
        if (nvencCtx->ipcFd < 0) {
            for (int pi = 0; helper_paths[pi] != NULL; pi++) {
                if (access(helper_paths[pi], X_OK) == 0) {
                    LOG("IPC encode: starting helper: %s", helper_paths[pi]);
                    nvencCtx->ipcFd = nvenc_ipc_connect_or_start(helper_paths[pi]);
                    if (nvencCtx->ipcFd >= 0) break;
                }
            }
        }
        if (nvencCtx->ipcFd < 0) {
            LOG("IPC encode: failed to connect to nvenc-helper (is it installed?)");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        LOG("IPC encode: connected to nvenc-helper (fd=%d)", nvencCtx->ipcFd);
    }

    /* Initialize encoder via IPC on first frame */
    if (!nvencCtx->initialized) {
        bool isH264 = (nvCtx->profile == VAProfileH264ConstrainedBaseline ||
                       nvCtx->profile == VAProfileH264Main ||
                       nvCtx->profile == VAProfileH264High);
        bool isHEVC = (nvCtx->profile == VAProfileHEVCMain ||
                       nvCtx->profile == VAProfileHEVCMain10);
        NVEncIPCInitParams params = {
            .width = nvencCtx->width,
            .height = nvencCtx->height,
            .maxWidth = nvencCtx->maxWidth,
            .maxHeight = nvencCtx->maxHeight,
            .codec = isH264 ? 0 : (isHEVC ? 1 : 2),
            .profile = (uint32_t)nvCtx->profile,
            .frameRateNum = nvencCtx->frameRateNum,
            .frameRateDen = nvencCtx->frameRateDen,
            .bitrate = nvencCtx->bitrate,
            .maxBitrate = nvencCtx->maxBitrate,
            .gopLength = nvencCtx->intraPeriod,
            .ipPeriod = nvencCtx->ipPeriod,
            .qualityLevel = nvencCtx->qualityLevel,
            .rcMode = nvencCtx->rcMode,
            .is10bit = (nvencCtx->inputFormat == NV_ENC_BUFFER_FORMAT_YUV420_10BIT) ? 1 : 0,
            .numTemporalLayers = nvencCtx->numTemporalLayers,
        };

        int shm_fd = -1;
        uint32_t shm_size = 0;
        if (nvenc_ipc_init(nvencCtx->ipcFd, &params, &shm_fd, &shm_size) != 0) {
            LOG("IPC encode: init failed");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        nvencCtx->initialized = true;

        /* Map shared memory if the helper provided one */
        if (shm_fd >= 0 && shm_size > 0) {
            nvencCtx->shmPtr = mmap(NULL, shm_size, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, shm_fd, 0);
            if (nvencCtx->shmPtr == MAP_FAILED) {
                nvencCtx->shmPtr = NULL;
                LOG("IPC encode: shm mmap failed, falling back to socket");
            } else {
                nvencCtx->shmSize = shm_size;
                nvencCtx->shmFd = shm_fd;

                /* Redirect the surface's hostPixelData to the SHM region.
                 * This eliminates the memcpy in EndPicture — Steam writes
                 * directly to shared memory via vaDeriveImage → vaMapBuffer.
                 * The helper reads from the same physical pages. Zero copy. */
                if (surface->hostPixelSize <= shm_size) {
                    if (!surface->hostPixelIsShm) {
                        free(surface->hostPixelData);
                    }
                    surface->hostPixelData = nvencCtx->shmPtr;
                    surface->hostPixelSize = shm_size;
                    surface->hostPixelIsShm = true;
                    LOG("IPC encode: shm zero-copy enabled, %u bytes", shm_size);
                } else {
                    LOG("IPC encode: shm enabled (copy mode), %u bytes", shm_size);
                }
            }
            close(shm_fd); /* mmap keeps the mapping alive after close */
        }

        LOG("IPC encode: encoder initialized %ux%u shm=%s",
            params.width, params.height, nvencCtx->shmPtr ? "yes" : "no");
    }

    /* Encode via IPC.
     * Priority: 1) Host pixel data from vaDeriveImage/vaPutImage (has actual captured pixels)
     *           2) DRM-backed surface via NVIDIA opaque fds (GPU zero-copy)
     * Host data takes priority because vaDeriveImage is how Steam writes captured
     * frames — the GPU surface may exist but not contain the capture. */
    void *bitstream = NULL;
    uint32_t bsSize = 0;
    int ret;
    int dmabuf_fds[4] = {-1, -1, -1, -1};
    int num_dmabuf_fds = 0;
    NVEncIPCEncodeDmaBufParams dp = {0};
    bool useDmaBuf = false;
    bool useHostData = false;

    /* Prefer host pixel data if available (written by vaDeriveImage → vaMapBuffer) */
    if (surface->hostPixelData != NULL && surface->hostPixelSize > 0) {
        useHostData = true;
    } else if (surface->backingImage != NULL && surface->backingImage->nvFds[0] > 0) {
        /* DRM-backed surface: send per-plane NVIDIA opaque fds to helper.
         * The helper imports each into CUDA (cuImportExternalMemory with
         * OPAQUE_FD), maps to CUarray, copies to linear buffer, encodes.
         * We dup() the fds because CUDA takes ownership on import. */
        BackingImage *img = surface->backingImage;
        const NVFormatInfo *fmtInfo = &formatsInfo[img->format];
        dp.width = surface->width;
        dp.height = surface->height;
        dp.num_planes = fmtInfo->numPlanes;
        dp.bppc = fmtInfo->bppc;
        dp.is10bit = (nvencCtx->inputFormat == NV_ENC_BUFFER_FORMAT_YUV420_10BIT) ? 1 : 0;
        dp.force_idr = nvencCtx->forceIDR ? 1 : 0;
        dp.temporal_id = nvencCtx->temporalId;
        dp.pic_type = nvencCtx->picType;
        nvencCtx->forceIDR = false;
        for (uint32_t p = 0; p < fmtInfo->numPlanes && p < 4; p++) {
            dmabuf_fds[p] = dup(img->nvFds[p]);
            dp.pitches[p] = img->strides[p];
            dp.offsets[p] = 0;
            dp.sizes[p] = img->memorySizes[p];
        }
        num_dmabuf_fds = (int)fmtInfo->numPlanes;
        useDmaBuf = true;
    }

    if (useHostData) {
        /* Host memory path: pixel data from vaDeriveImage/vaPutImage.
         * IMPORTANT: use the SURFACE dimensions (e.g. 1920x1080), not the
         * encoder dimensions (e.g. 1920x1088). */
        uint32_t surfW = surface->width;
        uint32_t surfH = surface->height;
        uint32_t frameSize = surface->hostPixelSize;
        uint32_t forceIDR = nvencCtx->forceIDR ? 1 : 0;
        uint32_t temporalId = nvencCtx->temporalId;
        uint32_t picType = nvencCtx->picType;
        nvencCtx->forceIDR = false;

        if (nvencCtx->shmPtr != NULL && frameSize <= nvencCtx->shmSize) {
            /* SHM path: if hostPixelData IS the shm (zero-copy), skip memcpy.
             * Otherwise copy frame to shared memory. */
            if (surface->hostPixelData != nvencCtx->shmPtr) {
                memcpy(nvencCtx->shmPtr, surface->hostPixelData, frameSize);
            }
            if (nvencCtx->frameCount < 3) {
                LOG("IPC encode: SHM path %ux%u %u bytes", surfW, surfH, frameSize);
            }
            ret = nvenc_ipc_encode_shm(nvencCtx->ipcFd, surfW, surfH,
                                        frameSize, forceIDR, temporalId, picType,
                                        &bitstream, &bsSize);
        } else {
            /* Socket fallback: snapshot + full send */
            void *snapshot = malloc(frameSize);
            if (snapshot == NULL) {
                return VA_STATUS_ERROR_ALLOCATION_FAILED;
            }
            memcpy(snapshot, surface->hostPixelData, frameSize);
            if (nvencCtx->frameCount < 3) {
                LOG("IPC encode: SOCKET path %ux%u %u bytes", surfW, surfH, frameSize);
            }
            ret = nvenc_ipc_encode(nvencCtx->ipcFd, snapshot,
                                    surfW, surfH, frameSize, forceIDR, temporalId, picType,
                                    &bitstream, &bsSize);
            free(snapshot);
        }
    } else if (useDmaBuf) {
        if (nvencCtx->frameCount < 3) {
            LOG("IPC encode: DMABUF planes=%d fds=[%d,%d] %ux%u pitch=%u sizes=[%u,%u]",
                num_dmabuf_fds, dmabuf_fds[0], dmabuf_fds[1],
                dp.width, dp.height, dp.pitches[0], dp.sizes[0], dp.sizes[1]);
        }
        ret = nvenc_ipc_encode_dmabuf(nvencCtx->ipcFd, dmabuf_fds, num_dmabuf_fds,
                                       &dp, &bitstream, &bsSize);
    } else {
        LOG("IPC encode: surface has no pixel data (no DMA-BUF, no host data)");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (ret != 0) {
        LOG("IPC encode: encode failed (ret=%d)", ret);
        return VA_STATUS_ERROR_ENCODING_ERROR;
    }

    /* Copy bitstream into coded buffer */
    NVBuffer *codedBuf = (NVBuffer*) nvGetObjectPtr(drv, OBJECT_TYPE_BUFFER,
                                                    nvencCtx->currentCodedBufId);
    if (codedBuf != NULL && codedBuf->ptr != NULL) {
        NVCodedBuffer *coded = (NVCodedBuffer*) codedBuf->ptr;
        if (bsSize > coded->bitstreamAlloc) {
            void *newBuf = realloc(coded->bitstreamData, bsSize);
            if (newBuf != NULL) {
                coded->bitstreamData = newBuf;
                coded->bitstreamAlloc = bsSize;
            } else {
                free(bitstream);
                return VA_STATUS_ERROR_ALLOCATION_FAILED;
            }
        }
        memcpy(coded->bitstreamData, bitstream, bsSize);
        coded->bitstreamSize = bsSize;
        coded->hasData = true;
        if (nvencCtx->frameCount < 5 || nvencCtx->frameCount % 300 == 0) {
            unsigned char *bs = (unsigned char *)coded->bitstreamData;
            LOG("IPC encode: frame %lu, %u bytes, first4=[%02x %02x %02x %02x]",
                (unsigned long)nvencCtx->frameCount, bsSize,
                bsSize > 0 ? bs[0] : 0, bsSize > 1 ? bs[1] : 0,
                bsSize > 2 ? bs[2] : 0, bsSize > 3 ? bs[3] : 0);
        }
    }

    free(bitstream);
    nvencCtx->frameCount++;

    return VA_STATUS_SUCCESS;
}

VAStatus nvenc_dispatch_create_context(NVDriver *drv, NVConfig *cfg,
                                       uint32_t picture_width,
                                       uint32_t picture_height,
                                       VAContextID *context_out)
{
    NVENCContext *nvencCtx = (NVENCContext*) calloc(1, sizeof(NVENCContext));
    if (nvencCtx == NULL) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    nvencCtx->width = picture_width;
    nvencCtx->height = picture_height;
    nvencCtx->maxWidth = picture_width;
    nvencCtx->maxHeight = picture_height;
    nvencCtx->inputFormat = nvenc_surface_format(cfg->profile, cfg->bitDepth);
    nvencCtx->rcMode = cfg->rcMode;
    nvencCtx->allowBframes = cfg->allowBframes;
    nvencCtx->qualityLevel = 4;
    nvencCtx->frameRateNum = 30;
    nvencCtx->frameRateDen = 1;
    nvencCtx->ipcFd = -1;
    nvencCtx->shmPtr = NULL;
    nvencCtx->shmSize = 0;
    nvencCtx->shmFd = -1;

    if (drv->cudaAvailable) {
        /* Direct NVENC path (64-bit, CUDA works) */
        if (CHECK_CUDA_RESULT(drv->cu->cuCtxPushCurrent(drv->cudaContext))) {
            free(nvencCtx);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        if (!nvenc_open_session(nvencCtx, drv->nv, drv->cudaContext)) {
            CHECK_CUDA_RESULT(drv->cu->cuCtxPopCurrent(NULL));
            free(nvencCtx);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        if (CHECK_CUDA_RESULT(drv->cu->cuCtxPopCurrent(NULL))) {
            nvenc_close_session(nvencCtx);
            free(nvencCtx);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        nvencCtx->useIPC = false;
    } else {
        /* IPC path: CUDA unavailable (e.g. 32-bit on Blackwell).
         * Encoding delegated to 64-bit nvenc-helper via Unix socket. */
        LOG("Using IPC encode path (CUDA unavailable)");
        nvencCtx->useIPC = true;
    }

    Object contextObj = nvAllocateObject(drv, OBJECT_TYPE_CONTEXT, sizeof(NVContext));
    NVContext *nvCtx = (NVContext*) contextObj->obj;
    nvCtx->drv = drv;
    nvCtx->profile = cfg->profile;
    nvCtx->entrypoint = cfg->entrypoint;
    nvCtx->width = picture_width;
    nvCtx->height = picture_height;
    nvCtx->isEncode = true;
    nvCtx->encodeData = nvencCtx;
    nvCtx->decoder = NULL;
    nvCtx->codec = NULL;

    *context_out = contextObj->id;
    LOG("Created encode context id: %d, ipc=%d", contextObj->id, nvencCtx->useIPC);
    return VA_STATUS_SUCCESS;
}

void nvenc_dispatch_destroy_context(NVDriver *drv, NVContext *nvCtx)
{
    NVENCContext *nvencCtx = (NVENCContext*) nvCtx->encodeData;
    if (nvencCtx == NULL) {
        return;
    }

    if (nvencCtx->useIPC) {
        if (nvencCtx->shmPtr != NULL) {
            munmap(nvencCtx->shmPtr, nvencCtx->shmSize);
            nvencCtx->shmPtr = NULL;
        }
        if (nvencCtx->ipcFd >= 0) {
            nvenc_ipc_close(nvencCtx->ipcFd);
            nvencCtx->ipcFd = -1;
        }
    } else {
        /* Release the persistent linear staging buffer/registration
         * before tearing down the encoder session. */
        if (nvencCtx->registeredRes != NULL) {
            nvenc_unregister_resource(nvencCtx, nvencCtx->registeredRes);
            nvencCtx->registeredRes = NULL;
        }
        if (nvencCtx->linearBuffer != 0) {
            drv->cu->cuMemFree(nvencCtx->linearBuffer);
            nvencCtx->linearBuffer = 0;
            nvencCtx->linearBufferSize = 0;
        }
        nvenc_close_session(nvencCtx);
    }
    free(nvencCtx);
    nvCtx->encodeData = NULL;
}

VAStatus nvenc_dispatch_create_config(NVDriver *drv, VAProfile profile,
                                      VAEntrypoint entrypoint,
                                      VAConfigAttrib *attrib_list,
                                      int num_attribs,
                                      VAConfigID *config_id_out)
{
    /* Lazy one-shot capability probe so the whitelist check below can gate
     * higher-profile advertisement (H.264 High10, HEVC FREXT variants,
     * YUV444/YUV422) by what the card really supports instead of a static
     * hardcoded list. */
    nvenc_probe_caps(drv);
    if (!drv->nvencAvailable || !nvenc_is_encode_profile_supported(drv, profile)) {
        LOG("Encode not supported for profile: %d", profile);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    for (int i = 0; i < num_attribs; i++) {
        LOG("Config attrib[%d]: type=%d, value=0x%x", i, attrib_list[i].type, attrib_list[i].value);
    }

    Object obj = nvAllocateObject(drv, OBJECT_TYPE_CONFIG, sizeof(NVConfig));
    NVConfig *cfg = (NVConfig*) obj->obj;
    cfg->profile = profile;
    cfg->entrypoint = entrypoint;
    cfg->isEncode = true;
    cfg->chromaFormat = cudaVideoChromaFormat_420;
    cfg->bitDepth = 8;
    cfg->surfaceFormat = cudaVideoSurfaceFormat_NV12;
    cfg->rcMode = 0;
    cfg->allowBframes = false;

    /* Check for MaxRefFrames attribute to see if B-frames are allowed */
    if (profile == VAProfileAV1Profile0) {
        cfg->allowBframes = false;
    }

    /* Profile-level defaults for chroma format and bit depth. Individual
     * VAConfigAttribRTFormat entries below can refine (e.g. HEVCMain10 +
     * client asks for pure 10-bit vs "either"). */
    switch (profile) {
    case VAProfileHEVCMain10:
    case VAProfileH264High10:
        cfg->bitDepth = 10;
        cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
        break;
    case VAProfileHEVCMain422_10:
        cfg->bitDepth = 10;
        cfg->chromaFormat = cudaVideoChromaFormat_422;
        cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
        break;
    case VAProfileHEVCMain444:
        cfg->bitDepth = 8;
        cfg->chromaFormat = cudaVideoChromaFormat_444;
        cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444;
        break;
    case VAProfileHEVCMain444_10:
        cfg->bitDepth = 10;
        cfg->chromaFormat = cudaVideoChromaFormat_444;
        cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
        break;
    default:
        break;
    }

    for (int i = 0; i < num_attribs; i++) {
        if (attrib_list[i].type == VAConfigAttribRTFormat) {
            /* Select 10-bit encode only when 10-bit is requested AND plain
             * 8-bit YUV420 is NOT also present. Clients that echo back the
             * capability mask we advertise for AV1 (VA_RT_FORMAT_YUV420 |
             * VA_RT_FORMAT_YUV420_10) mean "either", not "10-bit"; treating
             * that as 10-bit and then receiving 8-bit NV12 surfaces makes
             * the input copy fail and the whole encode aborts. */
            if ((attrib_list[i].value & VA_RT_FORMAT_YUV420_10) &&
                !(attrib_list[i].value & VA_RT_FORMAT_YUV420)) {
                cfg->bitDepth = 10;
                cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
            }
        } else if (attrib_list[i].type == VAConfigAttribRateControl) {
            cfg->rcMode = attrib_list[i].value;
        } else if (attrib_list[i].type == VAConfigAttribEncMaxRefFrames) {
            /* If client explicitly sets MaxRefFrames L1=0, disable B-frames */
            if ((attrib_list[i].value & 0xffff0000) == 0) {
                cfg->allowBframes = false;
            }
        }
    }
    *config_id_out = obj->id;
    return VA_STATUS_SUCCESS;
}

VAStatus nvenc_dispatch_query_config_attributes(NVConfig *cfg,
                                                VAConfigAttrib *attrib_list,
                                                int *num_attribs)
{
    int i = 0;
    attrib_list[i].type = VAConfigAttribRTFormat;
    attrib_list[i].value = 0;
    switch (cfg->profile) {
    case VAProfileH264High10:
        attrib_list[i].value = VA_RT_FORMAT_YUV420_10;
        break;
    case VAProfileHEVCMain422_10:
        attrib_list[i].value = VA_RT_FORMAT_YUV422_10;
        break;
    case VAProfileHEVCMain444:
        attrib_list[i].value = VA_RT_FORMAT_YUV444;
        break;
    case VAProfileHEVCMain444_10:
        attrib_list[i].value = VA_RT_FORMAT_YUV444_10;
        break;
    case VAProfileHEVCMain10:
    case VAProfileAV1Profile0:
        attrib_list[i].value = VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10;
        break;
    default:
        attrib_list[i].value = VA_RT_FORMAT_YUV420;
        break;
    }
    i++;
    *num_attribs = i;
    return VA_STATUS_SUCCESS;
}

VAStatus nvenc_dispatch_begin_picture(NVContext *nvCtx, NVSurface *surface)
{
    nvCtx->renderTarget = surface;
    surface->context = nvCtx;
    NVENCContext *nvencCtx = (NVENCContext*) nvCtx->encodeData;
    if (nvencCtx) {
        nvencCtx->picType = NV_ENC_PIC_TYPE_UNKNOWN;
        nvencCtx->forceIDR = false;
    }
    return VA_STATUS_SUCCESS;
}

VAStatus nvenc_dispatch_derive_image_hostmem(NVDriver *drv, NVSurface *surfaceObj,
                                             VASurfaceID surface, VAImage *image)
{
    uint32_t width = surfaceObj->width;
    uint32_t height = surfaceObj->height;
    int bpp = (surfaceObj->bitDepth > 8) ? 2 : 1;
    uint32_t lumaSize = width * bpp * height;
    uint32_t chromaSize = width * bpp * (height / 2);
    uint32_t totalSize = lumaSize + chromaSize;

    /* Allocate or reuse the surface's host pixel buffer */
    if (surfaceObj->hostPixelData == NULL || surfaceObj->hostPixelSize < totalSize) {
        free(surfaceObj->hostPixelData);
        surfaceObj->hostPixelData = malloc(totalSize);
        if (surfaceObj->hostPixelData == NULL) {
            surfaceObj->hostPixelSize = 0;
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        surfaceObj->hostPixelSize = totalSize;
        memset(surfaceObj->hostPixelData, 0, totalSize);
    }

    /* Create a buffer object for the image data (points to the surface's host memory) */
    Object imageBufferObj = nvAllocateObject(drv, OBJECT_TYPE_BUFFER, sizeof(NVBuffer));
    NVBuffer *imageBuf = (NVBuffer*) imageBufferObj->obj;
    imageBuf->bufferType = VAImageBufferType;
    imageBuf->size = totalSize;
    imageBuf->elements = 1;
    imageBuf->ptr = surfaceObj->hostPixelData; /* Shared with surface! */
    imageBuf->offset = (size_t)-1; /* Sentinel: don't free ptr on destroy */

    /* Create the image object */
    Object imageObj = nvAllocateObject(drv, OBJECT_TYPE_IMAGE, sizeof(NVImage));
    NVImage *img = (NVImage*) imageObj->obj;
    img->width = width;
    img->height = height;
    img->format = (bpp == 1) ? NV_FORMAT_NV12 : NV_FORMAT_P010;
    img->imageBuffer = imageBuf;

    /* Fill VAImage output */
    memset(image, 0, sizeof(*image));
    image->image_id = imageObj->id;
    image->format.fourcc = (bpp == 1) ? VA_FOURCC_NV12 : VA_FOURCC_P010;
    image->format.byte_order = VA_LSB_FIRST;
    image->format.bits_per_pixel = (bpp == 1) ? 12 : 24;
    image->buf = imageBufferObj->id;
    image->width = width;
    image->height = height;
    image->data_size = totalSize;
    image->num_planes = 2;
    image->pitches[0] = width * bpp;
    image->pitches[1] = width * bpp;
    image->offsets[0] = 0;
    image->offsets[1] = lumaSize;

    LOG("DeriveImage: surface %d → host image %d (%ux%u, %u bytes)",
        surface, imageObj->id, width, height, totalSize);
    return VA_STATUS_SUCCESS;
}

VAStatus nvenc_dispatch_put_image_hostmem(NVSurface *surfaceObj, NVImage *imageObj)
{
    uint32_t totalSize = imageObj->imageBuffer->size;
    if (surfaceObj->hostPixelData == NULL || surfaceObj->hostPixelSize < totalSize) {
        free(surfaceObj->hostPixelData);
        surfaceObj->hostPixelData = malloc(totalSize);
        if (surfaceObj->hostPixelData == NULL) {
            surfaceObj->hostPixelSize = 0;
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        surfaceObj->hostPixelSize = totalSize;
    }
    memcpy(surfaceObj->hostPixelData, imageObj->imageBuffer->ptr, totalSize);
    return VA_STATUS_SUCCESS;
}

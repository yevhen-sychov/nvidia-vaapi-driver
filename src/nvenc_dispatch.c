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
        VAProfile profile,
        VAConfigAttrib *attrib_list,
        int num_attribs
    )
{
    for (int i = 0; i < num_attribs; i++)
    {
        switch (attrib_list[i].type) {
        case VAConfigAttribRTFormat:
            attrib_list[i].value = VA_RT_FORMAT_YUV420;
            if (profile == VAProfileHEVCMain10 || profile == VAProfileAV1Profile0) {
                attrib_list[i].value |= VA_RT_FORMAT_YUV420_10;
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
            //accept all packed header types; NVENC generates its own but
            //apps (Steam) expect the driver to accept them without warning
            attrib_list[i].value = VA_ENC_PACKED_HEADER_SEQUENCE
                                 | VA_ENC_PACKED_HEADER_PICTURE
                                 | VA_ENC_PACKED_HEADER_SLICE
                                 | VA_ENC_PACKED_HEADER_MISC;
            break;
        case VAConfigAttribEncMaxRefFrames:
            /* Disable B-frames for all codecs for now to ensure 1:1 mapping and no reordering issues.
             * FFmpeg VA-API sends frames in encode order, and with PTD=0 we must match that.
             * Supporting B-frames with manual PTD requires complex DPB management. */
            attrib_list[i].value = 1; /* 1 L0, 0 L1 */
            break;
        case VAConfigAttribMaxPictureWidth:
            attrib_list[i].value = 4096;
            break;
        case VAConfigAttribMaxPictureHeight:
            attrib_list[i].value = 4096;
            break;
        case VAConfigAttribEncQualityRange:
            attrib_list[i].value = 7; //NVENC presets P1-P7
            break;
        case VAConfigAttribEncHEVCFeatures:
            if (profile == VAProfileHEVCMain || profile == VAProfileHEVCMain10) {
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
            if (profile == VAProfileHEVCMain || profile == VAProfileHEVCMain10) {
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
                   nvCtx->profile == VAProfileH264High);
    bool isHEVC = (nvCtx->profile == VAProfileHEVCMain ||
                   nvCtx->profile == VAProfileHEVCMain10);
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
        /* Packed headers: NVENC generates its own headers, skip these */
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

    LOG("Encode: surface %ux%u (format %d, bitDepth %d), encoder %ux%u (format %d), copy %ux%u",
        surfWidth, surfHeight, img->format, surface->bitDepth,
        encWidth, encHeight, encFmt, copyWidth, copyHeight);

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
        LOG("Encode: frame %lu buffered (needs more input)",
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
        LOG("Encode: frame %lu encoded, %u bytes",
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

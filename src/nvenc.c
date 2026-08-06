#include "nvenc.h"
#include "vabackend.h"

#include <string.h>
#include <stdlib.h>

static bool check_nvenc_status(NVENCSTATUS status, const char *func, int line)
{
    if (status != NV_ENC_SUCCESS) {
        LOG("NVENC error %d at %s:%d", status, func, line);
        return false;
    }
    return true;
}
#define CHECK_NVENC(status) check_nvenc_status(status, __func__, __LINE__)

bool nvenc_load(NvencFunctions **nvenc_dl)
{
    int ret = nvenc_load_functions(nvenc_dl, NULL);
    if (ret != 0) {
        LOG("Failed to load NVENC functions (libnvidia-encode.so)");
        *nvenc_dl = NULL;
        return false;
    }
    //version format: API returns (major << 4 | minor)
    uint32_t maxVersion = 0;
    NVENCSTATUS st = (*nvenc_dl)->NvEncodeAPIGetMaxSupportedVersion(&maxVersion);
    if (st != NV_ENC_SUCCESS) {
        LOG("NvEncodeAPIGetMaxSupportedVersion failed: %d", st);
        nvenc_free_functions(nvenc_dl);
        *nvenc_dl = NULL;
        return false;
    }
    uint32_t currentVersion = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    LOG("NVENC max supported version: %u.%u, header version: %u.%u",
        maxVersion >> 4, maxVersion & 0xf,
        NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);

    if (currentVersion > maxVersion) {
        LOG("NVENC header version (%u) is newer than driver supports (%u)",
            currentVersion, maxVersion);
        nvenc_free_functions(nvenc_dl);
        *nvenc_dl = NULL;
        return false;
    }
    return true;
}

void nvenc_unload(NvencFunctions **nvenc_dl)
{
    if (*nvenc_dl != NULL) {
        nvenc_free_functions(nvenc_dl);
        *nvenc_dl = NULL;
    }
}

bool nvenc_open_session(NVENCContext *nvencCtx, NvencFunctions *nvenc_dl, CUcontext cudaCtx)
{
    /* Fill function list */
    nvencCtx->funcs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS st = nvenc_dl->NvEncodeAPICreateInstance(&nvencCtx->funcs);
    if (!CHECK_NVENC(st)) {
        return false;
    }

    /* Open encode session */
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = {0};
    sessionParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    sessionParams.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    sessionParams.device = cudaCtx;
    sessionParams.apiVersion = NVENCAPI_VERSION;

    st = nvencCtx->funcs.nvEncOpenEncodeSessionEx(&sessionParams, &nvencCtx->encoder);
    if (!CHECK_NVENC(st)) {
        nvencCtx->encoder = NULL;
        return false;
    }

    LOG("NVENC session opened: %p", nvencCtx->encoder);
    return true;
}

void nvenc_close_session(NVENCContext *nvencCtx)
{
    if (nvencCtx->encoder == NULL) {
        return;
    }

    /* Send EOS to flush encoder before freeing any buffers */
    if (nvencCtx->initialized) {
        NV_ENC_PIC_PARAMS picParams = {0};
        picParams.version = NV_ENC_PIC_PARAMS_VER;
        picParams.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        nvencCtx->funcs.nvEncEncodePicture(nvencCtx->encoder, &picParams);
    }

    /* Free output buffer after flush */
    nvenc_free_output_buffer(nvencCtx);

    /*
     * The persistent linear staging buffer/registered resource (if any) is
     * owned by CUDA device memory allocated in vabackend.c (which has access
     * to the CUDA function table) and is released by the caller via
     * nvenc_release_linear_buffer() before this function destroys the encoder.
     */

    /* Destroy encoder */
    NVENCSTATUS st = nvencCtx->funcs.nvEncDestroyEncoder(nvencCtx->encoder);
    if (st != NV_ENC_SUCCESS) {
        LOG("nvEncDestroyEncoder failed: %d", st);
    }

    LOG("NVENC session closed");
    nvencCtx->encoder = NULL;
    nvencCtx->initialized = false;
}

static GUID nvenc_get_preset_guid(uint32_t level) {
    switch (level) {
        case 1: return NV_ENC_PRESET_P1_GUID;
        case 2: return NV_ENC_PRESET_P2_GUID;
        case 3: return NV_ENC_PRESET_P3_GUID;
        case 4: return NV_ENC_PRESET_P4_GUID;
        case 5: return NV_ENC_PRESET_P5_GUID;
        case 6: return NV_ENC_PRESET_P6_GUID;
        case 7: return NV_ENC_PRESET_P7_GUID;
        default: return NV_ENC_PRESET_P4_GUID;
    }
}

bool nvenc_init_encoder(NVENCContext *nvencCtx, uint32_t width, uint32_t height,
                        GUID codecGuid, GUID profileGuid,
                        NV_ENC_TUNING_INFO tuningInfo)
{
    NVENCSTATUS st;
    GUID presetGuid = nvenc_get_preset_guid(nvencCtx->qualityLevel);

    nvencCtx->codecGuid = codecGuid;
    nvencCtx->profileGuid = profileGuid;
    nvencCtx->width = width;
    nvencCtx->height = height;

    //get preset config
    NV_ENC_PRESET_CONFIG presetConfig = {0};
    presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
    presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;

    st = nvencCtx->funcs.nvEncGetEncodePresetConfigEx(
        nvencCtx->encoder, codecGuid, presetGuid, tuningInfo, &presetConfig);
    if (!CHECK_NVENC(st)) {
        return false;
    }

    //apply overrides
    memcpy(&nvencCtx->encodeConfig, &presetConfig.presetCfg, sizeof(NV_ENC_CONFIG));
    nvencCtx->encodeConfig.version = NV_ENC_CONFIG_VER;
    nvencCtx->encodeConfig.profileGUID = profileGuid;

    if (memcmp(&codecGuid, &NV_ENC_CODEC_HEVC_GUID, sizeof(GUID)) == 0) {
        NV_ENC_CONFIG_HEVC *hevc = &nvencCtx->encodeConfig.encodeCodecConfig.hevcConfig;
        /* Bit depth: 10-bit for any format whose input buffer is 10-bit
         * (P010 for 4:2:0, P210 for 4:2:2, YUV444_10 for 4:4:4). */
        bool is10 = (nvencCtx->inputFormat == NV_ENC_BUFFER_FORMAT_YUV420_10BIT ||
                     nvencCtx->inputFormat == NV_ENC_BUFFER_FORMAT_P210 ||
                     nvencCtx->inputFormat == NV_ENC_BUFFER_FORMAT_YUV444_10BIT);
        hevc->inputBitDepth  = is10 ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
        hevc->outputBitDepth = is10 ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
        /* Chroma format: NV12/P010 → 4:2:0 (IDC=1), NV16/P210 → 4:2:2
         * (IDC=2), YUV444/YUV444_10 → 4:4:4 (IDC=3). The FREXT profile
         * GUID accepts any of 2/3; Main/Main10 GUIDs must stay on IDC=1. */
        switch (nvencCtx->inputFormat) {
        case NV_ENC_BUFFER_FORMAT_NV16:
        case NV_ENC_BUFFER_FORMAT_P210:
            hevc->chromaFormatIDC = 2;
            break;
        case NV_ENC_BUFFER_FORMAT_YUV444:
        case NV_ENC_BUFFER_FORMAT_YUV444_10BIT:
            hevc->chromaFormatIDC = 3;
            break;
        default:
            hevc->chromaFormatIDC = 1;
            break;
        }
    }

    if (memcmp(&codecGuid, &NV_ENC_CODEC_H264_GUID, sizeof(GUID)) == 0) {
        /* H.264 High10 encode uses P010 input; NV_ENC_CONFIG_H264 doesn't
         * have separate bit-depth fields (the profile GUID carries it),
         * but we still pin chromaFormatIDC=1 explicitly for clarity. */
        NV_ENC_CONFIG_H264 *h264 = &nvencCtx->encodeConfig.encodeCodecConfig.h264Config;
        h264->chromaFormatIDC = 1;
    }

    if (memcmp(&codecGuid, &NV_ENC_CODEC_AV1_GUID, sizeof(GUID)) == 0) {
        NV_ENC_CONFIG_AV1 *av1 = &nvencCtx->encodeConfig.encodeCodecConfig.av1Config;
        if (nvencCtx->inputFormat == NV_ENC_BUFFER_FORMAT_YUV420_10BIT) {
            av1->inputBitDepth = NV_ENC_BIT_DEPTH_10;
            av1->outputBitDepth = NV_ENC_BIT_DEPTH_10;
        } else {
            av1->inputBitDepth = NV_ENC_BIT_DEPTH_8;
            av1->outputBitDepth = NV_ENC_BIT_DEPTH_8;
        }
        /* AV1 chroma format. The NV_ENC_CONFIG_AV1.chromaFormatIDC field is
         * bit-packed and defaults to 0 when the preset config is zeroed —
         * but NVENC's own docs say it *must* be set to 1 for 4:2:0 input
         * (YUV444 is not currently supported for AV1 encode). Without this
         * explicit pin the encoder emits a stream whose chroma-plane layout
         * doesn't match the actual NV12 input; decoders then render a
         * saturated pink/magenta blob because chroma is sampled with the
         * wrong stride, plus a green tail where later frames drift from
         * corrupted references. Reproduced in a WebRTC loopback at
         * 3824x1792; H.264 at the same resolution stayed clean because its
         * h264->chromaFormatIDC is also pinned to 1 above. */
        av1->chromaFormatIDC = 1;
        /* WebRTC (and most desktop / Chrome content) transports full-swing
         * YUV [0..255] with BT.709 primaries + matrix. NVENC's default of
         * colorRange=0 (studio swing 16..235) + unset color* fields
         * produces washed-out / off-hue output when Chrome's decoder
         * assumes BT.709 full-range. Setting these lands the correct VUI
         * in the encoded sequence header so decoders reconstruct colors
         * consistently across sender ↔ receiver. */
        av1->colorRange = 1;
        av1->colorPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
        av1->transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
        av1->matrixCoefficients = NV_ENC_VUI_MATRIX_COEFFS_BT709;
        av1->repeatSeqHdr = 1;
        av1->idrPeriod = nvencCtx->intraPeriod > 0 ? nvencCtx->intraPeriod : 0xffffffff;
        av1->maxNumRefFramesInDPB = 8;

        /* Temporal SVC (e.g. WebRTC L1T2/L1T3 screenshare). Only enable when the
         * application actually requested more than one temporal layer, otherwise
         * a plain single-layer stream is produced. */
        if (nvencCtx->numTemporalLayers > 1) {
            uint32_t layers = nvencCtx->numTemporalLayers;
            if (layers > 4) {
                layers = 4; /* NVENC AV1 supports up to 4 temporal layers */
            }
            av1->enableTemporalSVC = 1;
            av1->numTemporalLayers = layers;
            av1->maxTemporalLayersMinus1 = layers - 1;
            LOG("NVENC: AV1 temporal SVC enabled, layers=%u", layers);
        } else {
            av1->enableTemporalSVC = 0;
            av1->numTemporalLayers = 0;
            av1->maxTemporalLayersMinus1 = 0;
        }
    }

    if (nvencCtx->rcMode != 0) {
        if (nvencCtx->rcMode & VA_RC_CQP) {
            nvencCtx->encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
        } else if (nvencCtx->rcMode & VA_RC_CBR) {
            nvencCtx->encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        } else if (nvencCtx->rcMode & (VA_RC_VBR | VA_RC_VBR_CONSTRAINED)) {
            nvencCtx->encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
        }
    }
    if (nvencCtx->bitrate > 0) {
        nvencCtx->encodeConfig.rcParams.averageBitRate = nvencCtx->bitrate;
    }
    if (nvencCtx->maxBitrate > 0) {
        nvencCtx->encodeConfig.rcParams.maxBitRate = nvencCtx->maxBitrate;
    }
    if (nvencCtx->vbvBufferSize > 0) {
        nvencCtx->encodeConfig.rcParams.vbvBufferSize = nvencCtx->vbvBufferSize;
    }
    if (nvencCtx->vbvInitialDelay > 0) {
        nvencCtx->encodeConfig.rcParams.vbvInitialDelay = nvencCtx->vbvInitialDelay;
    }

    /* QP wiring. For CONSTQP: initial_qp (or per-picture qp when set later)
     * becomes the fixed QP applied to all frame types. For CBR/VBR: min/max
     * QP bound the encoder's adaptive QP range. Zero means "unset" — keep
     * NVENC's default behavior. */
    if (nvencCtx->encodeConfig.rcParams.rateControlMode == NV_ENC_PARAMS_RC_CONSTQP) {
        uint32_t qp = nvencCtx->picQP > 0 ? nvencCtx->picQP :
                      (nvencCtx->initialQP > 0 ? nvencCtx->initialQP : 0);
        if (qp > 0) {
            nvencCtx->encodeConfig.rcParams.constQP.qpInterP = qp;
            nvencCtx->encodeConfig.rcParams.constQP.qpInterB = qp;
            nvencCtx->encodeConfig.rcParams.constQP.qpIntra  = qp;
        }
    }
    if (nvencCtx->minQP > 0) {
        nvencCtx->encodeConfig.rcParams.enableMinQP = 1;
        nvencCtx->encodeConfig.rcParams.minQP.qpInterP = nvencCtx->minQP;
        nvencCtx->encodeConfig.rcParams.minQP.qpInterB = nvencCtx->minQP;
        nvencCtx->encodeConfig.rcParams.minQP.qpIntra  = nvencCtx->minQP;
    }
    if (nvencCtx->maxQP > 0) {
        nvencCtx->encodeConfig.rcParams.enableMaxQP = 1;
        nvencCtx->encodeConfig.rcParams.maxQP.qpInterP = nvencCtx->maxQP;
        nvencCtx->encodeConfig.rcParams.maxQP.qpInterB = nvencCtx->maxQP;
        nvencCtx->encodeConfig.rcParams.maxQP.qpIntra  = nvencCtx->maxQP;
    }
    /* enableInitialRCQP for CBR/VBR seeding — NVENC accepts a per-frame-type
     * initial QP hint distinct from constQP. */
    if (nvencCtx->initialQP > 0 &&
        nvencCtx->encodeConfig.rcParams.rateControlMode != NV_ENC_PARAMS_RC_CONSTQP) {
        nvencCtx->encodeConfig.rcParams.enableInitialRCQP = 1;
        nvencCtx->encodeConfig.rcParams.initialRCQP.qpInterP = nvencCtx->initialQP;
        nvencCtx->encodeConfig.rcParams.initialRCQP.qpInterB = nvencCtx->initialQP;
        nvencCtx->encodeConfig.rcParams.initialRCQP.qpIntra  = nvencCtx->initialQP;
    }

    if (nvencCtx->intraPeriod > 0) {
        nvencCtx->encodeConfig.gopLength = nvencCtx->intraPeriod;
    } else {
        nvencCtx->encodeConfig.gopLength = 0xffffffff; /* Infinite GOP per VA-API default */
    }
    if (nvencCtx->allowBframes) {
        nvencCtx->encodeConfig.frameIntervalP = (nvencCtx->ipPeriod > 0) ? nvencCtx->ipPeriod : 3;
    } else {
        nvencCtx->encodeConfig.frameIntervalP = 1;
    }

    memset(&nvencCtx->initParams, 0, sizeof(nvencCtx->initParams));
    nvencCtx->initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
    nvencCtx->initParams.encodeGUID = codecGuid;
    nvencCtx->initParams.presetGUID = presetGuid;
    nvencCtx->initParams.encodeWidth = width;
    nvencCtx->initParams.encodeHeight = height;
    nvencCtx->initParams.darWidth = width;
    nvencCtx->initParams.darHeight = height;
    nvencCtx->initParams.frameRateNum = nvencCtx->frameRateNum > 0 ? nvencCtx->frameRateNum : 30;
    nvencCtx->initParams.frameRateDen = nvencCtx->frameRateDen > 0 ? nvencCtx->frameRateDen : 1;
    nvencCtx->initParams.enablePTD = 0;
    nvencCtx->initParams.encodeConfig = &nvencCtx->encodeConfig;
    nvencCtx->initParams.maxEncodeWidth = nvencCtx->maxWidth;
    nvencCtx->initParams.maxEncodeHeight = nvencCtx->maxHeight;
    nvencCtx->initParams.tuningInfo = tuningInfo;

    LOG("NVENC: initializing %ux%u (max %ux%u), rcMode=0x%x, bitrate=%u, maxBitrate=%u, gop=%u, frameRate=%u/%u",
        width, height, nvencCtx->initParams.maxEncodeWidth, nvencCtx->initParams.maxEncodeHeight,
        nvencCtx->rcMode, nvencCtx->bitrate, nvencCtx->maxBitrate,
        nvencCtx->encodeConfig.gopLength, nvencCtx->initParams.frameRateNum, nvencCtx->initParams.frameRateDen);

    st = nvencCtx->funcs.nvEncInitializeEncoder(nvencCtx->encoder, &nvencCtx->initParams);
    if (!CHECK_NVENC(st)) {
        return false;
    }

    nvencCtx->initialized = true;
    nvencCtx->appliedBitrate = nvencCtx->encodeConfig.rcParams.averageBitRate;
    nvencCtx->appliedMaxBitrate = nvencCtx->encodeConfig.rcParams.maxBitRate;
    nvencCtx->appliedFrameRateNum = nvencCtx->initParams.frameRateNum;
    nvencCtx->appliedFrameRateDen = nvencCtx->initParams.frameRateDen;
    nvencCtx->appliedConstQP = nvencCtx->encodeConfig.rcParams.constQP.qpIntra;
    LOG("NVENC encoder initialized: %ux%u codec=%s",
        width, height,
        memcmp(&codecGuid, &NV_ENC_CODEC_H264_GUID, sizeof(GUID)) == 0 ? "H.264" :
        (memcmp(&codecGuid, &NV_ENC_CODEC_HEVC_GUID, sizeof(GUID)) == 0 ? "HEVC" : "AV1"));

    return true;
}

bool nvenc_reconfigure_if_needed(NVENCContext *nvencCtx)
{
    if (!nvencCtx->initialized || nvencCtx->encoder == NULL) {
        return true;
    }

    const uint32_t reqBitrate = nvencCtx->bitrate;
    const uint32_t reqMaxBitrate = nvencCtx->maxBitrate > 0 ? nvencCtx->maxBitrate : reqBitrate;
    const uint32_t reqFrameRateNum = nvencCtx->frameRateNum > 0 ? nvencCtx->frameRateNum : nvencCtx->appliedFrameRateNum;
    const uint32_t reqFrameRateDen = nvencCtx->frameRateDen > 0 ? nvencCtx->frameRateDen : nvencCtx->appliedFrameRateDen;

    const bool bitrateChanged = reqBitrate > 0 && reqBitrate != nvencCtx->appliedBitrate;
    const bool maxBitrateChanged = reqMaxBitrate > 0 && reqMaxBitrate != nvencCtx->appliedMaxBitrate;
    const bool frameRateChanged =
        reqFrameRateNum != nvencCtx->appliedFrameRateNum ||
        reqFrameRateDen != nvencCtx->appliedFrameRateDen;

    /* Per-picture QP override — only meaningful in CONSTQP mode; other modes
     * use min/max/initial as bounds and NVENC picks per-frame QP itself. */
    const bool constQPActive = nvencCtx->encodeConfig.rcParams.rateControlMode
                                    == NV_ENC_PARAMS_RC_CONSTQP;
    const uint32_t reqConstQP = nvencCtx->picQP > 0 ? nvencCtx->picQP :
                                 (nvencCtx->initialQP > 0 ? nvencCtx->initialQP : 0);
    const bool constQPChanged = constQPActive && reqConstQP > 0 &&
                                 reqConstQP != nvencCtx->appliedConstQP;

    if (!bitrateChanged && !maxBitrateChanged && !frameRateChanged && !constQPChanged) {
        return true;
    }

    if (bitrateChanged) {
        nvencCtx->encodeConfig.rcParams.averageBitRate = reqBitrate;
    }
    if (maxBitrateChanged) {
        nvencCtx->encodeConfig.rcParams.maxBitRate = reqMaxBitrate;
    }
    if (frameRateChanged) {
        nvencCtx->initParams.frameRateNum = reqFrameRateNum;
        nvencCtx->initParams.frameRateDen = reqFrameRateDen;
    }
    if (constQPChanged) {
        nvencCtx->encodeConfig.rcParams.constQP.qpInterP = reqConstQP;
        nvencCtx->encodeConfig.rcParams.constQP.qpInterB = reqConstQP;
        nvencCtx->encodeConfig.rcParams.constQP.qpIntra  = reqConstQP;
    }

    NV_ENC_RECONFIGURE_PARAMS reconf = {0};
    reconf.version = NV_ENC_RECONFIGURE_PARAMS_VER;
    reconf.reInitEncodeParams = nvencCtx->initParams;
    reconf.reInitEncodeParams.encodeConfig = &nvencCtx->encodeConfig;
    reconf.resetEncoder = 0;
    reconf.forceIDR = 0;

    NVENCSTATUS st = nvencCtx->funcs.nvEncReconfigureEncoder(nvencCtx->encoder, &reconf);
    if (!CHECK_NVENC(st)) {
        LOG("NVENC: reconfigure failed (bitrate=%u max=%u fps=%u/%u), keeping previous config",
            reqBitrate, reqMaxBitrate, reqFrameRateNum, reqFrameRateDen);
        return false;
    }

    LOG("NVENC: reconfigured bitrate=%u->%u max=%u->%u fps=%u/%u->%u/%u constQP=%u->%u",
        nvencCtx->appliedBitrate, reqBitrate,
        nvencCtx->appliedMaxBitrate, reqMaxBitrate,
        nvencCtx->appliedFrameRateNum, nvencCtx->appliedFrameRateDen,
        reqFrameRateNum, reqFrameRateDen,
        nvencCtx->appliedConstQP, constQPChanged ? reqConstQP : nvencCtx->appliedConstQP);

    nvencCtx->appliedBitrate = reqBitrate;
    nvencCtx->appliedMaxBitrate = reqMaxBitrate;
    nvencCtx->appliedFrameRateNum = reqFrameRateNum;
    nvencCtx->appliedFrameRateDen = reqFrameRateDen;
    if (constQPChanged) nvencCtx->appliedConstQP = reqConstQP;
    return true;
}

bool nvenc_alloc_output_buffer(NVENCContext *nvencCtx)
{
    if (nvencCtx->outputBuffer.allocated) {
        return true;
    }

    NV_ENC_CREATE_BITSTREAM_BUFFER createBuf = {0};
    createBuf.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;

    NVENCSTATUS st = nvencCtx->funcs.nvEncCreateBitstreamBuffer(
        nvencCtx->encoder, &createBuf);
    if (!CHECK_NVENC(st)) {
        return false;
    }

    nvencCtx->outputBuffer.bitstreamBuffer = createBuf.bitstreamBuffer;
    nvencCtx->outputBuffer.allocated = true;
    nvencCtx->outputBuffer.locked = false;
    nvencCtx->outputBuffer.lockedPtr = NULL;
    nvencCtx->outputBuffer.lockedSize = 0;

    return true;
}

void nvenc_free_output_buffer(NVENCContext *nvencCtx)
{
    if (!nvencCtx->outputBuffer.allocated || nvencCtx->encoder == NULL) {
        return;
    }

    /* Unlock if still locked */
    if (nvencCtx->outputBuffer.locked) {
        nvenc_unlock_bitstream(nvencCtx);
    }

    nvencCtx->funcs.nvEncDestroyBitstreamBuffer(
        nvencCtx->encoder, nvencCtx->outputBuffer.bitstreamBuffer);
    nvencCtx->outputBuffer.bitstreamBuffer = NULL;
    nvencCtx->outputBuffer.allocated = false;
}

bool nvenc_register_cuda_resource(NVENCContext *nvencCtx, CUdeviceptr devPtr,
                                  uint32_t width, uint32_t height, uint32_t pitch,
                                  NV_ENC_BUFFER_FORMAT format,
                                  NV_ENC_REGISTERED_PTR *outRegistered)
{
    NV_ENC_REGISTER_RESOURCE regRes = {0};
    regRes.version = NV_ENC_REGISTER_RESOURCE_VER;
    regRes.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
    regRes.resourceToRegister = (void*)devPtr;
    regRes.width = width;
    regRes.height = height;
    regRes.pitch = pitch;
    regRes.bufferFormat = format;
    regRes.bufferUsage = NV_ENC_INPUT_IMAGE;

    NVENCSTATUS st = nvencCtx->funcs.nvEncRegisterResource(
        nvencCtx->encoder, &regRes);
    if (!CHECK_NVENC(st)) {
        return false;
    }

    *outRegistered = regRes.registeredResource;
    return true;
}

bool nvenc_map_resource(NVENCContext *nvencCtx, NV_ENC_REGISTERED_PTR registered,
                        NV_ENC_INPUT_PTR *outMapped, NV_ENC_BUFFER_FORMAT *outFmt)
{
    NV_ENC_MAP_INPUT_RESOURCE mapRes = {0};
    mapRes.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mapRes.registeredResource = registered;

    NVENCSTATUS st = nvencCtx->funcs.nvEncMapInputResource(
        nvencCtx->encoder, &mapRes);
    if (!CHECK_NVENC(st)) {
        return false;
    }

    *outMapped = mapRes.mappedResource;
    if (outFmt) {
        *outFmt = mapRes.mappedBufferFmt;
    }
    return true;
}

bool nvenc_unmap_resource(NVENCContext *nvencCtx, NV_ENC_INPUT_PTR mapped)
{
    NVENCSTATUS st = nvencCtx->funcs.nvEncUnmapInputResource(
        nvencCtx->encoder, mapped);
    return CHECK_NVENC(st);
}

bool nvenc_unregister_resource(NVENCContext *nvencCtx, NV_ENC_REGISTERED_PTR registered)
{
    NVENCSTATUS st = nvencCtx->funcs.nvEncUnregisterResource(
        nvencCtx->encoder, registered);
    return CHECK_NVENC(st);
}

/*
 * Encode a frame. Returns:
 *  1 = encoded successfully, output available
 *  0 = needs more input (B-frame buffering), no output yet
 * -1 = error
 */
int nvenc_encode_frame(NVENCContext *nvencCtx, NV_ENC_INPUT_PTR inputBuffer,
                       NV_ENC_BUFFER_FORMAT bufferFmt,
                       uint32_t inputWidth, uint32_t inputHeight, uint32_t inputPitch,
                       NV_ENC_PIC_TYPE picType, uint32_t picFlags)
{
    if (!nvencCtx->outputBuffer.allocated) {
        if (!nvenc_alloc_output_buffer(nvencCtx)) {
            return -1;
        }
    }

    NV_ENC_PIC_PARAMS picParams = {0};
    picParams.version = NV_ENC_PIC_PARAMS_VER;
    picParams.inputBuffer = inputBuffer;
    picParams.bufferFmt = bufferFmt;
    picParams.inputWidth = inputWidth;
    picParams.inputHeight = inputHeight;
    picParams.inputPitch = inputPitch;
    picParams.outputBitstream = nvencCtx->outputBuffer.bitstreamBuffer;
    picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picParams.pictureType = picType;
    if (nvencCtx->frameCount == 0 || (picFlags & NV_ENC_PIC_FLAG_FORCEIDR)) {
        picParams.pictureType = NV_ENC_PIC_TYPE_IDR;
    } else if (picParams.pictureType == NV_ENC_PIC_TYPE_UNKNOWN) {
        picParams.pictureType = NV_ENC_PIC_TYPE_P;
    }
    picParams.encodePicFlags = picFlags;
    picParams.frameIdx = (uint32_t)nvencCtx->frameCount;
    picParams.inputTimeStamp = nvencCtx->frameCount;

    if (memcmp(&nvencCtx->codecGuid, &NV_ENC_CODEC_H264_GUID, sizeof(GUID)) == 0) {
        picParams.codecPicParams.h264PicParams.displayPOCSyntax = (uint32_t)(nvencCtx->frameCount * 2);
        picParams.codecPicParams.h264PicParams.refPicFlag = 1;
    } else if (memcmp(&nvencCtx->codecGuid, &NV_ENC_CODEC_HEVC_GUID, sizeof(GUID)) == 0) {
        picParams.codecPicParams.hevcPicParams.displayPOCSyntax = (uint32_t)(nvencCtx->frameCount * 2);
        picParams.codecPicParams.hevcPicParams.refPicFlag = 1;
        picParams.codecPicParams.hevcPicParams.temporalId = nvencCtx->temporalId;
    } else if (memcmp(&nvencCtx->codecGuid, &NV_ENC_CODEC_AV1_GUID, sizeof(GUID)) == 0) {
        picParams.codecPicParams.av1PicParams.displayPOCSyntax = (uint32_t)(nvencCtx->frameCount * 2);
        picParams.codecPicParams.av1PicParams.refPicFlag = 1;
        picParams.codecPicParams.av1PicParams.temporalId = nvencCtx->temporalId;
        if (nvencCtx->frameCount < 5) {
            LOG("NVENC: AV1 encode frame %lu, temporalId=%u", (unsigned long)nvencCtx->frameCount, nvencCtx->temporalId);
        }
    }

    NVENCSTATUS st = nvencCtx->funcs.nvEncEncodePicture(
        nvencCtx->encoder, &picParams);

    nvencCtx->frameCount++;

    if (st == NV_ENC_ERR_NEED_MORE_INPUT) {
        /* B-frame reordering: NVENC needs more frames before producing output */
        return 0;
    }
    if (st != NV_ENC_SUCCESS) {
        LOG("nvEncEncodePicture failed: %d", st);
        return -1;
    }

    return 1;
}

bool nvenc_lock_bitstream(NVENCContext *nvencCtx, void **outPtr, uint32_t *outSize)
{
    NV_ENC_LOCK_BITSTREAM lockParams = {0};
    lockParams.version = NV_ENC_LOCK_BITSTREAM_VER;
    lockParams.outputBitstream = nvencCtx->outputBuffer.bitstreamBuffer;
    lockParams.doNotWait = 0;

    NVENCSTATUS st = nvencCtx->funcs.nvEncLockBitstream(
        nvencCtx->encoder, &lockParams);
    if (!CHECK_NVENC(st)) {
        return false;
    }

    *outPtr = lockParams.bitstreamBufferPtr;
    *outSize = lockParams.bitstreamSizeInBytes;
    nvencCtx->outputBuffer.locked = true;
    nvencCtx->outputBuffer.lockedPtr = lockParams.bitstreamBufferPtr;
    nvencCtx->outputBuffer.lockedSize = lockParams.bitstreamSizeInBytes;

    return true;
}

bool nvenc_unlock_bitstream(NVENCContext *nvencCtx)
{
    if (!nvencCtx->outputBuffer.locked) {
        return true;
    }

    NVENCSTATUS st = nvencCtx->funcs.nvEncUnlockBitstream(
        nvencCtx->encoder, nvencCtx->outputBuffer.bitstreamBuffer);
    nvencCtx->outputBuffer.locked = false;
    nvencCtx->outputBuffer.lockedPtr = NULL;
    nvencCtx->outputBuffer.lockedSize = 0;

    return CHECK_NVENC(st);
}

/* Profile/entrypoint helpers */

bool nvenc_is_encode_profile(VAProfile profile)
{
    switch (profile) {
    case VAProfileH264ConstrainedBaseline:
    case VAProfileH264Main:
    case VAProfileH264High:
    case VAProfileH264High10:
    case VAProfileHEVCMain:
    case VAProfileHEVCMain10:
    case VAProfileHEVCMain422_10:
    case VAProfileHEVCMain444:
    case VAProfileHEVCMain444_10:
    case VAProfileAV1Profile0:
        return true;
    default:
        return false;
    }
}

bool nvenc_is_encode_profile_supported(NVDriver *drv, VAProfile profile)
{
    if (!nvenc_is_encode_profile(profile)) return false;

    /* No probe results yet (CUDA-less / IPC-only build, or probe hasn't
     * run) — fall back to the fork's historical hardcoded list. This is
     * the pre-caps behavior for the base six profiles. */
    if (!drv->nvencCapsProbed) {
        return profile == VAProfileH264ConstrainedBaseline ||
               profile == VAProfileH264Main ||
               profile == VAProfileH264High ||
               profile == VAProfileHEVCMain ||
               profile == VAProfileHEVCMain10 ||
               profile == VAProfileAV1Profile0;
    }

    switch (profile) {
    case VAProfileH264ConstrainedBaseline:
    case VAProfileH264Main:
    case VAProfileH264High:
        return drv->nvencSupportsH264;
    case VAProfileH264High10:
        return drv->nvencSupportsH264 && drv->nvencSupportsH264High10;
    case VAProfileHEVCMain:
        return drv->nvencSupportsHEVC;
    case VAProfileHEVCMain10:
        return drv->nvencSupportsHEVC && drv->nvencSupportsHEVCMain10;
    case VAProfileHEVCMain422_10:
        /* NVENC advertises the FREXT umbrella profile and NV16 or P210
         * input. Require both — a card that has FREXT but no P210 input
         * can't accept 10-bit 4:2:2 pixels. */
        return drv->nvencSupportsHEVC && drv->nvencSupportsHEVCFrext &&
               drv->nvencSupportsInputYUV422_10;
    case VAProfileHEVCMain444:
        return drv->nvencSupportsHEVC && drv->nvencSupportsHEVCFrext &&
               drv->nvencSupportsInputYUV444;
    case VAProfileHEVCMain444_10:
        return drv->nvencSupportsHEVC && drv->nvencSupportsHEVCFrext &&
               drv->nvencSupportsInputYUV444_10;
    case VAProfileAV1Profile0:
        return drv->nvencSupportsAV1;
    default:
        return false;
    }
}

GUID nvenc_va_profile_to_codec_guid(VAProfile profile)
{
    switch (profile) {
    case VAProfileH264ConstrainedBaseline:
    case VAProfileH264Main:
    case VAProfileH264High:
    case VAProfileH264High10:
        return NV_ENC_CODEC_H264_GUID;
    case VAProfileHEVCMain:
    case VAProfileHEVCMain10:
    case VAProfileHEVCMain422_10:
    case VAProfileHEVCMain444:
    case VAProfileHEVCMain444_10:
        return NV_ENC_CODEC_HEVC_GUID;
    case VAProfileAV1Profile0:
        return NV_ENC_CODEC_AV1_GUID;
    default: {
        GUID empty = {0};
        return empty;
    }
    }
}

GUID nvenc_va_profile_to_profile_guid(VAProfile profile)
{
    switch (profile) {
    case VAProfileH264ConstrainedBaseline:
        return NV_ENC_H264_PROFILE_BASELINE_GUID;
    case VAProfileH264Main:
        return NV_ENC_H264_PROFILE_MAIN_GUID;
    case VAProfileH264High:
        return NV_ENC_H264_PROFILE_HIGH_GUID;
    case VAProfileH264High10:
        return NV_ENC_H264_PROFILE_HIGH_10_GUID;
    case VAProfileHEVCMain:
        return NV_ENC_HEVC_PROFILE_MAIN_GUID;
    case VAProfileHEVCMain10:
        return NV_ENC_HEVC_PROFILE_MAIN10_GUID;
    case VAProfileHEVCMain422_10:
    case VAProfileHEVCMain444:
    case VAProfileHEVCMain444_10:
        /* NVENC exposes a single FREXT profile GUID for all HEVC range-
         * extension flavors; the actual chroma/bit-depth combination is
         * selected via NV_ENC_CONFIG_HEVC.{chromaFormatIDC, pixelBitDepth}
         * and matched to the input buffer format. */
        return NV_ENC_HEVC_PROFILE_FREXT_GUID;
    case VAProfileAV1Profile0:
        return NV_ENC_AV1_PROFILE_MAIN_GUID;
    default: {
        GUID empty = {0};
        return empty;
    }
    }
}

NvencChromaFormat nvenc_profile_chroma(VAProfile profile)
{
    switch (profile) {
    case VAProfileHEVCMain422_10:
        return NVENC_CHROMA_422;
    case VAProfileHEVCMain444:
    case VAProfileHEVCMain444_10:
        return NVENC_CHROMA_444;
    default:
        return NVENC_CHROMA_420;
    }
}

NV_ENC_BUFFER_FORMAT nvenc_surface_format(VAProfile profile, int bitDepth)
{
    switch (profile) {
    case VAProfileHEVCMain422_10:
        return NV_ENC_BUFFER_FORMAT_P210;
    case VAProfileHEVCMain444:
        return NV_ENC_BUFFER_FORMAT_YUV444;
    case VAProfileHEVCMain444_10:
        return NV_ENC_BUFFER_FORMAT_YUV444_10BIT;
    case VAProfileH264High10:
        return NV_ENC_BUFFER_FORMAT_YUV420_10BIT;
    default:
        break;
    }
    if (bitDepth == 10) {
        return NV_ENC_BUFFER_FORMAT_YUV420_10BIT;
    }
    return NV_ENC_BUFFER_FORMAT_NV12;
}

/* ---------------- Capability probe ----------------
 *
 * Opens a scratch NVENC session on drv->cudaContext, walks the encoder's
 * exported GUIDs / profile GUIDs / input format list / per-codec caps,
 * and records the results on drv. Called once, lazily, on the first
 * config-side encode query — cost is one session-open+destroy per driver
 * instance. */

static bool guid_eq(const GUID *a, const GUID *b) {
    return memcmp(a, b, sizeof(GUID)) == 0;
}

static bool nvenc_query_cap(NV_ENCODE_API_FUNCTION_LIST *funcs,
                             void *encoder, GUID codecGuid,
                             NV_ENC_CAPS capsToQuery, int *out) {
    NV_ENC_CAPS_PARAM param = { .version = NV_ENC_CAPS_PARAM_VER,
                                 .capsToQuery = capsToQuery };
    return funcs->nvEncGetEncodeCaps(encoder, codecGuid, &param, out) == NV_ENC_SUCCESS;
}

bool nvenc_probe_caps(NVDriver *drv)
{
    if (drv->nvencCapsProbed) return true;
    if (!drv->cudaAvailable || drv->cudaContext == NULL) {
        LOG("NVENC caps probe skipped: no CUDA context (IPC-only build?)");
        drv->nvencCapsProbed = true;  /* don't retry */
        return false;
    }
    if (drv->nv == NULL) {
        LOG("NVENC caps probe skipped: nvenc library not loaded");
        drv->nvencCapsProbed = true;
        return false;
    }

    NVENCContext tmp = {0};
    if (!nvenc_open_session(&tmp, drv->nv, drv->cudaContext)) {
        LOG("NVENC caps probe: could not open scratch session");
        drv->nvencCapsProbed = true;
        return false;
    }

    /* 1. Codec GUIDs the driver supports at all. */
    uint32_t codecCount = 0;
    if (tmp.funcs.nvEncGetEncodeGUIDCount(tmp.encoder, &codecCount) != NV_ENC_SUCCESS ||
        codecCount == 0) {
        LOG("NVENC caps probe: no codec GUIDs advertised");
        goto done;
    }
    GUID *codecs = calloc(codecCount, sizeof(GUID));
    if (!codecs) goto done;
    uint32_t codecFilled = 0;
    tmp.funcs.nvEncGetEncodeGUIDs(tmp.encoder, codecs, codecCount, &codecFilled);
    for (uint32_t i = 0; i < codecFilled; i++) {
        if      (guid_eq(&codecs[i], &NV_ENC_CODEC_H264_GUID)) drv->nvencSupportsH264 = true;
        else if (guid_eq(&codecs[i], &NV_ENC_CODEC_HEVC_GUID)) drv->nvencSupportsHEVC = true;
        else if (guid_eq(&codecs[i], &NV_ENC_CODEC_AV1_GUID))  drv->nvencSupportsAV1  = true;
    }

    /* 2. For each supported codec, walk the profile GUIDs it advertises.
     * We only care about the ones that gate new fork surface: H264 High10
     * (via NV_ENC_H264_PROFILE_HIGH_10_GUID) and HEVC FREXT (umbrella for
     * Main422_10 / Main444 / Main444_10 via NV_ENC_HEVC_PROFILE_FREXT_GUID). */
    GUID interested[] = {
        NV_ENC_H264_PROFILE_HIGH_10_GUID,
        NV_ENC_HEVC_PROFILE_MAIN10_GUID,
        NV_ENC_HEVC_PROFILE_FREXT_GUID,
    };
    for (uint32_t i = 0; i < codecFilled; i++) {
        uint32_t profCount = 0;
        if (tmp.funcs.nvEncGetEncodeProfileGUIDCount(tmp.encoder, codecs[i],
                                                     &profCount) != NV_ENC_SUCCESS)
            continue;
        if (profCount == 0) continue;
        GUID *profs = calloc(profCount, sizeof(GUID));
        if (!profs) continue;
        uint32_t profFilled = 0;
        tmp.funcs.nvEncGetEncodeProfileGUIDs(tmp.encoder, codecs[i], profs,
                                              profCount, &profFilled);
        for (uint32_t j = 0; j < profFilled; j++) {
            for (size_t k = 0; k < sizeof(interested)/sizeof(interested[0]); k++) {
                if (!guid_eq(&profs[j], &interested[k])) continue;
                if (guid_eq(&interested[k], &NV_ENC_H264_PROFILE_HIGH_10_GUID))
                    drv->nvencSupportsH264High10 = true;
                else if (guid_eq(&interested[k], &NV_ENC_HEVC_PROFILE_MAIN10_GUID))
                    drv->nvencSupportsHEVCMain10 = true;
                else if (guid_eq(&interested[k], &NV_ENC_HEVC_PROFILE_FREXT_GUID))
                    drv->nvencSupportsHEVCFrext = true;
            }
        }
        free(profs);
    }

    /* 3. Per-codec caps for 10-bit / YUV444 / YUV422 encode. */
    if (drv->nvencSupportsAV1) {
        int cap = 0;
        if (nvenc_query_cap(&tmp.funcs, tmp.encoder, NV_ENC_CODEC_AV1_GUID,
                            NV_ENC_CAPS_SUPPORT_10BIT_ENCODE, &cap))
            drv->nvencSupportsAV1_10bit = (cap != 0);
    }

    /* 4. Input formats — walk supported formats for each active codec to
     * populate the YUV444 / YUV444_10 / YUV422 / YUV422_10 flags. Format
     * support is codec-agnostic in NVENC's API but the cap query is per-
     * codec, so we OR across the codecs we care about. */
    for (uint32_t i = 0; i < codecFilled; i++) {
        uint32_t fmtCount = 0;
        if (tmp.funcs.nvEncGetInputFormatCount(tmp.encoder, codecs[i],
                                                &fmtCount) != NV_ENC_SUCCESS ||
            fmtCount == 0) continue;
        NV_ENC_BUFFER_FORMAT *fmts = calloc(fmtCount, sizeof(NV_ENC_BUFFER_FORMAT));
        if (!fmts) continue;
        uint32_t fmtFilled = 0;
        tmp.funcs.nvEncGetInputFormats(tmp.encoder, codecs[i], fmts, fmtCount,
                                        &fmtFilled);
        for (uint32_t j = 0; j < fmtFilled; j++) {
            switch (fmts[j]) {
            case NV_ENC_BUFFER_FORMAT_YUV444:        drv->nvencSupportsInputYUV444    = true; break;
            case NV_ENC_BUFFER_FORMAT_YUV444_10BIT:  drv->nvencSupportsInputYUV444_10 = true; break;
            case NV_ENC_BUFFER_FORMAT_NV16:          drv->nvencSupportsInputYUV422    = true; break;
            case NV_ENC_BUFFER_FORMAT_P210:          drv->nvencSupportsInputYUV422_10 = true; break;
            default: break;
            }
        }
        free(fmts);
    }
    free(codecs);

    LOG("NVENC caps: H264=%d(High10=%d) HEVC=%d(Main10=%d, FREXT=%d) "
        "AV1=%d(10bit=%d) input{YUV444=%d YUV444_10=%d YUV422=%d YUV422_10=%d}",
        drv->nvencSupportsH264, drv->nvencSupportsH264High10,
        drv->nvencSupportsHEVC, drv->nvencSupportsHEVCMain10,
        drv->nvencSupportsHEVCFrext,
        drv->nvencSupportsAV1, drv->nvencSupportsAV1_10bit,
        drv->nvencSupportsInputYUV444, drv->nvencSupportsInputYUV444_10,
        drv->nvencSupportsInputYUV422, drv->nvencSupportsInputYUV422_10);

done:
    nvenc_close_session(&tmp);
    drv->nvencCapsProbed = true;
    return true;
}

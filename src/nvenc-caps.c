#include "nvenc-caps.h"

#include <stdlib.h>
#include <string.h>

static bool guid_eq(const GUID *a, const GUID *b) {
    return memcmp(a, b, sizeof(GUID)) == 0;
}

static bool query_cap(NV_ENCODE_API_FUNCTION_LIST *funcs, void *encoder,
                      GUID codecGuid, NV_ENC_CAPS capsToQuery, int *out) {
    NV_ENC_CAPS_PARAM param = { .version = NV_ENC_CAPS_PARAM_VER,
                                .capsToQuery = capsToQuery };
    return funcs->nvEncGetEncodeCaps(encoder, codecGuid, &param, out) == NV_ENC_SUCCESS;
}

bool nvenc_caps_probe_session(NV_ENCODE_API_FUNCTION_LIST *funcs, void *encoder,
                              NVEncIPCCaps *out)
{
    memset(out, 0, sizeof(*out));

    /* 1. Codec GUIDs the driver supports at all. */
    uint32_t codecCount = 0;
    if (funcs->nvEncGetEncodeGUIDCount(encoder, &codecCount) != NV_ENC_SUCCESS ||
        codecCount == 0) {
        return false;
    }
    GUID *codecs = calloc(codecCount, sizeof(GUID));
    if (codecs == NULL) {
        return false;
    }
    uint32_t codecFilled = 0;
    funcs->nvEncGetEncodeGUIDs(encoder, codecs, codecCount, &codecFilled);
    for (uint32_t i = 0; i < codecFilled; i++) {
        if      (guid_eq(&codecs[i], &NV_ENC_CODEC_H264_GUID)) out->h264 = 1;
        else if (guid_eq(&codecs[i], &NV_ENC_CODEC_HEVC_GUID)) out->hevc = 1;
        else if (guid_eq(&codecs[i], &NV_ENC_CODEC_AV1_GUID))  out->av1  = 1;
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
        if (funcs->nvEncGetEncodeProfileGUIDCount(encoder, codecs[i],
                                                  &profCount) != NV_ENC_SUCCESS)
            continue;
        if (profCount == 0) continue;
        GUID *profs = calloc(profCount, sizeof(GUID));
        if (!profs) continue;
        uint32_t profFilled = 0;
        funcs->nvEncGetEncodeProfileGUIDs(encoder, codecs[i], profs,
                                          profCount, &profFilled);
        for (uint32_t j = 0; j < profFilled; j++) {
            for (size_t k = 0; k < sizeof(interested)/sizeof(interested[0]); k++) {
                if (!guid_eq(&profs[j], &interested[k])) continue;
                if (guid_eq(&interested[k], &NV_ENC_H264_PROFILE_HIGH_10_GUID))
                    out->h264High10 = 1;
                else if (guid_eq(&interested[k], &NV_ENC_HEVC_PROFILE_MAIN10_GUID))
                    out->hevcMain10 = 1;
                else if (guid_eq(&interested[k], &NV_ENC_HEVC_PROFILE_FREXT_GUID))
                    out->hevcFrext = 1;
            }
        }
        free(profs);
    }

    /* 3. Per-codec caps for 10-bit / YUV444 / YUV422 encode. */
    if (out->av1) {
        int cap = 0;
        if (query_cap(funcs, encoder, NV_ENC_CODEC_AV1_GUID,
                      NV_ENC_CAPS_SUPPORT_10BIT_ENCODE, &cap))
            out->av1_10bit = (cap != 0);
    }

    /* 3b. Maximum encode dimensions, per codec. These are not uniform: H.264
     * tops out at 4096 on current hardware while HEVC and AV1 go to 8192, so
     * reporting one hardcoded number through VAConfigAttribMaxPictureWidth /
     * Height either hides 8K HEVC/AV1 capability or promises H.264 sizes the
     * encoder will reject at session init. */
    struct { uint32_t supported; GUID guid; uint32_t *w, *h; } dims[] = {
        { out->h264, NV_ENC_CODEC_H264_GUID, &out->maxWidthH264, &out->maxHeightH264 },
        { out->hevc, NV_ENC_CODEC_HEVC_GUID, &out->maxWidthHEVC, &out->maxHeightHEVC },
        { out->av1,  NV_ENC_CODEC_AV1_GUID,  &out->maxWidthAV1,  &out->maxHeightAV1  },
    };
    for (size_t i = 0; i < sizeof(dims) / sizeof(dims[0]); i++) {
        if (!dims[i].supported) continue;
        int cap = 0;
        if (query_cap(funcs, encoder, dims[i].guid, NV_ENC_CAPS_WIDTH_MAX, &cap) && cap > 0)
            *dims[i].w = (uint32_t) cap;
        cap = 0;
        if (query_cap(funcs, encoder, dims[i].guid, NV_ENC_CAPS_HEIGHT_MAX, &cap) && cap > 0)
            *dims[i].h = (uint32_t) cap;
    }

    /* 4. Input formats — walk supported formats for each active codec to
     * populate the YUV444 / YUV444_10 / YUV422 / YUV422_10 flags. Format
     * support is codec-agnostic in NVENC's API but the cap query is per-
     * codec, so we OR across the codecs we care about. */
    for (uint32_t i = 0; i < codecFilled; i++) {
        uint32_t fmtCount = 0;
        if (funcs->nvEncGetInputFormatCount(encoder, codecs[i],
                                            &fmtCount) != NV_ENC_SUCCESS ||
            fmtCount == 0) continue;
        NV_ENC_BUFFER_FORMAT *fmts = calloc(fmtCount, sizeof(NV_ENC_BUFFER_FORMAT));
        if (!fmts) continue;
        uint32_t fmtFilled = 0;
        funcs->nvEncGetInputFormats(encoder, codecs[i], fmts, fmtCount, &fmtFilled);
        for (uint32_t j = 0; j < fmtFilled; j++) {
            switch (fmts[j]) {
            case NV_ENC_BUFFER_FORMAT_YUV444:        out->inputYUV444    = 1; break;
            case NV_ENC_BUFFER_FORMAT_YUV444_10BIT:  out->inputYUV444_10 = 1; break;
            case NV_ENC_BUFFER_FORMAT_NV16:          out->inputYUV422    = 1; break;
            case NV_ENC_BUFFER_FORMAT_P210:          out->inputYUV422_10 = 1; break;
            default: break;
            }
        }
        free(fmts);
    }
    free(codecs);

    out->struct_size = sizeof(*out);
    return true;
}

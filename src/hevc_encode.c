#include "vabackend.h"
#include "nvenc.h"
#include <string.h>
#include <va/va.h>

void hevcenc_handle_sequence_params(NVENCContext *nvencCtx, NVBuffer *buffer)
{
    VAEncSequenceParameterBufferHEVC *seq =
        (VAEncSequenceParameterBufferHEVC*) buffer->ptr;

    /* Resent every frame by some clients; log only on change. */
    if (LOG_ENABLED()) {
        const NVENCSeqLog seqLog = {
            .width = seq->pic_width_in_luma_samples,
            .height = seq->pic_height_in_luma_samples,
            .intraPeriod = seq->intra_period,
            .ipPeriod = seq->ip_period,
            .bitsPerSecond = seq->bits_per_second,
        };
        if (nvenc_log_state_changed(&nvencCtx->loggedSeq, &seqLog, sizeof(seqLog))) {
            LOG("HEVC encode: seq params %ux%u, intra_period=%u, ip_period=%u, bitrate=%u",
                seqLog.width, seqLog.height, seqLog.intraPeriod, seqLog.ipPeriod,
                seqLog.bitsPerSecond);
        }
    }

    nvencCtx->width = seq->pic_width_in_luma_samples;
    nvencCtx->height = seq->pic_height_in_luma_samples;

    if (seq->intra_period > 0) {
        nvencCtx->intraPeriod = seq->intra_period;
    }
    if (seq->ip_period > 0) {
        nvencCtx->ipPeriod = seq->ip_period;
    }

    /* VUI timing info */
    if (seq->vui_num_units_in_tick > 0 && seq->vui_time_scale > 0) {
        nvencCtx->frameRateNum = seq->vui_time_scale;
        nvencCtx->frameRateDen = seq->vui_num_units_in_tick * 2;
    }

    /* Bitrate (VA-API provides in bits/sec) */
    if (seq->bits_per_second > 0) {
        nvencCtx->bitrate = seq->bits_per_second;
        if (nvencCtx->maxBitrate == 0) {
            nvencCtx->maxBitrate = seq->bits_per_second;
        }
    }

    nvencCtx->seqParamSet = true;
}

void hevcenc_handle_picture_params(NVENCContext *nvencCtx, NVBuffer *buffer)
{
    VAEncPictureParameterBufferHEVC *pic =
        (VAEncPictureParameterBufferHEVC*) buffer->ptr;

    nvencCtx->currentCodedBufId = pic->coded_buf;
    nvencCtx->forceIDR = (pic->pic_fields.bits.idr_pic_flag != 0);
    if (nvencCtx->forceIDR) {
        LOG("HEVC encode: picture params, coded_buf=%d, IDR requested", pic->coded_buf);
    }
    /* Per-picture CQP hint. VA-API HEVC's pic_init_qp is the resolved QP
     * (init_qp_minus26 + 26). In CONSTQP mode nvenc_reconfigure_if_needed
     * picks this up before the next encode. */
    if (pic->pic_init_qp > 0 && pic->pic_init_qp <= 51) {
        nvencCtx->picQP = pic->pic_init_qp;
    }
}

void hevcenc_handle_slice_params(NVENCContext *nvencCtx, NVBuffer *buffer)
{
    const VAEncSliceParameterBufferHEVC *slice =
        (VAEncSliceParameterBufferHEVC*) buffer->ptr;

    /* Map VA-API HEVC slice_type to NVENC picture type.
     * HEVC slice types: 0=B, 1=P, 2=I */
    switch (slice->slice_type) {
    case 2: /* I */
        nvencCtx->picType = nvencCtx->forceIDR
            ? NV_ENC_PIC_TYPE_IDR : NV_ENC_PIC_TYPE_I;
        break;
    case 1: /* P */
        nvencCtx->picType = NV_ENC_PIC_TYPE_P;
        break;
    case 0: /* B */
        nvencCtx->picType = nvencCtx->allowBframes ? NV_ENC_PIC_TYPE_B : NV_ENC_PIC_TYPE_P;
        break;
    default:
        nvencCtx->picType = NV_ENC_PIC_TYPE_UNKNOWN;
        break;
    }
}

void hevcenc_handle_misc_params(NVENCContext *nvencCtx, NVBuffer *buffer)
{
    VAEncMiscParameterBuffer *misc = (VAEncMiscParameterBuffer*) buffer->ptr;

    switch (misc->type) {
    case VAEncMiscParameterTypeRateControl: {
        VAEncMiscParameterRateControl *rc =
            (VAEncMiscParameterRateControl*) misc->data;
        if (LOG_ENABLED()) {
            const NVENCRateLog rateLog = {
                .bitsPerSecond = rc->bits_per_second,
                .targetPercentage = rc->target_percentage,
                .initialQP = rc->initial_qp,
                .minQP = rc->min_qp,
                .maxQP = rc->max_qp,
            };
            if (nvenc_log_state_changed(&nvencCtx->loggedRate, &rateLog, sizeof(rateLog))) {
                LOG("HEVC encode: rate control bits_per_second=%u qp{init=%u min=%u max=%u}",
                    rc->bits_per_second, rc->initial_qp, rc->min_qp, rc->max_qp);
            }
        }
        if (rc->bits_per_second > 0) {
            nvencCtx->maxBitrate = rc->bits_per_second;
            if (rc->target_percentage > 0) {
                nvencCtx->bitrate = (uint32_t)((uint64_t)rc->bits_per_second * rc->target_percentage / 100);
            } else {
                nvencCtx->bitrate = rc->bits_per_second;
            }
        }
        if (rc->initial_qp > 0) nvencCtx->initialQP = rc->initial_qp;
        if (rc->min_qp > 0)     nvencCtx->minQP     = rc->min_qp;
        if (rc->max_qp > 0)     nvencCtx->maxQP     = rc->max_qp;
        break;
    }
    case VAEncMiscParameterTypeFrameRate: {
        const VAEncMiscParameterFrameRate *fr =
            (VAEncMiscParameterFrameRate*) misc->data;
        if (fr->framerate > 0) {
            uint32_t num = fr->framerate & 0xffff;
            uint32_t den = (fr->framerate >> 16) & 0xffff;
            if (den == 0) den = 1;
            nvencCtx->frameRateNum = num;
            nvencCtx->frameRateDen = den;
        }
        break;
    }
    case VAEncMiscParameterTypeHRD: {
        VAEncMiscParameterHRD *hrd =
            (VAEncMiscParameterHRD*) misc->data;
        if (hrd->buffer_size > 0)
            nvencCtx->vbvBufferSize = hrd->buffer_size;
        if (hrd->initial_buffer_fullness > 0)
            nvencCtx->vbvInitialDelay = hrd->initial_buffer_fullness;
        break;
    }
    case VAEncMiscParameterTypeQualityLevel: {
        VAEncMiscParameterBufferQualityLevel *ql =
            (VAEncMiscParameterBufferQualityLevel*) misc->data;
        if (ql->quality_level > 0 && ql->quality_level <= 7)
            nvencCtx->qualityLevel = ql->quality_level;
        break;
    }
    default:
        break;
    }
}

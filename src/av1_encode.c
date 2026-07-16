#include "vabackend.h"
#include "nvenc.h"
#include <string.h>
#include <va/va.h>
#include <va/va_enc_av1.h>
#include <malloc.h>

void av1enc_handle_sequence_params(NVENCContext *nvencCtx, NVBuffer *buffer)
{
    VAEncSequenceParameterBufferAV1 *seq =
        (VAEncSequenceParameterBufferAV1*) buffer->ptr;

    LOG("AV1 encode: seq params, intra_period=%u, ip_period=%u, bitrate=%u",
        seq->intra_period, seq->ip_period, seq->bits_per_second);

    if (seq->intra_period > 0) {
        nvencCtx->intraPeriod = seq->intra_period;
    }
    if (seq->ip_period > 0) {
        nvencCtx->ipPeriod = seq->ip_period;
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

void av1enc_handle_picture_params(NVENCContext *nvencCtx, NVBuffer *buffer)
{
    VAEncPictureParameterBufferAV1 *pic =
        (VAEncPictureParameterBufferAV1*) buffer->ptr;

    nvencCtx->width = pic->frame_width_minus_1 + 1;
    nvencCtx->height = pic->frame_height_minus_1 + 1;
    nvencCtx->currentCodedBufId = pic->coded_buf;
    nvencCtx->temporalId = pic->temporal_id;

    /* AV1 frame types: 0=KEY, 1=INTER, 2=INTRA_ONLY, 3=SWITCH */
    switch (pic->picture_flags.bits.frame_type) {
    case 0: /* KEY */
        nvencCtx->picType = NV_ENC_PIC_TYPE_IDR;
        nvencCtx->forceIDR = true;
        break;
    case 2: /* INTRA_ONLY */
        nvencCtx->picType = NV_ENC_PIC_TYPE_I;
        nvencCtx->forceIDR = false;
        break;
    case 1: /* INTER */
        /* Map to P for now to ensure 1:1 mapping and no reordering. 
         * If B-frames are needed, we'd need more logic to distinguish them. */
        nvencCtx->picType = NV_ENC_PIC_TYPE_P;
        nvencCtx->forceIDR = false;
        break;
    default:
        nvencCtx->picType = NV_ENC_PIC_TYPE_UNKNOWN;
        nvencCtx->forceIDR = false;
        break;
    }

    if (nvencCtx->forceIDR) {
        LOG("AV1 encode: picture params, coded_buf=%d, IDR requested", pic->coded_buf);
    }
}

void av1enc_handle_slice_params(NVENCContext *nvencCtx, NVBuffer *buffer)
{
    /* VA-API AV1 uses VAEncTileGroupBufferAV1 here.
     * Currently we let NVENC handle tiling automatically. */
    (void)nvencCtx;
    (void)buffer;
}

void av1enc_handle_misc_params(NVENCContext *nvencCtx, NVBuffer *buffer)
{
    VAEncMiscParameterBuffer *misc = (VAEncMiscParameterBuffer*) buffer->ptr;

    switch (misc->type) {
    case VAEncMiscParameterTypeRateControl: {
        VAEncMiscParameterRateControl *rc =
            (VAEncMiscParameterRateControl*) misc->data;
        if (rc->bits_per_second > 0) {
            nvencCtx->maxBitrate = rc->bits_per_second;
            if (rc->target_percentage > 0) {
                nvencCtx->bitrate = (uint32_t)((uint64_t)rc->bits_per_second * rc->target_percentage / 100);
            } else {
                nvencCtx->bitrate = rc->bits_per_second;
            }
        }
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
    case VAEncMiscParameterTypeTemporalLayerStructure: {
        VAEncMiscParameterTemporalLayerStructure *tl =
            (VAEncMiscParameterTemporalLayerStructure*) misc->data;
        if (tl->number_of_layers > 0) {
            nvencCtx->numTemporalLayers = tl->number_of_layers;
            LOG("AV1 encode: temporal layer structure, number_of_layers=%u",
                tl->number_of_layers);
        }
        break;
    }
    default:
        break;
    }
}

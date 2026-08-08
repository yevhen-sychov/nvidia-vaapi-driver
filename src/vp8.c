#include "vabackend.h"

static void copyVP8PicParam(NVContext *ctx, NVBuffer* buffer, CUVIDPICPARAMS *picParams)
{
    //Untested, my 1060 (GP106) doesn't support this, however it's simple enough that it should work
    VAPictureParameterBufferVP8* buf = (VAPictureParameterBufferVP8*) buffer->ptr;

    picParams->PicWidthInMbs    = (buf->frame_width + 15) / 16;
    picParams->FrameHeightInMbs = (buf->frame_height + 15) / 16;

    picParams->CodecSpecific.vp8.width = buf->frame_width;
    picParams->CodecSpecific.vp8.height = buf->frame_height;

    picParams->CodecSpecific.vp8.LastRefIdx = pictureIdxFromSurfaceId(ctx->drv, buf->last_ref_frame);
    picParams->CodecSpecific.vp8.GoldenRefIdx = pictureIdxFromSurfaceId(ctx->drv, buf->golden_ref_frame);
    picParams->CodecSpecific.vp8.AltRefIdx = pictureIdxFromSurfaceId(ctx->drv, buf->alt_ref_frame);

    picParams->CodecSpecific.vp8.vp8_frame_tag.frame_type = buf->pic_fields.bits.key_frame;
    picParams->CodecSpecific.vp8.vp8_frame_tag.version = buf->pic_fields.bits.version;
    picParams->CodecSpecific.vp8.vp8_frame_tag.show_frame = 1;//?
    picParams->CodecSpecific.vp8.vp8_frame_tag.update_mb_segmentation_data = buf->pic_fields.bits.segmentation_enabled ? buf->pic_fields.bits.update_segment_feature_data : 0;
}

static void copyVP8SliceParam(NVContext *ctx, NVBuffer* buffer, CUVIDPICPARAMS *picParams)
{
    VASliceParameterBufferVP8* buf = (VASliceParameterBufferVP8*) buffer->ptr;

    picParams->CodecSpecific.vp8.first_partition_size = buf->partition_size[0] + ((buf->macroblock_offset + 7) / 8);

    ctx->lastSliceParams = buffer->ptr;
    ctx->lastSliceParamsCount = buffer->elements;

    picParams->nNumSlices += buffer->elements;
}

/* Rebuild the VP8 "uncompressed data chunk" (RFC 6386 §9.1) that VA-API strips
 * before handing us slice data, but that NVDEC still expects at the head of the
 * bitstream.
 *
 * Layout, all little-endian:
 *   3-byte frame tag: bit 0 frame_type (0 = key), bits 1-3 version,
 *                     bit 4 show_frame, bits 5-23 first_part_size
 *   keyframes only:   3-byte sync code 9d 01 2a,
 *                     16-bit width  | horizontal_scale << 14,
 *                     16-bit height | vertical_scale   << 14
 *
 * Every field comes from the VA-API buffers we were given: frame_type, version
 * and the dimensions from VAPictureParameterBufferVP8, and first_part_size from
 * VASliceParameterBufferVP8 (copyVP8SliceParam already derives it as
 * partition_size[0] + ceil(macroblock_offset / 8), which reproduces the real
 * frame tag's field exactly).
 *
 * The one field VA-API does not carry is show_frame. Upstream read it out of
 * the frame tag it recovered from memory before the buffer; we default it to 1,
 * matching what copyVP8PicParam already assumes. A decoder is only asked to
 * decode frames the client intends to use, and NVDEC is told the same value
 * through CUVIDPICPARAMS.CodecSpecific.vp8.vp8_frame_tag, so the synthesized
 * byte stays consistent with the struct.
 *
 * Returns the number of bytes written into `out` (3 or 10). */
static size_t buildVP8UncompressedChunk(const CUVIDPICPARAMS *picParams, uint8_t out[10])
{
    const uint32_t frameType = picParams->CodecSpecific.vp8.vp8_frame_tag.frame_type;
    const uint32_t version = picParams->CodecSpecific.vp8.vp8_frame_tag.version;
    const uint32_t showFrame = picParams->CodecSpecific.vp8.vp8_frame_tag.show_frame;
    const uint32_t firstPartSize = picParams->CodecSpecific.vp8.first_partition_size;

    const uint32_t tag = (frameType & 0x1)
                       | ((version & 0x7) << 1)
                       | ((showFrame & 0x1) << 4)
                       | ((firstPartSize & 0x7ffff) << 5);

    out[0] = (uint8_t) (tag & 0xff);
    out[1] = (uint8_t) ((tag >> 8) & 0xff);
    out[2] = (uint8_t) ((tag >> 16) & 0xff);

    if (frameType != 0) {
        return 3; //interframe: frame tag only
    }

    const uint32_t width = picParams->CodecSpecific.vp8.width;
    const uint32_t height = picParams->CodecSpecific.vp8.height;

    out[3] = 0x9d;
    out[4] = 0x01;
    out[5] = 0x2a;
    //scale fields are 0: VA-API carries no upscaling factor, and the decoded
    //size is already the coded size we were handed.
    out[6] = (uint8_t) (width & 0xff);
    out[7] = (uint8_t) ((width >> 8) & 0x3f);
    out[8] = (uint8_t) (height & 0xff);
    out[9] = (uint8_t) ((height >> 8) & 0x3f);

    return 10;
}

static void copyVP8SliceData(NVContext *ctx, NVBuffer* buf, CUVIDPICPARAMS *picParams)
{
    uint8_t chunk[10];
    const size_t chunkSize = buildVP8UncompressedChunk(picParams, chunk);

    for (unsigned int i = 0; i < ctx->lastSliceParamsCount; i++)
    {
        VASliceParameterBufferVP8 *sliceParams = &((VASliceParameterBufferVP8*) ctx->lastSliceParams)[i];
        uint32_t offset = (uint32_t) ctx->bitstreamBuffer.size;
        appendBuffer(&ctx->sliceOffsets, &offset, sizeof(offset));

        appendBuffer(&ctx->bitstreamBuffer, chunk, chunkSize);
        appendBuffer(&ctx->bitstreamBuffer, PTROFF(buf->ptr, sliceParams->slice_data_offset),
                     sliceParams->slice_data_size);
        picParams->nBitstreamDataLen += chunkSize + sliceParams->slice_data_size;
    }
}

static void ignoreVP8Buffer(NVContext *ctx, NVBuffer *buffer, CUVIDPICPARAMS *picParams)
{
    // Intentionally do nothing
    (void)ctx;
    (void)buffer;
    (void)picParams;
}

static cudaVideoCodec computeVP8CudaCodec(VAProfile profile) {
    if (profile == VAProfileVP8Version0_3) {
        return cudaVideoCodec_VP8;
    }

    return cudaVideoCodec_NONE;
}

static const VAProfile vp8SupportedProfiles[] = {
    VAProfileVP8Version0_3,
};

const DECLARE_CODEC(vp8Codec) = {
    .computeCudaCodec = computeVP8CudaCodec,
    .handlers = {
        [VAPictureParameterBufferType] = copyVP8PicParam,
        [VASliceParameterBufferType] = copyVP8SliceParam,
        [VASliceDataBufferType] = copyVP8SliceData,
        [VAIQMatrixBufferType]         = ignoreVP8Buffer,
        [VAProbabilityBufferType]      = ignoreVP8Buffer,
    },
    .supportedProfileCount = ARRAY_SIZE(vp8SupportedProfiles),
    .supportedProfiles = vp8SupportedProfiles,
};

#ifndef NVENC_CAPS_H
#define NVENC_CAPS_H

#include <stdbool.h>
#include <ffnvcodec/nvEncodeAPI.h>

#include "nvenc-ipc.h"

/*
 * Capability enumeration against an already-open NVENC session.
 *
 * Split out of nvenc.c so the 64-bit nvenc-helper can run the exact same walk
 * and ship the result back over IPC. In encode-only mode the driver process has
 * no usable CUDA context (that is why the helper exists), so it cannot open a
 * scratch session of its own to probe with — but the helper can, and a caps
 * answer from the helper describes the same GPU that will do the encoding.
 *
 * `out` is fully overwritten, including struct_size. Returns false if the
 * session refused to enumerate anything, in which case `out` is left zeroed.
 */
bool nvenc_caps_probe_session(NV_ENCODE_API_FUNCTION_LIST *funcs, void *encoder,
                              NVEncIPCCaps *out);

#endif // NVENC_CAPS_H

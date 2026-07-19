/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef NV_DRIVER_H
#define NV_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

#include "../common.h"
#include "nvidia-drm-ioctl.h"

#define ROUND_UP(N, S) ((((N) + (S) - 1) / (S)) * (S))

typedef struct {
    int nvctlFd;
    int nv0Fd;
    int drmFd;
    uint32_t clientObject;
    uint32_t deviceObject;
    uint32_t subdeviceObject;
    uint32_t driverMajorVersion;
    uint32_t driverMinorVersion;
    //bool hasHugePage;
    uint32_t gpu_id;
    uint32_t generic_page_kind;
    uint32_t page_kind_generation;
    uint32_t sector_layout;
    bool useSystemMemory;  // True for unified memory systems (Grace-Blackwell/Grace-Hopper)
} NVDriverContext;

typedef struct {
    int nvFd;
    int nvFd2;
    int drmFd;
    uint32_t width;
    uint32_t height;
    uint64_t mods;
    uint32_t memorySize;
    uint32_t offset;
    uint32_t pitch;
    uint32_t fourcc;
    uint32_t log2GobsPerBlockX;
    uint32_t log2GobsPerBlockY;
    uint32_t log2GobsPerBlockZ;
} NVDriverImage;

typedef enum {
    DESCRIPTOR_MODE_MULTI,
    DESCRIPTOR_MODE_SINGLE,
    /* Like SINGLE, but exports YUV formats as a single combined-fourcc layer
     * (e.g. NV12 with 2 planes) instead of one split single-channel layer per
     * plane (R8 + GR88). Some EGL/ANGLE DMA-BUF importers (used by Chrome's
     * WebGL/canvas "video-processing" worker path) only advertise support for
     * the combined fourcc and reject the split-plane layout with
     * EGL_BAD_MATCH. Decode-display paths keep working with SINGLE/MULTI;
     * use COMBINED only to work around that importer. */
    DESCRIPTOR_MODE_COMBINED,
    /* Default mode: decide the layer layout per-surface instead of a single
     * global choice. Encode-context surfaces (local capture/preview, which
     * Chrome imports through the WebGL/canvas worker path) are exported with
     * the COMBINED layout; decode-context surfaces (remote/video display,
     * which needs the split per-plane layout) are exported like SINGLE. This
     * lets a single Chrome session get correct rendering for both the local
     * encode preview and remote decode without the user having to pick a
     * mode manually. Explicitly setting NVD_DESCRIPTOR_MODE overrides this
     * per-surface decision for every surface. */
    DESCRIPTOR_MODE_AUTO
} DescriptorMode;

bool init_nvdriver(NVDriverContext *context, int drmFd);
bool free_nvdriver(NVDriverContext *context);
bool get_device_uuid(const NVDriverContext *context, uint8_t uuid[16]);
bool alloc_memory(const NVDriverContext *context, uint32_t size, int *fd);
bool alloc_image(NVDriverContext *context, uint32_t width, uint32_t height, uint8_t channels, uint8_t bytesPerChannel, uint32_t fourcc, NVDriverImage *image);
uint32_t calculate_unified_image_layout(const NVDriverContext *context, NVDriverImage images[], uint32_t width, uint32_t height,
                                        uint32_t bppc, uint32_t numPlanes, const NVFormatPlane planes[],
                                        bool unifyBlockHeight);
bool alloc_buffer(NVDriverContext *context, uint32_t totalSize, const NVDriverImage images[], int *nvFd, int *nvFd2, int *drmFd);

#endif

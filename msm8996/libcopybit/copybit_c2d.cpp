/*
 * Copyright (C) 2008 The Android Open Source Project
 * Copyright (c) 2010-2015, The Linux Foundation. All rights reserved.
 *
 * Not a Contribution, Apache license notifications and license are retained
 * for attribution purposes only.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <cutils/log.h>
#include <sys/resource.h>
#include <sys/prctl.h>

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/mman.h>

#include </home/hondajohn88/DU/hardware/qcom/msm8996/kernel-headers/linux/msm_kgsl.h>

#include <EGL/eglplatform.h>
#include <cutils/native_handle.h>

#include <copybit.h>
#include <alloc_controller.h>
#include <memalloc.h>

#include "c2d2.h"
#include "software_converter.h"

#include <dlfcn.h>

using gralloc::IMemAlloc;
using gralloc::IonController;
using gralloc::alloc_data;

C2D_STATUS (*LINK_c2dCreateSurface)( uint32 *surface_id,
                                     uint32 surface_bits,
                                     C2D_SURFACE_TYPE surface_type,
                                     void *surface_definition );

C2D_STATUS (*LINK_c2dUpdateSurface)( uint32 surface_id,
                                     uint32 surface_bits,
                                     C2D_SURFACE_TYPE surface_type,
                                     void *surface_definition );

C2D_STATUS (*LINK_c2dReadSurface)( uint32 surface_id,
                                   C2D_SURFACE_TYPE surface_type,
                                   void *surface_definition,
                                   int32 x, int32 y );

C2D_STATUS (*LINK_c2dDraw)( uint32 target_id,
                            uint32 target_config, C2D_RECT *target_scissor,
                            uint32 target_mask_id, uint32 target_color_key,
                            C2D_OBJECT *objects_list, uint32 num_objects );

C2D_STATUS (*LINK_c2dFinish)( uint32 target_id);

C2D_STATUS (*LINK_c2dFlush)( uint32 target_id, c2d_ts_handle *timestamp);

C2D_STATUS (*LINK_c2dWaitTimestamp)( c2d_ts_handle timestamp );

C2D_STATUS (*LINK_c2dDestroySurface)( uint32 surface_id );

C2D_STATUS (*LINK_c2dMapAddr) ( int mem_fd, void * hostptr, size_t len,
                                size_t offset, uint32 flags, void ** gpuaddr);

C2D_STATUS (*LINK_c2dUnMapAddr) ( void * gpuaddr);

C2D_STATUS (*LINK_c2dGetDriverCapabilities) ( C2D_DRIVER_INFO * driver_info);

/* create a fence fd for the timestamp */
C2D_STATUS (*LINK_c2dCreateFenceFD) ( uint32 target_id, c2d_ts_handle timestamp,
                                                            int32 *fd);

C2D_STATUS (*LINK_c2dFillSurface) ( uint32 surface_id, uint32 fill_color,
                                    C2D_RECT * fill_rect);

/******************************************************************************/

#if defined(COPYBIT_Z180)
#define MAX_SCALE_FACTOR    (4096)
#define MAX_DIMENSION       (4096)
#else
#error "Unsupported HW version"
#endif

// The following defines can be changed as required i.e. as we encounter
// complex use cases.
#define MAX_RGB_SURFACES 32       // Max. RGB layers currently supported per draw
#define MAX_YUV_2_PLANE_SURFACES 4// Max. 2-plane YUV layers currently supported per draw
#define MAX_YUV_3_PLANE_SURFACES 1// Max. 3-plane YUV layers currently supported per draw
// +1 for the destination surface. We cannot have multiple destination surfaces.
#define MAX_SURFACES (MAX_RGB_SURFACES + MAX_YUV_2_PLANE_SURFACES + MAX_YUV_3_PLANE_SURFACES + 1)
#define NUM_SURFACE_TYPES 3      // RGB_SURFACE + YUV_SURFACE_2_PLANES + YUV_SURFACE_3_PLANES
#define MAX_BLIT_OBJECT_COUNT 50 // Max. blit objects that can be passed per draw

enum {
    RGB_SURFACE,
    YUV_SURFACE_2_PLANES,
    YUV_SURFACE_3_PLANES
};

enum eConversionType {
    CONVERT_TO_ANDROID_FORMAT,
    CONVERT_TO_C2D_FORMAT
};

enum eC2DFlags {
    FLAGS_PREMULTIPLIED_ALPHA  = 1<<0,
    FLAGS_YUV_DESTINATION      = 1<<1,
    FLAGS_TEMP_SRC_DST         = 1<<2,
    FLAGS_UBWC_FORMAT_MODE     = 1<<3
};

/******************************************************************************/

/** State information for each device instance */
struct copybit_context_t {
    struct copybit_device_t device;
    // Templates for the various source surfaces. These templates are created
    // to avoid the expensive create/destroy C2D Surfaces
    C2D_OBJECT_STR blit_rgb_object[MAX_RGB_SURFACES];
    C2D_OBJECT_STR blit_yuv_2_plane_object[MAX_YUV_2_PLANE_SURFACES];
    C2D_OBJECT_STR blit_yuv_3_plane_object[MAX_YUV_3_PLANE_SURFACES];
    C2D_OBJECT_STR blit_list[MAX_BLIT_OBJECT_COUNT]; // Z-ordered list of blit objects
    C2D_DRIVER_INFO c2d_driver_info;
    void *libc2d2;
    alloc_data temp_src_buffer;
    alloc_data temp_dst_buffer;
    unsigned int dst[NUM_SURFACE_TYPES]; // dst surfaces
    uintptr_t mapped_gpu_addr[MAX_SURFACES]; // GPU addresses mapped inside copybit
    int blit_rgb_count;         // Total RGB surfaces being blit
    int blit_yuv_2_plane_count; // Total 2 plane YUV surfaces being
    int blit_yuv_3_plane_count; // Total 3 plane YUV  surfaces being blit
    int blit_count;             // Total blit objects.
    unsigned int trg_transform;      /* target transform */
    int fb_width;
    int fb_height;
    int src_global_alpha;
    int config_mask;
    int dst_surface_type;
    bool is_premultiplied_alpha;
    void* time_stamp;
    bool dst_surface_mapped; // Set when dst surface is mapped to GPU addr
    void* dst_surface_base; // Stores the dst surface addr
    bool is_src_ubwc_format;
    bool is_dst_ubwc_format;

    // used for signaling the wait thread
    bool wait_timestamp;
    pthread_t wait_thread_id;
    bool stop_thread;
    pthread_mutex_t wait_cleanup_lock;
    pthread_cond_t wait_cleanup_cond;

};

struct bufferInfo {
    int width;
    int height;
    int format;
};

struct yuvPlaneInfo {
    int yStride;       //luma stride
    int plane1_stride;
    int plane2_stride;
    size_t plane1_offset;
    size_t plane2_offset;
};



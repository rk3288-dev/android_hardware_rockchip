/*
 * Copyright (C) 2011 The Android Open Source Project
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

#ifndef HARDWARE_ROCKCHIP_INCLUDE_ANDROID_GRAPHICS_H
#define HARDWARE_ROCKCHIP_INCLUDE_ANDROID_GRAPHICS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * pixel format definitions
 */

typedef enum android_pixel_format_extended {
    HAL_PIXEL_FORMAT_YCrCb_NV12         = 0x15, // YUY2
    HAL_PIXEL_FORMAT_YCrCb_NV12_VIDEO   = 0x16,
    HAL_PIXEL_FORMAT_YCrCb_NV12_10      = 0x17, // YUY2_10bit
    HAL_PIXEL_FORMAT_YCbCr_422_SP_10    = 0x18,
    HAL_PIXEL_FORMAT_YCrCb_420_SP_10    = 0x19,
} android_pixel_format_extended_t;

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_ROCKCHIP_INCLUDE_ANDROID_GRAPHICS_H */

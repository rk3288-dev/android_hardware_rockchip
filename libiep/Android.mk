LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    iep_api.cpp

LOCAL_CFLAGS := \
    -DLOG_TAG=\"libiep\"

LOCAL_C_INCLUDES += \
    hardware/rockchip/include \
    hardware/libhardware/include

LOCAL_LDFLAGS := \
    -Wl,-z,defs

LOCAL_SHARED_LIBRARIES := \
    libcutils

LOCAL_MODULE := libiep
LOCAL_MODULE_TAGS := optional
LOCAL_PRELINK_MODULE := false

include $(BUILD_SHARED_LIBRARY)

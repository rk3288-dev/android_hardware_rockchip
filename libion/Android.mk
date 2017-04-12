LOCAL_PATH := $(call my-dir)


include $(CLEAR_VARS)

LOCAL_SRC_FILES := rockchip_ion.c

LOCAL_MODULE := libion.rockchip
LOCAL_MODULE_TAGS := optional
LOCAL_SHARED_LIBRARIES := libion liblog

LOCAL_C_INCLUDES := $(LOCAL_PATH)/include
LOCAL_EXPORT_C_INCLUDE_DIRS := $(LOCAL_PATH)/include

LOCAL_CFLAGS := -Werror

include $(BUILD_SHARED_LIBRARY)

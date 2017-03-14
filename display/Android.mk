ifneq ($(filter rk3288,$(TARGET_BOARD_PLATFORM)),)
include $(call all-makefiles-under,$(LOCAL_PATH))
endif

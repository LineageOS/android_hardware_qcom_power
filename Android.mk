ifneq ($(WITH_QC_PERF),true)
ifeq ($(TARGET_POWERHAL_VARIANT),)
ifneq ($(filter deb flo hammerhead mako,$(TARGET_DEVICE)),)

LOCAL_PATH := $(call my-dir)

# HAL module implemenation stored in
# hw/<POWERS_HARDWARE_MODULE_ID>.<ro.hardware>.so
include $(CLEAR_VARS)


LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_SHARED_LIBRARIES := liblog libcutils libdl
LOCAL_SRC_FILES := \
                   power.c \

LOCAL_MODULE:= power.$(TARGET_BOARD_PLATFORM)
LOCAL_MODULE_TAGS := optional

ifneq ($(TARGET_MPDECISION_BOOST_SOCKET),)
LOCAL_CFLAGS += -DBOOST_SOCKET=\"$(TARGET_MPDECISION_BOOST_SOCKET)\"
endif

ifeq ($(TARGET_POWERHAL_TOUCH_BOOST),true)
LOCAL_CFLAGS += -DTOUCH_BOOST
endif

include $(BUILD_SHARED_LIBRARY)

endif # TARGET_DEVICE = googly_device
endif # !TARGET_POWERHAL_VARIANT
endif # !WITH_QC_PERF

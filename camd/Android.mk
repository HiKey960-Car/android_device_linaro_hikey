LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := camd
LOCAL_SHARED_LIBRARIES := liblog libcutils
LOCAL_SRC_FILES := camd.c
include $(BUILD_EXECUTABLE)


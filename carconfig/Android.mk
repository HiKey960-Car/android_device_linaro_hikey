LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_SRC_FILES := $(call all-java-files-under, src)
LOCAL_RESOURCE_DIR := $(LOCAL_PATH)/res
LOCAL_MODULE_TAGS := optional
LOCAL_PACKAGE_NAME := RBCarSettings
LOCAL_OVERRIDES_PACKAGES += LatinIME
LOCAL_CERTIFICATE := platform
LOCAL_PROGUARD_ENABLED := disabled
LOCAL_PRIVILEGED_MODULE := true
LOCAL_DEX_PREOPT := false
LOCAL_PRIVATE_PLATFORM_APIS := true
include $(BUILD_PACKAGE)


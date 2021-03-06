#
# Copyright (C) 2011 The Android Open-Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

PRODUCT_COPY_FILES +=   $(TARGET_PREBUILT_KERNEL):kernel \
                        $(TARGET_PREBUILT_DTB):hi6220-hikey.dtb \
			$(LOCAL_PATH)/$(TARGET_FSTAB):root/fstab.hikey \
			device/linaro/hikey/init.common.rc:root/init.hikey.rc \
			device/linaro/hikey/init.hikey.power.rc:root/init.hikey.power.rc \
			device/linaro/hikey/init.common.usb.rc:root/init.hikey.usb.rc \
			device/linaro/hikey/ueventd.common.rc:root/ueventd.hikey.rc \
			device/linaro/hikey/common.kl:system/usr/keylayout/hikey.kl

# Copy BT firmware
PRODUCT_COPY_FILES += \
	device/linaro/hikey/bt-wifi-firmware-util/TIInit_11.8.32.bts:$(TARGET_COPY_OUT_VENDOR)/firmware/ti-connectivity/TIInit_11.8.32.bts

# Copy wlan firmware
PRODUCT_COPY_FILES += \
	device/linaro/hikey/bt-wifi-firmware-util/wl18xx-fw-4.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/ti-connectivity/wl18xx-fw-4.bin \
	device/linaro/hikey/bt-wifi-firmware-util/wl18xx-conf.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/ti-connectivity/wl18xx-conf.bin

# Build HiKey HDMI audio HAL
PRODUCT_PACKAGES += audio.primary.hikey

# Include USB speed switch App
PRODUCT_PACKAGES += UsbSpeedSwitch

# Build libion
PRODUCT_PACKAGES += libion

# Build gralloc for hikey
PRODUCT_PACKAGES += gralloc.hikey

# PowerHAL
PRODUCT_PACKAGES += power.hikey

# Sensors HAL
PRODUCT_PACKAGES += sensors.hikey

# Include mali blobs from ARM
PRODUCT_PACKAGES += libGLES_mali.so END_USER_LICENCE_AGREEMENT.txt

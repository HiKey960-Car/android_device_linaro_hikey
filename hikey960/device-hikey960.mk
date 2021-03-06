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

PRODUCT_COPY_FILES +=	$(TARGET_PREBUILT_KERNEL):kernel \
			$(TARGET_PREBUILT_DTB):hi3660-hikey960.dtb

PRODUCT_COPY_FILES +=	$(LOCAL_PATH)/fstab.hikey960:root/fstab.hikey960 \
			device/linaro/hikey/init.common.rc:root/init.hikey960.rc \
			device/linaro/hikey/init.hikey960.power.rc:root/init.hikey960.power.rc \
			device/linaro/hikey/init.common.usb.rc:root/init.hikey960.usb.rc \
			device/linaro/hikey/ueventd.common.rc:root/ueventd.hikey960.rc \
			device/linaro/hikey/common.kl:system/usr/keylayout/hikey960.kl \
			frameworks/native/data/etc/android.hardware.vulkan.level-1.xml:system/etc/permissions/android.hardware.vulkan.level.xml \
			frameworks/native/data/etc/android.hardware.vulkan.version-1_0_3.xml:system/etc/permissions/android.hardware.vulkan.version.xml

# Copy BT firmware
PRODUCT_COPY_FILES += \
	device/linaro/hikey/bt-wifi-firmware-util/TIInit_11.8.32-pcm-960.bts:$(TARGET_COPY_OUT_VENDOR)/firmware/ti-connectivity/TIInit_11.8.32.bts

# Copy wlan firmware
PRODUCT_COPY_FILES += \
	device/linaro/hikey/bt-wifi-firmware-util/wl18xx-fw-4.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/ti-connectivity/wl18xx-fw-4.bin \
	device/linaro/hikey/bt-wifi-firmware-util/wl18xx-conf-wl1837mod.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/ti-connectivity/wl18xx-conf.bin

# Copy hifi firmware
PRODUCT_COPY_FILES += \
	device/linaro/hikey/hifi/firmware/hifi-hikey960.img:$(TARGET_COPY_OUT_VENDOR)/firmware/hifi/hifi.img

ifeq ($(TARGET_PRODUCT),hikey960)
# Build HiKey960 HDMI audio HAL. Experimental only may not work. FIXME
PRODUCT_PACKAGES += audio.primary.hikey960
endif

PRODUCT_PACKAGES += gralloc.hikey960

#binary blobs from ARM
PRODUCT_PACKAGES +=	libGLES_mali.so libbccArm.so libRSDriverArm.so libmalicore.bc \
			vulkan.hikey960.so android.hardware.renderscript@1.0-impl.so \
			END_USER_LICENCE_AGREEMENT.txt

PRODUCT_PACKAGES += power.hikey960

PRODUCT_PACKAGES += sensors.hikey960

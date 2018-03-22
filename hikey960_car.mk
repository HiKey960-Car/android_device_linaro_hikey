$(call inherit-product, device/linaro/hikey/hikey960.mk)

BOARD_BLUETOOTH_BDROID_BUILDCFG_INCLUDE_DIR := device/linaro/hikey/bluetooth_car

$(call inherit-product, device/linaro/hikey/car.mk)
PRODUCT_PACKAGE_OVERLAYS := device/linaro/hikey/overlay_car

PRODUCT_PACKAGES += vehicle.default \
	CarSettings \
	Launcher3 \
	tinymix \
	tinypcminfo \
	tinyhostless \
	android.hardware.automotive.vehicle@2.0 \
	android.hardware.automotive.vehicle@2.0-service \
	android.hardware.broadcastradio@1.1-service.dmhd1000

# Steering wheel interface daemon
PRODUCT_PACKAGES += \
		swid \
		camd \
		RBCarSettings

# Build HiKey960 USB primary audio HAL
PRODUCT_PACKAGES += audio.primary.hikey960

# Build HiKey960 lights HAL
PRODUCT_PACKAGES += \
		lights.hikey960 \
		android.hardware.light@2.0-impl

# Build generic USB GPS HAL
PRODUCT_PACKAGES += gps.hikey960 \
        android.hardware.gnss@1.0 \
        android.hardware.gnss@1.0-impl \
        android.hardware.gnss@1.0-service \
        gps.conf \
        u-blox.conf

PRODUCT_COPY_FILES += \
    device/generic/car/common/bootanimations/bootanimation-832.zip:system/media/bootanimation.zip \
    device/generic/car/common/android.hardware.dummy.xml:system/etc/permissions/handheld_core_hardware.xml \
    packages/services/Car/car_product/init/init.bootstat.rc:root/init.bootstat.rc

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.type.automotive.xml:system/etc/permissions/android.hardware.type.automotive.xml \
    frameworks/native/data/etc/android.hardware.screen.landscape.xml:system/etc/permissions/android.hardware.screen.landscape.xml

PRODUCT_COPY_FILES += \
    device/linaro/hikey/usbaudio/audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_configuration.xml

PRODUCT_COPY_FILES += \
    device/linaro/hikey/broadcastradio/android.hardware.broadcastradio.xml:system/etc/permissions/android.hardware.broadcastradio.xml

#
# Overrides
PRODUCT_NAME := hikey960_car
PRODUCT_DEVICE := hikey960
PRODUCT_BRAND := Android
PRODUCT_MODEL := AOSP CAR on hikey960

PRODUCT_PROPERTY_OVERRIDES += \
    android.car.drawer.unlimited=true \
    service.adb.tcp.port=5555

# Add car related sepolicy.
BOARD_SEPOLICY_DIRS += \
    device/generic/car/common/sepolicy \
    packages/services/Car/car_product/sepolicy

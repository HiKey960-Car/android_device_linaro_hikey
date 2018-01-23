$(call inherit-product, device/linaro/hikey/hikey960.mk)

$(call inherit-product, packages/services/Car/car_product/build/car.mk)
PRODUCT_PACKAGE_OVERLAYS := packages/services/Car/car_product/overlay

PRODUCT_PACKAGES += vehicle.default \
	CarSettings \
	Launcher3 \
	tinymix \
	tinypcminfo \
	tinyhostless \
	android.hardware.automotive.vehicle@2.0 \
	android.hardware.automotive.vehicle@2.0-service \
	car-radio-service

# Build HiKey960 USB audio HAL
PRODUCT_PACKAGES += audio.usb.hikey960

PRODUCT_COPY_FILES += \
    device/generic/car/common/bootanimations/bootanimation-832.zip:system/media/bootanimation.zip \
    device/generic/car/common/android.hardware.dummy.xml:system/etc/permissions/handheld_core_hardware.xml \
    packages/services/Car/car_product/init/init.car.rc:root/init.car.rc \
    packages/services/Car/car_product/init/init.bootstat.rc:root/init.bootstat.rc

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.type.automotive.xml:system/etc/permissions/android.hardware.type.automotive.xml \
    frameworks/native/data/etc/android.hardware.screen.landscape.xml:system/etc/permissions/android.hardware.screen.landscape.xml

#
# Overrides
PRODUCT_NAME := hikey960_car
PRODUCT_DEVICE := hikey960
PRODUCT_BRAND := Android
PRODUCT_MODEL := AOSP CAR on hikey960

PRODUCT_PROPERTY_OVERRIDES += \
    android.car.drawer.unlimited=true \
    android.car.hvac.demo=true \
    com.android.car.radio.demo=true \
    com.android.car.radio.demo.dual=true

# Add car related sepolicy.
BOARD_SEPOLICY_DIRS += \
    device/generic/car/common/sepolicy \
    packages/services/Car/car_product/sepolicy

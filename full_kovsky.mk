#
# Copyright (C) 2008 The Android Open Source Project
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
$(call inherit-product, $(SRC_TARGET_DIR)/product/small_base.mk)

# The gps config appropriate for this device
$(call inherit-product, device/common/gps/gps_us_supl.mk)

DEVICE_PACKAGE_OVERLAYS := device/htc/kovsky/overlay

# Use high-density artwork where available
PRODUCT_LOCALES += hdpi

PRODUCT_PACKAGES += \
    VoiceDialer \
    sensors.msm7k \
	microp-keypad.kcm \
	vold.fstab \
	libOmxCore \
	gps.xdandroid \
	copybit.msm7k \
	gralloc.kovsky \
	lights.msm7k \
	libhtc_acoustic \
	libhtcgeneric_ril \
	libcamera \
	wl1251.ko \
	wl1251_sdio.ko

# Install the features available on this device.
PRODUCT_COPY_FILES += \
    frameworks/base/data/etc/handheld_core_hardware.xml:system/etc/permissions/handheld_core_hardware.xml \
    frameworks/base/data/etc/android.hardware.camera.autofocus.xml:system/etc/permissions/android.hardware.camera.autofocus.xml \
    frameworks/base/data/etc/android.hardware.telephony.gsm.xml:system/etc/permissions/android.hardware.telephony.gsm.xml \
    frameworks/base/data/etc/android.hardware.location.gps.xml:system/etc/permissions/android.hardware.location.gps.xml \
    frameworks/base/data/etc/android.hardware.wifi.xml:system/etc/permissions/android.hardware.wifi.xml \

# Prebuilt stuff
PRODUCT_COPY_FILES += \
    device/htc/kovsky/configs/init.kovsky.rc:root/init.kovsky.rc \
    device/htc/kovsky/configs/build.kovsky.prop:system/build.kovsky.prop \
    device/htc/kovsky/configs/ueventd.kovsky.rc:root/ueventd.kovsky.rc \
    device/htc/kovsky/configs/vold.fstab:system/etc/vold.fstab

# Keylayout / Headset
PRODUCT_COPY_FILES += \
    device/htc/kovsky/configs/keylayout/microp-keypad.kl:system/usr/keylayout/microp-keypad.kl \
    device/htc/kovsky/configs/keylayout/3.5mm_headset.kl:system/usr/keylayout/3.5mm_headset.kl

# Acoustic (Sound processor) settings
PRODUCT_COPY_FILES += \
    device/htc/kovsky/configs/acoustic/AudioFilterTable.csv:system/etc/AudioFilterTable.csv \
    device/htc/kovsky/configs/acoustic/AudioPara3.csv:system/etc/AudioPara3.csv \
    device/htc/kovsky/configs/acoustic/AudioPreProcessTable.csv:system/etc/AudioPreProcessTable.csv

PRODUCT_PROPERTY_OVERRIDES := \
    keyguard.no_require_sim=true \
    ro.ril.hsxpa=2 \
    ro.ril.gprsclass=10 \
    ro.media.dec.jpeg.memcap=10000000 \
    ro.com.google.clientidbase=android-tmobile-us \
    ro.com.google.clientidbase.vs=android-hms-tmobile-us \
    ro.com.google.clientidbase.ms=android-hms-tmobile-us \
    ro.com.google.locationfeatures=1 \
    ro.com.google.networklocation=1 \
    ro.setupwizard.enable_bypass=1 \
    ro.media.dec.aud.wma.enabled=1 \
    ro.media.dec.vid.wmv.enabled=1

PRODUCT_PROPERTY_OVERRIDES += \
    rild.libpath=/system/lib/libhtcgeneric_ril.so \
    wifi.interface=wlan0

# Time between scans in seconds. Keep it high to minimize battery drain.
# This only affects the case in which there are remembered access points,
# but none are in range.
PRODUCT_PROPERTY_OVERRIDES += \
    wifi.supplicant_scan_interval=15

# density in DPI of the LCD of this board. This is used to scale the UI
# appropriately. If this property is not defined, the default value is 160 dpi. 
PRODUCT_PROPERTY_OVERRIDES += \
    ro.sf.lcd_density=240 \
	ro.sf.hwrotation=180

# Default network type
# 0 => WCDMA Preferred.
PRODUCT_PROPERTY_OVERRIDES += \
    ro.telephony.default_network=0

# Disable JIT by default
PRODUCT_PROPERTY_OVERRIDES += \
    dalvik.vm.execution-mode=int:fast

# The OpenGL ES API level that is natively supported by this device.
# This is a 16.16 fixed point number
PRODUCT_PROPERTY_OVERRIDES += \
    ro.opengles.version=65536

# media configuration xml file
PRODUCT_COPY_FILES += \
    device/htc/kovsky/media_profiles.xml:/system/etc/media_profiles.xml

# Kernel stuff
ifeq ($(TARGET_PREBUILT_KERNEL),)
	LOCAL_KERNEL := device/htc/kovsky/prebuilt_kernel/kernel
else
	LOCAL_KERNEL := $(TARGET_PREBUILT_KERNEL)
endif
PRODUCT_COPY_FILES += \
	$(LOCAL_KERNEL):kernel

## (2) Also get non-open-source aspects if available
$(call inherit-product-if-exists, vendor/htc/kovsky/kovsky-vendor.mk)

# stuff common to all HTC phones
$(call inherit-product, device/htc/common/common.mk)

$(call inherit-product, $(SRC_TARGET_DIR)/product/generic.mk)

# Discard inherited values and use our own instead.
PRODUCT_NAME := full_kovsky
PRODUCT_DEVICE := kovsky
PRODUCT_BRAND := Android
PRODUCT_MODEL := Full Android on Kovsky

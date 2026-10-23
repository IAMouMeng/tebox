# inherit-product analog: generic HALs from src/common/hardware/interfaces.
# AOSP still uses *.mk for products; modules are Android.bp.

PRODUCT_PACKAGES += android.hardware.health-service
PRODUCT_PACKAGES += android.hardware.health.storage-service
PRODUCT_PACKAGES += android.hardware.power-service
PRODUCT_PACKAGES += android.hardware.lights-service
PRODUCT_PACKAGES += android.hardware.thermal-service
PRODUCT_PACKAGES += android.hardware.dumpstate-service
PRODUCT_PACKAGES += android.hardware.memtrack-service
PRODUCT_PACKAGES += android.hardware.nfc-service
PRODUCT_PACKAGES += android.hardware.bluetooth-service
PRODUCT_PACKAGES += android.hardware.gnss-service
PRODUCT_PACKAGES += android.hardware.biometrics.fingerprint-service
PRODUCT_PACKAGES += android.hardware.sensors-service
PRODUCT_PACKAGES += graphics
PRODUCT_PACKAGES += android.hardware.audio-service
PRODUCT_PACKAGES += android.hardware.wifi-service
PRODUCT_PACKAGES += android.hardware.camera.provider-service
PRODUCT_PACKAGES += android.hardware.media.c2-service
PRODUCT_PACKAGES += android.hardware.radio-service
PRODUCT_PACKAGES += android.hardware.security.keymint-service

# Mbed TLS 2.28 LTS (Apache-2.0), used by the netplay client for wss:// (encrypted WebSocket to Cloudflare).
LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := mbedtls_all
LOCAL_SRC_FILES := $(subst $(LOCAL_PATH)/,,$(wildcard $(LOCAL_PATH)/library/*.c))
LOCAL_C_INCLUDES := $(LOCAL_PATH)/include
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/include
LOCAL_CFLAGS := -O2 -w
include $(BUILD_STATIC_LIBRARY)

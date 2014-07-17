LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	main_surfaceflinger.cpp 

LOCAL_SHARED_LIBRARIES := \
	libsurfaceflinger \
	libbinder \
	libutils

LOCAL_C_INCLUDES :=  frameworks/native/services/surfaceflinger

LOCAL_C_INCLUDES +=  hardware/qcom/display/libqcomui

LOCAL_MODULE:= surfaceflinger

include $(BUILD_EXECUTABLE)

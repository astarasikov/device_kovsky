# Copyright 2006 The Android Open Source Project

# XXX using libutils for simulator build only...
#
LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_target := lib

ifeq ($(LOCAL_target),lib)
  LOCAL_MODULE_CLASS := SHARED_LIBRARIES
  LOCAL_MODULE := libhtcgeneric-ril
else
  LOCAL_MODULE_CLASS := EXECUTABLES
  LOCAL_MODULE:= htcgeneric-ril
endif

intermediates:= $(local-intermediates-dir)
GEN := $(intermediates)/gitver.h
$(GEN): PRIVATE_CUSTOM_TOOL = git --git-dir=$(<D) log -1 --format=format:'"%h %ci"' > $@
$(GEN):	$(LOCAL_PATH)/.git/index
	$(transform-generated-source)

LOCAL_GENERATED_SOURCES += $(GEN)

LOCAL_SRC_FILES:= \
    htcgeneric-ril.c \
    atchannel.c \
    misc.c \
    at_tok.c \
    sms.c \
    sms_gsm.c \
    gsm.c \
	sms_cdma.c

LOCAL_SHARED_LIBRARIES := \
	libril

	# for asprinf
LOCAL_CFLAGS := -D_GNU_SOURCE

LOCAL_C_INCLUDES := $(KERNEL_HEADERS)

ifeq ($(TARGET_PRODUCT),sooner)
  LOCAL_CFLAGS += -DOMAP_CSMI_POWER_CONTROL -DUSE_TI_COMMANDS 
endif

ifeq ($(TARGET_PRODUCT),surf)
  LOCAL_CFLAGS += -DPOLL_CALL_STATE -DUSE_QMI
endif

ifeq ($(TARGET_PRODUCT),dream)
  LOCAL_CFLAGS += -DPOLL_CALL_STATE -DUSE_QMI
endif

LOCAL_MODULE_TAGS := optional

ifeq ($(LOCAL_target),lib)
  #build shared library
  LOCAL_SHARED_LIBRARIES += \
	libcutils libutils
  LOCAL_LDLIBS += -lpthread 
  LOCAL_CFLAGS += -DRIL_SHLIB 
  LOCAL_PRELINK_MODULE := false
  include $(BUILD_SHARED_LIBRARY)
else
  #build executable
  include $(BUILD_EXECUTABLE)
endif

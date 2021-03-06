# Copyright (C) 2012 The Android Open Source Project
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

ifeq ($(REFERENCE_DEVICE),loki)
LOCAL_PATH := $(my-dir)
subdir_makefiles := \
	$(LOCAL_PATH)/liblights/Android.mk \
	$(LOCAL_PATH)/power/Android.mk \
	$(LOCAL_PATH)/sensors/Android.mk

ifeq ($(BOARD_HAVE_LBH_SUPPORT), true)
LBH_MAKEFILE := $(TOP)/vendor/nvidia/fury/tools/lbh/AndroidLBH.mk
ifeq ($(wildcard $(LBH_MAKEFILE)),$(LBH_MAKEFILE))
FURY_CODE_NAME := loki
subdir_makefiles += $(LBH_MAKEFILE)
endif
endif

include $(subdir_makefiles)
endif

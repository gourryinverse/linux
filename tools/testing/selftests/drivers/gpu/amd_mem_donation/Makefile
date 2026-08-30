# SPDX-License-Identifier: GPL-2.0

CC ?= gcc
HIPCC ?= hipcc
HIP_ARCH ?= gfx1150

CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -Werror
HIPFLAGS ?= -O2 -g
HIPFLAGS += -Wall -Wextra --offload-arch=$(HIP_ARCH)
HIPLDLIBS ?= -lamdhip64
DRM_CFLAGS := $(shell pkg-config --cflags libdrm_amdgpu)
DRM_LDLIBS := $(shell pkg-config --libs libdrm_amdgpu)

TEST_GEN_PROGS := amd_mem_donation_cpu amd_mem_donation_hip \
	amd_mem_donation_drm_hip amd_mem_donation_userptr_hip

.PHONY: all clean smoke

all: $(TEST_GEN_PROGS)

amd_mem_donation_cpu: amd_mem_donation_cpu.c
	$(CC) $(CFLAGS) -o $@ $<

amd_mem_donation_hip: amd_mem_donation_hip.cpp
	$(HIPCC) $(HIPFLAGS) -o $@ $< $(HIPLDLIBS)

amd_mem_donation_drm_hip: amd_mem_donation_drm_hip.cpp
	$(HIPCC) $(HIPFLAGS) $(DRM_CFLAGS) -o $@ $< \
		$(HIPLDLIBS) $(DRM_LDLIBS)

amd_mem_donation_userptr_hip: amd_mem_donation_userptr_hip.cpp
	$(HIPCC) $(HIPFLAGS) -o $@ $< $(HIPLDLIBS)

smoke: all
	sudo ./amd_mem_donation.py --scenario smoke --cycles 3

clean:
	$(RM) $(TEST_GEN_PROGS)

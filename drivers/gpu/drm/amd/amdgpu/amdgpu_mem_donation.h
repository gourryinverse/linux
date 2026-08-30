/* SPDX-License-Identifier: MIT */
#ifndef __AMDGPU_MEM_DONATION_H__
#define __AMDGPU_MEM_DONATION_H__

struct amdgpu_device;

int amdgpu_mem_donation_init(struct amdgpu_device *adev);
void amdgpu_mem_donation_fini(struct amdgpu_device *adev);
int amdgpu_mem_donation_set_size(struct amdgpu_device *adev, u64 size);
u64 amdgpu_mem_donation_get_size(struct amdgpu_device *adev);
u64 amdgpu_mem_donation_get_quarantined_size(struct amdgpu_device *adev);
int
amdgpu_mem_donation_begin(struct amdgpu_device *adev,
			  enum amdgpu_mem_donation_lifecycle lifecycle,
			  bool rollback);
int amdgpu_mem_donation_shutdown_begin(struct amdgpu_device *adev);
bool amdgpu_mem_donation_shutdown_safe(struct amdgpu_device *adev);
void
amdgpu_mem_donation_end(struct amdgpu_device *adev,
			enum amdgpu_mem_donation_lifecycle lifecycle,
			bool restore);

#endif

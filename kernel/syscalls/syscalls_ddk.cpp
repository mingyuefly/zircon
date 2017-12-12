// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <err.h>
#include <platform.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <trace.h>

#include <dev/interrupt.h>
#include <dev/udisplay.h>
#include <vm/vm.h>
#include <vm/vm_object_paged.h>
#include <vm/vm_object_physical.h>
#include <lib/user_copy/user_ptr.h>
#include <object/handle.h>
#include <object/interrupt_dispatcher.h>
#include <object/interrupt_event_dispatcher.h>
#include <object/process_dispatcher.h>
#include <object/resources.h>
#include <object/vm_object_dispatcher.h>

#if ARCH_X86
#include <platform/pc/bootloader.h>
#endif

#include <zircon/syscalls/pci.h>

#include "syscalls_priv.h"

#define LOCAL_TRACE 0

static_assert(ZX_CACHE_POLICY_CACHED == ARCH_MMU_FLAG_CACHED,
              "Cache policy constant mismatch - CACHED");
static_assert(ZX_CACHE_POLICY_UNCACHED == ARCH_MMU_FLAG_UNCACHED,
              "Cache policy constant mismatch - UNCACHED");
static_assert(ZX_CACHE_POLICY_UNCACHED_DEVICE == ARCH_MMU_FLAG_UNCACHED_DEVICE,
              "Cache policy constant mismatch - UNCACHED_DEVICE");
static_assert(ZX_CACHE_POLICY_WRITE_COMBINING == ARCH_MMU_FLAG_WRITE_COMBINING,
              "Cache policy constant mismatch - WRITE_COMBINING");

zx_status_t sys_interrupt_create(zx_handle_t hrsrc, uint32_t options,
                                 user_out_ptr<zx_handle_t> out_handle) {
    LTRACEF("options 0x%x\n", options);

    if (options != 0u)
        return ZX_ERR_INVALID_ARGS;

    // TODO(ZX-971): finer grained validation
    zx_status_t status;
    if ((status = validate_resource(hrsrc, ZX_RSRC_KIND_ROOT)) < 0) {
        return status;
    }

    fbl::RefPtr<Dispatcher> dispatcher;
    zx_rights_t rights;
    zx_status_t result = InterruptEventDispatcher::Create(&dispatcher, &rights);
    if (result != ZX_OK)
        return result;

    HandleOwner handle(Handle::Make(fbl::move(dispatcher), rights));

    auto up = ProcessDispatcher::GetCurrent();
    zx_handle_t hv = up->MapHandleToValue(handle);

    status = out_handle.copy_to_user(hv);
    if (status != ZX_OK)
        return status;

    up->AddHandle(fbl::move(handle));
    return ZX_OK;
}

zx_status_t sys_interrupt_bind(zx_handle_t handle, uint32_t slot, zx_handle_t hrsrc,
                               uint32_t vector, uint32_t options) {
    LTRACEF("handle %x\n", handle);

    // TODO(ZX-971): finer grained validation
    zx_status_t status;
    if ((status = validate_resource(hrsrc, ZX_RSRC_KIND_ROOT)) < 0) {
        return status;
    }

    auto up = ProcessDispatcher::GetCurrent();
    fbl::RefPtr<InterruptDispatcher> interrupt;
    status = up->GetDispatcher(handle, &interrupt);
    if (status != ZX_OK)
        return status;

    return interrupt->Bind(slot, vector, options);
}

zx_status_t sys_interrupt_unbind(zx_handle_t handle, uint32_t slot) {
    LTRACEF("handle %x\n", handle);

    auto up = ProcessDispatcher::GetCurrent();
    fbl::RefPtr<InterruptDispatcher> interrupt;
    zx_status_t status = up->GetDispatcher(handle, &interrupt);
    if (status != ZX_OK)
        return status;

    return interrupt->Unbind(slot);
}

zx_status_t sys_interrupt_complete(zx_handle_t handle_value) {
    LTRACEF("handle %x\n", handle_value);

    // this syscall is now deprecated and no longer does anything
    // TODO(voydanoff) remove this
    auto up = ProcessDispatcher::GetCurrent();
    fbl::RefPtr<InterruptDispatcher> interrupt;
    return up->GetDispatcher(handle_value, &interrupt);
}

zx_status_t sys_interrupt_wait(zx_handle_t handle, user_out_ptr<uint64_t> out_slots) {
    LTRACEF("handle %x\n", handle);

    auto up = ProcessDispatcher::GetCurrent();
    fbl::RefPtr<InterruptDispatcher> interrupt;
    zx_status_t status = up->GetDispatcher(handle, &interrupt);
    if (status != ZX_OK)
        return status;

    uint64_t slots = 0;
    status = interrupt->WaitForInterrupt(slots);
    if (status == ZX_OK && out_slots)
        status = out_slots.copy_to_user(slots);
    return status;
}

zx_status_t sys_interrupt_wait_with_timestamp(zx_handle_t handle, user_out_ptr<uint32_t> out_slot,
                                              user_out_ptr<zx_time_t> out_timestamp) {
    LTRACEF("handle %x\n", handle);

    auto up = ProcessDispatcher::GetCurrent();
    fbl::RefPtr<InterruptDispatcher> interrupt;
    zx_status_t status = up->GetDispatcher(handle, &interrupt);
    if (status != ZX_OK)
        return status;

    uint32_t slot = 0;
    zx_time_t timestamp = 0;
    status = interrupt->WaitForInterruptWithTimeStamp(slot, timestamp);
    if (status == ZX_OK && out_slot)
        status = out_slot.copy_to_user(slot);
    if (status == ZX_OK && out_timestamp)
        status = out_timestamp.copy_to_user(timestamp);
    return status;
}

zx_status_t sys_interrupt_signal(zx_handle_t handle, uint32_t slot, zx_time_t timestamp) {
    LTRACEF("handle %x\n", handle);

    auto up = ProcessDispatcher::GetCurrent();
    fbl::RefPtr<InterruptDispatcher> interrupt;
    zx_status_t status = up->GetDispatcher(handle, &interrupt);
    if (status != ZX_OK)
        return status;

    return interrupt->UserSignal(slot, timestamp);
}

zx_status_t sys_vmo_create_contiguous(zx_handle_t hrsrc, size_t size,
                                      uint32_t alignment_log2,
                                      user_out_ptr<zx_handle_t> _out) {
    LTRACEF("size 0x%zu\n", size);

    if (size == 0) return ZX_ERR_INVALID_ARGS;
    if (alignment_log2 == 0)
        alignment_log2 = PAGE_SIZE_SHIFT;
    // catch obviously wrong values
    if (alignment_log2 < PAGE_SIZE_SHIFT ||
            alignment_log2 >= (8 * sizeof(uint64_t)))
        return ZX_ERR_INVALID_ARGS;

    // TODO(ZX-971): finer grained validation
    zx_status_t status;
    if ((status = validate_resource(hrsrc, ZX_RSRC_KIND_ROOT)) < 0) {
        return status;
    }

    size = ROUNDUP_PAGE_SIZE(size);
    // create a vm object
    fbl::RefPtr<VmObject> vmo;
    status = VmObjectPaged::Create(PMM_ALLOC_FLAG_ANY, size, &vmo);
    if (status != ZX_OK)
        return status;

    // always immediately commit memory to the object
    uint64_t committed;
    // CommitRangeContiguous takes a uint8_t for the alignment
    auto align_log2_arg = static_cast<uint8_t>(alignment_log2);
    status = vmo->CommitRangeContiguous(0, size, &committed, align_log2_arg);
    if (status < 0 || (size_t)committed < size) {
        LTRACEF("failed to allocate enough pages (asked for %zu, got %zu)\n", size / PAGE_SIZE,
                (size_t)committed / PAGE_SIZE);
        return ZX_ERR_NO_MEMORY;
    }

    // create a Vm Object dispatcher
    fbl::RefPtr<Dispatcher> dispatcher;
    zx_rights_t rights;
    zx_status_t result = VmObjectDispatcher::Create(fbl::move(vmo), &dispatcher, &rights);
    if (result != ZX_OK)
        return result;

    // create a handle and attach the dispatcher to it
    HandleOwner handle(Handle::Make(fbl::move(dispatcher), rights));
    if (!handle)
        return ZX_ERR_NO_MEMORY;

    auto up = ProcessDispatcher::GetCurrent();

    status = _out.copy_to_user(up->MapHandleToValue(handle));
    if (status != ZX_OK)
        return status;

    up->AddHandle(fbl::move(handle));
    return ZX_OK;
}

zx_status_t sys_vmo_create_physical(zx_handle_t hrsrc, uintptr_t paddr, size_t size,
                                    user_out_ptr<zx_handle_t> _out) {
    LTRACEF("size 0x%zu\n", size);

    // TODO: attempting to create a physical VMO that points to memory should be an error

    zx_status_t status;
    if ((status = validate_resource_mmio(hrsrc, paddr, size)) < 0) {
        return status;
    }

    size = ROUNDUP_PAGE_SIZE(size);

    // create a vm object
    fbl::RefPtr<VmObject> vmo;
    zx_status_t result = VmObjectPhysical::Create(paddr, size, &vmo);
    if (result != ZX_OK) {
        return result;
    }

    // create a Vm Object dispatcher
    fbl::RefPtr<Dispatcher> dispatcher;
    zx_rights_t rights;
    result = VmObjectDispatcher::Create(fbl::move(vmo), &dispatcher, &rights);
    if (result != ZX_OK)
        return result;

    // create a handle and attach the dispatcher to it
    HandleOwner handle(Handle::Make(fbl::move(dispatcher), rights));
    if (!handle)
        return ZX_ERR_NO_MEMORY;

    auto up = ProcessDispatcher::GetCurrent();

    status = _out.copy_to_user(up->MapHandleToValue(handle));
    if (status != ZX_OK)
        return status;

    up->AddHandle(fbl::move(handle));
    return ZX_OK;
}

zx_status_t sys_bootloader_fb_get_info(user_out_ptr<uint32_t> format, user_out_ptr<uint32_t> width,
                                       user_out_ptr<uint32_t> height, user_out_ptr<uint32_t> stride) {
#if ARCH_X86
    if (!bootloader.fb.base)
        return ZX_ERR_INVALID_ARGS;
    zx_status_t status = format.copy_to_user(bootloader.fb.format);
    if (status != ZX_OK)
        return status;
    status = width.copy_to_user(bootloader.fb.width);
    if (status != ZX_OK)
        return status;
    status = height.copy_to_user(bootloader.fb.height);
    if (status != ZX_OK)
        return status;
    status = stride.copy_to_user(bootloader.fb.stride);
    if (status != ZX_OK)
        return status;
    return ZX_OK;
#else
    return ZX_ERR_NOT_SUPPORTED;
#endif
}

zx_status_t sys_set_framebuffer(zx_handle_t hrsrc, user_inout_ptr<void> vaddr, uint32_t len, uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
    // TODO(ZX-971): finer grained validation
    zx_status_t status;
    if ((status = validate_resource(hrsrc, ZX_RSRC_KIND_ROOT)) < 0) {
        return status;
    }

    intptr_t paddr = vaddr_to_paddr(vaddr.get());
    udisplay_set_framebuffer(paddr, len);

    struct display_info di;
    memset(&di, 0, sizeof(struct display_info));
    di.format = format;
    di.width = width;
    di.height = height;
    di.stride = stride;
    di.flags = DISPLAY_FLAG_HW_FRAMEBUFFER;
    udisplay_set_display_info(&di);

    return ZX_OK;
}

zx_status_t sys_set_framebuffer_vmo(zx_handle_t hrsrc, zx_handle_t vmo_handle, uint32_t len, uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
    zx_status_t status;
    if ((status = validate_resource(hrsrc, ZX_RSRC_KIND_ROOT)) < 0)
        return status;

    auto up = ProcessDispatcher::GetCurrent();

    // lookup the dispatcher from handle
    fbl::RefPtr<VmObjectDispatcher> vmo;
    status = up->GetDispatcher(vmo_handle, &vmo);
    if (status != ZX_OK)
        return status;

    status = udisplay_set_framebuffer_vmo(vmo->vmo());
    if (status != ZX_OK)
        return status;

    struct display_info di;
    memset(&di, 0, sizeof(struct display_info));
    di.format = format;
    di.width = width;
    di.height = height;
    di.stride = stride;
    di.flags = DISPLAY_FLAG_HW_FRAMEBUFFER;
    udisplay_set_display_info(&di);

    return ZX_OK;
}

#if ARCH_X86
#include <arch/x86/descriptor.h>
#include <arch/x86/ioport.h>

zx_status_t sys_mmap_device_io(zx_handle_t hrsrc, uint32_t io_addr, uint32_t len) {
    // TODO(ZX-971): finer grained validation
    zx_status_t status;
    if ((status = validate_resource(hrsrc, ZX_RSRC_KIND_ROOT)) < 0) {
        return status;
    }

    LTRACEF("addr 0x%x len 0x%x\n", io_addr, len);

    return IoBitmap::GetCurrent().SetIoBitmap(io_addr, len, 1);
}
#else
zx_status_t sys_mmap_device_io(zx_handle_t hrsrc, uint32_t io_addr, uint32_t len) {
    // doesn't make sense on non-x86
    return ZX_ERR_NOT_SUPPORTED;
}
#endif

uint64_t sys_acpi_uefi_rsdp(zx_handle_t hrsrc) {
    // TODO(ZX-971): finer grained validation
    zx_status_t status;
    if ((status = validate_resource(hrsrc, ZX_RSRC_KIND_ROOT)) < 0) {
        return status;
    }
#if ARCH_X86
    return bootloader.acpi_rsdp;
#endif
    return 0;
}

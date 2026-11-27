// Battlefield 1943 Reborn, SDK v0.11+ port (phase 4): v0.10's rex/system/kernel_state.h.
//
// v0.10's KernelState held the guest memory, the achievements, the function dispatcher, the object table and the title's
// XDBF. v0.11 has no KernelState: those belong to the title's runtime (rex::kernel::TitleRuntime, GetProcessRuntime()).
// This facade gives the game v0.10's calls on top of it, and rex::memory::Memory the v0.10 memory object's (v0.11 split
// it into the address space and the system heap). kernel_state() is null while no title runs, as before.
#pragma once

#include <cstdint>
#include <memory>

#include <rex/filesystem/xdbf.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/guest_memory.h>
#include <rex/kernel/heap.h>
#include <rex/kernel/object/object_table.h>
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/system/achievement_store.h>
#include <rex/xam/achievement_manager.h>

namespace rex::memory {

using BaseHeap = ::rex::kernel::BaseHeap;
using HeapAllocationInfo = ::rex::kernel::HeapAllocation;
using ::rex::kernel::kMemoryAllocationCommit;
using ::rex::kernel::kMemoryAllocationReserve;
using ::rex::kernel::kMemoryProtectNoAccess;
using ::rex::kernel::kMemoryProtectRead;
using ::rex::kernel::kMemoryProtectWrite;

// v0.10's guest memory object: the address space's translation and heaps, and the system heap's allocations. Both are
// read from the runtime on each call (cheap getters), so a relaunch that gives the runtime new memory is followed.
class Memory {
 public:
  explicit Memory(::rex::kernel::TitleRuntime& runtime) : runtime_(&runtime) {}

  template <typename T = uint8_t*>
  T TranslateVirtual(uint32_t guest_address) const {
    return space().TranslateVirtual<T>(guest_address);
  }
  template <typename T = uint8_t*>
  T TranslatePhysical(uint32_t guest_address) const {
    return space().TranslatePhysical<T>(guest_address);
  }
  uint8_t* virtual_membase() const { return space().GetVirtualMembase(); }
  BaseHeap* LookupHeap(uint32_t address) { return space().LookupHeap(address); }
  uint32_t SystemHeapAlloc(uint32_t size, uint32_t alignment = 0x20) {
    return runtime_->GetGuestHeap().SystemHeapAlloc(size, alignment);
  }
  void SystemHeapFree(uint32_t address) { runtime_->GetGuestHeap().SystemHeapFree(address); }

  ::rex::kernel::GuestAddressSpace& space() const { return runtime_->GetAddressSpace(); }

 private:
  ::rex::kernel::TitleRuntime* runtime_;
};

}  // namespace rex::memory

namespace rex::system {

// v0.10's function dispatcher, for the one call the game makes on it.
class FunctionDispatcher {
 public:
  PPCFunc* GetFunction(uint32_t guest_address) const { return ::rex::ppc::ResolveFunction(guest_address); }
};

class KernelState {
 public:
  explicit KernelState(::rex::kernel::TitleRuntime& runtime);

  ::rex::memory::Memory* memory() { return &memory_; }
  ::rex::xam::IAchievementManager& achievements() { return runtime_->GetAchievementManager(); }
  FunctionDispatcher* function_dispatcher() { return &dispatcher_; }
  ::rex::kernel::IObjectTable* object_table() { return &runtime_->GetObjectTable(); }
  // v0.10 returned the XDBF by value; v0.11 owns it in a pointer, read again on each call.
  std::unique_ptr<::rex::filesystem::IXDBFTitleData> title_xdbf() const { return runtime_->GetTitleXDBF(); }
  ::rex::kernel::TitleRuntime& runtime() const { return *runtime_; }
  uint32_t title_id() const { return runtime_->GetTitleID(); }

 private:
  ::rex::kernel::TitleRuntime* runtime_;
  ::rex::memory::Memory memory_;
  FunctionDispatcher dispatcher_;
};

// The running title's state, or null while no title runs (as v0.10's).
KernelState* kernel_state();

}  // namespace rex::system

namespace rex::runtime {
// v0.10's indirect call resolver: v0.11's resolves direct and indirect addresses alike.
inline PPCFunc* ResolveIndirectFunction(uint32_t guest_address) { return ::rex::ppc::ResolveFunction(guest_address); }
}  // namespace rex::runtime

#define REX_KERNEL_STATE() (::rex::system::kernel_state())
#define REX_KERNEL_MEMORY() (REX_KERNEL_STATE()->memory())
#define REX_KERNEL_OBJECTS() (REX_KERNEL_STATE()->object_table())

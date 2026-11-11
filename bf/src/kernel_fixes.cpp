// ObDereferenceObject: the runtime finds the object behind a guest object pointer through the handle the guest
// struct holds, then releases that object's first handle. The game also dereferences threads that have exited and
// whose handles are closed (a batch of them while a level loads); the dead thread's struct still holds its old handle
// number, which by then names another object, and that object loses its handle instead. In run ded8 this was the
// FESL socket (its handle came back as a new thread, recv and closesocket found no socket, EA's layer dropped the
// connection and the game). Here the release only happens when the object found owns that guest pointer.

#include <cstdint>
#include <cstdlib>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xobject.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

extern "C" REX_FUNC(__imp__ObDereferenceObject) {
  const uint32_t native = ctx.r3.u32;
  ctx.r3.u64 = 0;
  if (native == 0xDEADF00D || !native) return;  // the runtime's dummy object from ObReferenceObjectByHandle
  auto object = rex::system::XObject::GetNativeObject<rex::system::XObject>(
      REX_KERNEL_STATE(), REX_KERNEL_MEMORY()->TranslateVirtual(native));
  if (!object) return;
  const auto handles = object->handles();
  if (const char* v = std::getenv("BF_HANDLE_TRACE"); v && *v == '1')
    REXLOG_INFO("BF HANDLE ObDereferenceObject {:08X} type {} handle {:08X} (lr {:08X})", native,
                static_cast<uint32_t>(object->type()), handles.empty() ? 0 : handles[0],
                static_cast<uint32_t>(ctx.lr));
  if (object->guest_object() != native) {
    static int stale = 0;
    if (stale++ < 20)
      REXLOG_WARN("BF kernel: ObDereferenceObject({:08X}) skipped, the struct's handle {:08X} now names an object "
                  "of type {}", native, handles.empty() ? 0 : handles[0], static_cast<uint32_t>(object->type()));
    return;
  }
  if (handles.empty()) return;
  auto current = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XObject>(handles[0]);
  if (current.get() != object.get()) {
    static int logged = 0;
    if (logged++ < 20)
      REXLOG_WARN("BF kernel: ObDereferenceObject({:08X}) skipped, its handle {:08X} now names {}", native,
                  handles[0], current ? "another object" : "nothing");
    return;
  }
  object->ReleaseHandle();
}

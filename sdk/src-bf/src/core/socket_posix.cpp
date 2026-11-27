#include <rex/net/socket.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace rex::net {

int socket_close(SocketHandle handle) {
  return close(static_cast<int>(handle));
}

// The guest's ioctlsocket commands are the console's (Winsock's) numbers, and its argument is a big endian u_long in
// guest memory. Windows' ioctlsocket takes the same numbers; POSIX ioctl does not (FIONBIO is 0x5421 on Linux), so the
// command was refused and every socket stayed blocking: Battlefield 1943's DirtySock then waited in recv for the
// server's next packet, the game frozen for up to 30 s after its login (ROG Ally, run t7: gdb caught it in recv).
int socket_ioctl(SocketHandle handle, uint32_t cmd, uint8_t* arg) {
  const int fd = static_cast<int>(handle);
  constexpr uint32_t kWinsockFionbio = 0x8004667E;   // _IOW('f', 126, u_long)
  constexpr uint32_t kWinsockFionread = 0x4004667F;  // _IOR('f', 127, u_long)
  switch (cmd) {
    case kWinsockFionbio: {
      // Non zero (in either byte order) turns non blocking on.
      const bool on = arg && (arg[0] | arg[1] | arg[2] | arg[3]) != 0;
      const int flags = fcntl(fd, F_GETFL, 0);
      if (flags < 0) {
        return -1;
      }
      return fcntl(fd, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
    }
    case kWinsockFionread: {
      int available = 0;
      if (ioctl(fd, FIONREAD, &available) < 0) {
        return -1;
      }
      if (arg) {  // big endian, as the guest reads it
        const uint32_t value = static_cast<uint32_t>(available);
        arg[0] = uint8_t(value >> 24), arg[1] = uint8_t(value >> 16), arg[2] = uint8_t(value >> 8),
        arg[3] = uint8_t(value);
      }
      return 0;
    }
    default:
      return ioctl(fd, cmd, arg);
  }
}

}  // namespace rex::net

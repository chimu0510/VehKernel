// SPDX-License-Identifier: GPL-3.0-or-later
// The Link over a serial device (ADR-0002, ADR-0027).
#include <vehkernel_bridge/serial_transport.hpp>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

namespace vk::bridge {
namespace {

// The termios speed constant of a baud rate, 0 when there is none.
speed_t speed_of(int baud)
{
    switch (baud) {
    case 9600:
        return B9600;
    case 19200:
        return B19200;
    case 38400:
        return B38400;
    case 57600:
        return B57600;
    case 115200:
        return B115200;
    case 230400:
        return B230400;
    case 460800:
        return B460800;
    case 500000:
        return B500000;
    case 576000:
        return B576000;
    case 921600:
        return B921600;
    case 1000000:
        return B1000000;
    case 1152000:
        return B1152000;
    case 1500000:
        return B1500000;
    case 2000000:
        return B2000000;
    case 3000000:
        return B3000000;
    default:
        return 0;
    }
}

}  // namespace

SerialTransport::SerialTransport(std::string device, int baud)
    : device_(std::move(device)), baud_(baud), description_("serial " + device_ + " at " + std::to_string(baud_))
{
}

SerialTransport::~SerialTransport()
{
    close();
}

bool SerialTransport::baud_supported(int baud)
{
    return speed_of(baud) != 0;
}

bool SerialTransport::open()
{
    close();
    const speed_t speed = speed_of(baud_);
    if (speed == 0) {
        return false;
    }
    const int fd = ::open(device_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    termios mode{};
    if (tcgetattr(fd, &mode) != 0) {
        ::close(fd);
        return false;
    }
    // Raw 8N1: every byte passes unchanged, nothing is echoed. A read returns at
    // once with what is there.
    cfmakeraw(&mode);
    mode.c_cflag |= CLOCAL | CREAD;
    mode.c_cc[VMIN] = 0;
    mode.c_cc[VTIME] = 0;
    cfsetispeed(&mode, speed);
    cfsetospeed(&mode, speed);
    if (tcsetattr(fd, TCSANOW, &mode) != 0) {
        ::close(fd);
        return false;
    }
    tcflush(fd, TCIOFLUSH);  // bytes from before it was opened
    // Opening the port asserts DTR and RTS. On a port wired for one-key download, RTS
    // held asserts BOOT0, so a reset while connected would start the system
    // bootloader; both are released in one request, as the Link monitor does (a pty
    // has no modem lines, so a failure is no error).
    int lines = TIOCM_DTR | TIOCM_RTS;
    ioctl(fd, TIOCMBIC, &lines);
    fd_ = fd;
    return true;
}

void SerialTransport::close()
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

std::ptrdiff_t SerialTransport::read(std::span<std::uint8_t> out)
{
    if (fd_ < 0) {
        return transport_lost;
    }
    const ssize_t count = ::read(fd_, out.data(), out.size());
    if (count >= 0) {
        return count;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return 0;
    }
    return transport_lost;  // EIO or ENODEV: the adapter was unplugged
}

bool SerialTransport::write(std::span<const std::uint8_t> data)
{
    if (fd_ < 0) {
        return false;
    }
    while (!data.empty()) {
        const ssize_t count = ::write(fd_, data.data(), data.size());
        if (count > 0) {
            data = data.subspan(static_cast<std::size_t>(count));
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;  // the output queue is full: the rest is dropped
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace vk::bridge

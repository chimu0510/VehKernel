// SPDX-License-Identifier: GPL-3.0-or-later
// The Link over UDP (ADR-0018, ADR-0027).
#include <vehkernel_bridge/udp_transport.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

namespace vk::bridge {

UdpTransport::UdpTransport(std::string address, std::uint16_t port)
    : address_(std::move(address)), port_(port), description_("udp " + address_ + ":" + std::to_string(port_))
{
}

UdpTransport::~UdpTransport()
{
    close();
}

bool UdpTransport::open()
{
    close();
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port_);
    if (inet_pton(AF_INET, address_.c_str(), &peer.sin_addr) != 1) {
        return false;
    }
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    // Connecting a datagram socket only fixes the peer: datagrams from anyone else are
    // dropped, and a peer that is not there shows as ECONNREFUSED, which read() and
    // write() treat as silence.
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&peer), sizeof(peer)) != 0) {
        ::close(fd);
        return false;
    }
    socket_ = fd;
    return true;
}

void UdpTransport::close()
{
    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }
}

std::ptrdiff_t UdpTransport::read(std::span<std::uint8_t> out)
{
    if (socket_ < 0) {
        return transport_lost;
    }
    const ssize_t count = ::recv(socket_, out.data(), out.size(), 0);
    if (count >= 0) {
        return count;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNREFUSED || errno == EINTR) {
        return 0;
    }
    return transport_lost;
}

bool UdpTransport::write(std::span<const std::uint8_t> data)
{
    if (socket_ < 0) {
        return false;
    }
    if (::send(socket_, data.data(), data.size(), 0) >= 0) {
        return true;
    }
    // A full buffer or no listener drops the datagram, which UDP allows.
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNREFUSED || errno == EINTR;
}

}  // namespace vk::bridge

#include "ipc.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <utility>

using Pipe = std::array<Fd, 2>;
using Descriptors = std::array<int, 2>;

static Pipe makePipe()
{
    Descriptors descriptors{};
    if (pipe2(descriptors.data(), O_CLOEXEC | O_NONBLOCK) < 0) {
        fail("pipe2");
    }
    return {Fd(descriptors[0]), Fd(descriptors[1])};
}

static Pipe transfer(int socket, const Descriptors* descriptors, Deadline deadline)
{
    const bool sending = descriptors != nullptr;
    char marker = 'P';
    iovec payload{&marker, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(Descriptors))]{};
    msghdr message{};
    message.msg_iov = &payload;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);

    if (sending) {
        auto* header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(Descriptors));
        std::memcpy(CMSG_DATA(header), descriptors->data(), sizeof(Descriptors));
    }

    for (;;) {
        waitFd(socket, sending ? POLLOUT : POLLIN, deadline, "Передача дескрипторов pipe");
        const auto size = sending ? sendmsg(socket, &message, MSG_NOSIGNAL)
                                  : recvmsg(socket, &message, MSG_CMSG_CLOEXEC);
        if (size < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            fail(sending ? "sendmsg SCM_RIGHTS" : "recvmsg SCM_RIGHTS");
        }
        if (sending && size == 1) {
            return {};
        }

        Pipe result;
        std::size_t received = 0;
        for (auto* header = CMSG_FIRSTHDR(&message); header;
             header = CMSG_NXTHDR(&message, header)) {
            if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) {
                const auto count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                for (std::size_t index = 0; index < count; ++index) {
                    int value = -1;
                    std::memcpy(&value, CMSG_DATA(header) + index * sizeof(int), sizeof(int));
                    Fd fd(value);
                    if (received < result.size()) {
                        result[received] = std::move(fd);
                    }
                    ++received;
                }
            }
        }
        if (size != 1 || marker != 'P' || received != result.size()
            || (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC))) {
            throw std::runtime_error("Некорректная передача дескрипторов pipe");
        }
        return result;
    }
}

struct Conn::State {
    State(Session session, bool create)
        : path(session.path("pipe"), false),
          listener(create ? listenUnix(path, SOCK_SEQPACKET) : Fd{}),
          up(create ? makePipe() : Pipe{}), down(create ? makePipe() : Pipe{}), host(create) {}

    Path path;
    Fd listener;
    Pipe up;
    Pipe down;
    Fd input;
    Fd output;
    bool host;
};

Conn::Conn(Session session, bool create) : state_(std::make_unique<State>(session, create)) {}
Conn::~Conn() = default;

void Conn::Connect(pid_t peer, Deadline deadline)
{
    if (state_->host) {
        const auto service = acceptUnix(state_->listener.get(), peer, deadline);
        const Descriptors descriptors{state_->down[0].get(), state_->up[1].get()};
        transfer(service.get(), &descriptors, deadline);
        state_->input = std::move(state_->up[0]);
        state_->output = std::move(state_->down[1]);
        state_->up[1] = Fd{};
        state_->down[0] = Fd{};
        state_->listener = Fd{};
    } else {
        const auto service = connectUnix(state_->path, SOCK_SEQPACKET, peer, deadline);
        auto descriptors = transfer(service.get(), nullptr, deadline);
        state_->input = std::move(descriptors[0]);
        state_->output = std::move(descriptors[1]);
    }
}

void Conn::Read(void* buffer, std::size_t count, Deadline deadline)
{
    readAll(state_->input.get(), buffer, count, deadline);
}

void Conn::Write(const void* buffer, std::size_t count, Deadline deadline)
{
    writeAll(state_->output.get(), buffer, count, deadline);
}

const char* transportName() { return LAB_TRANSPORT; }

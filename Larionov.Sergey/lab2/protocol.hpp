#pragma once

#include "ipc.hpp"

#include <cstdint>
#include <stdexcept>

enum class Kind : std::int32_t {
    hello = 1, welcome, ping, pong, round, number, result, stop
};

struct Message {
    Kind kind;
    std::int32_t round = 0;
    std::int32_t value = 0;
};

static_assert(sizeof(Message) == 12);

class Channel {
public:
    Channel(Session session, bool host)
        : toHost_(session.semaphore("up"), host),
          toClient_(session.semaphore("down"), host),
          conn_(session, host),
          incoming_(host ? toHost_ : toClient_),
          outgoing_(host ? toClient_ : toHost_) {}

    void connect(pid_t peer) { conn_.Connect(peer, Clock::now() + ipcTimeout); }

    void send(Message message)
    {
        conn_.Write(&message, sizeof(message), Clock::now() + ipcTimeout);
        outgoing_.post();
    }

    Message receive()
    {
        const auto deadline = Clock::now() + ipcTimeout;
        incoming_.wait(deadline);
        Message message{};
        conn_.Read(&message, sizeof(message), deadline);
        return message;
    }

    void unlinkSemaphores()
    {
        toHost_.unlinkName();
        toClient_.unlinkName();
    }

private:
    NamedSemaphore toHost_;
    NamedSemaphore toClient_;
    Conn conn_;
    NamedSemaphore& incoming_;
    NamedSemaphore& outgoing_;
};

inline void expect(Message message, Kind kind, int round = 0)
{
    if (message.kind != kind || message.round != round) {
        throw std::runtime_error("Неожиданное сообщение протокола");
    }
}

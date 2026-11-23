#include "ipc.hpp"

struct Conn::State {
    State(Session session, bool create)
        : path(session.path("sock"), false),
          listener(create ? listenUnix(path, SOCK_STREAM) : Fd{}), host(create) {}

    Path path;
    Fd listener;
    Fd connection;
    bool host;
};

Conn::Conn(Session session, bool create) : state_(std::make_unique<State>(session, create)) {}
Conn::~Conn() = default;

void Conn::Connect(pid_t peer, Deadline deadline)
{
    state_->connection = state_->host ? acceptUnix(state_->listener.get(), peer, deadline)
                                      : connectUnix(state_->path, SOCK_STREAM, peer, deadline);
    state_->listener = Fd{};
}

void Conn::Read(void* buffer, std::size_t count, Deadline deadline)
{
    readAll(state_->connection.get(), buffer, count, deadline);
}

void Conn::Write(const void* buffer, std::size_t count, Deadline deadline)
{
    writeAll(state_->connection.get(), buffer, count, deadline);
}

const char* transportName() { return LAB_TRANSPORT; }

#include "ipc.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <thread>

static void makeFifo(Path& path)
{
    if (mkfifo(path.name().c_str(), 0600) < 0) {
        fail("mkfifo " + path.name());
    }
    path.own();
}

struct Conn::State {
    State(Session session, bool create)
        : up(session.path("up.fifo"), false), down(session.path("down.fifo"), false),
          outgoing(create ? down : up)
    {
        if (create) {
            makeFifo(up);
            makeFifo(down);
        }
        const Path& incoming = create ? up : down;
        input = Fd(open(incoming.name().c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    }

    Path up;
    Path down;
    const Path& outgoing;
    Fd input;
    Fd output;
};

Conn::Conn(Session session, bool create) : state_(std::make_unique<State>(session, create)) {}
Conn::~Conn() = default;

void Conn::Connect(pid_t, Deadline deadline)
{
    while (Clock::now() < deadline) {
        const int output = open(state_->outgoing.name().c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (output >= 0) {
            state_->output = Fd(output);
            return;
        }
        if (errno != ENXIO && errno != EINTR) {
            fail("Открытие FIFO " + state_->outgoing.name());
        }
        std::this_thread::sleep_until(std::min(deadline, Clock::now() + 20ms));
    }
    throw Timeout("Открытие FIFO");
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

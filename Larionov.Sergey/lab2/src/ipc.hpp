#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <semaphore.h>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
using namespace std::chrono_literals;
inline constexpr auto ipcTimeout = 5s;

class Timeout : public std::runtime_error {
public:
    explicit Timeout(const std::string& operation)
        : std::runtime_error(operation + ": превышен тайм-аут 5 с") {}
};

[[noreturn]] void fail(const std::string& operation, int error);
[[noreturn]] void fail(const std::string& operation);

class Fd {
public:
    Fd() = default;
    explicit Fd(int value);
    ~Fd();
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept;
    Fd& operator=(Fd&& other) noexcept;

    int get() const { return value_; }

private:
    int value_ = -1;
};

struct Session {
    pid_t host;
    int id;

    std::string directory() const;
    std::string path(const char* suffix) const;
    std::string semaphore(const char* suffix) const;
};

class Path {
public:
    Path(std::string name, bool directory);
    ~Path();
    Path(const Path&) = delete;
    Path& operator=(const Path&) = delete;

    const std::string& name() const { return name_; }
    void own() { owned_ = true; }

private:
    std::string name_;
    bool directory_;
    bool owned_ = false;
};

class Conn {
public:
    Conn(Session session, bool create);
    ~Conn();
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;

    void Connect(pid_t peer, Deadline deadline);
    void Read(void* buffer, std::size_t count, Deadline deadline);
    void Write(const void* buffer, std::size_t count, Deadline deadline);

private:
    struct State;
    std::unique_ptr<State> state_;
};

const char* transportName();
void waitFd(int fd, short events, Deadline deadline, const char* operation);
void readAll(int fd, void* buffer, std::size_t count, Deadline deadline);
void writeAll(int fd, const void* buffer, std::size_t count, Deadline deadline);

Fd listenUnix(Path& path, int type);
Fd acceptUnix(int listener, pid_t peer, Deadline deadline);
Fd connectUnix(const Path& path, int type, pid_t peer, Deadline deadline);

int transportCode();
sigset_t blockSignals(bool host);
void sendHandshake(pid_t recipient, int value);
int awaitHandshake(const sigset_t& signals, pid_t host);

class NamedSemaphore {
public:
    NamedSemaphore(std::string name, bool create);
    ~NamedSemaphore();
    NamedSemaphore(const NamedSemaphore&) = delete;
    NamedSemaphore& operator=(const NamedSemaphore&) = delete;

    void wait(Deadline deadline);
    void post();
    void unlinkName();

private:
    std::string name_;
    bool owned_;
    sem_t* semaphore_;
};

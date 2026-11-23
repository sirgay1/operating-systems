#include "ipc.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

void fail(const std::string& operation, int error)
{
    throw std::system_error(error, std::generic_category(), operation);
}

void fail(const std::string& operation)
{
    fail(operation, errno);
}

Fd::Fd(int value) : value_(value)
{
    if (value < 0) {
        fail("Получение файлового дескриптора");
    }
}

Fd::~Fd()
{
    if (value_ >= 0) {
        close(value_);
    }
}

Fd::Fd(Fd&& other) noexcept : value_(std::exchange(other.value_, -1)) {}

Fd& Fd::operator=(Fd&& other) noexcept
{
    std::swap(value_, other.value_);
    return *this;
}

std::string Session::directory() const
{
    return "/tmp/os-lab2-" + std::to_string(getuid()) + "-" + std::to_string(host);
}

std::string Session::path(const char* suffix) const
{
    return directory() + "/" + std::to_string(id) + "-" + suffix;
}

std::string Session::semaphore(const char* suffix) const
{
    return "/os-lab2-" + std::to_string(getuid()) + "-" + std::to_string(host)
        + "-" + std::to_string(id) + "-" + suffix;
}

Path::Path(std::string name, bool directory)
    : name_(std::move(name)), directory_(directory) {}

Path::~Path()
{
    if (owned_) {
        if (directory_) {
            rmdir(name_.c_str());
        } else {
            unlink(name_.c_str());
        }
    }
}

void waitFd(int fd, short events, Deadline deadline, const char* operation)
{
    pollfd descriptor{fd, events, 0};
    while (Clock::now() < deadline) {
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
            deadline - Clock::now());
        const int ready = poll(&descriptor, 1, std::max(0, static_cast<int>(remaining.count())));
        if (ready > 0) {
            if (descriptor.revents & POLLNVAL) {
                fail(operation, EBADF);
            }
            return;
        }
        if (ready < 0 && errno != EINTR) {
            fail(operation);
        }
    }
    throw Timeout(operation);
}

void readAll(int fd, void* buffer, std::size_t count, Deadline deadline)
{
    auto* bytes = static_cast<char*>(buffer);
    while (count > 0) {
        waitFd(fd, POLLIN, deadline, "Чтение");
        const auto amount = read(fd, bytes, count);
        if (amount == 0) {
            throw std::runtime_error("Канал закрыт другой стороной");
        }
        if (amount < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            fail("Чтение");
        }
        bytes += amount;
        count -= amount;
    }
}

void writeAll(int fd, const void* buffer, std::size_t count, Deadline deadline)
{
    auto* bytes = static_cast<const char*>(buffer);
    while (count > 0) {
        waitFd(fd, POLLOUT, deadline, "Запись");
        const auto amount = write(fd, bytes, count);
        if (amount < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            fail("Запись");
        }
        bytes += amount;
        count -= amount;
    }
}

static sockaddr_un addressFor(const Path& path)
{
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.name().size() >= sizeof(address.sun_path)) {
        fail("Путь Unix-сокета", ENAMETOOLONG);
    }
    std::memcpy(address.sun_path, path.name().c_str(), path.name().size() + 1);
    return address;
}

static bool isPeer(int socket, pid_t pid)
{
    ucred credentials{};
    socklen_t size = sizeof(credentials);
    if (getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &credentials, &size) < 0) {
        fail("SO_PEERCRED");
    }
    return credentials.pid == pid && credentials.uid == getuid();
}

Fd listenUnix(Path& path, int type)
{
    Fd listener(socket(AF_UNIX, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    const auto address = addressFor(path);
    if (bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        fail("bind " + path.name());
    }
    path.own();
    if (listen(listener.get(), 8) < 0) {
        fail("listen");
    }
    return listener;
}

Fd acceptUnix(int listener, pid_t peer, Deadline deadline)
{
    for (;;) {
        waitFd(listener, POLLIN, deadline, "accept");
        const int result = accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (result < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        }
        Fd connection(result);
        if (isPeer(connection.get(), peer)) {
            return connection;
        }
    }
}

Fd connectUnix(const Path& path, int type, pid_t peer, Deadline deadline)
{
    Fd connection(socket(AF_UNIX, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    const auto address = addressFor(path);
    if (connect(connection.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        if (errno != EINPROGRESS) {
            fail("connect " + path.name());
        }
        waitFd(connection.get(), POLLOUT, deadline, "connect");
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(connection.get(), SOL_SOCKET, SO_ERROR, &error, &size) < 0) {
            fail("SO_ERROR");
        }
        if (error != 0) {
            fail("connect " + path.name(), error);
        }
    }
    if (!isPeer(connection.get(), peer)) {
        throw std::runtime_error("Сокет принадлежит другому процессу");
    }
    return connection;
}

sigset_t blockSignals(bool host)
{
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGUSR1);
    if (host) {
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGTERM);
    }
    const int error = pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    if (error != 0) {
        fail("pthread_sigmask", error);
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        fail("signal SIGPIPE");
    }
    return signals;
}

int transportCode()
{
    int code = 0;
    for (const char* character = transportName(); *character != '\0'; ++character) {
        code = (code * 31 + *character) % 65521;
    }
    return code;
}

void sendHandshake(pid_t recipient, int value)
{
    union sigval payload{};
    payload.sival_int = value;
    if (sigqueue(recipient, SIGUSR1, payload) < 0) {
        fail("Отправка SIGUSR1");
    }
}

int awaitHandshake(const sigset_t& signals, pid_t host)
{
    const auto deadline = Clock::now() + ipcTimeout;
    auto nextRequest = Clock::now();
    while (Clock::now() < deadline) {
        const auto now = Clock::now();
        if (now >= nextRequest) {
            sendHandshake(host, transportCode());
            nextRequest = now + 200ms;
        }
        const auto wait = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::min(deadline, nextRequest) - now);
        const timespec timeout{static_cast<time_t>(wait.count() / 1000000000),
                               static_cast<long>(wait.count() % 1000000000)};
        siginfo_t info{};
        const int result = sigtimedwait(&signals, &info, &timeout);
        if (result < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            fail("Ожидание SIGUSR1");
        }
        if (info.si_pid == host && info.si_uid == getuid() && info.si_code == SI_QUEUE) {
            if (info.si_value.sival_int <= 0) {
                throw std::runtime_error("Хост отклонил подключение: проверьте тип транспорта");
            }
            return info.si_value.sival_int;
        }
    }
    throw Timeout("Ожидание ответа хоста по SIGUSR1");
}

NamedSemaphore::NamedSemaphore(std::string name, bool create)
    : name_(std::move(name)), owned_(create),
      semaphore_(create ? sem_open(name_.c_str(), O_CREAT | O_EXCL, 0600, 0)
                        : sem_open(name_.c_str(), 0))
{
    if (semaphore_ == SEM_FAILED) {
        fail("sem_open " + name_);
    }
}

NamedSemaphore::~NamedSemaphore()
{
    if (owned_) {
        sem_unlink(name_.c_str());
    }
    sem_close(semaphore_);
}

void NamedSemaphore::wait(Deadline deadline)
{
    const auto remaining = std::max(Clock::duration::zero(), deadline - Clock::now());
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        fail("clock_gettime");
    }
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count();
    now.tv_sec += nanos / 1000000000;
    now.tv_nsec += nanos % 1000000000;
    now.tv_sec += now.tv_nsec / 1000000000;
    now.tv_nsec %= 1000000000;
    while (sem_clockwait(semaphore_, CLOCK_MONOTONIC, &now) < 0) {
        if (errno == ETIMEDOUT) {
            throw Timeout("Ожидание семафора");
        }
        if (errno != EINTR) {
            fail("sem_clockwait");
        }
    }
}

void NamedSemaphore::post()
{
    if (sem_post(semaphore_) < 0) {
        fail("sem_post");
    }
}

void NamedSemaphore::unlinkName()
{
    if (owned_) {
        if (sem_unlink(name_.c_str()) < 0) {
            fail("sem_unlink " + name_);
        }
        owned_ = false;
    }
}

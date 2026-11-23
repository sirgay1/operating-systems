#include "game.hpp"
#include "protocol.hpp"

#include <charconv>
#include <iostream>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

static pid_t parsePid(std::string_view text)
{
    pid_t pid = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), pid);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || pid <= 1
        || pid == getpid()) {
        throw std::runtime_error("Укажите корректный PID хоста");
    }
    return pid;
}

static void checkHost(pid_t pid)
{
    struct stat attributes{};
    const auto path = Session{pid, 0}.directory();
    if (lstat(path.c_str(), &attributes) < 0) {
        fail("Хост недоступен " + path);
    }
    if (!S_ISDIR(attributes.st_mode) || attributes.st_uid != getuid()
        || (attributes.st_mode & 0777) != 0700) {
        throw std::runtime_error("Некорректный каталог хоста");
    }
}

static void play(Channel& channel)
{
    int lastRound = 0;
    bool alive = true;
    std::mt19937 random(std::random_device{}());
    for (;;) {
        const auto message = channel.receive();
        switch (message.kind) {
        case Kind::stop:
            expect(message, Kind::stop);
            std::cout << "Хост завершил сеанс" << std::endl;
            return;
        case Kind::ping:
            expect(message, Kind::ping);
            channel.send({Kind::pong});
            break;
        case Kind::round: {
            if (message.round <= lastRound || message.value != static_cast<int>(alive)) {
                throw std::runtime_error("Некорректное состояние козлёнка в запросе хода");
            }
            lastRound = message.round;
            const int number = roll(random, alive);
            channel.send({Kind::number, lastRound, number});
            const auto result = channel.receive();
            expect(result, Kind::result, lastRound);
            if (result.value != 0 && result.value != 1) {
                throw std::runtime_error("Некорректный статус козлёнка");
            }
            alive = result.value == 1;
            std::cout << "Ход " << lastRound << " · число: " << number << " · статус: "
                      << (alive ? "жив" : "мёртв") << std::endl;
            break;
        }
        default:
            throw std::runtime_error("Неожиданное сообщение от хоста");
        }
    }
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "Использование: " << argv[0] << " PID_хоста" << std::endl;
        return 2;
    }
    try {
        const auto host = parsePid(argv[1]);
        const auto signals = blockSignals(false);
        checkHost(host);
        const auto id = awaitHandshake(signals, host);
        Channel channel({host, id}, false);
        channel.connect(host);
        channel.send({Kind::hello});
        expect(channel.receive(), Kind::welcome);
        std::cout << "Подключился как козлёнок " << id << " · " << transportName() << std::endl;
        play(channel);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Ошибка клиента: " << error.what() << std::endl;
        return 1;
    }
}

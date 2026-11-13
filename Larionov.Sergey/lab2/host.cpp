#include "game.hpp"
#include "protocol.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>

struct Turn {
    int round;
    int wolf;
    int players;
    bool alive;
};

struct RollResult {
    int number;
    Outcome outcome;
};

class Host;

class ClientWorker {
public:
    ClientWorker(Host& host, Session session, pid_t peer)
        : host_(host), session_(session), peer_(peer) {}

    ~ClientWorker()
    {
        stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void start() { thread_ = std::thread(&ClientWorker::run, this); }

    void play(Turn turn)
    {
        {
            std::lock_guard lock(mutex_);
            turn_ = turn;
        }
        changed_.notify_one();
    }

    void stop()
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        changed_.notify_one();
    }

private:
    void run();
    void exchange(Channel& channel, Turn turn);

    Host& host_;
    Session session_;
    pid_t peer_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<Turn> turn_;
    bool stopping_ = false;
    std::thread thread_;
};

class Host : public QWidget {
public:
    explicit Host(const sigset_t& signalMask)
        : directory_(Session{getpid(), 0}.directory(), true),
          signalFd_(signalfd(-1, &signalMask, SFD_NONBLOCK | SFD_CLOEXEC))
    {
        if (mkdir(directory_.name().c_str(), 0700) < 0) {
            fail("mkdir " + directory_.name());
        }
        directory_.own();

        setWindowTitle(QStringLiteral("Волк и семеро козлят — %1")
                           .arg(QString::fromLatin1(transportName())));
        resize(780, 540);
        auto* layout = new QVBoxLayout(this);
        const auto title = QStringLiteral("Волк и семеро козлят");
        auto* heading = new QLabel(title, this);
        auto font = heading->font();
        font.setPointSize(19);
        font.setBold(true);
        heading->setFont(font);
        layout->addWidget(heading);
        auto* connection = new QLabel(
            QStringLiteral("Подключение из терминала  ·  ./client_%1 %2")
                .arg(QString::fromLatin1(transportName())).arg(getpid()), this);
        connection->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(connection);
        status_ = new QLabel(QStringLiteral("Ожидание козлят"), this);
        layout->addWidget(status_);
        log_ = new QPlainTextEdit(this);
        log_->setReadOnly(true);
        log_->setMaximumBlockCount(1500);
        layout->addWidget(log_, 1);

        auto* controls = new QHBoxLayout;
        controls->addWidget(new QLabel(QStringLiteral("Число волка"), this));
        input_ = new QSpinBox(this);
        input_->setObjectName(QStringLiteral("wolfNumber"));
        input_->setRange(1, 100);
        input_->setValue(50);
        controls->addWidget(input_);
        submit_ = new QPushButton(QStringLiteral("Сделать ход"), this);
        controls->addWidget(submit_);
        controls->addStretch();
        layout->addLayout(controls);
        setInputEnabled(false);

        moveTimer_.setSingleShot(true);
        moveTimer_.setTimerType(Qt::PreciseTimer);
        connect(&moveTimer_, &QTimer::timeout, this, [this] { startRound(roll(random_, true), true); });
        connect(submit_, &QPushButton::clicked, this, [this] { startRound(input_->value(), false); });

        connect(&signalTimer_, &QTimer::timeout, this, [this] { readSignals(); });
        signalTimer_.start(25);
        log(QStringLiteral("Хост готов · %1").arg(connection->text()));
    }

    void joined(int id)
    {
        auto& client = clients_.at(id);
        client.ready = true;
        log(QStringLiteral("Козлёнок %1 подключился (PID %2)").arg(id).arg(client.pid));
        prepareRound();
    }

    void received(int id, int round, RollResult result)
    {
        if (round != round_ || pending_.erase(id) == 0) {
            return;
        }
        clients_.at(id).alive = isAlive(result.outcome);
        results_.emplace(id, result);
        finishRoundIfReady();
    }

    void disconnected(int id, const QString& reason, bool error)
    {
        const QString message = QStringLiteral("Козлёнок %1 отключился · %2").arg(id).arg(reason);
        log(message, error);
        clients_.erase(id);
        pending_.erase(id);
        if (closing_) {
            if (clients_.empty()) {
                close();
            }
        } else if (roundRunning_) {
            finishRoundIfReady();
        } else {
            prepareRound();
        }
    }

protected:
    void closeEvent(QCloseEvent* event) override
    {
        if (!closing_) {
            closing_ = true;
            moveTimer_.stop();
            setInputEnabled(false);
            status_->setText(QStringLiteral("Завершение работы"));
            log(QStringLiteral("Хост завершает работу"));
            stopClients();
        }
        event->setAccepted(clients_.empty());
    }

private:
    struct Client {
        pid_t pid;
        std::unique_ptr<ClientWorker> worker;
        bool ready = false;
        bool alive = true;
    };

    void log(const QString& message, bool error = false)
    {
        log_->appendPlainText(message);
        (error ? std::cerr : std::cout) << message.toStdString() << std::endl;
    }

    void setInputEnabled(bool enabled)
    {
        input_->setEnabled(enabled);
        submit_->setEnabled(enabled);
    }

    void readSignals()
    {
        for (int count = 0; count < 64; ++count) {
            signalfd_siginfo info{};
            const auto size = read(signalFd_.get(), &info, sizeof(info));
            if (size < 0 && errno == EAGAIN) {
                return;
            }
            if (size < 0 && errno == EINTR) {
                continue;
            }
            if (size != sizeof(info)) {
                log(QStringLiteral("Ошибка чтения signalfd"));
                signalTimer_.stop();
                close();
                return;
            }
            if (info.ssi_signo == SIGINT || info.ssi_signo == SIGTERM) {
                close();
                return;
            }
            if (info.ssi_signo == SIGUSR1 && info.ssi_uid == getuid()
                && static_cast<int>(info.ssi_code) == SI_QUEUE && info.ssi_pid > 0) {
                registerClient(static_cast<pid_t>(info.ssi_pid), info.ssi_int);
            }
        }
    }

    void registerClient(pid_t pid, int transport)
    {
        for (const auto& entry : clients_) {
            if (entry.second.pid == pid) {
                return;
            }
        }

        int id = 0;
        try {
            if (closing_ || finished_ || transport != transportCode()
                || nextId_ == std::numeric_limits<int>::max()) {
                sendHandshake(pid, 0);
                return;
            }
            id = nextId_++;
            auto worker = std::make_unique<ClientWorker>(*this, Session{getpid(), id}, pid);
            auto entry = clients_.emplace(id, Client{pid, std::move(worker)}).first;
            entry->second.worker->start();
        } catch (const std::exception& error) {
            if (id > 0) {
                clients_.erase(id);
            }
            log(QStringLiteral("Ошибка подключения PID %1 · %2")
                    .arg(pid).arg(QString::fromUtf8(error.what())));
        }
    }

    void prepareRound()
    {
        if (roundRunning_ || finished_ || closing_) {
            return;
        }
        const bool hasPlayers = std::any_of(clients_.begin(), clients_.end(), [](const auto& entry) {
            return entry.second.ready;
        });
        if (!hasPlayers) {
            moveTimer_.stop();
            setInputEnabled(false);
            allDeadRounds_ = 0;
            status_->setText(QStringLiteral("Ожидание козлят"));
        } else if (!moveTimer_.isActive()) {
            status_->setText(QStringLiteral("Ход %1 · введите число в течение 3 секунд").arg(round_ + 1));
            setInputEnabled(true);
            moveTimer_.start(3000);
        }
    }

    void startRound(int wolf, bool automatic)
    {
        if (!submit_->isEnabled()) {
            return;
        }
        moveTimer_.stop();
        setInputEnabled(false);
        for (const auto& entry : clients_) {
            if (entry.second.ready) {
                pending_.insert(entry.first);
            }
        }
        const auto players = static_cast<int>(pending_.size());
        roundRunning_ = true;
        ++round_;
        status_->setText(QStringLiteral("Ход %1 · ожидание чисел козлят").arg(round_));
        log(QStringLiteral("Ход %1 · волк: %2 (%3) · участников: %4")
                .arg(round_).arg(wolf)
                .arg(automatic ? QStringLiteral("случайно") : QStringLiteral("вручную"))
                .arg(players));
        for (const auto id : pending_) {
            auto& client = clients_.at(id);
            client.worker->play(Turn{round_, wolf, players, client.alive});
        }
    }

    void finishRoundIfReady()
    {
        if (!pending_.empty()) {
            return;
        }
        roundRunning_ = false;
        if (results_.empty()) {
            allDeadRounds_ = 0;
            log(QStringLiteral("Ход %1 прерван · никто не ответил").arg(round_));
            prepareRound();
            return;
        }

        int hidden = 0;
        int caught = 0;
        int revived = 0;
        int dead = 0;
        for (const auto& [id, result] : results_) {
            QString label;
            switch (result.outcome) {
            case Outcome::hidden:
                ++hidden;
                label = QStringLiteral("спрятался");
                break;
            case Outcome::caught:
                ++caught;
                label = QStringLiteral("попался");
                break;
            case Outcome::revived:
                ++revived;
                label = QStringLiteral("воскрес");
                break;
            case Outcome::dead:
                label = QStringLiteral("остался мёртвым");
                break;
            }
            dead += !isAlive(result.outcome);
            log(QStringLiteral("Козлёнок %1: %2 · %3").arg(id).arg(result.number).arg(label));
        }
        const bool anyAlive = std::any_of(clients_.begin(), clients_.end(), [](const auto& entry) {
            return entry.second.ready && entry.second.alive;
        });
        allDeadRounds_ = anyAlive || dead == 0 ? 0 : allDeadRounds_ + 1;
        results_.clear();
        log(QStringLiteral("Спрятались: %1 · попались: %2 · мертвы: %3 · воскресли: %4")
                .arg(hidden).arg(caught).arg(dead).arg(revived));

        if (allDeadRounds_ == 2) {
            finished_ = true;
            status_->setText(QStringLiteral("Игра завершилась"));
            log(QStringLiteral("Игра завершилась: все козлята мертвы два хода подряд"));
            stopClients();
        } else {
            prepareRound();
        }
    }

    void stopClients()
    {
        for (const auto& entry : clients_) {
            entry.second.worker->stop();
        }
    }

    Path directory_;
    Fd signalFd_;
    QLabel* status_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QSpinBox* input_ = nullptr;
    QPushButton* submit_ = nullptr;
    QTimer signalTimer_;
    QTimer moveTimer_;
    std::map<int, Client> clients_;
    std::set<int> pending_;
    std::map<int, RollResult> results_;
    std::mt19937 random_{std::random_device{}()};
    int nextId_ = 1;
    int round_ = 0;
    int allDeadRounds_ = 0;
    bool roundRunning_ = false;
    bool finished_ = false;
    bool closing_ = false;
};

void ClientWorker::exchange(Channel& channel, Turn turn)
{
    channel.send({Kind::round, turn.round, turn.alive});
    const auto reply = channel.receive();
    expect(reply, Kind::number, turn.round);
    if (reply.value < 1 || reply.value > maximumRoll(turn.alive)) {
        throw std::runtime_error("Число козлёнка вне допустимого диапазона");
    }
    const RollResult result{reply.value, evaluateRoll(turn.alive, turn.wolf, reply.value, turn.players)};
    channel.send({Kind::result, turn.round, isAlive(result.outcome)});
    QMetaObject::invokeMethod(&host_, [host = &host_, id = session_.id, round = turn.round, result] {
        host->received(id, round, result);
    }, Qt::QueuedConnection);
}

void ClientWorker::run()
{
    QString reason = QStringLiteral("хост завершил сеанс");
    bool failed = false;
    try {
        Channel channel(session_, true);
        sendHandshake(peer_, session_.id);
        channel.connect(peer_);
        expect(channel.receive(), Kind::hello);
        channel.unlinkSemaphores();
        channel.send({Kind::welcome});
        QMetaObject::invokeMethod(&host_, [host = &host_, id = session_.id] {
            host->joined(id);
        }, Qt::QueuedConnection);

        for (;;) {
            std::unique_lock lock(mutex_);
            changed_.wait_for(lock, 1s, [this] { return turn_.has_value() || stopping_; });
            const bool stopping = stopping_;
            const auto turn = std::exchange(turn_, std::nullopt);
            lock.unlock();
            if (stopping) {
                channel.send({Kind::stop});
                break;
            }
            if (turn) {
                exchange(channel, *turn);
            } else {
                channel.send({Kind::ping});
                expect(channel.receive(), Kind::pong);
            }
        }
    } catch (const std::exception& error) {
        failed = true;
        reason = QString::fromUtf8(error.what());
    }
    QMetaObject::invokeMethod(&host_, [host = &host_, id = session_.id, reason, failed] {
        host->disconnected(id, reason, failed);
    }, Qt::QueuedConnection);
}

int main(int argc, char** argv)
{
    try {
        const auto signalMask = blockSignals(true);
        QApplication app(argc, argv);
        Host host(signalMask);
        host.show();
        return app.exec();
    } catch (const std::exception& error) {
        std::cerr << "Ошибка хоста: " << error.what() << std::endl;
        return 1;
    }
}

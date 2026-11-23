#!/usr/bin/env python3
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor


PROJECT = Path(__file__).resolve().parents[1]


def wait_for(log, pattern, timeout=7):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        match = re.search(pattern, log.read_text(encoding="utf-8"))
        if match:
            return match
        time.sleep(0.025)
    raise AssertionError(f"Missing {pattern!r} in {log.read_text(encoding='utf-8')!r}")


def session_directory(host):
    return Path(f"/tmp/os-lab2-{os.getuid()}-{host.pid}")


def check_cleanup(host):
    assert not session_directory(host).exists(), "Host directory was not removed"
    semaphores = Path("/dev/shm").glob(f"sem.os-lab2-{os.getuid()}-{host.pid}-*")
    assert not list(semaphores), "Named semaphores were not removed"


def check_transport(transport):
    processes = []
    hosts = []

    with tempfile.TemporaryDirectory(prefix=f"lab2-test-{transport}-") as temporary:
        directory = Path(temporary)

        def launch(name, executable, *arguments):
            log = directory / f"{name}.txt"
            with log.open("wb") as output:
                process = subprocess.Popen(
                    [str(PROJECT / executable), *map(str, arguments)],
                    cwd=PROJECT,
                    stdout=output,
                    stderr=subprocess.STDOUT,
                    env={**os.environ, "QT_QPA_PLATFORM": "offscreen"},
                )
            processes.append(process)
            return process, log

        try:
            host, host_log = launch("host", f"host_{transport}")
            hosts.append(host)
            wait_for(host_log, "Хост готов")
            time.sleep(0.25)
            assert "Ход 1" not in host_log.read_text(encoding="utf-8")

            wrong_transport = "fifo" if transport == "pipe" else "pipe"
            wrong, wrong_log = launch("wrong", f"client_{wrong_transport}", host.pid)
            assert wrong.wait(timeout=6) == 1
            wait_for(wrong_log, "проверьте тип транспорта")

            kids = [
                launch(f"kid-{index}", f"client_{transport}", host.pid)
                for index in range(6)
            ]
            identifiers = {
                wait_for(log, r"Подключился как козлёнок (\d+)").group(1)
                for _, log in kids
            }
            assert len(identifiers) == len(kids)
            wait_for(host_log, r"Ход 1 · волк: \d+ \(случайно\)")
            for _, log in kids:
                wait_for(log, r"Ход 1 · число: \d+ · статус:")
            wait_for(host_log, r"Спрятались: \d+ · попались: \d+ · мертвы: \d+")

            victim, _ = kids.pop()
            victim.send_signal(signal.SIGSTOP)
            wait_for(host_log, "отключился · Ожидание семафора: превышен тайм-аут 5 с")
            victim.kill()
            victim.wait(timeout=3)
            for _, log in kids:
                assert "Ошибка клиента" not in log.read_text(encoding="utf-8")

            host.terminate()
            assert host.wait(timeout=7) == 0
            for kid, log in kids:
                assert kid.wait(timeout=7) == 0, log.read_text(encoding="utf-8")
                wait_for(log, "Хост завершил сеанс")
            check_cleanup(host)

            host, host_log = launch("abandoned-host", f"host_{transport}")
            hosts.append(host)
            wait_for(host_log, "Хост готов")
            kid, kid_log = launch("abandoned-kid", f"client_{transport}", host.pid)
            wait_for(kid_log, "Подключился")
            host.kill()
            host.wait(timeout=3)
            started = time.monotonic()
            assert kid.wait(timeout=7) == 1
            assert 4 < time.monotonic() - started < 6.5
            wait_for(kid_log, "превышен тайм-аут 5 с")

            print(
                f"PASS {transport}: concurrent connections, transport rejection, "
                "random round, 5-second timeouts, graceful cleanup",
                flush=True,
            )
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=3)
            # SIGKILL prevents the host from removing its filesystem entries.
            for host in hosts:
                path = session_directory(host)
                if path.exists():
                    for entry in path.iterdir():
                        entry.unlink()
                    path.rmdir()
                check_cleanup(host)


if __name__ == "__main__":
    with ThreadPoolExecutor(max_workers=3) as pool:
        list(pool.map(check_transport, ("pipe", "fifo", "sock")))

import socket
import subprocess
import sys
import time

SERVER, NET_STRESS = sys.argv[1:3]


def main():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    server = subprocess.Popen([SERVER, str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        for _ in range(100):
            if server.poll() is not None:
                raise SystemExit(f"server exited: {server.stderr.read().decode()}")
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                    break
            except OSError:
                time.sleep(0.02)
        else:
            raise SystemExit("server did not start")

        # (clients, seconds, keys, value bytes): many keys, one hot key, and large values.
        for clients, seconds, keys, size in ((16, 1, 64, 100), (16, 1, 1, 1000), (8, 1, 8, 100000)):
            result = subprocess.run(
                [NET_STRESS, str(port), str(clients), str(seconds), str(keys), str(size)],
                capture_output=True, text=True, timeout=30,
            )
            print(result.stdout.strip())
            if result.returncode != 0:
                raise SystemExit(f"net_stress failed: {result.stdout}{result.stderr}")
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait(timeout=5)
        output = server.stderr.read().decode(errors="replace")
        server.stderr.close()
        if any(m in output for m in ("ThreadSanitizer", "AddressSanitizer", "runtime error")):
            raise SystemExit(f"server reported a sanitizer error:\n{output}")


main()

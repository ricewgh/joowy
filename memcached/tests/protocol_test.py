import concurrent.futures
import socket
import subprocess
import sys
import time
import unittest


SERVER, CLI = sys.argv[1:3]
sys.argv = sys.argv[:1]


SANITIZER_MARKERS = ("ThreadSanitizer", "AddressSanitizer", "runtime error")


def stop_server(server):
    server.terminate()
    try:
        server.wait(timeout=5)
    except subprocess.TimeoutExpired:
        server.kill()
        server.wait(timeout=5)
    output = server.stderr.read().decode(errors="replace")
    server.stderr.close()
    return output


def start_server(*extra_args):
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    server = subprocess.Popen(
        [SERVER, str(port), *extra_args], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE
    )
    for _ in range(100):
        if server.poll() is not None:
            raise RuntimeError(f"server exited: {server.stderr.read().decode()}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return server, port
        except OSError:
            time.sleep(0.02)
    stop_server(server)
    raise RuntimeError("server did not start")


class ServerTestCase(unittest.TestCase):
    server_args = ()

    @classmethod
    def setUpClass(cls):
        cls.server, cls.port = start_server(*cls.server_args)

    @classmethod
    def tearDownClass(cls):
        output = stop_server(cls.server)
        if any(marker in output for marker in SANITIZER_MARKERS):
            raise AssertionError(f"server reported a sanitizer error:\n{output}")

    def connect(self):
        sock = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        self.addCleanup(sock.close)
        stream = sock.makefile("rb")
        self.addCleanup(stream.close)
        return sock, stream


class ProtocolTest(ServerTestCase):
    def test_set_get_delete_and_pipeline(self):
        sock, stream = self.connect()
        sock.sendall(
            b"set a 42 0 7\r\nhi\r\n\x00ok\r\n"
            b"set empty 3 0 0\r\n\r\n"
            b"get a missing empty\r\n"
            b"delete a\r\ndelete a\r\nget a\r\n"
        )
        self.assertEqual(stream.readline(), b"STORED\r\n")
        self.assertEqual(stream.readline(), b"STORED\r\n")
        self.assertEqual(stream.readline(), b"VALUE a 42 7\r\n")
        self.assertEqual(stream.read(9), b"hi\r\n\x00ok\r\n")
        self.assertEqual(stream.readline(), b"VALUE empty 3 0\r\n")
        self.assertEqual(stream.read(2), b"\r\n")
        self.assertEqual(stream.readline(), b"END\r\n")
        self.assertEqual(stream.readline(), b"DELETED\r\n")
        self.assertEqual(stream.readline(), b"NOT_FOUND\r\n")
        self.assertEqual(stream.readline(), b"END\r\n")

    def test_fragmentation_overwrite_and_noreply(self):
        sock, stream = self.connect()
        for fragment in (b"se", b"t frag 1 0 3\r", b"\na", b"bc\r", b"\n"):
            sock.sendall(fragment)
        self.assertEqual(stream.readline(), b"STORED\r\n")
        sock.sendall(b"set frag 9 0 1 noreply\r\nx\r\nget frag\r\n")
        self.assertEqual(stream.readline(), b"VALUE frag 9 1\r\n")
        self.assertEqual(stream.read(3), b"x\r\n")
        self.assertEqual(stream.readline(), b"END\r\n")
        sock.sendall(b"delete frag noreply\r\nget frag\r\n")
        self.assertEqual(stream.readline(), b"END\r\n")

    def test_expiration(self):
        sock, stream = self.connect()
        sock.sendall(b"set past 0 2592001 1\r\nx\r\nget past\r\ndelete past\r\n")
        self.assertEqual(stream.readline(), b"STORED\r\n")
        self.assertEqual(stream.readline(), b"END\r\n")
        self.assertEqual(stream.readline(), b"NOT_FOUND\r\n")
        sock.sendall(b"set ttl 0 1 1\r\nx\r\n")
        self.assertEqual(stream.readline(), b"STORED\r\n")
        time.sleep(1.1)
        sock.sendall(b"get ttl\r\n")
        self.assertEqual(stream.readline(), b"END\r\n")

    def test_invalid_requests(self):
        sock, stream = self.connect()
        sock.sendall(b"unknown\r\nget\r\ndelete\r\nget " + b"k" * 251 + b"\r\n")
        self.assertEqual(stream.readline(), b"ERROR\r\n")
        self.assertTrue(stream.readline().startswith(b"CLIENT_ERROR"))
        self.assertTrue(stream.readline().startswith(b"CLIENT_ERROR"))
        self.assertTrue(stream.readline().startswith(b"CLIENT_ERROR"))
        sock.sendall(b"set k 0 0 1048577\r\n")
        self.assertEqual(stream.readline(), b"SERVER_ERROR object too large\r\n")
        self.assertEqual(stream.read(), b"")

    def test_exact_key_and_value_limits(self):
        sock, stream = self.connect()
        key = b"k" * 250
        value = b"x" * (1024 * 1024)
        sock.sendall(b"set " + key + b" 4294967295 0 1048576\r\n" + value + b"\r\n")
        self.assertEqual(stream.readline(), b"STORED\r\n")
        sock.sendall(b"get " + key + b"\r\n")
        self.assertEqual(stream.readline(), b"VALUE " + key + b" 4294967295 1048576\r\n")
        self.assertEqual(stream.read(len(value) + 2), value + b"\r\n")
        self.assertEqual(stream.readline(), b"END\r\n")

    def test_bad_data_chunk_is_rejected(self):
        sock, stream = self.connect()
        sock.sendall(b"set malformed 0 0 1\r\nx!!")
        self.assertEqual(stream.readline(), b"CLIENT_ERROR bad data chunk\r\n")
        self.assertEqual(stream.read(), b"")

    def test_get_hit_is_not_delayed(self):
        # A get hit used to be sent as two writes, which Nagle and delayed ACK stalled for ~40ms.
        sock, stream = self.connect()
        sock.sendall(b"set k 0 0 5\r\nhello\r\n")
        self.assertEqual(stream.readline(), b"STORED\r\n")
        start = time.monotonic()
        for _ in range(100):
            sock.sendall(b"get k\r\n")
            self.assertEqual(stream.readline(), b"VALUE k 0 5\r\n")
            self.assertEqual(stream.read(7), b"hello\r\n")
            self.assertEqual(stream.readline(), b"END\r\n")
        self.assertLess(time.monotonic() - start, 2.0)

    def test_half_close_still_receives_responses(self):
        sock, stream = self.connect()
        sock.sendall(b"set half 0 0 2\r\nok\r\nget half\r\n")
        sock.shutdown(socket.SHUT_WR)
        self.assertEqual(
            stream.read(), b"STORED\r\nVALUE half 0 2\r\nok\r\nEND\r\n"
        )

    def test_large_pipelined_responses_wait_for_a_slow_reader(self):
        sock, stream = self.connect()
        value = b"v" * (1024 * 1024)
        sock.sendall(b"set big 0 0 1048576\r\n" + value + b"\r\n")
        self.assertEqual(stream.readline(), b"STORED\r\n")
        count = 20
        sock.sendall(b"get big\r\n" * count)
        time.sleep(0.5)  # the server must hold back, not buffer or drop responses
        for _ in range(count):
            self.assertEqual(stream.readline(), b"VALUE big 0 1048576\r\n")
            self.assertEqual(stream.read(len(value) + 2), value + b"\r\n")
            self.assertEqual(stream.readline(), b"END\r\n")
        sock.sendall(b"delete big\r\n")
        self.assertEqual(stream.readline(), b"DELETED\r\n")

    def test_get_with_too_many_keys_is_rejected(self):
        sock, stream = self.connect()
        sock.sendall(b"get " + b" ".join(b"k%d" % i for i in range(101)) + b"\r\n")
        self.assertEqual(stream.readline(), b"CLIENT_ERROR too many keys\r\n")
        sock.sendall(b"get k1\r\n")
        self.assertEqual(stream.readline(), b"END\r\n")

    def test_concurrent_clients(self):
        def worker(worker_id):
            with socket.create_connection(("127.0.0.1", self.port), timeout=5) as sock:
                with sock.makefile("rb") as stream:
                    key = f"worker-{worker_id}".encode()
                    for i in range(100):
                        value = f"{worker_id}-{i}".encode()
                        sock.sendall(
                            b"set " + key + b" 0 0 " + str(len(value)).encode()
                            + b"\r\n" + value + b"\r\nget " + key + b"\r\n"
                            + b"delete " + key + b"\r\n"
                        )
                        self.assertEqual(stream.readline(), b"STORED\r\n")
                        self.assertEqual(
                            stream.readline(), b"VALUE " + key + b" 0 "
                            + str(len(value)).encode() + b"\r\n"
                        )
                        self.assertEqual(stream.read(len(value) + 2), value + b"\r\n")
                        self.assertEqual(stream.readline(), b"END\r\n")
                        self.assertEqual(stream.readline(), b"DELETED\r\n")

        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as executor:
            list(executor.map(worker, range(12)))

    def test_cli(self):
        result = subprocess.run(
            [CLI, str(self.port)],
            input='set human "hello world"\nget human\ndelete human\nget human\nquit\n',
            text=True,
            capture_output=True,
            timeout=5,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("STORED", result.stdout)
        self.assertIn('VALUE human flags=0 bytes=11 "hello world"', result.stdout)
        self.assertIn("DELETED", result.stdout)
        self.assertTrue(result.stdout.count("END") >= 2)


class IdleTimeoutTest(ServerTestCase):
    server_args = ("256", "1")

    def test_idle_connection_is_closed_but_active_one_is_kept(self):
        idle_sock, _ = self.connect()
        active_sock, active_stream = self.connect()
        for _ in range(6):
            time.sleep(0.5)
            active_sock.sendall(b"get x\r\n")
            self.assertEqual(active_stream.readline(), b"END\r\n")
        try:
            self.assertEqual(idle_sock.recv(10), b"")
        except ConnectionResetError:
            pass


class EvictionTest(ServerTestCase):
    server_args = ("1",)

    def test_memory_is_bounded_and_newest_entry_is_kept(self):
        sock, stream = self.connect()
        value = b"v" * 10000
        keys = 300  # about 3 MB written to a 1 MB cache
        for i in range(keys):
            sock.sendall(b"set key-%d 0 0 %d\r\n" % (i, len(value)) + value + b"\r\n")
            self.assertEqual(stream.readline(), b"STORED\r\n")
        hits = set()
        for i in range(keys):
            sock.sendall(b"get key-%d\r\n" % i)
            line = stream.readline()
            if line != b"END\r\n":
                hits.add(i)
                stream.read(len(value) + 2)
                self.assertEqual(stream.readline(), b"END\r\n")
        self.assertIn(keys - 1, hits)
        self.assertGreater(len(hits), 0)
        self.assertLessEqual(len(hits), 100)
        self.assertNotIn(0, hits)


if __name__ == "__main__":
    unittest.main()

"""Integration tests for the movie ticket application over real localhost TCP."""

import concurrent.futures
import contextlib
import itertools
import json
import os
import pty
import select
import signal
from pathlib import Path
import socket
import sqlite3
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BIN = Path(os.environ.get("TICKET_TEST_BIN", ROOT / "bin")).resolve()
SECRET = "demo_pass_7934"
DATE = "2030-01-15"


def line(request_id, action, data):
    return json.dumps(
        {"id": request_id, "action": action, "data": data},
        ensure_ascii=False,
        separators=(",", ":"),
    ).encode() + b"\n"


class Connection:
    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.reader = self.socket.makefile("rb")
        self.ids = itertools.count(1)

    def close(self):
        self.reader.close()
        self.socket.close()

    def receive(self):
        response = self.reader.readline(4097)
        if not response.endswith(b"\n") or len(response) > 4096:
            raise AssertionError(f"Invalid response framing: {response!r}")
        return json.loads(response)

    def call(self, action, **data):
        request_id = next(self.ids)
        self.socket.sendall(line(request_id, action, data))
        response = self.receive()
        if response.get("id") != request_id:
            raise AssertionError(f"Expected response for {request_id}, got {response!r}")
        return response


class TicketTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="ticket-booking-test-")
        self.addCleanup(directory.cleanup)
        self.cwd = Path(directory.name)
        self.connections = []
        self.process = None
        self.addCleanup(self.stop_server)
        self.names = itertools.count(1)
        self.start_server()

    def start_server(self):
        stdout = self.cwd / "stdout.log"
        stderr = self.cwd / "stderr.log"
        for attempt in range(5):
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 0))
                self.port = probe.getsockname()[1]
            with stdout.open("w") as out, stderr.open("w") as err:
                self.process = subprocess.Popen(
                    [BIN / "server", str(self.port)], cwd=self.cwd, stdout=out, stderr=err
                )

            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                rc = self.process.poll()
                if rc is not None:
                    break
                if f"listening on TCP port {self.port}" in stdout.read_text():
                    return
                time.sleep(0.02)

            details = stderr.read_text()
            self.stop_server()
            if "Address already in use" not in details or attempt == 4:
                self.fail(f"Server failed to start on port {self.port}: {details}")

    def stop_server(self):
        for connection in self.connections:
            connection.close()
        self.connections.clear()
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        self.process = None

    def connect(self):
        connection = Connection(self.port)
        self.connections.append(connection)
        return connection

    def ok(self, response):
        self.assertEqual(set(response), {"id", "status", "data"}, response)
        self.assertEqual(response["status"], "OK", response)
        self.assertIsInstance(response["data"], dict)
        return response["data"]

    def error(self, response, code):
        self.assertEqual(set(response), {"id", "status", "error"}, response)
        self.assertEqual(response["status"], "ERROR", response)
        self.assertEqual(set(response["error"]), {"code", "message"}, response)
        self.assertEqual(response["error"]["code"], code, response)
        self.assertTrue(response["error"]["message"], response)
        return response["error"]["message"]

    def bootstrap(self, name="rootadmin", password=SECRET):
        return subprocess.run(
            [BIN / "server", "--init-admin", name], cwd=self.cwd,
            env={**os.environ, "TICKET_ADMIN_PASSWORD": password},
            text=True, capture_output=True, timeout=10,
        )

    def admin(self):
        result = self.bootstrap()
        self.assertEqual(result.returncode, 0, result.stderr)
        conn = self.connect()
        self.ok(conn.call("LOGIN", username="rootadmin", password=SECRET))
        return conn

    def user(self, role="BUYER", admin=None):
        conn = self.connect()
        username = f"user_{next(self.names)}"
        response = (
            admin.call("CREATE_USER", username=username, password=SECRET, role=role)
            if admin else conn.call("REGISTER", username=username, password=SECRET)
        )
        user_id = self.ok(response)["user_id"]
        self.assertEqual(self.ok(conn.call("LOGIN", username=username, password=SECRET)),
                         {"role": role, "user_id": user_id})
        return conn, user_id, username

    def manager(self):
        return self.user("MANAGER", self.admin())[0]

    def movie(self, manager, **overrides):
        data = dict(title="Đại Dương", genre="Phiêu lưu", duration=120, description="Đêm trên biển")
        data.update(overrides)
        return self.ok(manager.call("ADD_MOVIE", **data))["movie_id"]

    @staticmethod
    def slot(theater=1, starts_at=DATE + " 18:00"):
        return {"theater_id": theater, "starts_at": starts_at}

    def showings(self, manager, movie, *slots):
        return self.ok(manager.call("ADD_SHOWING", movie_id=movie, slots=slots))["showing_ids"]

    def showing(self, manager, **slot):
        return self.showings(manager, self.movie(manager), self.slot(**slot))[0]

    @contextlib.contextmanager
    def database(self):
        with contextlib.closing(sqlite3.connect(self.cwd / "data/tickets.sqlite3")) as db:
            with db:
                yield db

    def records(self, sql, values=()):
        with self.database() as db:
            return db.execute(sql, values).fetchall()

    def cli(self, *commands):
        return subprocess.run(
            [BIN / "client", "127.0.0.1", str(self.port)], cwd=self.cwd,
            input="\n".join(commands) + "\n", capture_output=True, text=True, timeout=15,
        )

    @staticmethod
    def race(*calls):
        barrier = threading.Barrier(len(calls))

        def start(call):
            barrier.wait(timeout=5)
            return call()

        with concurrent.futures.ThreadPoolExecutor(max_workers=len(calls)) as pool:
            return list(pool.map(start, calls))

    def assert_one_winner(self, responses, code):
        self.assertCountEqual([r["status"] for r in responses], ["OK", "ERROR"])
        winner = next(i for i, r in enumerate(responses) if r["status"] == "OK")
        self.ok(responses[winner])
        self.error(responses[1 - winner], code)
        return winner


class NetworkTests(TicketTest):
    def test_01_connection_and_disconnect(self):
        first = self.connect()
        first.socket.sendall(b'{"id":1,"action":"LOGOUT"')
        first.socket.shutdown(socket.SHUT_WR)
        self.assertEqual(first.reader.readline(), b"")
        self.error(self.connect().call("LOGOUT"), "NOT_LOGGED_IN")
        log = (self.cwd / "data/server.log").read_text()
        self.assertIn("CONNECT fd=", log)
        self.assertIn("DISCONNECT fd=", log)
        self.assertIn("peer=127.0.0.1", log)

    def test_02_tcp_split_and_pipelined_requests(self):
        conn = self.connect()
        first = line(101, "REGISTER", {"username": "wire_user", "password": SECRET})
        conn.socket.sendall(first[:23])
        conn.socket.settimeout(0.08)
        with self.assertRaises(socket.timeout):
            conn.socket.recv(1)
        conn.socket.settimeout(10)
        self.error(self.connect().call("LOGOUT"), "NOT_LOGGED_IN")
        conn.socket.sendall(first[23:] + line(102, "LOGIN", {"username": "wire_user", "password": SECRET})
                            + line(103, "LOGOUT", {}))
        for request_id in (101, 102, 103):
            response = conn.receive()
            self.assertEqual(response["id"], request_id)
            self.ok(response)

    def test_03_malformed_envelopes_and_utf8(self):
        conn = self.connect()
        malformed = (
            b"\n", b"{broken}\n", b"[]\n", b"null\n",
            b'{"action":"LOGOUT","data":{}}\n',
            b'{"id":17,"data":{}}\n',
            b'{"id":17,"action":"LOGOUT"}\n',
            b'{"id":17,"action":"LOGOUT","data":[]}\n',
            b'{"id":17,"action":true,"data":{}}\n',
            b'{"id":17,"action":"LOGOUT","data":{"x":"\xff"}}\n',
            b'{"id":17,"action":"LOGOUT","data":{}}\x00\n',
            b'{"id":17,"action":"LOGOUT","data":{}} garbage\n',
        )
        conn.socket.sendall(b"".join(malformed))
        for request in malformed:
            with self.subTest(request=request):
                self.error(conn.receive(), "INVALID_REQUEST")
        self.error(conn.call("LOGOUT"), "NOT_LOGGED_IN")

    def test_04_id_and_unknown_action(self):
        conn = self.connect()
        for request_id in (0, -1, "12", True, 1.5, 2147483648):
            with self.subTest(request_id=request_id):
                conn.socket.sendall(line(request_id, "LOGOUT", {}))
                response = conn.receive()
                self.assertIsNone(response["id"])
                self.error(response, "INVALID_REQUEST")
        conn.socket.sendall(line(2147483647, "UNKNOWN_EVENT", {}))
        response = conn.receive()
        self.assertEqual(response["id"], 2147483647)
        self.error(response, "UNKNOWN_ACTION")

    def test_05_frame_size_and_recovery(self):
        conn = self.connect()
        valid = line(23, "LOGOUT", {}).rstrip(b"\n").ljust(4095, b" ") + b"\n"
        self.assertEqual(len(valid), 4096)
        conn.socket.sendall(valid + b"x" * 4096 + b"\n" + b"y" * 12000 + b"\n" + line(24, "LOGOUT", {}))
        for expected_id, code in (
            (23, "NOT_LOGGED_IN"), (None, "MESSAGE_TOO_LARGE"),
            (None, "MESSAGE_TOO_LARGE"), (24, "NOT_LOGGED_IN"),
        ):
            response = conn.receive()
            self.assertEqual(response["id"], expected_id)
            self.error(response, code)

    def test_06_log_requests_responses_and_redaction(self):
        conn = self.connect()
        hidden_extra = "nested_secret_1323"
        self.ok(conn.call("REGISTER", username="log_user", password=SECRET,
                          extra=[{"PASSWORD": hidden_extra}]))
        self.ok(conn.call("LOGIN", username="log_user", password=SECRET))
        response = conn.call("BOOK_TICKETS", showing_id=999, seats=["A1", "A2"])
        self.error(response, "NOT_FOUND")
        conn.socket.sendall(b'{"password":"invalid_secret_4523"\n')
        self.error(conn.receive(), "INVALID_REQUEST")
        self.ok(conn.call("LOGOUT"))
        log = (self.cwd / "data/server.log").read_text()
        self.assertIn('action=BOOK_TICKETS data={"showing_id":999,"seats":["A1","A2"]}', log)
        self.assertIn('"password":"<redacted>"', log)
        self.assertIn('"PASSWORD":"<redacted>"', log)
        self.assertRegex(log, r'SEND fd=\d+ \{"id":3,"status":"ERROR","error":\{"code":"NOT_FOUND"')
        self.assertIn('"status":"OK"', log)
        for password in (SECRET, hidden_extra, "invalid_secret_4523"):
            self.assertNotIn(password, log)


class AuthTests(TicketTest):
    def test_07_registration_boundaries_and_duplicate(self):
        conn = self.connect()
        for username, password in (
            ("ab", SECRET), ("a" * 21, SECRET), ("a-b", SECRET), ("ábc", SECRET),
            ("lower", "a" * 5), ("lower", "a" * 33), ("lower", "ab cd1"),
            ("lower", "abc\t123"), ("lower", "abc\0xyz"), ("lower", "abc\u00a0123"),
            ("lower", "abc\u202f123"), ("lower", "abc\u0085123"),
            (None, SECRET), ("lower", 123456),
        ):
            with self.subTest(username=username, password=password):
                self.error(conn.call("REGISTER", username=username, password=password), "INVALID_REQUEST")

        for username, password in (("a_9", "123456"), ("a" * 20, "a" * 32), ("utf_user", "mậtKhẩu7")):
            user_id = self.ok(conn.call("REGISTER", username=username, password=password, role="ADMIN"))["user_id"]
            self.error(conn.call("REGISTER", username=username, password=password), "USERNAME_TAKEN")
            self.assertEqual(self.ok(conn.call("LOGIN", username=username, password=password)),
                             {"role": "BUYER", "user_id": user_id})
            self.ok(conn.call("LOGOUT"))
        self.assertEqual(self.records("SELECT count(*) FROM users"), [(3,)])

    def test_08_login_session_transitions(self):
        conn = self.connect()
        self.error(conn.call("SEARCH_MOVIES", title="anything"), "NOT_LOGGED_IN")
        self.error(conn.call("LOGIN", username="user_auth", password=SECRET), "INVALID_CREDENTIALS")
        self.ok(conn.call("REGISTER", username="user_auth", password=SECRET))
        self.error(conn.call("SEARCH_MOVIES", title="anything"), "NOT_LOGGED_IN")
        self.error(conn.call("LOGIN", username="user_auth", password="wrong_password"), "INVALID_CREDENTIALS")
        self.error(conn.call("LOGIN", username="user_auth"), "INVALID_REQUEST")
        self.error(conn.call("LOGOUT"), "NOT_LOGGED_IN")
        self.ok(conn.call("LOGIN", username="user_auth", password=SECRET))
        self.error(conn.call("LOGIN", username="user_auth", password=SECRET), "PERMISSION_DENIED")
        self.error(conn.call("REGISTER", username="other_auth", password=SECRET), "PERMISSION_DENIED")
        self.error(self.connect().call("SEARCH_MOVIES", title="anything"), "NOT_LOGGED_IN")
        self.ok(conn.call("LOGOUT"))
        self.error(conn.call("LOGOUT"), "NOT_LOGGED_IN")
        self.error(conn.call("SEARCH_MOVIES", title="anything"), "NOT_LOGGED_IN")
        self.ok(conn.call("LOGIN", username="user_auth", password=SECRET))

    def test_09_admin_bootstrap_rejects_bad_input_and_is_idempotent(self):
        result = self.bootstrap(password="short")
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.records("SELECT count(*) FROM users"), [(0,)])
        self.assertEqual(self.bootstrap().returncode, 0)
        self.assertEqual(self.bootstrap("other_admin", "new_admin_password").returncode, 0)
        self.assertEqual(self.records("SELECT username, role FROM users"), [("rootadmin", "ADMIN")])
        conn = self.connect()
        self.error(conn.call("LOGIN", username="other_admin", password="new_admin_password"), "INVALID_CREDENTIALS")
        self.assertEqual(self.ok(conn.call("LOGIN", username="rootadmin", password=SECRET))["role"], "ADMIN")

    def test_10_roles_and_live_permission_changes(self):
        admin = self.admin()
        buyer, uid, _ = self.user()
        self.error(buyer.call("LIST_THEATERS"), "PERMISSION_DENIED")
        self.ok(admin.call("SET_ROLE", user_id=uid, new_role="MANAGER"))
        self.ok(buyer.call("LIST_THEATERS"))
        self.ok(buyer.call("SEARCH_MOVIES", title="anything"))
        self.error(buyer.call("GET_SEATS", showing_id=999), "PERMISSION_DENIED")
        self.error(buyer.call("CREATE_USER", username="forbidden", password=SECRET, role="ADMIN"), "PERMISSION_DENIED")
        self.ok(admin.call("SET_ROLE", user_id=uid, new_role="ADMIN"))
        self.ok(buyer.call("CREATE_USER", username="new_buyer", password=SECRET, role="BUYER"))
        self.error(buyer.call("ADD_MOVIE"), "PERMISSION_DENIED")
        self.error(buyer.call("BOOK_TICKETS", showing_id=999, seats=["A1"]), "PERMISSION_DENIED")
        self.ok(admin.call("SET_ROLE", user_id=uid, new_role="BUYER"))
        self.error(buyer.call("SET_ROLE", user_id=uid, new_role="ADMIN"), "PERMISSION_DENIED")
        self.error(buyer.call("ADD_SHOWING"), "PERMISSION_DENIED")
        self.error(buyer.call("UPDATE_SHOWING"), "PERMISSION_DENIED")

    def test_11_admin_validation_and_last_admin(self):
        admin = self.admin()
        for bad_role in (None, 7, "OWNER", "manager"):
            with self.subTest(role=bad_role):
                self.error(admin.call("CREATE_USER", username="new_user", password=SECRET, role=bad_role), "INVALID_REQUEST")
                self.error(admin.call("SET_ROLE", user_id=1, new_role=bad_role), "INVALID_REQUEST")
        for uid in (0, -1, "1", 1.5, None):
            self.error(admin.call("SET_ROLE", user_id=uid, new_role="BUYER"), "INVALID_REQUEST")
        self.error(admin.call("SET_ROLE", user_id=999, new_role="BUYER"), "NOT_FOUND")
        self.error(admin.call("CREATE_USER", username="rootadmin", password=SECRET, role="BUYER"), "USERNAME_TAKEN")
        self.error(admin.call("SET_ROLE", user_id=1, new_role="BUYER"), "PERMISSION_DENIED")
        self.ok(admin.call("SET_ROLE", user_id=1, new_role="ADMIN"))
        self.assertEqual(self.records("SELECT role FROM users WHERE id=1"), [("ADMIN",)])

    def test_12_hashes_are_salted_and_unique(self):
        self.user()
        self.user()
        rows = self.records("SELECT typeof(password_hash),length(password_hash),length(salt),password_hash,salt FROM users")
        self.assertEqual([r[:3] for r in rows], [("blob", 32, 16), ("blob", 32, 16)])
        self.assertNotEqual(rows[0][3], rows[1][3])
        self.assertNotEqual(rows[0][4], rows[1][4])
        self.assertNotIn(SECRET.encode(), (self.cwd / "data/tickets.sqlite3").read_bytes())

    def test_13_duplicate_concurrent_registration_has_one_winner(self):
        first, second = self.connect(), self.connect()
        results = self.race(*(
            lambda c=c: c.call("REGISTER", username="same_user", password=SECRET)
            for c in (first, second)
        ))
        self.assert_one_winner(results, "USERNAME_TAKEN")
        self.assertEqual(self.records("SELECT username, role FROM users"), [("same_user", "BUYER")])

    def test_14_concurrent_demotion_keeps_one_admin(self):
        first = self.admin()
        second, second_id, _ = self.user("ADMIN", first)
        results = self.race(
            lambda: first.call("SET_ROLE", user_id=second_id, new_role="BUYER"),
            lambda: second.call("SET_ROLE", user_id=1, new_role="BUYER"),
        )
        self.assert_one_winner(results, "PERMISSION_DENIED")
        self.assertEqual(self.records("SELECT count(*) FROM users WHERE role='ADMIN'"), [(1,)])


    def test_45_permission_matrix_for_all_actions(self):
        admin = self.admin()
        manager, _, _ = self.user("MANAGER", admin)
        buyer, _, _ = self.user()
        guest = self.connect()
        matrix = {
            "REGISTER": ({"GUEST"}, "INVALID_REQUEST"),
            "LOGIN": ({"GUEST"}, "INVALID_REQUEST"),
            "SEARCH_MOVIES": ({"BUYER", "MANAGER"}, "INVALID_REQUEST"),
            "BROWSE_MOVIES": ({"BUYER", "MANAGER"}, "OK"),
            "GET_SHOWINGS": ({"BUYER", "MANAGER"}, "INVALID_REQUEST"),
            "GET_SEATS": ({"BUYER"}, "INVALID_REQUEST"),
            "BOOK_TICKETS": ({"BUYER"}, "INVALID_REQUEST"),
            "LIST_THEATERS": ({"MANAGER"}, "OK"),
            "ADD_MOVIE": ({"MANAGER"}, "INVALID_REQUEST"),
            "ADD_SHOWING": ({"MANAGER"}, "INVALID_REQUEST"),
            "UPDATE_SHOWING": ({"MANAGER"}, "INVALID_REQUEST"),
            "CREATE_USER": ({"ADMIN"}, "INVALID_REQUEST"),
            "SET_ROLE": ({"ADMIN"}, "INVALID_REQUEST"),
        }
        connections = {"GUEST": guest, "BUYER": buyer, "MANAGER": manager, "ADMIN": admin}
        for role, conn in connections.items():
            for action, (permitted, allowed_result) in matrix.items():
                with self.subTest(role=role, action=action):
                    response = conn.call(action)
                    if role not in permitted:
                        code = "NOT_LOGGED_IN" if role == "GUEST" else "PERMISSION_DENIED"
                        self.error(response, code)
                    elif allowed_result == "OK":
                        self.ok(response)
                    else:
                        self.error(response, allowed_result)
            if role == "GUEST":
                self.error(conn.call("LOGOUT"), "NOT_LOGGED_IN")
            else:
                self.ok(conn.call("LOGOUT"))


class CatalogTests(TicketTest):
    def test_15_add_movie_boundaries_and_persistence(self):
        manager = self.manager()
        short = self.movie(manager, title="A", genre="B", duration=1, description="")
        long = self.movie(manager, title="Đ" * 100, genre="ê" * 30, duration=500, description="ở" * 500)
        self.assertEqual(self.records("SELECT id,title,genre,duration,description FROM movies ORDER BY id"),
                         [(short, "A", "B", 1, ""), (long, "Đ" * 100, "ê" * 30, 500, "ở" * 500)])

    def test_16_invalid_movie_fields_have_no_side_effect(self):
        manager = self.manager()
        valid = dict(title="Movie", genre="Drama", duration=90, description="Scene")
        invalid = {
            "title": ("", "x" * 101, None, 34, "a\x00b", "a\x1bb"),
            "genre": ("", "x" * 31, None, True),
            "duration": (0, -1, 501, "90", 90.0, True),
            "description": ("x" * 501, None, False),
        }
        for key, values in invalid.items():
            for value in values:
                with self.subTest(field=key, value=value):
                    self.error(manager.call("ADD_MOVIE", **{**valid, key: value}), "INVALID_REQUEST")
            data = valid.copy()
            data.pop(key)
            self.error(manager.call("ADD_MOVIE", **data), "INVALID_REQUEST")
        self.assertEqual(self.records("SELECT count(*) FROM movies"), [(0,)])

    def test_17_search_title_literal_case_insensitive_and_missing(self):
        manager = self.manager()
        movie = self.movie(manager, title="Đại Dương 100%_")
        self.movie(manager, title="A match for anything")
        buyer, _, _ = self.user()
        for query in ("đẠi dƯƠNg", "DƯƠNG", "%_"):
            result = self.ok(buyer.call("SEARCH_MOVIES", title=query))["movies"]
            self.assertEqual([r["id"] for r in result], [movie])
        for query in ("no match", "' OR 1=1", "dai duong"):
            self.assertEqual(self.ok(buyer.call("SEARCH_MOVIES", title=query))["movies"], [])
        for query in ("", "A" * 101, None, 2):
            self.error(buyer.call("SEARCH_MOVIES", title=query), "INVALID_REQUEST")

    def test_18_browse_filters_individually_and_together(self):
        manager = self.manager()
        adventure = self.movie(manager)
        drama = self.movie(manager, title="Night", genre="Drama")
        first, second, third = self.showings(
            manager, adventure, self.slot(1), self.slot(3), self.slot(3, "2030-01-16 19:30")
        )
        fourth, = self.showings(manager, drama, self.slot(2, DATE + " 19:30"))
        buyer, _, _ = self.user()
        for filters, shows in (
            ({}, [first, second, third, fourth]),
            ({"genre": "PHIÊU LƯU"}, [first, second, third]),
            ({"location": "dA nAnG"}, [second, third]),
            ({"date": DATE}, [first, second, fourth]),
            ({"time": "19:30"}, [third, fourth]),
            ({"genre": "Phiêu lưu", "location": "Da Nang", "date": "2030-01-16", "time": "19:30"}, [third]),
            ({"genre": "Drama", "location": "Da Nang"}, []),
        ):
            with self.subTest(filters=filters):
                items = self.ok(buyer.call("BROWSE_MOVIES", **filters))["showings"]
                self.assertEqual([r["id"] for r in items], shows)
        self.assertEqual([r["id"] for r in self.ok(buyer.call("GET_SHOWINGS", movie_id=adventure))["showings"]],
                         [first, second, third])

    def test_19_bad_browse_filters_and_ids(self):
        buyer, _, _ = self.user()
        for key, values in (
            ("genre", ("unknown", "", None, 8, "x" * 31)),
            ("location", ("unknown", "", None, [], "x" * 31)),
            ("date", ("2030-02-29", "2030-13-01", "0000-01-01", "30-01-01", "2030-1-01", None)),
            ("time", ("24:00", "23:60", "6:30", "03:15:00", None)),
        ):
            for value in values:
                with self.subTest(key=key, value=value):
                    self.error(buyer.call("BROWSE_MOVIES", **{key: value}), "INVALID_FILTER")
        for value in (None, True, 0, -1, "1", 1.5, 2147483648):
            self.error(buyer.call("GET_SHOWINGS", movie_id=value), "INVALID_REQUEST")
        self.error(buyer.call("GET_SHOWINGS", movie_id=999), "NOT_FOUND")

    def test_20_paginate_all_list_endpoints_without_skipping(self):
        manager = self.manager()
        buyer, _, _ = self.user()
        title = "Page " + "Đ" * 95
        genre = "Ê" * 30
        movies = [self.movie(manager, title=title, genre=genre) for _ in range(19)]
        slots = [self.slot(1, f"2030-03-{day:02d} 18:00") for day in range(1, 20)]
        showings = self.showings(manager, movies[0], *slots[:10])
        showings += self.showings(manager, movies[0], *slots[10:])

        for conn, action, field, filters, expected in (
            (buyer, "SEARCH_MOVIES", "movies", {"title": "PAGE"}, movies),
            (buyer, "BROWSE_MOVIES", "showings", {}, showings),
            (buyer, "GET_SHOWINGS", "showings", {"movie_id": movies[0]}, showings),
            (manager, "LIST_THEATERS", "theaters", {}, [1, 2, 3, 4]),
        ):
            with self.subTest(action=action):
                for invalid in (0, -1, None, "1", False):
                    self.error(conn.call(action, after_id=invalid, **filters), "INVALID_REQUEST")
                self.assertEqual(self.ok(conn.call(action, after_id=expected[-1], **filters))[field], [])
                entries = []
                cursor = {}
                for _ in expected:
                    page = self.ok(conn.call(action, **cursor, **filters))
                    entries.extend(row["id"] for row in page[field])
                    if "next_after_id" not in page:
                        break
                    self.assertTrue(page[field])
                    self.assertEqual(page["next_after_id"], entries[-1])
                    cursor = {"after_id": page["next_after_id"]}
                self.assertEqual(entries, expected)
                if action != "LIST_THEATERS":
                    self.assertTrue(cursor, f"{action} never returned a second page")

    def test_21_theater_discovery_matches_layouts(self):
        manager = self.manager()
        rows = self.ok(manager.call("LIST_THEATERS"))["theaters"]
        self.assertEqual([(r["id"], r["location"], r["rows"], r["columns"]) for r in rows],
                         [(1, "Hanoi", 6, 10), (2, "Hanoi", 5, 8), (3, "Da Nang", 6, 10), (4, "Da Nang", 4, 8)])
        self.assertEqual([r["id"] for r in self.ok(manager.call("LIST_THEATERS", after_id=2))["theaters"]], [3, 4])


class ScheduleTests(TicketTest):
    def test_22_batch_creation_order_and_slot_count(self):
        manager = self.manager()
        movie = self.movie(manager)
        slots = [self.slot(theater=1 + i % 4, starts_at=f"2030-02-{i + 1:02d} 18:00") for i in range(10)]
        result = self.showings(manager, movie, *slots)
        self.assertEqual(len(result), 10)
        stored = self.records("SELECT id,movie_id,theater_id,starts_at FROM showings ORDER BY id")
        self.assertEqual(stored, [(id_, movie, s["theater_id"], s["starts_at"]) for id_, s in zip(result, slots)])
        for values in ([], slots + [slots[0]], None, "slots", {}, [None], [{}], [self.slot(theater=0)]):
            with self.subTest(slots=values):
                self.error(manager.call("ADD_SHOWING", movie_id=movie, slots=values), "INVALID_REQUEST")

    def test_23_calendar_boundaries_and_missing_entities(self):
        manager = self.manager()
        movie = self.movie(manager)
        for stamp in ("2030-02-29 18:00", "2100-02-29 18:00", "2030-04-31 18:00", "2030-01-15 24:00",
                      "2030-01-15 00:60", "2030-01-15T18:00", "2030-01-15 18:00:00", None):
            self.error(manager.call("ADD_SHOWING", movie_id=movie, slots=[self.slot(starts_at=stamp)]), "INVALID_REQUEST")
        self.error(manager.call("ADD_SHOWING", movie_id=999, slots=[self.slot()]), "NOT_FOUND")
        self.error(manager.call("ADD_SHOWING", movie_id=movie, slots=[self.slot(999)]), "NOT_FOUND")
        id_, = self.showings(manager, movie, self.slot(starts_at="2032-02-29 23:59"))
        self.assertEqual(self.records("SELECT starts_at FROM showings WHERE id=?", (id_,)),
                         [("2032-02-29 23:59",)])

    def test_24_overlap_and_boundary_across_midnight(self):
        manager = self.manager()
        movie = self.movie(manager, duration=120)
        self.showings(manager, movie, self.slot(starts_at=DATE + " 23:00"))
        for stamp in (DATE + " 23:00", DATE + " 22:30", "2030-01-16 00:59"):
            self.error(manager.call("ADD_SHOWING", movie_id=movie, slots=[self.slot(starts_at=stamp)]), "SHOWING_CONFLICT")
        self.showings(manager, movie, self.slot(starts_at=DATE + " 21:00"), self.slot(starts_at="2030-01-16 01:00"))
        self.showings(manager, movie, self.slot(2, DATE + " 23:00"))
        self.assertEqual(self.records("SELECT count(*) FROM showings"), [(4,)])

    def test_25_batch_rolls_back_after_internal_conflict_or_missing_room(self):
        manager = self.manager()
        movie = self.movie(manager)
        for other_slot, code in ((self.slot(1, DATE + " 19:59"), "SHOWING_CONFLICT"),
                                 (self.slot(999), "NOT_FOUND")):
            response = manager.call("ADD_SHOWING", movie_id=movie, slots=[self.slot(), other_slot])
            self.error(response, code)
            self.assertEqual(self.records("SELECT count(*) FROM showings"), [(0,)])
        self.showings(manager, movie, self.slot(), self.slot(1, DATE + " 20:00"))

    def test_26_update_room_and_time_and_reject_invalid_updates(self):
        manager = self.manager()
        movie = self.movie(manager)
        first, second = self.showings(manager, movie, self.slot(), self.slot(2, DATE + " 19:00"))
        for changes in ({}, {"theater_id": 0}, {"theater_id": None}, {"starts_at": None}, {"starts_at": "not a date"}):
            self.error(manager.call("UPDATE_SHOWING", showing_id=second, **changes), "INVALID_REQUEST")
        self.error(manager.call("UPDATE_SHOWING", showing_id=999, theater_id=3), "NOT_FOUND")
        self.error(manager.call("UPDATE_SHOWING", showing_id=second, theater_id=999), "NOT_FOUND")
        self.error(manager.call("UPDATE_SHOWING", showing_id=second, theater_id=1), "SHOWING_CONFLICT")
        self.assertEqual(self.records("SELECT theater_id, starts_at FROM showings WHERE id=?", (second,)),
                         [(2, DATE + " 19:00")])
        for changes, theater, starts_at in (
            ({"starts_at": DATE + " 20:00"}, 2, DATE + " 20:00"),
            ({"theater_id": 1}, 1, DATE + " 20:00"),
            ({"theater_id": 3, "starts_at": DATE + " 18:00"}, 3, DATE + " 18:00"),
        ):
            row = self.ok(manager.call("UPDATE_SHOWING", showing_id=second, **changes))["showing"]
            self.assertEqual(row, {"id": second, "theater_id": theater, "starts_at": starts_at})
        self.assertEqual(self.records("SELECT theater_id FROM showings WHERE id=?", (first,)), [(1,)])

    def test_27_booked_showings_cannot_be_edited(self):
        manager = self.manager()
        showing = self.showing(manager)
        buyer, _, _ = self.user()
        self.ok(buyer.call("BOOK_TICKETS", showing_id=showing, seats=["A1"]))
        for changes in ({"starts_at": DATE + " 19:00"}, {"theater_id": 2}):
            self.error(manager.call("UPDATE_SHOWING", showing_id=showing, **changes), "SHOWING_HAS_BOOKINGS")
        self.assertEqual(self.records("SELECT theater_id,starts_at FROM showings"), [(1, DATE + " 18:00")])

    def test_28_concurrent_scheduling_detects_overlap(self):
        admin = self.admin()
        first = self.user("MANAGER", admin)[0]
        second = self.user("MANAGER", admin)[0]
        movie = self.movie(first)
        responses = self.race(
            lambda: first.call("ADD_SHOWING", movie_id=movie, slots=[self.slot()]),
            lambda: second.call("ADD_SHOWING", movie_id=movie, slots=[self.slot(1, DATE + " 19:00")]),
        )
        self.assert_one_winner(responses, "SHOWING_CONFLICT")
        self.assertEqual(self.records("SELECT count(*) FROM showings"), [(1,)])


class BookingTests(TicketTest):
    def test_29_empty_and_updated_seat_maps_at_different_theaters(self):
        manager = self.manager()
        movie = self.movie(manager)
        first, second = self.showings(manager, movie, self.slot(1), self.slot(4))
        buyer, _, _ = self.user()
        for id_, layout in ((first, "6x10"), (second, "4x8")):
            info = self.ok(buyer.call("GET_SEATS", showing_id=id_))
            self.assertEqual((info["layout"], info["booked"]), (layout, []))
        self.ok(buyer.call("BOOK_TICKETS", showing_id=first, seats=["F10"]))
        self.ok(buyer.call("BOOK_TICKETS", showing_id=second, seats=["D8"]))
        self.assertEqual(self.ok(buyer.call("GET_SEATS", showing_id=first))["booked"], ["F10"])
        self.assertEqual(self.ok(buyer.call("GET_SEATS", showing_id=second))["booked"], ["D8"])

    def test_30_validation_and_missing_showings_leave_no_booking(self):
        manager = self.manager()
        showing = self.showing(manager, theater=4)
        buyer, _, _ = self.user()
        for seats in (
            [], None, "A1", {}, ["A1"] * 11, ["A1", "A1"], ["a1"], [None], [1], [[]],
            [""], ["A"], ["1A"], ["A0"], ["A01"], ["A-1"], ["A1\0x"], ["A10"], ["E1"], ["A1000"],
        ):
            with self.subTest(seats=seats):
                self.error(buyer.call("BOOK_TICKETS", showing_id=showing, seats=seats), "INVALID_REQUEST")
        for id_ in (0, -1, "1", 1.5, None, False):
            self.error(buyer.call("GET_SEATS", showing_id=id_), "INVALID_REQUEST")
            self.error(buyer.call("BOOK_TICKETS", showing_id=id_, seats=["A1"]), "INVALID_REQUEST")
        self.error(buyer.call("GET_SEATS", showing_id=999), "NOT_FOUND")
        self.error(buyer.call("BOOK_TICKETS", showing_id=999, seats=["A1"]), "NOT_FOUND")
        self.assertEqual(self.records("SELECT count(*) FROM bookings"), [(0,)])
        self.assertEqual(self.records("SELECT count(*) FROM booked_seats"), [(0,)])

    def test_31_one_and_ten_seats_and_scope_per_showing(self):
        manager = self.manager()
        movie = self.movie(manager)
        first, second = self.showings(manager, movie, self.slot(), self.slot(1, DATE + " 20:00"))
        buyer, uid, _ = self.user()
        seats = [f"A{i}" for i in range(1, 11)]
        one = self.ok(buyer.call("BOOK_TICKETS", showing_id=first, seats=seats))["booking_id"]
        two = self.ok(buyer.call("BOOK_TICKETS", showing_id=second, seats=["A1"]))["booking_id"]
        self.assertNotEqual(one, two)
        self.assertEqual(self.records("SELECT id,user_id,showing_id FROM bookings ORDER BY id"),
                         [(one, uid, first), (two, uid, second)])
        self.assertCountEqual(self.ok(buyer.call("GET_SEATS", showing_id=first))["booked"], seats)
        self.assertEqual(self.ok(buyer.call("GET_SEATS", showing_id=second))["booked"], ["A1"])

    def test_32_rolls_back_all_seats_if_one_is_taken(self):
        manager = self.manager()
        showing = self.showing(manager)
        first, _, _ = self.user()
        second, _, _ = self.user()
        self.ok(first.call("BOOK_TICKETS", showing_id=showing, seats=["A2"]))
        failure = second.call("BOOK_TICKETS", showing_id=showing, seats=["A1", "A2", "A3"])
        self.assertIn("A2", self.error(failure, "SEAT_UNAVAILABLE"))
        self.assertEqual(self.records("SELECT count(*) FROM bookings"), [(1,)])
        self.assertEqual(self.ok(first.call("GET_SEATS", showing_id=showing))["booked"], ["A2"])
        self.ok(second.call("BOOK_TICKETS", showing_id=showing, seats=["A1", "A3"]))
        self.error(second.call("BOOK_TICKETS", showing_id=showing, seats=["A1", "A3"]), "SEAT_UNAVAILABLE")
        self.assertEqual(self.records("SELECT count(*) FROM bookings"), [(2,)])

    def test_33_concurrent_overlapping_and_disjoint_bookings(self):
        manager = self.manager()
        showing = self.showing(manager)
        first, _, _ = self.user()
        second, _, _ = self.user()
        selections = (["A1", "A2"], ["A3", "A2"])
        responses = self.race(*(
            lambda c=c, seats=seats: c.call("BOOK_TICKETS", showing_id=showing, seats=seats)
            for c, seats in zip((first, second), selections)
        ))
        winner = self.assert_one_winner(responses, "SEAT_UNAVAILABLE")
        self.assertCountEqual(self.ok(first.call("GET_SEATS", showing_id=showing))["booked"], selections[winner])
        for r in self.race(
            lambda: first.call("BOOK_TICKETS", showing_id=showing, seats=["B1"]),
            lambda: second.call("BOOK_TICKETS", showing_id=showing, seats=["B2"]),
        ):
            self.ok(r)
        self.assertEqual(self.records("SELECT count(*) FROM bookings"), [(3,)])
        self.assertEqual(self.records("SELECT count(*) FROM booked_seats"), [(4,)])

    def test_34_stale_seat_map_requires_refresh_after_schedule_change(self):
        manager = self.manager()
        showing = self.showing(manager)
        buyer, _, _ = self.user()
        old = self.ok(buyer.call("GET_SEATS", showing_id=showing))["showing"]
        self.ok(manager.call("UPDATE_SHOWING", showing_id=showing, theater_id=2, starts_at=DATE + " 19:00"))
        self.error(buyer.call("BOOK_TICKETS", showing_id=showing, seats=["A1"], expected_showing=old),
                   "SHOWING_CHANGED")
        for snapshot in (None, {}, {"theater_id": 2}, {"theater_id": 2, "starts_at": "invalid"}):
            self.error(buyer.call("BOOK_TICKETS", showing_id=showing, seats=["A1"], expected_showing=snapshot),
                       "INVALID_REQUEST")
        self.assertEqual(self.records("SELECT count(*) FROM bookings"), [(0,)])
        fresh = self.ok(buyer.call("GET_SEATS", showing_id=showing))["showing"]
        self.assertEqual(fresh, {"theater_id": 2, "starts_at": DATE + " 19:00"})
        self.ok(buyer.call("BOOK_TICKETS", showing_id=showing, seats=["A1"], expected_showing=fresh))

    def test_35_concurrent_update_and_booking_use_same_showing_state(self):
        manager = self.manager()
        showing = self.showing(manager)
        buyer, _, _ = self.user()
        snapshot = self.ok(buyer.call("GET_SEATS", showing_id=showing))["showing"]
        booking, update = self.race(
            lambda: buyer.call("BOOK_TICKETS", showing_id=showing, seats=["A1"], expected_showing=snapshot),
            lambda: manager.call("UPDATE_SHOWING", showing_id=showing, theater_id=2),
        )
        if booking["status"] == "OK":
            self.ok(booking)
            self.error(update, "SHOWING_HAS_BOOKINGS")
            self.assertEqual(self.records("SELECT theater_id FROM showings"), [(1,)])
            self.assertEqual(self.records("SELECT count(*) FROM bookings"), [(1,)])
        else:
            self.error(booking, "SHOWING_CHANGED")
            self.ok(update)
            self.assertEqual(self.records("SELECT theater_id FROM showings"), [(2,)])
            self.assertEqual(self.records("SELECT count(*) FROM bookings"), [(0,)])

    def test_36_mid_transaction_storage_failure_rolls_back_and_allows_retry(self):
        manager = self.manager()
        showing = self.showing(manager)
        buyer, _, _ = self.user()
        with self.database() as db:
            db.execute("CREATE TRIGGER test_write_failure BEFORE INSERT ON booked_seats "
                       "WHEN NEW.seat='A2' BEGIN SELECT RAISE(ABORT,'test failure'); END")
        self.error(buyer.call("BOOK_TICKETS", showing_id=showing, seats=["A1", "A2"]), "INTERNAL_ERROR")
        self.assertEqual(self.records("SELECT count(*) FROM bookings"), [(0,)])
        self.assertEqual(self.ok(buyer.call("GET_SEATS", showing_id=showing))["booked"], [])
        with self.database() as db:
            db.execute("DROP TRIGGER test_write_failure")
        self.ok(buyer.call("BOOK_TICKETS", showing_id=showing, seats=["A1", "A2"]))

    def test_37_persists_users_catalog_schedule_and_booking_after_restart(self):
        manager = self.manager()
        movie = self.movie(manager)
        showing, = self.showings(manager, movie, self.slot())
        buyer, uid, username = self.user()
        booking = self.ok(buyer.call("BOOK_TICKETS", showing_id=showing, seats=["A1", "A2"]))["booking_id"]
        self.stop_server()
        self.start_server()
        buyer = self.connect()
        self.error(buyer.call("GET_SEATS", showing_id=showing), "NOT_LOGGED_IN")
        self.assertEqual(self.ok(buyer.call("LOGIN", username=username, password=SECRET))["user_id"], uid)
        self.assertEqual(self.ok(buyer.call("SEARCH_MOVIES", title="đại"))["movies"][0]["id"], movie)
        self.assertEqual(self.ok(buyer.call("GET_SHOWINGS", movie_id=movie))["showings"][0]["id"], showing)
        self.assertEqual(self.ok(buyer.call("GET_SEATS", showing_id=showing))["booked"], ["A1", "A2"])
        self.assertEqual(self.records("SELECT id,user_id,showing_id FROM bookings"), [(booking, uid, showing)])


class Terminal:
    def __init__(self, cwd, command, env=None):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.chdir(cwd)
            os.execve(command[0], command, os.environ if env is None else env)
        self.pending = b""

    def send(self, text):
        os.write(self.fd, text.encode() + b"\n")

    def expect(self, text):
        marker = text.encode()
        deadline = time.monotonic() + 10
        while marker not in self.pending:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.fd], [], [], remaining)[0]:
                raise AssertionError(f"Expected {marker!r}, got {self.pending!r}")
            try:
                reply = os.read(self.fd, 4096)
            except OSError as exc:
                raise AssertionError(f"Terminal exited while waiting for {marker!r}: {self.pending!r}") from exc
            if not reply:
                raise AssertionError(f"Terminal closed while waiting for {marker!r}: {self.pending!r}")
            self.pending += reply
        before, marker, self.pending = self.pending.partition(marker)
        return before + marker

    def close(self):
        os.close(self.fd)
        if os.waitpid(self.pid, os.WNOHANG)[0] == 0:
            try:
                os.kill(self.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            os.waitpid(self.pid, 0)


class CliTests(TicketTest):
    def test_38_cli_demo_for_all_roles(self):
        self.assertEqual(self.bootstrap().returncode, 0)
        result = self.cli(
            "help", f"login rootadmin {SECRET}", f"create-user sales MANAGER {SECRET}",
            f"create-user staff BUYER {SECRET}", "set-role 3 MANAGER", "logout",
            f"LOGIN sales {SECRET}", "theaters",
            'add-movie "Đại Dương" "Science Fiction" 120 "A \\"quoted\\" description"',
            f'add-showing 1 1 "{DATE} 18:00" 3 "{DATE} 18:00"',
            f'update-showing 2 starts_at="{DATE} 19:30"', "logout",
            f"register movie_buyer {SECRET}", f"login movie_buyer {SECRET}",
            'search "đại dương"', f'browse location="Da Nang" date={DATE} time=19:30',
            "showings 1", "seats 2", "book 2 a1 a2", "seats 2", "exit",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        for text in ("Guest and session", "Sales manager", "Logged in as ADMIN", "Logged in as MANAGER",
                     "Logged in as BUYER", "User #3 now has the MANAGER role.", "Created movie #1.",
                     "Created showings #1 #2.", "Cinema Da Nang", "19:30", "Science Fiction", "Booking #1 confirmed.",
                     " A1  ", " XX  XX"):
            self.assertIn(text, result.stdout)
        self.assertNotRegex(result.stdout, r"INVALID_|NOT_FOUND|PERMISSION_DENIED|Usage:")
        self.assertEqual(self.records("SELECT description FROM movies"), [('A "quoted" description',)])

    def test_39_cli_bad_arguments_and_server_errors_recover(self):
        buyer, _, username = self.user()
        result = self.cli(
            'search "unterminated', "nonexistent", "book 0 A1",
            f"login {username} wrong_password", f"login {username} {SECRET}",
            "browse location=Hanoi location=Hanoi", "browse unknown=value",
            "browse time=99:99", "seats 999", "logout", "browse", "exit",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        for text in ("Invalid quoting", "Unknown command", "Usage: book", "INVALID_CREDENTIALS",
                     "Logged in as BUYER", "Usage: browse", "INVALID_FILTER",
                     "NOT_FOUND", "Logged out", "NOT_LOGGED_IN"):
            self.assertIn(text, result.stdout)
        self.ok(buyer.call("LOGOUT"))

    def test_40_cli_bad_startup_arguments(self):
        for arguments in ([], ["127.0.0.1", "0"], ["127.0.0.1", "two"], ["127.0.0.1", "65536"],
                          ["127.0.0.1", f"{self.port}extra"], ["not-an-ip", str(self.port)]):
            with self.subTest(arguments=arguments):
                result = subprocess.run([BIN / "client", *arguments], text=True, capture_output=True, timeout=5)
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(result.stderr)
        for arguments in (["xyz"], ["-1"], ["65536"], ["--init-admin"], ["1", "2", "3"]):
            with self.subTest(arguments=arguments):
                result = subprocess.run([BIN / "server", *arguments], text=True, capture_output=True, timeout=5)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Usage:", result.stderr)


    def test_41_cli_hides_interactive_password(self):
        terminal = Terminal(self.cwd, [BIN / "client", "127.0.0.1", str(self.port)])
        self.addCleanup(terminal.close)
        terminal.expect("tickets> ")
        terminal.send("register terminal_user")
        terminal.expect("Password: ")
        terminal.send(SECRET)
        result = terminal.expect("tickets> ")
        self.assertIn(b"Created user #1", result)
        self.assertNotIn(SECRET.encode(), result)
        terminal.send("login terminal_user")
        terminal.expect("Password: ")
        terminal.send(SECRET)
        result = terminal.expect("tickets> ")
        self.assertIn(b"Logged in as BUYER", result)
        self.assertNotIn(SECRET.encode(), result)
        terminal.send("exit")
        terminal.expect("Goodbye!")

    def test_42_cli_resends_seat_map_snapshot_after_schedule_refresh(self):
        manager = self.manager()
        movie = self.movie(manager)
        first, second = self.showings(manager, movie, self.slot(), self.slot(3))
        _, _, username = self.user()
        terminal = Terminal(self.cwd, [BIN / "client", "127.0.0.1", str(self.port)])
        self.addCleanup(terminal.close)
        terminal.expect("tickets> ")
        terminal.send(f"login {username}")
        terminal.expect("Password: ")
        terminal.send(SECRET)
        terminal.expect("tickets> ")
        for showing in (first, second):
            terminal.send(f"seats {showing}")
            terminal.expect("tickets> ")
        self.ok(manager.call("UPDATE_SHOWING", showing_id=first, starts_at=DATE + " 20:00"))
        terminal.send(f"book {first} A1")
        stale = terminal.expect("tickets> ")
        self.assertIn(b"SHOWING_CHANGED", stale)
        self.assertEqual(self.records("SELECT count(*) FROM bookings"), [(0,)])
        terminal.send(f"seats {first}")
        self.assertIn(b"20:00", terminal.expect("tickets> "))
        terminal.send(f"book {first} A1")
        self.assertIn(b"Booking #1 confirmed", terminal.expect("tickets> "))

    def test_43_cli_reports_connection_refused(self):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        result = subprocess.run(
            [BIN / "client", "127.0.0.1", str(port)],
            stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=5,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Could not connect", result.stderr)

    def test_47_cli_rejects_lost_or_invalid_reply(self):
        for reply in (
            b"",
            b'{"id":1,"status":"OK"',
            b'{"id":2,"status":"OK","data":{}}\n',
            b'{"id":1,"status":"UNKNOWN","data":{}}\n',
            b'{"id":1,"status":"OK","data":[]}\n',
            b'{"id":1,"status":"ERROR","error":{"code":"NOT_LOGGED_IN"}}\n',
        ):
            with self.subTest(reply=reply), socket.socket() as listener:
                listener.bind(("127.0.0.1", 0))
                listener.listen()
                listener.settimeout(5)

                def respond():
                    connection, _ = listener.accept()
                    with connection, connection.makefile("rb") as requests:
                        connection.settimeout(5)
                        self.assertEqual(json.loads(requests.readline()),
                                         {"id": 1, "action": "BROWSE_MOVIES", "data": {}})
                        connection.sendall(reply)

                with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                    response = pool.submit(respond)
                    result = subprocess.run(
                        [BIN / "client", "127.0.0.1", str(listener.getsockname()[1])],
                        input="browse\nexit\n", capture_output=True, text=True, timeout=8,
                    )
                    response.result(timeout=5)
                self.assertNotEqual(result.returncode, 0, result)
                self.assertRegex(result.stderr, r"Connection closed or invalid reply|Invalid reply from the server")


    def test_44_sold_out_seat_map_has_no_available_labels(self):
        manager = self.manager()
        showing = self.showing(manager, theater=4)
        buyer, _, username = self.user()
        labels = [f"{row}{column}" for row in "ABCD" for column in range(1, 9)]
        for i in range(0, len(labels), 10):
            self.ok(buyer.call("BOOK_TICKETS", showing_id=showing, seats=labels[i:i + 10]))
        self.assertCountEqual(self.ok(buyer.call("GET_SEATS", showing_id=showing))["booked"], labels)
        self.error(buyer.call("BOOK_TICKETS", showing_id=showing, seats=["A1"]), "SEAT_UNAVAILABLE")
        result = self.cli(f"login {username} {SECRET}", f"seats {showing}", "exit")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("This showing is sold out.", result.stdout)
        seat_rows = [row.split() for row in result.stdout.splitlines() if row.startswith(" XX")]
        self.assertEqual(seat_rows, [["XX"] * 8] * 4)


    def test_46_fresh_install_interactive_admin_setup(self):
        self.stop_server()
        self.cwd = self.cwd / "fresh"
        self.cwd.mkdir()
        env = {key: value for key, value in os.environ.items() if key != "TICKET_ADMIN_PASSWORD"}
        terminal = Terminal(self.cwd, [BIN / "server", "--init-admin", "first_admin"], env)
        self.addCleanup(terminal.close)
        terminal.expect("Initial admin password: ")
        terminal.send(SECRET)
        setup = terminal.expect("An admin account is configured.")
        self.assertNotIn(SECRET.encode(), setup)
        self.assertEqual(self.records("SELECT username,role FROM users"), [("first_admin", "ADMIN")])

        self.start_server()
        result = self.cli(f"login first_admin {SECRET}", f"create-user sample_manager MANAGER {SECRET}", "exit")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Logged in as ADMIN", result.stdout)
        self.assertIn("Created user #2", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)

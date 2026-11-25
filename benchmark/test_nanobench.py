import io
from contextlib import closing, redirect_stdout
from pathlib import Path
import sqlite3
from tempfile import TemporaryDirectory
from threading import Event
import unittest
from unittest.mock import patch

import chess
import chess.pgn

import nanobench


class SimulatedNano:
    def __init__(self, name):
        self.port = name

    def write(self, data):
        command, *args = data.decode("ascii").split()
        if command == "start":
            self.board = chess.Board()
        elif command == "play":
            self.board.push_uci(args[0])
        if command == "go":
            move = next(iter(self.board.legal_moves))
            self.reply = f"bestmove {move} score 0 depth 1 nodes 20"
        else:
            self.reply = "ok"

    def readline(self):
        return (self.reply + "\n").encode("ascii")


class BenchmarkTest(unittest.TestCase):
    def setUp(self):
        self.config, self.jobs = nanobench.load_config(Path(__file__).with_name("example.json"))
        self.config["max_plies"] = 2
        self.pair = self.config["board_pairs"][0]
        temporary = TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.path = Path(temporary.name) / "results.sqlite3"
        nanobench.prepare_database(self.path, self.config)

    def test_full_matrix(self):
        self.assertEqual(len(self.jobs), 135)
        first_builds = {a.name for a, _, _, _ in self.jobs}
        second_builds = {b.name for _, b, _, _ in self.jobs}
        self.assertEqual(len(first_builds | second_builds), 10)

    def test_balanced_games_and_resume(self):
        with patch.object(nanobench, "open_pair", side_effect=lambda *_: [SimulatedNano(x) for x in self.pair]):
            for _ in range(2):
                nanobench.run_pair(self.path, self.pair, self.jobs, self.config, Event(), 1)
        with closing(sqlite3.connect(self.path)) as db:
            rows = db.execute("SELECT job, repetition, a_white, result, pgn FROM games ORDER BY job, game").fetchall()
        self.assertEqual([(i, 0) for i in (0, 1) for _ in range(4)], [row[:2] for row in rows])
        placements = set()
        for _, _, a_white, result, pgn in rows[:4]:
            game = chess.pgn.read_game(io.StringIO(pgn))
            self.assertFalse(game.errors)
            self.assertEqual(game.end().ply(), len(self.jobs[0][3]) + 2)
            self.assertIn("nodes 20", game.end().comment)
            self.assertEqual(result, "*")
            placements.add((a_white, game.headers["WhiteBoard" if a_white else "BlackBoard"]))
        self.assertEqual(placements, {(color, board) for color in (True, False) for board in self.pair})
        with redirect_stdout(io.StringIO()) as output:
            nanobench.report(self.path, export=False)
        self.assertIn("\t0\t0\t0\t8", output.getvalue())

    def test_failed_partial_set_leaves_no_games(self):
        original = nanobench.play_game
        stop = Event()

        def lose_second_game(*args):
            if args[5] == 1:
                raise TimeoutError("disconnected")
            return original(*args)

        def stop_on_warning(*args):
            stop.set()
            # Defer the stop until after the failure has been recorded.

        with (
            patch.object(nanobench, "open_pair", return_value=[SimulatedNano(x) for x in self.pair]),
            patch.object(nanobench, "play_game", side_effect=lose_second_game),
            patch.object(nanobench.logging, "warning", side_effect=stop_on_warning),
        ):
            nanobench.run_pair(self.path, self.pair, self.jobs, self.config, stop, 1)
        with closing(sqlite3.connect(self.path)) as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM games").fetchone()[0], 0)
            self.assertEqual(db.execute("SELECT error FROM failures").fetchone()[0], "disconnected")

    def test_rejects_config_change_on_resume(self):
        with self.assertRaisesRegex(ValueError, "Configuration changed"):
            nanobench.prepare_database(self.path, {**self.config, "move_ms": 200})


if __name__ == "__main__":
    unittest.main()

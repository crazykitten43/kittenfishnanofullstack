"""Run continuous, opening-paired KittenFish matches on USB-connected Nanos."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack, closing
from dataclasses import dataclass
from datetime import datetime, timezone
import fcntl
from hashlib import sha256
from itertools import combinations, product
import json
import logging
from pathlib import Path
import re
import signal
import sqlite3
from threading import Event
import time

import chess
import chess.pgn
import serial
from serial.tools import list_ports


PROFILES = {"selectivepvs", "quietpruning", "latemovereductions", "aspirationpvs"}
BESTMOVE = re.compile(r"bestmove ([a-h][1-8][a-h][1-8][qrbn]?) score (-?\d+) depth (\d+) nodes (\d+)")
RETRY_SECONDS = 30


@dataclass(frozen=True)
class Variant:
    engine: str
    search: str
    evaluator: str
    profile: str
    mask: int

    @property
    def name(self):
        return f"{self.engine}/{self.search}"

    def configure(self, port):
        for command in (self.evaluator, f"search {self.profile}", f"opt {self.mask}", "start"):
            request(port, command)


def load_config(path):
    config = json.loads(path.read_text())
    boards = config["board_pairs"]
    if not boards or any(not isinstance(pair, list) or len(pair) != 2 for pair in boards):
        raise ValueError("board_pairs must contain pairs of USB serial numbers")
    ids = [number for pair in boards for number in pair]
    if any(not isinstance(number, str) or not number.strip() for number in ids) or len(set(ids)) != len(ids):
        raise ValueError("Each board must have a unique USB serial number")

    engines, searches = config["engines"], config["searches"]
    for pool in (engines, searches):
        if not pool or any(not re.fullmatch(r"[\w.-]+", name) for name in pool):
            raise ValueError("Pool names must contain only letters, numbers, dots, underscores or hyphens")
    if any(command not in ("hce 4", "nnue") for command in engines.values()):
        raise ValueError("Engine entries must select hce 4 or nnue")
    for search in searches.values():
        if search["profile"] not in PROFILES or type(search["mask"]) is not int or not 0 <= search["mask"] <= 63:
            raise ValueError("Search entries require a supported profile and a mask from 0 to 63")
    variants = [
        Variant(engine, search, engines[engine], **searches[search])
        for engine, search in product(sorted(engines), sorted(searches))
    ]
    if len(variants) < 2:
        raise ValueError("At least two engine/search combinations are required")

    if not config["openings"]:
        raise ValueError("At least one opening is required")
    for name, moves in config["openings"].items():
        board = chess.Board()
        for move in moves.split():
            board.push_uci(move)
        if board.is_game_over(claim_draw=True):
            raise ValueError(f"Opening {name!r} already ends the game")
    for name, maximum in (("move_ms", 60000), ("max_plies", 10000)):
        if type(config[name]) is not int or not 1 <= config[name] <= maximum:
            raise ValueError(f"{name} must be an integer from 1 to {maximum}")
    if not isinstance(config["firmware"], str) or not config["firmware"].strip():
        raise ValueError("Record the installed firmware revision in firmware")

    jobs = [
        (a, b, name, moves.split())
        for (a, b), (name, moves) in product(combinations(variants, 2), sorted(config["openings"].items()))
    ]
    return config, jobs


def request(port, command, *, move_ms=None):
    start = time.monotonic()
    deadline = start + 3 + (move_ms or 0) / 1000
    port.write((command + "\n").encode("ascii"))
    while time.monotonic() < deadline:
        reply = port.readline().decode("ascii", errors="replace").strip()
        if not reply or reply.startswith("KittenFish Nano Fullstack "):
            continue
        match = BESTMOVE.fullmatch(reply) if move_ms is not None else None
        if match:
            return (*match.groups(), (time.monotonic() - start) * 1000)
        if move_ms is None and reply == "ok":
            return None
        raise RuntimeError(f"{port.port}: {command!r} returned {reply!r}")
    raise TimeoutError(f"{port.port}: no reply to {command!r}")


def open_pair(stack, pair):
    devices = list(list_ports.comports())
    ports = []
    for number in pair:
        paths = {device.device for device in devices if device.serial_number == number}
        # macOS exposes separate dial-in and callout paths for the same device.
        paths = {path for path in paths if path.startswith("/dev/cu.")} or paths
        if len(paths) != 1:
            raise RuntimeError(f"Expected one USB port for {number}, found {sorted(paths)}")
        port = serial.Serial(baudrate=115200, timeout=0.25, write_timeout=2, exclusive=True)
        port.port = paths.pop()
        # The Nano's USB CDC firmware needs the default DTR handshake.
        stack.enter_context(port)
        port.reset_input_buffer()
        ports.append(port)
    return ports


def play_game(ports, pair, job, config, a_white, a_board, stop):
    a, b, opening, moves = job
    a.configure(ports[a_board])
    b.configure(ports[1 - a_board])
    board = chess.Board()
    game = chess.pgn.Game()
    white, black = (a, b) if a_white else (b, a)
    white_board = a_board if a_white else 1 - a_board
    game.headers.update(
        Event="KittenFish Nano benchmark", Date=datetime.now(timezone.utc).strftime("%Y.%m.%d"),
        White=white.name, Black=black.name, Opening=opening,
        WhiteBoard=pair[white_board], BlackBoard=pair[1 - white_board],
        MoveTimeMs=str(config["move_ms"]),
    )
    node = game
    for move in moves:
        node = node.add_variation(board.push_uci(move))
        for port in ports:
            request(port, f"play {move}")
    for ply in range(config["max_plies"] + 1):
        if stop.is_set():
            return None
        outcome = board.outcome(claim_draw=True)
        if outcome:
            game.headers.update(Result=outcome.result(), Termination=outcome.termination.name.lower())
            break
        if ply == config["max_plies"]:
            game.headers["Termination"] = "ply limit"
            break
        port = ports[white_board if board.turn == chess.WHITE else 1 - white_board]
        move, score, depth, nodes, elapsed = request(port, f"go {config['move_ms']}", move_ms=config["move_ms"])
        node = node.add_variation(board.push_uci(move))
        node.comment = f"score {score}, depth {depth}, nodes {nodes}, elapsed {elapsed:.1f} ms"
        for port in ports:
            request(port, f"play {move}")
    return game


SCHEMA = """
CREATE TABLE IF NOT EXISTS experiment (
    id INTEGER PRIMARY KEY CHECK (id = 1), config TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS games (
    board_pair TEXT NOT NULL, job INTEGER NOT NULL, repetition INTEGER NOT NULL,
    game INTEGER NOT NULL, a TEXT NOT NULL, b TEXT NOT NULL, opening TEXT NOT NULL,
    a_white INTEGER NOT NULL, result TEXT NOT NULL, pgn TEXT NOT NULL,
    PRIMARY KEY (board_pair, job, repetition, game)
);
CREATE TABLE IF NOT EXISTS failures (
    timestamp TEXT NOT NULL, board_pair TEXT NOT NULL, a TEXT NOT NULL,
    b TEXT NOT NULL, opening TEXT NOT NULL, error TEXT NOT NULL
);
"""


def prepare_database(path, config):
    canonical = json.dumps(config, sort_keys=True)
    with closing(sqlite3.connect(path)) as db, db:
        db.execute("PRAGMA journal_mode=WAL")
        db.executescript(SCHEMA)
        db.execute("INSERT OR IGNORE INTO experiment VALUES (1, ?)", (canonical,))
        if db.execute("SELECT config FROM experiment").fetchone()[0] != canonical:
            raise ValueError("Configuration changed. Use a new results database for this experiment.")


def run_pair(path, pair, jobs, config, stop, limit):
    pair_id = ",".join(pair)
    retry_at = {}
    completed = 0
    with closing(sqlite3.connect(path, timeout=15)) as db:
        counts = dict(db.execute(
            "SELECT job, COUNT(*) / 4 FROM games WHERE board_pair = ? GROUP BY job", (pair_id,)
        ))
        while not stop.is_set() and (limit is None or completed < limit):
            now = time.monotonic()
            ready = [index for index in range(len(jobs)) if retry_at.get(index, 0) <= now]
            if not ready:
                stop.wait(max(0, min(retry_at.values()) - now))
                continue
            index = min(ready, key=lambda i: (counts.get(i, 0), i))
            job = jobs[index]
            a, b, opening, _ = job
            repetition = counts.get(index, 0)
            rows = []
            try:
                with ExitStack() as stack:
                    ports = open_pair(stack, pair)
                    for game_index, (a_white, a_board) in enumerate(product((True, False), (0, 1))):
                        game = play_game(ports, pair, job, config, a_white, a_board, stop)
                        if game is None:
                            return
                        rows.append((pair_id, index, repetition, game_index, a.name, b.name,
                                     opening, a_white, game.headers["Result"], str(game)))
            except (OSError, RuntimeError, ValueError) as error:
                if stop.is_set():
                    return
                logging.warning("%s %s vs %s (%s): %s", pair_id, a.name, b.name, opening, error)
                with db:
                    db.execute("INSERT INTO failures VALUES (?, ?, ?, ?, ?, ?)",
                               (datetime.now(timezone.utc).isoformat(), pair_id, a.name, b.name, opening, str(error)))
                retry_at[index] = time.monotonic() + RETRY_SECONDS
                # Give disconnected boards time to reappear while allowing other jobs to advance.
                stop.wait(2)
                continue
            with db:
                db.executemany("INSERT INTO games VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", rows)
            counts[index] = repetition + 1
            completed += 1
            logging.info("%s %s vs %s (%s), set %d: %s", pair_id, a.name, b.name, opening,
                         repetition + 1, " ".join(row[-2] for row in rows))


def run(path, config, jobs, limit):
    stop = Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stop.set())
    # Keep the lock file in place so every process locks the same inode.
    with path.with_suffix(path.suffix + ".lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        prepare_database(path, config)
        with ThreadPoolExecutor(max_workers=len(config["board_pairs"])) as pool:
            def worker(pair):
                try:
                    run_pair(path, pair, jobs, config, stop, limit)
                except Exception:
                    stop.set()
                    raise

            futures = [pool.submit(worker, pair) for pair in config["board_pairs"]]
            for future in futures:
                future.result()


def report(path, export):
    with closing(sqlite3.connect(path.as_uri() + "?mode=ro", uri=True)) as db:
        if export:
            for (pgn,) in db.execute("SELECT pgn FROM games ORDER BY repetition, job, board_pair, game"):
                print(pgn, end="\n\n")
            return
        tallies = db.execute("""
            SELECT a, b,
              SUM((result = '1-0' AND a_white) OR (result = '0-1' AND NOT a_white)),
              SUM((result = '0-1' AND a_white) OR (result = '1-0' AND NOT a_white)),
              SUM(result = '1/2-1/2'), SUM(result = '*')
            FROM games GROUP BY a, b ORDER BY a, b
        """)
        print("A\tB\tA wins\tB wins\tDraws\tUnfinished")
        for row in tallies:
            print(*row, sep="\t")
        failures = db.execute("SELECT COUNT(*) FROM failures").fetchone()[0]
        print(f"Failed sets: {failures}")
        for timestamp, pair, a, b, opening, error in db.execute(
            "SELECT * FROM failures ORDER BY rowid DESC LIMIT 5"
        ):
            print(f"{timestamp} {pair} {a} vs {b} ({opening}): {error}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for command in ("plan", "run"):
        sub = commands.add_parser(command)
        sub.add_argument("config", type=Path)
        if command == "run":
            sub.add_argument("database", type=Path)
            sub.add_argument("--sets-per-pair", type=int, help="Stop after this many new four-game sets per pair")
    for command in ("status", "pgn"):
        commands.add_parser(command).add_argument("database", type=Path)
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    try:
        if args.command in ("status", "pgn"):
            report(args.database.resolve(), args.command == "pgn")
            return
        config, jobs = load_config(args.config)
        if args.command == "plan":
            print(f"{len(jobs)} sets, {4 * len(jobs)} games per board pair per pass")
            print(f"Config SHA-256: {sha256(json.dumps(config, sort_keys=True).encode()).hexdigest()}")
            for a, b, opening, _ in jobs:
                print(a.name, b.name, opening, sep="\t")
            return
        if args.sets_per_pair is not None and args.sets_per_pair < 1:
            parser.error("--sets-per-pair must be positive")
        run(args.database.resolve(), config, jobs, args.sets_per_pair)
    except (ValueError, KeyError, TypeError, AttributeError, OSError, sqlite3.Error) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()

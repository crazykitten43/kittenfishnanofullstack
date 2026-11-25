# KittenFish Nano Fullstack

Chess engine firmware for the Arduino Nano ESP32. The two evaluators are
HCE-4 and the large-run 704×128 INT16 NNUE from KittenFish Nano's
`l4-20261121` run. The firmware stores the weights in flash and copies them
into internal SRAM at boot. If the allocation fails, HCE-4 remains available
and the NNUE command returns `error nnue sram`.

Build and upload with PlatformIO.

```sh
pio run -e arduino_nano_esp32
pio run -e arduino_nano_esp32 -t upload
pio device monitor -b 115200
```

Send one command per line over USB serial.

| Command | Effect |
| --- | --- |
| `hce 4` | Select the HCE-4 evaluator (power-on default) |
| `nnue` | Select the 704×128 INT16 evaluator |
| `nnue status` | Report the NNUE weight storage location and size |
| `search selectivepvs` | PVS with aspiration, late move reductions, shallow check extensions, and reverse futility (default) |
| `search quietpruning` | PVS with late move reductions and reverse futility |
| `search latemovereductions` | PVS with late move reductions at eligible non-PV nodes |
| `search aspirationpvs` | PVS with a ±40 centipawn aspiration window from depth 3 |
| `start` | Reset to the starting position and clear the transposition table |
| `fen <FEN>` | Set a position and clear the transposition table |
| `play <move>` | Apply a coordinate move such as `e2e4` |
| `go <ms>` | Search for 1–60000 ms and report move, score, depth, and nodes |
| `depth <plies>` | Search to a depth of 1–32 plies with a three-second time limit |
| `opt <mask>` | Select search optimizations and clear the transposition table |

Add the values below to combine optimizations. Use `opt 0` to disable
all six (the power-on default) and `opt 63` to enable all six. Adaptive reductions and widening
only apply to search profiles that use LMR and aspiration, respectively.

| Value | Optimization |
| ---: | --- |
| 1 | Order quiet moves by their cutoff history within the current search |
| 2 | Skip capture ordering when a quiescence stand-pat cutoff is already available |
| 4 | Cache quiescence bounds while prioritizing normal entries for replacement |
| 8 | Reduce eligible eighth and later moves by two plies at depth four or greater |
| 16 | Skip non-checking quiescence captures when their material gain plus 200 cp cannot reach alpha |
| 32 | Widen a failed aspiration bound to 120 cp and then 360 cp before retrying at full width |

Changing evaluator or search profile clears the transposition table. Use
`start` or `fen` before each timed run when comparing cold-table searches.

The PeSTO piece values and tables originate with Ronald Friederich's Rofchade
parameters. The adapted tables are licensed under CC BY-SA 3.0.
[PeSTO source and credits](https://www.chessprogramming.org/PeSTO%27s_Evaluation_Function)
· [CC BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0/)

## Continuous Arduino matches

NanoBench runs an OpenBench-style round robin across a configurable pool of
evaluators and a pool of search profiles and optimization masks. It forms
every engine–search combination and plays every distinct matchup on every
opening. For each matchup and opening, it plays four games to give each
variant both colors on each board. Each side has exclusive use of one Nano
during the game. The runner sets its evaluator, profile and mask and clears
its position, history and transposition table before replaying the opening.

From the repository root on a macOS or Linux host with Python 3.10 or newer:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r benchmark/requirements.txt
.venv/bin/python benchmark/nanobench.py plan benchmark/example.json
```

The example uses the HCE-4 and NNUE evaluators and five search settings
(135 four-game sets, 540 games per pass). Copy the JSON to
`benchmark.local.json` and replace the placeholders with your two USB serial
numbers. Find the serial numbers with `.venv/bin/python -m serial.tools.list_ports -v`.
You can also change board pairs, openings, time per move, or the search pool. Each search entry
combines one profile and one mask from the firmware tables above. Each board
pair independently works through the full matrix. Use each USB serial number
once and record the firmware revision already installed on all boards in
the `firmware` field. That field is experiment metadata supplied by the
operator. The runner uses the existing serial protocol and performs no uploads.

To start a run when ready:

```sh
.venv/bin/python benchmark/nanobench.py run benchmark.local.json results.sqlite3
.venv/bin/python benchmark/nanobench.py status results.sqlite3
.venv/bin/python benchmark/nanobench.py pgn results.sqlite3 > matches.pgn
```

The runner repeats the matrix until stopped. Add `--sets-per-pair 1` for a
four-game smoke run per board pair. A disconnected board, rejected command,
or illegal move logs a failed set and the worker retries eligible work.
A set counts once all four games are saved together. Restart with the same
config and database to resume, or choose a new database when the experiment
changes. Stop with Ctrl-C or SIGTERM. Games that reach the configured ply
limit are recorded as unfinished. The status report includes wins, losses,
draws, unfinished games and recent failures. Exported PGNs record board
assignments and search depth, nodes, score and elapsed time for each move.

The workflow is inspired by [OpenBench](https://github.com/AndyGrant/OpenBench).

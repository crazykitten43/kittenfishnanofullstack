# KittenFish Nano Fullstack

Chess engine firmware for the Arduino Nano ESP32. The two evaluators are
HCE-4 and the large-run 704×128 INT16 NNUE from KittenFish Nano's
`l4-20261121` run. The network weights are compiled into flash.

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
| `search baseline` | Select principal variation search (power-on default) |
| `search alphabeta` | Use full-window alpha-beta child searches |
| `search aspiration` | Use PVS with a ±40 centipawn aspiration window from depth 3 |
| `search lmr` | Use PVS with late move reductions at eligible non-PV nodes |
| `start` | Reset to the starting position and clear the transposition table |
| `fen <FEN>` | Set a position and clear the transposition table |
| `play <move>` | Apply a coordinate move such as `e2e4` |
| `go <ms>` | Search for 1–60000 ms and report move, score, depth, and nodes |

Changing evaluator or search profile clears the transposition table. Use
`start` or `fen` before each timed run when comparing cold-table searches.

The PeSTO piece values and tables originate with Ronald Friederich's Rofchade
parameters. The adapted tables are licensed under CC BY-SA 3.0.
[PeSTO source and credits](https://www.chessprogramming.org/PeSTO%27s_Evaluation_Function)
· [CC BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0/)

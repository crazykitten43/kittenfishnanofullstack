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

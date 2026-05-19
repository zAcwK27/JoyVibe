# JoyVibe

Homebrew Nintendo Switch app to manually control the four vibration parameters of each Joy-Con independently.

## Requirements

- [devkitPro](https://devkitpro.org/) with `switch-dev` and `libnx` installed.
- `DEVKITPRO` environment variable correctly configured in your system.

## Build

To clean previous builds and compile the project generating the final .nro file, run the following commands in the root directory:

```bash
make clean; make
```

Output file: `joyvibe.nro`

## Install

1. Copy the generated `joyvibe.nro` file.
2. Paste it inside the `/switch/` directory on your Nintendo Switch SD card.
3. Launch the application using the Homebrew Menu.

## Controls

### Left Joy-Con

| Action | Button |
| --- | --- |
| `amp_low` +5% | D-Pad Up |
| `amp_high` +5% | D-Pad Down |
| `freq_low` +5% | D-Pad Left |
| `freq_high` +5% | D-Pad Right |
| `amp_low` -5% | SL |
| `amp_high` -5% | SR |
| `freq_low` -5% | L |
| `freq_high` -5% | ZL |

### Right Joy-Con

| Action | Button |
| --- | --- |
| `amp_low` +5% | X |
| `amp_high` +5% | B |
| `freq_low` +5% | Y |
| `freq_high` +5% | A |
| `amp_low` -5% | SR |
| `amp_high` -5% | SL |
| `freq_low` -5% | R |
| `freq_high` -5% | ZR |

### Global Actions

* **Plus (+):** Exit application.
* **Minus (-):** Reset all parameters to 0%.

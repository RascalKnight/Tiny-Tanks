# Tiny Tanks

A wireless 2-player tank battle game built for the ESP32, rendered on a 2.4" TFT shield. No router, no Wi-Fi network required — devices talk directly to each other over **ESP-NOW**.

```
╔══════════════════════════════════════════════════════╗
║               T I N Y   T A N K S                     ║
║   2.4" TFT Shield · MCUFRIEND_kbv · 320×240            ║
║   Wireless 2-player via ESP-NOW (no router)            ║
╚══════════════════════════════════════════════════════╝
```

## How it works

Each player has their own ESP32 + TFT shield + analog joystick + fire button. Every frame, each device:

1. Reads its local joystick/button input
2. Broadcasts that input (and its own HP) to the peer device via ESP-NOW
3. Receives the peer's latest input and replays their tank's movement/firing locally
4. Applies hit detection **only for its own tank** (each device is the authority on whether it got hit) and broadcasts the result

This keeps both screens in sync without a central server — each player's device is authoritative over its own tank's health.

## Hardware requirements

- 2x ESP32 dev boards (tested against `esp32doit-devkit-v1`)
- 2x 2.4" TFT shields compatible with `MCUFRIEND_kbv` (320×240, landscape)
- 2x analog joysticks (X/Y on separate ADC pins)
- 2x momentary push buttons (fire)

### Pinout

| Signal | Pin |
|---|---|
| Joystick X | GPIO 34 |
| Joystick Y | GPIO 35 |
| Fire button | GPIO 21 (`INPUT_PULLUP`) |

## Gameplay

- Each tank has 3 HP and fires up to 3 bullets at a time
- A fixed arena layout with 6 walls blocks movement and bullets
- First tank to reduce the opponent to 0 HP wins
- After a win, both players must press Fire on the win screen to trigger a rematch

## Setup

This is a [PlatformIO](https://platformio.org/) project.

1. Clone the repo:
   ```bash
   git clone https://github.com/RascalKnight/Tiny-Tanks.git
   cd Tiny-Tanks
   ```
2. Open the folder in PlatformIO (VS Code extension, or PlatformIO Core CLI).
3. **Before building for each device**, edit `src/main.cpp`:
   - Set `PLAYER_ID` to `1` on the first device, `2` on the second.
   - Set `PEER_MAC` to the other device's Wi-Fi MAC address (each device prints its own MAC over Serial at 115200 baud on boot — flash one device first, read its MAC, then set it as the peer for the other).
4. Build and upload to each ESP32:
   ```bash
   pio run --target upload
   ```
5. Power on both devices. Each will show a splash screen; press Fire on both to start the match.

### Dependencies

Declared in `platformio.ini` and fetched automatically by PlatformIO:

- [`Adafruit GFX Library`](https://github.com/adafruit/Adafruit-GFX-Library) — display drawing primitives
- [`MCUFRIEND_kbv`](https://github.com/prenticedavid/MCUFRIEND_kbv) — TFT shield driver

## Project structure

```
Tiny-Tanks/
├── platformio.ini      # Board, framework, and library configuration
├── src/
│   └── main.cpp         # Game loop, rendering, input, and ESP-NOW networking
├── include/             # (project headers, currently empty)
├── lib/                 # (private/local libraries, currently empty)
└── test/                # (PlatformIO unit tests, currently empty)
```

## Notes

- The two devices don't need a shared Wi-Fi network — ESP-NOW is a direct peer-to-peer radio protocol between MAC addresses, so this works anywhere within range.
- Joystick center/deadzone is auto-calibrated on boot from the resting analog reading.
- HUD redraws are cached so HP is only repainted on screen when it actually changes, keeping the frame rate steady (~25 fps).

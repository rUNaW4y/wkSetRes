# wkSetRes

`wkSetResCustom` is a WormKit module for Worms Armageddon 3.8.x that lets the player change resolution directly from the multiplayer lobby without manually opening the video settings menu and restarting the game.

The module supports two resolution workflows:

- `/checkres` prints the list of detected resolutions for the current monitor and highlights the active one.
- `/setres 1`, `/setres 2`, etc. applies one of the listed resolutions by index.
- `/setres 1920x1080` applies a custom resolution request.

## Main behavior

- Designed for Worms Armageddon `3.8.x`.
- Supports both listed monitor modes and arbitrary custom `widthxheight` values.
- Keeps a detailed runtime log in `wkSetResCustom.log` inside the game directory.
- The compiled module binary is included in [`dist/wkSetResCustom.dll`](dist/wkSetResCustom.dll).

## Files

- `src/`: module source code
- `CMakeLists.txt`: build definition
- `resource.rc.in`: version resource template
- `dist/wkSetResCustom.dll`: prebuilt DLL ready to copy into the game folder

## Installation

Copy `dist/wkSetResCustom.dll` into:

```text
C:\Program Files\Worms Armageddon\Team17\Worms Armageddon
```

## Commands

```text
/checkres
/setres 1
/setres 2
/setres 1366x768
```

## Build notes

This repository currently keeps the same build layout used during development. The CMake project expects the sibling folder `../wkWormsTracker` to be available locally because it reuses its `hacklib` sources and local libraries such as `capstone` and `Polyhook_2`.

## Status

This repository contains the latest local development state of the module together with the current compiled DLL.

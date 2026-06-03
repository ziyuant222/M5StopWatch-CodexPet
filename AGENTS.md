# Repository Guidelines

## Project Structure & Module Organization

This is an ESP-IDF firmware project for the M5Stack StopWatch. The app entrypoint is `main/main.cpp`; application modules live under `main/apps/`, with shared UI/input/audio helpers in `main/apps/common/`. Hardware support is in `main/hal/`. Assets are under `main/assets/`, including `images/`, `fonts/`, `sfx/`, `gif_source/`, and generated `gif_anim/` files. Third-party components are split between `components/` and `managed_components/`. Utility scripts live in `tools/`, for example `tools/capture_screen.py` for serial framebuffer screenshots. Build directories such as `build/`, `build-idf55/`, and `build-codex-only/` are generated output.

## Build, Test, and Development Commands

Fetch component repositories before first build:

```bash
python3 ./fetch_repos.py
```

Build with ESP-IDF v5.5.4:

```bash
idf.py build
```

Flash a connected ESP32-S3 device:

```bash
ESPPORT=/dev/cu.usbmodem2101 idf.py flash
```

For the smaller Codex Buddy-focused build, use the existing CMake build dir:

```bash
cmake --build build-codex-only
ESPPORT=/dev/cu.usbmodem2101 cmake --build build-codex-only --target flash
```

Capture the device screen without resetting it:

```bash
python3 tools/capture_screen.py --port /dev/cu.usbmodem2101 --output reports/screen.png
```

## Coding Style & Naming Conventions

Use C++17-style code consistent with the existing ESP-IDF modules. Keep indentation at four spaces. Name app classes as `AppName` or `AppCodexBuddy`, private members with a leading underscore, and constants with `kPascalCase` or `constexpr` names. Prefer local helpers and existing HAL APIs over new abstractions. Do not commit generated build output.

## Testing Guidelines

There is no standalone unit-test suite in this repo. Verify changes with a firmware build, targeted serial commands, and screenshots when UI is affected. For device behavior, run through the relevant app flow and watch serial output for `assert failed`, `Guru Meditation`, `Backtrace`, or unexpected resets.

## Commit & Pull Request Guidelines

Existing history uses concise, imperative messages such as `update v0.5` and `fix missing ... dependency`. Keep commits focused and describe the user-visible firmware change. PRs should include a short summary, build/flash verification, affected hardware, screenshots for UI changes, and linked issues when available.

## Agent-Specific Instructions

Preserve user changes in dirty worktrees. Use `rg` for search, prefer `apply_patch` for manual edits, and verify with build or device evidence before reporting completion.

# Contributing to Javelin

Thanks for your interest in contributing to Javelin! This document covers the guidelines for submitting code, reporting issues, and contributing translations.

## Getting Started

1. Fork the repository
2. Clone your fork with submodules:
   ```bash
   git clone --recurse-submodules https://github.com/<your-username>/Javelin.git
   ```
3. Set up [devkitPro](https://devkitpro.org/) with the `switch-dev` package group
4. Verify you can build: `make -j$(nproc)`

## Development Environment

- **C++ Standard**: C++17 with `-fno-rtti -fno-exceptions`
- **Target**: ARM64 (aarch64-none-elf) via devkitA64
- **No tests or linter** are currently configured. Verify your changes compile cleanly and test on hardware or an emulator.

## Submitting Changes

### Branch Naming

- `feature/<description>` for new features
- `fix/<description>` for bug fixes
- `refactor/<description>` for code restructuring

### Pull Requests

1. Create a feature branch from `main`
2. Keep commits focused - one logical change per commit
3. Make sure the project builds with `make -j$(nproc)` before submitting
4. Open a pull request against `main` with a clear description of what changed and why
5. Reference any related issues

### Code Style

- Follow the existing patterns in the codebase
- Use `snake_case` for C functions and file names
- Use `camelCase` for C++ methods and local variables
- Use `PascalCase` for C++ types and structs
- Keep lines reasonable in length
- Use `#pragma once` for header guards
- Headers in `include/` should mirror the `source/` directory structure

### Commit Messages

- Use imperative mood ("Add feature" not "Added feature")
- Keep the first line under 72 characters
- Add a body for non-trivial changes explaining the "why"

## Architecture Overview

Understanding the codebase before contributing:

- **Main thread** handles rendering (EGL/OpenGL + ImGui) and input at ~60 FPS
- **MTP thread** runs on-demand for USB communication with 1ms polling
- **Event system** (`libs/EventBus/`) provides thread-safe communication between subsystems
- **GUI Manager** subscribes to events and renders notifications, modals, and screens

Key directories:
- `source/mtp/` - MTP protocol, storage backends, install/dump/save/gamecard handlers
- `source/install/` - NSP/XCI parsing, NCA installation, CNMT metadata
- `source/dump/` - Installed game and gamecard dump logic
- `source/tickets/` - Ticket browser and detail viewer
- `source/core/` - Event types and GUI manager
- `source/ui/` - ImGui platform backend (gamepad + touchscreen) and OpenGL renderer
- `source/i18n/` - Localization system
- `libs/libnx-ext/` - Extended IPC wrappers for Switch services (ES, etc.)

## Adding New Features

### New MTP Storage

1. Define a storage ID in `include/mtp/mtp_storage.h`
2. Add mount/scan logic in `source/mtp/mtp_storage.cpp`
3. Register the storage in the protocol handler

### New Screens

1. Add a screen enum value in `include/core/GuiEvents.h`
2. Add state struct and render function in a new source/include pair
3. Wire it up in `source/main.cpp` render loop and the GUI manager

### New Localization Keys

1. Add keys to the `DEFAULT_TRANSLATIONS` map in `tools/gen_translations.cpp`
2. Add the same keys to `romfs/javelin/i18n/en.json`
3. Use `TR("your.key")` in source code
4. Run `make gen_translations` to regenerate embedded translations

## Translations

We use [Crowdin](https://crowdin.com/project/javelin) for community translations. See [LOCALIZATION.md](LOCALIZATION.md) for the full localization guide.

To add a string that needs translation:
1. Add it to `DEFAULT_TRANSLATIONS` in `tools/gen_translations.cpp` (this is the source of truth for English)
2. Add it to `romfs/javelin/i18n/en.json`
3. It will automatically be picked up by Crowdin for other languages

## Reporting Issues

- Use the [GitHub issue tracker](https://github.com/1312Delta/Javelin/issues)
- Include your Switch firmware version and Atmosphere version
- Describe the steps to reproduce the issue
- Include any error messages or log output if available

## License

By contributing to Javelin, you agree that your contributions will be licensed under the [MIT](LICENSE).

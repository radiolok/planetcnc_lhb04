# Code Review: Quality and Stability Task List

Review of the whole codebase except the tool programs (`tools/mpg_gui`, `tools/tng_probe`),
which were only skimmed. The project could not be built on Linux during the review:
CMake configure fails because hidapi is forced to its libusb backend and libusb is not
installed (see task 27).

Tasks are ordered by risk, highest first.

## P0: Safety and correctness (machine motion)

- [ ] **1. The wheel's jog target goes stale, so the axis can drive back to an old position.**
  `target_[axis]` is set once (`src/logic/JogController.cpp:106`) and never re-synced.
  After the servo stops, `runServo` still runs every tick while an axis is selected. If the
  axis moves by any other route (Home button, a G-code program, jogging in the TNG GUI),
  the difference from the old target exceeds 0.15 mm and the servo drives the axis back to
  where it was. This also happens after E-stop is released.
  *Fix:* reset the target to NaN whenever the servo stops or the machine is not idle, and
  only follow a target while there are fresh wheel counts.
- [ ] **2. The axis can keep moving forever if a position read fails.**
  At `src/logic/JogController.cpp:127`, when `infoMotorPosition` returns NaN the function
  returns even while `servoActive_` is true, so the controller keeps jogging at the last
  speed it was given.
  *Fix:* call `stopServo()` on a failed read, and after repeated `jog()` failures.
- [ ] **3. No block on jogging while a program runs or the machine is paused.**
  `JogController::tick()` only checks E-stop and attach mode. Add `running` / `!idle` to
  that check.
- [ ] **4. Two threads use the same USB device handle unsafely.**
  `UsbPollThread` closes and reopens `writeDevice_` (`src/threads/UsbPollThread.cpp:49`,
  `:76-77`) while `DisplayThread` calls `isOpen()` and `sendFeatureReport()` on it. This can
  use a freed handle and crash.
  *Fix:* let one thread own the device, or guard it with a mutex or `shared_ptr` handoff.
- [ ] **5. The jog controller is called from two threads at shutdown.**
  `src/main.cpp:238` calls `jogController.stopNow()` while the jog thread may still be
  inside `tick()`. `JogThread::run` already calls `stopNow()` on exit, so remove the call
  in `main`.
- [ ] **6. Shutdown can crash inside the TNG library.**
  At `src/main.cpp:254` the TNG thread may be detached while still running inside the
  library; `~TngApi` then calls `unload()`, which frees the library under it.
  *Fix:* never unload while that thread is alive, or `std::quick_exit` in that case.
- [ ] **7. Ctrl+C is not handled during startup.**
  Signal handlers are installed at `src/main.cpp:222`, after the TNG start-up wait of up
  to 30 s. Install them first.

## P1: Features that silently do nothing, and config dangers

- [ ] **8. Step size can never change.** `jogging.step_sizes` and
  `SharedState::stepSizeIndex` are never read. The `step` button maps to
  `toggle_jog_mode`, and `jogMode` is never read by any logic. Either implement step-size
  cycling and step vs. continuous mode, or remove them.
- [ ] **9. Several config options are never used:** `verify_checksum` (checksum mismatches
  are logged but never rejected, although the comment at `src/usb/XhcPendant.h:22` says
  they are), `auto_detect`, `ButtonAction::value`, `estopBlocked`, `jogging.mode`,
  `PendantState::button1`, and `parseUint16`.
- [ ] **10. One config error discards the whole file.** `ConfigManager::loadOrDefault`
  (`src/config/ConfigManager.cpp:112`) falls back to defaults on any error. The defaults
  have **no button mappings**, so E-stop, Stop and the rest silently do nothing, with only
  a warning logged. Make config errors fatal, or fall back per section, and ship default
  button bindings.
- [ ] **11. Config values are not validated.** `usb_hz` or `display_hz` above 1000 gives a
  0 ms period, so the loop spins at full CPU. Also unchecked: negative or zero
  `jog_speed`, `max_speed` and step sizes, unknown action names, and unknown button names.
  Validate at load time and report every problem at once.
- [ ] **12. `Logger::init` can throw** when the log-file path is bad, which ends the
  program with an uncaught exception. An invalid level string silently turns logging off.
- [ ] **13. `TngApi::load` ignores its own required-symbol check.** It collects "required"
  symbol errors, then clears them (`src/planetcnc/TngApi.cpp:154-157`), so the library
  loads even with required functions missing. Fail when a required symbol is missing.
- [ ] **14. Failed API reads look like real values.** `getParam` returns `0.0` on failure,
  so a feed-override turn of the wheel before TNG has started sets the override to about
  10 %. Change the interface to something like `std::optional<double>`.

## P2: Threading and robustness

- [ ] **15. Button actions run on the USB poll thread.** `XhcPendant::onButton` →
  `ButtonHandler` makes blocking TNG calls (`cmdExec`, `startCode`) under the API mutex,
  so pendant input stops being read while they run. Move actions to a queue with its own
  worker thread.
- [ ] **16. E-stop status only refreshes while the pendant is connected.**
  `StateReader::read()` runs only inside `DisplayThread` when the device is open, at 20 Hz.
  Read E-stop separately, or query it directly in the jog tick.
- [ ] **17. Nothing stops callers from using shared state without the lock.**
  `SharedState` members and helpers like `selectedAxis()` must be called under the mutex,
  but nothing enforces it. Consider copying a snapshot under the lock, or adding
  thread-safety annotations.
- [ ] **18. Debounce can lose a press.** A press within 50 ms of a release updates
  `lastButton_` but is dropped, and no later edge re-triggers it
  (`src/usb/XhcPendant.cpp:57-61`).
- [ ] **19. Timed loops drift.** They use `sleep_for(period)` rather than `sleep_until`,
  and the jog loop borrows the USB period (`src/main.cpp:218`). Give the jog loop its own
  configurable tick.
- [ ] **20. Values sent to the LCD can overflow.** `writeS16(llround(feed*60))` and the
  integer part of `encodeCoordinate` wrap around silently. Clamp them.
- [ ] **21. Non-ASCII text is corrupted.** Library paths are widened byte by byte
  (`src/planetcnc/TngApi.cpp:27`) and USB device strings are narrowed the same way in
  `narrow()` (`src/usb/HidDevice.cpp:13`). Use proper UTF-8 conversion.

## P3: Code quality

- [ ] **22. Button actions are compared as strings on every press.** Turn them into an enum
  when the config loads, which also provides validation for task 11.
- [ ] **23. Duplicated override constants.** The 0.0–2.5 clamp appears in both
  `src/logic/ButtonHandler.cpp:76,81` and `src/logic/JogController.cpp:14-15`, and the
  servo's deceleration is hard-coded as a copy of `_motion_maxdec`. Put them in shared
  constants or config.
- [ ] **24. Duplicated or misleading code.**
  - Feed and spindle values are read twice: `DisplayUpdater` calls the API directly
    although `StateReader` already stores them.
  - The "hello frame" in `DisplayThread` is only a log message; no special frame is sent.
  - The stubs `getCmdCount` and `isCmdEnabled` return fake values.
  - `open()` and `openPath()` in `HidDevice` duplicate each other.
- [ ] **25. The A axis always shows 0.** It is displayed and jogged but never read into
  state (`src/logic/DisplayUpdater.cpp:46-49`).
- [ ] **26. The logging helpers aren't format-checked.** Add
  `__attribute__((format(printf, 1, 2)))` to the `log*` helpers, or switch to spdlog's
  `fmt`-style calls.

## P4: Build, tests and CI

- [ ] **27. CMake fails outside Windows.** `mpg_gui` always builds against Win32/OpenGL
  libraries, and hidapi is forced to its libusb backend. Guard the GUI with `if(WIN32)`
  plus an option, and allow the hidraw backend or system packages.
- [ ] **28. Build settings.** Wrap tests in `BUILD_TESTING` and tools in an option.
  Replace `file(GLOB_RECURSE)` with an explicit source list. Add
  `-Wpedantic -Wshadow -Wconversion`, plus warnings-as-errors in CI.
- [ ] **29. There is no CI at all.** Add GitHub Actions for a Windows MSVC build and a
  Linux build with `ctest`, plus ASan/UBSan and TSan jobs. TSan would catch tasks 4 and 5.
- [ ] **30. Test gaps.** Nothing tests `XhcPendant` (debounce, sleeping state, count
  accumulation), `ConfigManager` (partial or invalid YAML), the USB reconnect path, or the
  scenarios behind tasks 1–3 (target resync after an external move, NaN position, program
  running). Consider switching the home-made test harness to Catch2 or doctest.
- [ ] **31. Add `.clang-format` and `.clang-tidy`** (bugprone-\*, concurrency-\*,
  cppcoreguidelines-\*) and enforce them in CI.

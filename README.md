# mpgd — адаптер пульта XHC LHB04 ↔ PlanetCNC TNG

`mpgd` (Manual Pulse Generator Daemon) — кросс-платформенный фоновый процесс,
связывающий USB-HID пульт **XHC LHB04** (VID `0x10CE`, PID `0xEB70` и варианты;
также совместимые клоны, например **KTURT**) с управляющим ПО **PlanetCNC TNG**
(контроллеры Mk3/4) через C API библиотеки
`PlanetCNCLib64.dll` / `libPlanetCNCLib64.so`.

Демон читает кнопки и маховик (MPG) пульта, выполняет джог осей, обновляет
LCD-дисплей пульта (координаты, подача, обороты шпинделя), обрабатывает кнопки
согласно YAML-конфигурации и автоматически переподключается при потере USB.

---

## Возможности (v1)

- **Чтение пульта** — опрос HID-пакетов (report `0x04`, 6 или 8 байт в
  зависимости от устройства) с частотой 100 Гц, edge-детекция кнопок с
  debounce 50 мс, накопление импульсов маховика.
- **Джог маховиком (MPG)** — режимы Step и Continuous; выбор оси ручкой
  (X/Y/Z/A); шаг определяется положением шагового переключателя
  (`0.001 / 0.01 / 0.1 / 1.0` мм).
- **Override подачи и шпинделя** — вращение маховика при ручке в положении
  Feed/Spindle изменяет `SpeedFeedOverride` / `SpeedSpindleOverride` через
  `SetParam`.
- **LCD-дисплей** — координаты (формат `±XXXX.XXX`), % override, подача
  (мм/мин) и обороты шпинделя (RPM); пакет дисплея кодируется по эталону
  LinuxCNC `xhc-hb04`.
- **Кнопки** — 18 кнопок, сопоставление «кнопка → действие» в YAML.
- **Авто-переподключение USB** — при потере связи опрос каждые 2 с,
  восстановление состояния.
- **Graceful shutdown** — Ctrl+C / SIGINT / SIGTERM: `JogStop()`, завершение
  потоков, `Exit()` TNG.
- **Работа как Windows-консольное приложение**; код кросс-платформенный
  (Linux — отдельный этап).

### Вне области (v1)

- Беспроводная версия WHB04B.
- G-code интерпретатор и GUI-конфигуратор.
- Сервисная обёртка (Windows Service / systemd) — выносится в последний этап.

---

## Режимы интеграции с TNG

| Режим | CLI | Поведение |
|---|---|---|
| В-процессе, GUI | (по умолчанию) | `Run(false)` — запускает TNG с интерфейсом в том же процессе |
| В-процессе, headless | `--no-gui` | `Run(true)` — TNG без интерфейса |
| Профиль | `--profile <name>` | `RunProfile(hideUI, profile)` |
| Attach | `--attach` | Подключение к уже запущенной внешней TNG через named pipes. **Джог отключён** (функция `Jog` недоступна по pipe-интерфейсу), работают кнопки и дисплей |

Библиотека загружается динамически через `LoadLibrary`/`GetProcAddress`
(`dlopen`/`dlsym` на Linux) — заголовок SDK не требуется. Все вызовы API
сериализуются одним mutex (SDK не гарантирует потокобезопасность).

---

## Требования

- **Windows 10/11 x64** (основная целевая платформа) или Linux.
- **Visual Studio 2022** с компонентом C++ (или GCC/Clang 11+ на Linux).
- **CMake 3.22+**.
- Доступ в интернет при первой сборке (зависимости скачиваются через
  CMake `FetchContent`).

Зависимости (подтягиваются автоматически):

| Библиотека | Версия | Назначение |
|---|---|---|
| [hidapi](https://github.com/libusb/hidapi) | 0.14.0 | USB HID |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp) | 0.8.0 | конфигурация |
| [spdlog](https://github.com/gabime/spdlog) | 1.14.1 | логирование |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.90.9 | GUI тестовой утилиты `mpg_gui` |

---

## Сборка

```powershell
# Windows (Visual Studio 2022)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

```bash
# Linux (GCC/Clang)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Результат — `build/Release/mpgd.exe` (Windows) или `build/mpgd` (Linux).
Все зависимости линкуются статически, исполняемый файл самодостаточен.

Вместе с демоном собирается тестовая GUI-утилита `mpg_gui.exe` (см. раздел
«Диагностика» и [`tools/README.md`](tools/README.md)):

```powershell
cmake --build build --config Release --target mpg_gui
```

### Запуск тестов

```powershell
ctest --test-dir build -C Release --output-on-failure
```

---

## Использование

```powershell
mpgd [--config <path>] [--profile <name>] [--no-gui] [--attach] [--sniff] [--list] [--help]
```

| Параметр | Описание |
|---|---|
| `--config <path>` | Путь к YAML-конфигу (по умолчанию `mpgd.yaml`) |
| `--profile <name>` | Имя профиля PlanetCNC для `RunProfile` |
| `--no-gui` | Запуск TNG headless (`Run(true)`) |
| `--attach` | Подключение к запущенной внешней TNG |
| `--sniff` | Захват/лог сырых пакетов пульта без обращения к TNG |
| `--list` | Список HID-устройств и выход |
| `--help` | Справка |

Примеры:

```powershell
# Обычный запуск с GUI TNG и конфигом по умолчанию
mpgd

# Headless с конкретным профилем
mpgd --no-gui --profile MyProfile

# Подключение к уже запущенной TNG (только кнопки + дисплей)
mpgd --attach

# Сниффер пакетов (отладка протокола)
mpgd --sniff
```

---

## Диагностика

Для проверки работы адаптера (кнопки, маховик, селектор оси) есть тестовая
GUI-утилита **`mpg_gui`** — оконное приложение, читающее пульт напрямую без
обращения к TNG и показывающее сырые пакеты и их расшифровку в реальном
времени (включая захват всех пакетов в `mpg_gui_raw.log`). Подробности — в
[`tools/README.md`](tools/README.md).

```powershell
build\Release\mpg_gui.exe
```

Полезные режимы демона для диагностики:

- `mpgd --list` — список HID-устройств (VID/PID, строки производителя);
- `mpgd --sniff` — захват и hex-дамп сырых пакетов пульта в консоль (без TNG).

---

## Конфигурация (`mpgd.yaml`)

Пример см. в [`config/mpgd.yaml`](config/mpgd.yaml). Основные секции:

```yaml
device:
  vendor_id: 0x10CE
  product_ids: [0xEB70, 0xEB71, 0xEB93]
  auto_detect: true
  verify_checksum: false      # эталонный драйвер чексумму не проверяет

planetcnc:
  profile: ""                 # "" = профиль по умолчанию
  lib_path: ""                # "" = автоопределение DLL/SO
  attach: false

jogging:
  step_sizes: [0.001, 0.01, 0.1, 1.0]   # мм
  max_speed: 1000.0                     # мм/мин
  mode: "step"                          # step | continuous
  override_step: 10.0                   # % на клик маховика
  feed_override_param: "SpeedFeedOverride"
  spindle_override_param: "SpeedSpindleOverride"

polling:
  usb_hz: 100
  display_hz: 20
  reconnect_ms: 2000
  button_debounce_ms: 50
  display_always: false       # слать LCD и при ручке OFF

logging:
  level: "info"               # trace|debug|info|warn|error|critical|off
  file: ""                    # "" = только консоль

buttons:
  reset:       { action: "estop" }
  stop:        { action: "stop" }
  start_pause: { action: "toggle_start_pause" }
  home:        { action: "home_all" }
  spindle:     { action: "spindle_toggle" }
  zero:        { action: "set_work_zero" }
  ...
```

### Ключи кнопок

Канонические имена кнопок (18-кнопочный пульт, раскладка
`xhc-hb04-layout2`):

| Код | Кнопка | Код | Кнопка |
|---|---|---|---|
| 0x01 | `goto_zero` | 0x0D | `step` |
| 0x02 | `start_pause` | 0x0E | `mode` |
| 0x03 | `rewind` | 0x0F | `macro_6` |
| 0x04 | `probe_z` | 0x10 | `macro_7` |
| 0x05 | `macro_3` | 0x16 | `stop` |
| 0x06 | `half` | 0x17 | `reset` |
| 0x07 | `zero` | 0x08 | `safe_z` |
| 0x09 | `home` | 0x0C | `spindle` |
| 0x0A | `macro_1` | 0x0B | `macro_2` |

### Поддерживаемые действия

`estop`, `stop`, `start`, `pause`, `pause_toggle`, `toggle_start_pause`,
`home_all`, `set_work_zero`, `set_work_zero_xy`, `set_work_zero_z`,
`spindle_toggle`, `flood_toggle`, `mist_toggle`, `feed_override`
(`delta` в %), `spindle_override` (`delta` в %), `toggle_jog_mode`,
`command` (именованная команда TNG в `cmd`, напр. `Machine.Home`),
`gcode` (строка G-code в `cmd`), `noop`.

---

## Архитектура

```
 ┌──────────────┐     ┌─────────────────────┐     ┌───────────────┐
 │  XHC LHB04   │────▶│       mpgd          │────▶│ PlanetCNC TNG │
 │  (USB HID)   │◀────│     (daemon)        │◀────│   (C API)     │
 └──────────────┘     └─────────────────────┘     └───────────────┘
   report 0x04 (6/8 B)     потоки + shared state     LoadLibrary +
   report 0x06 (6×8 B)        (mutex)               GetProcAddress
```

### Потоки

| Поток | Период | Назначение |
|---|---|---|
| Main | событийный | CLI, конфиг, запуск/attach TNG, сигналы, graceful shutdown |
| USB Poll | 10 мс | открытие коллекций входа/дисплея, `hid_read`, разбор пакетов, debounce кнопок, накопление дельты маховика, реконнект |
| Jog Process | 10 мс | вызов `Jog()`/`Jog9()`/`JogStop()`, override подачи/шпинделя |
| Display Update | 50 мс | чтение `Info*`, кодирование LCD, feature report `0x06` |

Все вызовы TNG-API сериализуются через `TngApi` (внутренний mutex);
общее состояние — `SharedState` под mutex.

### Структура проекта

```
mpgd/
├── CMakeLists.txt
├── PLAN.md                           # план работ и статус
├── config/mpgd.yaml
├── include/TNG_API.h                 # декларации C API (LoadLibrary, не линкуется)
├── src/
│   ├── main.cpp                      # оркестрация
│   ├── config/ConfigManager.*        # YAML-конфиг
│   ├── usb/
│   │   ├── XhcProtocol.h             # константы протокола (из xhc-hb04)
│   │   ├── PacketParser.*            # разбор/кодирование пакетов, чексумма
│   │   ├── HidDevice.*               # обёртка hidapi
│   │   └── XhcPendant.*              # state machine пульта (edge/debounce)
│   ├── planetcnc/
│   │   ├── ITngApi.h                 # интерфейс TNG-API (mock в тестах)
│   │   ├── TngApi.*                  # динамическая загрузка SDK + mutex
│   │   └── StateReader.*             # чтение Info*/состояния
│   ├── logic/
│   │   ├── SharedState.h             # общее состояние под mutex
│   │   ├── JogMath.h                 # чистые функции джога (тестируемые)
│   │   ├── JogController.*           # маховик → Jog/override
│   │   ├── ButtonHandler.*           # кнопки → действия
│   │   └── DisplayUpdater.*          # кодирование LCD
│   ├── threads/
│   │   ├── UsbPollThread.*
│   │   ├── JogThread.*
│   │   └── DisplayThread.*
│   └── utils/
│       ├── Logger.*                  # обёртка spdlog
│       └── Daemon.*                  # сигналы/остановка
├── tools/
│   └── mpg_gui/main.cpp              # тестовая GUI-утилита (Dear ImGui)
└── tests/                            # unit-тесты (packet/jog/display)
```

---

## Протокол пульта (XHC LHB04)

Формат полей и кодирование дисплея перенесены из эталонного драйвера
LinuxCNC `xhc-hb04.cc` для устройства `10CE:EB70`.

### Входящий пакет (report `0x04`)

| Байт | Поле |
|---|---|
| 0 | Report ID (`0x04`) |
| 1 | Код нажатой кнопки (`0x00` — нет) |
| 2 | Вторичная кнопка |
| 3 | Положение ручки выбора оси |
| 4 | Дельта маховика (int8, −128…+127, 50 имп./оборот) |
| 5 | Шаговый/feed ротатор (у клонов KTURT — дубль кода кнопки) |
| 6 | Seed (только у эталонного XHC) |
| 7 | Чексумма — XOR байтов 1–6 (только у эталонного XHC) |

> Длина отчёта — **8 байт** у эталонного устройства XHC и **6 байт** у
> совместимых клонов (например, KTURT), которые не передают seed и чексумму.
> `PacketParser::parseInput` принимает обе длины: при 6 байтах чексумма не
> проверяется (`hasChecksum == false`), поля кнопок/оси/маховика читаются
> одинаково. Отчёты приходят только при активности (кнопка/маховик/ручка);
> в простое устройство молчит.

Положения ручки (ось): `OFF=0x00`, `X=0x11`, `Y=0x12`, `Z=0x13`,
`Spindle=0x14`, `Feed=0x15`, `A=0x18`.

> ⚠️ Значения из исходного ТЗ (0x06=OFF, 0x14=A и т.д.) отличаются от
> реального протокола — здесь использованы значения из `xhc-hb04.cc`.
> Таблицу кодов кнопок следует подтвердить на конкретном экземпляре пульта
> (сниффер `--sniff`).

### Исходящий пакет (дисплей, report `0x06`)

42-байтный буфер, мультиплексированный в **6 × 8-байтных** отчётов
(каждый начинается с report ID `0x06`):

| Смещение | Поле |
|---|---|
| 0–2 | Заголовок `0xFE 0xFD 0x0C` |
| 3–26 | 6 координат (3 work + 3 machine) по 4 байта: int16 целая + int16 дробь (×10000, бит 15 = знак) |
| 27–28 | Feed override ×100 |
| 29–30 | Spindle override ×100 |
| 31–32 | Feed value ×60 (мм/мин) |
| 33–34 | Spindle RPS ×60 (RPM) |
| 35 | Код шага (1/10/100/1000 → `0x01/0x03/0x08/0x0A`) |
| 36 | Флаги (бит 7 — значок дюймов) |

---

## Безопасность

- Блокировка джога при E-Stop (`IsEStop()`).
- `JogStop()` при простое маховика (continuous), потере связи и завершении.
- Клампинг накопленных импульсов (защита от переполнения, `JogMath.h`).
- Debounce кнопок 50 мс.
- Ограничение скорости джога через `max_speed` в конфиге.

---

## Ограничения и дальнейшие шаги

- Точная семантика `Jog(step, x, y, z)` (шаг vs скорость в normal-режиме) и
  имена параметров скорости джога уточняются на реальном контроллере (Phase 3
  плана); имена override-параметров настраиваются в YAML.
- Дисплей пишется через feature report (SET_REPORT `0x06`). На Windows пульт
  перечисляется как несколько HID-коллекций: входной отчёт (`0x04`) и
  feature-отчёт дисплея (`0x06`) живут в разных device-узлах, поэтому демону
  и `mpg_gui` нужно открывать их отдельно (пробой feature report).
- Чексумма (XOR) опциональна и по умолчанию выключена — эталонный драйвер
  её не проверяет; включить после сверки с захватом пакетов.
- Ось A/B/C: джог реализован через `Jog9`; LCD для A/B/C в v1 не отображается.
- Приёмка AC-01…AC-06 (точность шага, 24-часовой прогон, Linux-порт,
  udev-правило `ATTR{idProduct}=="eb70", ATTR{idVendor}=="10ce", MODE="0666"`)
  — отдельный этап после Windows.

---

## Ссылки

- PlanetCNC TNG API Reference (локально: `PlanetCNC_TNG_API.pdf`)
- LinuxCNC `xhc-hb04` man page и исходник `src/hal/user_comps/xhc-hb04.cc`
- Раскладки кнопок: `lib/hallib/xhc-hb04-layout{1,2}.cfg`

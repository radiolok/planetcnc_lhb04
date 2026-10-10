# PlanetCNC TNG API — справочник

Документ описывает C API библиотеки **PlanetCNC TNG** (`PlanetCNCLib64.dll` на
Windows, `libPlanetCNCLib64.so` на Linux), как оно используется в проекте
`mpgd`. Декларации собраны из PDF «PlanetCNC_TNG_API.pdf» (API Reference,
2020/07/21) и уточнены на реальном контроллере Mk3/4 (см. `STAGE3_FINDINGS.md`).

> SDK **не поставляет публичный заголовок**. Все сигнатуры объявлены в
> `include/TNG_API.h` и загружаются динамически через `LoadLibrary`/`dlopen` +
> `GetProcAddress`/`dlsym` (обёртка `src/planetcnc/TngApi.cpp`). Символы с
> библиотекой не линкуются.

---

## 1. Загрузка библиотеки

| Аспект | Значение |
|---|---|
| Имя (Windows) | `PlanetCNCLib64.dll` |
| Имя (Linux) | `libPlanetCNCLib64.so` |
| Соглашение вызова | **CDECL** |
| Зависимость | `PlanetCNCCore64.dll` (лежит рядом; грузится с `LOAD_WITH_ALTERED_SEARCH_PATH`) |
| Проверка | после загрузки вызывается `GetVer()`; `0` означает «API недоступно» |

Функции, помеченные в `TngApi::load` как `required`, обязаны присутствовать в
экспорте (`Run`, `RunProfile`, `Exit`, `ExitForce`, `GetVer`, `GetCmdId`,
`CmdExec`, `CmdExecStr`, `CmdExecVal`, `SetParam`, `GetParam`, `InfoSpeed`,
`InfoSpindle`, `InfoWorkPosition3`, `InfoMotorPosition3`, `Jog`, `JogStop`).
Остальные — опциональны и при отсутствии возвращают «безопасные» значения.

> В конкретной DLL экспорт `IsRunning` называется **`IsRun`** (в мануале —
> `IsRunning`). Обёртка резолвит оба имени, предпочитая `IsRunning`.
>
> **Внимание:** `IsRun` означает «TNG запущена», а **не** «выполняется
> программа» — in-process он всегда `true`. Признак «ничего не выполняется» —
> `IsStop` (см. §4).

---

## 2. Модель потоков

- Все вызовы API (кроме `Run`/`RunProfile`/`Exit`/`ExitForce`) сериализуются
  **одним mutex** в `TngApi` — SDK не гарантирует потокобезопасность.
- **`Run(hideUI)` / `RunProfile(hideUI, profile)` блокируют**: крутят цикл
  сообщений TNG на вызывающем потоке до `Exit()`. Поэтому их вызывают на
  **отдельном потоке** и **без удержания mutex** (иначе дедлок всех прочих
  вызовов).
- `Exit()` может не остановить in-process TNG сразу — используется
  `ExitForce()` + ограниченный grace-период.

---

## 3. Запуск и завершение

```c
int  Run(bool hideUI);                    // запуск TNG, hideUI = headless
int  RunProfile(bool hideUI, const char* profile);
void Exit();
void ExitForce();
int  GetVer();                            // 0 = не готово
```

Режимы интеграции (см. `README.md`):

| Режим | Вызов | Примечание |
|---|---|---|
| In-process GUI | `Run(false)` | по умолчанию |
| In-process headless | `Run(true)` | `--no-gui` |
| Профиль | `RunProfile(hideUI, name)` | `--profile` |
| Attach | не вызывается | подключение к внешней TNG через named pipes |

---

## 4. Статус (`bool (*)()`)

| Функция | Описание |
|---|---|
| `IsRunning` (alias `IsRun`) | TNG запущена (in-process всегда `true`; **не** признак выполнения программы) |
| `IsRunningExt` | запущена внешняя TNG (detect для attach) |
| `IsInitialized` | TNG инициализирована |
| `IsControllerReady` | контроллер готов |
| `IsIdle` | станок в простое |
| `IsEStop` | активен E-Stop |
| `IsStop` | ничего не выполняется; `false` при любом движении (программа, пауза, `G4`, в т.ч. наш `MoveAxis`) |
| `IsPause` | станок на паузе |

---

## 5. Команды станка

```c
bool EStop(bool on);      // false — снять E-Stop
bool EStopToggle();
bool Stop();
bool Pause(bool on);
bool PauseToggle();
bool Start();
```

---

## 6. Именованные команды

```c
int  GetCmdId(const char* name);          // id команды, <0 — не найдена
bool CmdExec(int id);
bool CmdExecStr(int id, const char* str);
bool CmdExecVal(int id, double val);
```

Имена команд, используемые в `mpgd` (из примера профиля «Handwheel»):

| Имя | Действие |
|---|---|
| `Machine.Emergency_Stop` | E-Stop |
| `Machine.Start` / `Machine.Pause` / `Machine.Stop` | старт/пауза/стоп |
| `Machine.Home` | хоминг всех осей |
| `Machine.Spindle` | вкл/выкл шпинделя |
| `Machine.Flood` / `Machine.Mist` | охлаждение |
| `Machine.Work_Position.Offset.To_Zero` | обнулить рабочую позицию |
| `Machine.Work_Position.Axis_To_Zero.XY` | ноль XY |
| `Machine.Work_Position.Axis_To_Zero.Z` | ноль Z |
| `Machine.Tool_Offset.Measure_Length` | замер длины инструмента |

> ⚠️ `GetCmdId` в **attach-режиме** возвращает `0` для любого имени (включая
> заведомо неверные) — резолв через pipe не работает. В in-process-режиме часть
> имён также требует дальнейшей проверки.

---

## 7. Параметры

```c
bool   SetParam(const char* name, double value);
double GetParam(const char* name);        // NaN, если параметра нет
```

Подтверждённые имена (через `GetParam` на реальном контроллере):

| Параметр | Смысл | Пример значения |
|---|---|---|
| `_ovrd_speedfeed` | override подачи (1.0 = 100%) | 1.6 |
| `_ovrd_spindle` | override шпинделя | 1.0 |
| `_ovrd_speedtraverse` | override быстрых перемещений | 1.0 |
| `_jog_speed` | скорость джога, мм/с (также множитель `Jog`) | 12 |
| `_jog_speeddef` | скорость джога по умолчанию | 12 |
| `_jog_step` | дистанция шагового джога, мм | 0.1 |
| `_jog_stepdef` | шаг по умолчанию | 0.1 |
| `_jog_round` | округление позиции при остановке джога, мм | 0.1 |
| `_jog_rounddef` | округление по умолчанию | 0.1 |
| `_motion_maxspeed` | макс. скорость, мм/с (ограничивает `Jog` cont.) | 60 |
| `_motion_maxacc` / `_motion_maxdec` | ускорение/замедление | 200 |
| `_speed_feed` / `_speed_traverse` | подача/быстрые, мм/мин | 1600 / 2000 |

> Устаревшие имена `SpeedFeedOverride` / `SpeedSpindleOverride` возвращают
> **NaN** — не являются валидными параметрами.

---

## 8. G-code

```c
bool StartCode(const char* gcode);   // выполнить строку G-code
bool OpenCode(const char* gcode);    // открыть файл/строку
```

Пример из `mpgd`: включение моторов перед джогом — `StartCode("M10 P1")`.

---

## 9. Информация

```c
double InfoSpeed();                       // текущая скорость подачи, мм/мин
double InfoSpindle();                     // скорость шпинделя, RPM
unsigned int InfoJogPot();                // положение потенциометра джога
bool InfoWorkPosition3(double* x, double* y, double* z);   // рабочая позиция
bool InfoMotorPosition3(double* x, double* y, double* z);  // моторная позиция
double InfoMotorPosition(int axis);       // позиция одной оси (0=X..8=W)
```

Индексы осей: `0=X, 1=Y, 2=Z, 3=A, 4=B, 5=C, 6=U, 7=V, 8=W`.

> `InfoWorkPosition3` возвращает **NaN**, пока рабочая позиция не установлена
> (хоминг/сброс нуля). `InfoMotorPosition`/`InfoMotorPosition3` читаются
> корректно.

---

## 10. Джог и перемещение

```c
bool Jog(bool step, double x, double y, double z);
bool Jog9(bool step, double x, double y, double z,
          double a, double b, double c, double u, double v, double w);
bool JogStop();
bool MoveAxis(double speed, int axis, double val);
```

### `Jog` — семантика (подтверждено на Mk3/4)

| Режим | Параметр | Поведение |
|---|---|---|
| **Step** (`step=true`) | `x/y/z` **игнорируется** | ось движется ровно на `_jog_step` (мм) в направлении знака; рывок на макс. ускорении |
| **Continuous** (`step=false`) | `x/y/z` = множитель скорости | факт. скорость мм/с = `value × _jog_speed`, ограничена `_motion_maxspeed` |

Измерено (`_jog_speed`=12, `_motion_maxspeed`=60):

- `Jog(false, 0.5)` → 6 мм/с (= 0.5 × 12)
- `Jog(false, 5)` → ~60 мм/с (упор в потолок)
- `Jog(false, 60)` → ~60 мм/с (упор в потолок)

> Джог/перемещение возвращают `ok`, но **не двигают ось** при выключенных
> моторах — сначала `StartCode("M10 P1")`.

### `MoveAxis` — точное позиционирование

Едет одной осью к **абсолютной** моторной позиции `val` плавно (чистые
разгон/торможение), **без** округления `_jog_round`.

- **Масштаб скорости: факт. мм/мин = `speed` / 10** (для N мм/мин передавать
  `N*10`). Проверено: `speed=1000` → ~100 мм/мин, `speed=20` → ~2 мм/мин.
- Приезжает точно, без округления (проверено на `185.19 мм`).
- Повторный вызов с новой целью **во время движения** иногда возвращает
  `false` — такой вызов надо повторить позже, а не сбрасывать накопленное
  смещение.

### `Jog9` — джог осей A/B/C

Расширение для 9 осей; в `mpgd` используется для A/B/C (индексы 3..8).
`JogStop()` останавливает джог.

---

## 11. Коллбэки (CDECL)

```c
typedef void (*InitialiseCB)(int);   // 1 = ready, 0 = shutdown
typedef void (*RefreshCB)();
typedef void (*IdleCB)();
typedef void (*LineNumCB)(int);
typedef void (*OpenCB)(int);
typedef void (*OutputCB)(const char*);

void SetInitialiseCB(InitialiseCB);
void SetRefreshCB(RefreshCB);
void SetIdleCB(IdleCB);
void SetLineNumCB(LineNumCB);
```

`InitialiseCB` обновляет process-wide `std::atomic<int>` (доступен через
`TngApi::initState()`).

---

## 12. Практические находки (см. `STAGE3_FINDINGS.md`)

- **Колесо (MPG)** реализовано как позиционное следование через `MoveAxis`
  (плавно, без рывков), а не через `Jog`.
- «Программа выполняется» = `!IsStop()`; `IsRun` для этого не годится (всегда
  `true`).
- Пульт LHB04 (KTURT) теряет импульсы энкодера при быстром вращении
  (~5 об/с); хост получает все отчёты — потеря внутри пульта.
- In-process запуск требует **прав администратора** (контроллер через libusbK;
  у `PlanetCNC64.exe` манифест `highestAvailable`).
- Мягкие лимиты активны (ход ограничен, напр. ~394.4 мм по X) — джог
  упирается и останавливается.
- `_jog_round` (0.1 мм) квантует позицию при остановке `Jog`; для шагов
  < 0.1 мм использовать `MoveAxis` либо снизить `_jog_round`.

---

## 13. Полный список экспортируемых символов

`Run`, `RunProfile`, `Exit`, `ExitForce`, `GetVer`, `IsRun`/`IsRunning`,
`IsRunningExt`, `IsInitialized`, `IsControllerReady`, `IsIdle`, `IsEStop`,
`IsStop`, `IsPause`, `EStop`, `EStopToggle`, `Stop`, `Pause`, `PauseToggle`,
`Start`, `GetCmdId`, `CmdExec`, `CmdExecStr`, `CmdExecVal`, `SetParam`,
`GetParam`, `StartCode`, `OpenCode`, `InfoSpeed`, `InfoSpindle`, `InfoJogPot`,
`InfoWorkPosition3`, `InfoMotorPosition3`, `InfoMotorPosition`, `Jog`, `Jog9`,
`JogStop`, `MoveAxis`, `SetInitialiseCB`, `SetRefreshCB`, `SetIdleCB`,
`SetLineNumCB`.

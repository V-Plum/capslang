# capslang

<img src="preview.png" width="128" align="right" alt="capslang icon">

Перемикання розкладки клавіатури по **CapsLock** для Windows 11.
**Shift + CapsLock** — звичайний Caps Lock.

*Keyboard layout switcher for Windows 11: tap CapsLock to switch layouts,
Shift+CapsLock for regular Caps Lock. Tray app, ~50 KB, no dependencies.*

## Механізм

- `RegisterHotKey(VK_CAPITAL, MOD_NOREPEAT)` — система проковтує CapsLock до того,
  як його побачить будь-який застосунок; реєстрація ексклюзивна і не зникає з
  часом (на відміну від low-level hook, який Windows тихо знімає за повільний
  колбек, а пізніше встановлені хуки перехоплюють клавішу першими).
- Перемикання: `WM_INPUTLANGCHANGEREQUEST` (через `PostMessage`, неблокуюче) у
  реально сфокусоване вікно (`GetGUIThreadInfo().hwndFocus` — коректно для UWP).
- Маніфест `requireAdministrator`: без нього UIPI блокує повідомлення у бік
  elevated-вікон (адмінський термінал, regedit) — Caps ковтався б, а розкладка
  не перемикалась. Наслідок: ручний запуск — через UAC-промпт.
- Автозапуск (чекбокс у вікні) = задача Task Scheduler **`capslang`**
  (`ONLOGON`, `RL HIGHEST`) — стартує elevated БЕЗ UAC-промпта. Run-ключ реєстру
  для elevated-програм не працює, тому саме задача.

## Використання

- `capslang.exe` — сидить у треї. Лівий клік по іконці або повторний запуск
  exe — вікно налаштувань (автозапуск on/off). Правий клік — меню з «Вихід».
- Іконка трею переживає перезапуск Explorer (обробка `TaskbarCreated`).
- Один екземпляр (named mutex `capslang_single_instance`).

## Збірка

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

Потрібно: Python + Pillow (генерація іконки, `make_icon.py`) і LLVM-MinGW
(`winget install MartinStorsjo.LLVM-MinGW.UCRT`). `build.ps1` сам знаходить
toolchain у winget-пакеті.

## Файли

| Файл | Що це |
|---|---|
| `capslang.cpp` | весь код утиліти |
| `capslang.manifest` | requireAdministrator + dpiAware |
| `capslang.rc` | іконка + маніфест у ресурси exe |
| `make_icon.py` | генератор `capslang.ico` (Q ⇄ Ї) |
| `build.ps1` | збірка (windres + clang++, статичний лінк) |
| `preview.png` | прев'ю іконки (генерується разом з .ico) |

## Відомі обмеження

- Якщо CapsLock уже зареєстрував хтось інший — при старті буде помилка
  (це навмисно: чесніше за мовчазний конфлікт двох хуків).
- Задача автозапуску тригериться на логон будь-якого користувача машини
  (стандартна поведінка `schtasks /SC ONLOGON`); на однокористувацькій
  машині неважливо.

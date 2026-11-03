# Heroes of Might and Magic IV for Apple Silicon Macs and iPad

> **This is a fork of [LeoDeadware/HoMM4Deadware](https://github.com/LeoDeadware/HoMM4Deadware)
> that adds an iPad build.** The Mac build and the original README below are that
> project's; this fork keeps them as they are and adds the iPad port and a few fixes.

## What this fork adds

- **iPad:** the same native port on an Apple-silicon iPad (tested on an iPad Pro 11"
  M4), with touch controls. See [iPad](#ipad) below.
- **Fixes, offered upstream as pull requests** (they apply to the Mac build too):
  - a town froze after a double click on an army in wide mode (the creature dialog
    was opened but never drawn);
  - the pointer lagged wherever windows were open (towns, combat dialogs): it is now
    drawn by the GPU instead of a repaint for every mouse move;
  - about 28% less CPU at rest (waits sleep once instead of a millisecond at a time);
  - `tools/setup.sh` installs `llvm@22`, which the runtime needs to configure.
- **File names with non-ASCII characters** (e.g. the autosaves of a Cyrillic
  translation) can be written on macOS/iPadOS (APFS takes only UTF-8 names).

Runtime fixes for the Mac come as separate patches, `patches/x86recomp-*.patch`,
applied by `tools/setup.sh` after `x86recomp-h4.patch`.

## iPad

What you need:

- everything the Mac build needs (see the original instructions below), done first:
  `tools/setup.sh` puts the game in `game/` and builds the Mac app;
- Xcode with the iOS platform (Xcode > Settings > Components), signed in with your
  Apple ID (Xcode > Settings > Apple Accounts) with an Apple Development certificate
  (Manage Certificates > + > Apple Development);
- an Apple-silicon iPad, connected by cable, trusted, with Developer Mode on
  (Settings > Privacy & Security > Developer Mode).

Then:

```
tools/ios/setup.sh                                 # the iOS runtime, its lifter, the app
python3 -I tools/ios/install_device.py --game      # sign, install, copy the game once
```

The first time, trust your developer certificate on the iPad (Settings > General >
VPN & Device Management). A free Apple ID signs for 7 days: run
`python3 -I tools/ios/install_device.py` again to renew (saves are kept). Later builds:
`tools/ios/build_app.sh device` and `install_device.py` without `--game`.

The iPad build runs on another x86recomp: the one
[macnativegames](https://github.com/ly05010419/macnativegames) carries for its
Red Alert 2 port (x86recomp with WinNativeGames' iOS support).
`tools/ios/fetch_runtime.sh` takes it from there at a pinned commit and applies
`patches/ios/x86recomp-ios.patch` (this port's runtime patch merged onto it, and the
iPad fixes).

**Touch controls:** tap = left click; hold still for half a second = right click (the
game shows its help, which the next tap closes); drag = drag with the left button
(moves armies, scrolls the map); two-finger tap = right click. A trackpad or mouse
works as on the Mac.

**Good to know:**

- The iPad gets `game/` as it is: the original English game, or whatever you put
  there. The Ukrainian translation of the Mac app is not applied to the iPad copy.
- Saves, settings and the log (`h4-log.txt`) are in the app's Documents (Files app).
- The iOS Simulator cannot run it (its Metal lacks what SDL_GPU needs): use a device.

## Credits

[LeoDeadware/HoMM4Deadware](https://github.com/LeoDeadware/HoMM4Deadware) (the port),
[x86recomp](https://github.com/JTM-rootstorm/x86recomp) (the recompiler and runtime),
[macnativegames](https://github.com/ly05010419/macnativegames) (x86recomp with iOS
support, from its Red Alert 2 port). All MIT, as is this fork.

---

*The original README follows.*


## Heroes of Might and Magic IV for Apple Silicon Macs

Native macOS port of Heroes of Might and Magic IV, in Ukrainian. Pick your language below.<br>
Нативний порт «Героїв меча і магії IV» для macOS, українською. Оберіть мову нижче.

<details>
<summary><b>🇺🇦 Українською</b></summary>

«Герої меча і магії IV» з обома доповненнями, українською, як звичайна програма
для Мака. Без Wine, емуляторів і віртуальних машин: гра працює на Маці напряму.

- Українська мова: інтерфейс, кампанії, озвучка кампаній.
- Широкий екран і чітка картинка на Retina-дисплеях.
- Плавна анімація на карті й у бою.

**Що потрібно:**

- Мак з процесором Apple (M1, M2, M3, M4 або новіший) і macOS 13 або новішою.
  Перевірити: меню  → «Про цей Mac», рядок «Чип».
- Куплена гра **Heroes of Might and Magic IV: Complete Edition** на
  [GOG](https://www.gog.com/game/heroes_of_might_and_magic_4_complete).
  Самої гри тут немає, лише те, що робить її рідною для Мака.
- Інтернет і близько 5 ГБ вільного місця.
- Приблизно 40 хвилин, з них самої роботи — хвилин 10. Решту часу Мак усе робить
  сам.

---

## Встановлення крок за кроком

### Крок 1. Завантажте гру з GOG

1. Зайдіть на [gog.com](https://www.gog.com) у свій акаунт і відкрийте
   **«Бібліотека» → Heroes of Might and Magic IV: Complete**.
2. Знайдіть розділ із **файлами для завантаження** (Offline backup game
   installers) і завантажте інсталятор **для Windows**, англійською мовою.
   Його назва починається з `setup_heroes_of_might_and_magic_4_complete`.
   Якщо поруч пропонують ще файл `.bin` — завантажте й його.
3. Не відкривайте ці файли. Нехай просто лежать у теці **«Завантаження»**.

### Крок 2. Завантажте цей порт

1. Натисніть зелену кнопку **Code** вгорі цієї сторінки, потім **Download ZIP**.
2. Відкрийте [сторінку Releases](https://github.com/LeoDeadware/HoMM4Deadware/releases)
   і завантажте файл **heroes4-mac-assets.zip** (арт і українська озвучка).
3. У теці «Завантаження» мають з'явитися теки **HoMM4Deadware-main** та
   **heroes4-mac-assets**. Якщо замість тек там лежать `.zip`-файли, клацніть
   двічі по кожному: Мак сам їх розпакує.

Нічого не перейменовуйте й не переносьте: програма встановлення шукає файли саме
в «Завантаженнях».

### Крок 3. Відкрийте Термінал

Термінал — це вбудована програма Мака, у яку вставляють команди.

Натисніть **Cmd ⌘ + пробіл**, надрукуйте `Термінал` (або `Terminal`) і натисніть
**Enter**. Відкриється вікно з текстом. Нічого страшного, ми лише вставимо туди
дві команди.

### Крок 4. Встановіть Homebrew

Homebrew — безкоштовний помічник, який ставить на Мак потрібні гри інструменти.
Якщо він у вас уже є, переходьте до кроку 5.

1. Скопіюйте цей рядок повністю (**Cmd ⌘ + C**):

   ```
   /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
   ```

2. Клацніть у вікно Термінала, вставте рядок (**Cmd ⌘ + V**) і натисніть **Enter**.
3. Термінал попросить **пароль від вашого Мака** (той, яким ви входите в систему).
   Надрукуйте його й натисніть **Enter**. **Під час друку пароля нічого не видно,
   навіть зірочок.** Так і має бути: пароль вводиться, просто не показується.
4. Коли попросить, натисніть **Enter** ще раз.
5. Якщо з'явиться вікно з пропозицією встановити «інструменти командного рядка»
   (Command Line Tools), натисніть **«Встановити»** і погодьтеся з умовами.
6. Зачекайте 5–15 хвилин, поки не з'явиться напис **Installation successful!**
   Наприкінці Homebrew покаже «Next steps» з командами. Їх можна пропустити:
   наша програма встановлення все налаштує сама.

### Крок 5. Встановіть гру

1. Скопіюйте цей рядок:

   ```
   bash ~/Downloads/HoMM4Deadware-main/tools/setup.sh
   ```

2. Вставте його в Термінал і натисніть **Enter**.
3. Якщо Мак спитає, чи дозволити Терміналу доступ до теки «Завантаження»,
   натисніть **«Дозволити»**.
4. Тепер просто чекайте **15–30 хвилин** і не закривайте Термінал. Текст буде
   бігти, вентилятор може шуміти — це Мак збирає гру під себе. Мак у цей час
   можна використовувати, але краще не запускати важких програм.
5. Готово, коли в кінці з'явиться рядок, що починається зі слова **встановлено**.

### Крок 6. Грайте

На Робочому столі з'явиться ярлик **Heroes IV**. Клацніть по ньому двічі. Сама
гра лежить у теці «Програми» вашої домашньої теки, тож її можна перетягнути
й на Dock.

Після встановлення теки з «Завантажень» (HoMM4Deadware-main, heroes4-mac-assets
та інсталятор GOG) можна видалити. Гра працюватиме без них.

---

## Якщо щось пішло не так

**Термінал пише «помилка: …».** Програма встановлення пояснює, що не так. Найчастіше:

- **«не знайшов інсталятор гри з GOG»** — інсталятора немає в «Завантаженнях»
  або завантажено не той (потрібен для Windows, Complete Edition). Поверніться
  до кроку 1.
- **«не з GOG Complete 3.0»** — у вас інша версія гри. Порт працює тільки
  з актуальною версією з GOG.
- **«спершу встановіть Homebrew»** — повторіть крок 4.
- **«встановіть Command Line Tools»** — у вікні, що відкрилося, натисніть
  «Встановити». Коли встановлення закінчиться, повторіть крок 5.

**Термінал пише «command not found» або «No such file or directory».** Перевірте,
що тека в «Завантаженнях» називається саме **HoMM4Deadware-main** (крок 2).

**Встановлення перервалося** (закрили Термінал, вимкнувся Мак). Просто повторіть
крок 5: усе, що вже зроблено, вдруге не робитиметься.

**Гра не відкривається.** Клацніть по ярлику правою кнопкою миші й виберіть
**«Відкрити»**, потім ще раз **«Відкрити»**.

**Гра вилетіла або поводиться дивно.** Створіть
[issue](https://github.com/LeoDeadware/HoMM4Deadware/issues) і додайте до нього
два файли:

- `h4.log`. У Finder натисніть **Cmd ⌘ + Shift + G**, вставте `~/Library/Logs`
  і натисніть **Enter**.
- Найновіший файл з теки `~/Library/Application Support/h4/crashes` (так само,
  через **Cmd ⌘ + Shift + G**).

## Керування

Як в оригінальній грі. Додатково: **F10** перемикає фільтр згладжування картинки.

---

## Для тих, кому цікаво, як це зроблено

Порт зроблено статичною рекомпіляцією за допомогою
[x86recomp](https://github.com/JTM-rootstorm/x86recomp): код `heroes4.exe` з
вашої копії гри заздалегідь перекладається в нативний код для Apple Silicon, а
графіка, звук і вікно працюють через SDL3 і Metal. Тому в репозиторії немає ні
коду, ні файлів гри: усе це збирається на вашому Маці з вашої копії.

| Шлях | Що |
|---|---|
| `h4/` | порт: `lifter.manifest`, замінені функції гри (`src/`) |
| `patches/` | зміни до x86recomp, що накладаються на зафіксований коміт |
| `symbols/` | адреси функцій і таблиць переходів `heroes4.exe` (з Ghidra) |
| `loc/` | український переклад (прив'язаний до хешів англійського тексту) та інструменти |
| `tools/` | встановлення (`setup.sh`), збирання додатка, інструменти для арту, скрипти Ghidra |

Інсталятор GOG можна передати й явно: `tools/setup.sh <шлях до setup_….exe>`.

## Ліцензія

Власний код порту та інструменти — MIT (див. `LICENSE`). x86recomp — MIT від
його авторів. На саму гру ліцензія не поширюється: вона ваша, з GOG.

Heroes of Might and Magic — торгова марка Ubisoft. Це фанатський проєкт, не
пов'язаний з Ubisoft чи GOG.

</details>

<details>
<summary><b>🇬🇧 English</b></summary>

Heroes of Might and Magic IV with both expansions as a regular Mac app. No Wine,
no emulators, no virtual machines: the game runs on the Mac directly.

- **The game is in Ukrainian:** interface, campaigns, campaign voice-over. There
  is no English option.
- Wide screen and a sharp picture on Retina displays.
- Smooth animation on the adventure map and in combat.

**What you need:**

- A Mac with an Apple chip (M1, M2, M3, M4 or newer) running macOS 13 or later.
  To check: menu  → **About This Mac**, the **Chip** line.
- A purchased copy of **Heroes of Might and Magic IV: Complete Edition** from
  [GOG](https://www.gog.com/game/heroes_of_might_and_magic_4_complete).
  The game itself is not here, only what makes it run natively on a Mac.
- An internet connection and about 5 GB of free space.
- About 40 minutes, of which about 10 are actual work. The Mac does the rest
  on its own.

---

## Step-by-step installation

### Step 1. Download the game from GOG

1. Sign in at [gog.com](https://www.gog.com) and open
   **Library → Heroes of Might and Magic IV: Complete**.
2. Find the **Offline backup game installers** section and download the
   installer **for Windows**, in English. Its name starts with
   `setup_heroes_of_might_and_magic_4_complete`. If a `.bin` file is offered
   next to it, download that too.
3. Do not open these files. Just leave them in your **Downloads** folder.

### Step 2. Download this port

1. Click the green **Code** button at the top of this page, then **Download ZIP**.
2. Open the [Releases page](https://github.com/LeoDeadware/HoMM4Deadware/releases)
   and download **heroes4-mac-assets.zip** (the art and the Ukrainian voice-over).
3. The **HoMM4Deadware-main** and **heroes4-mac-assets** folders should appear
   in Downloads. If you see `.zip` files instead of folders, double-click each
   one and the Mac unpacks it.

Do not rename or move anything: the installer looks for the files in Downloads.

### Step 3. Open Terminal

Terminal is a built-in Mac app where you paste commands.

Press **Cmd ⌘ + Space**, type `Terminal` and press **Enter**. A window with
text opens. Nothing to worry about: you only paste two commands into it.

### Step 4. Install Homebrew

Homebrew is a free helper that installs the tools the game needs. If you
already have it, go to step 5.

1. Copy this whole line (**Cmd ⌘ + C**):

   ```
   /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
   ```

2. Click into the Terminal window, paste the line (**Cmd ⌘ + V**) and press **Enter**.
3. Terminal asks for **your Mac's password** (the one you log in with). Type it
   and press **Enter**. **Nothing appears while you type, not even asterisks.**
   That is normal: the password is being entered, just not shown.
4. When asked, press **Enter** once more.
5. If a window offers to install the **Command Line Tools**, click
   **Install** and accept the terms.
6. Wait 5–15 minutes until you see **Installation successful!** At the end
   Homebrew shows "Next steps" with some commands. You can skip them: our
   installer sets everything up itself.

### Step 5. Install the game

1. Copy this line:

   ```
   bash ~/Downloads/HoMM4Deadware-main/tools/setup.sh
   ```

2. Paste it into Terminal and press **Enter**.
3. If the Mac asks whether Terminal may access the Downloads folder, click
   **Allow**.
4. Now just wait **15–30 minutes** and do not close Terminal. Text scrolls by and
   the fan may get loud: the Mac is building the game for itself. You can use
   the Mac meanwhile, but better not run anything heavy.
5. It is done when the last line starts with the word **встановлено**
   ("installed").

### Step 6. Play

A **Heroes IV** shortcut appears on the Desktop. Double-click it. The game itself
is in the Applications folder inside your home folder, so you can drag it to the
Dock as well.

After installing you can delete the folders in Downloads (HoMM4Deadware-main,
heroes4-mac-assets and the GOG installer). The game works without them.

---

## If something goes wrong

**Terminal says «помилка: …» ("error").** The installer explains what is wrong
(in Ukrainian). The most common ones:

- **«не знайшов інсталятор гри з GOG»** (GOG installer not found): the installer
  is not in Downloads, or it is the wrong one (you need the Windows one,
  Complete Edition). Go back to step 1.
- **«не з GOG Complete 3.0»** (not GOG Complete 3.0): you have a different
  version of the game. The port works only with the current GOG version.
- **«спершу встановіть Homebrew»** (install Homebrew first): repeat step 4.
- **«встановіть Command Line Tools»** (install the Command Line Tools): click
  **Install** in the window that opened. When it finishes, repeat step 5.

**Terminal says "command not found" or "No such file or directory".** Check that
the folder in Downloads is named exactly **HoMM4Deadware-main** (step 2).

**The installation was interrupted** (Terminal closed, the Mac shut down). Just
repeat step 5: whatever is already done is not done again.

**The game does not open.** Right-click the shortcut, choose **Open**, then
**Open** again.

**The game crashed or behaves oddly.** Open an
[issue](https://github.com/LeoDeadware/HoMM4Deadware/issues) and attach two files:

- `h4.log`. In Finder press **Cmd ⌘ + Shift + G**, paste `~/Library/Logs` and
  press **Enter**.
- The newest file in `~/Library/Application Support/h4/crashes` (the same way,
  with **Cmd ⌘ + Shift + G**).

## Controls

As in the original game. In addition, **F10** switches the picture smoothing filter.

---

## How it is made

The port is made by static recompilation with
[x86recomp](https://github.com/JTM-rootstorm/x86recomp): the code of `heroes4.exe`
from your copy of the game is translated ahead of time into native Apple
Silicon code, while graphics, sound and the window run on SDL3 and Metal. That
is why the repository holds no game code or files: all of it is built on your
Mac from your copy.

| Path | What |
|---|---|
| `h4/` | the port: `lifter.manifest`, replaced game functions (`src/`) |
| `patches/` | changes to x86recomp, applied on a pinned commit |
| `symbols/` | function and jump-table addresses of `heroes4.exe` (from Ghidra) |
| `loc/` | the Ukrainian translation (keyed by hashes of the English text) and its tools |
| `tools/` | setup (`setup.sh`), the app bundle, art tools, Ghidra scripts |

The GOG installer can also be given explicitly: `tools/setup.sh <path to setup_….exe>`.

## License

The port's own code and tools: MIT (see `LICENSE`). x86recomp: MIT, by its
authors. The license does not cover the game itself: it stays yours, from GOG.

Heroes of Might and Magic is a trademark of Ubisoft. This is a fan project,
unaffiliated with Ubisoft or GOG.

</details>

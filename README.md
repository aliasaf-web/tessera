# Tessera

Tessera is a clean, modern, easy desktop for Debian 13. It has its own window
manager, bar, app launcher, quick settings, notifications and lock screen.

- **Windows arrange themselves.** The newest window gets the big space on the
  left; the others stack neatly on the right. Nothing overlaps unless you want it to.
- **One button for apps.** Tap the **Super** key (the Windows key) or click the
  grid button at the bottom left. Then start typing.
- **Quick settings in one place.** Click the clock at the bottom right for
  Wi-Fi, Bluetooth, sound, brightness, dark mode, night light, battery and power.
- **Touchpad swipes.** Swipe left or right with three fingers to change workspace,
  up to see your apps.
- **A simple Settings app** for colours, background, screens (size,
  resolution, a second screen or projector), night light, touchpad, keyboard,
  sound, screen lock and default apps.
- **The whole screen for one window.** Gaps separate windows from each other,
  so a window on its own fills the screen and the gap comes back with a second
  one. Set `gaps=N` in the config to have exactly N pixels either way.
- **Light on your laptop.** Small screens get a compact layout automatically,
  and it only needs a small amount of memory.

---

## Part 1: Install Debian 13 (about 30–60 minutes)

> ⚠️ **Installing Debian erases everything on the laptop.** Copy your photos and
> documents to a USB stick or cloud storage first.

You need: a USB stick (2 GB or bigger, which will be wiped), your Wi-Fi name and
password, and the laptop plugged into its charger.

1. **Download Debian.** On any computer, go to **debian.org**, click **Download**,
   and get the file that ends in **`amd64-netinst.iso`**.
2. **Put it on the USB stick.** On Windows, download **Rufus** from **rufus.ie**,
   open it, pick your USB stick, click **SELECT** and choose the Debian file, then
   click **START**. If it asks how to write the image, choose **DD Image mode**.
3. **Start the laptop from the USB stick.** Plug the stick into the IdeaPad, turn
   it on and keep tapping **F12** (on some models **Fn + F12**) until a menu
   appears. Choose the USB stick. If F12 doesn't work, switch the laptop off and
   press the tiny **Novo** button (a pinhole near the power socket) with a paper
   clip, then choose **Boot Menu**.
4. **In the Debian installer**, choose **Graphical install**, then:
   - Language **English**, country **United Kingdom**, keyboard **British English**.
   - Connect to your **Wi-Fi** when asked.
   - Hostname: anything, for example `ideapad`. Domain: leave empty.
   - **Root password: leave it EMPTY** (press Continue twice). This lets your
     own account do admin tasks with `sudo`, which the Tessera installer needs.
   - Type your full name, a username and a password you will remember.
   - Partitioning: **Guided – use entire disk**, then your disk, then
     **All files in one partition**, then **Finish** and **Yes**.
   - On **Software selection**, **untick** "Debian desktop environment" and
     "GNOME". Keep **standard system utilities** ticked. Click Continue.
   - Say **Yes** to installing the GRUB boot loader on your disk.
5. Remove the USB stick when it asks, and let the laptop restart.

You will see a black text screen asking for your **login**. Type your username,
press Enter, then your password (nothing shows as you type, which is normal) and Enter.

## Part 2: Install Tessera (about 10–20 minutes)

1. **Get Tessera onto the laptop.**
   - Type these two lines (press Enter after each):
     ```
     sudo apt install git
     git clone https://github.com/aliasaf-web/tessera
     ```
   - Or, if you have it on a USB stick instead, plug it in and type:
     ```
     sudo apt install udisks2
     udisksctl mount -b /dev/sdb1
     cp /media/$USER/*/tessera.tar.gz ~ && tar xf ~/tessera.tar.gz -C ~
     ```
     (If `/dev/sdb1` doesn't work, try `/dev/sdc1`.)
2. **Run the installer:**
   ```
   cd ~/tessera
   sh install.sh
   ```
   Type your password when asked. When it asks
   *"Use the Tessera lock screen as your login screen?"* press **Enter** for yes.
3. When it says **All done!**, restart:
   ```
   sudo reboot
   ```

After the restart you'll see the Tessera lock screen. Type your password and
press Enter. A welcome screen explains the basics.

If you log out (or if the desktop ever stops), a simple text login appears
instead: type your user name, press Enter, then your password. Restarting the
laptop brings back the lock screen.

---

## Everyday use

To open an app, tap **Super** (the key with the Windows logo), type its name
and press **Enter**. New windows open in the big space on the left; the others
line up on the right. Drag a window's title bar onto another window to swap
them, and drag the gap between the columns to make one side wider.

<!-- Shortcuts: made from shell/tessera_shell/shortcuts.py by tools/update-readme.py -->

**Getting around**

| Keys | What it does |
|---|---|
| **Super** or **Super + A** | Open the app launcher (tap Super on its own) |
| **Super + Enter** | Open a terminal |
| **Super + E** | Open your files |
| **Super + B** | Open the web browser |
| **Super + L** | Lock the screen |
| **Super + Esc** or **Super + Shift + E** | Sleep, restart or shut down |

**Windows**

| Keys | What it does |
|---|---|
| **Alt + Tab** or **Super + Tab** | Switch windows: hold Alt or Super, tap Tab to choose, let go (add Shift to go back) |
| **Super + Q** or **Super + W** or **Alt + F4** | Close the window |
| **Super + ← → ↑ ↓** | Move between windows |
| **Super + Shift + ← → ↑ ↓** | Swap windows around |
| **Super + F** | Full screen on/off |
| **Super + M** or **double-click the title bar** | Make a window big on/off |
| **Super + Space** | Let a window float freely on/off |
| **Super + left mouse drag** | Move a floating window, or swap a window with another |
| **Super + right mouse drag** | Resize a floating window |

**Workspaces**

| Keys | What it does |
|---|---|
| **3 fingers ← →** | Swipe on the touchpad to change workspace |
| **3 fingers ↑ ↓** | Swipe up to show your apps, down to hide them |
| **Super + Ctrl + ← →** | Previous or next workspace |
| **Super + 1 … 9** | Go to workspace 1 to 9 |
| **Super + Shift + 1 … 9** | Move the window to that workspace |

**More**

| Keys | What it does |
|---|---|
| **Print Screen** | Take a screenshot of the whole screen |
| **Shift + Print Screen** or **Super + Shift + S** | Take a screenshot of an area |
| **Super + Shift + R** | Apply changes made by hand to the settings file |
| **Super + /** | Show this help again |

<!-- End of shortcuts -->

Wi-Fi, Bluetooth, volume, brightness, dark mode, night light, **Sleep**,
**Restart** and **Shut down** are all in quick settings (click the clock).

To add a Bluetooth device, click the arrow next to Bluetooth, then **Look for new
devices**, and click the device's name. If pairing needs a code, quick settings
shows it there: check that it matches, type it on the device, or type the code
the device shows.

The dots at the bottom left show your workspaces: the long one is where you are.
Click a dot, or scroll over them, to move between workspaces. A dot in your
highlight colour means a window there needs you (for example, it opened a
question while you were somewhere else).

With a second screen, each screen has its own workspaces, and the dots on each
screen's bar are for that screen. **Super + 1 … 9** and new windows go to the
screen you are using: the one with the active window, or the one whose
workspace dots or desktop you clicked last.

Touch screens work too: apps that understand touch get your fingers, and
anywhere else (title bars, the desktop, the lock screen) a tap is a click. A
drawing tablet's pen moves the pointer and clicks; pen pressure isn't used yet.

A video playing in a window you can see keeps the screen on. Once the window
is hidden (on another workspace, or behind another window) the screen locks
and turns off as usual.

The lock screen has **Sleep**, **Restart** and **Shut down** buttons at the
bottom (Restart and Shut down ask you to click twice, because open apps close).
The power button puts the laptop to sleep from there. If Caps Lock is on, the
lock screen tells you. A machine without a keyboard can still unlock: an
on-screen keyboard or a switch device can type the password there, and a wrong
one from either waits exactly as long as a wrong one from a keyboard.

While the screen is locked the desktop behind it isn't drawn at all, so a
program watching the screen sees only the lock screen; nothing that starts while
you're away can ask to watch the screen, read the clipboard or change the
monitors, until the screen opens again; the control socket (`tessera msg`) can be
read but not used to start or move anything; and no program can move, resize or
switch off a monitor from behind the lock. It is a lock for a coffee break, not a
vault: a program you started *before* locking keeps whatever it had already
picked up, and your files are as readable to your own account as they always
were. Close an app you don't trust rather than locking over it.

Screenshots are saved in **Pictures › Screenshots** and copied, so you can paste them straight away.

## Settings

Click the clock, then the **gear** button at the top of quick settings. (Or tap
**Super** and type *settings*.) Changes apply straight away.

| Page | What you can change |
|---|---|
| Appearance | Dark or light, highlight colour, background (plain, mosaic or your own picture), compact or comfortable layout, animations |
| Displays | For each screen: on or off, resolution, size of everything (scale), and where a second screen sits (left, right, above, below, or showing the same picture). Changes come back after 15 seconds unless you press **Keep**, in case a screen can't show them. Also night light and its times and warmth |
| Touchpad & mouse | Tap to click, natural scrolling, pointer speed, three-finger swipes |
| Keyboard | Keyboard layout, list of shortcuts |
| Sound | Which speakers and microphone to use |
| Power & lock | When the screen locks and turns off |
| Default apps | Web browser, files app and terminal |
| About | Your computer's details, which build is installed, and **Update Tessera** |

Your choices are saved in `~/.config/tessera/config`. You can also edit that file
by hand (any line from `/etc/tessera/config` can go in it, and the format is
explained at its top); press **Super + Shift + R** afterwards to apply it.

Screen tools made for wlroots desktops, such as `wlr-randr` and `kanshi`, work
too; their changes last until the settings are applied again. Apps in a
sandbox (such as Flatpaks) can't change the screens. Games and 3D
apps can hold the mouse pointer in place or keep it inside their window while
they have the keyboard.

## Updating

Open **Settings › About** and click **Update**, or open a terminal
(**Super + Enter**) and type:

```
tessera-update
```

Type your password when asked. It downloads the newest version, installs it and
then offers to restart. Your settings are kept, and apps you removed (or things
you turned off, such as Bluetooth) stay that way.

When it finishes it names the commit it installed, and **Settings › About**
shows the same under **This build**: the commit, the branch it came from, the
day it went in, and whether the folder held changes of your own. That's how to
tell whether an update actually brought something new, instead of taking
"Already up to date." on trust.

> **Updating from the first version (1.0):** it doesn't have `tessera-update`
> yet, so the first time type this instead:
> ```
> cd ~/tessera && git pull && sh install.sh --update
> ```

## If something goes wrong

- **The screen is stuck or the desktop won't start:** press **Ctrl + Alt + F2**
  for a text screen, log in, and look at `~/.local/state/tessera/tessera.log`.
  **Ctrl + Alt + F7** goes back to the desktop.
- **The bar disappeared:** it restarts by itself within a few seconds. Its log is
  `~/.local/state/tessera/shell.log`. Both logs are kept to the last few hundred
  kilobytes of the previous run, so a month of complaining can't fill the disk.
- **An update said nothing was new, and the version stayed the same:** the Tessera
  folder is probably not on the branch that has the newer version. Look with
  `cd ~/tessera && git status -sb` (the first word after `##` is the branch);
  `git checkout main && git pull` and run `tessera-update` again. If the folder has
  changes of your own in it, ask whoever made them before throwing anything away.
- **Wi-Fi doesn't connect after installing:** click the clock, then the arrow next
  to Wi-Fi, choose your network and type the password again.
- **Remove Tessera:** `cd ~/tessera && sh uninstall.sh`

---

## How it is built

| Part | What it does | Made with |
|---|---|---|
| `compositor/` | Window manager: draws windows, tiling, title bars, rounded corners, workspaces and their animation, window switcher, keyboard, touchpad and gestures, touch screens and drawing tablets, night light, background, lock screen, screen locking before sleep | C with wlroots 0.18 (Debian's version) |
| `shell/` | Bar, app launcher, quick settings, notifications, volume/brightness pop-up, window switcher, welcome screen, Settings app | Python with GTK 4, gtk4-layer-shell and libadwaita |
| `data/` | Session start-up scripts, update command, default settings, password check (PAM), login screen and screen-sharing setup, package lists | Shell scripts and config files |

Tessera uses Debian's standard services rather than replacing them:
greetd (login screen), NetworkManager (Wi-Fi), PipeWire (sound), BlueZ
(Bluetooth), UPower (battery), systemd-logind (sleep, brightness, power), and
Xwayland for older X11 apps.

What it installs is listed in two small packages that the installer makes:
`tessera-desktop` and `tessera-build-deps` (see `data/packages`). Apps that
come with Tessera are "recommended", so you can remove any of them with
`sudo apt remove NAME` and updates will leave it removed.

The control command `tessera msg` can switch workspaces, lock the screen and more;
run it without arguments to see the list.

### Checks

Every change on GitHub is built on Debian 13 with warnings treated as errors,
and tested (see `.github/workflows/checks.yml`). To run the same checks on your
computer, from the `tessera` folder:

```sh
meson setup build compositor && ninja -C build  # build the window manager
meson test -C build --print-errorlogs           # buffers, the config file, idle
python3 -m unittest discover -s tests           # the shell, and the desktop
flake8 shell tools tests                        # Python style
ruff check .                                    # the same Python, linted
cppcheck --quiet --inline-suppr --std=c11 --language=c \
  --enable=warning --error-exitcode=1 \
  --suppress=uninitvar --suppress=memleak -DTESSERA_VERSION='"'$(cat VERSION)'"' \
  -Icompositor/src -Ibuild $(pkg-config --cflags-only-I wlroots-0.18) \
  compositor/src/*.c                           # the desktop's C
```

`ruff.toml` at the top of the tree holds the rules ruff keeps. flake8 is still run
as well, because the shell's `# noqa` comments are written for it and it is what
most people already have. `cppcheck` needs wlroots' headers, and the generated
protocol headers from the build, so it says something about this code only after
the two lines above have run.

Most of what `python3 -m unittest` runs needs only Python. Two files go
further: `tests/test_contract.py` reads the C sources and the Python sources
and checks the words they exchange still match (key actions, event names, the
commands `tessera msg` accepts), and `tests/test_compositor.py` starts the
window manager you just built on a headless backend - no screen, no graphics
driver - and drives it over its control socket the way the bar does. It skips
itself when nothing has been built, and `TESSERA=/path/to/tessera` points it
at a different build. It can also put a window *inside* that desktop:
`compositor/tests/window_client.c` is built beside `tessera` as
`tessera-test-window`, mapping one window that prints a line per event, and
`--globals` listing what the desktop advertises to whoever asks. That is how
tiling a real window, closing one from the socket, and what the lock screen
hides from a program started behind it get checked rather than only read. The sanitiser job runs the first two again with
AddressSanitizer and UndefinedBehaviorSanitizer turned on.

A red run does not need the Actions log to be understood: `tools/ci-annotate.py`
copies each failed test's report onto the check itself, where the checks tab and
`gh api .../check-runs/ID/annotations` both show it.

The keyboard shortcuts are listed once, in `shell/tessera_shell/shortcuts.py`.
After changing them, run `python3 tools/update-readme.py` to update the tables
above. The version number is in the `VERSION` file.

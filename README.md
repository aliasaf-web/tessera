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
- **A simple Settings app** for colours, background, screen size, night light,
  touchpad, keyboard, sound, screen lock and default apps.
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

| To do this | Press |
|---|---|
| Open an app | Tap **Super**, type its name, press **Enter** |
| Open a terminal / files / web browser | **Super + Enter** / **Super + E** / **Super + B** |
| Close a window | **Super + Q**, or click the **×** on its title bar |
| Switch to another window | Hold **Alt**, tap **Tab** until it's highlighted, let go |
| Move between windows | **Super + arrow keys**, or click a window |
| Swap windows around | **Super + Shift + arrow keys**, or drag a title bar onto another window |
| Make one window big / full screen | **Super + M** / **Super + F** |
| Let a window float freely | **Super + Space** (then drag it with **Super + left mouse**) |
| Make the left column wider | Drag the gap between the columns |
| Change workspace (separate desktops) | Swipe left/right with **3 fingers**, **Super + Ctrl + ← / →**, or **Super + 1 … 9** |
| Move a window to another workspace | **Super + Shift + 1 … 9** |
| See your apps / hide them | Swipe up / down with **3 fingers** |
| Screenshot (whole screen / an area) | **Print Screen** / **Shift + Print Screen** |
| Lock the screen | **Super + L** |
| Show all shortcuts | **Super + /** |

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

The lock screen has **Sleep**, **Restart** and **Shut down** buttons at the
bottom (Restart and Shut down ask you to click twice, because open apps close).
The power button puts the laptop to sleep from there. If Caps Lock is on, the
lock screen tells you.

Screenshots are saved in **Pictures › Screenshots** and copied, so you can paste them straight away.

## Settings

Click the clock, then the **gear** button at the top of quick settings. (Or tap
**Super** and type *settings*.) Changes apply straight away.

| Page | What you can change |
|---|---|
| Appearance | Dark or light, highlight colour, background (plain, mosaic or your own picture), compact or comfortable layout, animations |
| Display | Size of everything (scale), night light and its times and warmth |
| Touchpad & mouse | Tap to click, natural scrolling, pointer speed, three-finger swipes |
| Keyboard | Keyboard layout, list of shortcuts |
| Sound | Which speakers and microphone to use |
| Power & lock | When the screen locks and turns off |
| Default apps | Web browser, files app and terminal |
| About | Your computer's details and **Update Tessera** |

Your choices are saved in `~/.config/tessera/config`. You can also edit that file
by hand (any line from `/etc/tessera/config` can go in it); press
**Super + Shift + R** afterwards to apply it.

## Updating

Open **Settings › About** and click **Update**, or open a terminal
(**Super + Enter**) and type:

```
tessera-update
```

Type your password when asked. It downloads the newest version, installs it and
then offers to restart. Your settings are kept, and apps you removed (or things
you turned off, such as Bluetooth) stay that way.

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
  `~/.local/state/tessera/shell.log`.
- **Wi-Fi doesn't connect after installing:** click the clock, then the arrow next
  to Wi-Fi, choose your network and type the password again.
- **Remove Tessera:** `cd ~/tessera && sh uninstall.sh`

---

## How it is built

| Part | What it does | Made with |
|---|---|---|
| `compositor/` | Window manager: draws windows, tiling, title bars, rounded corners, workspaces and their animation, window switcher, keyboard, touchpad and gestures, night light, background, lock screen, screen locking before sleep | C with wlroots 0.18 (Debian's version) |
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

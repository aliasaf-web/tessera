# Tessera

Tessera is a clean, modern, easy desktop for Debian 13. It has its own window
manager, bar, app launcher, quick settings, notifications and lock screen.

- **Windows arrange themselves.** The newest window gets the big space on the
  left; the others stack neatly on the right. Nothing overlaps unless you want it to.
- **One button for apps.** Tap the **Super** key (the Windows key) or click the
  grid button at the bottom left. Then start typing.
- **Quick settings in one place.** Click the clock at the bottom right for
  Wi-Fi, Bluetooth, sound, brightness, dark mode, battery and power.
- **Light on your laptop.** It uses no extra effects and only a small amount of memory.

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
   - If Tessera is on GitHub, type the following (replace the address with yours):
     ```
     sudo apt install git
     git clone https://github.com/YOUR-NAME/tessera
     ```
   - If you have it on a USB stick, plug it in and type:
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

---

## Everyday use

| To do this | Press |
|---|---|
| Open an app | Tap **Super**, type its name, press **Enter** |
| Open a terminal / files / web browser | **Super + Enter** / **Super + E** / **Super + B** |
| Close a window | **Super + Q**, or click the **×** on its title bar |
| Move between windows | **Super + arrow keys**, or click a window |
| Swap windows around | **Super + Shift + arrow keys**, or drag a title bar onto another window |
| Make one window big / full screen | **Super + M** / **Super + F** |
| Let a window float freely | **Super + Space** (then drag it with **Super + left mouse**) |
| Make the left column wider | Drag the gap between the columns |
| Workspaces (separate desktops) | **Super + 1 … 9**; move a window with **Super + Shift + 1 … 9** |
| Screenshot (whole screen / an area) | **Print Screen** / **Shift + Print Screen** |
| Lock the screen | **Super + L** |
| Show all shortcuts | **Super + /** |

Wi-Fi, Bluetooth, volume, brightness, dark mode, **Sleep**, **Restart** and
**Shut down** are all in quick settings (click the clock).

Screenshots are saved in **Pictures › Screenshots** and copied, so you can paste them straight away.

## Settings

Most things are in quick settings. For the rest, open a terminal (**Super + Enter**) and type:

```
mkdir -p ~/.config/tessera && nano ~/.config/tessera/config
```

You can copy any line from `/etc/tessera/config` into it and change it, for example:

```
accent=#3cc8b4
gaps=10
scale=1.25
natural_scroll=true
idle_lock_minutes=10
```

Press **Ctrl + O**, **Enter**, **Ctrl + X** to save, then **Super + Shift + R** to apply.

## Updating

```
cd ~/tessera
git pull          # only if you got it from GitHub
sh install.sh
```

Your settings are kept.

## If something goes wrong

- **The screen is stuck or the desktop won't start:** press **Ctrl + Alt + F2**
  for a text screen, log in, and look at `~/.local/state/tessera/tessera.log`.
  **Ctrl + Alt + F1** goes back.
- **The bar disappeared:** it restarts by itself within a few seconds. Its log is
  `~/.local/state/tessera/shell.log`.
- **Wi-Fi doesn't connect after installing:** click the clock, then the arrow next
  to Wi-Fi, choose your network and type the password again.
- **Remove Tessera:** `cd ~/tessera && sh uninstall.sh`

---

## How it is built

| Part | What it does | Made with |
|---|---|---|
| `compositor/` | Window manager: draws windows, tiling, title bars, rounded corners, workspaces, keyboard and touchpad, lock screen, screen locking before sleep | C with wlroots 0.18 (Debian's version) |
| `shell/` | Bar, app launcher, quick settings, notifications, volume/brightness pop-up, welcome screen | Python with GTK 4 and gtk4-layer-shell |
| `data/` | Session start-up scripts, default settings, password check (PAM), screen-sharing setup | Shell scripts and config files |

Tessera uses Debian's standard services rather than replacing them:
NetworkManager (Wi-Fi), PipeWire (sound), BlueZ (Bluetooth), UPower (battery),
systemd-logind (sleep, brightness, power), and Xwayland for older X11 apps.

The control command `tessera msg` can switch workspaces, lock the screen and more;
run it without arguments to see the list.

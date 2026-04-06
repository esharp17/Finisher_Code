# Finisher Touch UI (Electron)

This folder contains a Raspberry Pi touchscreen front end (800x480) intended to control the Arduino sketch in the repo over USB serial.

## Raspberry Pi OS Desktop setup

1. Install Node.js + npm
   - Recommended: Node 20 LTS.
2. Install dependencies:
   - `npm install`
3. Serial permissions (so the app can open `/dev/ttyACM0` / `/dev/ttyUSB0`)
   - Add your user to the `dialout` group:
     - `sudo usermod -a -G dialout $USER`
   - Reboot or log out/in.
4. Run:
   - `npm run dev`

### Auto-start on boot (kiosk-like)

Run these commands on the Pi **once** after cloning and `npm install`:

```bash
mkdir -p ~/.config/autostart
cp ~/Finisher_Code/frontend/finisher-ui.desktop ~/.config/autostart/
```

This makes the app launch automatically when the Pi boots into the desktop.

> **Note:** If your Pi username is not `pi`, edit the `Exec` line in
> `finisher-ui.desktop` to use the correct home directory path.

To also hide the mouse cursor on the touchscreen:
```bash
sudo apt install -y unclutter
```
Then add `unclutter -idle 0 &` to `~/.config/lxsession/LXDE-pi/autostart`.

## Dev (on a machine with Node installed)

1. Install deps:
   - `npm install`
2. Run:
   - `npm run dev`

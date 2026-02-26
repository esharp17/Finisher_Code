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

### Full-screen (kiosk-like)
The Electron window is sized to 800x480. For a kiosk feel:
- Hide the taskbar / run the app on login.
- Optionally toggle fullscreen with your window manager shortcuts.

## Dev (on a machine with Node installed)

1. Install deps:
   - `npm install`
2. Run:
   - `npm run dev`

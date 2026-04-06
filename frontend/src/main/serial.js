const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');
const fs = require('fs');
const path = require('path');

const DEFAULT_BAUD = 115200;

function safeReadJson(filePath) {
  try {
    if (!fs.existsSync(filePath)) return null;
    return JSON.parse(fs.readFileSync(filePath, 'utf8'));
  } catch {
    return null;
  }
}

function safeWriteJson(filePath, data) {
  try {
    fs.mkdirSync(path.dirname(filePath), { recursive: true });
    fs.writeFileSync(filePath, JSON.stringify(data, null, 2), 'utf8');
  } catch {
    // ignore
  }
}

class SerialManager {
  constructor({ userDataDir, onLine, onConnectionChange }) {
    this._userDataDir = userDataDir;
    this._onLine = onLine;
    this._onConnectionChange = onConnectionChange;

    this._port = null;
    this._parser = null;
    this._connectedPath = null;
    this._validated = false;
    this._settingsPath = path.join(this._userDataDir, 'serial-settings.json');
  }

  async listPorts() {
    return SerialPort.list();
  }

  getConnectionInfo() {
    return {
      connected: Boolean(this._port && this._port.isOpen && this._validated),
      path: this._connectedPath
    };
  }

  _setConnected(pathValue) {
    this._connectedPath = pathValue;
    if (this._onConnectionChange) this._onConnectionChange(this.getConnectionInfo());
  }

  async connect(portPath, baudRate = DEFAULT_BAUD) {
    if (this._port && this._port.isOpen) {
      return this.getConnectionInfo();
    }

    this._port = new SerialPort({ path: portPath, baudRate, autoOpen: false });

    await new Promise((resolve, reject) => {
      this._port.open((err) => (err ? reject(err) : resolve()));
    });

    console.log(`[SERIAL] Opened ${portPath} @ ${baudRate}, waiting for Arduino...`);

    // Wait for Arduino bootloader reset to complete
    await new Promise((r) => setTimeout(r, 2500));

    this._parser = this._port.pipe(new ReadlineParser({ delimiter: '\n' }));

    // Listen for data and track whether we've seen a valid STATUS line
    let gotStatus = false;
    const dataHandler = (line) => {
      const trimmed = String(line).trim();
      if (!trimmed) return;
      console.log(`[SERIAL RX] ${trimmed}`);
      if (trimmed.startsWith('STATUS ') || trimmed.includes('Finisher Controller')) {
        gotStatus = true;
      }
      if (this._onLine) this._onLine(trimmed);
    };
    this._parser.on('data', dataHandler);

    this._port.on('close', () => {
      this._port = null;
      this._parser = null;
      this._validated = false;
      this._setConnected(null);
      this._startRetryLoop();
    });

    this._port.on('error', () => {
      // let the close handler clear state
    });

    // Send a status request and wait for a valid response
    try {
      this._port.write('status\n');
    } catch {}

    // Wait up to 4 seconds for a STATUS response
    const deadline = Date.now() + 4000;
    while (!gotStatus && Date.now() < deadline) {
      await new Promise((r) => setTimeout(r, 200));
    }

    if (!gotStatus) {
      console.log(`[SERIAL] No Arduino response on ${portPath}, disconnecting.`);
      try {
        const p = this._port;
        this._port = null;
        this._parser = null;
        await new Promise((r) => p.close(() => r()));
      } catch {}
      this._validated = false;
      this._setConnected(null);
      throw new Error(`No Arduino found on ${portPath}`);
    }

    console.log(`[SERIAL] Arduino verified on ${portPath}`);
    this._validated = true;
    this._setConnected(portPath);
    safeWriteJson(this._settingsPath, { lastPortPath: portPath, baudRate });

    return this.getConnectionInfo();
  }

  async disconnect() {
    if (!this._port) return this.getConnectionInfo();

    const portToClose = this._port;
    await new Promise((resolve) => portToClose.close(() => resolve()));

    this._port = null;
    this._parser = null;
    this._validated = false;
    this._setConnected(null);

    return this.getConnectionInfo();
  }

  async sendLine(text) {
    if (!this._port || !this._port.isOpen) {
      throw new Error('Serial not connected');
    }

    const payload = `${text}\n`;
    console.log(`[SERIAL TX] ${text}`);
    await new Promise((resolve, reject) => {
      this._port.write(payload, (err) => (err ? reject(err) : resolve()));
    });
    await new Promise((resolve, reject) => {
      this._port.drain((err) => (err ? reject(err) : resolve()));
    });
  }

  async autoConnect() {
    const result = await this._tryAutoConnect();
    if (!result.connected) {
      this._startRetryLoop();
    }
    return result;
  }

  async _tryAutoConnect() {
    if (this._port && this._port.isOpen) {
      return { ...this.getConnectionInfo(), auto: true };
    }

    const saved = safeReadJson(this._settingsPath) || {};
    const ports = await this.listPorts();

    const candidates = [];

    if (saved.lastPortPath) {
      candidates.push(saved.lastPortPath);
    }

    // Prefer Arduino-like ports if nothing saved
    for (const p of ports) {
      if (p.path && (String(p.manufacturer || '').toLowerCase().includes('arduino') || String(p.friendlyName || '').toLowerCase().includes('arduino'))) {
        candidates.push(p.path);
      }
    }

    // Prefer common Pi serial paths (ttyACM/ttyUSB are USB-serial devices)
    for (const p of ports) {
      if (p.path && (/ttyACM|ttyUSB/.test(p.path))) {
        candidates.push(p.path);
      }
    }

    // On Windows, also try COM ports
    for (const p of ports) {
      if (p.path && /^COM\d+$/i.test(p.path)) {
        candidates.push(p.path);
      }
    }

    // Do NOT fall back to all ports — avoids connecting to
    // Pi internal UART (ttyAMA0, ttyS0) or Bluetooth serial.

    const unique = [...new Set(candidates)].filter(Boolean);
    for (const portPath of unique) {
      try {
        await this.connect(portPath, saved.baudRate || DEFAULT_BAUD);
        return { ...this.getConnectionInfo(), auto: true };
      } catch {
        // try next
      }
    }

    return { ...this.getConnectionInfo(), auto: true };
  }

  _startRetryLoop() {
    if (this._retryTimer) return;
    this._retryTimer = setInterval(async () => {
      if (this._port && this._port.isOpen) {
        clearInterval(this._retryTimer);
        this._retryTimer = null;
        return;
      }
      try {
        await this._tryAutoConnect();
        if (this._port && this._port.isOpen) {
          clearInterval(this._retryTimer);
          this._retryTimer = null;
        }
      } catch {
        // will retry next interval
      }
    }, 3000);
  }
}

module.exports = {
  SerialManager
};

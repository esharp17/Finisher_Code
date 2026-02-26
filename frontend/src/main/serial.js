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
    this._lastStatus = null;

    this._settingsPath = path.join(this._userDataDir, 'serial-settings.json');
  }

  async listPorts() {
    return SerialPort.list();
  }

  getConnectionInfo() {
    return {
      connected: Boolean(this._port && this._port.isOpen),
      path: this._connectedPath
    };
  }

  getLastStatus() {
    return this._lastStatus;
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

    this._parser = this._port.pipe(new ReadlineParser({ delimiter: '\n' }));
    this._parser.on('data', (line) => {
      const trimmed = String(line).trim();
      if (!trimmed) return;

      if (trimmed.startsWith('STATUS ')) {
        this._lastStatus = trimmed;
      }

      if (this._onLine) this._onLine(trimmed);
    });

    this._port.on('close', () => {
      this._port = null;
      this._parser = null;
      this._setConnected(null);
    });

    this._port.on('error', () => {
      // let the close handler clear state
    });

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
    this._setConnected(null);

    return this.getConnectionInfo();
  }

  async sendLine(text) {
    if (!this._port || !this._port.isOpen) {
      throw new Error('Serial not connected');
    }

    const payload = `${text}\n`;
    await new Promise((resolve, reject) => {
      this._port.write(payload, (err) => (err ? reject(err) : resolve()));
    });
  }

  async autoConnect() {
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

    // Fall back to first available
    for (const p of ports) {
      if (p.path) candidates.push(p.path);
    }

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
}

module.exports = {
  SerialManager
};

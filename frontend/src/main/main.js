const { app, BrowserWindow, ipcMain, shell } = require('electron');
const path = require('path');
const fs = require('fs');
const os = require('os');
const { SerialManager } = require('./serial');

let mainWindow;
let serial;

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 800,
    height: 480,
    useContentSize: true,
    resizable: false,
    frame: false,
    kiosk: true,
    fullscreen: true,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false
    }
  });

  mainWindow.setMenuBarVisibility(false);
  mainWindow.loadFile(path.join(__dirname, '../renderer/index.html'));
}

app.whenReady().then(() => {
  createWindow();

  serial = new SerialManager({
    userDataDir: app.getPath('userData'),
    onLine: (line) => {
      const win = BrowserWindow.getAllWindows()[0];
      if (win && !win.isDestroyed()) win.webContents.send('serial:line', line);
    },
    onConnectionChange: (info) => {
      const win = BrowserWindow.getAllWindows()[0];
      if (win && !win.isDestroyed()) win.webContents.send('serial:connection', info);
    }
  });

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow();
  });
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});

// Serial wiring is added in a later milestone.
ipcMain.handle('app:getVersion', () => app.getVersion());

ipcMain.handle('serial:listPorts', async () => {
  return serial.listPorts();
});

ipcMain.handle('serial:autoConnect', async () => {
  return serial.autoConnect();
});

ipcMain.handle('serial:connect', async (_evt, { path: portPath, baudRate }) => {
  return serial.connect(portPath, baudRate);
});

ipcMain.handle('serial:disconnect', async () => {
  return serial.disconnect();
});

ipcMain.handle('serial:sendLine', async (_evt, line) => {
  await serial.sendLine(line);
  return true;
});

ipcMain.handle('serial:getConnectionInfo', async () => {
  return serial.getConnectionInfo();
});

// ---- Settings persistence ----
const settingsFilePath = path.join(app.getPath('userData'), 'app-settings.json');

ipcMain.handle('app:loadSettings', async () => {
  try {
    if (fs.existsSync(settingsFilePath)) {
      return JSON.parse(fs.readFileSync(settingsFilePath, 'utf8'));
    }
  } catch {}
  return null;
});

ipcMain.handle('app:saveSettings', async (_evt, settings) => {
  try {
    fs.writeFileSync(settingsFilePath, JSON.stringify(settings, null, 2), 'utf8');
  } catch {}
  return true;
});

// ---- Abrasive timer persistence ----
const abrasiveFilePath = path.join(app.getPath('userData'), 'abrasive-timer.json');

ipcMain.handle('app:loadAbrasiveMs', async () => {
  try {
    if (fs.existsSync(abrasiveFilePath)) {
      const data = JSON.parse(fs.readFileSync(abrasiveFilePath, 'utf8'));
      if (typeof data.abrasiveMs === 'number') return data.abrasiveMs;
    }
  } catch {}
  return null;
});

ipcMain.handle('app:saveAbrasiveMs', async (_evt, ms) => {
  try {
    fs.writeFileSync(abrasiveFilePath, JSON.stringify({ abrasiveMs: ms }), 'utf8');
  } catch {}
  return true;
});

ipcMain.handle('app:saveCsv', async (_evt, csvString, suggestedName) => {
  const docsDir = path.join(os.homedir(), 'Documents');
  const saveDir = fs.existsSync(docsDir) ? docsDir : os.homedir();
  const filePath = path.join(saveDir, suggestedName || 'finisher-log.csv');
  fs.writeFileSync(filePath, csvString, 'utf8');
  shell.showItemInFolder(filePath);
  return filePath;
});


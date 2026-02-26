const { app, BrowserWindow, ipcMain } = require('electron');
const path = require('path');
const { SerialManager } = require('./serial');

let mainWindow;
let serial;

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 800,
    height: 480,
    useContentSize: true,
    resizable: false,
    fullscreenable: true,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false
    }
  });

  mainWindow.setMenuBarVisibility(false);
  mainWindow.setContentSize(800, 480);
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

ipcMain.handle('serial:getLastStatus', async () => {
  return serial.getLastStatus();
});

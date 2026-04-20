const { app, BrowserWindow, ipcMain, shell } = require('electron');
const path = require('path');
const fs = require('fs');
const os = require('os');
const { execFile } = require('child_process');
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

  // Forward renderer console messages to main process stdout/stderr so
  // --enable-logging captures them (useful for debugging on headless/kiosk).
  mainWindow.webContents.on('console-message', (_e, level, message, line, sourceId) => {
    const levels = ['LOG', 'WARN', 'ERROR', 'INFO'];
    const tag = levels[level] || 'LOG';
    process.stderr.write(`[RENDERER ${tag}] ${message} (${sourceId}:${line})\n`);
  });
  mainWindow.webContents.on('render-process-gone', (_e, details) => {
    process.stderr.write(`[RENDERER CRASH] ${JSON.stringify(details)}\n`);
  });

  // Open DevTools if FINISHER_DEBUG=1 env var is set
  if (process.env.FINISHER_DEBUG === '1') {
    mainWindow.webContents.openDevTools({ mode: 'detach' });
  }
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

// ---- Data log persistence ----
const logFilePath = path.join(app.getPath('userData'), 'data-log.json');

ipcMain.handle('app:loadLog', async () => {
  try {
    if (fs.existsSync(logFilePath)) {
      const data = JSON.parse(fs.readFileSync(logFilePath, 'utf8'));
      if (Array.isArray(data.entries)) return data;
    }
  } catch {}
  return null;
});

ipcMain.handle('app:saveLog', async (_evt, logData) => {
  try {
    fs.writeFileSync(logFilePath, JSON.stringify(logData), 'utf8');
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

// ---- Update from GitHub & relaunch ----
// Repo root is one level above the frontend/ directory.
const repoRoot = path.resolve(__dirname, '..', '..', '..');
const frontendDir = path.resolve(__dirname, '..', '..');

function runCmd(cmd, args, cwd) {
  return new Promise((resolve, reject) => {
    execFile(cmd, args, { cwd, timeout: 120000 }, (err, stdout, stderr) => {
      if (err) return reject(new Error(`${cmd} ${args.join(' ')} failed: ${stderr || err.message}`));
      resolve(stdout.trim());
    });
  });
}

ipcMain.handle('app:updateApp', async () => {
  try {
    const isLinux = process.platform === 'linux';
    const git = isLinux ? 'git' : 'git';
    const npm = isLinux ? 'npm' : process.platform === 'win32' ? 'npm.cmd' : 'npm';

    // 1) git pull
    const pullResult = await runCmd(git, ['pull', '--ff-only'], repoRoot);
    console.log('[UPDATE] git pull:', pullResult);

    const alreadyUpToDate = pullResult.includes('Already up to date') || pullResult.includes('Already up-to-date');

    // 2) npm install only if package.json was in the changed files
    if (!alreadyUpToDate && pullResult.includes('package.json')) {
      console.log('[UPDATE] package.json changed, running npm install...');
      const installResult = await runCmd(npm, ['install', '--no-optional'], frontendDir);
      console.log('[UPDATE] npm install:', installResult);
    }

    // 3) Relaunch
    app.relaunch();
    app.exit(0);

    return { success: true, message: pullResult };
  } catch (e) {
    console.error('[UPDATE] Error:', e.message);
    return { success: false, message: e.message };
  }
});


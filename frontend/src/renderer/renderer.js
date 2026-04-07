// ============================================================
//  Finisher UI – Renderer
// ============================================================

const settings = {
  sopPlanetRpm: 300,
  sopCentralRpm: 280,
  sopVib: 35,
  sopTimeMins: 180,
  abrasiveHours: 12
};

// Goal values (what the operator sets via spinners / SOP)
const goal = {
  planetRpm: 0,
  centralRpm: 0,
  vib: 0,
  timeMins: 0
};

// Live values (actual readings from Arduino STATUS lines)
const live = {
  planetRpm: 0,
  centralRpm: 0,
  vib: 0
};

const state = {
  running: false,
  paused: false,
  sopActive: false,
  countdownMs: 0,
  abrasiveMs: 12 * 60 * 60 * 1000,
  connected: false
};

// Pending SOP-exit adjustment to apply after confirmation
let _pendingSopAdjust = null;

// Motor directions: 'CW' or 'CCW'
const dirs = {
  p1: 'CCW',   // P1 defaults CCW (flipped wiring)
  p2: 'CW',
  central: 'CW'
};

// Track whether we've already reversed at the halfway point this run
let halfwayReversed = false;

const dataLog = [];
let runNumber = 0;
let abrasiveLocked = false;

// ============================================================
//  Helpers
// ============================================================
function pad2(n) { return String(n).padStart(2, '0'); }

function formatMMSS(ms) {
  const s = Math.max(0, Math.floor(ms / 1000));
  return `${pad2(Math.floor(s / 60))}:${pad2(s % 60)}`;
}

function formatHHMMSS(ms) {
  const s = Math.max(0, Math.floor(ms / 1000));
  const hh = Math.floor(s / 3600);
  const mm = Math.floor((s % 3600) / 60);
  return `${pad2(hh)}:${pad2(mm)}:${pad2(s % 60)}`;
}

function timeStamp() {
  const d = new Date();
  return `${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
}

function stateLabel() {
  if (state.paused) return 'PAUSED';
  if (state.sopActive) return 'SOP';
  if (state.running) return 'RUNNING';
  return 'IDLE';
}

// ============================================================
//  Serial (queued to avoid interleaving rapid commands)
// ============================================================
let _cmdQueue = Promise.resolve();

function sendCommand(cmd) {
  _cmdQueue = _cmdQueue.then(async () => {
    try {
      console.log(`[UI TX] ${cmd}`);
      await window.finisher.serial.sendLine(cmd);
      // small gap so Arduino serial buffer can process each line
      await new Promise(r => setTimeout(r, 50));
    } catch {
      // UI works in demo mode if disconnected
    }
  });
}

// ============================================================
//  Beacon helpers
// ============================================================
const BEACON_TOLERANCE = 5; // RPM or % tolerance for "at speed"

function beaconClass(goalVal, liveVal) {
  if (goalVal === 0 && liveVal < BEACON_TOLERANCE) return 'red';
  if (Math.abs(liveVal - goalVal) <= BEACON_TOLERANCE) return 'green';
  return 'yellow';
}

function setBeacon(id, cls) {
  const el = document.getElementById(id);
  el.className = 'beacon ' + cls;
}

// ============================================================
//  Render
// ============================================================
function render() {
  document.getElementById('planetVal').textContent = goal.planetRpm;
  document.getElementById('centralVal').textContent = Math.round(goal.centralRpm / 4);
  document.getElementById('vibVal').textContent = goal.vib;
  document.getElementById('timeVal').textContent = goal.timeMins;

  // Direction buttons
  document.getElementById('dirP1').textContent = dirs.p1;
  document.getElementById('dirCentral').textContent = dirs.central;
  document.getElementById('mainTimer').textContent = formatMMSS(state.countdownMs);
  document.getElementById('abrasiveTimer').textContent = formatHHMMSS(state.abrasiveMs);

  // Beacons
  if (state.running && !state.paused) {
    setBeacon('beaconPlanet', beaconClass(goal.planetRpm, live.planetRpm));
    setBeacon('beaconCentral', beaconClass(goal.centralRpm, live.centralRpm));
    setBeacon('beaconVib', beaconClass(goal.vib, live.vib));
  } else {
    setBeacon('beaconPlanet', 'red');
    setBeacon('beaconCentral', 'red');
    setBeacon('beaconVib', 'red');
  }

  const connEl = document.getElementById('connStatus');
  if (state.connected) {
    connEl.textContent = '\u25CF Connected';
    connEl.className = 'conn-status connected';
  } else {
    connEl.textContent = '\u25CF Disconnected';
    connEl.className = 'conn-status';
  }

  const startBtn = document.getElementById('startBtn');
  const pauseBtn = document.getElementById('pauseBtn');
  const sopBtn   = document.getElementById('sopBtn');

  if (state.paused) {
    startBtn.textContent = 'Resume';
    startBtn.disabled = false;
    pauseBtn.textContent = 'Cancel';
    pauseBtn.className = 'btn btn-stop';
    sopBtn.disabled = true;
  } else if (state.running) {
    startBtn.textContent = 'Start';
    startBtn.disabled = true;
    pauseBtn.textContent = 'Pause';
    pauseBtn.className = 'btn btn-pause';
    pauseBtn.disabled = false;
    sopBtn.disabled = true;
  } else {
    startBtn.textContent = 'Start';
    startBtn.disabled = false;
    pauseBtn.textContent = 'Pause';
    pauseBtn.className = 'btn btn-pause';
    pauseBtn.disabled = true;
    sopBtn.disabled = false;
  }
}

// ============================================================
//  Actions
// ============================================================
function startRun() {
  if (abrasiveLocked) return;
  if (state.paused) {
    state.paused = false;
    sendCommand('resume');
    addLogEntry('RESUMED');
    render();
    return;
  }
  runNumber++;
  state.running = true;
  state.paused  = false;
  halfwayReversed = false;
  state.countdownMs = goal.timeMins * 60 * 1000;
  sendCommand(`prpm ${goal.planetRpm}`);
  sendCommand(`crpm ${goal.centralRpm}`);
  sendCommand(`vib ${goal.vib}`);
  sendCommand('start');
  addLogEntry('STARTED');
  render();
}

function pauseOrCancel() {
  if (abrasiveLocked) return;
  if (state.paused) {
    state.running   = false;
    state.paused    = false;
    state.sopActive = false;
    state.countdownMs = 0;
    sendCommand('cancel');
    addLogEntry('CANCELLED');
    render();
    return;
  }
  if (state.running) {
    state.paused = true;
    sendCommand('pause');
    addLogEntry('PAUSED');
    render();
  }
}

function startSop() {
  if (abrasiveLocked) return;
  runNumber++;
  state.running   = true;
  state.paused    = false;
  state.sopActive = true;
  halfwayReversed = false;
  goal.planetRpm  = settings.sopPlanetRpm;
  goal.centralRpm = settings.sopCentralRpm;
  goal.vib        = settings.sopVib;
  goal.timeMins   = settings.sopTimeMins;
  state.countdownMs = settings.sopTimeMins * 60 * 1000;
  sendCommand(`prpm ${settings.sopPlanetRpm}`);
  sendCommand(`crpm ${settings.sopCentralRpm}`);
  sendCommand(`vib ${settings.sopVib}`);
  sendCommand('start');
  addLogEntry('SOP STARTED');
  render();
}

function fullStop() {
  state.running   = false;
  state.paused    = false;
  state.sopActive = false;
  state.countdownMs = 0;
  sendCommand('stop');
  addLogEntry('FINISHED');
  render();
}

// ============================================================
//  SOP exit confirmation
// ============================================================
function requestSopExit(target, dir) {
  _pendingSopAdjust = { target, dir };
  document.getElementById('confirmSopExitOverlay').classList.add('visible');
}

function confirmSopExit() {
  document.getElementById('confirmSopExitOverlay').classList.remove('visible');
  state.sopActive = false;
  if (_pendingSopAdjust) {
    applyAdjust(_pendingSopAdjust.target, _pendingSopAdjust.dir);
    _pendingSopAdjust = null;
  }
}

function cancelSopExit() {
  document.getElementById('confirmSopExitOverlay').classList.remove('visible');
  _pendingSopAdjust = null;
}

function adjust(target, dir) {
  // If SOP is active and they're changing speed/vib, confirm first
  if (state.sopActive && (target === 'planet' || target === 'central' || target === 'vib')) {
    requestSopExit(target, dir);
    return;
  }
  applyAdjust(target, dir);
}

function applyAdjust(target, dir) {
  if (target === 'planet') {
    const delta = dir === 'up' ? 10 : -10;
    goal.planetRpm = Math.max(0, goal.planetRpm + delta);
    if (state.running && !state.paused) sendCommand(`prpm ${goal.planetRpm}`);
  }
  if (target === 'central') {
    const delta = dir === 'up' ? 10 : -10;
    goal.centralRpm = Math.max(0, goal.centralRpm + delta);
    if (state.running && !state.paused) sendCommand(`crpm ${goal.centralRpm}`);
  }
  if (target === 'vib') {
    const delta = dir === 'up' ? 5 : -5;
    goal.vib = Math.max(0, Math.min(100, goal.vib + delta));
    sendCommand(`vib ${goal.vib}`);
  }
  if (target === 'time') {
    goal.timeMins = Math.max(0, goal.timeMins + (dir === 'up' ? 10 : -10));
  }
  render();
}

// ============================================================
//  Data Log (event-based: start, pause, resume, finish only)
// ============================================================
function addLogEntry(event) {
  const entry = {
    run: runNumber,
    time: timeStamp(),
    event: event,
    planetRpm: goal.planetRpm,
    centralRpm: goal.centralRpm,
    vib: goal.vib,
    timer: formatMMSS(state.countdownMs)
  };
  dataLog.push(entry);

  const tbody = document.getElementById('logBody');
  const tr = document.createElement('tr');
  tr.innerHTML = `<td>${entry.run}</td><td>${entry.time}</td><td>${entry.event}</td><td>${entry.planetRpm}</td><td>${entry.centralRpm}</td><td>${entry.vib}%</td><td>${entry.timer}</td>`;
  tbody.appendChild(tr);
  tr.scrollIntoView({ block: 'end' });

  document.getElementById('logCount').textContent = `${dataLog.length} entries`;
}

async function exportCsv() {
  if (dataLog.length === 0) return;
  const header = 'Run,Time,Event,Planet RPM,Central RPM,Vib %,Timer\n';
  const rows = dataLog.map(e => `${e.run},${e.time},${e.event},${e.planetRpm},${e.centralRpm},${e.vib},${e.timer}`).join('\n');
  const fileName = `finisher-log-${new Date().toISOString().slice(0, 10)}.csv`;
  try {
    const savedPath = await window.finisher.saveCsv(header + rows, fileName);
    console.log('CSV saved to', savedPath);
  } catch (err) {
    console.error('CSV export failed', err);
  }
}

function clearLog() {
  dataLog.length = 0;
  runNumber = 0;
  document.getElementById('logBody').innerHTML = '';
  document.getElementById('logCount').textContent = '0 entries';
}

// ============================================================
//  Abrasive Change Popup
// ============================================================
function showAbrasivePopup() {
  abrasiveLocked = true;
  if (state.running) {
    sendCommand('stop');
    state.running = false;
    state.paused = false;
    state.sopActive = false;
    state.countdownMs = 0;
    addLogEntry('ABRASIVE STOP');
  }
  document.getElementById('abrasiveOverlay').classList.add('visible');
  render();
}

function dismissAbrasivePopup() {
  abrasiveLocked = false;
  state.abrasiveMs = settings.abrasiveHours * 60 * 60 * 1000;
  window.finisher.saveAbrasiveMs(state.abrasiveMs).catch(() => {});
  document.getElementById('abrasiveOverlay').classList.remove('visible');
  render();
}

function resetAbrasiveTimer() {
  state.abrasiveMs = settings.abrasiveHours * 60 * 60 * 1000;
  window.finisher.saveAbrasiveMs(state.abrasiveMs).catch(() => {});
  render();
  renderSettings();
}

// ============================================================
//  Settings
// ============================================================
function renderSettings() {
  document.getElementById('sopPlanetVal').textContent = settings.sopPlanetRpm;
  document.getElementById('sopCentralVal').textContent = settings.sopCentralRpm;
  document.getElementById('sopVibVal').textContent = settings.sopVib;
  document.getElementById('sopTimeVal').textContent = settings.sopTimeMins;
  document.getElementById('abrasiveHoursVal').textContent = settings.abrasiveHours;
  document.getElementById('abrasiveRemaining').textContent = formatHHMMSS(state.abrasiveMs);
}

function adjustSetting(key, delta, min, max) {
  settings[key] = Math.max(min, Math.min(max, settings[key] + delta));
  renderSettings();
  saveSettings();
}

function saveSettings() {
  window.finisher.saveSettings({ ...settings }).catch(() => {});
}

// SOP parameter buttons
document.getElementById('sopPlanetUp').addEventListener('click', () => adjustSetting('sopPlanetRpm', 10, 0, 1000));
document.getElementById('sopPlanetDown').addEventListener('click', () => adjustSetting('sopPlanetRpm', -10, 0, 1000));
document.getElementById('sopCentralUp').addEventListener('click', () => adjustSetting('sopCentralRpm', 10, 0, 1000));
document.getElementById('sopCentralDown').addEventListener('click', () => adjustSetting('sopCentralRpm', -10, 0, 1000));
document.getElementById('sopVibUp').addEventListener('click', () => adjustSetting('sopVib', 5, 0, 100));
document.getElementById('sopVibDown').addEventListener('click', () => adjustSetting('sopVib', -5, 0, 100));
document.getElementById('sopTimeUp').addEventListener('click', () => adjustSetting('sopTimeMins', 10, 10, 600));
document.getElementById('sopTimeDown').addEventListener('click', () => adjustSetting('sopTimeMins', -10, 10, 600));

// Abrasive duration buttons
document.getElementById('abrasiveHoursUp').addEventListener('click', () => adjustSetting('abrasiveHours', 1, 1, 48));
document.getElementById('abrasiveHoursDown').addEventListener('click', () => adjustSetting('abrasiveHours', -1, 1, 48));

// SOP exit confirmation buttons
document.getElementById('confirmSopExitYes').addEventListener('click', confirmSopExit);
document.getElementById('confirmSopExitNo').addEventListener('click', cancelSopExit);

// Manual abrasive reset (with confirmation)
document.getElementById('resetAbrasiveBtn').addEventListener('click', () => {
  document.getElementById('confirmResetOverlay').classList.add('visible');
});
document.getElementById('confirmResetYes').addEventListener('click', () => {
  document.getElementById('confirmResetOverlay').classList.remove('visible');
  resetAbrasiveTimer();
});
document.getElementById('confirmResetNo').addEventListener('click', () => {
  document.getElementById('confirmResetOverlay').classList.remove('visible');
});

// Software update button
document.getElementById('updateAppBtn').addEventListener('click', async () => {
  const statusEl = document.getElementById('updateStatus');
  const btn = document.getElementById('updateAppBtn');
  btn.disabled = true;
  btn.textContent = 'Updating...';
  statusEl.textContent = 'Pulling from GitHub...';
  statusEl.style.color = '#ccc';

  try {
    const result = await window.finisher.updateApp();
    if (result.success) {
      statusEl.textContent = 'Update successful — restarting...';
      statusEl.style.color = '#4caf50';
    } else {
      statusEl.textContent = result.message || 'Update failed';
      statusEl.style.color = '#d94b44';
      btn.disabled = false;
      btn.textContent = 'Update & Restart';
    }
  } catch (e) {
    statusEl.textContent = 'Error: ' + (e.message || 'unknown');
    statusEl.style.color = '#d94b44';
    btn.disabled = false;
    btn.textContent = 'Update & Restart';
  }
});

// ============================================================
//  Tab switching
// ============================================================
for (const tab of document.querySelectorAll('.tab')) {
  tab.addEventListener('click', () => {
    document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
    document.querySelectorAll('.tab-content').forEach(p => p.classList.remove('active'));
    tab.classList.add('active');
    document.getElementById(`tab-${tab.dataset.tab}`).classList.add('active');
    if (tab.dataset.tab === 'settings') renderSettings();
  });
}

// ============================================================
//  Event wiring
// ============================================================
document.getElementById('startBtn').addEventListener('click', startRun);
document.getElementById('pauseBtn').addEventListener('click', pauseOrCancel);
document.getElementById('sopBtn').addEventListener('click', startSop);
document.getElementById('exportCsvBtn').addEventListener('click', exportCsv);
document.getElementById('clearLogBtn').addEventListener('click', clearLog);
document.getElementById('abrasiveDoneBtn').addEventListener('click', dismissAbrasivePopup);

for (const el of document.querySelectorAll('.spin')) {
  el.addEventListener('click', () => adjust(el.dataset.target, el.dataset.dir));
}

// Direction toggle buttons
function toggleDir(key, cmdPrefix) {
  dirs[key] = dirs[key] === 'CW' ? 'CCW' : 'CW';
  sendCommand(`${cmdPrefix} ${dirs[key].toLowerCase()}`);
  render();
}

document.getElementById('dirP1').addEventListener('click', () => toggleDir('p1', 'dir1'));
document.getElementById('dirCentral').addEventListener('click', () => toggleDir('central', 'dirc'));

// ============================================================
//  Timer tick (1 second)
// ============================================================
let abrasiveSaveCounter = 0;

setInterval(() => {
  if ((state.running && !state.paused) && state.countdownMs > 0) {
    state.countdownMs = Math.max(0, state.countdownMs - 1000);
    state.abrasiveMs  = Math.max(0, state.abrasiveMs - 1000);

    // Save abrasive timer to disk every 30 seconds of run time
    abrasiveSaveCounter++;
    if (abrasiveSaveCounter >= 30) {
      abrasiveSaveCounter = 0;
      window.finisher.saveAbrasiveMs(state.abrasiveMs).catch(() => {});
    }

    // Auto-reverse all stepper directions at the halfway point
    const totalMs = goal.timeMins * 60 * 1000;
    if (!halfwayReversed && totalMs > 0 && state.countdownMs <= totalMs / 2) {
      halfwayReversed = true;
      sendCommand('reverse');
      dirs.p1 = dirs.p1 === 'CW' ? 'CCW' : 'CW';
      dirs.p2 = dirs.p2 === 'CW' ? 'CCW' : 'CW';
      dirs.central = dirs.central === 'CW' ? 'CCW' : 'CW';
      addLogEntry('DIRECTION REVERSED (halfway)');
    }

    if (state.abrasiveMs === 0 && !abrasiveLocked) {
      window.finisher.saveAbrasiveMs(0).catch(() => {});
      showAbrasivePopup();
      return;
    }

    if (state.countdownMs === 0) {
      fullStop();
      return;
    }
  }
  render();
}, 1000);

// ============================================================
//  Serial line parsing (STATUS lines from Arduino)
// ============================================================
function parseSerialLine(line) {
  if (line.startsWith('STATUS ')) {
    const kv = {};
    for (const p of line.substring(7).split(' ')) {
      const i = p.indexOf('=');
      if (i !== -1) kv[p.substring(0, i)] = p.substring(i + 1);
    }

    if (kv.state) {
      const st = kv.state.toUpperCase();
      state.running   = st !== 'IDLE';
      state.paused    = st === 'PAUSED';
      state.sopActive = st === 'SOP';
    }
    if (kv.planetRPM !== undefined) {
      const v = Number(kv.planetRPM);
      if (!isNaN(v)) live.planetRpm = Math.round(v);
    }
    if (kv.centralRPM !== undefined) {
      const v = Number(kv.centralRPM);
      if (!isNaN(v)) live.centralRpm = Math.round(v);
    }
    if (kv.vib !== undefined) {
      const v = Number(kv.vib);
      if (!isNaN(v)) live.vib = v;
    }
    render();
  }
}

window.finisher.serial.onLine((line) => parseSerialLine(line));

window.finisher.serial.onConnectionChange((info) => {
  state.connected = Boolean(info && info.connected);
  render();
});

(async () => {
  // Load persisted settings
  try {
    const saved = await window.finisher.loadSettings();
    if (saved) {
      if (typeof saved.sopPlanetRpm === 'number')  settings.sopPlanetRpm  = saved.sopPlanetRpm;
      if (typeof saved.sopCentralRpm === 'number') settings.sopCentralRpm = saved.sopCentralRpm;
      if (typeof saved.sopVib === 'number')        settings.sopVib        = saved.sopVib;
      if (typeof saved.sopTimeMins === 'number')   settings.sopTimeMins   = saved.sopTimeMins;
      if (typeof saved.abrasiveHours === 'number') settings.abrasiveHours = saved.abrasiveHours;
    }
  } catch {}

  // Load persisted abrasive timer
  try {
    const saved = await window.finisher.loadAbrasiveMs();
    if (typeof saved === 'number' && saved >= 0) {
      state.abrasiveMs = saved;
      if (saved === 0) showAbrasivePopup();
    }
  } catch {}

  // Auto-connect serial
  try { await window.finisher.serial.autoConnect(); } catch {}

  render();
  renderSettings();
})();

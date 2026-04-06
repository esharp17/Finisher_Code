// ============================================================
//  Finisher UI – Renderer
// ============================================================

const state = {
  planetRpm: 0,
  centralRpm: 0,
  vib: 0,
  timeMins: 0,
  running: false,
  paused: false,
  sopActive: false,
  countdownMs: 0,
  abrasiveMs: 12 * 60 * 60 * 1000,
  connected: false
};

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
//  Serial
// ============================================================
async function sendCommand(cmd) {
  try {
    await window.finisher.serial.sendLine(cmd);
  } catch {
    // UI works in demo mode if disconnected
  }
}

// ============================================================
//  Render
// ============================================================
function render() {
  document.getElementById('planetVal').textContent = state.planetRpm;
  document.getElementById('centralVal').textContent = state.centralRpm;
  document.getElementById('vibVal').textContent = state.vib;
  document.getElementById('timeVal').textContent = state.timeMins;
  document.getElementById('mainTimer').textContent = formatMMSS(state.countdownMs);
  document.getElementById('abrasiveTimer').textContent = formatHHMMSS(state.abrasiveMs);

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
  state.countdownMs = state.timeMins * 60 * 1000;
  sendCommand(`prpm ${state.planetRpm}`);
  sendCommand(`crpm ${state.centralRpm}`);
  sendCommand(`vib ${state.vib}`);
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
  state.planetRpm  = 300;
  state.centralRpm = 280;
  state.vib        = 33;
  state.timeMins   = 180;
  state.countdownMs = 180 * 60 * 1000;
  sendCommand('sop');
  sendCommand('vib 33');
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

function adjust(target, dir) {
  if (target === 'planet') {
    const delta = dir === 'up' ? 10 : -10;
    state.planetRpm = Math.max(0, state.planetRpm + delta);
    if (state.running && !state.paused) sendCommand(`prpm ${state.planetRpm}`);
  }
  if (target === 'central') {
    const delta = dir === 'up' ? 10 : -10;
    state.centralRpm = Math.max(0, state.centralRpm + delta);
    if (state.running && !state.paused) sendCommand(`crpm ${state.centralRpm}`);
  }
  if (target === 'vib') {
    const delta = dir === 'up' ? 10 : -10;
    state.vib = Math.max(0, Math.min(100, state.vib + delta));
    sendCommand(`vib ${state.vib}`);
  }
  if (target === 'time') {
    state.timeMins = Math.max(0, state.timeMins + (dir === 'up' ? 10 : -10));
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
    planetRpm: state.planetRpm,
    centralRpm: state.centralRpm,
    vib: state.vib,
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
  state.abrasiveMs = 12 * 60 * 60 * 1000;
  document.getElementById('abrasiveOverlay').classList.remove('visible');
  render();
}

// ============================================================
//  Tab switching
// ============================================================
for (const tab of document.querySelectorAll('.tab')) {
  tab.addEventListener('click', () => {
    document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
    document.querySelectorAll('.tab-content').forEach(p => p.classList.remove('active'));
    tab.classList.add('active');
    document.getElementById(`tab-${tab.dataset.tab}`).classList.add('active');
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

// ============================================================
//  Timer tick (1 second)
// ============================================================
setInterval(() => {
  if ((state.running && !state.paused) && state.countdownMs > 0) {
    state.countdownMs = Math.max(0, state.countdownMs - 1000);
    state.abrasiveMs  = Math.max(0, state.abrasiveMs - 1000);

    if (state.abrasiveMs === 0 && !abrasiveLocked) {
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
      if (!isNaN(v)) state.planetRpm = Math.round(v);
    }
    if (kv.centralRPM !== undefined) {
      const v = Number(kv.centralRPM);
      if (!isNaN(v)) state.centralRpm = Math.round(v);
    }
    if (kv.vib !== undefined) {
      const v = Number(kv.vib);
      if (!isNaN(v)) state.vib = v;
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
  try { await window.finisher.serial.autoConnect(); } catch {}
})();

render();

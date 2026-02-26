const state = {
  centralRpm: 0,
  planetRpm: 0,
  timeMins: 0,
  running: false,
  countdownMs: 0,
  abrasiveMs: 12 * 60 * 60 * 1000,
  connected: false
};

let lastCommand = null;

function pad2(n) {
  return String(n).padStart(2, '0');
}

function formatMMSS(ms) {
  const totalSec = Math.max(0, Math.floor(ms / 1000));
  const mm = Math.floor(totalSec / 60);
  const ss = totalSec % 60;
  return `${pad2(mm)}:${pad2(ss)}`;
}

function formatHHMMSS(ms) {
  const totalSec = Math.max(0, Math.floor(ms / 1000));
  const hh = Math.floor(totalSec / 3600);
  const mm = Math.floor((totalSec % 3600) / 60);
  const ss = totalSec % 60;
  return `${pad2(hh)}:${pad2(mm)}:${pad2(ss)}`;
}

function render() {
  document.getElementById('centralVal').textContent = state.centralRpm;
  document.getElementById('planetVal').textContent = state.planetRpm;
  document.getElementById('timeVal').textContent = state.timeMins;

  document.getElementById('mainTimer').textContent = formatMMSS(state.countdownMs);
  document.getElementById('abrasiveTimer').textContent = formatHHMMSS(state.abrasiveMs);

  document.getElementById('startBtn').disabled = state.running;
  document.getElementById('sopBtn').disabled = state.running;
}

async function sendCommand(cmd) {
  lastCommand = cmd;
  try {
    await window.finisher.serial.sendLine(cmd);
  } catch {
    // ignore; UI can operate in demo mode if disconnected
  }
}

function startRun() {
  state.running = true;
  state.countdownMs = state.timeMins * 60 * 1000;
  sendCommand('START');
  render();
}

function stopRun() {
  state.running = false;
  state.countdownMs = 0;
  sendCommand('STOP');
  render();
}

function startSop() {
  state.running = true;
  state.countdownMs = state.timeMins * 60 * 1000;
  sendCommand('SOP');
  render();
}

function adjust(target, dir) {
  const delta = dir === 'up' ? 10 : -10;

  if (target === 'central') {
    state.centralRpm = Math.max(0, state.centralRpm + delta);
    sendCommand(dir === 'up' ? 'C UP' : 'C DOWN');
  }

  if (target === 'planet') {
    state.planetRpm = Math.max(0, state.planetRpm + delta);
    sendCommand(dir === 'up' ? 'P UP' : 'P DOWN');
  }

  if (target === 'time') {
    state.timeMins = Math.max(0, state.timeMins + (dir === 'up' ? 1 : -1));
  }

  render();
}

document.getElementById('startBtn').addEventListener('click', startRun);
document.getElementById('stopBtn').addEventListener('click', stopRun);
document.getElementById('sopBtn').addEventListener('click', startSop);

for (const el of document.querySelectorAll('.spin')) {
  el.addEventListener('click', () => {
    adjust(el.dataset.target, el.dataset.dir);
  });
}

setInterval(() => {
  if (state.running && state.countdownMs > 0) {
    state.countdownMs = Math.max(0, state.countdownMs - 1000);
    state.abrasiveMs = Math.max(0, state.abrasiveMs - 1000);
    if (state.countdownMs === 0) {
      stopRun();
      return;
    }
  }

  render();
}, 1000);

function parseStatus(line) {
  if (!line.startsWith('STATUS ')) return;
  const parts = line.substring(7).split(' ');
  const kv = {};
  for (const p of parts) {
    const idx = p.indexOf('=');
    if (idx === -1) continue;
    kv[p.substring(0, idx)] = p.substring(idx + 1);
  }

  if (kv.centralRPM !== undefined) {
    const v = Number(kv.centralRPM);
    if (!Number.isNaN(v)) state.centralRpm = Math.round(v);
  }
  if (kv.planetRPM !== undefined) {
    const v = Number(kv.planetRPM);
    if (!Number.isNaN(v)) state.planetRpm = Math.round(v);
  }

  if (kv.state) {
    const st = String(kv.state).toUpperCase();
    state.running = st !== 'STOPPED' && st !== 'IDLE';
  }

  render();
}

window.finisher.serial.onLine((line) => {
  parseStatus(line);
});

window.finisher.serial.onConnectionChange((info) => {
  state.connected = Boolean(info && info.connected);
  render();
});

(async () => {
  try {
    await window.finisher.serial.autoConnect();
  } catch {
    // ignore
  }
})();

render();

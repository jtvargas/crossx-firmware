import { X4Flasher, sha256Hex } from './flasher.js';

const $ = (id) => document.getElementById(id);

const state = {
  manifest: null,
  fw: null, // build id or 'custom'
  customFile: null,
  restoreFile: null,
  running: false,
  last: null, // { action, trigger } for Try again
};

const serialSupported = X4Flasher.serialSupported();

// --- Formatting ---

function formatBytes(bytes) {
  return bytes >= 1024 * 1024 ? `${(bytes / 1024 / 1024).toFixed(2)} MB` : `${Math.round(bytes / 1024)} KB`;
}

function shortSha(sha) {
  return sha ? sha.slice(0, 7) : '';
}

function formatDate(iso) {
  const date = new Date(iso);
  return Number.isNaN(date.getTime()) ? '' : date.toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' });
}

function selectedBuild() {
  return state.manifest?.builds.find((b) => b.id === state.fw) ?? null;
}

// --- Firmware catalog ---

async function loadManifest() {
  const status = $('builds-status');
  try {
    const response = await fetch('manifest.json', { cache: 'no-cache' });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    state.manifest = await response.json();
  } catch {
    status.textContent = 'No published build was found. You can still install a .bin from your computer under Advanced.';
    return;
  }
  const { builds, commit, generatedAt, repository } = state.manifest;
  status.textContent = builds.length ? '' : 'No published build yet.';
  status.hidden = builds.length > 0;

  const container = $('builds');
  for (const build of builds) {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'choice';
    button.dataset.fw = build.id;
    button.setAttribute('aria-pressed', 'false');
    button.innerHTML = `
      <span class="title"><span></span><span class="tag"></span></span>
      <span class="sub"></span>
      <span class="facts"></span>`;
    button.querySelector('.title span').textContent = build.name;
    const tag = button.querySelector('.tag');
    tag.textContent = build.channel;
    tag.classList.toggle('warn', build.requiresAck);
    button.querySelector('.sub').textContent = build.description;
    button.querySelector('.facts').textContent = `${build.version} · ${formatBytes(build.size)}`;
    container.append(button);
  }

  const info = $('build-info');
  const commitLink = document.createElement('a');
  commitLink.href = `https://github.com/${repository}/commit/${commit}`;
  commitLink.textContent = shortSha(commit);
  info.append('Builds from ', commitLink, ` · ${formatDate(generatedAt)}`);
}

// --- Step 2 -> 3: progressive reveal ---

function selectFirmware(fw) {
  if (state.running) return;
  state.fw = fw;
  document.querySelectorAll('[data-fw]').forEach((el) => el.setAttribute('aria-pressed', String(el.dataset.fw === fw)));
  document.querySelector('.advanced .file').hidden = fw !== 'custom';
  updateInstallStep();
}

function updateInstallStep() {
  const build = selectedBuild();
  const custom = state.fw === 'custom';
  const ready = Boolean(build) || (custom && state.customFile);

  document.querySelector('[data-step="2"]').classList.toggle('complete', ready);
  const step3 = document.querySelector('[data-step="3"]');
  step3.hidden = !state.fw;

  const needsAck = custom || Boolean(build?.requiresAck);
  $('ack-row').hidden = !needsAck;
  $('install-label').textContent = build ? `Install ${build.name} via USB` : 'Install via USB';
  $('install-meta').textContent = build
    ? `${build.version} · about two minutes`
    : custom && state.customFile
      ? `${state.customFile.name} · ${formatBytes(state.customFile.size)}`
      : custom
        ? 'Choose a .bin file above'
        : '';
  $('install').disabled = !serialSupported || state.running || !ready || (needsAck && !$('ack').checked);
}

// --- Progress card ---

const progress = {
  begin(title, steps) {
    $('progress').hidden = false;
    $('restart').hidden = true;
    $('progress-error').hidden = true;
    $('connection-issue').hidden = true;
    $('progress-title').textContent = title;
    $('progress-detail').textContent = '';
    $('progress-log').textContent = '';
    const list = $('progress-steps');
    list.replaceChildren(
      ...steps.map((name) => {
        const li = document.createElement('li');
        li.textContent = name;
        li.dataset.state = 'pending';
        return li;
      }),
    );
    this.percent(0);
    $('progress').scrollIntoView({ behavior: matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth', block: 'start' });
  },
  step(index, status) {
    const li = $('progress-steps').children[index];
    if (!li) return;
    li.dataset.state = status;
    this.log(`${status === 'running' ? '…' : status === 'done' ? '✓' : '✗'} ${li.textContent}`);
  },
  // esptool-js's own lines plus step changes: what to paste into an issue when something stalls.
  log(line) {
    const pre = $('progress-log');
    pre.textContent += `${line.trimEnd()}\n`;
    pre.scrollTop = pre.scrollHeight;
  },
  percent(value) {
    const clamped = Math.max(0, Math.min(100, value));
    $('meter-fill').style.transform = `scaleX(${clamped / 100})`;
    document.querySelector('.meter').setAttribute('aria-valuenow', String(Math.round(clamped)));
  },
  detail(text) {
    $('progress-detail').textContent = text;
  },
  done(title) {
    $('progress-title').textContent = title;
    this.percent(100);
    this.detail('');
  },
  fail(err, note) {
    $('progress-title').textContent = 'This needs attention';
    $('progress-error-note').textContent = note;
    $('progress-error-text').textContent = err?.message || String(err);
    $('progress-error').hidden = false;
    this.log(`Error: ${err?.message || err}`);
  },
};

function onFlashProgress(label, current, total) {
  const pct = total ? (current / total) * 100 : 0;
  progress.percent(pct);
  progress.detail(`${label}… ${Math.floor(pct)}%`);
}

function flasherFor(port) {
  return new X4Flasher(port, { log: (line) => progress.log(line) });
}

// --- Running guard ---

function preventLeave(event) {
  event.preventDefault();
}

// Every control is locked while the USB session runs; afterwards each one gets its own rule back.
function setRunning(running) {
  state.running = running;
  document.querySelectorAll('.steps button, .steps input, .recovery button, .recovery input, #retry').forEach((el) => {
    el.disabled = running;
  });
  if (running) {
    window.addEventListener('beforeunload', preventLeave);
    return;
  }
  window.removeEventListener('beforeunload', preventLeave);
  updateRecoveryTools();
  updateInstallStep();
}

function updateRecoveryTools() {
  $('rollback').disabled = !serialSupported || state.running;
  $('backup').disabled = !serialSupported || state.running;
  $('restore').disabled = !serialSupported || state.running || !state.restoreFile || !$('restore-ack').checked;
}

// --- Port selection: must be the first await in a click handler ---

async function choosePort(trigger) {
  const issue = $('connection-issue');
  issue.hidden = true;
  try {
    return await X4Flasher.requestPort();
  } catch (err) {
    // Show the problem next to the button that was pressed.
    trigger.closest('.step, .tool').append(issue);
    issue.hidden = false;
    issue.textContent =
      err?.name === 'NotFoundError'
        ? 'No reader was selected. If the X4 was not in the list: use a data cable, try another USB port, and close other flashing tools.'
        : `The USB connection could not be opened. Close other apps or tabs using the reader, then try again. (${err?.message || err})`;
    $('not-listed').open = err?.name === 'NotFoundError';
    return null;
  }
}

async function run(last, title, steps, work) {
  state.last = last;
  setRunning(true);
  progress.begin(title, steps);
  try {
    await work();
    return true;
  } catch (err) {
    progress.fail(err, last.note);
    return false;
  } finally {
    setRunning(false);
  }
}

// --- Firmware download, verified against the manifest ---

async function downloadBuild(build) {
  const response = await fetch(build.path, { cache: 'no-cache' });
  if (!response.ok) throw new Error(`Could not download the firmware (HTTP ${response.status}).`);
  const data = new Uint8Array(build.size);
  const reader = response.body.getReader();
  let received = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    if (received + value.length > data.length) throw new Error('The download is larger than the published build.');
    data.set(value, received);
    received += value.length;
    onFlashProgress('Downloading', received, build.size);
  }
  if (received !== build.size) throw new Error(`The download is incomplete (${received} of ${build.size} bytes).`);
  if ((await sha256Hex(data)) !== build.sha256) throw new Error('The download does not match the published SHA-256.');
  return data;
}

// --- Actions ---

// Each action receives the button that started it: the port picker needs that click's user
// gesture, and connection problems are shown next to it. Retry replays the last action.
async function install(trigger) {
  if (state.running) return;
  const port = await choosePort(trigger);
  if (!port) return;

  const build = selectedBuild();
  const steps = [build ? 'Download and verify firmware' : 'Read firmware file', ...X4Flasher.installSteps()];
  const title = `Installing ${build ? build.name : state.customFile.name}…`;

  const note = 'If it stopped before “Switch boot partition”, the reader still boots its previous firmware.';
  const ok = await run({ action: install, trigger, note }, title, steps, async () => {
    progress.step(0, 'running');
    let firmware;
    try {
      firmware = build ? await downloadBuild(build) : new Uint8Array(await state.customFile.arrayBuffer());
    } catch (err) {
      progress.step(0, 'error');
      throw err;
    }
    progress.step(0, 'done');
    await flasherFor(port).install(firmware, {
      onStep: (index, _name, status) => progress.step(index + 1, status),
      onProgress: onFlashProgress,
    });
    progress.done('Installed');
  });

  if (ok) {
    $('restart').hidden = false;
    $('confirm-row').hidden = false;
    $('welcome').hidden = true;
    $('restart').scrollIntoView({ behavior: 'smooth', block: 'nearest' });
  }
}

async function rollback(trigger) {
  if (state.running) return;
  const port = await choosePort(trigger);
  if (!port) return;
  const note = 'The reader still boots the firmware it booted before.';
  await run({ action: rollback, trigger, note }, 'Rolling back…', X4Flasher.rollbackSteps(), async () => {
    const result = await flasherFor(port).rollback({
      onStep: (index, _name, status) => progress.step(index, status),
      onProgress: onFlashProgress,
    });
    progress.done(`Rolled back to ${result.slot}. The reader is restarting.`);
  });
}

async function backup(trigger) {
  if (state.running) return;
  const port = await choosePort(trigger);
  if (!port) return;
  const note = 'Reading flash never changes the reader.';
  await run({ action: backup, trigger, note }, 'Backing up 16 MB of flash…', X4Flasher.backupSteps(), async () => {
    const image = await flasherFor(port).backup({
      onStep: (index, _name, status) => progress.step(index, status),
      onProgress: onFlashProgress,
    });
    const stamp = new Date().toISOString().slice(0, 10).replaceAll('-', '');
    const url = URL.createObjectURL(new Blob([image], { type: 'application/octet-stream' }));
    Object.assign(document.createElement('a'), { href: url, download: `x4-flash-backup-${stamp}.bin` }).click();
    setTimeout(() => URL.revokeObjectURL(url), 60_000);
    progress.done('Backup saved. Keep it somewhere safe.');
  });
}

async function restore(trigger) {
  if (state.running || !state.restoreFile || !$('restore-ack').checked) return;
  const port = await choosePort(trigger);
  if (!port) return;
  const note = 'If writing started, restore again before restarting the reader.';
  await run({ action: restore, trigger, note }, 'Restoring backup…', X4Flasher.restoreSteps(), async () => {
    const image = new Uint8Array(await state.restoreFile.arrayBuffer());
    await flasherFor(port).restore(image, {
      onStep: (index, _name, status) => progress.step(index, status),
      onProgress: onFlashProgress,
    });
    progress.done('Backup restored. The reader is restarting.');
  });
}

// --- Wiring ---

function init() {
  $('no-serial').hidden = serialSupported;

  document.querySelector('.steps').addEventListener('click', (event) => {
    const choice = event.target.closest('[data-fw]');
    if (choice) selectFirmware(choice.dataset.fw);
  });
  $('custom-file').addEventListener('change', (event) => {
    state.customFile = event.target.files[0] ?? null;
    $('custom-file-name').textContent = state.customFile ? state.customFile.name : 'Choose firmware.bin';
    updateInstallStep();
  });
  $('ack').addEventListener('change', updateInstallStep);
  $('install').addEventListener('click', (event) => install(event.currentTarget));
  $('rollback').addEventListener('click', (event) => rollback(event.currentTarget));
  $('backup').addEventListener('click', (event) => backup(event.currentTarget));
  $('restore').addEventListener('click', (event) => restore(event.currentTarget));
  $('restore-file').addEventListener('change', (event) => {
    state.restoreFile = event.target.files[0] ?? null;
    $('restore-file-name').textContent = state.restoreFile ? state.restoreFile.name : 'Choose backup .bin';
    updateRecoveryTools();
  });
  $('restore-ack').addEventListener('change', updateRecoveryTools);
  $('retry').addEventListener('click', () => state.last?.action(state.last.trigger));
  $('confirm-started').addEventListener('click', () => {
    $('confirm-row').hidden = true;
    $('welcome').hidden = false;
  });

  updateRecoveryTools();
  loadManifest().then(updateInstallStep);
}

init();

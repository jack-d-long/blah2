// Control page: pause/resume processing and edit operating parameters.

var host = window.location.hostname;
var api = is_localhost(host) ? 'http://' + host + ':3000' : '';
var C = 299792458;
var R820T_GAINS = [0.0, 0.9, 1.4, 2.7, 3.7, 7.7, 8.7, 12.5, 14.4, 15.7, 16.6,
  19.7, 20.7, 22.9, 25.4, 28.0, 29.7, 32.8, 33.8, 36.4, 37.2, 38.6, 40.2, 42.1,
  43.4, 43.9, 44.5, 48.0, 49.6];

var loaded = null;   // params as loaded from the API
var paused = false;
var restarting = false;

function $id(id) { return document.getElementById(id); }

function bin_m(fs) { return C / fs; }

function show(text, kind) {
  $id('msg').innerHTML = text ?
    '<div class="alert alert-' + (kind || 'secondary') + ' py-2 mb-0">' + text + '</div>' : '';
}

function request(method, path, body) {
  return fetch(api + path, { method: method, body: body, cache: 'no-store' })
    .then(function (res) {
      return res.json().then(function (data) {
        if (!res.ok) { throw new Error(data.error || res.statusText); }
        return data;
      });
    });
}

// --- parameters -----------------------------------------------------------

function fill_select(el, values, format, current) {
  if (current !== undefined && values.indexOf(current) < 0) {
    values = values.concat([current]).sort(function (a, b) { return a - b; });
  }
  el.innerHTML = values.map(function (v) {
    return '<option value="' + v + '">' + format(v) + '</option>';
  }).join('');
}

function current_fs() { return parseInt($id('fs').value, 10); }

function to_form(p, limits) {
  var fs = p['capture.fs'];
  $id('fc').value = (p['capture.fc'] / 1e6).toFixed(3);
  fill_select($id('fs'), limits['capture.fs'].values,
    function (v) { return (v / 1e6).toFixed(3); }, fs);
  $id('fs').value = fs;
  $id('cpi').value = p['process.data.cpi'];

  var hasGain = 'capture.device.gain' in p;
  document.querySelectorAll('.gain-field').forEach(function (el) {
    el.style.display = hasGain ? '' : 'none';
  });
  if (hasGain) {
    var gain = p['capture.device.gain'];
    [0, 1].forEach(function (i) {
      fill_select($id('gain' + i), R820T_GAINS, function (v) { return v.toFixed(1); }, gain[i]);
      $id('gain' + i).value = gain[i];
    });
  }

  $id('rangeMax').value = (p['process.ambiguity.delayMax'] * bin_m(fs) / 1000).toFixed(2);
  $id('delayMin').value = p['process.ambiguity.delayMin'];
  $id('dopplerMin').value = p['process.ambiguity.dopplerMin'];
  $id('dopplerMax').value = p['process.ambiguity.dopplerMax'];
  $id('clutterEnable').checked = p['process.clutter.enable'];
  $id('clutterRangeMax').value = (p['process.clutter.delayMax'] * bin_m(fs) / 1000).toFixed(2);
  $id('clutterDelayMin').value = p['process.clutter.delayMin'];
  $id('detectionEnable').checked = p['process.detection.enable'];
  $id('trackerEnable').checked = p['process.tracker.enable'];
  $id('pfa').value = p['process.detection.pfa'];
}

function from_form() {
  var fs = current_fs();
  var km_to_bins = function (km) { return Math.round(km * 1000 / bin_m(fs)); };
  var p = {
    'capture.fc': Math.round(parseFloat($id('fc').value) * 1e6),
    'capture.fs': fs,
    'process.data.cpi': parseFloat($id('cpi').value),
    'process.ambiguity.delayMax': km_to_bins(parseFloat($id('rangeMax').value)),
    'process.ambiguity.delayMin': parseInt($id('delayMin').value, 10),
    'process.ambiguity.dopplerMin': parseInt($id('dopplerMin').value, 10),
    'process.ambiguity.dopplerMax': parseInt($id('dopplerMax').value, 10),
    'process.clutter.enable': $id('clutterEnable').checked,
    'process.clutter.delayMax': km_to_bins(parseFloat($id('clutterRangeMax').value)),
    'process.clutter.delayMin': parseInt($id('clutterDelayMin').value, 10),
    'process.detection.enable': $id('detectionEnable').checked,
    'process.tracker.enable': $id('trackerEnable').checked,
    'process.detection.pfa': parseFloat($id('pfa').value)
  };
  if (loaded && 'capture.device.gain' in loaded) {
    p['capture.device.gain'] = [parseFloat($id('gain0').value), parseFloat($id('gain1').value)];
  }
  return p;
}

function changes() {
  if (!loaded) { return {}; }
  var p = from_form();
  var out = {};
  Object.keys(p).forEach(function (k) {
    if (JSON.stringify(p[k]) !== JSON.stringify(loaded[k])) { out[k] = p[k]; }
  });
  return out;
}

function update_hints() {
  var fs = current_fs();
  var p = from_form();
  var res = bin_m(fs);
  $id('rangeMax-hint').textContent = p['process.ambiguity.delayMax'] + ' bins × ' + res.toFixed(1) + ' m';
  $id('clutterRangeMax-hint').textContent = p['process.clutter.delayMax'] + ' bins';
  $id('delayMin-hint').textContent = (p['process.ambiguity.delayMin'] * res / 1000).toFixed(2) + ' km';
  $id('clutterDelayMin-hint').textContent = (p['process.clutter.delayMin'] * res / 1000).toFixed(2) + ' km';

  // highlight edited fields
  var changed = changes();
  var inputs = {
    'capture.fc': ['fc'], 'capture.fs': ['fs'], 'process.data.cpi': ['cpi'],
    'capture.device.gain': ['gain0', 'gain1'],
    'process.ambiguity.delayMax': ['rangeMax'], 'process.ambiguity.delayMin': ['delayMin'],
    'process.ambiguity.dopplerMin': ['dopplerMin'], 'process.ambiguity.dopplerMax': ['dopplerMax'],
    'process.clutter.enable': ['clutterEnable'], 'process.clutter.delayMax': ['clutterRangeMax'],
    'process.clutter.delayMin': ['clutterDelayMin'], 'process.detection.enable': ['detectionEnable'],
    'process.tracker.enable': ['trackerEnable'], 'process.detection.pfa': ['pfa']
  };
  Object.keys(inputs).forEach(function (k) {
    inputs[k].forEach(function (id) { $id(id).classList.toggle('changed', k in changed); });
  });
  var n = Object.keys(changed).length;
  $id('btn-apply').disabled = n === 0;
  $id('btn-discard').disabled = n === 0;
}

function load_params() {
  return request('GET', '/control/params').then(function (data) {
    loaded = data.params;
    to_form(data.params, data.limits);
    document.querySelectorAll('[data-key]').forEach(function (el) {
      el.classList.toggle('overridden', data.overridden.indexOf(el.dataset.key) >= 0);
    });
    update_hints();
  });
}

// --- status ---------------------------------------------------------------

function format_uptime(s) {
  var h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60);
  return h > 0 ? h + ' h ' + m + ' min' : m + ' min ' + Math.floor(s % 60) + ' s';
}

function poll_state() {
  request('GET', '/control/state').then(function (st) {
    paused = st.paused;
    var dev = st.device;
    var alive = dev && st.deviceAge < 5;
    if (restarting && alive && dev.time > restarting) { restarting = false; }

    var runDot = $id('run-dot');
    if (!alive) {
      $id('run-state').textContent = restarting ? 'Restarting blah2…' : 'blah2 not reporting';
      runDot.className = 'dot ' + (restarting ? 'warn' : 'bad');
    } else {
      $id('run-state').textContent = paused ? 'Processing paused' : 'Processing running';
      runDot.className = 'dot ' + (paused ? 'warn' : 'ok');
    }
    $id('btn-resync').disabled = !alive;
    var btn = $id('btn-pause');
    btn.disabled = false;
    btn.textContent = paused ? 'Resume processing' : 'Pause processing';

    if (dev && dev.state) {
      $id('st-sync').textContent = alive ? dev.state : 'unknown';
      $id('sync-dot').className = 'dot ' + (!alive ? '' : dev.state === 'aligned' ? 'ok' : dev.state.indexOf('waiting') === 0 ? 'bad' : 'warn');
      $id('st-offset').textContent = dev.offset.toFixed(2) + ' / ' + dev.peakRatio.toFixed(0);
      var age = dev.time ? Math.round((Date.now() - dev.time) / 1000) + ' s ago' : 'never';
      $id('st-detail').textContent = 'Last sync check ' + age + ' · acquires ' + dev.nAcquire +
        ' · samples received ' + dev.received[0] + ' / ' + dev.received[1] +
        ' · config ' + st.version;
    }
  }).catch(function () {
    $id('run-state').textContent = 'API not reachable';
    $id('run-dot').className = 'dot bad';
    $id('btn-pause').disabled = true;
    $id('btn-resync').disabled = true;
  });
}

function poll_timing() {
  request('GET', '/api/timing').then(function (t) {
    if (!loaded || t.cpi === undefined) { return; }
    var budget = loaded['process.data.cpi'] * 1000;
    var slow = t.cpi > budget;
    $id('st-cpi').innerHTML = Math.round(t.cpi) + ' / ' + budget + ' ms' +
      (slow ? ' <span class="hint">(behind)</span>' : '');
    $id('st-uptime').textContent = format_uptime(t.uptime_s);
  }).catch(function () {});
}

// --- actions --------------------------------------------------------------

$id('btn-pause').addEventListener('click', function () {
  this.disabled = true;
  request('POST', paused ? '/control/resume' : '/control/pause')
    .then(poll_state)
    .catch(function (e) { show('Failed: ' + e.message, 'danger'); });
});

$id('btn-resync').addEventListener('click', function () {
  this.disabled = true;
  request('POST', '/control/resync')
    .then(function () { show('Re-sync requested; the channels re-align within a second or two.', 'success'); })
    .catch(function (e) { show('Failed: ' + e.message, 'danger'); });
});

$id('params').addEventListener('input', update_hints);
$id('params').addEventListener('change', update_hints);

$id('params').addEventListener('submit', function (ev) {
  ev.preventDefault();
  var c = changes();
  if (Object.keys(c).length === 0) { return; }
  $id('btn-apply').disabled = true;
  request('POST', '/control/params', JSON.stringify(c)).then(function () {
    restarting = Date.now();
    show('Saved. blah2 is restarting with the new parameters.', 'success');
    return load_params();
  }).catch(function (e) {
    show('Not applied: ' + e.message, 'danger');
    update_hints();
  });
});

$id('btn-discard').addEventListener('click', function () {
  load_params();
  show('');
});

$id('btn-reset').addEventListener('click', function () {
  if (!confirm('Remove all overrides and restart blah2 with the base config?')) { return; }
  request('POST', '/control/reset').then(function () {
    restarting = Date.now();
    show('Reverted to base config. blah2 is restarting.', 'success');
    return load_params();
  }).catch(function (e) { show('Failed: ' + e.message, 'danger'); });
});

load_params().catch(function (e) { show('Could not load parameters: ' + e.message, 'danger'); });
poll_state();
poll_timing();
setInterval(poll_state, 1000);
setInterval(poll_timing, 2000);

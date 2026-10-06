// Control API: pause/resume processing and edit a whitelist of
// operating parameters through an override file. blah2 polls
// /control/state, pauses on request, and exits to be restarted
// by docker when the override version changes.

const fs = require('fs');
const path = require('path');
const yaml = require('js-yaml');
const crypto = require('crypto');
const express = require('express');

// editable parameters (paths into config) and their limits
const PARAMS = {
  'capture.fc': { type: 'int', min: 24000000, max: 1766000000 },
  'capture.fs': { type: 'int', values: [1024000, 1536000, 1800000, 2048000, 2400000] },
  'capture.device.gain': { type: 'pair', min: 0, max: 49.6, device: 'RtlSdr' },
  'process.data.cpi': { type: 'num', min: 0.1, max: 2 },
  'process.ambiguity.delayMin': { type: 'int', min: -200, max: 0 },
  'process.ambiguity.delayMax': { type: 'int', min: 1, max: 2000 },
  'process.ambiguity.dopplerMin': { type: 'int', min: -2000, max: 0 },
  'process.ambiguity.dopplerMax': { type: 'int', min: 0, max: 2000 },
  'process.clutter.enable': { type: 'bool' },
  'process.clutter.delayMin': { type: 'int', min: -200, max: 0 },
  'process.clutter.delayMax': { type: 'int', min: 1, max: 2000 },
  'process.detection.enable': { type: 'bool' },
  'process.detection.pfa': { type: 'num', min: 1e-12, max: 0.1 },
  'process.tracker.enable': { type: 'bool' }
};

function get(obj, key) {
  return key.split('.').reduce((o, k) => (o == null ? undefined : o[k]), obj);
}

function set(obj, key, value) {
  const keys = key.split('.');
  let o = obj;
  for (const k of keys.slice(0, -1)) {
    if (typeof o[k] !== 'object' || o[k] === null || Array.isArray(o[k])) {
      o[k] = {};
    }
    o = o[k];
  }
  o[keys[keys.length - 1]] = value;
}

// merge maps, replace everything else (matches blah2 overlay)
function merge(dst, src) {
  for (const k of Object.keys(src)) {
    const isMap = (v) => typeof v === 'object' && v !== null && !Array.isArray(v);
    if (isMap(src[k]) && isMap(dst[k])) {
      merge(dst[k], src[k]);
    } else {
      dst[k] = src[k];
    }
  }
  return dst;
}

function validate(key, value) {
  const p = PARAMS[key];
  const inRange = (v) => typeof v === 'number' && isFinite(v) &&
    (p.min === undefined || v >= p.min) && (p.max === undefined || v <= p.max);
  switch (p.type) {
    case 'bool':
      return typeof value === 'boolean';
    case 'int':
      if (p.values) {
        return p.values.includes(value);
      }
      return Number.isInteger(value) && inRange(value);
    case 'num':
      return inRange(value);
    case 'pair':
      return Array.isArray(value) && value.length === 2 && value.every(inRange);
  }
  return false;
}

module.exports = function (app, config, configFile, overrideFile) {
  // start paused, processing only runs once resumed from the control page
  let paused = true;
  let resync = 0;
  let device = null;
  let deviceTime = 0;

  function read_override() {
    try {
      return yaml.load(fs.readFileSync(overrideFile, 'utf8')) || {};
    } catch (e) {
      return {};
    }
  }

  function version() {
    let text = '';
    try {
      text = fs.readFileSync(overrideFile, 'utf8');
    } catch (e) {}
    return crypto.createHash('sha1').update(text).digest('hex').slice(0, 12);
  }

  // replace config contents in place (server.js holds the reference)
  function reload() {
    const base = yaml.load(fs.readFileSync(configFile, 'utf8'));
    for (const k of Object.keys(config)) {
      delete config[k];
    }
    merge(config, merge(base, read_override()));
  }

  function params() {
    const out = {};
    const type = get(config, 'capture.device.type');
    for (const key of Object.keys(PARAMS)) {
      if (PARAMS[key].device && PARAMS[key].device !== type) {
        continue;
      }
      out[key] = get(config, key);
    }
    return out;
  }

  function state() {
    return {
      paused: paused,
      resync: resync,
      version: version(),
      device: device,
      deviceAge: device ? (Date.now() - deviceTime) / 1000 : null
    };
  }

  reload();
  if (Object.keys(read_override()).length > 0) {
    console.log('Applied config override: ' + overrideFile);
  }

  const text = express.text({ type: '*/*' });

  app.get('/control/state', (req, res) => {
    res.json(state());
  });

  app.post('/control/pause', (req, res) => {
    paused = true;
    res.json(state());
  });

  app.post('/control/resume', (req, res) => {
    paused = false;
    res.json(state());
  });

  // blah2 re-aligns channels when this count changes
  app.post('/control/resync', (req, res) => {
    resync++;
    res.json(state());
  });

  app.post('/control/device', text, (req, res) => {
    try {
      device = JSON.parse(req.body);
      deviceTime = Date.now();
      res.json({});
    } catch (e) {
      res.status(400).json({ error: 'Invalid JSON.' });
    }
  });

  app.get('/control/params', (req, res) => {
    const override = read_override();
    const overridden = Object.keys(PARAMS).filter((k) => get(override, k) !== undefined);
    res.json({ params: params(), overridden: overridden, limits: PARAMS });
  });

  // body: JSON object of {path: value}
  app.post('/control/params', text, (req, res) => {
    let changes;
    try {
      changes = JSON.parse(req.body);
    } catch (e) {
      return res.status(400).json({ error: 'Invalid JSON.' });
    }
    const allowed = params();
    const override = read_override();
    for (const key of Object.keys(changes)) {
      if (!(key in allowed)) {
        return res.status(400).json({ error: 'Parameter not editable: ' + key });
      }
      if (!validate(key, changes[key])) {
        return res.status(400).json({ error: 'Invalid value for ' + key + '.' });
      }
      set(override, key, changes[key]);
    }

    // check combined result
    const result = merge(yaml.load(fs.readFileSync(configFile, 'utf8')), override);
    const pairs = [
      ['process.ambiguity.delayMin', 'process.ambiguity.delayMax'],
      ['process.ambiguity.dopplerMin', 'process.ambiguity.dopplerMax'],
      ['process.clutter.delayMin', 'process.clutter.delayMax']
    ];
    for (const [lo, hi] of pairs) {
      if (get(result, lo) >= get(result, hi)) {
        return res.status(400).json({ error: lo + ' must be less than ' + hi + '.' });
      }
    }

    // write atomically, world-writable so it can be edited on the host
    const header = '# Written by the blah2 control page. Overlays ' +
      path.basename(configFile) + '.\n# Delete this file to revert to the base config.\n';
    const tmp = overrideFile + '.tmp';
    fs.writeFileSync(tmp, header + yaml.dump(override));
    fs.chmodSync(tmp, 0o666);
    fs.renameSync(tmp, overrideFile);
    reload();
    console.log('Config override updated: ' + JSON.stringify(changes));
    res.json(state());
  });

  // remove override file, revert to base config
  app.post('/control/reset', (req, res) => {
    try {
      fs.unlinkSync(overrideFile);
    } catch (e) {}
    reload();
    console.log('Config override removed.');
    res.json(state());
  });
};

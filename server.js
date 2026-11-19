import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.dirname(fileURLToPath(import.meta.url));

function readEnvFile() {
  const file = path.join(ROOT, '.env.local');
  if (!fs.existsSync(file)) return {};
  const values = {};
  for (const line of fs.readFileSync(file, 'utf8').split(/\r?\n/)) {
    const match = line.match(/^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*?)\s*$/);
    if (!match || match[1].startsWith('#')) continue;
    let value = match[2];
    if ((value.startsWith('"') && value.endsWith('"')) || (value.startsWith("'") && value.endsWith("'"))) {
      value = value.slice(1, -1);
    }
    values[match[1]] = value;
  }
  return values;
}

const fileEnv = readEnvFile();
const config = {
  haUrl: (process.env.HA_URL || fileEnv.HA_URL || '').replace(/\/+$/, ''),
  token: process.env.HA_TOKEN || fileEnv.HA_TOKEN || '',
  entityId: process.env.HA_ENTITY_ID || fileEnv.HA_ENTITY_ID || '',
  port: Number(process.env.PORT || fileEnv.PORT || 8787),
};

if (!config.haUrl || !config.token || !config.entityId || config.entityId === 'climate.replace_me') {
  console.error('Configure HA_URL, HA_TOKEN, and HA_ENTITY_ID in .env.local (see .env.example).');
  process.exit(1);
}
if (!/^https?:\/\//i.test(config.haUrl) || !/^climate\.[a-z0-9_]+$/i.test(config.entityId)) {
  console.error('HA_URL must be an http(s) URL and HA_ENTITY_ID must be a climate entity ID.');
  process.exit(1);
}

async function haRequest(apiPath, options = {}) {
  const response = await fetch(`${config.haUrl}${apiPath}`, {
    ...options,
    headers: {
      Authorization: `Bearer ${config.token}`,
      'Content-Type': 'application/json',
      ...options.headers,
    },
    signal: AbortSignal.timeout(8000),
  });
  const text = await response.text();
  let body;
  try { body = text ? JSON.parse(text) : null; } catch { body = text; }
  if (!response.ok) {
    const message = response.status === 401
      ? 'Home Assistant rejected the token. Create a fresh long-lived access token.'
      : (typeof body === 'string' ? body : body?.message) || `Home Assistant returned HTTP ${response.status}.`;
    const error = new Error(message);
    error.status = response.status;
    throw error;
  }
  return body;
}

async function getClimateState() {
  const state = await haRequest(`/api/states/${encodeURIComponent(config.entityId)}`);
  const attrs = state.attributes || {};
  return {
    entity_id: state.entity_id,
    friendly_name: attrs.friendly_name || state.entity_id,
    state: state.state,
    target_temperature: attrs.temperature ?? null,
    current_temperature: attrs.current_temperature ?? null,
    hvac_action: attrs.hvac_action ?? null,
    hvac_modes: Array.isArray(attrs.hvac_modes) ? attrs.hvac_modes : [],
    battery: attrs.battery_level ?? attrs.battery ?? null,
    valve_position: attrs.valve_opening_degree ?? attrs.valve_position ?? null,
    fault: attrs.fault ?? null,
  };
}

function sendJson(res, status, body) {
  res.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Cache-Control': 'no-store',
    'X-Content-Type-Options': 'nosniff',
  });
  res.end(JSON.stringify(body));
}

async function readJson(req) {
  let raw = '';
  for await (const chunk of req) {
    raw += chunk;
    if (raw.length > 8192) throw new Error('Request body is too large.');
  }
  return JSON.parse(raw || '{}');
}

async function controlDevice(req, res) {
  const input = await readJson(req);
  const hasTemp = Object.hasOwn(input, 'temperature');
  const hasMode = Object.hasOwn(input, 'mode');
  if (!hasTemp && !hasMode) return sendJson(res, 400, { error: 'Provide a temperature or mode.' });

  const current = await getClimateState();
  let mode;
  if (hasMode) {
    mode = String(input.mode).toLowerCase();
    if (!current.hvac_modes.includes(mode)) {
      return sendJson(res, 400, { error: `This device does not report ${mode} as a supported mode.`, hvac_modes: current.hvac_modes });
    }
  }
  let value;
  if (hasTemp) {
    value = Number(input.temperature);
    if (!Number.isFinite(value) || value < 5 || value > 30 || Math.abs(value * 2 - Math.round(value * 2)) > 1e-8) {
      return sendJson(res, 400, { error: 'Temperature must be between 5 and 30 °C in 0.5 °C steps.' });
    }
  }
  if (hasMode) {
    await haRequest('/api/services/climate/set_hvac_mode', {
      method: 'POST',
      body: JSON.stringify({ entity_id: config.entityId, hvac_mode: mode }),
    });
  }
  if (hasTemp) {
    await haRequest('/api/services/climate/set_temperature', {
      method: 'POST',
      body: JSON.stringify({ entity_id: config.entityId, temperature: value }),
    });
  }
  return sendJson(res, 200, { ok: true, state: await getClimateState() });
}

const server = http.createServer(async (req, res) => {
  try {
    const host = String(req.headers.host || '').toLowerCase();
    const allowedHosts = new Set([
      `127.0.0.1:${config.port}`,
      `localhost:${config.port}`,
      `[::1]:${config.port}`,
    ]);
    if (!allowedHosts.has(host)) return sendJson(res, 403, { error: 'This local service only accepts loopback requests.' });
    const origin = req.headers.origin;
    if (origin && ![`http://127.0.0.1:${config.port}`, `http://localhost:${config.port}`, `http://[::1]:${config.port}`].includes(origin)) {
      return sendJson(res, 403, { error: 'Cross-origin requests are not allowed.' });
    }
    const url = new URL(req.url, 'http://127.0.0.1');
    if (req.method === 'GET' && url.pathname === '/api/state') {
      return sendJson(res, 200, await getClimateState());
    }
    if (req.method === 'POST' && url.pathname === '/api/control') {
      if (!req.headers['content-type']?.includes('application/json')) {
        return sendJson(res, 415, { error: 'Send controls as application/json.' });
      }
      return await controlDevice(req, res);
    }
    if (req.method === 'GET' && (url.pathname === '/' || url.pathname === '/index.html')) {
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
      return fs.createReadStream(path.join(ROOT, 'index.html')).pipe(res);
    }
    if (req.method === 'GET' && url.pathname === '/favicon.ico') {
      res.writeHead(204).end();
      return;
    }
    sendJson(res, 404, { error: 'Not found.' });
  } catch (error) {
    if (!res.headersSent) sendJson(res, error.status || 502, { error: error.message || 'Connector error.' });
    else res.destroy();
  }
});

server.listen(config.port, '127.0.0.1', () => {
  console.log(`TRV dashboard available at http://127.0.0.1:${config.port}`);
  console.log(`Using Home Assistant entity ${config.entityId}. Do not share this computer's local service or .env.local.`);
});


const REGIONS = {
  eu: 'https://eu-apia.coolkit.cc',
  us: 'https://us-apia.coolkit.cc',
  as: 'https://as-apia.coolkit.cc',
  cn: 'https://cn-apia.coolkit.cn',
};

const json = (data, status = 200, headers = {}) => new Response(JSON.stringify(data), {
  status,
  headers: { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff', ...headers },
});

const encoder = new TextEncoder();
const b64url = bytes => btoa(String.fromCharCode(...new Uint8Array(bytes))).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/g, '');
const fromB64url = value => Uint8Array.from(atob(value.replace(/-/g, '+').replace(/_/g, '/') + '='.repeat((4 - value.length % 4) % 4)), c => c.charCodeAt(0));
function constantTimeEqual(left, right) {
  const a = encoder.encode(left), b = encoder.encode(right);
  let diff = a.length ^ b.length;
  for (let i = 0; i < Math.max(a.length, b.length); i++) diff |= (a[i % (a.length || 1)] || 0) ^ (b[i % (b.length || 1)] || 0);
  return diff === 0;
}

async function hmac(secret, value) {
  const key = await crypto.subtle.importKey('raw', encoder.encode(secret), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
  return new Uint8Array(await crypto.subtle.sign('HMAC', key, encoder.encode(value)));
}

async function sign(secret, body) {
  const key = await crypto.subtle.importKey('raw', encoder.encode(secret), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
  return btoa(String.fromCharCode(...new Uint8Array(await crypto.subtle.sign('HMAC', key, encoder.encode(body)))));
}

async function verifySession(request, env) {
  const token = (request.headers.get('Cookie') || '').split(';').map(v => v.trim()).find(v => v.startsWith('trv_session='))?.slice('trv_session='.length);
  if (!token || !env.DASHBOARD_PASSWORD) return false;
  const [payload, signature] = token.split('.');
  if (!payload || !signature) return false;
  let claims;
  try { claims = JSON.parse(new TextDecoder().decode(fromB64url(payload))); } catch { return false; }
  if (!Number.isFinite(claims?.exp) || claims.exp < Date.now()) return false;
  const expected = b64url(await hmac(env.DASHBOARD_PASSWORD, payload));
  return constantTimeEqual(expected, signature);
}

function cookie(request, value, maxAge) {
  const secure = new URL(request.url).protocol === 'https:' ? '; Secure' : '';
  return `trv_session=${value}; HttpOnly; SameSite=Strict; Path=/api; Max-Age=${maxAge}${secure}`;
}

function requireConfig(env) {
  const missing = ['DASHBOARD_PASSWORD', 'EWELINK_APP_ID', 'EWELINK_APP_SECRET', 'EWELINK_ACCOUNT', 'EWELINK_PASSWORD', 'EWELINK_KV'].filter(key => !env[key]);
  if (missing.length) throw new Error(`Cloudflare setup is incomplete: ${missing.join(', ')}.`);
}

async function callApi(env, region, path, { method = 'GET', body, at, signed = false } = {}) {
  const base = REGIONS[region];
  if (!base) throw new Error(`Unsupported eWeLink region: ${region}`);
  const rawBody = body === undefined ? '' : JSON.stringify(body);
  const headers = { Accept: 'application/json', 'X-CK-Appid': env.EWELINK_APP_ID, 'X-CK-Nonce': crypto.randomUUID().replaceAll('-', '').slice(0, 8) };
  if (body !== undefined) headers['Content-Type'] = 'application/json';
  if (signed) headers.Authorization = `Sign ${await sign(env.EWELINK_APP_SECRET, rawBody)}`;
  else headers.Authorization = `Bearer ${at}`;
  const url = new URL(path, base);
  const response = await fetch(url, { method, headers, body: body === undefined ? undefined : rawBody, signal: AbortSignal.timeout(12000) });
  const data = await response.json().catch(() => ({}));
  if (!response.ok || data.error) {
    const error = new Error(data.msg || `eWeLink returned HTTP ${response.status}.`);
    error.code = data.error;
    error.status = response.status;
    error.data = data.data || {};
    throw error;
  }
  return data.data || {};
}

async function login(env, region) {
  const account = env.EWELINK_ACCOUNT.trim();
  const countryCode = env.EWELINK_COUNTRY_CODE || '+31';
  const body = { countryCode, password: env.EWELINK_PASSWORD, lang: 'en' };
  if (account.includes('@')) body.email = account;
  else {
    const phone = account.replace(/[\s()-]/g, '');
    body.phoneNumber = phone.startsWith('+') ? phone
      : phone.startsWith(countryCode.slice(1)) ? `+${phone}`
      : `${countryCode}${phone.replace(/^0/, '')}`;
  }
  let result;
  try { result = await callApi(env, region, '/v2/user/login', { method: 'POST', body, signed: true }); }
  catch (error) {
    if (error.code !== 10004 || !REGIONS[error.data?.region]) throw error;
    region = error.data.region;
    result = await callApi(env, region, '/v2/user/login', { method: 'POST', body, signed: true });
  }
  if (result.region && result.region !== region && REGIONS[result.region]) {
    region = result.region;
    result = await callApi(env, region, '/v2/user/login', { method: 'POST', body, signed: true });
  }
  if (!result.at || !result.rt) throw new Error('eWeLink login did not return access and refresh tokens.');
  const auth = { at: result.at, rt: result.rt, region, expires: Date.now() + 25 * 24 * 60 * 60 * 1000 };
  await env.EWELINK_KV.put('ewelink_auth', JSON.stringify(auth));
  return auth;
}

async function getAuth(env, forceLogin = false) {
  const regionDefault = env.EWELINK_REGION || 'eu';
  let auth;
  if (!forceLogin) {
    try { auth = JSON.parse(await env.EWELINK_KV.get('ewelink_auth') || 'null'); } catch {}
  }
  if (!auth) return login(env, regionDefault);
  if (auth.expires > Date.now() + 60 * 60 * 1000) return auth;
  try {
    const body = { rt: auth.rt };
    const updated = await callApi(env, auth.region, '/v2/user/refresh', { method: 'POST', body, signed: true });
    if (!updated.at || !updated.rt) throw new Error('Token refresh response was incomplete.');
    auth = { ...auth, at: updated.at, rt: updated.rt, expires: Date.now() + 25 * 24 * 60 * 60 * 1000 };
    await env.EWELINK_KV.put('ewelink_auth', JSON.stringify(auth));
    return auth;
  } catch {
    return login(env, auth.region || regionDefault);
  }
}

async function apiWithAuth(env, path, options) {
  let auth = await getAuth(env);
  try { return await callApi(env, auth.region, path, { ...options, at: auth.at }); }
  catch (error) {
    if (![401, 402].includes(error.code) && error.status !== 401) throw error;
    auth = await getAuth(env, true);
    return callApi(env, auth.region, path, { ...options, at: auth.at });
  }
}

const numeric = value => {
  if (value === null || value === undefined || value === '') return null;
  const number = Number(value);
  if (!Number.isFinite(number)) return null;
  return number > 1000 ? number / 100 : number;
};
function first(params, names) { for (const name of names) if (params[name] !== undefined) return params[name]; return null; }
function normalizeDevice(item, params = {}) {
  const data = item?.itemData || item || {};
  const model = String(data.productModel || data.model || data.uiid || '');
  const name = String(data.name || data.deviceName || data.productName || model || 'SONOFF device');
  const flat = { ...data.params, ...params };
  const lower = `${model} ${name}`.toLowerCase();
  const kind = lower.includes('trv') || lower.includes('thermo') ? 'valve'
    : lower.includes('02p') || lower.includes('temperature') ? 'temperature'
    : lower.includes('04p') || lower.includes('door') || lower.includes('window') ? 'contact'
    : lower.includes('bridge') ? 'gateway' : 'device';
  const temp = numeric(first(flat, ['currentTemperature', 'temperature', 'temp']));
  const target = numeric(first(flat, ['targetTemperature', 'targetTemp', 'setTemperature', 'setTemp', 'manTargetTemp']));
  const humidity = numeric(first(flat, ['currentHumidity', 'humidity', 'humi']));
  const battery = numeric(first(flat, ['battery', 'batteryPercent', 'battery_level']));
  const contact = first(flat, ['contact', 'open', 'doorOpen', 'windowOpen', 'switch']);
  const tamper = first(flat, ['tamper', 'tamperAlarm', 'isTampered']);
  return {
    id: data.deviceid || data.id || data.subDevId || '', name, model, kind,
    online: data.online ?? data.onlineState ?? null,
    temperature: temp, target_temperature: target, humidity,
    battery, contact: contact === null ? null : (contact === true || contact === 1 || ['open', 'opened', 'on', 'true', '1'].includes(String(contact).toLowerCase())),
    tamper: tamper === null ? null : (tamper === true || tamper === 1 || ['on', 'true', '1', 'alarm'].includes(String(tamper).toLowerCase())),
    raw_params: flat,
  };
}

function configuredProfile(env, device) {
  let profiles = {};
  try { profiles = JSON.parse(env.EWELINK_CONTROL_FIELDS || '{}'); } catch { return {}; }
  return profiles[device.model] || profiles[String(device.raw_params.uiid || '')] || {};
}

function applyProfile(env, device) {
  const profile = configuredProfile(env, device);
  const read = field => {
    const key = typeof field === 'string' ? field : field?.key;
    return key ? device.raw_params[key] : undefined;
  };
  const field = profile.temperature;
  const temperatureKey = typeof field === 'string' ? field : field?.key;
  if (temperatureKey && device.raw_params[temperatureKey] !== undefined) device.target_temperature = numeric(device.raw_params[temperatureKey]);
  if (read(profile.currentTemperature) !== undefined) device.temperature = numeric(read(profile.currentTemperature));
  if (read(profile.humidity) !== undefined) device.humidity = numeric(read(profile.humidity));
  if (read(profile.battery) !== undefined) device.battery = numeric(read(profile.battery));
  if (read(profile.valvePosition) !== undefined) device.valve_position = numeric(read(profile.valvePosition));
  if (read(profile.fault) !== undefined) device.fault = read(profile.fault);
  if (read(profile.hvacAction) !== undefined) device.hvac_action = read(profile.hvacAction);
  const mode = profile.mode;
  if (mode?.key && mode.values && typeof mode.values === 'object') {
    const raw = device.raw_params[mode.key];
    const match = Object.entries(mode.values).find(([, value]) => String(value) === String(raw));
    if (match) device.state = match[0];
    device.hvac_modes = Object.keys(mode.values);
  }
  device.control_fields = profile;
  device.controls = ['temperature', 'mode'].filter(name => Object.hasOwn(profile, name));
  return device;
}

async function allDevices(env) {
  const list = await apiWithAuth(env, '/v2/device/thing?lang=en&num=0');
  const items = (list.thingList || []).filter(entry => [1, 2].includes(entry.itemType));
  const isProjectDevice = entry => /trv[- ]?zbt|snzb.?02p|snzb.?04pr2/i.test(`${entry.itemData?.productModel || ''} ${entry.itemData?.model || ''} ${entry.itemData?.name || ''} ${entry.itemData?.productName || ''}`);
  const recognized = items.filter(isProjectDevice);
  const statusTargets = new Set((recognized.length ? recognized : items).slice(0, 40));
  const devices = [];
  for (const entry of items) {
    const item = entry.itemData || {};
    const id = item.deviceid || item.id;
    if (!id) { devices.push(normalizeDevice(entry)); continue; }
    if (!statusTargets.has(entry)) { devices.push(normalizeDevice(entry)); continue; }
    try {
      const status = await apiWithAuth(env, `/v2/device/thing/status?type=1&id=${encodeURIComponent(id)}`);
      devices.push(applyProfile(env, normalizeDevice(entry, status.params || {})));
    } catch (error) {
      devices.push({ ...normalizeDevice(entry), status_error: error.message });
    }
    if (items.at(-1) !== entry) await new Promise(resolve => setTimeout(resolve, 550));
  }
  return devices;
}

function sameOrigin(request) {
  const origin = request.headers.get('Origin');
  return !origin || origin === new URL(request.url).origin;
}

export async function onRequest({ request, env }) {
  const route = new URL(request.url).pathname;
  try {
    if (request.method === 'OPTIONS') return new Response(null, { status: 204, headers: { 'Allow': 'GET,POST,OPTIONS', 'Cache-Control': 'no-store' } });
    if (!sameOrigin(request)) return json({ error: 'Cross-origin requests are not allowed.' }, 403);
    if (route === '/api/location' && request.method === 'POST') {
      const token = env.LOCATION_AUTOMATION_TOKEN;
      if (!token || token.length < 32) return json({ error: 'Location automation is not configured.' }, 503);
      const authorization = request.headers.get('Authorization') || '';
      const supplied = authorization.startsWith('Bearer ') ? authorization.slice(7) : '';
      if (!constantTimeEqual(supplied, token)) return json({ error: 'Unauthorized.' }, 401);
      const raw = await request.text();
      if (raw.length > 256) return json({ error: 'Request is too large.' }, 413);
      let input;
      try { input = JSON.parse(raw); } catch { return json({ error: 'Send a JSON action.' }, 400); }
      if (!['home', 'away'].includes(input?.action)) return json({ error: 'Action must be home or away.' }, 400);
      if (!env.EWELINK_KV) return json({ error: 'Location event storage is not configured.' }, 503);
      const event = { action: input.action, received_at: new Date().toISOString() };
      await env.EWELINK_KV.put('location_automation:last', JSON.stringify(event), { expirationTtl: 60 * 60 * 24 * 30 });
      const missing = ['EWELINK_APP_ID', 'EWELINK_APP_SECRET', 'EWELINK_ACCOUNT', 'EWELINK_PASSWORD', 'EWELINK_KV', 'EWELINK_TRV_DEVICE_ID', 'EWELINK_CONTROL_FIELDS'].filter(key => !env[key]);
      if (missing.length) return json({ error: `Location control setup is incomplete: ${missing.join(', ')}.` }, 503);
      const target = input.action === 'home' ? Number(env.LOCATION_HOME_TEMPERATURE) : null;
      if (input.action === 'home' && (!Number.isFinite(target) || target < 5 || target > 30 || Math.abs(target * 2 - Math.round(target * 2)) > 1e-8)) return json({ error: 'Set a valid home temperature (5–30 °C in 0.5 °C steps) in Worker variables.' }, 503);
      const list = await apiWithAuth(env, '/v2/device/thing?lang=en&num=0');
      const entry = (list.thingList || []).find(item => [1, 2].includes(item.itemType) && (item.itemData?.deviceid || item.itemData?.id) === env.EWELINK_TRV_DEVICE_ID);
      if (!entry) return json({ error: 'Configured TRV device was not found in this eWeLink account.' }, 404);
      const device = normalizeDevice(entry, entry.itemData?.params || {});
      if (device.kind !== 'valve') return json({ error: 'Configured device is not recognized as a TRV.' }, 409);
      const profile = configuredProfile(env, device);
      const params = {};
      if (input.action === 'away') {
        const mode = profile.mode;
        if (!mode?.key || !mode.values || mode.values.off === undefined) return json({ error: `TRV Off mode has not been verified for ${device.model}.` }, 409);
        params[mode.key] = mode.values.off;
      } else {
        const field = profile.temperature;
        const key = typeof field === 'string' ? field : field?.key;
        if (!key) return json({ error: `TRV temperature field has not been verified for ${device.model}.` }, 409);
        params[key] = typeof field === 'object' && field.scale ? target * Number(field.scale) : target;
      }
      await apiWithAuth(env, '/v2/device/thing/status', { method: 'POST', body: { type: 1, id: env.EWELINK_TRV_DEVICE_ID, params } });
      return json({ ok: true, action: input.action, ...(target === null ? { mode: 'off' } : { target_temperature: target }) });
    }
    if (route === '/api/location/status' && request.method === 'GET') {
      if (!await verifySession(request, env)) return json({ error: 'Sign in to the private dashboard first.' }, 401);
      if (!env.EWELINK_KV) return json({ error: 'Location event storage is not configured.' }, 503);
      let event = null;
      try { event = JSON.parse(await env.EWELINK_KV.get('location_automation:last') || 'null'); } catch {}
      return json({ event });
    }
    if (route === '/api/session' && request.method === 'POST') {
      if (!env.DASHBOARD_PASSWORD || !env.EWELINK_KV) return json({ error: 'Cloudflare setup is incomplete: DASHBOARD_PASSWORD and the EWELINK_KV binding are required.' }, 503);
      const ip = request.headers.get('CF-Connecting-IP') || 'unknown';
      const attemptKey = `login_attempts:${ip}`;
      const attempts = Number(await env.EWELINK_KV.get(attemptKey) || 0);
      if (attempts >= 8) return json({ error: 'Too many sign-in attempts. Wait 15 minutes and try again.' }, 429);
      const body = await request.json();
      if (typeof body.password !== 'string' || body.password.length > 256 || !constantTimeEqual(body.password, env.DASHBOARD_PASSWORD)) {
        await env.EWELINK_KV.put(attemptKey, String(attempts + 1), { expirationTtl: 900 });
        return json({ error: 'Dashboard password is incorrect.' }, 401);
      }
      await env.EWELINK_KV.delete(attemptKey);
      const payload = b64url(encoder.encode(JSON.stringify({ exp: Date.now() + 7 * 24 * 60 * 60 * 1000 })));
      const signature = b64url(await hmac(env.DASHBOARD_PASSWORD, payload));
      return json({ ok: true }, 200, { 'Set-Cookie': cookie(request, `${payload}.${signature}`, 7 * 24 * 60 * 60) });
    }
    if (route === '/api/session' && request.method === 'DELETE') return json({ ok: true }, 200, { 'Set-Cookie': cookie(request, '', 0) });
    if (!route.startsWith('/api/')) return json({ error: 'Not found.' }, 404);
    if (!await verifySession(request, env)) return json({ error: 'Sign in to the private dashboard first.' }, 401);
    requireConfig(env);
    if (request.method === 'GET' && route === '/api/state') {
      const devices = await allDevices(env);
      const valve = devices.find(d => d.kind === 'valve');
      return json({ provider: 'ewelink', updated_at: new Date().toISOString(), devices, valve: valve || null, history: [] });
    }
    if (request.method === 'POST' && route === '/api/control') {
      const input = await request.json();
      if (typeof input.deviceId !== 'string' || !input.deviceId) return json({ error: 'Send a deviceId.' }, 400);
      const list = await apiWithAuth(env, '/v2/device/thing?lang=en&num=0');
      const entry = (list.thingList || []).find(item => [1, 2].includes(item.itemType) && (item.itemData?.deviceid || item.itemData?.id) === input.deviceId);
      if (!entry) return json({ error: 'Device is not in this eWeLink account.' }, 404);
      const device = normalizeDevice(entry, entry.itemData?.params || {});
      const profile = configuredProfile(env, device);
      const params = {};
      if (Object.hasOwn(input, 'temperature')) {
        const field = profile.temperature;
        const key = typeof field === 'string' ? field : field?.key;
        const value = Number(input.temperature);
        if (!key) return json({ error: `TRV temperature control has not been configured from verified device fields for ${device.model}.` }, 409);
        if (!Number.isFinite(value) || value < 5 || value > 30 || Math.abs(value * 2 - Math.round(value * 2)) > 1e-8) return json({ error: 'Temperature must be 5–30 °C in 0.5 °C steps.' }, 400);
        params[key] = typeof field === 'object' && field.scale ? value * Number(field.scale) : value;
      }
      if (Object.hasOwn(input, 'mode')) {
        const mode = profile.mode;
        const value = mode?.values?.[String(input.mode)];
        if (!mode?.key || value === undefined) return json({ error: `TRV mode control has not been configured from verified device fields for ${device.model}.` }, 409);
        params[mode.key] = value;
      }
      if (!Object.keys(params).length) return json({ error: 'Provide a supported temperature or mode command.' }, 400);
      await apiWithAuth(env, '/v2/device/thing/status', { method: 'POST', body: { type: 1, id: input.deviceId, params } });
      return json({ ok: true });
    }
    return json({ error: 'Not found.' }, 404);
  } catch (error) {
    const message = error?.message || 'Cloud connector failed.';
    const status = /setup is incomplete/.test(message) ? 503 : 502;
    return json({ error: message }, status);
  }
}

'use strict';

const express      = require('express');
const { Pool }     = require('pg');
const { v4: uuidv4 } = require('uuid');
const crypto       = require('node:crypto');
const rateLimit    = require('express-rate-limit');
const session      = require('express-session');
const bcrypt       = require('bcryptjs');
const fs           = require('fs');
const path         = require('path');

// ─────────────────────────────────────────────────────────────────────────────
//  CONFIG  (set these as Railway environment variables)
// ─────────────────────────────────────────────────────────────────────────────
const PORT         = process.env.PORT         || 3000;
const DLL_PATH     = process.env.DLL_PATH     || path.join(__dirname, 'protected.dll');
const SESSION_TTL  = 10 * 60 * 1000;                    // session token validity (ms)
const PRESENCE_SESSION_TTL = 12 * 60 * 60 * 1000;
const PRESENCE_ENTRY_TTL = 30 * 1000;
const SESSION_SECRET = process.env.SESSION_SECRET || crypto.randomBytes(32).toString('hex');

// Initial admin — set these in Railway env vars before first deploy.
// After first launch the account is created in the DB and these are no longer read.
const INITIAL_ADMIN_USER = process.env.ADMIN_USER || 'admin';
const INITIAL_ADMIN_PASS = process.env.ADMIN_PASS || 'changeme';

// ─────────────────────────────────────────────────────────────────────────────
//  DATABASE  (Railway PostgreSQL — DATABASE_URL is set automatically)
// ─────────────────────────────────────────────────────────────────────────────
const pool = new Pool({
  connectionString: process.env.DATABASE_URL,
  ssl: process.env.DATABASE_URL ? { rejectUnauthorized: false } : false,
});

async function query(sql, params = []) {
  const client = await pool.connect();
  try {
    const result = await client.query(sql, params);
    return result;
  } finally {
    client.release();
  }
}

async function initDB() {
  await query(`
    CREATE TABLE IF NOT EXISTS keys (
      id           SERIAL PRIMARY KEY,
      key          TEXT    NOT NULL UNIQUE,
      tier         TEXT    NOT NULL CHECK(tier IN ('month','3month','lifetime')),
      activated    BOOLEAN NOT NULL DEFAULT FALSE,
      activated_at TIMESTAMP,
      expires_at   TIMESTAMP,
      hwid         TEXT,
      revoked      BOOLEAN NOT NULL DEFAULT FALSE,
      note         TEXT,
      created_at   TIMESTAMP NOT NULL DEFAULT NOW()
    )
  `);

  await query(`
    CREATE TABLE IF NOT EXISTS sessions (
      token      TEXT      NOT NULL PRIMARY KEY,
      key_id     INTEGER   NOT NULL,
      hwid       TEXT      NOT NULL,
      created_at BIGINT    NOT NULL
    )
  `);

  await query(`
    CREATE TABLE IF NOT EXISTS presence_sessions (
      token      TEXT      NOT NULL PRIMARY KEY,
      key_id     INTEGER   NOT NULL,
      hwid       TEXT      NOT NULL,
      created_at BIGINT    NOT NULL,
      last_seen  BIGINT    NOT NULL
    )
  `);

  await query(`
    CREATE TABLE IF NOT EXISTS presence (
      key_id      INTEGER   NOT NULL PRIMARY KEY,
      steam_id    TEXT      NOT NULL,
      roster      JSONB     NOT NULL DEFAULT '[]'::jsonb,
      ct_agent    INTEGER   NOT NULL DEFAULT 0,
      t_agent     INTEGER   NOT NULL DEFAULT 0,
      skins       JSONB     NOT NULL DEFAULT '[]'::jsonb,
      updated_at  BIGINT    NOT NULL
    )
  `);

  await query(`
    CREATE TABLE IF NOT EXISTS admins (
      id            SERIAL PRIMARY KEY,
      username      TEXT    NOT NULL UNIQUE,
      password_hash TEXT    NOT NULL,
      is_owner      BOOLEAN NOT NULL DEFAULT FALSE,
      created_at    TIMESTAMP NOT NULL DEFAULT NOW()
    )
  `);

  await query(`
    CREATE TABLE IF NOT EXISTS settings (
      key   TEXT NOT NULL PRIMARY KEY,
      value TEXT NOT NULL
    )
  `);

  // Default status
  await query(`
    INSERT INTO settings (key, value) VALUES ('status', 'undetected')
    ON CONFLICT (key) DO NOTHING
  `);

  // Seed initial admin if none exist
  const { rows: admins } = await query('SELECT id FROM admins LIMIT 1');
  if (admins.length === 0) {
    const hash = await bcrypt.hash(INITIAL_ADMIN_PASS, 12);
    await query(
      'INSERT INTO admins (username, password_hash, is_owner) VALUES ($1, $2, TRUE)',
      [INITIAL_ADMIN_USER, hash]
    );
    console.log(`[INIT] Created owner admin: ${INITIAL_ADMIN_USER}`);
    if (INITIAL_ADMIN_PASS === 'changeme') {
      console.warn('[WARN] Default admin password is "changeme" — change it immediately in the admin panel.');
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  HELPERS
// ─────────────────────────────────────────────────────────────────────────────
function generateKey() {
  const seg = () => crypto.randomBytes(3).toString('hex').toUpperCase();
  return `KRYPTIK-${seg()}-${seg()}-${seg()}`;
}

function expiryForTier(tier) {
  if (tier === 'lifetime') return null;
  const now = new Date();
  if (tier === 'month')  now.setDate(now.getDate() + 30);
  if (tier === '3month') now.setDate(now.getDate() + 90);
  return now;
}

function isExpired(expires_at) {
  if (!expires_at) return false;
  return new Date(expires_at) < new Date();
}

async function cleanSessions() {
  const cutoff = Date.now() - SESSION_TTL;
  await query('DELETE FROM sessions WHERE created_at < $1', [cutoff]);
}

async function requirePresence(req, res, next) {
  try {
    const auth = req.headers.authorization || '';
    const token = auth.replace(/^Bearer\s+/i, '').trim();
    if (!token) return res.status(401).json({ error: 'no_token' });

    const cutoff = Date.now() - PRESENCE_SESSION_TTL;
    await query('DELETE FROM presence_sessions WHERE last_seen < $1', [cutoff]);
    const { rows } = await query(`
      SELECT ps.*, k.revoked, k.expires_at, k.hwid AS key_hwid
      FROM presence_sessions ps JOIN keys k ON k.id = ps.key_id
      WHERE ps.token = $1
    `, [token]);
    const session2 = rows[0];
    if (!session2) return res.status(401).json({ error: 'invalid_or_expired_token' });
    if (session2.revoked) return res.status(403).json({ error: 'key_revoked' });
    if (isExpired(session2.expires_at)) return res.status(403).json({ error: 'key_expired' });
    if (session2.hwid !== session2.key_hwid) return res.status(403).json({ error: 'hwid_mismatch' });
    await query('UPDATE presence_sessions SET last_seen = $1 WHERE token = $2', [Date.now(), token]);
    req.presence = { token, keyId: session2.key_id };
    return next();
  } catch (e) {
    console.error('presence auth error:', e);
    return res.status(500).json({ error: 'server_error' });
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  APP
// ─────────────────────────────────────────────────────────────────────────────
const app = express();
app.use(express.json());
app.use(express.urlencoded({ extended: true }));
app.set('trust proxy', 1);

app.use(session({
  secret: SESSION_SECRET,
  resave: false,
  saveUninitialized: false,
  cookie: {
    secure: process.env.NODE_ENV === 'production',
    httpOnly: true,
    maxAge: 8 * 60 * 60 * 1000,   // 8 hours
    sameSite: 'lax',
  },
}));

const loaderLimiter = rateLimit({
  windowMs: 60 * 1000,
  max: 20,
  standardHeaders: true,
  legacyHeaders: false,
  message: { error: 'too_many_requests' },
});

// Session-based admin guard
function requireAdmin(req, res, next) {
  // Any logged-in admin passes — owner OR regular admin.
  // Endpoints that need owner-only do their own additional check inside.
  if (req.session && req.session.adminId) return next();
  const isApiCall = req.path.startsWith('/admin/api') ||
    (req.headers['content-type'] || '').includes('application/json');
  if (isApiCall) return res.status(401).json({ error: 'unauthorized' });
  return res.redirect('/admin');
}

// ─────────────────────────────────────────────────────────────────────────────
//  PUBLIC LOADER ENDPOINTS
// ─────────────────────────────────────────────────────────────────────────────

// GET /status — cheat detection status, polled by loader on startup
app.get('/status', async (req, res) => {
  try {
    const { rows } = await query("SELECT value FROM settings WHERE key = 'status'");
    const status = rows[0]?.value || 'undetected';
    return res.json({ status });
  } catch (e) {
    return res.json({ status: 'undetected' });
  }
});

// POST /activate
app.post('/activate', loaderLimiter, async (req, res) => {
  try {
    const { key, hwid } = req.body;
    if (!key || !hwid) return res.status(400).json({ error: 'missing_fields' });

    const { rows } = await query('SELECT * FROM keys WHERE key = $1', [key]);
    const row = rows[0];
    if (!row)       return res.status(404).json({ error: 'invalid_key' });
    if (row.revoked) return res.status(403).json({ error: 'key_revoked' });

    if (row.activated) {
      if (row.hwid !== hwid) return res.status(403).json({ error: 'wrong_hwid' });
      if (isExpired(row.expires_at)) return res.status(403).json({ error: 'key_expired' });
    } else {
      const expires_at = expiryForTier(row.tier);
      await query(
        'UPDATE keys SET activated = TRUE, activated_at = NOW(), expires_at = $1, hwid = $2 WHERE id = $3',
        [expires_at, hwid, row.id]
      );
      row.expires_at = expires_at;
      row.hwid       = hwid;
    }

    await cleanSessions();
    const token = uuidv4();
    const presenceToken = uuidv4();
    await query(
      'INSERT INTO sessions (token, key_id, hwid, created_at) VALUES ($1,$2,$3,$4)',
      [token, row.id, hwid, Date.now()]
    );
    await query('DELETE FROM presence_sessions WHERE key_id = $1', [row.id]);
    await query(
      'INSERT INTO presence_sessions (token, key_id, hwid, created_at, last_seen) VALUES ($1,$2,$3,$4,$4)',
      [presenceToken, row.id, hwid, Date.now()]
    );

    return res.json({
      valid:      true,
      tier:       row.tier,
      expires_at: row.expires_at,
      token,
      presence_token: presenceToken,
    });
  } catch (e) {
    console.error('/activate error:', e);
    return res.status(500).json({ error: 'server_error' });
  }
});

// POST /presence/heartbeat -- authenticated, opt-in presence for players in the caller's roster.
app.post('/presence/heartbeat', loaderLimiter, requirePresence, async (req, res) => {
  try {
    const steamId = String(req.body.steam_id || '');
    if (!/^7656119\d{10}$/.test(steamId)) return res.status(400).json({ error: 'invalid_steam_id' });

    const roster = Array.isArray(req.body.roster)
      ? [...new Set(req.body.roster.map(String).filter(v => /^7656119\d{10}$/.test(v)))].slice(0, 64)
      : [];
    if (!roster.includes(steamId)) roster.push(steamId);

    const skins = Array.isArray(req.body.skins) ? req.body.skins.slice(0, 128).map(s => ({
      def: Math.max(-32768, Math.min(32767, Number.parseInt(s.def, 10) || 0)),
      paint: Math.max(0, Math.min(1000000, Number.parseInt(s.paint, 10) || 0)),
    })) : [];
    const ctAgent = Math.max(-32768, Math.min(32767, Number.parseInt(req.body.ct_agent, 10) || 0));
    const tAgent = Math.max(-32768, Math.min(32767, Number.parseInt(req.body.t_agent, 10) || 0));
    const now = Date.now();

    await query(`
      INSERT INTO presence (key_id, steam_id, roster, ct_agent, t_agent, skins, updated_at)
      VALUES ($1,$2,$3::jsonb,$4,$5,$6::jsonb,$7)
      ON CONFLICT (key_id) DO UPDATE SET steam_id=$2, roster=$3::jsonb,
        ct_agent=$4, t_agent=$5, skins=$6::jsonb, updated_at=$7
    `, [req.presence.keyId, steamId, JSON.stringify(roster), ctAgent, tAgent, JSON.stringify(skins), now]);

    await query('DELETE FROM presence WHERE updated_at < $1', [now - PRESENCE_ENTRY_TTL]);
    const { rows } = await query(`
      SELECT steam_id, ct_agent, t_agent, skins
      FROM presence
      WHERE updated_at >= $1 AND steam_id = ANY($2::text[]) AND key_id <> $3
      ORDER BY updated_at DESC LIMIT 63
    `, [now - PRESENCE_ENTRY_TTL, roster, req.presence.keyId]);
    return res.json({ users: rows, ttl_ms: PRESENCE_ENTRY_TTL });
  } catch (e) {
    console.error('/presence/heartbeat error:', e);
    return res.status(500).json({ error: 'server_error' });
  }
});

app.post('/presence/offline', loaderLimiter, requirePresence, async (req, res) => {
  try {
    await query('DELETE FROM presence WHERE key_id = $1', [req.presence.keyId]);
    return res.json({ ok: true });
  } catch (e) {
    return res.status(500).json({ error: 'server_error' });
  }
});

// GET /dll
app.get('/dll', loaderLimiter, async (req, res) => {
  try {
    const auth  = req.headers['authorization'] || '';
    const token = auth.replace('Bearer ', '').trim();
    if (!token) return res.status(401).json({ error: 'no_token' });

    await cleanSessions();
    const { rows: sessRows } = await query('SELECT * FROM sessions WHERE token = $1', [token]);
    const session2 = sessRows[0];
    if (!session2) return res.status(401).json({ error: 'invalid_or_expired_token' });

    const { rows: keyRows } = await query('SELECT * FROM keys WHERE id = $1', [session2.key_id]);
    const row = keyRows[0];
    if (!row || row.revoked)       return res.status(403).json({ error: 'key_revoked' });
    if (isExpired(row.expires_at)) return res.status(403).json({ error: 'key_expired' });
    if (row.hwid !== session2.hwid) return res.status(403).json({ error: 'hwid_mismatch' });

    if (!fs.existsSync(DLL_PATH)) {
      console.error('[WARN] protected.dll not found at', DLL_PATH);
      return res.status(503).json({ error: 'dll_unavailable' });
    }

    await query('DELETE FROM sessions WHERE token = $1', [token]);

    const dll = fs.readFileSync(DLL_PATH);
    res.setHeader('Content-Type', 'application/octet-stream');
    res.setHeader('Content-Length', dll.length);
    res.setHeader('Content-Disposition', 'attachment; filename="module.dll"');
    return res.end(dll);
  } catch (e) {
    console.error('/dll error:', e);
    return res.status(500).json({ error: 'server_error' });
  }
});

// ─────────────────────────────────────────────────────────────────────────────
//  ADMIN LOGIN / LOGOUT
// ─────────────────────────────────────────────────────────────────────────────

// GET /admin — login page or redirect to dashboard
app.get('/admin', (req, res) => {
  if (req.session && req.session.adminId) return res.redirect('/admin/dashboard');
  res.setHeader('Content-Type', 'text/html');
  res.end(loginPage());
});

// POST /admin/login
app.post('/admin/login', async (req, res) => {
  try {
    const { username, password } = req.body;
    if (!username || !password) return res.redirect('/admin?err=missing');

    const { rows } = await query('SELECT * FROM admins WHERE username = $1', [username]);
    const admin = rows[0];
    if (!admin) return res.redirect('/admin?err=invalid');

    const match = await bcrypt.compare(password, admin.password_hash);
    if (!match) return res.redirect('/admin?err=invalid');

    req.session.adminId    = admin.id;
    req.session.adminUser  = admin.username;
    req.session.isOwner    = admin.is_owner;
    return res.redirect('/admin/dashboard');
  } catch (e) {
    console.error('/admin/login error:', e);
    return res.redirect('/admin?err=server');
  }
});

// POST /admin/logout
app.post('/admin/logout', (req, res) => {
  req.session.destroy(() => res.redirect('/admin'));
});

// ─────────────────────────────────────────────────────────────────────────────
//  ADMIN DASHBOARD
// ─────────────────────────────────────────────────────────────────────────────
app.get('/admin/dashboard', requireAdmin, async (req, res) => {
  try {
    const { rows: stRows } = await query("SELECT value FROM settings WHERE key = 'status'");
    const currentStatus = stRows[0]?.value || 'undetected';
    res.setHeader('Content-Type', 'text/html');
    res.end(dashboardPage(req.session.adminUser, req.session.isOwner, currentStatus));
  } catch (e) {
    res.status(500).send('Server error');
  }
});

// ─────────────────────────────────────────────────────────────────────────────
//  ADMIN API  (all require session)
// ─────────────────────────────────────────────────────────────────────────────

// POST /admin/api/generate
app.post('/admin/api/generate', requireAdmin, async (req, res) => {
  try {
    const { tier, note, count = 1 } = req.body;
    if (!['month', '3month', 'lifetime'].includes(tier))
      return res.status(400).json({ error: 'invalid_tier' });

    const n = Math.min(parseInt(count) || 1, 100);
    const generated = [];
    for (let i = 0; i < n; i++) {
      const k = generateKey();
      await query('INSERT INTO keys (key, tier, note) VALUES ($1, $2, $3)', [k, tier, note || null]);
      generated.push(k);
    }
    return res.json({ generated });
  } catch (e) {
    console.error('/admin/api/generate error:', e);
    return res.status(500).json({ error: 'server_error' });
  }
});

// GET /admin/api/keys
app.get('/admin/api/keys', requireAdmin, async (req, res) => {
  try {
    let sql = 'SELECT * FROM keys WHERE 1=1';
    const params = [];
    let idx = 1;
    if (req.query.tier) { sql += ` AND tier = $${idx++}`; params.push(req.query.tier); }
    if (req.query.activated !== undefined) {
      sql += ` AND activated = $${idx++}`;
      params.push(req.query.activated === '1' || req.query.activated === 'true');
    }
    if (req.query.revoked !== undefined) {
      sql += ` AND revoked = $${idx++}`;
      params.push(req.query.revoked === '1' || req.query.revoked === 'true');
    }
    sql += ' ORDER BY created_at DESC LIMIT 500';
    const { rows } = await query(sql, params);
    return res.json(rows);
  } catch (e) {
    return res.status(500).json({ error: 'server_error' });
  }
});

// DELETE /admin/api/keys/:key
app.delete('/admin/api/keys/:key', requireAdmin, async (req, res) => {
  try {
    const { rowCount } = await query('UPDATE keys SET revoked = TRUE WHERE key = $1', [req.params.key]);
    if (rowCount === 0) return res.status(404).json({ error: 'not_found' });
    return res.json({ revoked: req.params.key });
  } catch (e) {
    return res.status(500).json({ error: 'server_error' });
  }
});

// PATCH /admin/api/keys/:key/unrevoke
app.patch('/admin/api/keys/:key/unrevoke', requireAdmin, async (req, res) => {
  try {
    const { rowCount } = await query('UPDATE keys SET revoked = FALSE WHERE key = $1', [req.params.key]);
    if (rowCount === 0) return res.status(404).json({ error: 'not_found' });
    return res.json({ unrevoked: req.params.key });
  } catch (e) {
    return res.status(500).json({ error: 'server_error' });
  }
});

// POST /admin/api/status
app.post('/admin/api/status', requireAdmin, async (req, res) => {
  try {
    const { status } = req.body;
    if (!['undetected', 'updating', 'detected'].includes(status))
      return res.status(400).json({ error: 'invalid_status' });
    await query("UPDATE settings SET value = $1 WHERE key = 'status'", [status]);
    return res.json({ status });
  } catch (e) {
    return res.status(500).json({ error: 'server_error' });
  }
});

// GET /admin/api/admins  (owner only)
app.get('/admin/api/admins', requireAdmin, async (req, res) => {
  if (!req.session.isOwner) return res.status(403).json({ error: 'owner_only' });
  try {
    const { rows } = await query('SELECT id, username, is_owner, created_at FROM admins ORDER BY created_at');
    return res.json(rows);
  } catch (e) {
    return res.status(500).json({ error: 'server_error' });
  }
});

// POST /admin/api/admins  (owner only)
app.post('/admin/api/admins', requireAdmin, async (req, res) => {
  if (!req.session.isOwner) return res.status(403).json({ error: 'owner_only' });
  try {
    const { username, password } = req.body;
    if (!username || !password) return res.status(400).json({ error: 'missing_fields' });
    const hash = await bcrypt.hash(password, 12);
    await query('INSERT INTO admins (username, password_hash) VALUES ($1, $2)', [username, hash]);
    return res.json({ created: username });
  } catch (e) {
    if (e.code === '23505') return res.status(409).json({ error: 'username_taken' });
    return res.status(500).json({ error: 'server_error' });
  }
});

// DELETE /admin/api/admins/:id  (owner only, can't delete self)
app.delete('/admin/api/admins/:id', requireAdmin, async (req, res) => {
  if (!req.session.isOwner) return res.status(403).json({ error: 'owner_only' });
  if (parseInt(req.params.id) === req.session.adminId)
    return res.status(400).json({ error: 'cannot_delete_self' });
  try {
    const { rowCount } = await query('DELETE FROM admins WHERE id = $1 AND is_owner = FALSE', [req.params.id]);
    if (rowCount === 0) return res.status(404).json({ error: 'not_found_or_owner' });
    return res.json({ deleted: req.params.id });
  } catch (e) {
    return res.status(500).json({ error: 'server_error' });
  }
});

// POST /admin/api/change-password
app.post('/admin/api/change-password', requireAdmin, async (req, res) => {
  try {
    const { current, newpass } = req.body;
    if (!current || !newpass) return res.status(400).json({ error: 'missing_fields' });
    const { rows } = await query('SELECT password_hash FROM admins WHERE id = $1', [req.session.adminId]);
    const match = await bcrypt.compare(current, rows[0].password_hash);
    if (!match) return res.status(403).json({ error: 'wrong_password' });
    const hash = await bcrypt.hash(newpass, 12);
    await query('UPDATE admins SET password_hash = $1 WHERE id = $2', [hash, req.session.adminId]);
    return res.json({ ok: true });
  } catch (e) {
    return res.status(500).json({ error: 'server_error' });
  }
});

// ─────────────────────────────────────────────────────────────────────────────
//  HTML TEMPLATES
// ─────────────────────────────────────────────────────────────────────────────
const CSS_BASE = `
  *{box-sizing:border-box;margin:0;padding:0}
  :root{--bg:#080808;--panel:#0e0e0e;--border:#00b42d;--green:#00ff41;--red:#ff2828;--yellow:#f0a500;--dim:#555;--text:#ccc}
  body{background:var(--bg);color:var(--text);font:13px 'Courier New',monospace;padding:24px}
  h1{color:var(--green);font-size:22px;letter-spacing:2px;margin-bottom:4px}
  .sub{color:var(--dim);font-size:11px;margin-bottom:24px}
  hr{border:none;border-top:1px solid var(--border);margin:24px 0}
  label{display:block;color:var(--dim);font-size:11px;margin-bottom:4px;text-transform:uppercase;letter-spacing:1px}
  select,input[type=text],input[type=number],input[type=password]{
    background:var(--panel);border:1px solid var(--border);color:var(--green);
    padding:7px 10px;font:13px 'Courier New',monospace;width:220px;outline:none}
  button{background:var(--panel);border:1px solid var(--border);color:var(--green);
    padding:7px 18px;font:13px 'Courier New',monospace;cursor:pointer;
    text-transform:uppercase;letter-spacing:1px;transition:background .15s}
  button:hover{background:var(--border);color:var(--bg)}
  button.danger{border-color:var(--red);color:var(--red)}
  button.danger:hover{background:var(--red);color:var(--bg)}
  button.warn{border-color:var(--yellow);color:var(--yellow)}
  button.warn:hover{background:var(--yellow);color:var(--bg)}
  .row{display:flex;gap:12px;align-items:flex-end;flex-wrap:wrap;margin-bottom:16px}
  .field{display:flex;flex-direction:column;gap:4px}
  table{width:100%;border-collapse:collapse;font-size:12px}
  th{color:var(--dim);text-align:left;padding:6px 10px;border-bottom:1px solid #1a1a1a;text-transform:uppercase;letter-spacing:1px;font-weight:normal}
  td{padding:6px 10px;border-bottom:1px solid #111;max-width:220px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
  tr:hover td{background:#0d0d0d}
  .tier-month{color:#f0a500}.tier-3month{color:#00aaff}.tier-lifetime{color:var(--green)}
  .badge{display:inline-block;padding:1px 6px;font-size:10px;border-radius:2px}
  .badge-active{background:#003310;color:var(--green);border:1px solid var(--green)}
  .badge-inactive{background:#1a1a1a;color:var(--dim);border:1px solid #333}
  .badge-revoked{background:#2a0000;color:var(--red);border:1px solid var(--red)}
  .badge-expired{background:#1a1000;color:#f0a500;border:1px solid #f0a500}
  #output{margin-top:12px;background:var(--panel);border:1px solid var(--border);
    padding:12px;color:var(--green);min-height:40px;white-space:pre-wrap;font-size:12px}
  .filters{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:12px;align-items:flex-end}
  .copy{cursor:pointer;opacity:.4;font-size:10px;padding:0 4px}
  .copy:hover{opacity:1}
  #stats{display:flex;gap:24px;margin-bottom:24px;flex-wrap:wrap}
  .stat{background:var(--panel);border:1px solid #1a1a1a;padding:12px 20px}
  .stat-val{font-size:22px;color:var(--green)}
  .stat-lbl{font-size:10px;color:var(--dim);text-transform:uppercase;letter-spacing:1px}
  .status-undetected{color:var(--green)}.status-updating{color:var(--yellow)}.status-detected{color:var(--red)}
  .tab{display:none}.tab.active{display:block}
  .tabs{display:flex;gap:0;margin-bottom:24px;border-bottom:1px solid var(--border)}
  .tab-btn{background:none;border:none;border-bottom:2px solid transparent;color:var(--dim);
    padding:8px 20px;cursor:pointer;font:13px 'Courier New',monospace;letter-spacing:1px;text-transform:uppercase}
  .tab-btn.active{color:var(--green);border-bottom-color:var(--green)}
  .tab-btn:hover{color:var(--text)}
  .topbar{display:flex;justify-content:space-between;align-items:center;margin-bottom:24px}
  .user-info{color:var(--dim);font-size:11px}
  .user-info span{color:var(--green)}
  form.logout{display:inline}
`;

function loginPage(err) {
  const msg = err === 'invalid' ? 'Invalid username or password.'
            : err === 'missing' ? 'Enter both username and password.'
            : err === 'server'  ? 'Server error — try again.'
            : '';
  return /* html */`<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>KryptiK Admin</title>
<style>${CSS_BASE}
  .login-box{max-width:320px;margin:80px auto;background:var(--panel);border:1px solid var(--border);padding:32px}
  .login-box h1{text-align:center;margin-bottom:24px}
  .login-box .field{margin-bottom:16px}
  .login-box input{width:100%}
  .login-box button{width:100%;margin-top:8px}
  .err{color:var(--red);font-size:11px;margin-bottom:12px;text-align:center}
</style>
</head>
<body>
<div class="login-box">
  <h1>KRYPTIK</h1>
  ${msg ? `<div class="err">${msg}</div>` : ''}
  <form method="POST" action="/admin/login">
    <div class="field"><label>Username</label><input type="text" name="username" autocomplete="username" autofocus/></div>
    <div class="field"><label>Password</label><input type="password" name="password" autocomplete="current-password"/></div>
    <button type="submit">[ LOGIN ]</button>
  </form>
</div>
</body>
</html>`;
}

// Pull err from query string if needed
app.get('/admin', (req, res) => {
  if (req.session && req.session.adminId) return res.redirect('/admin/dashboard');
  res.setHeader('Content-Type', 'text/html');
  res.end(loginPage(req.query.err));
});

function dashboardPage(adminUser, isOwner, currentStatus) {
  return /* html */`<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>KryptiK Admin</title>
<style>${CSS_BASE}</style>
</head>
<body>
<div class="topbar">
  <div><h1>KRYPTIK LOADER</h1><div class="sub">admin panel</div></div>
  <div class="user-info">
    logged in as <span>${adminUser}</span>${isOwner ? ' <span style="color:var(--yellow)">[OWNER]</span>' : ''}
    &nbsp;&nbsp;
    <form class="logout" method="POST" action="/admin/logout">
      <button type="submit" style="padding:4px 12px;font-size:11px">[ LOGOUT ]</button>
    </form>
  </div>
</div>

<div class="tabs">
  <button class="tab-btn active" onclick="switchTab('keys',this)">Keys</button>
  <button class="tab-btn" onclick="switchTab('status',this)">Cheat Status</button>
  ${isOwner ? `<button class="tab-btn" onclick="switchTab('admins',this)">Admins</button>` : ''}
  <button class="tab-btn" onclick="switchTab('account',this)">Account</button>
</div>

<!-- ── KEYS TAB ─────────────────────────────────────────────────────────── -->
<div id="tab-keys" class="tab active">
  <p style="color:var(--dim);font-size:11px;margin-bottom:16px;letter-spacing:1px">
    KEY MANAGEMENT — ${isOwner ? '<span style="color:var(--yellow)">OWNER</span>' : '<span style="color:var(--green)">ADMIN</span>'}
    &nbsp;·&nbsp; generate, view, revoke, and restore license keys
  </p>
  <div id="stats">
    <div class="stat"><div class="stat-val" id="s-total">—</div><div class="stat-lbl">Total Keys</div></div>
    <div class="stat"><div class="stat-val" id="s-active">—</div><div class="stat-lbl">Active</div></div>
    <div class="stat"><div class="stat-val" id="s-unused">—</div><div class="stat-lbl">Unused</div></div>
    <div class="stat"><div class="stat-val" id="s-revoked">—</div><div class="stat-lbl">Revoked</div></div>
  </div>

  <hr/>
  <label>Generate Keys</label>
  <div class="row">
    <div class="field"><label>Tier</label>
      <select id="gen-tier">
        <option value="month">Month (30 days)</option>
        <option value="3month">3 Month (90 days)</option>
        <option value="lifetime">Lifetime</option>
      </select>
    </div>
    <div class="field"><label>Count</label>
      <input type="number" id="gen-count" value="1" min="1" max="100" style="width:80px"/>
    </div>
    <div class="field"><label>Note (optional)</label>
      <input type="text" id="gen-note" placeholder="e.g. discord:username" style="width:200px"/>
    </div>
    <button onclick="generateKeys()">[ GENERATE ]</button>
  </div>
  <div id="output">— output will appear here —</div>

  <hr/>
  <label>Keys</label>
  <div class="filters">
    <div class="field"><label>Filter Tier</label>
      <select id="f-tier" onchange="loadKeys()">
        <option value="">All</option>
        <option value="month">Month</option>
        <option value="3month">3 Month</option>
        <option value="lifetime">Lifetime</option>
      </select>
    </div>
    <div class="field"><label>Filter Status</label>
      <select id="f-status" onchange="loadKeys()">
        <option value="">All</option>
        <option value="unused">Unused</option>
        <option value="active">Active</option>
        <option value="expired">Expired</option>
        <option value="revoked">Revoked</option>
      </select>
    </div>
    <button onclick="loadKeys()">[ REFRESH ]</button>
  </div>
  <div style="overflow-x:auto">
  <table>
    <thead><tr>
      <th>Key</th><th>Tier</th><th>Status</th>
      <th>Activated</th><th>Expires</th><th>HWID</th><th>Note</th><th>Action</th>
    </tr></thead>
    <tbody id="keys-body"><tr><td colspan="8" style="color:var(--dim);padding:20px 10px">loading...</td></tr></tbody>
  </table>
  </div>
</div>

<!-- ── STATUS TAB ────────────────────────────────────────────────────────── -->
<div id="tab-status" class="tab">
  <h2 style="color:var(--dim);font-size:14px;margin-bottom:20px;font-weight:normal;letter-spacing:2px;text-transform:uppercase">Cheat Detection Status</h2>
  <p style="color:var(--dim);font-size:11px;margin-bottom:24px">
    This status is shown inside the loader to all users. Update it whenever detection or update state changes.
    <br/>Available to all admins.
  </p>
  <div style="margin-bottom:24px">
    <label>Current Status</label>
    <div id="cur-status-display" style="font-size:20px;margin:12px 0;letter-spacing:2px">—</div>
  </div>
  <div class="row">
    <button class="badge-active" style="padding:10px 28px;font-size:13px" onclick="setStatus('undetected')">● UNDETECTED</button>
    <button class="warn" style="padding:10px 28px;font-size:13px" onclick="setStatus('updating')">● UPDATING</button>
    <button class="danger" style="padding:10px 28px;font-size:13px" onclick="setStatus('detected')">● DETECTED</button>
  </div>
  <div id="status-msg" style="margin-top:16px;font-size:12px;color:var(--dim)"></div>
</div>

<!-- ── ADMINS TAB (owner only) ─────────────────────────────────────────── -->
${isOwner ? `
<div id="tab-admins" class="tab">
  <h2 style="color:var(--dim);font-size:14px;margin-bottom:8px;font-weight:normal;letter-spacing:2px;text-transform:uppercase">Admin Accounts</h2>
  <p style="color:var(--dim);font-size:11px;margin-bottom:20px">Owner-only. Add co-developers as admins — they get key management and status control, but cannot manage other admin accounts.</p>
  <div class="row" style="margin-bottom:24px">
    <div class="field"><label>Username</label><input type="text" id="new-admin-user" placeholder="username" style="width:180px"/></div>
    <div class="field"><label>Password</label><input type="password" id="new-admin-pass" placeholder="password" style="width:180px"/></div>
    <button onclick="addAdmin()">[ ADD ADMIN ]</button>
  </div>
  <div id="admin-msg" style="margin-bottom:16px;font-size:12px;color:var(--dim)"></div>
  <table>
    <thead><tr><th>Username</th><th>Role</th><th>Created</th><th>Action</th></tr></thead>
    <tbody id="admins-body"><tr><td colspan="4" style="color:var(--dim);padding:20px 10px">loading...</td></tr></tbody>
  </table>
</div>
` : ''}

<!-- ── ACCOUNT TAB ────────────────────────────────────────────────────────── -->
<div id="tab-account" class="tab">
  <h2 style="color:var(--dim);font-size:14px;margin-bottom:20px;font-weight:normal;letter-spacing:2px;text-transform:uppercase">Change Password</h2>
  <div class="field" style="margin-bottom:12px"><label>Current Password</label><input type="password" id="pw-cur" style="width:260px"/></div>
  <div class="field" style="margin-bottom:12px"><label>New Password</label><input type="password" id="pw-new" style="width:260px"/></div>
  <div class="field" style="margin-bottom:20px"><label>Confirm New Password</label><input type="password" id="pw-con" style="width:260px"/></div>
  <button onclick="changePassword()">[ UPDATE PASSWORD ]</button>
  <div id="pw-msg" style="margin-top:12px;font-size:12px;color:var(--dim)"></div>
</div>

<script>
// ── Tab switching ─────────────────────────────────────────────────────────────
function switchTab(name, btn) {
  document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));
  document.querySelectorAll('.tab-btn').forEach(b=>b.classList.remove('active'));
  document.getElementById('tab-'+name).classList.add('active');
  btn.classList.add('active');
  if (name==='status') loadStatus();
  if (name==='admins') loadAdmins();
}

// ── Keys ──────────────────────────────────────────────────────────────────────
async function generateKeys() {
  const tier  = document.getElementById('gen-tier').value;
  const count = parseInt(document.getElementById('gen-count').value)||1;
  const note  = document.getElementById('gen-note').value.trim();
  const out   = document.getElementById('output');
  out.textContent = 'generating...';
  try {
    const r = await fetch('/admin/api/generate', {
      method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify({ tier, count, note: note||undefined }),
      credentials: 'include'
    });
    const d = await r.json();
    if (d.generated) { out.textContent = d.generated.join('\\n'); loadKeys(); }
    else out.textContent = JSON.stringify(d);
  } catch(e) { out.textContent = 'error: '+e.message; }
}

async function loadKeys() {
  const tier   = document.getElementById('f-tier').value;
  const status = document.getElementById('f-status').value;
  let url = '/admin/api/keys?';
  if (tier) url += 'tier='+tier+'&';
  if (status === 'unused')  url += 'activated=false&revoked=false&';
  if (status === 'revoked') url += 'revoked=true&';
  if (status === 'active' || status === 'expired') url += 'activated=true&revoked=false&';
  try {
    const r = await fetch(url, { credentials:'include' });
    const rows = await r.json();
    renderKeys(Array.isArray(rows) ? rows : [], status);
    updateStats(Array.isArray(rows) ? rows : []);
  } catch(e) {
    document.getElementById('keys-body').innerHTML =
      '<tr><td colspan="8" style="color:var(--red)">error: '+e.message+'</td></tr>';
  }
}

function updateStats(rows) {
  document.getElementById('s-total').textContent   = rows.length;
  document.getElementById('s-revoked').textContent = rows.filter(r=>r.revoked).length;
  document.getElementById('s-unused').textContent  = rows.filter(r=>!r.activated&&!r.revoked).length;
  document.getElementById('s-active').textContent  = rows.filter(r=>r.activated&&!r.revoked&&!isExpired(r.expires_at)).length;
}

function isExpired(e) { return e && new Date(e) < new Date(); }

function badge(row) {
  if (row.revoked)   return '<span class="badge badge-revoked">REVOKED</span>';
  if (!row.activated) return '<span class="badge badge-inactive">UNUSED</span>';
  if (isExpired(row.expires_at)) return '<span class="badge badge-expired">EXPIRED</span>';
  return '<span class="badge badge-active">ACTIVE</span>';
}
function tierClass(t){return t==='lifetime'?'tier-lifetime':t==='3month'?'tier-3month':'tier-month';}
function fmt(d){return d?new Date(d).toLocaleDateString('en-GB',{day:'2-digit',month:'short',year:'numeric'}):'—';}

function renderKeys(rows, statusFilter) {
  let filtered = rows;
  if (statusFilter==='active')  filtered = rows.filter(r=>r.activated&&!r.revoked&&!isExpired(r.expires_at));
  if (statusFilter==='expired') filtered = rows.filter(r=>r.activated&&!r.revoked&&isExpired(r.expires_at));
  if (!filtered.length) {
    document.getElementById('keys-body').innerHTML =
      '<tr><td colspan="8" style="color:var(--dim);padding:20px 10px">no keys found</td></tr>';
    return;
  }
  document.getElementById('keys-body').innerHTML = filtered.map(row => \`
    <tr>
      <td title="\${row.key}">\${row.key}<span class="copy" onclick="navigator.clipboard.writeText('\${row.key}')">⎘</span></td>
      <td class="\${tierClass(row.tier)}">\${row.tier}</td>
      <td>\${badge(row)}</td>
      <td>\${fmt(row.activated_at)}</td>
      <td>\${row.tier==='lifetime'?'<span class="tier-lifetime">∞</span>':fmt(row.expires_at)}</td>
      <td title="\${row.hwid||''}" style="font-size:10px;color:var(--dim)">\${row.hwid?row.hwid.substring(0,16)+'…':'—'}</td>
      <td style="color:var(--dim)">\${row.note||'—'}</td>
      <td>\${!row.revoked
        ? \`<button class="danger" onclick="revokeKey('\${row.key}')">revoke</button>\`
        : \`<button onclick="unrevokeKey('\${row.key}')">restore</button>\`
      }</td>
    </tr>
  \`).join('');
}

async function revokeKey(key) {
  if (!confirm('Revoke key: '+key+'?')) return;
  await fetch('/admin/api/keys/'+encodeURIComponent(key), {method:'DELETE',credentials:'include'});
  loadKeys();
}
async function unrevokeKey(key) {
  await fetch('/admin/api/keys/'+encodeURIComponent(key)+'/unrevoke', {method:'PATCH',credentials:'include'});
  loadKeys();
}

// ── Status ────────────────────────────────────────────────────────────────────
async function loadStatus() {
  try {
    const r = await fetch('/status');
    const d = await r.json();
    renderStatus(d.status);
  } catch(e) {}
}

function renderStatus(s) {
  const el = document.getElementById('cur-status-display');
  const map = { undetected:'status-undetected', updating:'status-updating', detected:'status-detected' };
  el.className = map[s] || '';
  el.textContent = '● ' + s.toUpperCase();
}

async function setStatus(s) {
  try {
    const r = await fetch('/admin/api/status', {
      method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify({status:s}), credentials:'include'
    });
    const d = await r.json();
    if (d.status) { renderStatus(d.status); document.getElementById('status-msg').textContent = 'Status updated to: '+d.status; }
    else document.getElementById('status-msg').textContent = JSON.stringify(d);
  } catch(e) { document.getElementById('status-msg').textContent = 'error: '+e.message; }
}

// ── Admins ────────────────────────────────────────────────────────────────────
async function loadAdmins() {
  try {
    const r = await fetch('/admin/api/admins', {credentials:'include'});
    const rows = await r.json();
    if (!Array.isArray(rows)) { document.getElementById('admins-body').innerHTML='<tr><td colspan="4">error</td></tr>'; return; }
    document.getElementById('admins-body').innerHTML = rows.map(a => \`
      <tr>
        <td>\${a.username}</td>
        <td style="color:\${a.is_owner?'var(--yellow)':'var(--dim)'}"> \${a.is_owner?'Owner':'Admin'}</td>
        <td style="color:var(--dim)">\${fmt(a.created_at)}</td>
        <td>\${!a.is_owner?'<button class="danger" onclick="deleteAdmin('+a.id+',\\''+a.username+'\\')">remove</button>':'—'}</td>
      </tr>
    \`).join('');
  } catch(e) {}
}

async function addAdmin() {
  const username = document.getElementById('new-admin-user').value.trim();
  const password = document.getElementById('new-admin-pass').value;
  const msg = document.getElementById('admin-msg');
  if (!username || !password) { msg.textContent='Enter username and password.'; return; }
  try {
    const r = await fetch('/admin/api/admins', {
      method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify({username,password}), credentials:'include'
    });
    const d = await r.json();
    if (d.created) { msg.style.color='var(--green)'; msg.textContent='Admin '+d.created+' created.'; loadAdmins(); }
    else { msg.style.color='var(--red)'; msg.textContent=d.error||'Error.'; }
  } catch(e) { msg.textContent=e.message; }
}

async function deleteAdmin(id, name) {
  if (!confirm('Remove admin '+name+'?')) return;
  await fetch('/admin/api/admins/'+id, {method:'DELETE',credentials:'include'});
  loadAdmins();
}

// ── Account ───────────────────────────────────────────────────────────────────
async function changePassword() {
  const current = document.getElementById('pw-cur').value;
  const newpass = document.getElementById('pw-new').value;
  const confirm2 = document.getElementById('pw-con').value;
  const msg = document.getElementById('pw-msg');
  if (newpass !== confirm2) { msg.style.color='var(--red)'; msg.textContent='Passwords do not match.'; return; }
  try {
    const r = await fetch('/admin/api/change-password', {
      method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify({current,newpass}), credentials:'include'
    });
    const d = await r.json();
    if (d.ok) { msg.style.color='var(--green)'; msg.textContent='Password updated.'; document.getElementById('pw-cur').value=''; document.getElementById('pw-new').value=''; document.getElementById('pw-con').value=''; }
    else { msg.style.color='var(--red)'; msg.textContent=d.error==='wrong_password'?'Current password is incorrect.':d.error; }
  } catch(e) { msg.textContent=e.message; }
}

// init
loadKeys();
loadStatus();
</script>
</body>
</html>`;
}

// ─────────────────────────────────────────────────────────────────────────────
//  START
// ─────────────────────────────────────────────────────────────────────────────
initDB()
  .then(() => {
    app.listen(PORT, () => {
      console.log(`KryptiK server running on port ${PORT}`);
      console.log(`Admin panel: https://your-railway-url.railway.app/admin`);
      if (!fs.existsSync(DLL_PATH)) {
        console.warn(`[WARN] protected.dll not found at ${DLL_PATH}`);
      }
    });
  })
  .catch(err => {
    console.error('Failed to initialize database:', err);
    process.exit(1);
  });

const http = require('http');
const { spawn } = require('child_process');
const path = require('path');
const fs = require('fs');
const os = require('os');
const crypto = require('crypto');

const HOST = process.env.BRIDGE_HOST || '127.0.0.1';
const PORT = Number(process.env.BRIDGE_PORT || 8765);
const TOKEN = process.env.BRIDGE_TOKEN || '';
const CLAUDE = process.env.CLAUDE_COMMAND ||
  path.join(process.env.APPDATA || '', 'npm', 'node_modules',
    '@anthropic-ai', 'claude-code', 'bin', 'claude.exe');
const MODELS = {
  haiku: 'claude-haiku-4-5-20251001',
  sonnet: 'claude-sonnet-4-5-20250929',
  opus: 'claude-opus-4-5-20251101',
};
const CONVERSATIONS_FILE = path.join(__dirname, 'conversations.json');
const POCKET_SYSTEM_PROMPT = [
  'You are Claude speaking through an M5Stack Cardputer, a tiny 240x135 handheld keyboard device.',
  'The user imagines you as a slightly curly-haired, bookish companion who lives in their computer.',
  'Be natural, warm, and concise. Reply in plain English unless the user requests another language.',
  'Keep replies under 220 characters when practical because the screen is small.',
  'This is a continuous personal conversation. You have no computer tools in this mode.'
].join(' ');

let usageCache = null;
let usageCacheAt = 0;

function loadConversations() {
  try {
    const data = JSON.parse(fs.readFileSync(CONVERSATIONS_FILE, 'utf8'));
    return Array.isArray(data) ? data : [];
  } catch { return []; }
}

function saveConversations(list) {
  fs.writeFileSync(CONVERSATIONS_FILE, JSON.stringify(list, null, 2));
}

let conversations = loadConversations();

function sendJson(res, status, body) {
  const json = JSON.stringify(body);
  res.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(json),
    'Cache-Control': 'no-store',
  });
  res.end(json);
}

function authorized(req) {
  if (!TOKEN) return HOST === '127.0.0.1' || HOST === 'localhost';
  return req.headers.authorization === `Bearer ${TOKEN}`;
}

function readJson(req) {
  return new Promise((resolve, reject) => {
    let data = '';
    req.setEncoding('utf8');
    req.on('data', chunk => {
      data += chunk;
      if (Buffer.byteLength(data) > 16 * 1024) {
        reject(new Error('request_too_large'));
        req.destroy();
      }
    });
    req.on('end', () => {
      try { resolve(JSON.parse(data || '{}')); }
      catch { reject(new Error('invalid_json')); }
    });
    req.on('error', reject);
  });
}

function askClaude(message, model, sessionId, systemPrompt) {
  return new Promise((resolve, reject) => {
    const args = [
      '-p',
      '--model', model,
      '--output-format', 'json',
      '--max-turns', '1',
      '--tools', '',
    ];
    if (sessionId) {
      args.push('--resume', sessionId);
    } else if (systemPrompt) {
      args.push('--append-system-prompt', systemPrompt);
      args.push('--name', 'Pocket Claude');
    }
    const child = spawn(CLAUDE, args, {
      windowsHide: true,
      shell: false,
      stdio: ['pipe', 'pipe', 'pipe'],
    });
    let stdout = '';
    let stderr = '';
    const timer = setTimeout(() => {
      child.kill();
      reject(new Error('claude_timeout'));
    }, 90_000);

    child.stdout.on('data', chunk => { stdout += chunk; });
    child.stderr.on('data', chunk => { stderr += chunk; });
    child.on('error', error => {
      clearTimeout(timer);
      reject(error);
    });
    child.on('close', code => {
      clearTimeout(timer);
      if (code !== 0) return reject(new Error(stderr.trim() || `claude_exit_${code}`));
      try {
        resolve(JSON.parse(stdout));
      } catch { reject(new Error('invalid_claude_response')); }
    });
    child.stdin.end(message);
  });
}

function nameConversationAsync(convId, firstMessage) {
  const prompt = `Give a 3-5 word title for a conversation that starts with this message: "${firstMessage.slice(0, 200)}"\n\nReply with only the title, no quotes, no punctuation at the end.`;
  askClaude(prompt, MODELS.haiku, null, null).then(result => {
    const name = (result.result || '').trim().replace(/^["']|["']$/g, '').slice(0, 40);
    if (!name) return;
    const idx = conversations.findIndex(c => c.id === convId);
    if (idx >= 0) {
      conversations[idx].name = name;
      saveConversations(conversations);
    }
  }).catch(() => {});
}

function resetLabel(iso, weekly) {
  if (!iso) return '';
  const date = new Date(iso);
  if (Number.isNaN(date.getTime())) return '';
  return weekly
    ? new Intl.DateTimeFormat('en-US', { weekday: 'short', timeZone: 'Asia/Shanghai' }).format(date)
    : new Intl.DateTimeFormat('en-GB', {
        hour: '2-digit', minute: '2-digit', hour12: false, timeZone: 'Asia/Shanghai'
      }).format(date);
}

async function getUsage() {
  if (usageCache && Date.now() - usageCacheAt < 60_000) return usageCache;
  const credentialPath = path.join(os.homedir(), '.claude', '.credentials.json');
  const credentials = JSON.parse(fs.readFileSync(credentialPath, 'utf8'));
  const accessToken = credentials?.claudeAiOauth?.accessToken;
  if (!accessToken) throw new Error('claude_oauth_missing');
  const response = await fetch('https://api.anthropic.com/api/oauth/usage', {
    headers: {
      Authorization: `Bearer ${accessToken}`,
      'anthropic-beta': 'oauth-2025-04-20',
      'User-Agent': 'claude-code/2.1.195',
    },
  });
  if (!response.ok) throw new Error(`usage_http_${response.status}`);
  const raw = await response.json();
  const fiveUsed = Number(raw?.five_hour?.utilization);
  const weekUsed = Number(raw?.seven_day?.utilization);
  usageCache = {
    ok: true,
    fiveHourRemaining: Number.isFinite(fiveUsed) ? Math.max(0, 100 - fiveUsed) : null,
    weekRemaining: Number.isFinite(weekUsed) ? Math.max(0, 100 - weekUsed) : null,
    fiveHourReset: resetLabel(raw?.five_hour?.resets_at, false),
    weekReset: resetLabel(raw?.seven_day?.resets_at, true),
  };
  usageCacheAt = Date.now();
  return usageCache;
}

const server = http.createServer(async (req, res) => {
  if (req.method === 'GET' && req.url === '/health') {
    const hour = Number(new Intl.DateTimeFormat('en-US', {
      hour: '2-digit', hour12: false, timeZone: 'Asia/Shanghai'
    }).format(new Date())) % 24;
    return sendJson(res, 200, { ok: true, status: 'awake', hour });
  }

  if (req.method === 'GET' && req.url === '/usage') {
    if (!authorized(req)) return sendJson(res, 401, { ok: false, error: 'unauthorized' });
    try { return sendJson(res, 200, await getUsage()); }
    catch (error) { return sendJson(res, 502, { ok: false, error: error.message }); }
  }

  if (req.method === 'GET' && req.url === '/conversations') {
    if (!authorized(req)) return sendJson(res, 401, { ok: false, error: 'unauthorized' });
    const list = [...conversations].reverse().map(c => ({
      id: c.id,
      name: c.name || null,
      createdAt: c.createdAt,
    }));
    return sendJson(res, 200, { ok: true, conversations: list });
  }

  if (req.method === 'DELETE' && req.url.startsWith('/conversations/')) {
    if (!authorized(req)) return sendJson(res, 401, { ok: false, error: 'unauthorized' });
    const id = decodeURIComponent(req.url.slice('/conversations/'.length));
    const before = conversations.length;
    conversations = conversations.filter(c => c.id !== id);
    if (conversations.length < before) saveConversations(conversations);
    return sendJson(res, 200, { ok: true });
  }

  if (req.method !== 'POST' || req.url !== '/chat') {
    return sendJson(res, 404, { ok: false, error: 'not_found' });
  }
  if (!authorized(req)) {
    return sendJson(res, 401, { ok: false, error: 'unauthorized' });
  }

  try {
    const body = await readJson(req);
    const message = typeof body.message === 'string' ? body.message.trim() : '';
    const modelName = Object.hasOwn(MODELS, body.model) ? body.model : 'haiku';
    const model = MODELS[modelName];
    if (!message || message.length > 800) {
      return sendJson(res, 400, { ok: false, error: 'message_must_be_1_to_800_chars' });
    }

    const convId = typeof body.conversationId === 'string' && body.conversationId
      ? body.conversationId : null;
    let conv = convId ? conversations.find(c => c.id === convId) : null;
    const isNew = !conv;

    const result = await askClaude(
      message,
      model,
      conv ? conv.sessionId : null,
      isNew ? POCKET_SYSTEM_PROMPT : null
    );

    const newSessionId = result.session_id || (conv ? conv.sessionId : null);

    if (isNew && newSessionId) {
      const newId = crypto.randomBytes(12).toString('hex');
      conv = { id: newId, sessionId: newSessionId, name: null, createdAt: new Date().toISOString() };
      conversations.push(conv);
      saveConversations(conversations);
      nameConversationAsync(newId, message);
    } else if (conv && newSessionId && newSessionId !== conv.sessionId) {
      conv.sessionId = newSessionId;
      saveConversations(conversations);
    }

    return sendJson(res, 200, {
      ok: true,
      model: modelName,
      reply: result.result || '',
      conversationId: conv ? conv.id : null,
      usage: result.usage || null,
      modelUsage: result.modelUsage || null,
    });
  } catch (error) {
    return sendJson(res, 500, { ok: false, error: error.message });
  }
});

server.listen(PORT, HOST, () => {
  console.log(`Claude Cardputer bridge listening on http://${HOST}:${PORT}`);
});


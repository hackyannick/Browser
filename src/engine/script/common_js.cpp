// The JavaScript part of the APIs shared by pages and workers (see
// common.h). Kept free of DOM references.
namespace kite {

extern const char* const kCommonPrelude;
const char* const kCommonPrelude = R"KITEJS(
(function () {
'use strict';
const G = globalThis;
const KC = G.__kc;
function def(name, value) { Object.defineProperty(G, name, { value, writable: true, configurable: true, enumerable: false }); }

// ------------------------------------------------------------------ Errors
const DOM_CODES = { IndexSizeError: 1, HierarchyRequestError: 3, WrongDocumentError: 4, InvalidCharacterError: 5,
  NoModificationAllowedError: 7, NotFoundError: 8, NotSupportedError: 9, InvalidStateError: 11, SyntaxError: 12,
  InvalidModificationError: 13, NamespaceError: 14, InvalidAccessError: 15, SecurityError: 18, NetworkError: 19,
  AbortError: 20, URLMismatchError: 21, QuotaExceededError: 22, TimeoutError: 23, DataCloneError: 25 };
class DOMException extends Error {
  constructor(message, name) {
    super(message === undefined ? '' : String(message));
    this.name = name === undefined ? 'Error' : String(name);
    this.code = DOM_CODES[this.name] || 0;
  }
}
for (const k of Object.keys(DOM_CODES)) DOMException[k.replace(/([a-z])([A-Z])/g, '$1_$2').toUpperCase().replace(/_ERROR$/, '_ERR')] = DOM_CODES[k];

// ------------------------------------------------------------------ Bytes
function toU8(x) {
  if (x instanceof Uint8Array) return x;
  if (x instanceof ArrayBuffer) return new Uint8Array(x);
  if (ArrayBuffer.isView(x)) return new Uint8Array(x.buffer, x.byteOffset, x.byteLength);
  return null;
}
function concatBytes(list) {
  let n = 0;
  for (const a of list) n += a.length;
  const out = new Uint8Array(n);
  let o = 0;
  for (const a of list) { out.set(a, o); o += a.length; }
  return out;
}
function bytesToB64(u8) { return btoa(KC.latin1(u8)); }
function b64ToBytes(s) { return KC.fromLatin1(atob(s)); }

class TextEncoder {
  get encoding() { return 'utf-8'; }
  encode(s) { return KC.utf8Encode(s === undefined ? '' : String(s)); }
  encodeInto(s, dest) {
    const b = this.encode(s);
    const n = Math.min(b.length, dest.length);
    dest.set(b.subarray(0, n));
    return { read: n === b.length ? String(s).length : Math.floor(String(s).length * n / Math.max(1, b.length)), written: n };
  }
}
const ENC_ALIASES = { 'utf8': 'utf-8', 'unicode-1-1-utf-8': 'utf-8', 'latin1': 'windows-1252', 'iso-8859-1': 'windows-1252',
  'ascii': 'windows-1252', 'us-ascii': 'windows-1252', 'cp1252': 'windows-1252', 'utf-16': 'utf-16le' };
class TextDecoder {
  constructor(label, opts) {
    let enc = String(label === undefined ? 'utf-8' : label).trim().toLowerCase();
    enc = ENC_ALIASES[enc] || enc;
    this.encoding = enc;
    this.fatal = !!(opts && opts.fatal);
    this.ignoreBOM = !!(opts && opts.ignoreBOM);
    this._pending = null;
    this._bomSeen = false;
  }
  decode(input, opts) {
    let b = input === undefined || input === null ? new Uint8Array(0) : toU8(input);
    if (!b) throw new TypeError("Failed to execute 'decode' on 'TextDecoder': parameter is not an ArrayBuffer or view");
    if (this._pending) { b = concatBytes([this._pending, b]); this._pending = null; }
    const stream = !!(opts && opts.stream);
    if (this.encoding !== 'utf-8') return KC.decode(b, this.encoding);
    if (stream) {
      // Keep an incomplete trailing sequence for the next call.
      let cut = b.length;
      for (let i = Math.max(0, b.length - 3); i < b.length; i++) {
        const c = b[i];
        const need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (need > 1 && i + need > b.length) { cut = i; break; }
      }
      if (cut < b.length) { this._pending = b.slice(cut); b = b.subarray(0, cut); }
    }
    if (!this.ignoreBOM && !this._bomSeen && b.length >= 3 && b[0] === 0xEF && b[1] === 0xBB && b[2] === 0xBF) b = b.subarray(3);
    if (b.length) this._bomSeen = true;
    if (!stream) this._bomSeen = false;
    return KC.utf8Decode(b, this.fatal);
  }
}

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
function btoa(s) {
  s = String(s); let out = '';
  for (let i = 0; i < s.length; i += 3) {
    const a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2);
    if (a > 255 || b > 255 || c > 255) throw new DOMException("Failed to execute 'btoa': The string contains characters outside of the Latin1 range.", 'InvalidCharacterError');
    const n = (a << 16) | ((b || 0) << 8) | (c || 0);
    out += B64[n >> 18 & 63] + B64[n >> 12 & 63] + (isNaN(b) ? '=' : B64[n >> 6 & 63]) + (isNaN(c) ? '=' : B64[n & 63]);
  }
  return out;
}
function atob(s) {
  s = String(s).replace(/[\t\n\f\r ]/g, '');
  if (s.length % 4 === 0) s = s.replace(/==?$/, '');
  if (s.length % 4 === 1 || /[^A-Za-z0-9+\/]/.test(s)) throw new DOMException("Failed to execute 'atob': The string to be decoded is not correctly encoded.", 'InvalidCharacterError');
  let out = '', buf = 0, bits = 0;
  for (let i = 0; i < s.length; i++) {
    buf = (buf << 6) | B64.indexOf(s[i]); bits += 6;
    if (bits >= 8) { bits -= 8; out += String.fromCharCode((buf >> bits) & 255); }
  }
  return out;
}
class URLSearchParams {
  constructor(init) {
    this._p = [];
    if (init === undefined || init === null) return;
    if (init instanceof URLSearchParams) { this._p = init._p.map(p => [p[0], p[1]]); return; }
    if (typeof init === 'object' && !(init instanceof String)) {
      if (typeof init[Symbol.iterator] === 'function') {
        for (const pair of init) {
          const a = Array.from(pair);
          if (a.length !== 2) throw new TypeError("Failed to construct 'URLSearchParams': Sequence initializer must only contain pair elements");
          this._p.push([String(a[0]), String(a[1])]);
        }
      } else for (const k of Object.keys(init)) this._p.push([k, String(init[k])]);
      return;
    }
    let s = String(init);
    if (s[0] === '?') s = s.slice(1);
    const dec = x => { x = x.replace(/\+/g, ' '); try { return decodeURIComponent(x); } catch (e) { return x.replace(/%([0-9a-f]{2})/gi, (m, h) => String.fromCharCode(parseInt(h, 16))); } };
    for (const part of s.split('&')) {
      if (!part) continue;
      const i = part.indexOf('=');
      this._p.push(i < 0 ? [dec(part), ''] : [dec(part.slice(0, i)), dec(part.slice(i + 1))]);
    }
  }
  _update() { if (this._u) { const s = this.toString(); this._u._query = s === '' ? null : s; } }
  get size() { return this._p.length; }
  append(k, v) { this._p.push([String(k), String(v)]); this._update(); }
  delete(k, v) { k = String(k); this._p = this._p.filter(p => p[0] !== k || (v !== undefined && p[1] !== String(v))); this._update(); }
  get(k) { k = String(k); const p = this._p.find(p => p[0] === k); return p ? p[1] : null; }
  getAll(k) { k = String(k); return this._p.filter(p => p[0] === k).map(p => p[1]); }
  has(k, v) { k = String(k); return this._p.some(p => p[0] === k && (v === undefined || p[1] === String(v))); }
  set(k, v) {
    k = String(k); v = String(v);
    const i = this._p.findIndex(p => p[0] === k);
    if (i < 0) this._p.push([k, v]);
    else { this._p[i][1] = v; this._p = this._p.filter((p, j) => p[0] !== k || j === i); }
    this._update();
  }
  sort() {
    const idx = this._p.map((p, i) => [p, i]);
    idx.sort((a, b) => a[0][0] < b[0][0] ? -1 : a[0][0] > b[0][0] ? 1 : a[1] - b[1]);
    this._p = idx.map(x => x[0]);
    this._update();
  }
  forEach(fn, self) { for (const [k, v] of this._p.slice()) fn.call(self, v, k, this); }
  keys() { return this._p.map(p => p[0])[Symbol.iterator](); }
  values() { return this._p.map(p => p[1])[Symbol.iterator](); }
  entries() { return this._p.map(p => [p[0], p[1]])[Symbol.iterator](); }
  [Symbol.iterator]() { return this.entries(); }
  toString() {
    const enc = s => encodeURIComponent(s).replace(/%20/g, '+').replace(/[!'()~]/g, c => '%' + c.charCodeAt(0).toString(16).toUpperCase());
    return this._p.map(([k, v]) => enc(k) + '=' + enc(v)).join('&');
  }
}

// ------------------------------------------------------------------ URL (WHATWG URL Standard, simplified)
const SPECIAL = { 'ftp:': '21', 'file:': '', 'http:': '80', 'https:': '443', 'ws:': '80', 'wss:': '443' };
function pctEncode(s, set) {
  let out = '';
  for (const ch of s) {
    const c = ch.codePointAt(0);
    if (c < 0x21 || c > 0x7e || set.indexOf(ch) >= 0) {
      if (ch === '%' ) { out += ch; continue; }
      for (const b of KC.utf8Encode(ch)) out += '%' + (b < 16 ? '0' : '') + b.toString(16).toUpperCase();
    } else out += ch;
  }
  return out;
}
const FRAGMENT_SET = ' "<>`', QUERY_SET = ' "#<>', SPECIAL_QUERY_SET = ' "#<>\'', PATH_SET = ' "#<>?`{}',
  USERINFO_SET = ' "#<>?`{}/:;=@[\\]^|';
// Punycode (RFC 3492) for internationalized host names.
function punycode(label) {
  const base = 36, tMin = 1, tMax = 26, skew = 38, damp = 700;
  const cps = Array.from(label, c => c.codePointAt(0));
  let out = cps.filter(c => c < 0x80).map(c => String.fromCharCode(c)).join('');
  const b = out.length;
  let h = b, n = 128, delta = 0, bias = 72;
  if (b) out += '-';
  const adapt = (d, num, first) => {
    d = first ? Math.floor(d / damp) : d >> 1;
    d += Math.floor(d / num);
    let k = 0;
    while (d > ((base - tMin) * tMax) >> 1) { d = Math.floor(d / (base - tMin)); k += base; }
    return k + Math.floor((base - tMin + 1) * d / (d + skew));
  };
  const digit = d => String.fromCharCode(d + 22 + 75 * (d < 26));
  while (h < cps.length) {
    const m = Math.min(...cps.filter(c => c >= n));
    delta += (m - n) * (h + 1);
    n = m;
    for (const c of cps) {
      if (c < n) delta++;
      if (c === n) {
        let q = delta;
        for (let k = base; ; k += base) {
          const t = k <= bias ? tMin : k >= bias + tMax ? tMax : k - bias;
          if (q < t) break;
          out += digit(t + (q - t) % (base - t));
          q = Math.floor((q - t) / (base - t));
        }
        out += digit(q);
        bias = adapt(delta, h + 1, h === b);
        delta = 0;
        h++;
      }
    }
    delta++; n++;
  }
  return 'xn--' + out;
}
function parseHost(h, special) {
  if (h.startsWith('[')) {
    if (!h.endsWith(']')) throw new TypeError('Invalid URL');
    return h.toLowerCase();
  }
  if (!special) return pctEncode(h, ' #/<>?@[\\]^|');
  try { h = decodeURIComponent(h); } catch (e) {}
  h = h.toLowerCase();
  if (/[\x00-\x20#%\/:<>?@[\\\]^|]/.test(h)) throw new TypeError('Invalid URL');
  if (/[^\x00-\x7f]/.test(h)) h = h.split('.').map(l => /[^\x00-\x7f]/.test(l) ? punycode(l.normalize ? l.normalize('NFC') : l) : l).join('.');
  // Numeric IPv4 forms (0x7f.1 ...) are normalized.
  const parts = h.split('.');
  if (parts.length <= 4 && parts.every(p => /^(0x[0-9a-f]*|\d+)$/i.test(p))) {
    const nums = parts.map(p => /^0x/i.test(p) ? parseInt(p.slice(2) || '0', 16) : /^0\d/.test(p) ? parseInt(p, 8) : parseInt(p, 10));
    let v = nums[nums.length - 1];
    for (let i = 0; i < nums.length - 1; i++) { if (nums[i] > 255) throw new TypeError('Invalid URL'); v += nums[i] * Math.pow(256, 3 - i); }
    if (v > 0xffffffff) throw new TypeError('Invalid URL');
    return [v >>> 24, (v >>> 16) & 255, (v >>> 8) & 255, v & 255].join('.');
  }
  return h;
}
function normPath(segs, special) {
  const out = [];
  for (let i = 0; i < segs.length; i++) {
    const s = segs[i], low = s.toLowerCase();
    const dot = s === '.' || low === '%2e', dotdot = s === '..' || low === '.%2e' || low === '%2e.' || low === '%2e%2e';
    if (dotdot) { if (out.length) out.pop(); if (i === segs.length - 1) out.push(''); }
    else if (dot) { if (i === segs.length - 1) out.push(''); }
    else out.push(s);
  }
  return out;
}
class URL {
  constructor(url, base) {
    url = String(url);
    let b = null;
    if (base !== undefined) b = base instanceof URL ? base : new URL(String(base));
    this._parse(url, b);
  }
  _parse(input, base) {
    let s = input.replace(/^[\x00-\x20]+|[\x00-\x20]+$/g, '').replace(/[\t\n\r]/g, '');
    const m = /^([a-zA-Z][a-zA-Z0-9+.-]*):/.exec(s);
    let scheme = m ? m[1].toLowerCase() + ':' : null;
    let rest = m ? s.slice(m[0].length) : s;
    if (scheme && base && scheme === base._scheme && SPECIAL[scheme] !== undefined && !/^[\/\\]/.test(rest)) {
      // "http:foo" relative to an http: base.
      scheme = null;
      s = rest;
    }
    if (!scheme) {
      if (!base) throw new TypeError("Failed to construct 'URL': Invalid URL");
      return this._relative(scheme === null ? s : rest, base);
    }
    this._scheme = scheme;
    const special = SPECIAL[scheme] !== undefined;
    if (special) rest = rest.replace(/\\/g, '/');
    this._user = ''; this._pass = ''; this._host = null; this._port = ''; this._query = null; this._frag = null;
    let h = rest.indexOf('#');
    if (h >= 0) { this._frag = pctEncode(rest.slice(h + 1), FRAGMENT_SET); rest = rest.slice(0, h); }
    const q = rest.indexOf('?');
    if (q >= 0) { this._query = pctEncode(rest.slice(q + 1), special ? SPECIAL_QUERY_SET : QUERY_SET); rest = rest.slice(0, q); }
    if (rest.startsWith('//') || (special && scheme !== 'file:')) {
      rest = special && scheme !== 'file:' ? rest.replace(/^\/*/, '') : rest.replace(/^\/\//, '');
      const end = rest.search(/[\/]/);
      const auth = end < 0 ? rest : rest.slice(0, end);
      rest = end < 0 ? '' : rest.slice(end);
      this._authority(auth, special);
      if (special && scheme !== 'file:' && !this._host) throw new TypeError("Failed to construct 'URL': Invalid URL");
      this._path = special || rest ? normPath((rest || '/').split('/').slice(1).map(p => pctEncode(p, PATH_SET)), special) : [];
      this._opaque = false;
    } else if (rest.startsWith('/')) {
      this._host = scheme === 'file:' ? '' : null;
      this._path = normPath(rest.split('/').slice(1).map(p => pctEncode(p, PATH_SET)), special);
      this._opaque = false;
    } else {
      this._opaque = true;
      this._path = rest.replace(/[\x00-\x1f\x7f-\uffff]/g, c => pctEncode(c, ''));
    }
    this._sync();
  }
  _authority(auth, special) {
    const at = auth.lastIndexOf('@');
    if (at >= 0) {
      const cred = auth.slice(0, at);
      auth = auth.slice(at + 1);
      const c = cred.indexOf(':');
      this._user = pctEncode(c < 0 ? cred : cred.slice(0, c), USERINFO_SET);
      this._pass = c < 0 ? '' : pctEncode(cred.slice(c + 1), USERINFO_SET);
    }
    let host = auth, port = '';
    const pm = /:(\d*)$/.exec(auth);
    if (pm && !(auth.startsWith('[') && !/\]:\d*$/.test(auth))) { host = auth.slice(0, pm.index); port = pm[1]; }
    else if (/:[^\]]*$/.test(auth) && !auth.startsWith('[')) throw new TypeError("Failed to construct 'URL': Invalid URL");
    this._host = parseHost(host, special);
    if (port !== '') {
      const p = parseInt(port, 10);
      if (p > 65535) throw new TypeError("Failed to construct 'URL': Invalid URL");
      this._port = String(p) === SPECIAL[this._scheme] ? '' : String(p);
    }
  }
  _relative(s, b) {
    const special = SPECIAL[b._scheme] !== undefined;
    if (special) s = s.replace(/\\/g, '/');
    if (b._opaque && !s.startsWith('#')) throw new TypeError("Failed to construct 'URL': Invalid URL");
    if (s.startsWith('//')) return this._parse(b._scheme + s, null);
    Object.assign(this, { _scheme: b._scheme, _user: b._user, _pass: b._pass, _host: b._host, _port: b._port, _opaque: b._opaque });
    let frag = null, query = b._query;
    const h = s.indexOf('#');
    if (h >= 0) { frag = pctEncode(s.slice(h + 1), FRAGMENT_SET); s = s.slice(0, h); }
    const q = s.indexOf('?');
    if (q >= 0) { query = pctEncode(s.slice(q + 1), special ? SPECIAL_QUERY_SET : QUERY_SET); s = s.slice(0, q); }
    else if (s !== '') query = null;
    this._query = query;
    this._frag = frag;
    if (b._opaque) this._path = b._path;
    else if (s === '') this._path = b._path.slice();
    else if (s.startsWith('/')) this._path = normPath(s.split('/').slice(1).map(p => pctEncode(p, PATH_SET)), special);
    else this._path = normPath(b._path.slice(0, -1).concat(s.split('/').map(p => pctEncode(p, PATH_SET))), special);
    this._sync();
  }
  _sync() {
    if (!this._sp) { this._sp = new URLSearchParams(); this._sp._u = this; }
    this._sp._p = new URLSearchParams(this._query || '')._p;
  }
  get searchParams() { return this._sp; }
  get protocol() { return this._scheme; }
  set protocol(v) {
    const m = /^([a-zA-Z][a-zA-Z0-9+.-]*)/.exec(String(v));
    if (!m) return;
    const s = m[1].toLowerCase() + ':';
    if ((SPECIAL[s] !== undefined) !== (SPECIAL[this._scheme] !== undefined)) return;
    this._scheme = s;
    if (this._port === SPECIAL[s]) this._port = '';
  }
  get username() { return this._user; }
  set username(v) { if (this._host) this._user = pctEncode(String(v), USERINFO_SET); }
  get password() { return this._pass; }
  set password(v) { if (this._host) this._pass = pctEncode(String(v), USERINFO_SET); }
  get hostname() { return this._host || ''; }
  set hostname(v) { if (!this._opaque) try { this._host = parseHost(String(v), SPECIAL[this._scheme] !== undefined); } catch (e) {} }
  get port() { return this._port; }
  set port(v) {
    v = String(v);
    if (v === '') { this._port = ''; return; }
    const m = /^\d+/.exec(v);
    if (!m || +m[0] > 65535 || !this._host) return;
    this._port = String(+m[0]) === SPECIAL[this._scheme] ? '' : String(+m[0]);
  }
  get host() { return (this._host || '') + (this._port ? ':' + this._port : ''); }
  set host(v) {
    v = String(v);
    const m = /^(\[[^\]]*\]|[^:\/?#]*)(?::(\d*))?/.exec(v);
    if (!m || !m[1]) return;
    try { this._host = parseHost(m[1], SPECIAL[this._scheme] !== undefined); } catch (e) { return; }
    if (m[2] !== undefined) this.port = m[2];
  }
  get origin() {
    if (this._scheme === 'blob:') { try { return new URL(this._path).origin; } catch (e) { return 'null'; } }
    return SPECIAL[this._scheme] !== undefined && this._scheme !== 'file:' ? this._scheme + '//' + this.host : 'null';
  }
  get pathname() { return this._opaque ? this._path : '/' + this._path.join('/'); }
  set pathname(v) {
    if (this._opaque) return;
    v = String(v);
    const special = SPECIAL[this._scheme] !== undefined;
    if (special) v = v.replace(/\\/g, '/');
    const segs = (v.startsWith('/') ? v.slice(1) : v).split('/');
    this._path = normPath(segs.map(p => pctEncode(p, PATH_SET)), special);
  }
  get search() { return this._query ? '?' + this._query : ''; }
  set search(v) {
    v = String(v);
    if (v.startsWith('?')) v = v.slice(1);
    this._query = v === '' ? null : pctEncode(v, SPECIAL[this._scheme] !== undefined ? SPECIAL_QUERY_SET : QUERY_SET);
    this._sync();
  }
  get hash() { return this._frag ? '#' + this._frag : ''; }
  set hash(v) {
    v = String(v);
    if (v.startsWith('#')) v = v.slice(1);
    this._frag = v === '' ? null : pctEncode(v, FRAGMENT_SET);
  }
  get href() {
    let s = this._scheme;
    if (this._host !== null) {
      s += '//';
      if (this._user || this._pass) s += this._user + (this._pass ? ':' + this._pass : '') + '@';
      s += this.host;
    } else if (!this._opaque && this._path.length > 1 && this._path[0] === '') s += '/.';
    s += this.pathname;
    if (this._query !== null) s += '?' + this._query;
    if (this._frag !== null) s += '#' + this._frag;
    return s;
  }
  set href(v) { const u = new URL(String(v)); Object.assign(this, u); this._sp._u = this; }
  toString() { return this.href; }
  toJSON() { return this.href; }
  // Object URLs are data: URLs (good enough for images, downloads, workers).
  static createObjectURL(obj) {
    if (obj instanceof Blob) return 'data:' + (obj.type || 'application/octet-stream') + ';base64,' + bytesToB64(obj._b);
    return 'data:,';
  }
  static revokeObjectURL() {}
  static canParse(u, b) { try { new URL(u, b); return true; } catch (e) { return false; } }
  static parse(u, b) { try { return new URL(u, b); } catch (e) { return null; } }
}

// ------------------------------------------------------------------ Fetch bodies
class Headers {
  constructor(init) {
    this._h = new Map();
    if (init instanceof Headers) init.forEach((v, k) => this.append(k, v));
    else if (Array.isArray(init) || (init && typeof init[Symbol.iterator] === 'function')) for (const [k, v] of init) this.append(k, v);
    else if (init) for (const k of Object.keys(init)) this.append(k, init[k]);
  }
  append(k, v) { k = String(k).toLowerCase(); v = String(v).trim(); this._h.set(k, this._h.has(k) ? this._h.get(k) + ', ' + v : v); }
  set(k, v) { this._h.set(String(k).toLowerCase(), String(v).trim()); }
  get(k) { const v = this._h.get(String(k).toLowerCase()); return v === undefined ? null : v; }
  getSetCookie() { const v = this.get('set-cookie'); return v ? [v] : []; }
  has(k) { return this._h.has(String(k).toLowerCase()); }
  delete(k) { this._h.delete(String(k).toLowerCase()); }
  forEach(fn, self) { for (const [k, v] of Array.from(this._h).sort()) fn.call(self, v, k, this); }
  entries() { return Array.from(this._h).sort()[Symbol.iterator](); }
  keys() { return Array.from(this._h.keys()).sort()[Symbol.iterator](); }
  values() { return Array.from(this._h).sort().map(e => e[1])[Symbol.iterator](); }
  [Symbol.iterator]() { return this.entries(); }
}

// Bytes of a body-like value (strings are UTF-8).
function bodyBytes(x) {
  if (x === undefined || x === null) return new Uint8Array(0);
  if (typeof x === 'string') return KC.utf8Encode(x);
  if (x instanceof Blob) return x._b;
  const u = toU8(x);
  if (u) return new Uint8Array(u);
  if (x instanceof URLSearchParams) return KC.utf8Encode(x.toString());
  return KC.utf8Encode(String(x));
}
class Blob {
  constructor(parts, opts) {
    const list = [];
    for (const p of parts || []) list.push(bodyBytes(p));
    this._b = concatBytes(list);
    this.type = opts && opts.type ? String(opts.type).toLowerCase() : '';
  }
  get size() { return this._b.length; }
  text() { return Promise.resolve(KC.utf8Decode(this._b)); }
  arrayBuffer() { return Promise.resolve(this._b.slice().buffer); }
  bytes() { return Promise.resolve(this._b.slice()); }
  slice(a, b, type) {
    const n = this._b.length;
    a = a === undefined ? 0 : a < 0 ? Math.max(n + a, 0) : Math.min(a, n);
    b = b === undefined ? n : b < 0 ? Math.max(n + b, 0) : Math.min(b, n);
    const r = new Blob([], { type: type === undefined ? this.type : type });
    r._b = this._b.slice(a, Math.max(a, b));
    return r;
  }
  stream() { const b = this._b; return new ReadableStream({ start(c) { if (b.length) c.enqueue(b.slice()); c.close(); } }); }
}
class File extends Blob {
  constructor(parts, name, opts) { super(parts, opts); this.name = String(name); this.lastModified = opts && opts.lastModified || Date.now(); }
  get webkitRelativePath() { return ''; }
}
class FileReader {
  constructor() { this.readyState = 0; this.result = null; this.error = null; this.onload = this.onloadend = this.onerror = this.onprogress = this.onloadstart = this.onabort = null; this._l = {}; }
  addEventListener(t, f) { (this._l[t] = this._l[t] || []).push(f); }
  removeEventListener(t, f) { if (this._l[t]) this._l[t] = this._l[t].filter(x => x !== f); }
  _done(result) {
    this.readyState = 2; this.result = result;
    for (const t of ['load', 'loadend']) {
      const ev = { type: t, target: this, currentTarget: this, loaded: 0, total: 0 };
      if (typeof this['on' + t] === 'function') this['on' + t](ev);
      for (const f of this._l[t] || []) f.call(this, ev);
    }
  }
  _read(blob, how) {
    this.readyState = 1;
    const b = blob._b;
    Promise.resolve().then(() => this._done(how === 'text' ? KC.utf8Decode(b) : how === 'url'
      ? 'data:' + (blob.type || 'application/octet-stream') + ';base64,' + bytesToB64(b)
      : how === 'binary' ? KC.latin1(b) : b.slice().buffer));
  }
  readAsText(b) { this._read(b, 'text'); }
  readAsDataURL(b) { this._read(b, 'url'); }
  readAsArrayBuffer(b) { this._read(b, 'buffer'); }
  readAsBinaryString(b) { this._read(b, 'binary'); }
  abort() { this.readyState = 2; }
}
FileReader.EMPTY = 0; FileReader.LOADING = 1; FileReader.DONE = 2;

function readAllBytes(stream) {
  const reader = stream.getReader();
  const chunks = [];
  const pump = () => reader.read().then(({ value, done }) => {
    if (done) return concatBytes(chunks);
    chunks.push(typeof value === 'string' ? KC.utf8Encode(value) : bodyBytes(value));
    return pump();
  });
  return pump();
}
class BodyMixin {
  _initBody(body) {
    this._stream = null;
    this._bytes = null;
    if (body instanceof ReadableStream) this._stream = body;
    else if (body !== undefined && body !== null) this._bytes = bodyBytes(body);
    this._hasBody = body !== undefined && body !== null;
    this.bodyUsed = false;
  }
  get body() {
    if (!this._hasBody) return null;
    if (!this._stream) { const b = this._bytes; this._stream = new ReadableStream({ start(c) { if (b.length) c.enqueue(b); c.close(); } }); }
    return this._stream;
  }
  _consume() {
    if (this.bodyUsed) return Promise.reject(new TypeError('Body has already been consumed.'));
    this.bodyUsed = true;
    if (this._bytes && !this._stream) return Promise.resolve(this._bytes);
    if (this._stream) return readAllBytes(this._stream);
    return Promise.resolve(new Uint8Array(0));
  }
  text() { return this._consume().then(b => KC.utf8Decode(b)); }
  json() { return this.text().then(t => JSON.parse(t)); }
  arrayBuffer() { return this._consume().then(b => b.slice().buffer); }
  bytes() { return this._consume().then(b => b.slice()); }
  blob() { return this._consume().then(b => { const r = new Blob([], { type: this.headers.get('content-type') || '' }); r._b = b; return r; }); }
  formData() {
    return this.text().then(t => { const fd = new FormData(); for (const [k, v] of new URLSearchParams(t)) fd.append(k, v); return fd; });
  }
}
class Response extends BodyMixin {
  constructor(body, init) {
    super();
    init = init || {};
    this._initBody(body);
    this.status = init.status === undefined ? 200 : init.status | 0;
    this.statusText = init.statusText === undefined ? '' : String(init.statusText);
    this.headers = init.headers instanceof Headers ? init.headers : new Headers(init.headers);
    if (typeof body === 'string' && !this.headers.has('content-type')) this.headers.set('content-type', 'text/plain;charset=UTF-8');
    this.url = init.url || '';
    this.redirected = !!init.redirected;
    this.type = init.type || 'basic';
  }
  get ok() { return this.status >= 200 && this.status < 300; }
  clone() {
    if (this.bodyUsed) throw new TypeError('Response body is already used');
    let body = this._bytes;
    if (this._stream) { const [a, b] = this._stream.tee(); this._stream = a; body = b; }
    return new Response(body, { status: this.status, statusText: this.statusText, headers: new Headers(this.headers), url: this.url });
  }
  static json(d, i) { i = Object.assign({}, i); i.headers = new Headers(i.headers); if (!i.headers.has('content-type')) i.headers.set('content-type', 'application/json'); return new Response(JSON.stringify(d), i); }
  static error() { return new Response(null, { status: 0, type: 'error' }); }
  static redirect(url, status) { return new Response(null, { status: status || 302, headers: { location: String(url) } }); }
}
class Request extends BodyMixin {
  constructor(input, init) {
    super();
    init = init || {};
    const src = input instanceof Request ? input : null;
    this.url = src ? src.url : String(input instanceof URL ? input.href : input);
    this.method = String(init.method || (src ? src.method : 'GET')).toUpperCase();
    this.headers = new Headers(init.headers || (src ? src.headers : undefined));
    const body = init.body !== undefined ? init.body : src ? src._bytes : null;
    this._initBody(body);
    this._raw = init.body !== undefined ? init.body : src ? src._raw : null;
    this.signal = init.signal || (src ? src.signal : null);
    this.credentials = init.credentials || (src ? src.credentials : 'same-origin');
    this.mode = init.mode || (src ? src.mode : 'cors');
    this.cache = init.cache || 'default';
    this.redirect = init.redirect || 'follow';
    this.referrer = 'about:client';
    this.integrity = init.integrity || '';
    this.keepalive = !!init.keepalive;
    this.destination = '';
  }
  clone() { return new Request(this); }
}
class FormData {
  constructor(form) {
    this._p = [];
    if (form && form.elements) {
      for (const el of form.elements) {
        const name = el.name; if (!name || el.disabled) continue;
        const type = el.type;
        if ((type === 'checkbox' || type === 'radio') && !el.checked) continue;
        if (type === 'submit' || type === 'button' || type === 'reset' || type === 'file' || type === 'image') continue;
        if (el.localName === 'select' && el.multiple) { for (const o of el.options) if (o.selected) this._p.push([name, o.value]); continue; }
        this._p.push([name, el.value]);
      }
    }
  }
  append(k, v, fn) { this._p.push([String(k), v instanceof Blob ? (fn !== undefined || !(v instanceof File) ? new File([v], fn === undefined ? 'blob' : fn, { type: v.type }) : v) : String(v)]); }
  set(k, v, fn) { const i = this._p.findIndex(p => p[0] === String(k)); this.delete(k); this.append(k, v, fn); if (i >= 0) this._p.splice(i, 0, this._p.pop()); }
  get(k) { const p = this._p.find(p => p[0] === String(k)); return p ? p[1] : null; }
  getAll(k) { return this._p.filter(p => p[0] === String(k)).map(p => p[1]); }
  has(k) { return this._p.some(p => p[0] === String(k)); }
  delete(k) { this._p = this._p.filter(p => p[0] !== String(k)); }
  forEach(fn, self) { for (const [k, v] of this._p) fn.call(self, v, k, this); }
  entries() { return this._p.map(p => [p[0], p[1]])[Symbol.iterator](); }
  keys() { return this._p.map(p => p[0])[Symbol.iterator](); }
  values() { return this._p.map(p => p[1])[Symbol.iterator](); }
  [Symbol.iterator]() { return this.entries(); }
}

// ------------------------------------------------------------------ Streams
class ReadableStreamDefaultController {
  constructor(s) { this._s = s; }
  get desiredSize() { const s = this._s; return s._state === 'errored' ? null : s._state === 'closed' ? 0 : s._hwm - s._queued; }
  enqueue(chunk) {
    const s = this._s;
    if (s._closeRequested || s._state !== 'readable') throw new TypeError('Cannot enqueue a chunk into a closed stream');
    s._enqueue(chunk);
  }
  close() {
    const s = this._s;
    if (s._closeRequested || s._state !== 'readable') throw new TypeError('Cannot close a closed stream');
    s._closeRequested = true;
    if (!s._q.length) s._finish();
  }
  error(e) { this._s._fail(e); }
}
class ReadableStreamDefaultReader {
  constructor(stream) {
    if (!(stream instanceof ReadableStream)) throw new TypeError('not a ReadableStream');
    if (stream._reader) throw new TypeError('ReadableStream is locked');
    this._s = stream;
    stream._reader = this;
    this._reqs = [];
    this.closed = new Promise((res, rej) => { this._res = res; this._rej = rej; });
    this.closed.catch(() => {});
    if (stream._state === 'closed') this._res();
    else if (stream._state === 'errored') this._rej(stream._err);
  }
  read() {
    const s = this._s;
    if (!s) return Promise.reject(new TypeError('This reader has been released'));
    if (s._q.length) {
      const v = s._q.shift();
      s._queued -= s._sizeOf(v);
      if (s._closeRequested && !s._q.length) s._finish();
      else s._pull();
      return Promise.resolve({ value: v, done: false });
    }
    if (s._state === 'closed') return Promise.resolve({ value: undefined, done: true });
    if (s._state === 'errored') return Promise.reject(s._err);
    return new Promise((resolve, reject) => { this._reqs.push({ resolve, reject }); s._pull(); });
  }
  releaseLock() {
    if (!this._s) return;
    for (const r of this._reqs) r.reject(new TypeError('Reader was released'));
    this._reqs = [];
    this._s._reader = null;
    this._s = null;
  }
  cancel(reason) { return this._s ? this._s._cancel(reason) : Promise.reject(new TypeError('This reader has been released')); }
}
class ReadableStream {
  constructor(source, strategy) {
    source = source || {};
    strategy = strategy || {};
    this._source = source;
    this._q = [];
    this._queued = 0;
    this._state = 'readable';
    this._err = undefined;
    this._reader = null;
    this._started = false;
    this._pulling = false;
    this._pullAgain = false;
    this._closeRequested = false;
    this._hwm = strategy.highWaterMark !== undefined ? +strategy.highWaterMark : (source.type === 'bytes' ? 0 : 1);
    this._sizeFn = typeof strategy.size === 'function' ? strategy.size : null;
    const c = this._ctrl = new ReadableStreamDefaultController(this);
    if (source.type === 'bytes') { c.byobRequest = null; }
    let r;
    try { r = source.start ? source.start(c) : undefined; } catch (e) { this._fail(e); return; }
    Promise.resolve(r).then(() => { this._started = true; this._pull(); }, e => this._fail(e));
  }
  _sizeOf(c) { return this._sizeFn ? +this._sizeFn(c) || 0 : 1; }
  get locked() { return !!this._reader; }
  getReader(opts) { return new ReadableStreamDefaultReader(this); }
  cancel(reason) { return this._reader ? Promise.reject(new TypeError('Cannot cancel a locked stream')) : this._cancel(reason); }
  _cancel(reason) {
    if (this._state === 'closed') return Promise.resolve();
    if (this._state === 'errored') return Promise.reject(this._err);
    this._q = [];
    this._queued = 0;
    this._finish();
    let r;
    try { r = this._source.cancel ? this._source.cancel(reason) : undefined; } catch (e) { return Promise.reject(e); }
    return Promise.resolve(r).then(() => undefined);
  }
  _enqueue(chunk) {
    const r = this._reader;
    if (r && r._reqs.length) r._reqs.shift().resolve({ value: chunk, done: false });
    else { this._q.push(chunk); this._queued += this._sizeOf(chunk); }
    this._pull();
  }
  _finish() {
    if (this._state !== 'readable') return;
    this._state = 'closed';
    const r = this._reader;
    if (r) { for (const q of r._reqs) q.resolve({ value: undefined, done: true }); r._reqs = []; r._res(); }
  }
  _fail(e) {
    if (this._state !== 'readable') return;
    this._state = 'errored';
    this._err = e;
    this._q = [];
    const r = this._reader;
    if (r) { for (const q of r._reqs) q.reject(e); r._reqs = []; r._rej(e); }
  }
  _pull() {
    if (!this._started || this._state !== 'readable' || this._closeRequested || !this._source.pull) return;
    const wanted = (this._reader && this._reader._reqs.length > 0) || this._hwm - this._queued > 0;
    if (!wanted) return;
    if (this._pulling) { this._pullAgain = true; return; }
    this._pulling = true;
    let p;
    try { p = Promise.resolve(this._source.pull(this._ctrl)); } catch (e) { this._fail(e); return; }
    p.then(() => { this._pulling = false; if (this._pullAgain) { this._pullAgain = false; this._pull(); } }, e => this._fail(e));
  }
  tee() {
    const reader = this.getReader();
    let c1, c2;
    const pull = () => reader.read().then(({ value, done }) => {
      if (done) { try { c1.close(); } catch (e) {} try { c2.close(); } catch (e) {} return; }
      try { c1.enqueue(value); } catch (e) {}
      try { c2.enqueue(value); } catch (e) {}
    }, e => { c1.error(e); c2.error(e); });
    const a = new ReadableStream({ start(c) { c1 = c; }, pull });
    const b = new ReadableStream({ start(c) { c2 = c; }, pull });
    return [a, b];
  }
  pipeTo(dest, opts) {
    opts = opts || {};
    const reader = this.getReader();
    const writer = dest.getWriter();
    return new Promise((resolve, reject) => {
      const step = () => reader.read().then(({ value, done }) => {
        if (done) {
          reader.releaseLock();
          if (opts.preventClose) { writer.releaseLock(); resolve(); } else writer.close().then(() => { writer.releaseLock(); resolve(); }, reject);
          return;
        }
        return writer.write(value).then(step);
      }).catch(e => { if (!opts.preventAbort) writer.abort(e).catch(() => {}); reject(e); });
      if (opts.signal) opts.signal.addEventListener('abort', () => { reader.cancel(opts.signal.reason); reject(opts.signal.reason); });
      step();
    });
  }
  pipeThrough(pair, opts) { this.pipeTo(pair.writable, opts).catch(() => {}); return pair.readable; }
  values(opts) {
    const reader = this.getReader();
    const keep = opts && opts.preventCancel;
    return {
      next: () => reader.read(),
      return: v => { if (!keep) reader.cancel(v); else reader.releaseLock(); return Promise.resolve({ value: v, done: true }); },
      [Symbol.asyncIterator]() { return this; },
    };
  }
  [Symbol.asyncIterator](opts) { return this.values(opts); }
  static from(src) {
    if (src instanceof ReadableStream) return src;
    const it = src[Symbol.asyncIterator] ? src[Symbol.asyncIterator]() : src[Symbol.iterator]();
    return new ReadableStream({
      pull(c) { return Promise.resolve(it.next()).then(r => { if (r.done) c.close(); else c.enqueue(r.value); }); },
      cancel(r) { if (it.return) it.return(r); },
    });
  }
}
class WritableStreamDefaultWriter {
  constructor(stream) {
    if (stream._writer) throw new TypeError('WritableStream is locked');
    this._s = stream;
    stream._writer = this;
    this.closed = stream._closedP;
  }
  get ready() { return this._s ? this._s._chain.then(() => undefined) : Promise.reject(new TypeError('released')); }
  get desiredSize() { return this._s && this._s._state === 'writable' ? 1 : 0; }
  write(chunk) { return this._s ? this._s._write(chunk) : Promise.reject(new TypeError('released')); }
  close() { return this._s ? this._s._close() : Promise.reject(new TypeError('released')); }
  abort(r) { return this._s ? this._s._abort(r) : Promise.reject(new TypeError('released')); }
  releaseLock() { if (this._s) { this._s._writer = null; this._s = null; } }
}
class WritableStream {
  constructor(sink, strategy) {
    sink = sink || {};
    this._sink = sink;
    this._state = 'writable';
    this._writer = null;
    this._closedP = new Promise((res, rej) => { this._closedRes = res; this._closedRej = rej; });
    this._closedP.catch(() => {});
    const ac = new AbortController();
    this._ctrl = { error: e => this._fail(e), signal: ac.signal };
    this._ac = ac;
    let r;
    try { r = sink.start ? sink.start(this._ctrl) : undefined; } catch (e) { r = Promise.reject(e); }
    this._chain = Promise.resolve(r);
    this._chain.catch(e => this._fail(e));
  }
  get locked() { return !!this._writer; }
  getWriter() { return new WritableStreamDefaultWriter(this); }
  close() { return this._writer ? Promise.reject(new TypeError('locked')) : this._close(); }
  abort(r) { return this._writer ? Promise.reject(new TypeError('locked')) : this._abort(r); }
  _fail(e) { if (this._state === 'errored') return; this._state = 'errored'; this._err = e; this._closedRej(e); }
  _write(chunk) {
    if (this._state !== 'writable') return Promise.reject(this._err || new TypeError('The stream is closed'));
    this._chain = this._chain.then(() => this._sink.write ? this._sink.write(chunk, this._ctrl) : undefined);
    this._chain.catch(e => this._fail(e));
    return this._chain;
  }
  _close() {
    if (this._state !== 'writable') return Promise.reject(this._err || new TypeError('The stream is closed'));
    this._state = 'closing';
    this._chain = this._chain.then(() => this._sink.close ? this._sink.close() : undefined).then(() => { this._state = 'closed'; this._closedRes(); });
    this._chain.catch(e => this._fail(e));
    return this._chain;
  }
  _abort(r) {
    this._ac.abort(r);
    this._fail(r);
    return Promise.resolve(this._sink.abort ? this._sink.abort(r) : undefined);
  }
}
class TransformStream {
  constructor(transformer, wStrategy, rStrategy) {
    transformer = transformer || {};
    let rc;
    this.readable = new ReadableStream({ start(c) { rc = c; } }, rStrategy);
    const tc = {
      enqueue: c => rc.enqueue(c),
      error: e => { rc.error(e); },
      terminate: () => { try { rc.close(); } catch (e) {} },
      get desiredSize() { return rc.desiredSize; },
    };
    let start = transformer.start ? transformer.start(tc) : undefined;
    this.writable = new WritableStream({
      start: () => start,
      write: chunk => transformer.transform ? transformer.transform(chunk, tc) : tc.enqueue(chunk),
      close: () => Promise.resolve(transformer.flush ? transformer.flush(tc) : undefined).then(() => { try { rc.close(); } catch (e) {} }),
      abort: r => rc.error(r),
    }, wStrategy);
  }
}
class TextDecoderStream extends TransformStream {
  constructor(label, opts) {
    const d = new TextDecoder(label, opts);
    super({ transform(c, ctl) { const s = d.decode(c, { stream: true }); if (s) ctl.enqueue(s); }, flush(ctl) { const s = d.decode(); if (s) ctl.enqueue(s); } });
    this.encoding = d.encoding;
  }
}
class TextEncoderStream extends TransformStream {
  constructor() { const e = new TextEncoder(); super({ transform(c, ctl) { ctl.enqueue(e.encode(c)); } }); this.encoding = 'utf-8'; }
}
class CountQueuingStrategy { constructor(o) { this.highWaterMark = o.highWaterMark; } size() { return 1; } }
class ByteLengthQueuingStrategy { constructor(o) { this.highWaterMark = o.highWaterMark; } size(c) { return c.byteLength; } }

// ------------------------------------------------------------------ Events (shared base)
class AbortSignal {
  constructor() { this.aborted = false; this.reason = undefined; this.onabort = null; this._l = []; }
  addEventListener(t, fn) { if (t === 'abort' && fn) this._l.push(fn); }
  removeEventListener(t, fn) { this._l = this._l.filter(f => f !== fn); }
  dispatchEvent(ev) { if (typeof this.onabort === 'function') this.onabort(ev); for (const f of this._l.slice()) { try { typeof f === 'function' ? f.call(this, ev) : f.handleEvent(ev); } catch (e) { setTimeout(() => { throw e; }, 0); } } return true; }
  throwIfAborted() { if (this.aborted) throw this.reason; }
  static timeout(ms) { const c = new AbortController(); setTimeout(() => c.abort(new DOMException('signal timed out', 'TimeoutError')), ms); return c.signal; }
  static abort(r) { const c = new AbortController(); c.abort(r); return c.signal; }
  static any(signals) { const c = new AbortController(); for (const s of signals) { if (s.aborted) { c.abort(s.reason); break; } s.addEventListener('abort', () => c.abort(s.reason)); } return c.signal; }
}
class AbortController {
  constructor() { this.signal = new AbortSignal(); }
  abort(reason) {
    const s = this.signal; if (s.aborted) return;
    s.aborted = true; s.reason = reason === undefined ? new DOMException('signal is aborted without reason', 'AbortError') : reason;
    s.dispatchEvent({ type: 'abort', target: s, currentTarget: s });
  }
}
// ------------------------------------------------------------------ Structured clone
// Values are encoded as JSON-safe trees (used by structuredClone and
// postMessage between threads).
const TYPED = ['Int8Array', 'Uint8Array', 'Uint8ClampedArray', 'Int16Array', 'Uint16Array', 'Int32Array', 'Uint32Array',
  'Float32Array', 'Float64Array', 'BigInt64Array', 'BigUint64Array'];
function serialize(v, refs) {
  refs = refs || new Map();
  const t = typeof v;
  if (v === undefined) return { t: 'u' };
  if (v === null || t === 'boolean' || t === 'string') return v;
  if (t === 'number') return Number.isFinite(v) && !Object.is(v, -0) ? v : { t: 'n', v: String(v) };
  if (t === 'bigint') return { t: 'b', v: v.toString() };
  if (t === 'function' || t === 'symbol') throw new DOMException(String(v) + ' could not be cloned.', 'DataCloneError');
  if (refs.has(v)) return { t: 'ref', v: refs.get(v) };
  const id = refs.size;
  refs.set(v, id);
  if (v._h !== undefined && typeof v.nodeType === 'number') throw new DOMException('A DOM node could not be cloned.', 'DataCloneError');
  if (v instanceof Date) return { t: 'd', id, v: v.getTime() };
  if (v instanceof RegExp) return { t: 'r', id, s: v.source, f: v.flags };
  if (v instanceof ArrayBuffer) return { t: 'ab', id, v: bytesToB64(new Uint8Array(v)) };
  if (v instanceof DataView) return { t: 'dv', id, v: bytesToB64(new Uint8Array(v.buffer, v.byteOffset, v.byteLength)) };
  if (ArrayBuffer.isView(v)) return { t: 'ta', id, c: v.constructor.name, v: bytesToB64(new Uint8Array(v.buffer, v.byteOffset, v.byteLength)) };
  if (v instanceof Blob) return { t: 'blob', id, k: v instanceof File ? v.name : null, y: v.type, v: bytesToB64(v._b) };
  if (v instanceof Map) return { t: 'm', id, v: Array.from(v, ([a, b]) => [serialize(a, refs), serialize(b, refs)]) };
  if (v instanceof Set) return { t: 's', id, v: Array.from(v, a => serialize(a, refs)) };
  if (v instanceof Error) return { t: 'e', id, n: v.name, m: v.message, s: v.stack };
  if (v instanceof Number || v instanceof String || v instanceof Boolean) return { t: 'w', id, v: serialize(v.valueOf(), refs) };
  if (Array.isArray(v)) return { t: 'a', id, v: v.map(x => serialize(x, refs)) };
  const o = {};
  for (const k of Object.keys(v)) o[k] = serialize(v[k], refs);
  return { t: 'o', id, v: o };
}
function deserialize(x, refs) {
  refs = refs || [];
  if (x === null || typeof x !== 'object') return x;
  let r;
  switch (x.t) {
    case 'u': return undefined;
    case 'n': return Number(x.v);
    case 'b': return BigInt(x.v);
    case 'ref': return refs[x.v];
    case 'd': r = new Date(x.v); break;
    case 'r': r = new RegExp(x.s, x.f); break;
    case 'ab': r = b64ToBytes(x.v).buffer; break;
    case 'dv': r = new DataView(b64ToBytes(x.v).buffer); break;
    case 'ta': { const b = b64ToBytes(x.v).buffer; const C = G[x.c] || Uint8Array; r = new C(b); break; }
    case 'blob': r = x.k === null ? new Blob([], { type: x.y }) : new File([], x.k, { type: x.y }); r._b = b64ToBytes(x.v); break;
    case 'm': r = new Map(); refs[x.id] = r; for (const [a, b] of x.v) r.set(deserialize(a, refs), deserialize(b, refs)); return r;
    case 's': r = new Set(); refs[x.id] = r; for (const a of x.v) r.add(deserialize(a, refs)); return r;
    case 'e': { const C = G[x.n] && G[x.n].prototype instanceof Error ? G[x.n] : Error; r = new C(x.m); if (x.s) r.stack = x.s; break; }
    case 'w': r = Object(deserialize(x.v, refs)); break;
    case 'a': r = []; refs[x.id] = r; for (const a of x.v) r.push(deserialize(a, refs)); return r;
    case 'o': r = {}; refs[x.id] = r; for (const k of Object.keys(x.v)) r[k] = deserialize(x.v[k], refs); return r;
    default: return x;
  }
  refs[x.id] = r;
  return r;
}
function structuredClone(v) { return deserialize(serialize(v)); }

// ------------------------------------------------------------------ Crypto
class CryptoKey { constructor(type, alg, usages, raw) { this.type = type; this.algorithm = alg; this.usages = usages; this.extractable = true; this._raw = raw; } }
function algName(a) { return String(typeof a === 'string' ? a : a && a.name).toUpperCase(); }
function hashName(a) { const h = a && a.hash; return String(typeof h === 'string' ? h : h && h.name || 'SHA-256').toUpperCase(); }
const subtle = {
  digest(alg, data) {
    return new Promise((res, rej) => {
      const out = KC.digest(algName(alg), bodyBytes(data));
      if (!out) rej(new DOMException('Algorithm: Unrecognized name', 'NotSupportedError'));
      else res(out.buffer);
    });
  },
  importKey(format, key, alg, extractable, usages) {
    if (format !== 'raw' || algName(alg) !== 'HMAC') return Promise.reject(new DOMException('Unsupported key', 'NotSupportedError'));
    return Promise.resolve(new CryptoKey('secret', { name: 'HMAC', hash: { name: hashName(alg) } }, usages || [], new Uint8Array(bodyBytes(key))));
  },
  exportKey(format, key) { return format === 'raw' ? Promise.resolve(key._raw.slice().buffer) : Promise.reject(new DOMException('Unsupported format', 'NotSupportedError')); },
  sign(alg, key, data) {
    if (algName(alg) !== 'HMAC') return Promise.reject(new DOMException('Unsupported algorithm', 'NotSupportedError'));
    return Promise.resolve(KC.hmac(key.algorithm.hash.name, key._raw, bodyBytes(data)).buffer);
  },
  verify(alg, key, sig, data) {
    return subtle.sign(alg, key, data).then(m => { const a = new Uint8Array(m), b = bodyBytes(sig); return a.length === b.length && a.every((x, i) => x === b[i]); });
  },
  generateKey() { return Promise.reject(new DOMException('Not supported', 'NotSupportedError')); },
  encrypt() { return Promise.reject(new DOMException('Not supported', 'NotSupportedError')); },
  decrypt() { return Promise.reject(new DOMException('Not supported', 'NotSupportedError')); },
  deriveBits() { return Promise.reject(new DOMException('Not supported', 'NotSupportedError')); },
};
const crypto = {
  getRandomValues(a) {
    if (!ArrayBuffer.isView(a) || a instanceof Float32Array || a instanceof Float64Array) throw new DOMException('The provided ArrayBufferView is of type Float', 'TypeMismatchError');
    if (a.byteLength > 65536) throw new DOMException('The ArrayBufferView byte length exceeds 65536', 'QuotaExceededError');
    new Uint8Array(a.buffer, a.byteOffset, a.byteLength).set(KC.randomBytes(a.byteLength));
    return a;
  },
  randomUUID() {
    const b = KC.randomBytes(16);
    b[6] = (b[6] & 15) | 64;
    b[8] = (b[8] & 63) | 128;
    const h = Array.from(b, x => (x + 256).toString(16).slice(1)).join('');
    return h.slice(0, 8) + '-' + h.slice(8, 12) + '-' + h.slice(12, 16) + '-' + h.slice(16, 20) + '-' + h.slice(20);
  },
  subtle,
};
// ------------------------------------------------------------------ Intl
// QuickJS has no Intl: a compact implementation with data for common
// European languages (formats follow CLDR for the default styles).
const NBSP = ' ', NNBSP = ' ';
const MONTHS = {
  en: ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'],
  de: ['Januar', 'Februar', 'März', 'April', 'Mai', 'Juni', 'Juli', 'August', 'September', 'Oktober', 'November', 'Dezember'],
  fr: ['janvier', 'février', 'mars', 'avril', 'mai', 'juin', 'juillet', 'août', 'septembre', 'octobre', 'novembre', 'décembre'],
  es: ['enero', 'febrero', 'marzo', 'abril', 'mayo', 'junio', 'julio', 'agosto', 'septiembre', 'octubre', 'noviembre', 'diciembre'],
  it: ['gennaio', 'febbraio', 'marzo', 'aprile', 'maggio', 'giugno', 'luglio', 'agosto', 'settembre', 'ottobre', 'novembre', 'dicembre'],
  nl: ['januari', 'februari', 'maart', 'april', 'mei', 'juni', 'juli', 'augustus', 'september', 'oktober', 'november', 'december'],
  pt: ['janeiro', 'fevereiro', 'março', 'abril', 'maio', 'junho', 'julho', 'agosto', 'setembro', 'outubro', 'novembro', 'dezembro'],
};
const MONTHS_SHORT = {
  en: ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'],
  de: ['Jan.', 'Feb.', 'März', 'Apr.', 'Mai', 'Juni', 'Juli', 'Aug.', 'Sept.', 'Okt.', 'Nov.', 'Dez.'],
  fr: ['janv.', 'févr.', 'mars', 'avr.', 'mai', 'juin', 'juil.', 'août', 'sept.', 'oct.', 'nov.', 'déc.'],
  es: ['ene', 'feb', 'mar', 'abr', 'may', 'jun', 'jul', 'ago', 'sept', 'oct', 'nov', 'dic'],
  it: ['gen', 'feb', 'mar', 'apr', 'mag', 'giu', 'lug', 'ago', 'set', 'ott', 'nov', 'dic'],
  nl: ['jan', 'feb', 'mrt', 'apr', 'mei', 'jun', 'jul', 'aug', 'sep', 'okt', 'nov', 'dec'],
  pt: ['jan.', 'fev.', 'mar.', 'abr.', 'mai.', 'jun.', 'jul.', 'ago.', 'set.', 'out.', 'nov.', 'dez.'],
};
const DAYS = {
  en: ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'],
  de: ['Sonntag', 'Montag', 'Dienstag', 'Mittwoch', 'Donnerstag', 'Freitag', 'Samstag'],
  fr: ['dimanche', 'lundi', 'mardi', 'mercredi', 'jeudi', 'vendredi', 'samedi'],
  es: ['domingo', 'lunes', 'martes', 'miércoles', 'jueves', 'viernes', 'sábado'],
  it: ['domenica', 'lunedì', 'martedì', 'mercoledì', 'giovedì', 'venerdì', 'sabato'],
  nl: ['zondag', 'maandag', 'dinsdag', 'woensdag', 'donderdag', 'vrijdag', 'zaterdag'],
  pt: ['domingo', 'segunda-feira', 'terça-feira', 'quarta-feira', 'quinta-feira', 'sexta-feira', 'sábado'],
};
const DAYS_SHORT = {
  en: ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'],
  de: ['So.', 'Mo.', 'Di.', 'Mi.', 'Do.', 'Fr.', 'Sa.'],
  fr: ['dim.', 'lun.', 'mar.', 'mer.', 'jeu.', 'ven.', 'sam.'],
  es: ['dom', 'lun', 'mar', 'mié', 'jue', 'vie', 'sáb'],
  it: ['dom', 'lun', 'mar', 'mer', 'gio', 'ven', 'sab'],
  nl: ['zo', 'ma', 'di', 'wo', 'do', 'vr', 'za'],
  pt: ['dom.', 'seg.', 'ter.', 'qua.', 'qui.', 'sex.', 'sáb.'],
};
// decimal, group, minimum grouping digits, percent pattern, currency after the number
const NUM = {
  en: ['.', ',', 1, '#%', false], de: [',', '.', 1, '#' + NBSP + '%', true], fr: [',', NNBSP, 1, '#' + NBSP + '%', true],
  es: [',', '.', 2, '#' + NBSP + '%', true], it: [',', '.', 1, '#%', true], nl: [',', '.', 1, '#%', false],
  pt: [',', '.', 1, '#%', true], ru: [',', NBSP, 1, '#' + NBSP + '%', true], pl: [',', NBSP, 2, '#%', true],
  sv: [',', NBSP, 1, '#' + NBSP + '%', true], ja: ['.', ',', 1, '#%', false], zh: ['.', ',', 1, '#%', false],
};
const CURRENCY = { USD: '$', EUR: '€', GBP: '£', JPY: '¥', CNY: 'CN¥', CHF: 'CHF', INR: '₹', KRW: '₩', RUB: 'RUB', PLN: 'PLN',
  SEK: 'SEK', CAD: 'CA$', AUD: 'A$', BRL: 'R$', MXN: 'MX$', TRY: 'TRY' };
const CURRENCY_DIGITS = { JPY: 0, KRW: 0, CLP: 0, VND: 0, ISK: 0, HUF: 2 };
const DEFAULT_LOCALE = 'de-DE';

function canonLocale(tag) {
  const parts = String(tag).replace(/_/g, '-').split('-');
  const out = [parts[0].toLowerCase()];
  for (let i = 1; i < parts.length; i++) {
    const p = parts[i];
    out.push(p.length === 2 ? p.toUpperCase() : p.length === 4 ? p[0].toUpperCase() + p.slice(1).toLowerCase() : p.toLowerCase());
  }
  return out.join('-');
}
function resolveLocale(locales) {
  let l = locales;
  if (Array.isArray(l)) l = l[0];
  if (l && typeof l === 'object' && l.baseName) l = l.baseName;
  if (!l) return DEFAULT_LOCALE;
  try { return canonLocale(l); } catch (e) { return DEFAULT_LOCALE; }
}
function lang(locale) { return locale.split('-')[0]; }
function dataFor(table, locale) { return table[lang(locale)] || table.en; }

function groupDigits(int, sep, minGroup) {
  if (int.length < 4 || (minGroup === 2 && int.length < 5)) return int;
  let out = '';
  for (let i = 0; i < int.length; i++) {
    if (i > 0 && (int.length - i) % 3 === 0) out += sep;
    out += int[i];
  }
  return out;
}
function roundSig(n, sig) { if (n === 0) return 0; const p = Math.pow(10, sig - Math.ceil(Math.log10(Math.abs(n)))); return Math.round(n * p) / p; }

const UNIT_SHORT = { kilometer: 'km', meter: 'm', centimeter: 'cm', millimeter: 'mm', mile: 'mi', foot: 'ft', inch: 'in',
  kilogram: 'kg', gram: 'g', pound: 'lb', liter: 'l', milliliter: 'ml', second: 's', minute: 'min', hour: 'h', day: 'd',
  millisecond: 'ms', byte: 'byte', kilobyte: 'kB', megabyte: 'MB', gigabyte: 'GB', terabyte: 'TB', percent: '%',
  celsius: '°C', fahrenheit: '°F', 'kilometer-per-hour': 'km/h', 'mile-per-hour': 'mph', degree: '°' };

class NumberFormat {
  constructor(locales, options) {
    options = Object.assign({}, options);
    this._locale = resolveLocale(locales);
    const o = this._o = {
      locale: this._locale, numberingSystem: 'latn', style: options.style || 'decimal',
      currency: options.currency ? String(options.currency).toUpperCase() : undefined,
      currencyDisplay: options.currencyDisplay || 'symbol', currencySign: options.currencySign || 'standard',
      unit: options.unit, unitDisplay: options.unitDisplay || 'short',
      minimumIntegerDigits: options.minimumIntegerDigits || 1,
      useGrouping: options.useGrouping === undefined ? 'auto' : options.useGrouping,
      notation: options.notation || 'standard', compactDisplay: options.compactDisplay || 'short',
      signDisplay: options.signDisplay || 'auto', roundingMode: options.roundingMode || 'halfExpand',
    };
    if (o.style === 'currency' && !o.currency) throw new TypeError('Currency code is required with currency style.');
    const cd = o.style === 'currency' ? (CURRENCY_DIGITS[o.currency] !== undefined ? CURRENCY_DIGITS[o.currency] : 2) : 0;
    let minF = options.minimumFractionDigits, maxF = options.maximumFractionDigits;
    if (minF === undefined) minF = o.style === 'currency' && o.notation !== 'compact' ? cd : 0;
    if (maxF === undefined) maxF = Math.max(minF, o.style === 'currency' ? cd : o.style === 'percent' ? 0 : o.notation === 'compact' ? 0 : 3);
    if (minF > maxF) throw new RangeError('maximumFractionDigits value is out of range.');
    o.minimumFractionDigits = minF;
    o.maximumFractionDigits = maxF;
    if (options.maximumSignificantDigits !== undefined || options.minimumSignificantDigits !== undefined) {
      o.minimumSignificantDigits = options.minimumSignificantDigits || 1;
      o.maximumSignificantDigits = options.maximumSignificantDigits || 21;
    }
    this.format = this.format.bind(this);
  }
  resolvedOptions() { return Object.assign({}, this._o); }
  formatToParts(n) { return this._parts(n); }
  format(n) { return this._parts(n).map(p => p.value).join(''); }
  formatRange(a, b) { return this.format(a) + '–' + this.format(b); }
  _parts(value) {
    const o = this._o, L = lang(this._locale);
    const [dec, grp, minGroup, pctPattern, curAfter] = NUM[L] || NUM.en;
    let n = typeof value === 'bigint' ? Number(value) : Number(value);
    const parts = [];
    if (Number.isNaN(n)) return [{ type: 'nan', value: 'NaN' }];
    const neg = n < 0 || Object.is(n, -0);
    n = Math.abs(n);
    if (o.style === 'percent') n *= 100;
    let suffix = '', maxF = o.maximumFractionDigits;
    if (o.notation === 'compact' && Number.isFinite(n)) {
      const units = L === 'de' ? [[1e12, ' Bio.'], [1e9, ' Mrd.'], [1e6, ' Mio.'], [1e3, '']]
        : L === 'fr' ? [[1e12, ' Bn'], [1e9, ' Md'], [1e6, ' M'], [1e3, ' k']]
          : [[1e12, 'T'], [1e9, 'B'], [1e6, 'M'], [1e3, 'K']];
      for (const [d, s] of units) {
        if (n >= d && !(L === 'de' && d === 1e3)) { n /= d; suffix = s.replace(' ', NBSP); break; }
      }
      if (o.maximumSignificantDigits === undefined && o.maximumFractionDigits === 0) {
        n = n < 100 ? roundSig(n, 2) : Math.round(n);
        maxF = n < 10 ? 1 : 0;  // 1.2M, 12M, 123M
      }
    }
    let text;
    if (!Number.isFinite(n)) text = '∞';
    else if (o.notation === 'scientific' || o.notation === 'engineering') text = n.toExponential(Math.min(o.maximumFractionDigits, 3)).replace('e+', 'E').replace('e', 'E');
    else {
      let x = n;
      if (o.maximumSignificantDigits !== undefined) x = roundSig(x, o.maximumSignificantDigits);
      let s = o.maximumSignificantDigits !== undefined ? String(x) : x.toFixed(maxF);
      if (s.indexOf('e') >= 0) s = x.toFixed(Math.min(20, maxF));
      let [int, frac = ''] = s.split('.');
      while (frac.length > o.minimumFractionDigits && frac.endsWith('0')) frac = frac.slice(0, -1);
      while (int.length < o.minimumIntegerDigits) int = '0' + int;
      text = null;
      const grouping = o.useGrouping === false || o.useGrouping === 'false' ? false : true;
      const gInt = grouping ? groupDigits(int, grp, o.useGrouping === 'always' ? 1 : minGroup) : int;
      const pieces = gInt.split(grp);
      pieces.forEach((p, i) => { if (i) parts.push({ type: 'group', value: grp }); parts.push({ type: 'integer', value: p }); });
      if (frac) { parts.push({ type: 'decimal', value: dec }); parts.push({ type: 'fraction', value: frac }); }
    }
    if (text !== null) parts.push({ type: Number.isFinite(n) ? 'integer' : 'infinity', value: text });
    if (suffix) parts.push({ type: 'compact', value: suffix });
    const isZero = n === 0;
    let sign = '';
    if (o.signDisplay === 'always' || (o.signDisplay === 'exceptZero' && !isZero)) sign = neg ? '-' : '+';
    else if (o.signDisplay !== 'never' && neg && !(o.signDisplay === 'negative' && isZero)) sign = '-';
    if (sign) parts.unshift({ type: sign === '-' ? 'minusSign' : 'plusSign', value: sign });
    if (o.style === 'percent') {
      const after = pctPattern.slice(1);
      parts.push({ type: 'literal', value: after.slice(0, -1) }, { type: 'percentSign', value: '%' });
      if (!after.slice(0, -1)) parts.splice(parts.length - 2, 1);
    } else if (o.style === 'currency') {
      const local = L === 'fr' && o.currency === 'USD' ? '$US' : L === 'de' && o.currency === 'CHF' ? 'CHF' : null;
      const sym = o.currencyDisplay === 'code' ? o.currency : o.currencyDisplay === 'name' ? o.currency : (local || CURRENCY[o.currency] || o.currency);
      if (curAfter) parts.push({ type: 'literal', value: NBSP }, { type: 'currency', value: sym });
      else {
        const at = sign ? 1 : 0;
        parts.splice(at, 0, { type: 'currency', value: sym });
        if (sym.length > 1 && /[A-Z]$/.test(sym)) parts.splice(at + 1, 0, { type: 'literal', value: NBSP });
      }
    } else if (o.style === 'unit' && o.unit) {
      const u = o.unitDisplay === 'long' ? ' ' + o.unit : (o.unit === 'percent' ? '' : ' ') + (UNIT_SHORT[o.unit] || o.unit);
      parts.push({ type: 'literal', value: u[0] === ' ' ? NBSP : '' }, { type: 'unit', value: u.trim() });
    }
    return parts.filter(p => p.value !== '');
  }
  static supportedLocalesOf(l) { return (Array.isArray(l) ? l : l ? [l] : []).map(canonLocale); }
}

function pad2(n) { return n < 10 ? '0' + n : String(n); }
const DATE_FIELDS = ['weekday', 'era', 'year', 'month', 'day', 'dayPeriod', 'hour', 'minute', 'second', 'fractionalSecondDigits', 'timeZoneName'];
class DateTimeFormat {
  constructor(locales, options, defaults) {
    options = Object.assign({}, options);
    this._locale = resolveLocale(locales);
    const o = {};
    for (const k of DATE_FIELDS) if (options[k] !== undefined) o[k] = options[k];
    const ds = options.dateStyle, ts = options.timeStyle;
    if (ds) {
      if (ds === 'full') Object.assign(o, { weekday: 'long', year: 'numeric', month: 'long', day: 'numeric' });
      else if (ds === 'long') Object.assign(o, { year: 'numeric', month: 'long', day: 'numeric' });
      else if (ds === 'medium') Object.assign(o, { year: 'numeric', month: 'short', day: 'numeric' });
      else Object.assign(o, { year: '2-digit', month: '2-digit', day: '2-digit', _short: true });
    }
    if (ts) {
      Object.assign(o, { hour: 'numeric', minute: '2-digit' });
      if (ts !== 'short') o.second = '2-digit';
      if (ts === 'full' || ts === 'long') o.timeZoneName = 'short';
    }
    const hasDate = o.weekday || o.year || o.month || o.day, hasTime = o.hour || o.minute || o.second;
    if (!hasDate && !hasTime && defaults !== 'none') {
      if (defaults !== 'time') Object.assign(o, { year: 'numeric', month: 'numeric', day: 'numeric' });
      if (defaults === 'time' || defaults === 'all') Object.assign(o, { hour: 'numeric', minute: '2-digit', second: '2-digit' });
    }
    const L = lang(this._locale);
    let h12 = options.hour12 !== undefined ? !!options.hour12 : options.hourCycle ? /h1[12]/.test(options.hourCycle) : L === 'en' && !/-(GB|IE)$/.test(this._locale);
    o.hour12 = h12;
    o.timeZone = options.timeZone ? String(options.timeZone) : undefined;
    this._utc = o.timeZone && /^(utc|gmt|etc\/utc|etc\/gmt|z)$/i.test(o.timeZone);
    this._o = o;
    this.format = this.format.bind(this);
  }
  resolvedOptions() {
    const o = Object.assign({ locale: this._locale, calendar: 'gregory', numberingSystem: 'latn' }, this._o);
    delete o._short;
    o.timeZone = this._o.timeZone || localZoneName();
    o.hourCycle = o.hour12 ? 'h12' : 'h23';
    if (!this._o.hour) { delete o.hour12; delete o.hourCycle; }
    return o;
  }
  format(d) { return this.formatToParts(d).map(p => p.value).join(''); }
  formatRange(a, b) { return this.format(a) + ' – ' + this.format(b); }
  formatToParts(date) {
    const d = date === undefined ? new Date() : new Date(typeof date === 'object' ? date.valueOf() : date);
    if (isNaN(d.getTime())) throw new RangeError('Invalid time value');
    const u = this._utc;
    const v = {
      y: u ? d.getUTCFullYear() : d.getFullYear(), M: u ? d.getUTCMonth() : d.getMonth(), D: u ? d.getUTCDate() : d.getDate(),
      w: u ? d.getUTCDay() : d.getDay(), h: u ? d.getUTCHours() : d.getHours(), m: u ? d.getUTCMinutes() : d.getMinutes(),
      s: u ? d.getUTCSeconds() : d.getSeconds(), ms: u ? d.getUTCMilliseconds() : d.getMilliseconds(),
      off: u ? 0 : -d.getTimezoneOffset(),
    };
    const o = this._o, L = lang(this._locale), parts = [];
    const lit = s => { if (s) parts.push({ type: 'literal', value: s }); };
    const P = (type, value) => parts.push({ type, value: String(value) });
    const yearStr = o.year === '2-digit' ? pad2(v.y % 100) : String(v.y);
    const textMonth = o.month === 'long' || o.month === 'short' || o.month === 'narrow';
    const monthStr = o.month === 'long' ? dataFor(MONTHS, this._locale)[v.M] : o.month === 'short' ? dataFor(MONTHS_SHORT, this._locale)[v.M]
      : o.month === 'narrow' ? dataFor(MONTHS, this._locale)[v.M][0].toUpperCase() : o.month === '2-digit' ? pad2(v.M + 1) : String(v.M + 1);
    const dayStr = o.day === '2-digit' ? pad2(v.D) : String(v.D);
    const hasDate = o.year || o.month || o.day;
    if (o.weekday) {
      const wd = o.weekday === 'long' ? dataFor(DAYS, this._locale)[v.w] : o.weekday === 'short' ? dataFor(DAYS_SHORT, this._locale)[v.w] : dataFor(DAYS, this._locale)[v.w][0].toUpperCase();
      P('weekday', wd);
      if (hasDate) lit(L === 'fr' || L === 'es' || L === 'it' || L === 'nl' || L === 'pt' ? ' ' : ', ');
    }
    if (hasDate) {
      if (textMonth) {
        if (L === 'en') {
          if (o.month) P('month', monthStr);
          if (o.day) { lit(' '); P('day', dayStr); }
          if (o.year) { lit(o.day ? ', ' : ' '); P('year', yearStr); }
        } else {
          if (o.day) { P('day', dayStr); lit(L === 'de' ? '. ' : ' '); }
          if (o.month) P('month', (L === 'es' || L === 'pt') && o.day ? 'de ' + monthStr : monthStr);
          if (o.year) { lit((L === 'es' || L === 'pt') ? ' de ' : ' '); P('year', yearStr); }
        }
      } else {
        const sep = L === 'en' || L === 'fr' || L === 'es' || L === 'it' || L === 'pt' ? '/' : L === 'de' || L === 'ru' || L === 'pl' ? '.' : L === 'nl' ? '-' : L === 'ja' || L === 'zh' ? '/' : '/';
        const pad = L === 'fr' || L === 'it' || L === 'pt' || L === 'es' && o._short || o._short && L !== 'en';
        const dd = pad && o.day !== 'numeric' ? pad2(v.D) : (pad ? pad2(v.D) : dayStr);
        const mm = pad ? pad2(v.M + 1) : monthStr;
        let order = L === 'en' && !/-(GB|AU|NZ|IE|IN)$/.test(this._locale) ? 'MDY' : L === 'ja' || L === 'zh' ? 'YMD' : 'DMY';
        const seq = order.split('').filter(c => (c === 'Y' && o.year) || (c === 'M' && o.month) || (c === 'D' && o.day));
        seq.forEach((c, i) => {
          if (i) lit(sep);
          if (c === 'Y') P('year', yearStr); else if (c === 'M') P('month', mm); else P('day', dd);
        });
        if (L === 'de' && seq[seq.length - 1] !== 'Y' && seq.length) lit('.');
      }
    }
    if (o.hour || o.minute || o.second) {
      if (hasDate || o.weekday) lit(L === 'en' || L === 'de' || L === 'nl' ? ', ' : ' ');
      if (o.hour) {
        let h = v.h;
        if (o.hour12) { h = h % 12; if (h === 0) h = 12; }
        P('hour', o.hour === '2-digit' || (!o.hour12 && o.minute) ? pad2(h) : h);
      }
      if (o.minute) { if (o.hour) lit(':'); P('minute', o.hour || o.minute === '2-digit' ? pad2(v.m) : v.m); }
      if (o.second) { if (o.hour || o.minute) lit(':'); P('second', pad2(v.s)); }
      if (o.fractionalSecondDigits) { lit(L === 'en' ? '.' : ','); P('fractionalSecond', String(v.ms).padStart(3, '0').slice(0, o.fractionalSecondDigits)); }
      if (o.hour && o.hour12) { lit(' '); P('dayPeriod', v.h < 12 ? 'AM' : 'PM'); }
      if (L === 'de' && o.hour && !o.minute) lit(' Uhr');
    }
    if (o.timeZoneName) {
      lit(' ');
      const off = v.off;
      P('timeZoneName', u ? 'UTC' : off === 0 ? 'GMT' : 'GMT' + (off > 0 ? '+' : '-') + Math.floor(Math.abs(off) / 60) + (Math.abs(off) % 60 ? ':' + pad2(Math.abs(off) % 60) : ''));
    }
    return parts;
  }
  static supportedLocalesOf(l) { return NumberFormat.supportedLocalesOf(l); }
}
function localZoneName() {
  const jan = -new Date(2024, 0, 1).getTimezoneOffset(), jul = -new Date(2024, 6, 1).getTimezoneOffset();
  if (jan === 0 && jul === 0) return 'UTC';
  if (jan === 60 && jul === 120) return 'Europe/Berlin';
  if (jan === 0 && jul === 60) return 'Europe/London';
  if (jan === 120 && jul === 180) return 'Europe/Helsinki';
  if (jan === -300 && jul === -240) return 'America/New_York';
  if (jan === -360 && jul === -300) return 'America/Chicago';
  if (jan === -480 && jul === -420) return 'America/Los_Angeles';
  const h = -jan / 60;
  return Number.isInteger(h) ? 'Etc/GMT' + (h >= 0 ? '+' : '') + h : 'UTC';
}

class PluralRules {
  constructor(locales, options) { this._locale = resolveLocale(locales); this._type = options && options.type || 'cardinal'; }
  select(n) {
    n = Number(n);
    const L = lang(this._locale), i = Math.floor(Math.abs(n)), int = Number.isInteger(n);
    if (this._type === 'ordinal') {
      if (L !== 'en') return L === 'fr' && n === 1 ? 'one' : 'other';
      const m10 = i % 10, m100 = i % 100;
      return m10 === 1 && m100 !== 11 ? 'one' : m10 === 2 && m100 !== 12 ? 'two' : m10 === 3 && m100 !== 13 ? 'few' : 'other';
    }
    if (L === 'ja' || L === 'zh' || L === 'ko') return 'other';
    if (L === 'fr' || (L === 'pt' && !/-PT$/.test(this._locale))) return i === 0 || i === 1 ? 'one' : 'other';
    if (L === 'ru' || L === 'uk' || L === 'pl') {
      if (!int) return 'other';
      const m10 = i % 10, m100 = i % 100;
      if (m10 === 1 && m100 !== 11) return L === 'pl' && i !== 1 ? 'many' : 'one';
      if (m10 >= 2 && m10 <= 4 && !(m100 >= 12 && m100 <= 14)) return 'few';
      return 'many';
    }
    return i === 1 && int ? 'one' : 'other';
  }
  selectRange(a, b) { return this.select(b); }
  resolvedOptions() {
    const L = lang(this._locale);
    const cats = this._type === 'ordinal' ? (L === 'en' ? ['few', 'one', 'other', 'two'] : ['other']) : L === 'ja' || L === 'zh' ? ['other'] : /ru|uk|pl/.test(L) ? ['few', 'many', 'one', 'other'] : ['one', 'other'];
    return { locale: this._locale, type: this._type, pluralCategories: cats, minimumIntegerDigits: 1, minimumFractionDigits: 0, maximumFractionDigits: 3 };
  }
  static supportedLocalesOf(l) { return NumberFormat.supportedLocalesOf(l); }
}

const REL = {
  en: { units: { second: ['second', 'seconds'], minute: ['minute', 'minutes'], hour: ['hour', 'hours'], day: ['day', 'days'], week: ['week', 'weeks'], month: ['month', 'months'], quarter: ['quarter', 'quarters'], year: ['year', 'years'] },
    future: 'in {0}', past: '{0} ago', auto: { day: { '-1': 'yesterday', '0': 'today', '1': 'tomorrow' }, second: { '0': 'now' }, '*': { '-1': 'last {u}', '0': 'this {u}', '1': 'next {u}' } } },
  de: { units: { second: ['Sekunde', 'Sekunden'], minute: ['Minute', 'Minuten'], hour: ['Stunde', 'Stunden'], day: ['Tag', 'Tagen'], week: ['Woche', 'Wochen'], month: ['Monat', 'Monaten'], quarter: ['Quartal', 'Quartalen'], year: ['Jahr', 'Jahren'] },
    unitsFuture: { day: ['Tag', 'Tagen'], month: ['Monat', 'Monaten'], year: ['Jahr', 'Jahren'] },
    future: 'in {0}', past: 'vor {0}', auto: { day: { '-2': 'vorgestern', '-1': 'gestern', '0': 'heute', '1': 'morgen', '2': 'übermorgen' }, second: { '0': 'jetzt' } } },
  fr: { units: { second: ['seconde', 'secondes'], minute: ['minute', 'minutes'], hour: ['heure', 'heures'], day: ['jour', 'jours'], week: ['semaine', 'semaines'], month: ['mois', 'mois'], quarter: ['trimestre', 'trimestres'], year: ['an', 'ans'] },
    future: 'dans {0}', past: 'il y a {0}', auto: { day: { '-1': 'hier', '0': "aujourd’hui", '1': 'demain' }, second: { '0': 'maintenant' } } },
};
class RelativeTimeFormat {
  constructor(locales, options) { this._locale = resolveLocale(locales); this._o = Object.assign({ numeric: 'always', style: 'long' }, options); }
  format(value, unit) {
    unit = String(unit).replace(/s$/, '');
    const data = REL[lang(this._locale)] || REL.en;
    value = Number(value);
    if (this._o.numeric === 'auto') {
      const a = data.auto[unit] || data.auto['*'];
      if (a && a[String(value)] !== undefined) return a[String(value)].replace('{u}', (data.units[unit] || [unit])[0]);
    }
    const abs = Math.abs(value);
    const names = data.units[unit] || [unit, unit + 's'];
    const plural = new PluralRules(this._locale).select(abs) === 'one' ? 0 : 1;
    const num = new NumberFormat(this._locale).format(abs) + ' ' + names[plural];
    return (value < 0 || Object.is(value, -0) ? data.past : data.future).replace('{0}', num);
  }
  formatToParts(v, u) { return [{ type: 'literal', value: this.format(v, u) }]; }
  resolvedOptions() { return { locale: this._locale, style: this._o.style, numeric: this._o.numeric, numberingSystem: 'latn' }; }
  static supportedLocalesOf(l) { return NumberFormat.supportedLocalesOf(l); }
}

const LIST_WORDS = { en: ['and', 'or'], de: ['und', 'oder'], fr: ['et', 'ou'], es: ['y', 'o'], it: ['e', 'o'], nl: ['en', 'of'], pt: ['e', 'ou'] };
class ListFormat {
  constructor(locales, options) { this._locale = resolveLocale(locales); this._o = Object.assign({ type: 'conjunction', style: 'long' }, options); }
  formatToParts(list) {
    const items = Array.from(list, String), L = lang(this._locale);
    const w = (LIST_WORDS[L] || LIST_WORDS.en)[this._o.type === 'disjunction' ? 1 : 0];
    const parts = [];
    items.forEach((x, i) => {
      if (i) {
        const last = i === items.length - 1;
        let sep = ', ';
        if (last && this._o.type !== 'unit') sep = (L === 'en' && items.length > 2 ? ', ' : ' ') + w + ' ';
        if (this._o.type === 'unit' && this._o.style === 'narrow') sep = ' ';
        parts.push({ type: 'literal', value: sep });
      }
      parts.push({ type: 'element', value: x });
    });
    return parts;
  }
  format(list) { return this.formatToParts(list).map(p => p.value).join(''); }
  resolvedOptions() { return Object.assign({ locale: this._locale }, this._o); }
}

function collKey(s, sensitivity) {
  let k = String(s);
  if (k.normalize) k = k.normalize('NFD');
  if (sensitivity === 'base' || sensitivity === 'case') k = k.replace(/[̀-ͯ]/g, '');
  if (sensitivity === 'base' || sensitivity === 'accent') k = k.toLowerCase();
  return k;
}
class Collator {
  constructor(locales, options) {
    this._locale = resolveLocale(locales);
    this._o = Object.assign({ usage: 'sort', sensitivity: 'variant', ignorePunctuation: false, numeric: false, caseFirst: 'false' }, options);
    this.compare = this.compare.bind(this);
  }
  compare(a, b) {
    const o = this._o;
    a = String(a); b = String(b);
    if (o.ignorePunctuation) { a = a.replace(/[\s\p{P}]/gu, ''); b = b.replace(/[\s\p{P}]/gu, ''); }
    const cmp = (x, y) => {
      if (o.numeric) {
        const rx = x.match(/\d+|\D+/g) || [], ry = y.match(/\d+|\D+/g) || [];
        for (let i = 0; i < Math.min(rx.length, ry.length); i++) {
          const dx = /^\d/.test(rx[i]), dy = /^\d/.test(ry[i]);
          if (dx && dy) { const d = Number(rx[i]) - Number(ry[i]); if (d) return d < 0 ? -1 : 1; }
          else if (rx[i] !== ry[i]) return rx[i] < ry[i] ? -1 : 1;
        }
        return rx.length === ry.length ? 0 : rx.length < ry.length ? -1 : 1;
      }
      return x === y ? 0 : x < y ? -1 : 1;
    };
    const base = cmp(collKey(a, 'base'), collKey(b, 'base'));
    if (base || o.sensitivity === 'base') return base;
    const acc = cmp(collKey(a, 'accent'), collKey(b, 'accent'));
    if (acc || o.sensitivity === 'accent') return acc;
    // Lower case before upper case (CLDR default).
    const sw = s => s.replace(/[a-zA-Z]/g, c => c === c.toLowerCase() ? c.toUpperCase() : c.toLowerCase());
    return cmp(sw(collKey(a, 'variant')), sw(collKey(b, 'variant')));
  }
  resolvedOptions() { return Object.assign({ locale: this._locale, collation: 'default' }, this._o); }
  static supportedLocalesOf(l) { return NumberFormat.supportedLocalesOf(l); }
}

class Segmenter {
  constructor(locales, options) { this._locale = resolveLocale(locales); this._g = options && options.granularity || 'grapheme'; }
  segment(input) {
    const str = String(input), g = this._g, segs = [];
    if (g === 'grapheme') {
      const re = /\P{M}\p{M}*|\p{M}+/gu;
      let m;
      while ((m = re.exec(str))) {
        const last = segs[segs.length - 1];
        // Join emoji ZWJ sequences, variation selectors and regional indicator pairs.
        if (last && (/‍$/.test(last.segment) || /^[‍️\u{1f3fb}-\u{1f3ff}]/u.test(m[0]) ||
            (/^[\u{1f1e6}-\u{1f1ff}]$/u.test(last.segment) && /^[\u{1f1e6}-\u{1f1ff}]/u.test(m[0])))) last.segment += m[0];
        else segs.push({ segment: m[0], index: m.index, input: str });
      }
    } else if (g === 'word') {
      const re = /[\p{L}\p{N}_'’]+|\s+|[^\s\p{L}\p{N}_'’]/gu;
      let m;
      while ((m = re.exec(str))) segs.push({ segment: m[0], index: m.index, input: str, isWordLike: /[\p{L}\p{N}]/u.test(m[0]) });
    } else {
      const re = /[^.!?]+(?:[.!?]+|$)\s*/g;
      let m;
      while ((m = re.exec(str)) && m[0]) segs.push({ segment: m[0], index: m.index, input: str });
    }
    return {
      [Symbol.iterator]() { return segs[Symbol.iterator](); },
      containing(i) { i = i === undefined ? 0 : i; return segs.find(s => i >= s.index && i < s.index + s.segment.length); },
    };
  }
  resolvedOptions() { return { locale: this._locale, granularity: this._g }; }
  static supportedLocalesOf(l) { return NumberFormat.supportedLocalesOf(l); }
}

const LANG_NAMES = { de: { de: 'Deutsch', en: 'Englisch', fr: 'Französisch', es: 'Spanisch', it: 'Italienisch' },
  en: { de: 'German', en: 'English', fr: 'French', es: 'Spanish', it: 'Italian' } };
class DisplayNames {
  constructor(locales, options) { this._locale = resolveLocale(locales); this._o = Object.assign({ type: 'language', fallback: 'code' }, options); }
  of(code) {
    code = String(code);
    if (this._o.type === 'language') { const t = LANG_NAMES[lang(this._locale)] || LANG_NAMES.en; return t[code.toLowerCase()] || code; }
    if (this._o.type === 'currency') return code.toUpperCase();
    return code;
  }
  resolvedOptions() { return Object.assign({ locale: this._locale }, this._o); }
}
class Locale {
  constructor(tag, opts) {
    const c = canonLocale(tag instanceof Locale ? tag.baseName : tag).split('-');
    this.language = c[0];
    this.script = c.find((p, i) => i && p.length === 4);
    this.region = (opts && opts.region) || c.find((p, i) => i && (p.length === 2 || /^\d{3}$/.test(p)));
    this.baseName = [this.language, this.script, this.region].filter(Boolean).join('-');
    this.calendar = opts && opts.calendar; this.hourCycle = opts && opts.hourCycle;
  }
  maximize() { return this; }
  minimize() { return new Locale(this.language); }
  toString() { return this.baseName; }
  getWeekInfo() { return { firstDay: this.region === 'US' ? 7 : 1, weekend: [6, 7], minimalDays: this.region === 'US' ? 1 : 4 }; }
}
// NumberFormat, DateTimeFormat and Collator may be called without new.
function callable(C) {
  const F = function (a, b) { return new C(a, b); };
  F.prototype = C.prototype;
  Object.defineProperty(C.prototype, 'constructor', { value: F, writable: true, configurable: true });
  F.supportedLocalesOf = C.supportedLocalesOf;
  Object.defineProperty(F, 'name', { value: C.name });
  return F;
}
const Intl = {
  NumberFormat: callable(NumberFormat), DateTimeFormat: callable(DateTimeFormat), PluralRules, RelativeTimeFormat,
  ListFormat, Collator: callable(Collator), Segmenter, DisplayNames, Locale,
  getCanonicalLocales: l => (Array.isArray(l) ? l : l === undefined ? [] : [l]).map(canonLocale),
  supportedValuesOf: k => k === 'currency' ? Object.keys(CURRENCY) : k === 'timeZone' ? ['UTC', 'Europe/Berlin', 'Europe/London', 'America/New_York'] : [],
};
for (const k of Object.keys(Intl)) if (typeof Intl[k] === 'function' && Intl[k].prototype) Intl[k].prototype[Symbol.toStringTag] = 'Intl.' + k;
// The locale-aware built-ins use it.
const nfCache = new Map();
Number.prototype.toLocaleString = function (l, o) {
  if (o === undefined) { const k = String(l); let f = nfCache.get(k); if (!f) { f = new NumberFormat(l); nfCache.set(k, f); } return f.format(this.valueOf()); }
  return new NumberFormat(l, o).format(this.valueOf());
};
if (typeof BigInt !== 'undefined') BigInt.prototype.toLocaleString = function (l, o) { return new NumberFormat(l, o).format(this.valueOf()); };
Date.prototype.toLocaleDateString = function (l, o) { return new DateTimeFormat(l, o, 'date').format(this); };
Date.prototype.toLocaleTimeString = function (l, o) { return new DateTimeFormat(l, o, 'time').format(this); };
Date.prototype.toLocaleString = function (l, o) { return new DateTimeFormat(l, o, 'all').format(this); };
String.prototype.localeCompare = function (that, l, o) { return new Collator(l, o).compare(String(this), that); };
// ------------------------------------------------------------------ Export
const API = { DOMException, TextEncoder, TextDecoder, TextEncoderStream, TextDecoderStream, btoa, atob, URL, URLSearchParams,
  Headers, Blob, File, FileReader, FormData, Response, Request, ReadableStream, ReadableStreamDefaultReader,
  ReadableStreamDefaultController, WritableStream, WritableStreamDefaultWriter, TransformStream, CountQueuingStrategy,
  ByteLengthQueuingStrategy, AbortController, AbortSignal, structuredClone, crypto, CryptoKey, SubtleCrypto: function () {},
  Intl, queueMicrotask: fn => { Promise.resolve().then(fn); } };
for (const k of Object.keys(API)) def(k, API[k]);
G.__kiteSerialize = v => JSON.stringify(serialize(v));
G.__kiteDeserialize = s => deserialize(JSON.parse(s));
G.__kiteBodyBytes = bodyBytes;
})();
)KITEJS";

}  // namespace kite

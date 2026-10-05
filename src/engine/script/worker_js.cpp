// The global scope of Web Workers (on top of the shared APIs).
namespace kite {

extern const char* const kWorkerPrelude;
const char* const kWorkerPrelude = R"KITEJS(
(function () {
'use strict';
const G = globalThis, W = G.__w;
function def(name, value) { Object.defineProperty(G, name, { value, writable: true, configurable: true, enumerable: false }); }
class Event {
  constructor(type, init) {
    init = init || {};
    this.type = String(type); this.bubbles = !!init.bubbles; this.cancelable = !!init.cancelable;
    this.defaultPrevented = false; this.target = null; this.currentTarget = null; this.timeStamp = Date.now(); this.isTrusted = false;
    this._stopNow = false;
  }
  preventDefault() { if (this.cancelable) this.defaultPrevented = true; }
  stopPropagation() {}
  stopImmediatePropagation() { this._stopNow = true; }
  composedPath() { return this.currentTarget ? [this.currentTarget] : []; }
}
class MessageEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.data = i.data; this.origin = i.origin || ''; this.lastEventId = ''; this.source = null; this.ports = []; } }
class ErrorEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.message = i.message || ''; this.filename = i.filename || ''; this.lineno = i.lineno || 0; this.colno = 0; this.error = i.error || null; } }
class CustomEvent extends Event { constructor(t, i) { super(t, i); this.detail = i && 'detail' in i ? i.detail : null; } }
class EventTarget {
  addEventListener(type, fn, opts) {
    if (!fn) return;
    if (!this._l) Object.defineProperty(this, '_l', { value: Object.create(null), writable: true });
    const list = this._l[type] || (this._l[type] = []);
    if (list.some(e => e.fn === fn)) return;
    list.push({ fn, once: !!(opts && typeof opts === 'object' && opts.once) });
  }
  removeEventListener(type, fn) { if (this._l && this._l[type]) this._l[type] = this._l[type].filter(e => e.fn !== fn); }
  dispatchEvent(ev) {
    ev.target = ev.currentTarget = this;
    const h = this['on' + ev.type];
    if (typeof h === 'function') { try { h.call(this, ev); } catch (e) { reportError(e); } }
    const list = this._l && this._l[ev.type];
    if (list) for (const e of list.slice()) {
      if (ev._stopNow) break;
      if (e.once) this.removeEventListener(ev.type, e.fn);
      try { typeof e.fn === 'function' ? e.fn.call(this, ev) : e.fn.handleEvent(ev); } catch (err) { reportError(err); }
    }
    return !ev.defaultPrevented;
  }
}
// The global scope is the event target.
for (const k of ['addEventListener', 'removeEventListener', 'dispatchEvent']) def(k, EventTarget.prototype[k].bind(G));
G.onmessage = null; G.onerror = null; G.onmessageerror = null;
function reportError(e) {
  W.log('Fehler: ' + (e && e.stack ? e + '\n' + e.stack : String(e)));
}
const consoleObj = {};
for (const m of ['log', 'info', 'warn', 'error', 'debug', 'trace', 'dir', 'table']) {
  consoleObj[m] = (...a) => W.log('[Worker] ' + (m === 'log' ? '' : '[' + m + '] ') + a.map(x => {
    if (typeof x === 'string') return x;
    try { return x instanceof Error ? String(x) : JSON.stringify(x); } catch (e) { return String(x); }
  }).join(' '));
}
for (const m of ['group', 'groupCollapsed', 'groupEnd', 'time', 'timeEnd', 'timeLog', 'count', 'countReset', 'assert', 'clear']) consoleObj[m] = () => {};
function makeTimer(repeat) {
  return function (fn, ms, ...args) {
    if (typeof fn === 'string') { const code = fn; fn = () => (0, eval)(code); }
    if (typeof fn !== 'function') return 0;
    return W.timer(args.length ? () => fn(...args) : fn, +ms || 0, repeat);
  };
}
const start = Date.now();
const url = new URL(W.url());
const location = {
  href: url.href, protocol: url.protocol, host: url.host, hostname: url.hostname, port: url.port, pathname: url.pathname,
  search: url.search, hash: url.hash, origin: url.origin, toString() { return this.href; },
};
function fetch(input, init) {
  return new Promise((resolve, reject) => {
    const req = new Request(input, init);
    if (req.signal && req.signal.aborted) { reject(req.signal.reason); return; }
    const flat = [];
    req.headers.forEach((v, k) => flat.push(k, v));
    let body = req._raw;
    if (body instanceof URLSearchParams) body = body.toString();
    else if (body instanceof Blob) body = body._b;
    else if (body !== null && body !== undefined && typeof body !== 'string') body = __kiteBodyBytes(body);
    // Synchronous on this thread, delivered asynchronously.
    setTimeout(() => {
      const r = W.fetchSync(req.method, req.url, body === null || body === undefined ? '' : body, flat);
      if (!r) { reject(new TypeError('Failed to fetch')); return; }
      const h = new Headers();
      for (let i = 0; i < r[3].length; i += 2) h.append(r[3][i], r[3][i + 1]);
      resolve(new Response(r[0] === 204 || r[0] === 304 || req.method === 'HEAD' ? null : r[2], { status: r[0], statusText: r[1], headers: h, url: r[4] }));
    }, 0);
  });
}
class XMLHttpRequest extends EventTarget {
  constructor() { super(); this.readyState = 0; this.status = 0; this.statusText = ''; this.responseType = ''; this._bytes = null; this._h = new Headers(); this._resp = []; }
  open(m, u) { this._m = String(m).toUpperCase(); this._u = String(u); this.readyState = 1; }
  setRequestHeader(k, v) { this._h.append(k, v); }
  getResponseHeader(k) { k = String(k).toLowerCase(); for (let i = 0; i < this._resp.length; i += 2) if (this._resp[i] === k) return this._resp[i + 1]; return null; }
  getAllResponseHeaders() { let s = ''; for (let i = 0; i < this._resp.length; i += 2) s += this._resp[i] + ': ' + this._resp[i + 1] + '\r\n'; return s; }
  get responseText() { return this._bytes ? new TextDecoder().decode(this._bytes) : ''; }
  get response() {
    if (this.responseType === 'json') { try { return JSON.parse(this.responseText); } catch (e) { return null; } }
    if (this.responseType === 'arraybuffer') return this._bytes ? this._bytes.slice().buffer : null;
    if (this.responseType === 'blob') { const b = new Blob([]); b._b = this._bytes || new Uint8Array(0); return b; }
    return this.responseText;
  }
  send(body) {
    const flat = [];
    this._h.forEach((v, k) => flat.push(k, v));
    const go = () => {
      const r = W.fetchSync(this._m || 'GET', this._u, body === undefined || body === null ? '' : typeof body === 'string' ? body : __kiteBodyBytes(body), flat);
      if (!r) { this.readyState = 4; this.dispatchEvent(new Event('readystatechange')); this.dispatchEvent(new Event('error')); return; }
      this.status = r[0]; this.statusText = r[1]; this._bytes = new Uint8Array(r[2]); this._resp = r[3]; this.responseURL = r[4];
      this.readyState = 4;
      this.dispatchEvent(new Event('readystatechange'));
      this.dispatchEvent(new Event('load'));
      this.dispatchEvent(new Event('loadend'));
    };
    setTimeout(go, 0);
  }
  abort() {}
}
const performance = { now: () => Date.now() - start, timeOrigin: start, mark() {}, measure() {}, getEntriesByName: () => [], getEntriesByType: () => [] };
const navigator = { userAgent: 'Mozilla/5.0 (Windows NT 5.0) Kite/1.0 (like Gecko)', language: 'de-DE', languages: ['de-DE', 'de', 'en'],
  hardwareConcurrency: 1, onLine: true, platform: 'Win32' };
Object.assign(G, {
  self: G, console: consoleObj, location, navigator, performance, fetch, XMLHttpRequest,
  Event, MessageEvent, ErrorEvent, CustomEvent, EventTarget,
  setTimeout: makeTimer(false), setInterval: makeTimer(true),
  clearTimeout: id => W.clearTimer(id | 0), clearInterval: id => W.clearTimer(id | 0),
  importScripts: (...urls) => W.importScripts(...urls.map(String)),
  postMessage: (data) => W.post(__kiteSerialize(data)),
  close: () => W.close(),
  name: W.name(),
  WorkerGlobalScope: function () {}, DedicatedWorkerGlobalScope: function () {},
  isSecureContext: true, origin: url.origin,
});
G.__kiteWorkerMessage = function (s) {
  let data;
  try { data = __kiteDeserialize(s); } catch (e) { G.dispatchEvent(new MessageEvent('messageerror', {})); return; }
  G.dispatchEvent(new MessageEvent('message', { data }));
};
})();
)KITEJS";

}  // namespace kite

// The web-facing DOM API of Kite, written in JavaScript on top of the native
// primitives in script.cpp (exposed as the global "__kite").
namespace kite {

extern const char* const kDomPrelude;
const char* const kDomPrelude = R"KITEJS(
(function () {
'use strict';
const K = __kite;
const G = globalThis;
const wrappers = new Map();
const SVG_NS = 'http://www.w3.org/2000/svg';

function kebab(s) { return s.replace(/[A-Z]/g, m => '-' + m.toLowerCase()); }
function camel(s) { return s.replace(/-([a-z])/g, (m, c) => c.toUpperCase()); }
function H(n) { return n && n._h ? n._h : 0; }

// ------------------------------------------------------------------ Events
class Event {
  constructor(type, init) {
    init = init || {};
    this.type = String(type);
    this.bubbles = !!init.bubbles;
    this.cancelable = !!init.cancelable;
    this.composed = !!init.composed;
    this.defaultPrevented = false;
    this.target = null;
    this.currentTarget = null;
    this.eventPhase = 0;
    this.timeStamp = Date.now();
    this.isTrusted = false;
    this._stop = false;
    this._stopNow = false;
  }
  preventDefault() { if (this.cancelable) this.defaultPrevented = true; }
  stopPropagation() { this._stop = true; }
  stopImmediatePropagation() { this._stop = true; this._stopNow = true; }
  composedPath() { return this._path || []; }
  get returnValue() { return !this.defaultPrevented; }
  set returnValue(v) { if (!v) this.preventDefault(); }
  get srcElement() { return this.target; }
  initEvent(type, bubbles, cancelable) { this.type = type; this.bubbles = !!bubbles; this.cancelable = !!cancelable; }
}
Event.NONE = 0; Event.CAPTURING_PHASE = 1; Event.AT_TARGET = 2; Event.BUBBLING_PHASE = 3;
class UIEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.view = G; this.detail = i.detail || 0; } }
class MouseEvent extends UIEvent {
  constructor(t, i) {
    super(t, i); i = i || {};
    this.clientX = i.clientX || 0; this.clientY = i.clientY || 0;
    this.pageX = this.clientX; this.pageY = this.clientY; this.screenX = 0; this.screenY = 0;
    this.offsetX = 0; this.offsetY = 0;
    this.button = i.button || 0; this.buttons = i.buttons || 0;
    this.ctrlKey = !!i.ctrlKey; this.shiftKey = !!i.shiftKey; this.altKey = !!i.altKey; this.metaKey = !!i.metaKey;
    this.relatedTarget = i.relatedTarget || null; this.which = this.button + 1;
  }
}
class PointerEvent extends MouseEvent { constructor(t, i) { super(t, i); this.pointerType = 'mouse'; this.pointerId = 1; } }
class KeyboardEvent extends UIEvent {
  constructor(t, i) { super(t, i); i = i || {}; this.key = i.key || ''; this.code = i.code || ''; this.keyCode = i.keyCode || 0; this.which = this.keyCode; this.ctrlKey = !!i.ctrlKey; this.shiftKey = !!i.shiftKey; this.altKey = !!i.altKey; this.metaKey = !!i.metaKey; this.repeat = false; }
}
class FocusEvent extends UIEvent {}
class InputEvent extends UIEvent { constructor(t, i) { super(t, i); i = i || {}; this.data = i.data || null; this.inputType = i.inputType || ''; } }
class CustomEvent extends Event { constructor(t, i) { super(t, i); this.detail = i && 'detail' in i ? i.detail : null; } initCustomEvent(t, b, c, d) { this.initEvent(t, b, c); this.detail = d; } }
class MessageEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.data = i.data; this.origin = i.origin || ''; this.source = i.source || null; } }
class ErrorEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.message = i.message || ''; this.error = i.error || null; } }
class ProgressEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.loaded = i.loaded || 0; this.total = i.total || 0; this.lengthComputable = !!i.total; } }
class SubmitEvent extends Event { constructor(t, i) { super(t, i); this.submitter = i && i.submitter || null; } }

const EVENT_PROPS = ['click', 'dblclick', 'mousedown', 'mouseup', 'mouseover', 'mouseout', 'mousemove',
  'mouseenter', 'mouseleave', 'keydown', 'keyup', 'keypress', 'focus', 'blur', 'change', 'input', 'submit',
  'reset', 'load', 'error', 'scroll', 'resize', 'select', 'contextmenu', 'wheel', 'touchstart', 'touchend',
  'pointerdown', 'pointerup', 'animationend', 'transitionend', 'toggle', 'beforeunload', 'unload',
  'readystatechange', 'message', 'popstate', 'hashchange', 'DOMContentLoaded', 'abort', 'progress',
  'loadend', 'loadstart', 'timeout'];

class EventTarget {
  addEventListener(type, fn, opts) {
    if (!fn) return;
    const capture = typeof opts === 'boolean' ? opts : !!(opts && opts.capture);
    const once = !!(opts && typeof opts === 'object' && opts.once);
    if (!this._l) Object.defineProperty(this, '_l', { value: Object.create(null), writable: true });
    const list = this._l[type] || (this._l[type] = []);
    for (const e of list) if (e.fn === fn && e.capture === capture) return;
    list.push({ fn, capture, once });
    if (opts && typeof opts === 'object' && opts.signal) opts.signal.addEventListener('abort', () => this.removeEventListener(type, fn, opts));
  }
  removeEventListener(type, fn, opts) {
    if (!this._l || !this._l[type]) return;
    const capture = typeof opts === 'boolean' ? opts : !!(opts && opts.capture);
    this._l[type] = this._l[type].filter(e => !(e.fn === fn && e.capture === capture));
  }
  dispatchEvent(ev) {
    ev.target = this;
    const path = [];
    for (let n = this; n; n = n === G.document ? G : (n === G ? null : n.parentNode)) path.push(n);
    ev._path = path;
    for (let i = path.length - 1; i > 0 && !ev._stop; i--) { ev.eventPhase = 1; invoke(path[i], ev, true); }
    if (!ev._stop) { ev.eventPhase = 2; invoke(this, ev, true); if (!ev._stop) invoke(this, ev, false); }
    if (ev.bubbles) for (let i = 1; i < path.length && !ev._stop; i++) { ev.eventPhase = 3; invoke(path[i], ev, false); }
    ev.eventPhase = 0;
    ev.currentTarget = null;
    return !ev.defaultPrevented;
  }
}

function inlineHandler(node, type) {
  if (node === G) {
    if (G._on && G._on[type]) return G._on[type];
    const body = G.document && G.document.body;
    if (!body || (type !== 'load' && type !== 'unload' && type !== 'beforeunload')) return null;
    node = body;
  } else if (node._on && node._on[type]) {
    return node._on[type];
  }
  if (!node._h || K.type(node._h) !== 1) return null;
  const code = K.attr(node._h, 'on' + type);
  if (code === null) return null;
  if (!node._inl) Object.defineProperty(node, '_inl', { value: Object.create(null), writable: true });
  let c = node._inl[type];
  if (!c || c.code !== code) {
    let fn = null;
    try { fn = new Function('event', code); } catch (e) { console.error(String(e)); }
    c = node._inl[type] = { code, fn };
  }
  return c.fn;
}

function invoke(node, ev, capture) {
  ev.currentTarget = node;
  if (!capture || ev.eventPhase === 2) {
    if (ev.eventPhase !== 2 || !capture) {
      const h = !capture ? inlineHandler(node, ev.type) : null;
      if (h) {
        try { const r = h.call(node, ev); if (r === false) ev.preventDefault(); } catch (e) { reportError(e); }
      }
    }
  }
  const list = node._l && node._l[ev.type];
  if (!list) return;
  for (const e of list.slice()) {
    if (ev._stopNow) break;
    if (ev.eventPhase !== 2 && e.capture !== capture) continue;
    if (ev.eventPhase === 2 && e.capture !== capture) continue;
    if (e.once) node.removeEventListener(ev.type, e.fn, e.capture);
    try {
      if (typeof e.fn === 'function') e.fn.call(node, ev);
      else if (e.fn && typeof e.fn.handleEvent === 'function') e.fn.handleEvent(ev);
    } catch (err) { reportError(err); }
  }
}

function reportError(e) {
  K.log('Fehler: ' + (e && e.stack ? e + '\n' + e.stack : String(e)));
}

// ------------------------------------------------------------------ Collections
function nodeList(arr) {
  arr.item = i => arr[i] || null;
  return arr;
}
function htmlCollection(arr) {
  arr.item = i => arr[i] || null;
  arr.namedItem = name => arr.find(e => e.id === name || e.getAttribute('name') === name) || null;
  return arr;
}

class DOMTokenList {
  constructor(el, attr) { this._el = el; this._attr = attr || 'class'; }
  _get() { return (this._el.getAttribute(this._attr) || '').split(/\s+/).filter(Boolean); }
  _set(list) { this._el.setAttribute(this._attr, list.join(' ')); }
  get length() { return this._get().length; }
  get value() { return this._el.getAttribute(this._attr) || ''; }
  set value(v) { this._el.setAttribute(this._attr, v); }
  item(i) { return this._get()[i] || null; }
  contains(t) { return this._get().indexOf(t) >= 0; }
  add(...ts) { const l = this._get(); for (const t of ts) if (l.indexOf(t) < 0) l.push(t); this._set(l); }
  remove(...ts) { this._set(this._get().filter(x => ts.indexOf(x) < 0)); }
  toggle(t, force) {
    const has = this.contains(t);
    if (force === true || (force === undefined && !has)) { if (!has) this.add(t); return true; }
    if (has) this.remove(t);
    return false;
  }
  replace(a, b) { const l = this._get(); const i = l.indexOf(a); if (i < 0) return false; l[i] = b; this._set(l); return true; }
  supports() { return true; }
  forEach(fn, self) { this._get().forEach(fn, self); }
  entries() { return this._get().entries(); }
  keys() { return this._get().keys(); }
  values() { return this._get().values(); }
  [Symbol.iterator]() { return this._get()[Symbol.iterator](); }
  toString() { return this.value; }
}

function parseStyle(text) {
  const map = new Map();
  for (const part of (text || '').split(';')) {
    const i = part.indexOf(':');
    if (i < 0) continue;
    const k = part.slice(0, i).trim().toLowerCase();
    if (k) map.set(k, part.slice(i + 1).trim());
  }
  return map;
}
function styleDecl(el) {
  const target = {
    getPropertyValue(p) { const v = parseStyle(el.getAttribute('style')).get(p.toLowerCase()); return v === undefined ? '' : v.replace(/\s*!important$/, ''); },
    getPropertyPriority(p) { const v = parseStyle(el.getAttribute('style')).get(p.toLowerCase()) || ''; return /!important$/.test(v) ? 'important' : ''; },
    setProperty(p, v, prio) {
      const m = parseStyle(el.getAttribute('style'));
      p = p.startsWith('--') ? p : p.toLowerCase();
      if (v === null || v === undefined || v === '') m.delete(p); else m.set(p, String(v) + (prio === 'important' ? ' !important' : ''));
      el.setAttribute('style', Array.from(m, ([k, val]) => k + ': ' + val).join('; '));
    },
    removeProperty(p) { const old = this.getPropertyValue(p); this.setProperty(p, ''); return old; },
    item(i) { return Array.from(parseStyle(el.getAttribute('style')).keys())[i] || ''; },
  };
  return new Proxy(target, {
    get(t, k) {
      if (k in t) return t[k];
      if (k === 'cssText') return el.getAttribute('style') || '';
      if (k === 'length') return parseStyle(el.getAttribute('style')).size;
      if (typeof k !== 'string') return undefined;
      if (/^\d+$/.test(k)) return t.item(+k);
      return t.getPropertyValue(k === 'cssFloat' ? 'float' : kebab(k));
    },
    set(t, k, v) {
      if (k === 'cssText') { el.setAttribute('style', String(v)); return true; }
      if (typeof k === 'string') t.setProperty(k === 'cssFloat' ? 'float' : (k.startsWith('--') ? k : kebab(k)), v);
      return true;
    },
  });
}

function dataset(el) {
  return new Proxy({}, {
    get(t, k) { if (typeof k !== 'string') return undefined; const v = el.getAttribute('data-' + kebab(k)); return v === null ? undefined : v; },
    set(t, k, v) { el.setAttribute('data-' + kebab(k), String(v)); return true; },
    has(t, k) { return el.hasAttribute('data-' + kebab(k)); },
    deleteProperty(t, k) { el.removeAttribute('data-' + kebab(k)); return true; },
    ownKeys() { return el.getAttributeNames().filter(n => n.startsWith('data-')).map(n => camel(n.slice(5))); },
    getOwnPropertyDescriptor(t, k) { const v = el.getAttribute('data-' + kebab(k)); return v === null ? undefined : { value: v, enumerable: true, configurable: true }; },
  });
}

// ------------------------------------------------------------------ Nodes
class Node extends EventTarget {
  get nodeType() { return K.type(this._h); }
  get nodeName() {
    switch (this.nodeType) {
      case 1: return this.tagName; case 3: return '#text'; case 8: return '#comment';
      case 9: return '#document'; case 11: return '#document-fragment'; default: return 'html';
    }
  }
  get nodeValue() { const t = this.nodeType; return t === 3 || t === 8 ? K.data(this._h) : null; }
  set nodeValue(v) { const t = this.nodeType; if (t === 3 || t === 8) K.setData(this._h, String(v)); }
  get ownerDocument() { return this.nodeType === 9 ? null : G.document; }
  get parentNode() { return W(K.parent(this._h)); }
  get parentElement() { const p = this.parentNode; return p && p.nodeType === 1 ? p : null; }
  get childNodes() { return nodeList(K.kids(this._h).map(W)); }
  get firstChild() { const k = K.kids(this._h); return k.length ? W(k[0]) : null; }
  get lastChild() { const k = K.kids(this._h); return k.length ? W(k[k.length - 1]) : null; }
  _sibling(d) {
    const p = K.parent(this._h); if (!p) return null;
    const k = K.kids(p); const i = k.indexOf(this._h);
    return W(k[i + d]) || null;
  }
  get nextSibling() { return this._sibling(1); }
  get previousSibling() { return this._sibling(-1); }
  get isConnected() { let n = this._h; while (n) { if (K.type(n) === 9) return true; n = K.parent(n); } return false; }
  get textContent() { const t = this.nodeType; if (t === 3 || t === 8) return K.data(this._h); if (t === 9) return null; return K.text(this._h); }
  set textContent(v) {
    const t = this.nodeType;
    if (t === 3 || t === 8) { K.setData(this._h, String(v)); return; }
    for (const c of K.kids(this._h)) K.remove(c);
    if (v !== null && v !== undefined && String(v) !== '') K.insert(this._h, K.create(3, String(v)), 0);
  }
  hasChildNodes() { return K.kids(this._h).length > 0; }
  getRootNode() { let n = this; while (n.parentNode) n = n.parentNode; return n; }
  contains(o) { for (let n = o; n; n = n.parentNode) if (n === this) return true; return false; }
  appendChild(c) { checkNode(c); K.insert(this._h, c._h, 0); return c; }
  insertBefore(c, ref) { checkNode(c); K.insert(this._h, c._h, H(ref)); return c; }
  removeChild(c) { if (!c || K.parent(c._h) !== this._h) throw new DOMException('NotFoundError', 'NotFoundError'); K.remove(c._h); return c; }
  replaceChild(n, old) { this.insertBefore(n, old); return this.removeChild(old); }
  cloneNode(deep) {
    const t = this.nodeType;
    if (t === 3) return G.document.createTextNode(this.data);
    if (t === 8) return G.document.createComment(this.data);
    if (t === 11) { const f = G.document.createDocumentFragment(); if (deep) for (const c of this.childNodes) f.appendChild(c.cloneNode(true)); return f; }
    if (t !== 1) return null;
    if (deep) { const hs = K.parse(K.html(this._h, true), parseContextFor(this.parentNode)); for (const h of hs) if (K.type(h) === 1) return W(h); }
    const el = G.document.createElement(this.localName);
    const a = K.attrs(this._h);
    for (let i = 0; i < a.length; i += 2) K.setAttr(el._h, a[i], a[i + 1]);
    return el;
  }
  isEqualNode(o) { return !!o && o.nodeType === this.nodeType && (this.nodeType === 1 ? K.html(this._h, true) === K.html(o._h, true) : this.nodeValue === o.nodeValue); }
  isSameNode(o) { return o === this; }
  compareDocumentPosition(o) {
    if (o === this) return 0;
    if (this.contains(o)) return 20;
    if (o.contains(this)) return 10;
    return 4;
  }
  normalize() {}
  lookupNamespaceURI() { return null; }
}
Node.ELEMENT_NODE = 1; Node.TEXT_NODE = 3; Node.COMMENT_NODE = 8; Node.DOCUMENT_NODE = 9;
Node.DOCUMENT_FRAGMENT_NODE = 11; Node.DOCUMENT_POSITION_FOLLOWING = 4; Node.DOCUMENT_POSITION_CONTAINED_BY = 16;

function checkNode(c) { if (!c || !c._h) throw new TypeError('Argument is not a Node'); }
function toNode(x) { return x && x._h ? x : G.document.createTextNode(String(x)); }
function parseContextFor(p) {
  const t = p && p.nodeType === 1 ? p.localName : '';
  return ['table', 'tbody', 'thead', 'tfoot', 'tr', 'select', 'colgroup'].indexOf(t) >= 0 ? t : '';
}

const ChildMixin = {
  get children() { return htmlCollection(K.kids(this._h).filter(h => K.type(h) === 1).map(W)); },
  get childElementCount() { return this.children.length; },
  get firstElementChild() { return this.children[0] || null; },
  get lastElementChild() { const c = this.children; return c[c.length - 1] || null; },
  querySelector(s) { return W(K.query(this._h, String(s), false)); },
  querySelectorAll(s) { return nodeList(K.query(this._h, String(s), true).map(W)); },
  getElementsByTagName(t) { t = String(t).toLowerCase(); return htmlCollection(K.query(this._h, t === '*' ? '*' : t.replace(/[^a-z0-9-]/g, ''), true).map(W)); },
  getElementsByClassName(c) { const cs = String(c).split(/\s+/).filter(Boolean); if (!cs.length) return htmlCollection([]); return htmlCollection(K.query(this._h, cs.map(x => '.' + cssEscape(x)).join(''), true).map(W)); },
  append(...ns) { for (const n of ns) this.appendChild(toNode(n)); },
  prepend(...ns) { const first = this.firstChild; for (const n of ns) this.insertBefore(toNode(n), first); },
  replaceChildren(...ns) { for (const c of K.kids(this._h)) K.remove(c); this.append(...ns); },
};
const SiblingMixin = {
  before(...ns) { const p = this.parentNode; if (p) for (const n of ns) p.insertBefore(toNode(n), this); },
  after(...ns) { const p = this.parentNode; if (!p) return; const next = this.nextSibling; for (const n of ns) p.insertBefore(toNode(n), next); },
  replaceWith(...ns) { const p = this.parentNode; if (!p) return; this.before(...ns); p.removeChild(this); },
  remove() { if (K.parent(this._h)) K.remove(this._h); },
  get nextElementSibling() { let n = this.nextSibling; while (n && n.nodeType !== 1) n = n.nextSibling; return n; },
  get previousElementSibling() { let n = this.previousSibling; while (n && n.nodeType !== 1) n = n.previousSibling; return n; },
};
function mixin(cls, m) { for (const k of Object.getOwnPropertyNames(m)) Object.defineProperty(cls.prototype, k, Object.getOwnPropertyDescriptor(m, k)); }

class CharacterData extends Node {
  get data() { return K.data(this._h); }
  set data(v) { K.setData(this._h, String(v)); }
  get length() { return this.data.length; }
  appendData(s) { this.data += s; }
  substringData(o, c) { return this.data.substr(o, c); }
}
mixin(CharacterData, SiblingMixin);
class Text extends CharacterData {
  constructor(s) { super(); if (new.target) return G.document.createTextNode(s === undefined ? '' : s); }
  get wholeText() { return this.data; }
  splitText(o) { const t = G.document.createTextNode(this.data.slice(o)); this.data = this.data.slice(0, o); this.after(t); return t; }
}
class Comment extends CharacterData {}
class DocumentFragment extends Node {
  constructor() { super(); if (new.target) return G.document.createDocumentFragment(); }
  getElementById(id) { return this.querySelector('[id="' + String(id).replace(/"/g, '\\"') + '"]'); }
}
mixin(DocumentFragment, ChildMixin);

function cssEscape(s) { return String(s).replace(/([^a-zA-Z0-9_\u00a0-\uffff-])/g, '\\$1').replace(/^(\d)/, '\\3$1 '); }

class Element extends Node {
  get tagName() { const n = K.name(this._h); return this._svg() ? n : n.toUpperCase(); }
  get localName() { return K.name(this._h); }
  get namespaceURI() { return this._svg() ? SVG_NS : 'http://www.w3.org/1999/xhtml'; }
  _svg() { for (let h = this._h; h; h = K.parent(h)) { if (K.type(h) !== 1) return false; if (K.name(h) === 'svg') return true; } return false; }
  get prefix() { return null; }
  get id() { return this.getAttribute('id') || ''; }
  set id(v) { this.setAttribute('id', v); }
  get className() { return this.getAttribute('class') || ''; }
  set className(v) { this.setAttribute('class', v); }
  get classList() { return new DOMTokenList(this, 'class'); }
  get relList() { return new DOMTokenList(this, 'rel'); }
  getAttribute(n) { return K.attr(this._h, String(n)); }
  getAttributeNS(ns, n) { return this.getAttribute(n); }
  setAttribute(n, v) { K.setAttr(this._h, String(n), String(v)); }
  setAttributeNS(ns, n, v) { this.setAttribute(n.replace(/^.*:/, ''), v); }
  removeAttribute(n) { K.delAttr(this._h, String(n)); }
  removeAttributeNS(ns, n) { this.removeAttribute(n); }
  hasAttribute(n) { return K.attr(this._h, String(n)) !== null; }
  hasAttributeNS(ns, n) { return this.hasAttribute(n); }
  hasAttributes() { return K.attrs(this._h).length > 0; }
  toggleAttribute(n, force) {
    const has = this.hasAttribute(n);
    if (force === true || (force === undefined && !has)) { this.setAttribute(n, ''); return true; }
    this.removeAttribute(n); return false;
  }
  getAttributeNames() { const a = K.attrs(this._h); const r = []; for (let i = 0; i < a.length; i += 2) r.push(a[i]); return r; }
  get attributes() {
    const a = K.attrs(this._h); const r = [];
    for (let i = 0; i < a.length; i += 2) r.push({ name: a[i], localName: a[i], value: a[i + 1], nodeName: a[i], specified: true });
    r.getNamedItem = n => r.find(x => x.name === String(n).toLowerCase()) || null;
    r.item = i => r[i] || null;
    return r;
  }
  get dataset() { return dataset(this); }
  get style() { return styleDecl(this); }
  set style(v) { this.setAttribute('style', v); }
  get innerHTML() { return K.html(this._h, false); }
  set innerHTML(v) {
    for (const c of K.kids(this._h)) K.remove(c);
    const target = this.localName === 'template' ? this._h : this._h;
    for (const h of K.parse(String(v), parseContextFor(this) || this.localName)) K.insert(target, h, 0);
  }
  get outerHTML() { return K.html(this._h, true); }
  set outerHTML(v) {
    const p = this.parentNode; if (!p) return;
    for (const h of K.parse(String(v), parseContextFor(p))) K.insert(p._h, h, this._h);
    K.remove(this._h);
  }
  get innerText() { return this.textContent; }
  set innerText(v) { this.textContent = v; }
  get outerText() { return this.textContent; }
  insertAdjacentHTML(pos, html) {
    const hs = K.parse(String(html), parseContextFor(pos === 'beforebegin' || pos === 'afterend' ? this.parentNode : this));
    const p = this.parentNode;
    switch (String(pos).toLowerCase()) {
      case 'beforebegin': for (const h of hs) K.insert(p._h, h, this._h); break;
      case 'afterbegin': { const f = K.kids(this._h)[0] || 0; for (const h of hs) K.insert(this._h, h, f); break; }
      case 'beforeend': for (const h of hs) K.insert(this._h, h, 0); break;
      case 'afterend': { const n = this.nextSibling; for (const h of hs) K.insert(p._h, h, H(n)); break; }
    }
  }
  insertAdjacentElement(pos, el) {
    switch (String(pos).toLowerCase()) {
      case 'beforebegin': this.before(el); break;
      case 'afterbegin': this.prepend(el); break;
      case 'beforeend': this.append(el); break;
      case 'afterend': this.after(el); break;
    }
    return el;
  }
  insertAdjacentText(pos, t) { this.insertAdjacentElement(pos, G.document.createTextNode(t)); }
  matches(s) { return K.matches(this._h, String(s)); }
  webkitMatchesSelector(s) { return this.matches(s); }
  msMatchesSelector(s) { return this.matches(s); }
  closest(s) { for (let n = this; n && n.nodeType === 1; n = n.parentNode) if (n.matches(s)) return n; return null; }
  getBoundingClientRect() { const r = K.rect(this._h); return domRect(r[0], r[1], r[2], r[3]); }
  getClientRects() { const r = this.getBoundingClientRect(); return r.width || r.height ? [r] : []; }
  get offsetWidth() { return Math.round(K.rect(this._h)[2]); }
  get offsetHeight() { return Math.round(K.rect(this._h)[3]); }
  get offsetLeft() { const r = K.rect(this._h); return Math.round(r[0] + G.scrollX); }
  get offsetTop() { const r = K.rect(this._h); return Math.round(r[1] + G.scrollY); }
  get offsetParent() { return this.isConnected ? G.document.body : null; }
  get clientWidth() { return this === G.document.documentElement ? G.innerWidth : this.offsetWidth; }
  get clientHeight() { return this === G.document.documentElement ? G.innerHeight : this.offsetHeight; }
  get clientTop() { return 0; }
  get clientLeft() { return 0; }
  get scrollWidth() { return this.clientWidth; }
  get scrollHeight() { return this === G.document.documentElement || this === G.document.body ? K.viewport()[2] : this.clientHeight; }
  get scrollTop() { return this === G.document.documentElement || this === G.document.body ? G.scrollY : 0; }
  set scrollTop(v) { if (this === G.document.documentElement || this === G.document.body) G.scrollTo(G.scrollX, v); }
  get scrollLeft() { return 0; }
  set scrollLeft(v) {}
  scrollIntoView() { const r = K.rect(this._h); G.scrollTo(0, r[1] + G.scrollY); }
  scrollTo() {}
  scrollBy() {}
  scroll() {}
  focus() { G.document._active = this; }
  blur() { if (G.document._active === this) G.document._active = null; }
  click() {
    if (this.disabled) return;
    const type = (this.getAttribute('type') || '').toLowerCase();
    const tag = this.localName;
    if (tag === 'input' && (type === 'checkbox' || type === 'radio')) this.checked = type === 'radio' ? true : !this.checked;
    const ok = this.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true }));
    if (!ok) return;
    const link = this.closest('a[href]');
    if (link) { K.navigate(link.getAttribute('href'), false); return; }
    if ((tag === 'button' && (type === '' || type === 'submit')) || (tag === 'input' && (type === 'submit' || type === 'image'))) {
      const f = this.form; if (f) f.requestSubmit(this);
    }
    if (tag === 'input' && (type === 'checkbox' || type === 'radio')) {
      this.dispatchEvent(new Event('input', { bubbles: true }));
      this.dispatchEvent(new Event('change', { bubbles: true }));
    }
  }
  animate() { const a = { finished: Promise.resolve(), cancel() {}, finish() {}, play() {}, pause() {}, reverse() {}, onfinish: null, addEventListener() {} }; setTimeout(() => { if (a.onfinish) a.onfinish(); }, 0); return a; }
  getAnimations() { return []; }
  attachShadow() { const sr = this; sr.host = this; return sr; }
  get shadowRoot() { return null; }
  get slot() { return ''; }
  requestFullscreen() { return Promise.reject(new Error('not supported')); }
  setPointerCapture() {}
  releasePointerCapture() {}
  hasPointerCapture() { return false; }
}
mixin(Element, ChildMixin);
mixin(Element, SiblingMixin);

function domRect(x, y, w, h) {
  return { x, y, width: w, height: h, left: x, top: y, right: x + w, bottom: y + h, toJSON() { return { x, y, width: w, height: h }; } };
}

function urlPart(el, attr, part) {
  const v = el.getAttribute(attr);
  if (v === null) return '';
  const abs = K.resolve(v);
  if (!abs) return part === 'href' ? v : '';
  return part === 'href' ? abs : new URL(abs)[part];
}

class HTMLElement extends Element {
  get title() { return this.getAttribute('title') || ''; }
  set title(v) { this.setAttribute('title', v); }
  get lang() { return this.getAttribute('lang') || ''; }
  set lang(v) { this.setAttribute('lang', v); }
  get dir() { return this.getAttribute('dir') || ''; }
  set dir(v) { this.setAttribute('dir', v); }
  get hidden() { return this.hasAttribute('hidden'); }
  set hidden(v) { this.toggleAttribute('hidden', !!v); }
  get tabIndex() { const v = parseInt(this.getAttribute('tabindex'), 10); return isNaN(v) ? -1 : v; }
  set tabIndex(v) { this.setAttribute('tabindex', v); }
  get accessKey() { return this.getAttribute('accesskey') || ''; }
  get draggable() { return this.getAttribute('draggable') === 'true'; }
  get isContentEditable() { return false; }
  get contentEditable() { return 'inherit'; }
  // Links
  get href() { return urlPart(this, 'href', 'href'); }
  set href(v) { this.setAttribute('href', v); }
  get protocol() { return urlPart(this, 'href', 'protocol'); }
  get host() { return urlPart(this, 'href', 'host'); }
  get hostname() { return urlPart(this, 'href', 'hostname'); }
  get port() { return urlPart(this, 'href', 'port'); }
  get pathname() { return urlPart(this, 'href', 'pathname'); }
  get search() { return urlPart(this, 'href', 'search'); }
  get hash() { return urlPart(this, 'href', 'hash'); }
  get origin() { return urlPart(this, 'href', 'origin'); }
  get target() { return this.getAttribute('target') || ''; }
  set target(v) { this.setAttribute('target', v); }
  get rel() { return this.getAttribute('rel') || ''; }
  set rel(v) { this.setAttribute('rel', v); }
  get download() { return this.getAttribute('download') || ''; }
  // Media / scripts
  get src() { return urlPart(this, 'src', 'href'); }
  set src(v) {
    this.setAttribute('src', v);
    if (this.localName !== 'img') return;
    const st = K.imgLoad(this._h);  // 0 pending (load/error fire later), 1 loaded, 2 failed
    if (st === 1) setTimeout(() => this.dispatchEvent(new Event('load')), 0);
    else if (st === 2) setTimeout(() => this.dispatchEvent(new Event('error')), 0);
  }
  get srcset() { return this.getAttribute('srcset') || ''; }
  set srcset(v) { this.setAttribute('srcset', v); }
  get alt() { return this.getAttribute('alt') || ''; }
  set alt(v) { this.setAttribute('alt', v); }
  get width() { const v = parseInt(this.getAttribute('width'), 10); return isNaN(v) ? (this.isConnected ? this.offsetWidth : (this.localName === 'img' ? K.imgSize(this._h)[0] : 0)) : v; }
  set width(v) { this.setAttribute('width', v); }
  get height() { const v = parseInt(this.getAttribute('height'), 10); return isNaN(v) ? (this.isConnected ? this.offsetHeight : (this.localName === 'img' ? K.imgSize(this._h)[1] : 0)) : v; }
  set height(v) { this.setAttribute('height', v); }
  get complete() { return this.localName !== 'img' || !this.getAttribute('src') || K.imgSize(this._h)[0] > 0; }
  get naturalWidth() { return this.localName === 'img' ? K.imgSize(this._h)[0] : 0; }
  get naturalHeight() { return this.localName === 'img' ? K.imgSize(this._h)[1] : 0; }
  get currentSrc() { return this.src; }
  get loading() { return this.getAttribute('loading') || 'auto'; }
  set loading(v) { this.setAttribute('loading', v); }
  decode() { return Promise.resolve(); }
  get text() { return this.textContent; }
  set text(v) { this.textContent = v; }
  get async() { return this.hasAttribute('async'); }
  set async(v) { this.toggleAttribute('async', !!v); }
  get defer() { return this.hasAttribute('defer'); }
  set defer(v) { this.toggleAttribute('defer', !!v); }
  get type() {
    const t = this.getAttribute('type');
    if (this.localName === 'input') return (t || 'text').toLowerCase();
    if (this.localName === 'button') return (t || 'submit').toLowerCase();
    if (this.localName === 'select') return this.hasAttribute('multiple') ? 'select-multiple' : 'select-one';
    if (this.localName === 'textarea') return 'textarea';
    return t || '';
  }
  set type(v) { this.setAttribute('type', v); }
  get media() { return this.getAttribute('media') || ''; }
  get sheet() { return null; }
  get content() {
    if (this.localName === 'meta') return this.getAttribute('content') || '';
    if (this.localName !== 'template') return undefined;
    const f = G.document.createDocumentFragment();
    for (const h of K.parse(this.innerHTML, '')) K.insert(f._h, h, 0);
    return f;
  }
  set content(v) { if (this.localName === 'meta') this.setAttribute('content', v); }
  get name() { return this.getAttribute('name') || ''; }
  set name(v) { this.setAttribute('name', v); }
  // Forms
  get value() {
    if (this.localName === 'option') { const v = this.getAttribute('value'); return v === null ? this.text.trim() : v; }
    if (this.localName === 'button' || this.localName === 'data' || this.localName === 'li') return this.getAttribute('value') || '';
    return K.value(this._h);
  }
  set value(v) {
    if (this.localName === 'option' || this.localName === 'button') { this.setAttribute('value', v); return; }
    K.setValue(this._h, v === null || v === undefined ? '' : String(v));
  }
  get defaultValue() { return this.localName === 'textarea' ? this.textContent : (this.getAttribute('value') || ''); }
  set defaultValue(v) { if (this.localName === 'textarea') this.textContent = v; else this.setAttribute('value', v); }
  get valueAsNumber() { return parseFloat(this.value); }
  get checked() { return K.checked(this._h); }
  set checked(v) {
    v = !!v;
    if (v && this.type === 'radio' && this.name) {
      const scope = this.form || G.document;
      for (const r of scope.querySelectorAll('input[type=radio]')) if (r !== this && r.name === this.name) K.setChecked(r._h, false);
    }
    K.setChecked(this._h, v);
  }
  get defaultChecked() { return this.hasAttribute('checked'); }
  get indeterminate() { return false; }
  set indeterminate(v) {}
  get disabled() { return this.hasAttribute('disabled'); }
  set disabled(v) { this.toggleAttribute('disabled', !!v); }
  get readOnly() { return this.hasAttribute('readonly'); }
  set readOnly(v) { this.toggleAttribute('readonly', !!v); }
  get required() { return this.hasAttribute('required'); }
  set required(v) { this.toggleAttribute('required', !!v); }
  get placeholder() { return this.getAttribute('placeholder') || ''; }
  set placeholder(v) { this.setAttribute('placeholder', v); }
  get multiple() { return this.hasAttribute('multiple'); }
  get maxLength() { const v = parseInt(this.getAttribute('maxlength'), 10); return isNaN(v) ? -1 : v; }
  get selectedIndex() { return K.selIndex(this._h); }
  set selectedIndex(i) { K.selIndex(this._h, i | 0); }
  get options() { const o = htmlCollection(this.querySelectorAll('option').slice()); Object.defineProperty(o, 'selectedIndex', { get: () => this.selectedIndex, set: i => { this.selectedIndex = i; } }); o.add = el => this.appendChild(el); o.remove = i => { const x = o[i]; if (x) x.remove(); }; return o; }
  get selectedOptions() { const o = this.options; return htmlCollection(o.filter((x, i) => i === this.selectedIndex)); }
  get selected() { const s = this.closest('select'); return !!s && s.options[s.selectedIndex] === this; }
  set selected(v) { const s = this.closest('select'); if (s && v) s.selectedIndex = s.options.indexOf(this); }
  get index() { const s = this.closest('select'); return s ? s.options.indexOf(this) : 0; }
  get label() { return this.getAttribute('label') || this.text; }
  get form() {
    const id = this.getAttribute('form');
    if (id) return G.document.getElementById(id);
    return this.closest('form');
  }
  get labels() { return this.id ? G.document.querySelectorAll('label[for="' + this.id + '"]') : []; }
  get htmlFor() { return this.getAttribute('for') || ''; }
  set htmlFor(v) { this.setAttribute('for', v); }
  get control() { const f = this.htmlFor; return f ? G.document.getElementById(f) : this.querySelector('input,select,textarea'); }
  get elements() { return htmlCollection(this.querySelectorAll('input,select,textarea,button,fieldset,output').slice()); }
  get length() { return this.localName === 'form' ? this.elements.length : this.localName === 'select' ? this.options.length : undefined; }
  get action() { const a = this.getAttribute('action'); return a ? K.resolve(a) : K.url(); }
  set action(v) { this.setAttribute('action', v); }
  get method() { return (this.getAttribute('method') || 'get').toLowerCase(); }
  set method(v) { this.setAttribute('method', v); }
  get enctype() { return this.getAttribute('enctype') || 'application/x-www-form-urlencoded'; }
  get validity() { return { valid: true, valueMissing: false, typeMismatch: false, patternMismatch: false, tooLong: false, tooShort: false, rangeUnderflow: false, rangeOverflow: false, stepMismatch: false, badInput: false, customError: false }; }
  get validationMessage() { return ''; }
  get willValidate() { return false; }
  checkValidity() { return true; }
  reportValidity() { return true; }
  setCustomValidity() {}
  select() {}
  setSelectionRange() {}
  get selectionStart() { return this.value.length; }
  get selectionEnd() { return this.value.length; }
  submit() { if (this.localName === 'form') K.submit(this._h); }
  requestSubmit(submitter) {
    if (this.localName !== 'form') return;
    const ok = this.dispatchEvent(new SubmitEvent('submit', { bubbles: true, cancelable: true, submitter: submitter || null }));
    if (ok) K.submit(this._h);
  }
  reset() { if (this.localName === 'form') for (const el of this.elements) { if ('value' in el && el.localName !== 'button') el.value = el.defaultValue; } }
  get open() { return this.hasAttribute('open'); }
  set open(v) { this.toggleAttribute('open', !!v); }
  showModal() { this.setAttribute('open', ''); }
  show() { this.setAttribute('open', ''); }
  close() { this.removeAttribute('open'); }
  // Media elements
  play() { return Promise.reject(new Error('Medienwiedergabe wird nicht unterstuetzt')); }
  pause() {}
  load() {}
  canPlayType() { return ''; }
  get paused() { return true; }
  get muted() { return true; }
  set muted(v) {}
  get currentTime() { return 0; }
  set currentTime(v) {}
  get duration() { return NaN; }
  // Canvas
  getContext() { return null; }
  toDataURL() { return 'data:,'; }
  // Tables
  get rows() { return htmlCollection(this.querySelectorAll('tr').slice()); }
  get cells() { return htmlCollection(this.children.filter(c => c.localName === 'td' || c.localName === 'th')); }
  get tBodies() { return htmlCollection(this.children.filter(c => c.localName === 'tbody')); }
  insertRow() { const tr = G.document.createElement('tr'); (this.tBodies[0] || this).appendChild(tr); return tr; }
  insertCell() { const td = G.document.createElement('td'); this.appendChild(td); return td; }
  get colSpan() { return parseInt(this.getAttribute('colspan'), 10) || 1; }
  set colSpan(v) { this.setAttribute('colspan', v); }
}
EVENT_PROPS.forEach(t => {
  Object.defineProperty(EventTarget.prototype, 'on' + t, {
    get() { return this._on ? this._on[t] || null : null; },
    set(fn) { if (!this._on) Object.defineProperty(this, '_on', { value: Object.create(null), writable: true }); this._on[t] = typeof fn === 'function' ? fn : null; },
    configurable: true,
  });
});

const TAG_CLASSES = {};
function defineTag(name, tags) {
  const cls = { [name]: class extends HTMLElement {} }[name];
  G[name] = cls;
  for (const t of tags) TAG_CLASSES[t] = cls;
}
defineTag('HTMLAnchorElement', ['a']); defineTag('HTMLDivElement', ['div']); defineTag('HTMLSpanElement', ['span']);
defineTag('HTMLImageElement', ['img']); defineTag('HTMLInputElement', ['input']); defineTag('HTMLFormElement', ['form']);
defineTag('HTMLButtonElement', ['button']); defineTag('HTMLSelectElement', ['select']); defineTag('HTMLOptionElement', ['option']);
defineTag('HTMLTextAreaElement', ['textarea']); defineTag('HTMLScriptElement', ['script']); defineTag('HTMLStyleElement', ['style']);
defineTag('HTMLLinkElement', ['link']); defineTag('HTMLMetaElement', ['meta']); defineTag('HTMLHeadElement', ['head']);
defineTag('HTMLBodyElement', ['body']); defineTag('HTMLHtmlElement', ['html']); defineTag('HTMLParagraphElement', ['p']);
defineTag('HTMLHeadingElement', ['h1', 'h2', 'h3', 'h4', 'h5', 'h6']); defineTag('HTMLUListElement', ['ul']);
defineTag('HTMLOListElement', ['ol']); defineTag('HTMLLIElement', ['li']); defineTag('HTMLTableElement', ['table']);
defineTag('HTMLTableRowElement', ['tr']); defineTag('HTMLTableCellElement', ['td', 'th']);
defineTag('HTMLTableSectionElement', ['tbody', 'thead', 'tfoot']); defineTag('HTMLLabelElement', ['label']);
defineTag('HTMLIFrameElement', ['iframe']); defineTag('HTMLVideoElement', ['video']);
defineTag('HTMLAudioElement', ['audio']); defineTag('HTMLTemplateElement', ['template']); defineTag('HTMLDetailsElement', ['details']);
defineTag('HTMLDialogElement', ['dialog']); defineTag('HTMLPreElement', ['pre']); defineTag('HTMLBRElement', ['br']);
defineTag('HTMLHRElement', ['hr']); defineTag('HTMLTitleElement', ['title']); defineTag('HTMLFieldSetElement', ['fieldset']);
defineTag('HTMLLegendElement', ['legend']); defineTag('HTMLPictureElement', ['picture']); defineTag('HTMLSourceElement', ['source']);
defineTag('HTMLOutputElement', ['output']); defineTag('HTMLProgressElement', ['progress']); defineTag('HTMLDataListElement', ['datalist']);
defineTag('HTMLObjectElement', ['object']); defineTag('HTMLEmbedElement', ['embed']); defineTag('HTMLSlotElement', ['slot']);
G.HTMLMediaElement = HTMLElement;
G.HTMLUnknownElement = class extends HTMLElement {};
class SVGElement extends Element {
  get ownerSVGElement() { return this.closest('svg'); }
  get className() { const v = this.getAttribute('class') || ''; return { baseVal: v, animVal: v }; }
  set className(v) { this.setAttribute('class', v); }
  getBBox() { const r = this.getBoundingClientRect(); return { x: 0, y: 0, width: r.width, height: r.height }; }
}
class SVGSVGElement extends SVGElement {}

function protoFor(h) {
  const tag = K.name(h);
  for (let p = K.parent(h); p; p = K.parent(p)) {
    if (K.type(p) !== 1) break;
    if (K.name(p) === 'svg') return SVGElement.prototype;
  }
  if (tag === 'svg') return SVGSVGElement.prototype;
  const cls = TAG_CLASSES[tag];
  if (cls) return cls.prototype;
  const ce = customRegistry.get(tag);
  if (ce) return ce.prototype;
  return HTMLElement.prototype;
}

function W(h) {
  if (!h) return null;
  let w = wrappers.get(h);
  if (w) return w;
  const t = K.type(h);
  let proto;
  if (t === 9) return G.document;
  if (t === 1) proto = protoFor(h);
  else if (t === 3) proto = Text.prototype;
  else if (t === 8) proto = Comment.prototype;
  else if (t === 11) proto = DocumentFragment.prototype;
  else proto = Node.prototype;
  w = Object.create(proto);
  Object.defineProperty(w, '_h', { value: h });
  wrappers.set(h, w);
  return w;
}

// ------------------------------------------------------------------ Canvas
const CAPS = { butt: 0, round: 1, square: 2 };
const JOINS = { miter: 0, round: 1, bevel: 2 };
const OPS = { 'source-over': 0, 'source-in': 1, 'source-out': 2, 'source-atop': 3, 'destination-over': 4,
  'destination-in': 5, 'destination-out': 6, 'destination-atop': 7, 'lighter': 8, 'copy': 9, 'xor': 10 };
const ALIGNS = { start: 0, left: 0, center: 1, right: 2, end: 2 };
const BASELINES = { alphabetic: 0, top: 1, middle: 2, bottom: 3, hanging: 4, ideographic: 5 };

class DOMMatrix {
  constructor(init) {
    let v = [1, 0, 0, 1, 0, 0];
    if (Array.isArray(init) && init.length >= 6) v = init.length === 16 ? [init[0], init[1], init[4], init[5], init[12], init[13]] : init.slice(0, 6);
    else if (typeof init === 'string' && /matrix\(/.test(init)) v = init.replace(/^.*\(|\).*$/g, '').split(',').map(Number);
    [this.a, this.b, this.c, this.d, this.e, this.f] = v.map(Number);
  }
  get m11() { return this.a; } set m11(v) { this.a = v; }
  get m12() { return this.b; } set m12(v) { this.b = v; }
  get m21() { return this.c; } set m21(v) { this.c = v; }
  get m22() { return this.d; } set m22(v) { this.d = v; }
  get m41() { return this.e; } set m41(v) { this.e = v; }
  get m42() { return this.f; } set m42(v) { this.f = v; }
  get is2D() { return true; }
  get isIdentity() { return this.a === 1 && this.b === 0 && this.c === 0 && this.d === 1 && this.e === 0 && this.f === 0; }
  multiply(m) {
    m = m instanceof DOMMatrix ? m : new DOMMatrix([m.a ?? 1, m.b ?? 0, m.c ?? 0, m.d ?? 1, m.e ?? 0, m.f ?? 0]);
    return new DOMMatrix([this.a * m.a + this.c * m.b, this.b * m.a + this.d * m.b, this.a * m.c + this.c * m.d,
      this.b * m.c + this.d * m.d, this.a * m.e + this.c * m.f + this.e, this.b * m.e + this.d * m.f + this.f]);
  }
  translate(x, y) { return this.multiply(new DOMMatrix([1, 0, 0, 1, x || 0, y || 0])); }
  scale(sx, sy) { if (sy === undefined) sy = sx; return this.multiply(new DOMMatrix([sx, 0, 0, sy, 0, 0])); }
  rotate(deg) { const r = (deg || 0) * Math.PI / 180, c = Math.cos(r), s = Math.sin(r); return this.multiply(new DOMMatrix([c, s, -s, c, 0, 0])); }
  inverse() {
    const det = this.a * this.d - this.b * this.c;
    if (!det) return new DOMMatrix([NaN, NaN, NaN, NaN, NaN, NaN]);
    return new DOMMatrix([this.d / det, -this.b / det, -this.c / det, this.a / det,
      (this.c * this.f - this.d * this.e) / det, (this.b * this.e - this.a * this.f) / det]);
  }
  transformPoint(p) { p = p || {}; const x = p.x || 0, y = p.y || 0; return { x: this.a * x + this.c * y + this.e, y: this.b * x + this.d * y + this.f, z: 0, w: 1 }; }
  toString() { return 'matrix(' + [this.a, this.b, this.c, this.d, this.e, this.f].join(', ') + ')'; }
  static fromMatrix(m) { return new DOMMatrix([m.a ?? 1, m.b ?? 0, m.c ?? 0, m.d ?? 1, m.e ?? 0, m.f ?? 0]); }
}

class ImageData {
  constructor(a, b, c) {
    if (a instanceof Uint8ClampedArray) {
      this.data = a; this.width = b >>> 0; this.height = c === undefined ? (a.length / 4 / this.width) | 0 : c >>> 0;
    } else {
      this.width = a >>> 0; this.height = b >>> 0;
      if (!this.width || !this.height) throw new DOMException('IndexSizeError', 'IndexSizeError');
      this.data = new Uint8ClampedArray(this.width * this.height * 4);
    }
    this.colorSpace = 'srgb';
  }
}

class CanvasGradient {
  constructor(kind, args) { this._k = kind; this._a = args; this._s = []; }
  addColorStop(o, c) {
    o = +o;
    if (!(o >= 0 && o <= 1)) throw new DOMException('IndexSizeError', 'IndexSizeError');
    this._s.push(o, String(c));
  }
}

class CanvasPattern {
  constructor(src, rep) { this._src = src; this._rep = rep; this._m = [1, 0, 0, 1, 0, 0]; }
  setTransform(m) { if (m) this._m = [m.a ?? 1, m.b ?? 0, m.c ?? 0, m.d ?? 1, m.e ?? 0, m.f ?? 0]; }
}

class Path2D {
  constructor(p) {
    this._ops = [];
    if (p instanceof Path2D) this._ops = p._ops.slice();
    else if (typeof p === 'string') this._ops.push(['svg', p]);
  }
  addPath(p) { if (p instanceof Path2D) this._ops.push(...p._ops); }
  moveTo(x, y) { this._ops.push([0, x, y]); }
  lineTo(x, y) { this._ops.push([1, x, y]); }
  quadraticCurveTo(a, b, c, d) { this._ops.push([2, a, b, c, d]); }
  bezierCurveTo(a, b, c, d, e, f) { this._ops.push([3, a, b, c, d, e, f]); }
  arc(x, y, r, a0, a1, ccw) { if (r < 0) throw new DOMException('IndexSizeError', 'IndexSizeError'); this._ops.push([4, x, y, r, a0, a1, ccw ? 1 : 0]); }
  arcTo(a, b, c, d, r) { this._ops.push([5, a, b, c, d, r]); }
  ellipse(x, y, rx, ry, rot, a0, a1, ccw) { this._ops.push([6, x, y, rx, ry, rot, a0, a1, ccw ? 1 : 0]); }
  rect(x, y, w, h) { this._ops.push([7, x, y, w, h]); }
  roundRect(x, y, w, h, r) { this._ops.push([8, x, y, w, h, ...radiiOf(r)]); }
  closePath() { this._ops.push(['close']); }
  _replay(id) {
    for (const o of this._ops) {
      if (o[0] === 'svg') K.cvSvgPath(id, o[1]);
      else if (o[0] === 'close') K.cvState(id, 4);
      else K.cvPath(id, ...o);
    }
  }
}

function radiiOf(r) {
  if (r === undefined) r = 0;
  const list = Array.isArray(r) ? r : [r];
  const v = list.map(x => typeof x === 'object' && x ? (x.x || 0) : +x || 0);
  if (v.length === 1) return [v[0], v[0], v[0], v[0]];
  if (v.length === 2) return [v[0], v[1], v[0], v[1]];
  if (v.length === 3) return [v[0], v[1], v[2], v[1]];
  return v.slice(0, 4);
}

function canvasDefaults() {
  return { fillStyle: '#000000', strokeStyle: '#000000', lineWidth: 1, lineCap: 'butt', lineJoin: 'miter',
    miterLimit: 10, globalAlpha: 1, globalCompositeOperation: 'source-over', font: '10px sans-serif',
    textAlign: 'start', textBaseline: 'alphabetic', direction: 'ltr', imageSmoothingEnabled: true,
    imageSmoothingQuality: 'low', shadowColor: 'rgba(0, 0, 0, 0)', shadowBlur: 0, shadowOffsetX: 0,
    shadowOffsetY: 0, lineDash: [], lineDashOffset: 0, filter: 'none', letterSpacing: '0px',
    fontKerning: 'auto', wordSpacing: '0px' };
}

function normColor(s) {
  const v = String(s).trim().toLowerCase();
  if (/^#[0-9a-f]{3}$/.test(v)) return '#' + v[1] + v[1] + v[2] + v[2] + v[3] + v[3];
  return v;
}

class CanvasRenderingContext2D {
  constructor(canvas) {
    Object.defineProperty(this, 'canvas', { value: canvas });
    this._id = K.cvCreate(canvas._h);
    this._s = canvasDefaults();
    this._stack = [];
  }
  _paint(which) {
    const v = which ? this._s.strokeStyle : this._s.fillStyle;
    if (v instanceof CanvasGradient) K.cvGradient(this._id, which, v._k, ...v._a, v._s);
    else if (v instanceof CanvasPattern) K.cvPattern(this._id, which, v._src._h, v._rep, ...v._m);
  }
  _style(which, v) {
    const key = which ? 'strokeStyle' : 'fillStyle';
    if (v instanceof CanvasGradient || v instanceof CanvasPattern) { this._s[key] = v; return; }
    if (typeof v !== 'string' && !(v instanceof String)) return;
    if (K.cvColor(this._id, which, String(v))) this._s[key] = normColor(v);
  }
  get fillStyle() { return this._s.fillStyle; }
  set fillStyle(v) { this._style(0, v); }
  get strokeStyle() { return this._s.strokeStyle; }
  set strokeStyle(v) { this._style(1, v); }
  get lineWidth() { return this._s.lineWidth; }
  set lineWidth(v) { v = +v; if (v > 0 && isFinite(v)) { this._s.lineWidth = v; K.cvProp(this._id, 0, v); } }
  get lineCap() { return this._s.lineCap; }
  set lineCap(v) { if (v in CAPS) { this._s.lineCap = v; K.cvProp(this._id, 1, CAPS[v]); } }
  get lineJoin() { return this._s.lineJoin; }
  set lineJoin(v) { if (v in JOINS) { this._s.lineJoin = v; K.cvProp(this._id, 2, JOINS[v]); } }
  get miterLimit() { return this._s.miterLimit; }
  set miterLimit(v) { v = +v; if (v > 0 && isFinite(v)) { this._s.miterLimit = v; K.cvProp(this._id, 3, v); } }
  get globalAlpha() { return this._s.globalAlpha; }
  set globalAlpha(v) { v = +v; if (v >= 0 && v <= 1) { this._s.globalAlpha = v; K.cvProp(this._id, 4, v); } }
  get globalCompositeOperation() { return this._s.globalCompositeOperation; }
  set globalCompositeOperation(v) {
    // Blend modes (multiply, screen ...) are drawn like source-over.
    const op = OPS[v] !== undefined ? OPS[v] : (/^(multiply|screen|overlay|darken|lighten|color-dodge|color-burn|hard-light|soft-light|difference|exclusion|hue|saturation|color|luminosity)$/.test(v) ? 0 : -1);
    if (op >= 0) { this._s.globalCompositeOperation = v; K.cvProp(this._id, 5, op); }
  }
  get font() { return this._s.font; }
  set font(v) { if (K.cvFont(this._id, String(v))) this._s.font = String(v); }
  get textAlign() { return this._s.textAlign; }
  set textAlign(v) { if (v in ALIGNS) { this._s.textAlign = v; K.cvProp(this._id, 6, ALIGNS[v]); } }
  get textBaseline() { return this._s.textBaseline; }
  set textBaseline(v) { if (v in BASELINES) { this._s.textBaseline = v; K.cvProp(this._id, 7, BASELINES[v]); } }
  get direction() { return this._s.direction; }
  set direction(v) { this._s.direction = v; }
  get letterSpacing() { return this._s.letterSpacing; }
  set letterSpacing(v) { this._s.letterSpacing = v; }
  get fontKerning() { return this._s.fontKerning; }
  set fontKerning(v) { this._s.fontKerning = v; }
  get wordSpacing() { return this._s.wordSpacing; }
  set wordSpacing(v) { this._s.wordSpacing = v; }
  get filter() { return this._s.filter; }
  set filter(v) { this._s.filter = v; }
  get imageSmoothingEnabled() { return this._s.imageSmoothingEnabled; }
  set imageSmoothingEnabled(v) { this._s.imageSmoothingEnabled = !!v; K.cvProp(this._id, 8, !!v); }
  get imageSmoothingQuality() { return this._s.imageSmoothingQuality; }
  set imageSmoothingQuality(v) { this._s.imageSmoothingQuality = v; }
  _shadow() { K.cvShadow(this._id, this._s.shadowColor, this._s.shadowBlur, this._s.shadowOffsetX, this._s.shadowOffsetY); }
  get shadowColor() { return this._s.shadowColor; }
  set shadowColor(v) { this._s.shadowColor = String(v); this._shadow(); }
  get shadowBlur() { return this._s.shadowBlur; }
  set shadowBlur(v) { v = +v; if (v >= 0 && isFinite(v)) { this._s.shadowBlur = v; this._shadow(); } }
  get shadowOffsetX() { return this._s.shadowOffsetX; }
  set shadowOffsetX(v) { v = +v; if (isFinite(v)) { this._s.shadowOffsetX = v; this._shadow(); } }
  get shadowOffsetY() { return this._s.shadowOffsetY; }
  set shadowOffsetY(v) { v = +v; if (isFinite(v)) { this._s.shadowOffsetY = v; this._shadow(); } }
  get lineDashOffset() { return this._s.lineDashOffset; }
  set lineDashOffset(v) { v = +v; if (isFinite(v)) { this._s.lineDashOffset = v; K.cvProp(this._id, 9, v); } }
  setLineDash(a) {
    if (!a || typeof a.length !== 'number') return;
    const v = Array.from(a, Number);
    if (v.some(x => !(x >= 0) || !isFinite(x))) return;
    this._s.lineDash = v.length % 2 ? v.concat(v) : v;
    K.cvDash(this._id, v);
  }
  getLineDash() { return this._s.lineDash.slice(); }

  save() { this._stack.push(Object.assign({}, this._s)); K.cvState(this._id, 0); }
  restore() { if (!this._stack.length) return; this._s = this._stack.pop(); K.cvState(this._id, 1); }
  reset() { this._s = canvasDefaults(); this._stack = []; K.cvState(this._id, 2); }
  isContextLost() { return false; }
  getContextAttributes() { return { alpha: true, desynchronized: false, colorSpace: 'srgb', willReadFrequently: false }; }

  scale(x, y) { K.cvMatrix(this._id, false, +x, 0, 0, +y, 0, 0); }
  rotate(a) { const c = Math.cos(a), s = Math.sin(a); K.cvMatrix(this._id, false, c, s, -s, c, 0, 0); }
  translate(x, y) { K.cvMatrix(this._id, false, 1, 0, 0, 1, +x, +y); }
  transform(a, b, c, d, e, f) { K.cvMatrix(this._id, false, +a, +b, +c, +d, +e, +f); }
  setTransform(a, b, c, d, e, f) {
    if (a === undefined) { this.resetTransform(); return; }
    if (typeof a === 'object') { const m = a; K.cvMatrix(this._id, true, m.a ?? m.m11 ?? 1, m.b ?? m.m12 ?? 0, m.c ?? m.m21 ?? 0, m.d ?? m.m22 ?? 1, m.e ?? m.m41 ?? 0, m.f ?? m.m42 ?? 0); return; }
    K.cvMatrix(this._id, true, +a, +b, +c, +d, +e, +f);
  }
  resetTransform() { K.cvMatrix(this._id, true, 1, 0, 0, 1, 0, 0); }
  getTransform() { return new DOMMatrix(K.cvGetMatrix(this._id)); }

  createLinearGradient(x0, y0, x1, y1) { return new CanvasGradient(0, [+x0, +y0, 0, +x1, +y1, 0]); }
  createRadialGradient(x0, y0, r0, x1, y1, r1) {
    if (r0 < 0 || r1 < 0) throw new DOMException('IndexSizeError', 'IndexSizeError');
    return new CanvasGradient(1, [+x0, +y0, +r0, +x1, +y1, +r1]);
  }
  createConicGradient(a, x, y) { return new CanvasGradient(2, [+x, +y, +a, 0, 0, 0]); }
  createPattern(img, rep) {
    if (!img || !img._h) return null;
    if (!K.cvSourceSize(img._h)) return null;
    return new CanvasPattern(img, rep === null || rep === undefined ? 'repeat' : String(rep));
  }

  beginPath() { K.cvState(this._id, 3); }
  closePath() { K.cvState(this._id, 4); }
  moveTo(x, y) { K.cvPath(this._id, 0, x, y); }
  lineTo(x, y) { K.cvPath(this._id, 1, x, y); }
  quadraticCurveTo(a, b, c, d) { K.cvPath(this._id, 2, a, b, c, d); }
  bezierCurveTo(a, b, c, d, e, f) { K.cvPath(this._id, 3, a, b, c, d, e, f); }
  arc(x, y, r, a0, a1, ccw) {
    if (r < 0) throw new DOMException('IndexSizeError', 'IndexSizeError');
    K.cvPath(this._id, 4, x, y, r, a0, a1, ccw ? 1 : 0);
  }
  arcTo(a, b, c, d, r) {
    if (r < 0) throw new DOMException('IndexSizeError', 'IndexSizeError');
    K.cvPath(this._id, 5, a, b, c, d, r);
  }
  ellipse(x, y, rx, ry, rot, a0, a1, ccw) {
    if (rx < 0 || ry < 0) throw new DOMException('IndexSizeError', 'IndexSizeError');
    K.cvPath(this._id, 6, x, y, rx, ry, rot, a0, a1, ccw ? 1 : 0);
  }
  rect(x, y, w, h) { K.cvPath(this._id, 7, x, y, w, h); }
  roundRect(x, y, w, h, r) { K.cvPath(this._id, 8, x, y, w, h, ...radiiOf(r)); }
  _withPath(path, fn) {
    if (path instanceof Path2D) { K.cvState(this._id, 5); path._replay(this._id); const r = fn(); K.cvState(this._id, 6); return r; }
    return fn();
  }
  fill(a, b) {
    const path = a instanceof Path2D ? a : null, rule = path ? b : a;
    this._paint(0);
    this._withPath(path, () => K.cvDraw(this._id, 0, rule === 'evenodd' ? 1 : 0));
  }
  stroke(path) { this._paint(1); this._withPath(path instanceof Path2D ? path : null, () => K.cvDraw(this._id, 1)); }
  clip(a, b) {
    const path = a instanceof Path2D ? a : null, rule = path ? b : a;
    this._withPath(path, () => K.cvDraw(this._id, 2, rule === 'evenodd' ? 1 : 0));
  }
  isPointInPath(a, b, c, d) {
    if (a instanceof Path2D) return this._withPath(a, () => K.cvHit(this._id, 0, b, c, d === 'evenodd' ? 1 : 0));
    return K.cvHit(this._id, 0, a, b, c === 'evenodd' ? 1 : 0);
  }
  isPointInStroke(a, b, c) {
    if (a instanceof Path2D) return this._withPath(a, () => K.cvHit(this._id, 1, b, c, 0));
    return K.cvHit(this._id, 1, a, b, 0);
  }
  fillRect(x, y, w, h) { this._paint(0); K.cvDraw(this._id, 3, x, y, w, h); }
  strokeRect(x, y, w, h) { this._paint(1); K.cvDraw(this._id, 4, x, y, w, h); }
  clearRect(x, y, w, h) { K.cvDraw(this._id, 5, x, y, w, h); }
  fillText(t, x, y, mw) { this._paint(0); K.cvText(this._id, String(t), +x, +y, mw === undefined ? 0 : +mw, false); }
  strokeText(t, x, y, mw) { this._paint(1); K.cvText(this._id, String(t), +x, +y, mw === undefined ? 0 : +mw, true); }
  measureText(t) {
    const m = K.cvMeasure(this._id, String(t));
    const left = this._s.textAlign === 'center' ? m[0] / 2 : (this._s.textAlign === 'right' || this._s.textAlign === 'end') ? m[0] : 0;
    return { width: m[0], actualBoundingBoxLeft: left, actualBoundingBoxRight: m[0] - left,
      actualBoundingBoxAscent: m[1], actualBoundingBoxDescent: m[2], fontBoundingBoxAscent: m[1],
      fontBoundingBoxDescent: m[2], emHeightAscent: m[1], emHeightDescent: m[2], hangingBaseline: m[1] * 0.8,
      alphabeticBaseline: 0, ideographicBaseline: -m[2] };
  }
  drawImage(img, a, b, c, d, e, f, g, h) {
    if (!img) throw new TypeError('drawImage: no image');
    if (img instanceof ImageData) return;
    if (!img._h) return;
    const size = K.cvSourceSize(img._h);
    if (!size || !size[0] || !size[1]) return;
    let sx = 0, sy = 0, sw = size[0], sh = size[1], dx, dy, dw, dh;
    if (c === undefined) { dx = a; dy = b; dw = sw; dh = sh; }
    else if (e === undefined) { dx = a; dy = b; dw = c; dh = d; }
    else { sx = a; sy = b; sw = c; sh = d; dx = e; dy = f; dw = g; dh = h; }
    K.cvImage(this._id, img._h, +sx, +sy, +sw, +sh, +dx, +dy, +dw, +dh);
  }
  createImageData(w, h) {
    if (w instanceof ImageData) return new ImageData(w.width, w.height);
    return new ImageData(Math.abs(w | 0), Math.abs(h | 0));
  }
  getImageData(x, y, w, h) {
    x |= 0; y |= 0; w |= 0; h |= 0;
    if (!w || !h) throw new DOMException('IndexSizeError', 'IndexSizeError');
    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }
    return new ImageData(new Uint8ClampedArray(K.cvGetData(this._id, x, y, w, h)), w, h);
  }
  putImageData(img, dx, dy, x, y, w, h) {
    if (!img || !img.data) return;
    if (x === undefined) { x = 0; y = 0; w = img.width; h = img.height; }
    K.cvPutData(this._id, img.data, img.width, img.height, dx | 0, dy | 0, x | 0, y | 0, w | 0, h | 0);
  }
  drawFocusIfNeeded() {}
  scrollPathIntoView() {}
}

class HTMLCanvasElement extends HTMLElement {
  get width() { const v = parseInt(this.getAttribute('width'), 10); return isNaN(v) || v < 0 ? 300 : v; }
  set width(v) { this.setAttribute('width', String(Math.max(0, v | 0))); }
  get height() { const v = parseInt(this.getAttribute('height'), 10); return isNaN(v) || v < 0 ? 150 : v; }
  set height(v) { this.setAttribute('height', String(Math.max(0, v | 0))); }
  setAttribute(n, v) {
    super.setAttribute(n, v);
    n = String(n).toLowerCase();
    if ((n === 'width' || n === 'height') && this._ctx) {
      K.cvResize(this._ctx._id, this.width, this.height);
      this._ctx._s = canvasDefaults();
      this._ctx._stack = [];
    }
  }
  getContext(type) {
    if (type !== '2d') return null;
    if (!this._ctx) Object.defineProperty(this, '_ctx', { value: new CanvasRenderingContext2D(this), configurable: true });
    return this._ctx;
  }
  toDataURL() { return K.cvDataUrl(this.getContext('2d')._id); }
  toBlob(cb, type) {
    const url = this.toDataURL(type);
    setTimeout(() => cb(new Blob([atob(url.slice(url.indexOf(',') + 1))], { type: 'image/png' })), 0);
  }
  captureStream() { return null; }
  transferControlToOffscreen() { return this; }
}
TAG_CLASSES.canvas = HTMLCanvasElement;
G.HTMLCanvasElement = HTMLCanvasElement;

function OffscreenCanvas(w, h) {
  const c = G.document.createElement('canvas');
  c.width = w; c.height = h;
  c.convertToBlob = () => new Promise(res => c.toBlob(res));
  c.transferToImageBitmap = () => c;
  return c;
}

function createImageBitmap(src) {
  if (!src || !src._h) return Promise.reject(new TypeError('createImageBitmap: unsupported source'));
  return new Promise((res, rej) => {
    const done = () => { if (K.cvSourceSize(src._h)) { src.close = () => {}; res(src); } else rej(new DOMException('InvalidStateError', 'InvalidStateError')); };
    if (src.localName === 'img' && !K.cvSourceSize(src._h)) { src.addEventListener('load', done, { once: true }); src.addEventListener('error', done, { once: true }); }
    else done();
  });
}

// ------------------------------------------------------------------ Document
class Document extends Node {
  get documentElement() { return W(K.query(this._h, 'html', false)); }
  get head() { return W(K.query(this._h, 'head', false)); }
  get body() { return W(K.query(this._h, 'body', false)); }
  set body(b) {}
  get title() { const t = K.query(this._h, 'title', false); return t ? K.text(t).replace(/\s+/g, ' ').trim() : ''; }
  set title(v) {
    let t = this.querySelector('title');
    if (!t) { t = this.createElement('title'); if (this.head) this.head.appendChild(t); }
    t.textContent = v; K.setTitle(String(v));
  }
  getElementById(id) { if (!id) return null; return W(K.query(this._h, '[id="' + String(id).replace(/["\\]/g, '\\$&') + '"]', false)); }
  getElementsByName(n) { return nodeList(K.query(this._h, '[name="' + String(n).replace(/["\\]/g, '\\$&') + '"]', true).map(W)); }
  createElement(tag) { return W(K.create(1, String(tag).toLowerCase())); }
  createElementNS(ns, tag) { return W(K.create(1, String(tag).replace(/^.*:/, '').toLowerCase())); }
  createTextNode(s) { return W(K.create(3, String(s))); }
  createComment(s) { return W(K.create(8, String(s))); }
  createDocumentFragment() { return W(K.create(11, '')); }
  createEvent(type) { const t = String(type).toLowerCase(); if (t.indexOf('mouse') >= 0) return new MouseEvent(''); if (t.indexOf('custom') >= 0) return new CustomEvent(''); return new Event(''); }
  createRange() { return { setStart() {}, setEnd() {}, selectNodeContents() {}, collapse() {}, getBoundingClientRect() { return domRect(0, 0, 0, 0); }, createContextualFragment: html => { const f = this.createDocumentFragment(); for (const h of K.parse(html, '')) K.insert(f._h, h, 0); return f; } }; }
  createTreeWalker(root) { const all = [root, ...root.querySelectorAll('*')]; let i = 0; return { currentNode: root, nextNode() { i++; return this.currentNode = all[i] || null; } }; }
  importNode(n, deep) { return n.cloneNode(deep); }
  adoptNode(n) { return n; }
  get cookie() { return K.cookie(); }
  set cookie(v) { K.cookie(String(v)); }
  get readyState() { return readyState; }
  get URL() { return K.url(); }
  get documentURI() { return K.url(); }
  get baseURI() { return K.url(); }
  get location() { return G.location; }
  set location(v) { G.location.href = v; }
  get referrer() { return ''; }
  get domain() { return G.location.hostname; }
  get characterSet() { return 'UTF-8'; }
  get charset() { return 'UTF-8'; }
  get contentType() { return 'text/html'; }
  get compatMode() { return 'CSS1Compat'; }
  get defaultView() { return G; }
  get activeElement() { return this._active || this.body; }
  get hidden() { return false; }
  get visibilityState() { return 'visible'; }
  get fullscreenElement() { return null; }
  get currentScript() { return W(K.cur()); }
  get forms() { return htmlCollection(this.querySelectorAll('form').slice()); }
  get images() { return htmlCollection(this.querySelectorAll('img').slice()); }
  get links() { return htmlCollection(this.querySelectorAll('a[href],area[href]').slice()); }
  get scripts() { return htmlCollection(this.querySelectorAll('script').slice()); }
  get styleSheets() { return []; }
  get fonts() { return { ready: Promise.resolve(), load: () => Promise.resolve([]), check: () => true, add() {}, addEventListener() {} }; }
  get implementation() { return { hasFeature: () => true, createHTMLDocument: () => G.document }; }
  hasFocus() { return true; }
  elementFromPoint() { return null; }
  getSelection() { return G.getSelection(); }
  execCommand() { return false; }
  open() { return this; }
  close() {}
  write(...parts) {
    const html = parts.join('');
    const s = this.currentScript;
    const hs = K.parse(html, '');
    if (s && s.parentNode) { const next = s.nextSibling; for (const h of hs) K.insert(s.parentNode._h, h, H(next)); }
    else if (this.body) for (const h of hs) K.insert(this.body._h, h, 0);
  }
  writeln(...parts) { this.write(...parts, '\n'); }
}
mixin(Document, ChildMixin);
class HTMLDocument extends Document {}

let readyState = 'loading';
const doc = Object.create(HTMLDocument.prototype);
Object.defineProperty(doc, '_h', { value: K.doc() });
wrappers.set(K.doc(), doc);

// ------------------------------------------------------------------ URL
class URLSearchParams {
  constructor(init) {
    this._p = [];
    if (init === undefined || init === null) return;
    if (typeof init === 'object' && !(init instanceof String)) {
      if (Array.isArray(init) || typeof init[Symbol.iterator] === 'function') for (const [k, v] of init) this._p.push([String(k), String(v)]);
      else for (const k of Object.keys(init)) this._p.push([k, String(init[k])]);
      return;
    }
    let s = String(init);
    if (s[0] === '?') s = s.slice(1);
    for (const part of s.split('&')) {
      if (!part) continue;
      const i = part.indexOf('=');
      const dec = x => { try { return decodeURIComponent(x.replace(/\+/g, ' ')); } catch (e) { return x; } };
      this._p.push(i < 0 ? [dec(part), ''] : [dec(part.slice(0, i)), dec(part.slice(i + 1))]);
    }
  }
  get size() { return this._p.length; }
  append(k, v) { this._p.push([String(k), String(v)]); this._u && this._u._sync(); }
  delete(k) { this._p = this._p.filter(p => p[0] !== k); this._u && this._u._sync(); }
  get(k) { const p = this._p.find(p => p[0] === k); return p ? p[1] : null; }
  getAll(k) { return this._p.filter(p => p[0] === k).map(p => p[1]); }
  has(k) { return this._p.some(p => p[0] === k); }
  set(k, v) { const i = this._p.findIndex(p => p[0] === k); if (i < 0) this._p.push([String(k), String(v)]); else { this._p[i][1] = String(v); this._p = this._p.filter((p, j) => p[0] !== k || j === i); } this._u && this._u._sync(); }
  sort() { this._p.sort((a, b) => a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0); }
  forEach(fn, self) { for (const [k, v] of this._p) fn.call(self, v, k, this); }
  keys() { return this._p.map(p => p[0])[Symbol.iterator](); }
  values() { return this._p.map(p => p[1])[Symbol.iterator](); }
  entries() { return this._p.map(p => [p[0], p[1]])[Symbol.iterator](); }
  [Symbol.iterator]() { return this.entries(); }
  toString() {
    const enc = s => encodeURIComponent(s).replace(/%20/g, '+').replace(/[!'()~]/g, c => '%' + c.charCodeAt(0).toString(16).toUpperCase());
    return this._p.map(([k, v]) => enc(k) + '=' + enc(v)).join('&');
  }
}

const URL_RE = /^([a-zA-Z][a-zA-Z0-9+.-]*:)(?:\/\/(?:([^:@\/?#]*)(?::([^@\/?#]*))?@)?(\[[^\]]*\]|[^:\/?#]*)(?::(\d*))?)?([^?#]*)(\?[^#]*)?(#.*)?$/;
class URL {
  constructor(url, base) {
    let s = String(url);
    if (base !== undefined && !/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(s)) {
      const b = new URL(String(base));
      const pre = b._prefix();
      if (s.startsWith('//')) s = b.protocol + s;
      else if (s.startsWith('/')) s = pre + s;
      else if (s.startsWith('?')) s = pre + b.pathname + s;
      else if (s.startsWith('#')) s = pre + b.pathname + b.search + s;
      else if (s === '') s = pre + b.pathname + b.search;
      else s = pre + b.pathname.replace(/[^\/]*$/, '') + s;
    }
    const m = URL_RE.exec(s.trim());
    if (!m) throw new TypeError('Invalid URL: ' + s);
    this.protocol = m[1].toLowerCase();
    this.username = m[2] || '';
    this.password = m[3] || '';
    this.hostname = (m[4] || '').toLowerCase();
    this.port = m[5] || '';
    if ((this.protocol === 'http:' && this.port === '80') || (this.protocol === 'https:' && this.port === '443')) this.port = '';
    let path = m[6] || '';
    if (this.hostname || this.protocol === 'http:' || this.protocol === 'https:' || this.protocol === 'file:') {
      if (!path.startsWith('/')) path = '/' + path;
      const out = [];
      for (const seg of path.split('/').slice(1)) { if (seg === '..') out.pop(); else if (seg !== '.') out.push(seg); }
      path = '/' + out.join('/');
    }
    this.pathname = path;
    this._search = m[7] && m[7] !== '?' ? m[7] : '';
    this.hash = m[8] && m[8] !== '#' ? m[8] : '';
    this.searchParams = new URLSearchParams(this._search);
    this.searchParams._u = this;
  }
  _sync() { const q = this.searchParams.toString(); this._search = q ? '?' + q : ''; }
  get search() { return this._search; }
  set search(v) { v = String(v); this._search = v && v[0] !== '?' ? '?' + v : v; const sp = new URLSearchParams(this._search); this.searchParams._p = sp._p; }
  get host() { return this.hostname + (this.port ? ':' + this.port : ''); }
  set host(v) { const i = v.indexOf(':'); this.hostname = i < 0 ? v : v.slice(0, i); this.port = i < 0 ? '' : v.slice(i + 1); }
  get origin() { return this.hostname ? this.protocol + '//' + this.host : 'null'; }
  _prefix() {
    const auth = this.username ? this.username + (this.password ? ':' + this.password : '') + '@' : '';
    const slashes = this.hostname || this.protocol === 'file:' ? '//' : '';
    return this.protocol + slashes + auth + this.host;
  }
  get href() { return this._prefix() + this.pathname + this._search + this.hash; }
  set href(v) { Object.assign(this, new URL(v)); }
  toString() { return this.href; }
  toJSON() { return this.href; }
  static createObjectURL() { return 'blob:kite'; }
  static revokeObjectURL() {}
  static canParse(u, b) { try { new URL(u, b); return true; } catch (e) { return false; } }
}

// ------------------------------------------------------------------ Window
const winET = new EventTarget();
G.addEventListener = (t, f, o) => winET.addEventListener.call(G, t, f, o);
G.removeEventListener = (t, f, o) => winET.removeEventListener.call(G, t, f, o);
G.dispatchEvent = ev => EventTarget.prototype.dispatchEvent.call(G, ev);
for (const t of EVENT_PROPS) {
  Object.defineProperty(G, 'on' + t, {
    get() { return G._on ? G._on[t] || null : null; },
    set(fn) { if (!G._on) G._on = Object.create(null); G._on[t] = typeof fn === 'function' ? fn : null; },
    configurable: true,
  });
}

const location = {
  get href() { return K.url(); },
  set href(v) { K.navigate(String(v), false); },
  get protocol() { return new URL(K.url()).protocol; },
  get host() { return new URL(K.url()).host; },
  get hostname() { return new URL(K.url()).hostname; },
  get port() { return new URL(K.url()).port; },
  get pathname() { return new URL(K.url()).pathname; },
  set pathname(v) { const u = new URL(K.url()); u.pathname = v; K.navigate(u.href, false); },
  get search() { return new URL(K.url()).search; },
  set search(v) { const u = new URL(K.url()); u.search = v; K.navigate(u.href, false); },
  get hash() { return new URL(K.url()).hash; },
  set hash(v) { const u = new URL(K.url()); u.hash = v; K.navigate(u.href, false); },
  get origin() { return new URL(K.url()).origin; },
  assign(u) { K.navigate(String(u), false); },
  replace(u) { K.navigate(String(u), true); },
  reload() { K.navigate(K.url(), true); },
  toString() { return K.url(); },
  get ancestorOrigins() { return []; },
};

// Session history: states live here, the platform keeps the entries.
const histStates = new Map();
let histSeq = 0, histCur = 0;
function histNav(state, url, replace) {
  const target = url === undefined || url === null ? K.url() : new URL(String(url), K.url()).href;
  let copy = null;
  try { copy = state === undefined ? null : structuredClone(state); } catch (e) { copy = state; }
  const id = ++histSeq;
  if (!K.pushState(target, !!replace, id))
    throw new DOMException("Failed to execute '" + (replace ? 'replaceState' : 'pushState') + "' on 'History': a history state object with URL '" + target + "' cannot be created in a document with origin '" + location.origin + "'.", 'SecurityError');
  histStates.set(id, copy);
  histCur = id;
}
const history = {
  get length() { return K.histLen(); },
  get state() { return histStates.has(histCur) ? histStates.get(histCur) : null; },
  scrollRestoration: 'auto',
  pushState(state, title, url) { histNav(state, url, false); },
  replaceState(state, title, url) { histNav(state, url, true); },
  back() { K.histGo(-1); },
  forward() { K.histGo(1); },
  go(n) { K.histGo(n | 0); },
};
class PopStateEvent extends Event { constructor(t, i) { super(t, i); this.state = i && 'state' in i ? i.state : null; } }
class HashChangeEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.oldURL = i.oldURL || ''; this.newURL = i.newURL || ''; } }
G.__kitePopState = function (id, hashChanged, oldURL) {
  histCur = id;
  G.dispatchEvent(new PopStateEvent('popstate', { state: history.state }));
  if (hashChanged) G.dispatchEvent(new HashChangeEvent('hashchange', { oldURL, newURL: K.url() }));
};

class Storage {
  constructor(session) {
    Object.defineProperty(this, '_s', { value: !!session, configurable: true });
    // Items are also reachable as properties (localStorage.foo).
    return new Proxy(this, {
      get(t, k, r) { if (typeof k !== 'string' || k in t) return Reflect.get(t, k, r); return t.getItem(k); },
      set(t, k, v) { if (typeof k !== 'string' || k in t) return Reflect.set(t, k, v); t.setItem(k, v); return true; },
      has(t, k) { return k in t || (typeof k === 'string' && t.getItem(k) !== null); },
      deleteProperty(t, k) { if (typeof k === 'string' && !(k in t)) t.removeItem(k); return true; },
      ownKeys(t) { return K.storage(4, t._s); },
      getOwnPropertyDescriptor(t, k) {
        const v = typeof k === 'string' ? t.getItem(k) : null;
        return v === null ? undefined : { value: v, writable: true, enumerable: true, configurable: true };
      },
    });
  }
  get length() { return K.storage(4, this._s).length; }
  key(i) { const k = K.storage(4, this._s)[i | 0]; return k === undefined ? null : k; }
  getItem(k) { return K.storage(0, this._s, String(k)); }
  setItem(k, v) {
    if (!K.storage(1, this._s, String(k), String(v)))
      throw new DOMException("Failed to execute 'setItem' on 'Storage': exceeded the quota.", 'QuotaExceededError');
  }
  removeItem(k) { K.storage(2, this._s, String(k)); }
  clear() { K.storage(3, this._s); }
}

const consoleObj = {};
for (const m of ['log', 'info', 'warn', 'error', 'debug', 'trace', 'dir', 'table']) {
  consoleObj[m] = (...a) => K.log((m === 'log' ? '' : '[' + m + '] ') + a.map(x => {
    if (typeof x === 'string') return x;
    try { return x instanceof Error ? String(x) : JSON.stringify(x); } catch (e) { return String(x); }
  }).join(' '));
}
for (const m of ['group', 'groupCollapsed', 'groupEnd', 'time', 'timeEnd', 'timeLog', 'count', 'countReset', 'profile', 'profileEnd', 'clear']) consoleObj[m] = () => {};
consoleObj.assert = (c, ...a) => { if (!c) consoleObj.error('Assertion failed:', ...a); };

function makeTimer(repeat) {
  return function (fn, ms, ...args) {
    if (typeof fn === 'string') { const code = fn; fn = () => (0, eval)(code); }
    if (typeof fn !== 'function') return 0;
    return K.timer(args.length ? () => fn(...args) : fn, +ms || 0, repeat);
  };
}

const startTime = Date.now();
const performance = {
  now: () => Date.now() - startTime,
  timeOrigin: startTime,
  timing: { navigationStart: startTime, domContentLoadedEventEnd: 0, loadEventEnd: 0, responseStart: startTime },
  navigation: { type: 0 },
  mark() {}, measure() {}, clearMarks() {}, clearMeasures() {},
  getEntriesByType() { return []; }, getEntriesByName() { return []; }, getEntries() { return []; },
  memory: { usedJSHeapSize: 0, totalJSHeapSize: 0 },
};

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
function btoa(s) {
  s = String(s); let out = '';
  for (let i = 0; i < s.length; i += 3) {
    const a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2);
    if (a > 255 || b > 255 || c > 255) throw new DOMException('InvalidCharacterError', 'InvalidCharacterError');
    const n = (a << 16) | ((b || 0) << 8) | (c || 0);
    out += B64[n >> 18 & 63] + B64[n >> 12 & 63] + (isNaN(b) ? '=' : B64[n >> 6 & 63]) + (isNaN(c) ? '=' : B64[n & 63]);
  }
  return out;
}
function atob(s) {
  s = String(s).replace(/[\s=]/g, '').replace(/-/g, '+').replace(/_/g, '/'); let out = '', buf = 0, bits = 0;
  for (const ch of s) { const v = B64.indexOf(ch); if (v < 0) throw new DOMException('InvalidCharacterError', 'InvalidCharacterError'); buf = (buf << 6) | v; bits += 6; if (bits >= 8) { bits -= 8; out += String.fromCharCode((buf >> bits) & 255); } }
  return out;
}

class DOMException extends Error {
  constructor(message, name) { super(message); this.name = name || 'Error'; this.code = 0; }
}

class TextEncoder {
  get encoding() { return 'utf-8'; }
  encode(s) { const u = unescape(encodeURIComponent(String(s === undefined ? '' : s))); const a = new Uint8Array(u.length); for (let i = 0; i < u.length; i++) a[i] = u.charCodeAt(i); return a; }
}
class TextDecoder {
  constructor(enc) { this.encoding = enc || 'utf-8'; }
  decode(buf) {
    if (!buf) return '';
    const a = buf instanceof Uint8Array ? buf : new Uint8Array(buf.buffer || buf);
    let s = ''; for (let i = 0; i < a.length; i++) s += String.fromCharCode(a[i]);
    try { return decodeURIComponent(escape(s)); } catch (e) { return s; }
  }
}

class AbortSignal extends EventTarget {
  constructor() { super(); this.aborted = false; this.reason = undefined; this.onabort = null; }
  throwIfAborted() { if (this.aborted) throw this.reason; }
  static timeout(ms) { const c = new AbortController(); setTimeout(() => c.abort(new DOMException('TimeoutError', 'TimeoutError')), ms); return c.signal; }
  static abort(r) { const c = new AbortController(); c.abort(r); return c.signal; }
}
class AbortController {
  constructor() { this.signal = new AbortSignal(); }
  abort(reason) {
    const s = this.signal; if (s.aborted) return;
    s.aborted = true; s.reason = reason || new DOMException('AbortError', 'AbortError');
    const ev = new Event('abort'); if (typeof s.onabort === 'function') s.onabort(ev); s.dispatchEvent(ev);
  }
}

class Headers {
  constructor(init) {
    this._h = new Map();
    if (init instanceof Headers) init.forEach((v, k) => this.append(k, v));
    else if (Array.isArray(init)) for (const [k, v] of init) this.append(k, v);
    else if (init) for (const k of Object.keys(init)) this.append(k, init[k]);
  }
  append(k, v) { k = String(k).toLowerCase(); this._h.set(k, this._h.has(k) ? this._h.get(k) + ', ' + v : String(v)); }
  set(k, v) { this._h.set(String(k).toLowerCase(), String(v)); }
  get(k) { const v = this._h.get(String(k).toLowerCase()); return v === undefined ? null : v; }
  has(k) { return this._h.has(String(k).toLowerCase()); }
  delete(k) { this._h.delete(String(k).toLowerCase()); }
  forEach(fn, self) { for (const [k, v] of this._h) fn.call(self, v, k, this); }
  entries() { return this._h.entries(); }
  keys() { return this._h.keys(); }
  values() { return this._h.values(); }
  [Symbol.iterator]() { return this._h.entries(); }
}

class Blob {
  constructor(parts, opts) { this._s = (parts || []).map(p => typeof p === 'string' ? p : (p && p._s) || String(p)).join(''); this.type = opts && opts.type || ''; }
  get size() { return this._s.length; }
  text() { return Promise.resolve(this._s); }
  arrayBuffer() { return Promise.resolve(new TextEncoder().encode(this._s).buffer); }
  slice(a, b) { return new Blob([this._s.slice(a, b)], { type: this.type }); }
}
class File extends Blob { constructor(parts, name, opts) { super(parts, opts); this.name = name; this.lastModified = Date.now(); } }

class FormData {
  constructor(form) {
    this._p = [];
    if (form && form._h) {
      for (const el of form.elements) {
        const name = el.name; if (!name || el.disabled) continue;
        const type = el.type;
        if ((type === 'checkbox' || type === 'radio') && !el.checked) continue;
        if (type === 'submit' || type === 'button' || type === 'reset' || type === 'file' || type === 'image') continue;
        this._p.push([name, el.value]);
      }
    }
  }
  append(k, v) { this._p.push([String(k), v instanceof Blob ? v : String(v)]); }
  set(k, v) { this.delete(k); this.append(k, v); }
  get(k) { const p = this._p.find(p => p[0] === k); return p ? p[1] : null; }
  getAll(k) { return this._p.filter(p => p[0] === k).map(p => p[1]); }
  has(k) { return this._p.some(p => p[0] === k); }
  delete(k) { this._p = this._p.filter(p => p[0] !== k); }
  forEach(fn, self) { for (const [k, v] of this._p) fn.call(self, v, k, this); }
  entries() { return this._p[Symbol.iterator](); }
  [Symbol.iterator]() { return this._p[Symbol.iterator](); }
}

class Response {
  constructor(body, init) {
    init = init || {};
    this._body = body === undefined || body === null ? '' : typeof body === 'string' ? body : (body._s || String(body));
    this.status = init.status === undefined ? 200 : init.status;
    this.statusText = init.statusText || '';
    this.ok = this.status >= 200 && this.status < 300;
    this.headers = init.headers instanceof Headers ? init.headers : new Headers(init.headers);
    this.url = init.url || '';
    this.redirected = false;
    this.type = 'basic';
    this.bodyUsed = false;
  }
  text() { this.bodyUsed = true; return Promise.resolve(this._body); }
  json() { this.bodyUsed = true; return new Promise((res, rej) => { try { res(JSON.parse(this._body)); } catch (e) { rej(e); } }); }
  blob() { return Promise.resolve(new Blob([this._body], { type: this.headers.get('content-type') || '' })); }
  arrayBuffer() { return Promise.resolve(new TextEncoder().encode(this._body).buffer); }
  formData() { return Promise.resolve(new FormData()); }
  clone() { return new Response(this._body, { status: this.status, statusText: this.statusText, headers: this.headers, url: this.url }); }
  static json(d, i) { return new Response(JSON.stringify(d), i); }
  static error() { return new Response('', { status: 0 }); }
}
class Request {
  constructor(input, init) {
    init = init || {};
    this.url = input instanceof Request ? input.url : String(input);
    this.method = (init.method || (input instanceof Request ? input.method : 'GET')).toUpperCase();
    this.headers = new Headers(init.headers || (input instanceof Request ? input.headers : undefined));
    this.body = init.body !== undefined ? init.body : (input instanceof Request ? input.body : null);
    this.signal = init.signal || null;
    this.credentials = init.credentials || 'same-origin';
  }
  clone() { return new Request(this); }
}

const pending = new Map();
function encodeBody(body, headers) {
  if (body === undefined || body === null) return '';
  if (typeof body === 'string') return body;
  if (body instanceof URLSearchParams) { if (!headers.has('content-type')) headers.set('content-type', 'application/x-www-form-urlencoded;charset=UTF-8'); return body.toString(); }
  if (body instanceof FormData) { if (!headers.has('content-type')) headers.set('content-type', 'application/x-www-form-urlencoded'); return new URLSearchParams(body._p.map(([k, v]) => [k, typeof v === 'string' ? v : ''])).toString(); }
  if (body instanceof Blob) return body._s;
  return String(body);
}
function startRequest(method, url, body, headers, cb) {
  const flat = [];
  headers.forEach((v, k) => { flat.push(k, v); });
  const id = K.request(method, url, body, flat);
  if (!id) { setTimeout(() => cb(0, '', '', [], url, true), 0); return 0; }
  pending.set(id, cb);
  return id;
}
G.__kiteResponse = function (id, status, statusText, body, hdrs, url, netErr) {
  const cb = pending.get(id);
  if (!cb) return;
  pending.delete(id);
  cb(status, statusText, body, hdrs, url, netErr);
};
function fetch(input, init) {
  return new Promise((resolve, reject) => {
    const req = new Request(input, init);
    if (req.signal && req.signal.aborted) { reject(req.signal.reason); return; }
    const body = encodeBody(req.body, req.headers);
    const id = startRequest(req.method, req.url, body, req.headers, (status, statusText, text, hdrs, url, netErr) => {
      if (netErr) { reject(new TypeError('Failed to fetch')); return; }
      const h = new Headers();
      for (let i = 0; i < hdrs.length; i += 2) h.append(hdrs[i], hdrs[i + 1]);
      const r = new Response(text, { status, statusText, headers: h, url });
      resolve(r);
    });
    if (req.signal) req.signal.addEventListener('abort', () => { pending.delete(id); reject(req.signal.reason); });
  });
}

class XMLHttpRequest extends EventTarget {
  constructor() {
    super();
    this.readyState = 0; this.status = 0; this.statusText = ''; this.responseText = ''; this.responseXML = null;
    this.responseType = ''; this.responseURL = ''; this.timeout = 0; this.withCredentials = false;
    this._headers = new Headers(); this._resp = []; this.upload = new EventTarget();
  }
  open(method, url) { this._method = String(method).toUpperCase(); this._url = String(url); this.readyState = 1; this._fire('readystatechange'); }
  setRequestHeader(k, v) { this._headers.append(k, v); }
  overrideMimeType() {}
  get response() {
    if (this.responseType === 'json') { try { return JSON.parse(this.responseText); } catch (e) { return null; } }
    if (this.responseType === 'document') return null;
    if (this.responseType === 'blob') return new Blob([this.responseText]);
    if (this.responseType === 'arraybuffer') return new TextEncoder().encode(this.responseText).buffer;
    return this.responseText;
  }
  send(body) {
    const b = encodeBody(body, this._headers);
    this._fire('loadstart');
    this._id = startRequest(this._method || 'GET', this._url, b, this._headers, (status, statusText, text, hdrs, url, netErr) => {
      if (this._aborted) return;
      if (netErr) { this.readyState = 4; this.status = 0; this._fire('readystatechange'); this._fire('error'); this._fire('loadend'); return; }
      this.status = status; this.statusText = statusText; this.responseURL = url; this._resp = hdrs;
      this.readyState = 2; this._fire('readystatechange');
      this.readyState = 3; this._fire('readystatechange');
      this.responseText = text; this.readyState = 4;
      this._fire('readystatechange'); this._fire('load'); this._fire('loadend');
    });
  }
  abort() { this._aborted = true; pending.delete(this._id); this.readyState = 0; this._fire('abort'); }
  getResponseHeader(k) { k = String(k).toLowerCase(); for (let i = 0; i < this._resp.length; i += 2) if (this._resp[i] === k) return this._resp[i + 1]; return null; }
  getAllResponseHeaders() { let s = ''; for (let i = 0; i < this._resp.length; i += 2) s += this._resp[i] + ': ' + this._resp[i + 1] + '\r\n'; return s; }
  _fire(type) { const ev = new ProgressEvent(type); this.dispatchEvent(ev); }
}
XMLHttpRequest.UNSENT = 0; XMLHttpRequest.OPENED = 1; XMLHttpRequest.HEADERS_RECEIVED = 2; XMLHttpRequest.LOADING = 3; XMLHttpRequest.DONE = 4;
EVENT_PROPS.forEach(t => {
  Object.defineProperty(XMLHttpRequest.prototype, 'on' + t, Object.getOwnPropertyDescriptor(EventTarget.prototype, 'on' + t));
});
// XHR targets have no parent: dispatch only to themselves.
XMLHttpRequest.prototype.dispatchEvent = function (ev) {
  ev.target = this; ev.eventPhase = 2; ev._path = [this];
  const h = this._on && this._on[ev.type];
  if (h) { try { h.call(this, ev); } catch (e) { reportError(e); } }
  invoke(this, ev, false);
  return !ev.defaultPrevented;
};
AbortSignal.prototype.dispatchEvent = XMLHttpRequest.prototype.dispatchEvent;

class MediaQueryList extends EventTarget {
  constructor(q) { super(); this.media = q; this.onchange = null; }
  get matches() { return K.media(this.media); }
  addListener(fn) { this.addEventListener('change', fn); }
  removeListener(fn) { this.removeEventListener('change', fn); }
}

// MutationObserver: the DOM primitives report every change; records are
// delivered in a microtask.
const moRegs = new Map();  // node handle -> [{ obs, opts }]
let moCount = 0;
const moPending = new Set();
function moDeliver() {
  const list = Array.from(moPending);
  moPending.clear();
  for (const o of list) {
    const recs = o._q;
    o._q = [];
    if (recs.length) { try { o._cb.call(o, recs, o); } catch (e) { reportError(e); } }
  }
}
function moQueue(type, h, rec) {
  const seen = new Set();
  for (let n = h, first = true; n; n = K.parent(n), first = false) {
    const regs = moRegs.get(n);
    if (!regs) continue;
    for (const r of regs) {
      const o = r.opts;
      if (!first && !o.subtree) continue;
      if (type === 'childList' && !o.childList) continue;
      if (type === 'attributes' && (!o.attributes || (o.attributeFilter && o.attributeFilter.indexOf(rec.attributeName) < 0))) continue;
      if (type === 'characterData' && !o.characterData) continue;
      if (seen.has(r.obs)) continue;
      seen.add(r.obs);
      const record = { type, target: W(h), addedNodes: nodeList([]), removedNodes: nodeList([]), previousSibling: null,
        nextSibling: null, attributeName: null, attributeNamespace: null, oldValue: null };
      Object.assign(record, rec);
      if ((type === 'attributes' && !o.attributeOldValue) || (type === 'characterData' && !o.characterDataOldValue)) record.oldValue = null;
      r.obs._q.push(record);
      if (!moPending.size) Promise.resolve().then(moDeliver);
      moPending.add(r.obs);
    }
  }
}
function siblingsOf(p, c) {
  const kids = K.kids(p);
  const i = kids.indexOf(c);
  return [i > 0 ? W(kids[i - 1]) : null, i >= 0 && i + 1 < kids.length ? W(kids[i + 1]) : null];
}
{
  const rawInsert = K.insert, rawRemove = K.remove, rawSetAttr = K.setAttr, rawDelAttr = K.delAttr, rawSetData = K.setData;
  K.insert = function (p, c, before) {
    if (!moCount) return rawInsert(p, c, before);
    const added = K.type(c) === 11 ? K.kids(c) : [c];
    const old = K.type(c) === 11 ? 0 : K.parent(c);
    if (old) {
      const [ps, ns] = siblingsOf(old, c);
      moQueue('childList', old, { removedNodes: nodeList([W(c)]), previousSibling: ps, nextSibling: ns });
    }
    const r = rawInsert(p, c, before);
    if (added.length) {
      const [ps] = siblingsOf(p, added[0]);
      const [, ns] = siblingsOf(p, added[added.length - 1]);
      moQueue('childList', p, { addedNodes: nodeList(added.map(W)), previousSibling: ps, nextSibling: ns });
    }
    return r;
  };
  K.remove = function (c) {
    const p = moCount ? K.parent(c) : 0;
    if (!p) return rawRemove(c);
    const [ps, ns] = siblingsOf(p, c);
    const r = rawRemove(c);
    moQueue('childList', p, { removedNodes: nodeList([W(c)]), previousSibling: ps, nextSibling: ns });
    return r;
  };
  K.setAttr = function (h, n, v) {
    if (!moCount) return rawSetAttr(h, n, v);
    const old = K.attr(h, n);
    const r = rawSetAttr(h, n, v);
    moQueue('attributes', h, { attributeName: String(n).toLowerCase(), oldValue: old });
    return r;
  };
  K.delAttr = function (h, n) {
    if (!moCount) return rawDelAttr(h, n);
    const old = K.attr(h, n);
    const r = rawDelAttr(h, n);
    if (old !== null) moQueue('attributes', h, { attributeName: String(n).toLowerCase(), oldValue: old });
    return r;
  };
  K.setData = function (h, v) {
    if (!moCount) return rawSetData(h, v);
    const old = K.data(h);
    const r = rawSetData(h, v);
    moQueue('characterData', h, { oldValue: old });
    return r;
  };
}
class MutationObserver {
  constructor(cb) {
    if (typeof cb !== 'function') throw new TypeError('MutationObserver: callback is not a function');
    this._cb = cb; this._q = []; this._targets = new Set();
  }
  observe(target, options) {
    if (!target || !target._h) throw new TypeError("Failed to execute 'observe' on 'MutationObserver': parameter 1 is not a Node.");
    const o = Object.assign({}, options || {});
    if (o.attributeOldValue || o.attributeFilter) o.attributes = o.attributes !== false;
    if (o.characterDataOldValue) o.characterData = o.characterData !== false;
    if (!o.childList && !o.attributes && !o.characterData)
      throw new TypeError("Failed to execute 'observe' on 'MutationObserver': no mutation type selected.");
    if (o.attributeFilter) o.attributeFilter = Array.from(o.attributeFilter, x => String(x).toLowerCase());
    let regs = moRegs.get(target._h);
    if (!regs) { regs = []; moRegs.set(target._h, regs); }
    const existing = regs.find(r => r.obs === this);
    if (existing) existing.opts = o;
    else { regs.push({ obs: this, opts: o }); moCount++; this._targets.add(target._h); }
  }
  disconnect() {
    for (const h of this._targets) {
      const regs = moRegs.get(h);
      if (!regs) continue;
      const left = regs.filter(r => r.obs !== this);
      moCount -= regs.length - left.length;
      if (left.length) moRegs.set(h, left); else moRegs.delete(h);
    }
    this._targets.clear();
    this._q = [];
  }
  takeRecords() { const r = this._q; this._q = []; return r; }
}
class IntersectionObserver {
  constructor(cb, opts) { this._cb = cb; this._t = []; this.root = null; this.rootMargin = '0px'; this.thresholds = [0]; }
  observe(el) {
    this._t.push(el);
    // Everything counts as visible: lazy loaders then load their content.
    setTimeout(() => {
      if (this._t.indexOf(el) < 0) return;
      const r = el.getBoundingClientRect();
      try { this._cb([{ target: el, isIntersecting: true, intersectionRatio: 1, boundingClientRect: r, intersectionRect: r, rootBounds: null, time: performance.now() }], this); } catch (e) { reportError(e); }
    }, 0);
  }
  unobserve(el) { this._t = this._t.filter(x => x !== el); }
  disconnect() { this._t = []; }
  takeRecords() { return []; }
}
class ResizeObserver {
  constructor(cb) { this._cb = cb; }
  observe(el) {
    setTimeout(() => {
      const r = el.getBoundingClientRect();
      const size = [{ inlineSize: r.width, blockSize: r.height }];
      try { this._cb([{ target: el, contentRect: r, borderBoxSize: size, contentBoxSize: size }], this); } catch (e) { reportError(e); }
    }, 0);
  }
  unobserve() {}
  disconnect() {}
}
const customRegistry = new Map();
const customElements = {
  define(name, cls) { customRegistry.set(String(name).toLowerCase(), cls); },
  get(name) { return customRegistry.get(String(name).toLowerCase()); },
  whenDefined(name) { return Promise.resolve(customRegistry.get(String(name).toLowerCase())); },
  upgrade() {},
};

function getComputedStyle(el) {
  const get = p => el && el._h ? K.computed(el._h, p) : '';
  return new Proxy({ getPropertyValue: p => get(String(p).toLowerCase()) }, {
    get(t, k) { if (k in t) return t[k]; if (typeof k !== 'string') return undefined; return get(kebab(k)); },
  });
}

const selection = { rangeCount: 0, isCollapsed: true, toString: () => '', removeAllRanges() {}, addRange() {}, getRangeAt() { return null; }, collapse() {}, empty() {} };

const navigator = {
  userAgent: 'Mozilla/5.0 (Windows NT 5.0) Kite/1.0 (like Gecko)',
  appName: 'Netscape', appCodeName: 'Mozilla', appVersion: '5.0 (Windows NT 5.0)', product: 'Gecko',
  platform: 'Win32', vendor: '', language: 'de-DE', languages: ['de-DE', 'de', 'en'],
  cookieEnabled: true, onLine: true, doNotTrack: '1', hardwareConcurrency: 1, maxTouchPoints: 0,
  webdriver: false, pdfViewerEnabled: false, plugins: [], mimeTypes: [],
  javaEnabled() { return false; }, sendBeacon() { return true; }, vibrate() { return false; },
  clipboard: { writeText: () => Promise.resolve(), readText: () => Promise.resolve('') },
  userAgentData: undefined, connection: { effectiveType: '3g', saveData: false },
  permissions: { query: () => Promise.resolve({ state: 'denied' }) },
};

Object.assign(G, {
  window: G, self: G, top: G, parent: G, frames: G, globalThis: G, document: doc,
  location, navigator, console: consoleObj, performance,
  history, PopStateEvent, HashChangeEvent,
  screen: { width: 1024, height: 768, availWidth: 1024, availHeight: 740, colorDepth: 24, pixelDepth: 24, orientation: { type: 'landscape-primary', angle: 0, addEventListener() {} } },
  localStorage: new Storage(false), sessionStorage: new Storage(true),
  setTimeout: makeTimer(false), setInterval: makeTimer(true),
  clearTimeout: id => K.clearTimer(id | 0), clearInterval: id => K.clearTimer(id | 0),
  requestAnimationFrame: fn => K.timer(() => fn(performance.now()), 16, false),
  cancelAnimationFrame: id => K.clearTimer(id | 0),
  requestIdleCallback: fn => K.timer(() => fn({ didTimeout: false, timeRemaining: () => 10 }), 1, false),
  cancelIdleCallback: id => K.clearTimer(id | 0),
  queueMicrotask: fn => { Promise.resolve().then(fn); },
  alert: m => K.alert(m === undefined ? '' : String(m)),
  confirm: m => K.confirm(m === undefined ? '' : String(m)),
  prompt: (m, d) => K.prompt(m === undefined ? '' : String(m), d === undefined ? '' : String(d)),
  print() {}, focus() {}, blur() {}, close() {}, stop() {},
  open: (url) => { if (url) K.navigate(String(url), false); return null; },
  postMessage(data) { setTimeout(() => G.dispatchEvent(new MessageEvent('message', { data, origin: location.origin, source: G })), 0); },
  getComputedStyle, matchMedia: q => new MediaQueryList(String(q)), getSelection: () => selection,
  scrollTo(x, y) { if (x && typeof x === 'object') { y = x.top; x = x.left; } K.scrollTo(+x || 0, +y || 0); },
  scroll(x, y) { G.scrollTo(x, y); },
  scrollBy(x, y) { if (x && typeof x === 'object') { y = x.top; x = x.left; } K.scrollTo(G.scrollX + (+x || 0), G.scrollY + (+y || 0)); },
  atob, btoa, URL, URLSearchParams, DOMMatrix, DOMMatrixReadOnly: DOMMatrix, ImageData, Path2D,
  CanvasRenderingContext2D, CanvasGradient, CanvasPattern, OffscreenCanvas, createImageBitmap, fetch, XMLHttpRequest, Headers, Request, Response, Blob, File, FormData,
  AbortController, AbortSignal, TextEncoder, TextDecoder, DOMException,
  Event, UIEvent, MouseEvent, PointerEvent, KeyboardEvent, FocusEvent, InputEvent, CustomEvent, MessageEvent,
  ErrorEvent, ProgressEvent, SubmitEvent, EventTarget,
  Node, Element, HTMLElement, Document, HTMLDocument, DocumentFragment, Text, Comment, CharacterData,
  SVGElement, SVGSVGElement, DOMTokenList, Storage, MediaQueryList,
  MutationObserver, IntersectionObserver, ResizeObserver, customElements,
  Image: function (w, h) { const i = doc.createElement('img'); if (w) i.width = w; if (h) i.height = h; return i; },
  Option: function (text, value, d, sel) { const o = doc.createElement('option'); if (text !== undefined) o.text = text; if (value !== undefined) o.value = value; if (sel) o.setAttribute('selected', ''); return o; },
  DOMParser: class { parseFromString(s) { const d = doc.createElement('html'); for (const h of K.parse(String(s), '')) K.insert(d._h, h, 0); return { documentElement: d, body: d, head: d, querySelector: q => d.querySelector(q), querySelectorAll: q => d.querySelectorAll(q), getElementById: id => d.querySelector('#' + cssEscape(id)), get title() { const t = d.querySelector('title'); return t ? t.textContent : ''; } }; } },
  XMLSerializer: class { serializeToString(n) { return n.outerHTML || n.textContent || ''; } },
  CSS: { supports: () => false, escape: cssEscape },
  crypto: {
    getRandomValues(a) { for (let i = 0; i < a.length; i++) a[i] = Math.floor(Math.random() * 4294967296); return a; },
    randomUUID() { return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, c => { const r = Math.random() * 16 | 0; return (c === 'x' ? r : (r & 3 | 8)).toString(16); }); },
    subtle: {},
  },
  structuredClone: v => v === undefined ? v : JSON.parse(JSON.stringify(v)),
  devicePixelRatio: 1, isSecureContext: true, origin: '', name: '', closed: false, opener: null, frameElement: null,
  speechSynthesis: undefined, indexedDB: undefined, caches: undefined,
});
Object.defineProperty(G, 'innerWidth', { get: () => K.viewport()[0], configurable: true });
Object.defineProperty(G, 'innerHeight', { get: () => K.viewport()[1], configurable: true });
Object.defineProperty(G, 'outerWidth', { get: () => K.viewport()[0], configurable: true });
Object.defineProperty(G, 'outerHeight', { get: () => K.viewport()[1], configurable: true });
Object.defineProperty(G, 'scrollX', { get: () => K.scroll()[0], configurable: true });
Object.defineProperty(G, 'scrollY', { get: () => K.scroll()[1], configurable: true });
Object.defineProperty(G, 'pageXOffset', { get: () => K.scroll()[0], configurable: true });
Object.defineProperty(G, 'pageYOffset', { get: () => K.scroll()[1], configurable: true });
Object.defineProperty(G, 'origin', { get: () => location.origin, configurable: true });

// ------------------------------------------------------------------ Hooks
G.__kiteDocEvent = function (type) {
  if (type === 'DOMContentLoaded') {
    readyState = 'interactive';
    doc.dispatchEvent(new Event('readystatechange'));
    doc.dispatchEvent(new Event('DOMContentLoaded', { bubbles: true }));
  } else if (type === 'load') {
    readyState = 'complete';
    doc.dispatchEvent(new Event('readystatechange'));
    for (const img of doc.querySelectorAll('img')) img.dispatchEvent(new Event('load'));
    G.dispatchEvent(new Event('load'));
    G.dispatchEvent(new Event('pageshow'));
  }
};
G.__kiteWinEvent = type => { G.dispatchEvent(new Event(type)); };
G.__kiteDispatch = function (h, type) {
  const target = W(h);
  if (!target) return true;
  let ev;
  if (type === 'click' || type === 'dblclick' || type === 'mousedown' || type === 'mouseup' || type === 'contextmenu')
    ev = new MouseEvent(type, { bubbles: true, cancelable: true });
  else if (type === 'submit') ev = new SubmitEvent(type, { bubbles: true, cancelable: true });
  else if (type === 'focus' || type === 'blur') ev = new FocusEvent(type, {});
  else if (type === 'input') ev = new InputEvent(type, { bubbles: true });
  else ev = new Event(type, { bubbles: type !== 'load', cancelable: true });
  ev.isTrusted = true;
  return target.dispatchEvent(ev);
};
})();
)KITEJS";

}  // namespace kite

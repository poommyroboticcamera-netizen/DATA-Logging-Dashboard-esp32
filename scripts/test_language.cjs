const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

let observer;
const records = [];
class Element {
  constructor(id = '') { this.id = id; this.nodeType = 1; this.tagName = 'DIV'; this.childNodes = []; this.isConnected = true; }
  append(node) { node.parentElement = this; this.childNodes.push(node); }
  closest(selector) { return selector === '#language-switch' && this.id === 'language-switch' ? this : null; }
  getAttribute() { return null; }
  addEventListener(type, callback) { this[type] = callback; }
  set textContent(value) { this.label = value; }
}
class Text {
  constructor(value) { this.nodeType = 3; this.value = value; this.isConnected = true; }
  get nodeValue() { return this.value; }
  set nodeValue(value) {
    this.value = value;
    if (observer?.active) records.push({ type: 'characterData', target: this, addedNodes: [] });
  }
}
class Observer {
  constructor(callback) { this.callback = callback; observer = this; }
  observe() { this.active = true; }
}
function flush() {
  let rounds = 0;
  while (records.length) {
    assert.ok(++rounds < 20, 'translation mutations must settle');
    observer.callback(records.splice(0));
  }
}

const body = new Element();
const button = new Element('language-switch');
const text = new Text('พร้อม / ข้อความที่ยังไม่มีคำแปล');
body.append(button);
body.append(text);
const document = { body, documentElement: {}, getElementById: () => button };
const window = { dispatchEvent() {} };
vm.runInNewContext(fs.readFileSync('dashboard/language.js', 'utf8'), {
  document, window, Node: { TEXT_NODE: 3, ELEMENT_NODE: 1 }, MutationObserver: Observer,
  localStorage: { getItem: () => null, setItem() {} }, CustomEvent: class {}
});
assert.equal(text.nodeValue, 'ready / ข้อความที่ยังไม่มีคำแปล');
text.nodeValue = 'พร้อม / ข้อความที่ยังไม่มีคำแปลใหม่';
flush();
assert.equal(text.nodeValue, 'ready / ข้อความที่ยังไม่มีคำแปลใหม่');
button.click();
flush();
assert.equal(text.nodeValue, 'พร้อม / ข้อความที่ยังไม่มีคำแปลใหม่');
button.click();
flush();
assert.equal(text.nodeValue, 'ready / ข้อความที่ยังไม่มีคำแปลใหม่');
text.nodeValue = 'new sensor reading';
flush();
button.click();
flush();
assert.equal(text.nodeValue, 'new sensor reading');
console.log('PASS: untranslated Thai settles, language switches preserve latest text');

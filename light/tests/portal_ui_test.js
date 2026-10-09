const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const source = fs.readFileSync('light/main/app_portal_ui.h', 'utf8').split('<script>')[1].split('</script>')[0];
const elements = new Map();
class Element {
  constructor() { this.value = ''; this.style = {}; this.hidden = false; this.disabled = false; this.files = []; }
  set id(value) { this._id = value; elements.set(value, this); }
  get id() { return this._id; }
  append() {}
}
for (const id of ['login','panel','key','enter','loginStatus','module','ssid','names','info','settings','wifiPassword','openWifi','newKey','saveStatus','firmware','upload','progress','otaStatus','matterQr','matterManual','matterStatus','matterDevice','pairMatter']) {
  const element = new Element(); element.id = id;
}
let request;
class XHR {
  constructor() { request = this; this.upload = {}; this.headers = {}; }
  open(method, path) { this.method = method; this.path = path; }
  setRequestHeader(key, value) { this.headers[key] = value; }
  send(body) { this.body = body; }
}
const config = {module: 'Modulo <script>', ssid: 'Rede "teste"', names: ['Luz <1>'], inputs:[33],matter:{device:'80:65:99:4C:CB:9C',payload:'MT:TEST',manual:'12345678901',qr:['101','010','101']},version:'1.0', ip:'192.168.15.5'};
let lastFetch, unauthorized = false;
const context = vm.createContext({
  document: {getElementById: id => elements.get(id), createElement: () => new Element(), querySelectorAll: () => [elements.get('enter'),elements.get('upload')]},
  fetch: async (path, options) => {
    lastFetch = {path, options};
    return {ok: !unauthorized, text: async () => 'Senha incorreta', json: async () => options.method && path !== '/api/matter/pair' ? {message:'Salvo'} : config};
  },
  XMLHttpRequest: XHR,
});
vm.runInContext(source, context);
const get = id => elements.get(id);
(async () => {
  assert.equal(elements.has('name0'), true);
  assert.equal(elements.has('name1'), false);
  assert.equal(elements.has('input1'), false);
  get('key').value = 'configurar123';
  await get('enter').onclick();
  assert.equal(lastFetch.options.headers['X-Portal-Key'], 'configurar123');
  assert.equal(get('panel').hidden, false);
  assert.equal(get('module').value, config.module);
  assert.equal(get('name0').value, config.names[0]);
  assert.equal(get('matterQr').hidden, false);
  assert.equal(get('matterManual').textContent, config.matter.manual);
  assert.equal(get('matterDevice').textContent, config.matter.device);
  config.matter.windowOpen = false;
  get('key').value = 'configurar123';
  await get('enter').onclick();
  assert.match(get('matterStatus').textContent, /Pareamento fechado/);
  config.matter.windowOpen = true;
  await get('pairMatter').onclick();
  assert.equal(lastFetch.path, '/api/matter/pair');
  assert.equal(lastFetch.options.method, 'POST');
  assert.equal(lastFetch.options.headers['X-Portal-Key'], 'configurar123');
  assert.match(get('matterStatus').textContent, /Pareamento aberto/);
  config.matter.wifiConnected = false;
  await get('pairMatter').onclick();
  assert.match(get('matterStatus').textContent, /fora do Wi-Fi/);
  config.matter.wifiConnected = true;
  const qrSvg = decodeURIComponent(get('matterQr').src.split(',')[1]);
  assert.match(qrSvg, /viewBox="0 0 11 11"/);
  assert.match(qrSvg, /M4 4h1v1h-1z/);
  vm.runInContext('showMatter(null)', context);
  assert.equal(get('matterQr').hidden, true);
  vm.runInContext('showMatter({qr:["<script>"]})', context);
  assert.match(get('matterStatus').textContent, /invalido/);
  vm.runInContext('showMatter({manual:"123",qr:["1"]})', context);
  assert.equal(get('info').textContent, 'Versão 1.0 · IP 192.168.15.5');
  // Legacy API responses may include all six stored names.
  config.names.push('Luz 2', 'Luz 3', 'Luz 4', 'Luz 5', 'Luz 6');
  get('panel').hidden = true;
  get('key').value = 'configurar123';
  await get('enter').onclick();
  assert.equal(get('panel').hidden, false);
  assert.equal(get('name0').value, config.names[0]);
  config.names.splice(1);
  await get('settings').onsubmit({preventDefault(){}});
  const saved = JSON.parse(lastFetch.options.body);
  assert.equal(saved.ssid, config.ssid);
  assert.equal(saved.names.length, 1);
  assert.deepEqual(saved.inputs, config.inputs);
  assert.equal(saved.password, '');
  assert.equal(lastFetch.options.headers['Content-Type'], 'application/json');
  assert.equal(get('saveStatus').textContent, 'Salvo');
  get('input0').value = 34;
  const previousFetch = lastFetch;
  await get('settings').onsubmit({preventDefault(){}});
  assert.equal(lastFetch, previousFetch);
  assert.match(get('saveStatus').textContent, /GPIO 33/);
  get('input0').value = config.inputs[0];
  get('upload').onclick();
  assert.equal(get('otaStatus').textContent, 'Selecione o light.bin.');
  const file = {name:'light.bin', size:1234}; get('firmware').files = [file];
  get('upload').onclick();
  assert.equal(request.path, '/api/ota');
  assert.equal(request.headers['Content-Type'], 'application/octet-stream');
  assert.equal(request.headers['X-Portal-Key'], 'configurar123');
  assert.equal(request.body, file);
  request.upload.onprogress({lengthComputable:true, loaded:50,total:100});
  assert.equal(get('progress').value, 50);
  request.status = 400; request.responseText = 'Firmware inválido'; request.onload();
  assert.equal(get('otaStatus').textContent, 'Firmware inválido');
  assert.equal(get('upload').disabled, false);
  request.status = 200; request.onload();
  assert.match(get('otaStatus').textContent, /validada/);
  unauthorized = true;
  await get('enter').onclick();
  assert.equal(get('loginStatus').textContent, 'Senha incorreta');
  console.log('Portal UI: login, escaped names, settings, raw OTA, progress and errors passed.');
})().catch(error => {console.error(error);process.exitCode=1;});

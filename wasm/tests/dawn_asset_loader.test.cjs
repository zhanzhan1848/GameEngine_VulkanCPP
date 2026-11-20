const { test } = require('node:test');
const assert = require('node:assert/strict');
const { webcrypto, createHash } = require('node:crypto');
const { stageAssets } = require('../dawn_asset_loader.js');
const bytes = Buffer.from('scene asset');
const hash = createHash('sha256').update(bytes).digest('hex');
const model = {url: 'https://example.test/model', fsPath: '/assets/Sponza.model', size: bytes.length, sha256: hash};
function fixture() {
  let staged = new Map(), dirs = new Set(), progress = [];
  return { staged, progress, options: {
    crypto: webcrypto,
    fetch: async () => ({ok: true, arrayBuffer: async () => Uint8Array.from(bytes).buffer}),
    fs: {analyzePath: path => ({exists: dirs.has(path)}), mkdirTree: path => dirs.add(path), writeFile: (path, data) => staged.set(path, Buffer.from(data))},
    progress: (done,total) => progress.push([done,total])
  }};
}
test('stages verified model before reporting completion', async () => {
  const f = fixture();
  await stageAssets([model], f.options);
  assert.deepEqual(f.staged.get(model.fsPath), bytes);
  assert.deepEqual(f.progress, [[1, 1]]);
});
test('rejects missing model and corrupted data without writing', async () => {
  const f = fixture();
  await assert.rejects(stageAssets([], f.options), /missing the model/);
  await assert.rejects(stageAssets([{...model, sha256:'0'.repeat(64)}], f.options), /SHA-256 mismatch/);
  assert.equal(f.staged.size, 0);
});
test('rejects missing objects instead of silently starting', async () => {
  const f = fixture();
  f.options.fetch = async () => ({ok: false, status: 404});
  await assert.rejects(stageAssets([model], f.options), /HTTP 404/);
  assert.equal(f.staged.size, 0);
});

test('reassembles R2 ranges by offset before verifying a large model', async () => {
  const f = fixture();
  const part = 16 * 1024 * 1024;
  const size = 4 * part + 7;
  const digest = createHash('sha256');
  for (let start = 0; start < size; start += part)
    digest.update(Buffer.alloc(Math.min(part, size - start), start / part));
  const ranges = [];
  f.options.fetch = async (_, init) => {
    const [, a, b] = /^bytes=(\d+)-(\d+)$/.exec(init.headers.Range);
    const start = Number(a), end = Number(b);
    ranges.push([start, end]);
    if (start === part) await new Promise(resolve => setTimeout(resolve, 10));
    return {ok: true, status: 206, arrayBuffer: async () =>
      new Uint8Array(end - start + 1).fill(start / part).buffer};
  };
  await stageAssets([{...model, size, sha256: digest.digest('hex')}], f.options);
  assert.equal(ranges.length, 5);
  assert.equal(f.staged.get(model.fsPath)[size - 1], 4);
});

test('shares verified compressed assets across loads and repairs a damaged cached response', async () => {
  const f = fixture();
  const compressed = require('node:zlib').gzipSync(bytes);
  const item = {...model, gzip: {url: model.url + '.gz', size: compressed.length}};
  const entries = new Map();
  f.options.cacheStorage = {open: async () => ({
    match: async url => entries.get(url)?.clone(),
    put: async (url, response) => entries.set(url, response)
  })};
  let requests = 0;
  f.options.fetch = async url => {
    assert.equal(url, item.gzip.url);
    requests++;
    return new Response(compressed);
  };
  await stageAssets([item], f.options);
  await stageAssets([item], f.options);
  assert.equal(requests, 1);
  entries.set(item.gzip.url, new Response('broken'));
  await stageAssets([item], f.options);
  assert.equal(requests, 2);
  assert.deepEqual(f.staged.get(item.fsPath), bytes);
});

test('cache quota failures still allow rendering', async () => {
  const f = fixture();
  f.options.cacheStorage = {open: async () => ({
    match: async () => undefined, put: async () => {throw new Error('quota');}
  })};
  await stageAssets([model], f.options);
  assert.deepEqual(f.staged.get(model.fsPath), bytes);
});

test('downloads before WASM is ready and mounts only after MEMFS becomes available', async () => {
  const f = fixture();
  const fs = f.options.fs;
  let mount, downloaded;
  const started = new Promise(resolve => { downloaded = resolve; });
  f.options.fs = new Promise(resolve => { mount = resolve; });
  f.options.fetch = async () => { downloaded(); return new Response(bytes); };
  const ready = stageAssets([model], f.options);
  await started;
  assert.equal(f.staged.size, 0);
  mount(fs);
  await ready;
  assert.deepEqual(f.staged.get(model.fsPath), bytes);
});

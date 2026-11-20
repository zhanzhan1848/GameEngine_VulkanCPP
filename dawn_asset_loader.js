(function (root) {
  'use strict';
  async function stageAssets(items, options) {
    if (!Array.isArray(items) || !items.length || !items.some(function (item) { return item.fsPath === (options.modelPath || '/assets/Sponza.model'); })) {
      throw new Error('Dawn asset manifest is empty or missing the model');
    }
    if (!options.crypto || !options.crypto.subtle) throw new Error('SHA-256 requires a secure browser context');
    const compressed = typeof DecompressionStream === 'function';
    const sourceFor = item => compressed && item.gzip ? item.gzip : item;
    const queue = [...items].sort((a, b) => sourceFor(b).size - sourceFor(a).size);
    const totalBytes = queue.reduce((total, item) => total + sourceFor(item).size, 0);
    let cache;
    try {
      if (options.cacheStorage) cache = await options.cacheStorage.open('dawn-scene-assets-v1');
    } catch { /* Storage may be unavailable in private browsing. */ }
    let nextIndex = 0, finished = 0, readyBytes = 0;
    // R2 serves single byte ranges across origins. Bound both concurrency
    // and retry size for the tangent model and the offline SDF.
    const chunkSize = 16 * 1024 * 1024;
    async function download(item, start, end) {
      const partial = end !== undefined;
      let lastError;
      for (let attempt = 0; attempt < 3; attempt++) {
        const controller = new AbortController();
        const timer = setTimeout(() => controller.abort(), partial || item.size > 8 * 1024 * 1024 ? 180000 : 90000);
        let retry = true;
        try {
          const headers = partial ? { Range: 'bytes=' + start + '-' + end } : {};
          const response = await options.fetch(item.url, { signal: controller.signal, headers });
          if (!response.ok) {
            retry = response.status === 429 || response.status >= 500;
            throw new Error('HTTP ' + response.status);
          }
          const data = new Uint8Array(await response.arrayBuffer());
          // A local mirror may ignore Range and return the full first GET.
          if (start === 0 && response.status === 200 && data.length === item.size) return data;
          if (partial && (response.status !== 206 || data.length !== end - start + 1))
            throw new Error('invalid byte range');
          return data;
        } catch (error) {
          lastError = error;
          if (!retry) break;
        } finally {
          clearTimeout(timer);
        }
        if (attempt < 2) await new Promise(resolve => setTimeout(resolve, 500 * (attempt + 1)));
      }
      throw new Error(item.fsPath + ': ' + lastError.message);
    }

    async function readAsset(item) {
      if (item.size < 64 * 1024 * 1024) return download(item);
      const first = await download(item, 0, chunkSize - 1);
      if (first.length === item.size) return first;
      const data = new Uint8Array(item.size);
      data.set(first);
      let next = chunkSize;
      async function readRanges() {
        while (next < item.size) {
          const start = next;
          next += chunkSize;
          const chunk = await download(item, start, Math.min(next, item.size) - 1);
          data.set(chunk, start);
        }
      }
      await Promise.all([readRanges(), readRanges(), readRanges()]);
      return data;
    }

    async function decode(item, bytes) {
      const source = sourceFor(item);
      if (bytes.byteLength !== source.size) throw new Error(item.fsPath + ': size mismatch');
      let data = bytes;
      if (source !== item) {
        const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('gzip'));
        data = new Uint8Array(await new Response(stream).arrayBuffer());
      }
      if (data.byteLength !== item.size) throw new Error(item.fsPath + ': size mismatch');
      var hash = Array.from(new Uint8Array(await options.crypto.subtle.digest('SHA-256', data)))
        .map(function (v) { return v.toString(16).padStart(2, '0'); }).join('');
      if (hash !== item.sha256) throw new Error(item.fsPath + ': SHA-256 mismatch');
      return data;
    }

    async function stage(item) {
      const source = sourceFor(item);
      let data;
      if (cache) {
        try {
          const stored = await cache.match(source.url);
          if (stored) data = await decode(item, new Uint8Array(await stored.arrayBuffer()));
        } catch { /* A broken entry will be replaced after verification. */ }
      }
      if (!data) {
        const bytes = await readAsset({...source, fsPath: item.fsPath});
        data = await decode(item, bytes);
        if (cache) {
          try { await cache.put(source.url, new Response(bytes)); }
          catch { /* An evicted or full cache must not block rendering. */ }
        }
      }
      var dir = item.fsPath.slice(0, item.fsPath.lastIndexOf('/'));
      if (dir && !options.fs.analyzePath(dir).exists) options.fs.mkdirTree(dir);
      options.fs.writeFile(item.fsPath, data);
      readyBytes += source.size;
      options.progress(++finished, items.length, readyBytes, totalBytes);
    }
    async function worker() {
      while (nextIndex < items.length) await stage(queue[nextIndex++]);
    }
    await Promise.all(Array.from({length: Math.min(4, items.length)}, worker));
  }
  root.DawnAssetLoader = { stageAssets: stageAssets };
  if (typeof module !== 'undefined') module.exports = root.DawnAssetLoader;
})(typeof window !== 'undefined' ? window : globalThis);

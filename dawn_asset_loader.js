(function (root) {
  'use strict';
  async function stageAssets(items, options) {
    if (!Array.isArray(items) || !items.length || !items.some(function (item) { return item.fsPath === (options.modelPath || '/assets/Sponza.model'); })) {
      throw new Error('Dawn asset manifest is empty or missing the model');
    }
    if (!options.crypto || !options.crypto.subtle) throw new Error('SHA-256 requires a secure browser context');
    var nextIndex = 0, finished = 0;
    // R2 serves single byte ranges across origins. Bound both concurrency
    // and retry size for the tangent model and the offline SDF.
    const chunkSize = 16 * 1024 * 1024;
    async function download(item, start, end) {
      const partial = end !== undefined;
      let lastError;
      for (let attempt = 0; attempt < 3; attempt++) {
        const controller = new AbortController();
        const timer = setTimeout(() => controller.abort(), partial ? 180000 : 90000);
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

    async function stage(item) {
      const data = await readAsset(item);
      if (data.byteLength !== item.size) throw new Error(item.fsPath + ': size mismatch');
      var hash = Array.from(new Uint8Array(await options.crypto.subtle.digest('SHA-256', data)))
        .map(function (v) { return v.toString(16).padStart(2, '0'); }).join('');
      if (hash !== item.sha256) throw new Error(item.fsPath + ': SHA-256 mismatch');
      var dir = item.fsPath.slice(0, item.fsPath.lastIndexOf('/'));
      if (dir && !options.fs.analyzePath(dir).exists) options.fs.mkdirTree(dir);
      options.fs.writeFile(item.fsPath, data);
      options.progress(++finished, items.length);
    }
    async function worker() {
      while (nextIndex < items.length) await stage(items[nextIndex++]);
    }
    await Promise.all(Array.from({length: Math.min(4, items.length)}, worker));
  }
  root.DawnAssetLoader = { stageAssets: stageAssets };
  if (typeof module !== 'undefined') module.exports = root.DawnAssetLoader;
})(typeof window !== 'undefined' ? window : globalThis);

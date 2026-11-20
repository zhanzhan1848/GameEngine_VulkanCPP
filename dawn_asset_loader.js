(function (root) {
  'use strict';
  async function stageAssets(items, options) {
    if (!Array.isArray(items) || !items.length || !items.some(function (item) { return item.fsPath === (options.modelPath || '/assets/Sponza.model'); })) {
      throw new Error('Dawn asset manifest is empty or missing the model');
    }
    if (!options.crypto || !options.crypto.subtle) throw new Error('SHA-256 requires a secure browser context');
    var nextIndex = 0, finished = 0;
    async function stage(item) {
      var response, lastError;
      for (var attempt = 0; attempt < 3; ++attempt) {
        var controller = new AbortController();
        var timer = setTimeout(function () { controller.abort(); }, 45000);
        try {
          response = await options.fetch(item.url, { signal: controller.signal });
          if (response.ok) break;
          if (response.status !== 429 && response.status < 500) throw new Error('HTTP ' + response.status);
          lastError = new Error('HTTP ' + response.status);
          response = null;
        } catch (err) {
          lastError = err;
          if (response) throw err;
        } finally {
          clearTimeout(timer);
        }
        if (attempt < 2) await new Promise(function (resolve) { setTimeout(resolve, 500 * (attempt + 1)); });
      }
      if (!response) throw new Error(item.fsPath + ': ' + lastError);
      var data = await response.arrayBuffer();
      if (data.byteLength !== item.size) throw new Error(item.fsPath + ': size mismatch');
      var hash = Array.from(new Uint8Array(await options.crypto.subtle.digest('SHA-256', data)))
        .map(function (v) { return v.toString(16).padStart(2, '0'); }).join('');
      if (hash !== item.sha256) throw new Error(item.fsPath + ': SHA-256 mismatch');
      var dir = item.fsPath.slice(0, item.fsPath.lastIndexOf('/'));
      if (dir && !options.fs.analyzePath(dir).exists) options.fs.mkdirTree(dir);
      options.fs.writeFile(item.fsPath, new Uint8Array(data));
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

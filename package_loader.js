// package_loader.js — web-cloud-delivery S1（2026-10-15）：v2 运行时包入口。
//
// 与 PrimalPlayer 行式解析同一契约（RuntimeSceneExporter 生成的 .primalrun）：
// 行键 = 首个空格前的 token；未知键容忍并计入 diagnostics（与 Player 一致，
// 不静默也不阻断）；结构性错误（缺头/实体未闭合/坏数字）抛 ParseError。
// 可在浏览器（fetch+parse）与 Node（测试）双端运行——零依赖。

'use strict';

class ParseError extends Error {
  constructor(message, lineNo) {
    super(lineNo > 0 ? `${message}（第 ${lineNo} 行）` : message);
    this.name = 'RuntimePackageParseError';
  }
}

function parseFloats(rest, count) {
  const parts = rest.trim().split(/\s+/).filter(s => s.length > 0);
  const out = [];
  for (let i = 0; i < count; ++i) {
    const v = Number.parseFloat(parts[i]);
    if (!Number.isFinite(v)) return null;
    out.push(v);
  }
  return out;
}

function parseQuoted(rest) {
  const m = /^\s*"([^"]*)"/.exec(rest);
  return m ? m[1] : null;
}

/** 解析 v2 运行时包文本 → 结构化场景 + 资产清单 + 诊断。 */
function parseRuntimePackage(text) {
  if (typeof text !== 'string') throw new ParseError('包内容必须是文本', 0);
  const lines = text.split('\n');
  if (lines.length === 0 || !lines[0].startsWith('# primal-runtime-scene')) {
    throw new ParseError('缺少 v2 头（# primal-runtime-scene …）', 1);
  }

  const pkg = {
    formatVersion: 2,
    sceneName: 'Scene',
    env: null,
    entities: [],
    sequences: { fps: 60, duration: 0, propertyTracks: [], positionTracks: [] },
    assets: [],          // 包引用的资产路径（mesh/纹理/脚本/音频）
    diagnostics: [],     // 未知键/坏行（容忍项）——不静默
  };

  let current = null;
  let inEntity = false;
  let seqPropTarget = null;
  let seqPosTarget = null;
  let lineNo = 0;

  for (const raw of lines) {
    ++lineNo;
    if (lineNo === 1) continue;
    const line = raw.trim();
    if (line.length === 0 || line.startsWith('#')) continue;
    const space = line.indexOf(' ');
    const key = space < 0 ? line : line.slice(0, space);
    const rest = space < 0 ? '' : line.slice(space + 1).trim();

    if (key === 'entity') {
      current = { name: '', pos: [0, 0, 0], rot: [0, 0, 0], scl: [1, 1, 1] };
      inEntity = true;
      continue;
    }
    if (key === 'end') {
      if (!inEntity) { pkg.diagnostics.push(`end 出现在实体外（第 ${lineNo} 行）`); continue; }
      pkg.entities.push(current);
      current = null;
      inEntity = false;
      continue;
    }
    if (key === 'scene') { pkg.sceneName = parseQuoted(rest) ?? pkg.sceneName; continue; }
    if (key === 'env') {
      const v = parseFloats(rest, 24);
      if (v) pkg.env = v; else pkg.diagnostics.push(`env 参数不足（第 ${lineNo} 行）`);
      continue;
    }
    // ---- 序列行（顶层；实体块之后）----
    if (key === 'seqmeta') {
      const v = parseFloats(rest, 2);
      if (v) { pkg.sequences.fps = v[0] | 0; pkg.sequences.duration = v[1] | 0; }
      continue;
    }
    if (key === 'seqprop') {
      const name = parseQuoted(rest);
      const after = rest.slice(rest.indexOf('"', 1) + 1).trim().split(/\s+/);
      const track = { entityName: name, component: after[0] ?? '', property: after[1] ?? '', keys: [] };
      pkg.sequences.propertyTracks.push(track);
      seqPropTarget = track;
      continue;
    }
    if (key === 'seqkey') {
      const v = parseFloats(rest, 3);
      if (v && seqPropTarget) seqPropTarget.keys.push({ frame: v[0] | 0, value: v[1], interp: v[2] | 0 });
      continue;
    }
    if (key === 'seqpos') {
      const name = parseQuoted(rest);
      const track = { entityName: name, keys: [] };
      pkg.sequences.positionTracks.push(track);
      seqPosTarget = track;
      continue;
    }
    if (key === 'seqposkey') {
      const v = parseFloats(rest, 11);
      if (v && seqPosTarget) {
        seqPosTarget.keys.push({
          frame: v[0] | 0,
          pos: [v[1], v[2], v[3]], rot: [v[4], v[5], v[6]], scl: [v[7], v[8], v[9]],
          interp: v[10] | 0,
        });
      }
      continue;
    }

    if (!inEntity) {
      pkg.diagnostics.push(`顶层未知键 '${key}'（第 ${lineNo} 行）`);
      continue;
    }
    if (key === 'name') current.name = parseQuoted(rest) ?? current.name;
    else if (key === 'pos' || key === 'rot' || key === 'scl') {
      const v = parseFloats(rest, 3);
      if (v) current[key] = v; else pkg.diagnostics.push(`${key} 参数非数值（第 ${lineNo} 行）`);
    }
    else if (key === 'mesh' || key === 'script' || key === 'audio') {
      const path = parseQuoted(rest);
      if (path) { current[key === 'audio' ? 'audioPath' : key === 'script' ? 'scriptPath' : 'meshPath'] = path; pkg.assets.push(path); }
    }
    else if (key === 'builtin') current.builtin = rest;
    else if (key === 'tex') {
      const path = parseQuoted(rest);
      if (path) { (current.textures ??= []).push(path); pkg.assets.push(path); }
    }
    else if (key === 'light') { const v = parseFloats(rest, 9); if (v) current.light = v; }   // type/intensity/rgb/range/umbra/penumbra/enabled
    else if (key === 'camera') { const v = parseFloats(rest, 5); if (v) current.camera = v; }   // projection/fov/near/far/isMain
    else if (key === 'collider') { const v = parseFloats(rest, 9); if (v) current.collider = v; }
    else if (key === 'rigidbody') { const v = parseFloats(rest, 7); if (v) current.rigidbody = v; }
    else if (key === 'navagent') { const v = parseFloats(rest, 3); if (v) current.navagent = v; }
    else if (key === 'param') { (current.params ??= []).push({ key: rest.split(/\s+/)[0], value: parseQuoted(rest.slice(rest.indexOf(' '))) }); }
    else pkg.diagnostics.push(`实体内未知键 '${key}'（第 ${lineNo} 行）`);
  }

  if (inEntity) throw new ParseError('实体块未闭合（缺 end）', lines.length);
  return pkg;
}

/** 浏览器：fetch + parse。 */
async function fetchRuntimePackage(url) {
  const response = await fetch(url);
  if (!response.ok) throw new ParseError(`包获取失败 HTTP ${response.status}`, 0);
  return parseRuntimePackage(await response.text());
}

// Node 导出（浏览器经 <script> 引入时 window.PrimalPackage 已挂）。
if (typeof module !== 'undefined' && module.exports) {
  module.exports = { parseRuntimePackage, fetchRuntimePackage, ParseError };
}
if (typeof window !== 'undefined') {
  window.PrimalPackage = { parseRuntimePackage, fetchRuntimePackage, ParseError };
}

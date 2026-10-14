// test_package_loader.mjs — web-cloud-delivery S1 契约测试（node --test）。
// 夹具与 scripts/player_e2e_smoke.py 的 v2 包同构——两端（Player C++ /
// 浏览器 JS）解析同一格式是本模块的验收核心。

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const { parseRuntimePackage, ParseError } = require('./package_loader.js');

const fixture = [
  '# primal-runtime-scene v2',
  'scene "E2E"',
  'env 0.05 0.05 0.06 1.0 0 0 0 10 100 0 0 0.4 0.6 0.9 1 0 1.0 0 0 0 0 0 0 0',
  'entity',
  'name "sun"',
  'pos 0 10 0',
  'rot 0 0 0',
  'scl 1 1 1',
  'light 0 3.0 1 0.95 0.9 0 45 5 1',
  'end',
  'entity',
  'name "walker"',
  'pos -2 4 -2',
  'builtin box',
  'script "_assets/walk.lua"',
  'param speed "3.0"',
  'navagent 0.4 1.8 3',
  'collider 0 0.5 0.5 0.5 0 0 0 0 0',
  'rigidbody 1 1 0.1 0.5 0 0 0',
  'end',
  'entity',
  'name "cam"',
  'camera 0 60 0.1 500 1',
  'end',
  'seqmeta 60 60',
  'seqprop "sun" light intensity 2',
  'seqkey 0 1 0',
  'seqkey 59 8 0',
  'seqpos "walker" 2',
  'seqposkey 0 0 4 0 0 0 0 1 1 1 0',
  'seqposkey 59 0 1 0 0 0 0 1 1 1 0',
].join('\n');

test('参考包：场景/实体/组件/序列/资产全解析', () => {
  const pkg = parseRuntimePackage(fixture);
  assert.equal(pkg.sceneName, 'E2E');
  assert.equal(pkg.entities.length, 3);
  assert.deepEqual(pkg.entities[1].pos, [-2, 4, -2]);
  assert.equal(pkg.entities[1].builtin, 'box');
  assert.equal(pkg.entities[1].navagent[0], 0.4);
  assert.deepEqual(pkg.entities[1].collider.length, 9);
  assert.deepEqual(pkg.entities[2].camera[1], 60);
  assert.equal(pkg.assets.length, 1);                       // _assets/walk.lua
  assert.equal(pkg.assets[0], '_assets/walk.lua');
  assert.deepEqual(pkg.entities[1].params, [{ key: 'speed', value: '3.0' }]);
  assert.equal(pkg.sequences.fps, 60);
  assert.equal(pkg.sequences.propertyTracks[0].entityName, 'sun');
  assert.deepEqual(pkg.sequences.propertyTracks[0].keys[1], { frame: 59, value: 8, interp: 0 });
  assert.equal(pkg.sequences.positionTracks[0].keys[1].pos[1], 1);
  assert.equal(pkg.diagnostics.length, 0);
});

test('结构错误：缺头拒绝', () => {
  assert.throws(() => parseRuntimePackage('scene "x"'), ParseError);
});

test('结构错误：实体未闭合拒绝', () => {
  assert.throws(() => parseRuntimePackage('# primal-runtime-scene v2\nentity\nname "a"'), ParseError);
});

test('容忍语义：未知键进诊断不中断', () => {
  const pkg = parseRuntimePackage(fixture.replace(
    'builtin box', 'builtin box\nhologram 1'));
  assert.equal(pkg.entities.length, 3);
  assert.equal(pkg.diagnostics.length, 1);
  assert.match(pkg.diagnostics[0], /hologram/);
});

test('坏数值：该行进诊断，实体仍解析', () => {
  const pkg = parseRuntimePackage(fixture.replace('pos -2 4 -2', 'pos -2 four -2'));
  assert.equal(pkg.entities.length, 3);
  assert.equal(pkg.diagnostics.length, 1);
  assert.match(pkg.diagnostics[0], /pos/);
});

test('空包/非字符串：类型化拒绝', () => {
  assert.throws(() => parseRuntimePackage(''), ParseError);
  assert.throws(() => parseRuntimePackage(null), ParseError);
});

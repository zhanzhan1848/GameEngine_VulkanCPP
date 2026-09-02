#!/usr/bin/env node
// =============================================================================
// primal-mcp — MCP 桥（CLI 规划 §7.2 / Phase 2.5）
//
// 独立薄进程：stdio（MCP 传输，newline-delimited JSON-RPC 2.0）↔ primal CLI
// 双向转发。tool 定义运行时从 `primal schema`（§3.2 聚合表：editor + offline
// 两半张）自动生成——给 LLM 的说明绝不手写第二份（§7.1）。
//
// 双通道路由（§7.2）：channel=editor 的 verb 由 primal 客户端转发编辑器 socket，
// channel=offline 的 verb 由 primal 客户端路由到 primal-engine 子进程——本桥不
// 关心通道差异，统一委托给 `primal`（它已实现 §3.1 的双通道路由与退出码契约）。
// 编辑器未运行时 tool 返回明确错误（exit code 5 语义）。
//
// 任何支持 MCP 的 harness（ZCode / Claude / Cursor 等）零适配接入：
//   claude mcp add primal -- node /path/to/Tools/primal-mcp/primal-mcp.mjs
// 或在 harness 的 mcpServers 配置里：
//   { "command": "node", "args": ["/path/to/Tools/primal-mcp/primal-mcp.mjs"] }
// =============================================================================

import { spawn, spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const SCRIPT_DIR = dirname(fileURLToPath(import.meta.url));
const SERVER_VERSION = "0.1.0";
const SUPPORTED_PROTOCOL_VERSIONS = new Set(["2024-11-05", "2025-03-26", "2025-06-18"]);

// --- 定位 primal 客户端 -------------------------------------------------------
// 环境变量优先；然后按仓库布局探测（Tools/primal-mcp → Darwin/PrimalCli/<cfg>）；
// 最后 PATH。DebugEditor 是编辑器开发配置，Debug 兜底。
function locatePrimal() {
  const env = process.env.PRIMAL_BIN;
  if (env && existsSync(env)) return env;
  const repoRoot = join(SCRIPT_DIR, "..", "..");
  for (const cfg of ["DebugEditor", "ReleaseEditor", "Debug", "Release"]) {
    const candidate = join(repoRoot, "Darwin", "PrimalCli", cfg, process.platform === "win32" ? "primal.exe" : "primal");
    if (existsSync(candidate)) return candidate;
  }
  const pathHit = spawnSync(process.platform === "win32" ? "where" : "which", ["primal"], { encoding: "utf8" });
  if (pathHit.status === 0) return pathHit.stdout.trim().split(/\r?\n/)[0];
  return null;
}

let PRIMAL = locatePrimal();

// --- primal 子进程调用 --------------------------------------------------------
// 返回 { ok, exitCode, json }：json 为 stdout 最后一个完整 JSON 值（客户端契约：
// stdout 只输出响应 JSON；stderr 是人类日志，透传给 harness 的 stderr 即可省略）。
function runPrimal(args, timeoutMs = 60_000) {
  if (!PRIMAL) return { ok: false, exitCode: 5, json: null, error: "primal client not found (set PRIMAL_BIN)" };
  return new Promise((resolve) => {
    const child = spawn(PRIMAL, args, { stdio: ["ignore", "pipe", "inherit"] });
    let stdout = "";
    let settled = false;
    const timer = setTimeout(() => {
      if (!settled) { settled = true; child.kill("SIGKILL"); resolve({ ok: false, exitCode: 4, json: null, error: `timeout after ${timeoutMs}ms` }); }
    }, timeoutMs);
    child.stdout.on("data", (chunk) => { stdout += chunk; });
    child.on("error", (err) => {
      if (!settled) { settled = true; clearTimeout(timer); resolve({ ok: false, exitCode: 5, json: null, error: err.message }); }
    });
    child.on("close", (code) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      resolve({ ok: code === 0, exitCode: code ?? 4, json: extractJson(stdout) });
    });
  });
}

function extractJson(text) {
  // 客户端 stdout 可能含前置噪声；取最后一个以 { 开头的 JSON 值。
  const start = text.lastIndexOf("\n{");
  const body = start >= 0 ? text.slice(start + 1) : text;
  try { return JSON.parse(body); } catch { return null; }
}

// --- schema → MCP tools（单一事实源，运行时拉取） -----------------------------
// 编辑器未运行时（§7.2）：tools/list 仍要列出 editor 工具、tools/call 返回
// exit-5 语义的明确错误——所以成功拉到的 editor 表持久缓存于 ~/.primal/，
// 拉不到时用缓存兜底（调用自然落到 primal 的 NOT_CONNECTED → exit 5）。
const SCHEMA_CACHE = join(process.env.HOME ?? ".", ".primal", "editor-schema-cache.json");

async function fetchTools() {
  const { json } = await runPrimal(["schema"], 15_000);
  let editorTable = json?.editor ?? null;
  if (editorTable) {
    try {
      const { mkdirSync, writeFileSync } = await import("node:fs");
      mkdirSync(dirname(SCHEMA_CACHE), { recursive: true });
      writeFileSync(SCHEMA_CACHE, JSON.stringify(editorTable));
    } catch { /* 缓存失败不影响主流程 */ }
  } else {
    try {
      const { readFileSync } = await import("node:fs");
      editorTable = JSON.parse(readFileSync(SCHEMA_CACHE, "utf8"));
    } catch { /* 无缓存：编辑器工具不可列 */ }
  }

  const tools = [];
  const seen = new Set();
  const push = (cmd, channel) => {
    if (!cmd || typeof cmd.verb !== "string") return;
    // MCP tool name 约束 ^[a-zA-Z0-9_-]{1,128}$：点分 verb 的 '.' → '_'。
    const name = cmd.verb.replaceAll(".", "_").replaceAll(" ", "_");
    if (seen.has(name)) return;
    seen.add(name);
    const isOffline = channel === "offline";
    const properties = {};
    const required = [];
    for (const p of cmd.params ?? []) {
      const cleanName = String(p.name).replace(/^--/, "");
      properties[cleanName] = { type: jsonType(p.type), description: `${p.type}${p.required ? "" : " (optional)"}` };
      if (p.required) required.push(cleanName);
    }
    const lines = [
      `[${channel}] ${cmd.description ?? ""}`,
      cmd.example ? `Example params: ${cmd.example}` : "",
      `primal verb: \`${cmd.verb}\``,
    ].filter(Boolean);
    tools.push({
      name,
      description: lines.join("\n"),
      inputSchema: { type: "object", properties, required, additionalProperties: false },
      __verb: cmd.verb,
      __channel: channel,
    });
  };

  for (const cmd of editorTable?.commands ?? []) push(cmd, "editor");
  for (const cmd of json?.offline?.commands ?? []) push(cmd, "offline");

  // 离线基线门禁（§8）不在这张表里（primal 客户端内置），显式补一个入口。
  tools.push({
    name: "test_render",
    description: "[offline] 截图基线门禁：离屏渲染一帧并与 Testing/Baselines 基线做像素对比，" +
      "返回 verdict（baseline-created/identical/within-threshold/different）。回归验证的机器可读出口。",
    inputSchema: {
      type: "object",
      properties: {
        baseline: { type: "string", description: "基线 PNG 路径（默认 Testing/Baselines/offline-render-320x180.png）" },
        heatmap: { type: "string", description: "可选，输出 diff 热图 PNG 路径" },
        threshold: { type: "integer", description: "逐通道最大可容忍差值（默认 8）" },
        update_baseline: { type: "boolean", description: "预期改动时更新基线" },
      },
      additionalProperties: false,
    },
    __verb: "test render",
    __channel: "offline",
  });
  return tools;
}

function jsonType(t) {
  switch (t) {
    case "number": return "number";
    case "integer": return "integer";
    case "boolean": return "boolean";
    default: return "string"; // string/guid/vec3/enum/array → JSON 参数按宽松绑定处理
  }
}

// MCP 参数 → primal 参数。editor：params JSON 单参数；offline：flag 风格重建。
function toPrimalArgs(tool, args) {
  const verbParts = tool.__verb.split(" ");
  if (tool.__channel === "editor") {
    const jsonArgs = Object.keys(args ?? {}).length ? [JSON.stringify(args)] : [];
    return [...verbParts, ...jsonArgs];
  }
  const flags = [];
  for (const [key, value] of Object.entries(args ?? {})) {
    const flag = "--" + key.replaceAll("_", "-");
    if (typeof value === "boolean") {
      if (value) flags.push(flag); // 布尔 flag：true 出现，false 省略
    } else if (value !== undefined && value !== null) {
      flags.push(flag, String(value));
    }
  }
  return [...verbParts, ...flags];
}

// --- MCP JSON-RPC over stdio --------------------------------------------------
const sessions = { tools: null, toolsTs: 0 };

async function handleRequest(msg) {
  const { id, method, params } = msg;
  try {
    switch (method) {
      case "initialize": {
        const requested = params?.protocolVersion;
        const version = SUPPORTED_PROTOCOL_VERSIONS.has(requested) ? requested : "2024-11-05";
        return {
          id, result: {
            protocolVersion: version,
            capabilities: { tools: { listChanged: false } },
            serverInfo: { name: "primal-mcp", version: SERVER_VERSION },
          },
        };
      }
      case "ping":
        return { id, result: {} };
      case "tools/list": {
        // schema 缓存 5s：编辑器冷启动后第一问也能拿到，不至每问重拉。
        if (!sessions.tools || Date.now() - sessions.toolsTs > 5_000) {
          sessions.tools = await fetchTools();
          sessions.toolsTs = Date.now();
        }
        return {
          id, result: {
            tools: sessions.tools.map(({ __verb, __channel, ...tool }) => tool),
          },
        };
      }
      case "tools/call": {
        if (!sessions.tools) { sessions.tools = await fetchTools(); sessions.toolsTs = Date.now(); }
        const tool = sessions.tools.find((t) => t.name === params?.name);
        if (!tool) return rpcError(id, -32602, `Unknown tool: ${params?.name}`);
        const runArgs = toPrimalArgs(tool, params?.arguments);
        const timeout = tool.__channel === "editor" ? 120_000 : 300_000;
        const { exitCode, json, error } = await runPrimal(runArgs, timeout);
        const exitMeaning = { 0: "ok", 2: "usage/param error", 3: "not found", 4: "engine error", 5: "editor not running" };
        const text = json
          ? JSON.stringify(json, null, 2)
          : `${error ?? "no output"} (primal exit ${exitCode}${exitMeaning[exitCode] ? " = " + exitMeaning[exitCode] : ""})`;
        return {
          id, result: {
            content: [{ type: "text", text }],
            isError: exitCode !== 0,
          },
        };
      }
      default:
        if (method?.startsWith("notifications/")) return undefined; // 通知不回包
        return rpcError(id, -32601, `Method not found: ${method}`);
    }
  } catch (err) {
    return rpcError(id, -32603, err?.message ?? String(err));
  }
}

function rpcError(id, code, message) {
  return { id, error: { code, message } };
}

// --- 主循环：newline-delimited JSON-RPC ---------------------------------------
let buffer = "";
let pending = 0;
let stdinClosed = false;

function maybeExit() {
  if (stdinClosed && pending === 0) process.exit(0);
}

process.stdin.setEncoding("utf8");
process.stdin.on("data", (chunk) => {
  buffer += chunk;
  let newlineIndex;
  while ((newlineIndex = buffer.indexOf("\n")) >= 0) {
    const line = buffer.slice(0, newlineIndex).trim();
    buffer = buffer.slice(newlineIndex + 1);
    if (!line) continue;
    let msg;
    try { msg = JSON.parse(line); } catch { continue; } // 非法行按通知丢弃
    pending++;
    handleRequest(msg).then((response) => {
      if (response !== undefined) process.stdout.write(JSON.stringify(response) + "\n");
    }).catch((err) => {
      if (msg.id !== undefined)
        process.stdout.write(JSON.stringify(rpcError(msg.id, -32603, err?.message ?? String(err))) + "\n");
    }).finally(() => {
      pending--;
      maybeExit();
    });
  }
});
process.stdin.on("end", () => { stdinClosed = true; maybeExit(); });

if (!PRIMAL) {
  process.stderr.write("primal-mcp: 未找到 primal 客户端（设 PRIMAL_BIN 或先构建 PrimalCli）——tools/list 将为空\n");
}

#!/usr/bin/env python3
"""PrimalPlayer E2E 冒烟（release S1 验收，2026-10-14）。

手工构造最小 v2 运行时包，覆盖三件事再实跑 Player 录制：
  1. builtin 行 —— 无资产内置几何（ProceduralMeshCreate 现场生成）；
  2. 包内相对路径 —— 脚本以 `_assets/…` 形式引用（portable 解析顺序：
     包内相对 → 项目根），_assets 不在项目根下，命中包内分支即证明解析正确；
  3. 录制闭环 —— --record N 帧后进程自退，PNG 数 + 亮度 CSV 落盘。

用法：python3 scripts/player_e2e_smoke.py [player 二进制] [--frames N]
默认 player = Darwin/Debug/PrimalPlayer，默认 60 帧。退出码 0=通过，
非 0=失败（stdout 打印检查项清单）。
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def build_package(pkg_dir: Path) -> Path:
    assets = pkg_dir / "_assets"
    assets.mkdir(parents=True, exist_ok=True)
    # 旋转脚本：update 里连续转，管线 tick 生效即有日志可查（frame 计数落
    # Scripts/Log 沙箱，失败也会在 stderr 报 attach/执行错误）。input-camera
    # E2E：首次 update 校验 input 表存在且无输入时 is_down 为 false。
    # physics E2E：每 15 帧打印实体 y（"phy y=<val>"）——下落序列递减即证明
    # 物理步进 + ECS transform 回写在 Player 成立。
    (assets / "spin.lua").write_text(
        "local M = {}\n"
        "local r = 0\n"
        "local n = 0\n"
        "function M:begin_play() r = 0 n = 0 end\n"
        "function M:update(dt)\n"
        "    r = r + (params.speed or 1.0) * dt\n"
        "    n = n + 1\n"
        "    if n == 1 then\n"
        "        if input and input.is_down and not input.is_down('MoveForward') then\n"
        "            print('input-ok')\n"
        "        end\n"
        "    end\n"
        "    if n % 15 == 0 then\n"
        "        local x, y, z = entity.position()\n"
        "        print(string.format('phy y=%.3f', y))\n"
        "    end\n"
        "end\n"
        "return M\n",
        encoding="utf-8",
    )
    # ai-navigation E2E：首帧 nav.move_to(3,4,3)（速度 3）——地面盒顶 y=4，
    # walker 出生 (-2,4,-2)（避开 spinner 动态碰撞体脚印；烘焙只取静态碰撞），
    # 每 15 帧采样位置（"nav x=…"），断言递增 + 距出生点位移 ≥2m + y 恒 4。
    (assets / "walk.lua").write_text(
        "local M = {}\n"
        "function M:begin_play() M.started = false M.n = 0 end\n"
        "function M:update(dt)\n"
        "    if not M.started then\n"
        "        M.started = true\n"
        "        nav.move_to(3, 4, 3, 3)\n"        "        local ok = audio.play('_assets/beep.wav', 'sfx', 0.5, 0)\n"
        "        print('audio-play ok=' .. tostring(ok))\n"
        "    end\n"
        "    M.n = M.n + 1\n"
        "    if M.n % 15 == 0 then\n"
        "        local x, y, z = entity.position()\n"
        "        print(string.format('nav x=%.3f y=%.3f z=%.3f', x, y, z))\n"
        "    end\n"
        "end\n"
        "return M\n",
        encoding="utf-8",
    )
    # pfx Player 接通 E2E：最小 .pfx_emitter（SpawnRate 50/s + 1~2s 寿命）
    # ——统计行 "pfx active=..." 非零即证明 ParticleFX 在 Player 真正步进。
    (assets / "e2e.pfx_emitter").write_text(
        '{"name":"e2e","blend":"alpha","capacity":256,'
        '"stages":{"spawn":[{"module":"SpawnRate","rate":50}],'
        '"init":[{"module":"SetLifetime","min":1,"max":2}]}}',
        encoding="utf-8",
    )
    # audio S2 E2E：walk.lua 首帧触发 audio.play（事件播放经宿主回调）+
    # 打印结果；ground 音源改为空间声（camera@z=-6, 声源@z=0 → 增益<1 且 pan<0 偏左后）。
    # physics S2 E2E：角色移动（character.move 意图驱动 + 状态采样）
    (assets / "trooper.lua").write_text(
        "local M = {}\n"
        "function M:begin_play() M.started = false M.n = 0 end\n"
        "function M:update(dt)\n"
        "    if not M.started then\n"
        "        M.started = true\n"
        "        character.move(1, 0)\n"
        "        print('character-move ok=' .. tostring(character.state()))\n"
        "    end\n"
        "    M.n = M.n + 1\n"
        "    if M.n % 15 == 0 then\n"
        "        local x, y, z = entity.position()\n"
        "        print(string.format('chr x=%.3f state=%s', x, character.state()))\n"
        "    end\n"
        "end\n"
        "return M\n",
        encoding="utf-8",
    )
    # input-camera E2E：自定义映射文件（重绑定路径——MoveForward 同时绑 W/Up）。
    (pkg_dir / "input.bindings").write_text(
        "# primal-input-bindings v1\n"
        "action MoveForward W UpArrow\n"
        "action MoveBack S\n"
        "action MoveLeft A\n"
        "action MoveRight D\n"
        "action MoveUp E\n"
        "action MoveDown Q\n"
        "action Fast Shift\n",
        encoding="utf-8",
    )
    # audio E2E：程序化 0.5s 440Hz 正弦 WAV（PCM16 立体声 44.1kHz）——
    # Player 解析头（时长/格式）并经 AVAudioPlayer 播放（E2E 断言元信息行）。
    import math
    import struct
    import wave as wavlib
    with wavlib.open(str(assets / "beep.wav"), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(44100)
        frames = bytearray()
        for i in range(44100 // 2):
            v = int(12000 * math.sin(2 * math.pi * 440 * i / 44100))
            frames += struct.pack("<hh", v, v)
        w.writeframes(bytes(frames))
    # physics E2E：动态方块（含脚本采样 y 打印）落到静态地面——
    # y 序列递减即证明 PipelineTick 的物理段在 Player 真正步进且回写实体。
    pkg = pkg_dir / "e2e.primalrun"
    pkg.write_text(
        "# primal-runtime-scene v2\n"
        'scene "E2E"\n'
        "env 0.05 0.05 0.06 1.0 0 0 0 10 100 0 0 0.4 0.6 0.9 1 0 1.0 0 0 0 0 0 0 0\n"
        "entity\n"
        'name "sun"\n'
        "pos 0 10 0\nrot 0 0 0\nscl 1 1 1\n"
        "light 0 3.0 1 0.95 0.9 0 45 5 1\n"
        "end\n"
        "entity\n"
        'name "ground"\n'
        "pos 0 -1 0\nrot 0 0 0\nscl 1 1 1\n"
        "builtin plane\n"
        "collider 0 5 5 5 0 0 0 0 0\n"
        'pfx "_assets/e2e.pfx_emitter"\n'
        'audio "_assets/beep.wav" 0.8 0 1 1\n'
        'audiogroup "ambient"\n'
        "end\n"
        "entity\n"
        'name "spinner"\n'
        "pos 0 4 0\nrot 0 0 0\nscl 1 1 1\n"
        "builtin box\n"
        'script "_assets/spin.lua"\n'
        'param speed "2.0"\n'
        "collider 0 0.5 0.5 0.5 0 0 0 0 0\n"
        "rigidbody 1 1 0.1 0.5 0 0 0\n"
        "end\n"
        "entity\n"
        'name "walker"\n'
        "pos -2 4 -2\nrot 0 0 0\nscl 1 1 1\n"
        "builtin box\n"
        'script "_assets/walk.lua"\n'
        "navagent 0.4 1.8 3\n"
        "end\n"
        "entity\n"
        'name "post"\n'
        "pos 2 6 2\nrot 0 0 0\nscl 1 1 1\n"
        "builtin box\n"
        "collider 0 0.25 1 0.25 0 0 0 0 0\n"
        "end\n"
        "entity\n"
        'name "door"\n'
        "pos 3 6 2\nrot 0 0 0\nscl 1 1 1\n"
        "builtin box\n"
        "collider 0 0.5 0.25 0.25 0 0 0 0 0\n"
        "rigidbody 1 1 0.05 0.5 0 0 0\n"
        "end\n"
        "entity\n"
        'name "trooper"\n'
        "pos -4 4.9 -4\nrot 0 0 0\nscl 1 1 1\n"
        "builtin box\n"
        'script "_assets/trooper.lua"\n'
        "character 0.4 1.8 4 80\n"
        "end\n"
        "entity\n"
        'name "mover"\n'
        "pos 0 4 0\nrot 0 0 0\nscl 1 1 1\n"
        "builtin box\n"
        "end\n"
        "entity\n"
        'name "cam"\n'
        "pos 0 1.5 -6\nrot 0 0 0\nscl 1 1 1\n"
        "camera 0 60 0.1 500 1\n"
        "end\n"
        # sequence S1 E2E：sun.intensity 1→8 线性（属性轨）+ mover y 4→1（变换轨）
        # audio S2：混音分组注册（sfx=1.0；ambient=0.6——ground 空间声源）
        'audiomix "sfx" 1.0\n'
        'audiomix "ambient" 0.6\n'
        'audiomix "master" 1.0\n'
        "seqmeta 60 60\n"
        'seqprop "sun" light intensity 2\n'
        "seqkey 0 1 0\n"
        "seqkey 59 8 0\n"
        'seqpos "mover" 2\n'
        "seqposkey 0 0 4 0 0 0 0 1 1 1 0\n"
        "seqposkey 59 0 1 0 0 0 0 1 1 1 0\n"
        # sequence S2 E2E：事件轨（帧 10/30 各一——恰一次断言）
        "seqevent 2\n"
        "seqeventkey 10 \"mover-halfway\"\n"
        "seqeventkey 30 \"mover-low\"\n"
        # physics S2 E2E：门板铰链（重力下摆被限位钳制；角度采样断言）
        'joint "post" "door" 0 0 1 2 6 2 -80 80 0 0 0\n',
        encoding="utf-8",
    )
    return pkg


def main() -> int:
    player = Path(sys.argv[1]) if len(sys.argv) > 1 and not sys.argv[1].startswith("--") \
        else REPO / "Darwin" / "Debug" / "PrimalPlayer"
    frames = 60
    if "--frames" in sys.argv:
        frames = int(sys.argv[sys.argv.index("--frames") + 1])
    if not player.exists():
        print(f"FAIL: player 不存在: {player}")
        return 1

    with tempfile.TemporaryDirectory(prefix="primal_e2e_") as tmp:
        pkg = build_package(Path(tmp))
        record = Path(tmp) / "record"
        record.mkdir()
        # 注意：包必须在 .Primal/runtime 两级下？不需要——包内相对优先分支
        # 不依赖项目根；即使三级上溯失败，_assets 命中即通过。
        proc = subprocess.run(
            [str(player), "--scene", str(pkg),
             "--record", str(record), "--record-frames", str(frames),
             "--record-metrics", str(record / "luma.csv")],
            capture_output=True, text=True, timeout=180,
            cwd=str(player.parent))
        log = proc.stderr
        # 已知引擎侧移交项（Docs/completed/2026-10-09-runtime-chain-*.md）：
        # RHIAllocator MetalRenderPass 泄漏 → 退出 SIGABRT(134)，但帧序列完整
        # （RuntimeLauncher 同一豁免契约）。真失败 = 非零退出且无该签名。
        known_leak_abort = proc.returncode in (-6, 134) and "RHIAllocator" in log
        checks = {
            f"player 退出(0 或已知 RHI 泄漏 abort, rc={proc.returncode})":
                proc.returncode == 0 or known_leak_abort,
            "builtin 几何生成日志": "builtin geometry 'plane' created" in log
                                  and "builtin geometry 'box' created" in log,
            "脚本挂载日志": "script attached" in log,
            "input 映射加载": "input bindings loaded" in log,
            "Lua input 表可用": "input-ok" in proc.stdout,
            "录制 PNG 数量": len(list(record.glob("frame_*.png"))) == frames,
            "亮度 CSV 存在": (record / "luma.csv").exists()
                            and sum(1 for _ in open(record / "luma.csv")) >= frames,
            "无 mesh/纹理失败": "ImportMeshAsync rejected" not in log
                              and "texture import failed" not in log,
            "无脚本执行错误": "script error" not in log.lower()
                            and "script attach failed" not in log,
        }
        # physics：stdout 的 "phy y=" 样本序列须递减（下落）
        ys = [float(m) for m in
              __import__("re").findall(r"phy y=([-\d.]+)", proc.stdout)]
        checks["物理步进（y 递减）"] = len(ys) >= 2 and all(
            ys[i + 1] < ys[i] for i in range(len(ys) - 1))
        checks["物理开启日志"] = "physics running=1" in log
        # audio：AVAudioPlayer 播放 + WAV 头解析（0.5s PCM 立体声 44100Hz）
        checks["音频播放（WAV 解析+平台播放）"] = (
            "audio playing" in log and "PCM 44100Hz 2ch 0.50s" in log
            and "vol=0.80" in log)
        # ai-navigation：烘焙日志 + walker 沿路径推进（x/z 递增、贴地 y=4、位移≥2m）
        nav_samples = [(float(a), float(b), float(c)) for a, b, c in
                       __import__("re").findall(r"nav x=([-\d.]+) y=([-\d.]+) z=([-\d.]+)", proc.stdout)]
        checks["导航烘焙日志"] = "navigation baked" in log
        checks["导航采样数"] = len(nav_samples) >= 3
        if nav_samples:
            xs = [q[0] for q in nav_samples]
            zs = [q[2] for q in nav_samples]
            ys = [q[1] for q in nav_samples]
            # 到达后样本持平（3.000, 3.000…）——判"不减 + 净前进"而非严格递增
            checks["导航推进（x/z 前进）"] = (
                all(xs[i + 1] >= xs[i] for i in range(len(xs) - 1))
                and all(zs[i + 1] >= zs[i] for i in range(len(zs) - 1))
                and xs[-1] > xs[0] and zs[-1] > zs[0])
            checks["导航贴地（y≈4）"] = all(abs(y - 4.0) < 0.26 for y in ys)
            disp = ((nav_samples[-1][0] + 2) ** 2 + (nav_samples[-1][2] + 2) ** 2) ** 0.5
            checks["导航位移（%.2fm≥2m）" % disp] = disp >= 2.0
        else:
            checks["导航推进（x/z 递增）"] = False
            checks["导航贴地（y≈4）"] = False
            checks["导航位移（≥2m）"] = False
        # sequence S1：属性轨（sun.intensity 1→8 线性）+ 变换轨（mover y 4→1）
        seq_frames = [int(m) for m in __import__("re").findall(r"seq f=(\d+)", proc.stdout)]
        seq_intensity = [float(m) for m in
                         __import__("re").findall(r"light\.intensity=([\d.]+)", proc.stdout)]
        seq_posy = [float(m) for m in __import__("re").findall(r"pos\.y=([-\d.]+)", proc.stdout)]
        checks["序列采样数"] = len(seq_frames) >= 3
        checks["序列强度递增"] = (
            len(seq_intensity) >= 3
            and all(seq_intensity[i + 1] > seq_intensity[i] for i in range(len(seq_intensity) - 1))
            and 6.0 <= seq_intensity[-1] <= 8.5)
        # audio S2：事件触发播放（Lua audio.play）+ 空间声增益/声像采样
        audio_play_ok = "audio-play ok=true" in proc.stdout
        spatial_gains = [float(m) for m in
                         __import__("re").findall(r"audio spatial gain=([\d.]+)", proc.stdout)]
        checks["Audio 事件播放（Lua audio.play）"] = audio_play_ok
        checks["Audio 空间声采样"] = len(spatial_gains) >= 2
        checks["Audio 空间声增益<1（衰减生效）"] = (
            len(spatial_gains) >= 2 and 0.01 <= max(spatial_gains) < 0.99)
        # physics S2：角色移动（意图驱动 x 递增 + 状态 moving）
        chr_x = [float(m) for m in __import__("re").findall(r"chr x=([\d.-]+)", proc.stdout)]
        chr_states = __import__("re").findall(r"chr x=[\d.-]+ state=(\w+)", proc.stdout)
        checks["Character 创建日志"] = "character created" in log
        checks["Character 移动采样"] = len(chr_x) >= 3
        checks["Character x 递增"] = (
            len(chr_x) >= 3 and all(chr_x[i + 1] > chr_x[i] for i in range(len(chr_x) - 1)))
        checks["Character 状态 moving"] = len(chr_states) >= 2 and "moving" in chr_states
        # sequence S2：事件恰一次（各事件出现次数==1）
        evt_halfway = proc.stdout.count("seq event mover-halfway@10")
        evt_low = proc.stdout.count("seq event mover-low@30")
        checks["序列事件 mover-halfway 恰一次"] = evt_halfway == 1
        checks["序列事件 mover-low 恰一次"] = evt_low == 1
        # physics S2：关节创建日志 + 角度采样（门板在重力下摆动被限位钳制）
        joint_angles = [float(m) for m in
                        __import__("re").findall(r"joint post-door angle=([\d.-]+)", proc.stdout)]
        checks["Joint 关节创建"] = "joint" in log and "created" in log
        checks["Joint 角度采样"] = len(joint_angles) >= 2
        checks["Joint 门板摆动（|角度|>5°）"] = (
            len(joint_angles) >= 2 and max(abs(a) for a in joint_angles) > 5.0)
        checks["Joint 限位钳制（|角度|≤85°）"] = (
            len(joint_angles) >= 2 and max(abs(a) for a in joint_angles) <= 85.0)
        # pfx：发射器创建日志 + 粒子统计非零（系统在跑）
        pfx_active = [int(m) for m in __import__("re").findall(r"pfx active=(\d+)", proc.stdout)]
        checks["PFX 发射器创建"] = "pfx emitter" in log and "created" in log
        checks["PFX 粒子活跃"] = len(pfx_active) >= 2 and max(pfx_active) > 0
        checks["序列位移递减"] = (
            len(seq_posy) >= 3
            and all(seq_posy[i + 1] < seq_posy[i] for i in range(len(seq_posy) - 1))
            and seq_posy[0] >= 3.9 and seq_posy[-1] <= 1.9)
        ok = True
        for name, passed in checks.items():
            print(f"{'PASS' if passed else 'FAIL'}: {name}")
            ok = ok and passed
        if not ok:
            print("--- stderr tail ---")
            print("\n".join(log.splitlines()[-40:]))
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

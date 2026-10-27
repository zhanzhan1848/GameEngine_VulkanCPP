# gameplay S1 Player 冒烟夹具（手动）

窗口式冒烟（不进 ctest；无头自动化由 TestGameplayE2E 覆盖同一 C ABI 路径）：

    cp -r EngineTest/GameplayTests/fixtures/player_smoke /tmp/gp_smoke
    Darwin/Debug/PrimalPlayer --scene /tmp/gp_smoke/scene.primalrun
    # ~5 秒后 Ctrl-C；验证：/tmp/gp_smoke/ScriptsSave/door_opened.txt 存在
    #（Player 进程 cwd 须在包目录——ScriptsSave 沙箱根按 cwd 解析）。

包内容：seqpos 驱动 Player 实体 10→0→20 往返；Button 上 `dist self Player lt 2`
触发器发 button.near；Door 机器 closed --manual--> open --guard dist gte 5--> closed；
door.lua 受控转移 + 开门位移。已知无关噪声：mode 3 Metal 黑帧（既有移交项）、
RHI 泄漏 abort（kill -9 路径）。

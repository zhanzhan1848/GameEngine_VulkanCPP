#pragma once

// Minimal AppKit window for PrimalPlayer (plan §Run/Stop Phase 1; 输入桥
// 2026-10-09 关闭原留痕①的 Esc 缺口). The engine surface attaches to the
// returned NSView via CreateRenderSurface(host, w, h); the event loop pumps
// manually so the render tick stays on the main thread (same constraint the
// editor's DispatcherTimer render loop documents).

#ifdef __cplusplus
extern "C" {
#endif

// 每帧输入快照（PlayerWindowPollInput 拷出后清零累计量；按键位集是
// "当前按住"状态而非事件累计）。
typedef struct PlayerInputState {
    unsigned keys;   // kPlayerKey* 位集
    float look_dx;   // 自上次 poll 的拖拽/方向键视角增量（度，右/下为正）
    float look_dy;
    float scroll_dy; // 滚轮增量（速度调节）
    int dragging;    // 当前是否按住拖拽
} PlayerInputState;

enum PlayerInputKey {
    kPlayerKeyW = 1u << 0,
    kPlayerKeyA = 1u << 1,
    kPlayerKeyS = 1u << 2,
    kPlayerKeyD = 1u << 3,
    kPlayerKeyQ = 1u << 4,
    kPlayerKeyE = 1u << 5,
    kPlayerKeyShift = 1u << 6,
    kPlayerKeyUpArrow = 1u << 7,
    kPlayerKeyDownArrow = 1u << 8,
    kPlayerKeyLeftArrow = 1u << 9,
    kPlayerKeyRightArrow = 1u << 10,
};

// Creates the application, a titled window and its content view.
// Returns the content NSView pointer (for CreateRenderSurface's host),
// or null on failure. w/h are the initial content size. resizable=false
// 去掉窗口的 Resizable 样式（--fixed：固定分辨率录制/比对要求显示
// 分辨率与渲染分辨率严格一致）。
void* PlayerWindowCreate(int width, int height, const char* title, int resizable);

// Pumps NSApp events. Returns 0 once the user asked to close (close box
// 或 Esc——输入视图 keyDown 里置位同一标志), non-zero while the window
// should stay up.
int PlayerWindowPump(void);

// 拷出并消费本帧输入（累计量清零）。无窗口平台不链接本符号——调用方
// 只在 windowed 路径使用。
void PlayerWindowPollInput(PlayerInputState* out);

// audio S1（2026-10-14）：播放一个音频文件（macOS AVAudioPlayer）。
// loop=1 → 无限循环；音量 0..1。返回 1=已开始播放，0=失败（文件缺/格式
// 不支持——stderr 留痕）。多次调用叠加（多音源）；PlayerWindowDestroy
// 停止全部。非 Apple 构建不链接本符号——调用方按平台守卫。
int PlayerAudioPlayFile(const char* path, float volume, int loop);
// audio S2：空间声源（带声像）+ 活动声源每帧 volume/pan 更新
int PlayerAudioPlaySpatial(const char* path, float volume, float pan, int loop);
int PlayerAudioVoiceCount(void);
void PlayerAudioUpdateVoice(int index, float volume, float pan);

// Tears the window down. Call before ShutdownEngine.
void PlayerWindowDestroy(void);

#ifdef __cplusplus
}
#endif

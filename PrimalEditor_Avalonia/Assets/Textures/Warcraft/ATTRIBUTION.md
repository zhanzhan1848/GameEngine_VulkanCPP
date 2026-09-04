# Warcraft UI 纹理来源与处理说明

来源：[warcraftcn-ui](https://github.com/TheOrcDev/warcraftcn-ui) 仓库
`public/warcraftcn/` 目录（MIT License，见该仓库 `license.md`）。

本目录全部图片在原始资产基础上做了二次加工（`Pillow` 脚本）：

1. **清晰化**：LANCZOS 重采样 + UnsharpMask 锐化；
2. **金色化**：`button-*.png` 对蓝色系像素做了选择性色相偏移（蓝→黄铜金），
   使按钮与主题的锻金配色一致；
3. **做旧**：轻微去饱和（~0.86）、压暗（~0.90）、降对比、暖色通道偏移，
   并叠加细颗粒噪点（grain）模拟使用痕迹；
4. hover 态 = normal 提亮 18%，pressed 态 = normal 压暗 18% 并去饱和；
5. **九宫格适配**：`frame-panel.png` 裁掉了源图四周的透明边距和顶部铰链
   扣件（凸出物会破坏九宫格切片），得到四边均匀的矩形框，切片参数与
   `Themes/Warcraft/WarcraftTheme.Styles.axaml` 中各 tex-* 类的
   Slice/Scale 一一对应——修改图片后记得同步这些数值。

| 文件 | 原始资产 | 用途 |
| --- | --- | --- |
| button-normal / hover / pressed.png | button-bg.webp | 普通按钮九宫格（三态） |
| button-accent.png / button-accent-hover.png | button-bg-with-frame.webp | 强调按钮（`Classes="accent"`）九宫格 |
| frame-strip.png | input-frame.webp | 细铁边条：菜单栏、工具条、状态条 |
| node-frame-orc/human/elf/undead/default.png | button-bg-with-frame.webp | 材质编辑器节点外框(按阵营色相偏移：兽人红/人类蓝/精灵青/亡灵紫/中立金；内环挖空,叠加于节点边界之外) |
| frame-panel.png | card-bg.webp | 木纹大面板：层级/检查器/文件/日志等 |

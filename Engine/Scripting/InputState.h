#pragma once

// input-camera S1（2026-10-14）：宿主输入动作状态桥。
// 宿主（Player/编辑器）把原始键位经"动作映射"翻译成语义动作名写入本单例，
// 脚本经 Lua `input.is_down("<动作名>")` 读取——脚本不感知具体键位，
// 重绑定（改映射文件）对脚本透明。
//
// 线程契约：主线程单线程序列——宿主在 PipelineTick 前写入、tick 中脚本读。
// 与 LuaBackend 同层（Engine/Scripting），不碰 Components/game_entity，
// 也不触碰任何图形路径。

#include <string>
#include <unordered_map>

namespace primal::scripting {

class InputState {
public:
    static InputState& instance()
    {
        static InputState s;
        return s;
    }

    /// 宿主每帧写入动作状态（down=true 按住）。动作名区分大小写、
    /// 不做规范化——映射双方（宿主映射表/Lua 脚本）使用同一字符串。
    void set(const std::string& action, bool down)
    {
        actions_[action] = down;
    }

    /// 脚本查询。未注册动作恒 false（不是错误——动作集是项目定义的）。
    bool is_down(const std::string& action) const
    {
        const auto it = actions_.find(action);
        return it != actions_.end() && it->second;
    }

    /// 进程/场景重置时清空（宿主关闭或输入源重建）。
    void clear() { actions_.clear(); }

    std::size_t registered_count() const { return actions_.size(); }

private:
    InputState() = default;
    std::unordered_map<std::string, bool> actions_;
};

} // namespace primal::scripting

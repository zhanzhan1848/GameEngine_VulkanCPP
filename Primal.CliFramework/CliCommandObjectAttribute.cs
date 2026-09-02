using System;

namespace Primal.CliFramework;

/// <summary>
/// 把一个 POCO 标记为 [CliCommand] 方法参数的合法 DTO 类型（schema 里显示为
/// "object"，绑定走 System.Text.Json 标准反序列化）。
/// 框架对参数类型采用白名单 fail-fast（规划 §4.3）：结构漂移必须以编译期/
/// 注册期的显式动作出现，而不是静默通过。新增参数 DTO = 声明字段 + 本 attribute，
/// 同一次提交完成。
/// </summary>
[AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct, AllowMultiple = false)]
public sealed class CliCommandObjectAttribute : Attribute
{
}

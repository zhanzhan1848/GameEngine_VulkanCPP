using UnrealBuildTool;
public class LumenCompareCapture : ModuleRules
{
    public LumenCompareCapture(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "RenderCore", "RHI", "Json" });
    }
}

// M6.3: Virtual Texture 资产检查器 ViewModel
// 数据源:Engine/Graphics/VirtualTexture/VirtualTextureSystem(vt_stats 经
// P/Invoke 或引擎侧查询接口桥接 — C# 侧仅做展示;当前引擎 DLL 未导出
// VT 查询,先用快照结构 + 轮询占位,引擎导出后无缝切换)。
using System;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace PrimalEditor_Avalonia.ViewModels
{
    /// <summary>VT 系统统计快照(与 C++ vt_stats 对应)</summary>
    public struct VTStatsSnapshot
    {
        public uint Registered;
        public uint ResidentPages;
        public uint PendingRequests;
        public uint PoolPagesUsed;
        public uint PoolPagesTotal;
        public uint EvictionsTotal;
        public uint ProducerPagesProduced;
        public uint ProducerCacheHits;
        public uint FeedbackPageHits;
        public double HitRate;
    }

    /// <summary>单个 VT 资产条目(检查器列表行)</summary>
    public class VTAssetEntry : INotifyPropertyChanged
    {
        private string _name = "";
        private uint _vtId;
        private uint _pagesTop;
        private string _ptOrigin = "";
        private uint _residentPages;
        private uint _totalPages;
        private string _avgColor = "";

        public string Name { get => _name; set => Set(ref _name, value); }
        public uint VtId { get => _vtId; set => Set(ref _vtId, value); }
        public uint PagesTop { get => _pagesTop; set => Set(ref _pagesTop, value); }
        public string PtOrigin { get => _ptOrigin; set => Set(ref _ptOrigin, value); }
        public uint ResidentPages { get => _residentPages; set => Set(ref _residentPages, value); }
        public uint TotalPages { get => _totalPages; set => Set(ref _totalPages, value); }
        /// <summary>驻留进度文本(如 "4/5")</summary>
        public string Residency => $"{_residentPages}/{_totalPages}";
        /// <summary>平均色(hex,#RRGGBB)</summary>
        public string AvgColor { get => _avgColor; set => Set(ref _avgColor, value); }

        public event PropertyChangedEventHandler? PropertyChanged;
        private void Set<T>(ref T field, T value, [CallerMemberName] string? name = null)
        {
            if (Equals(field, value)) return;
            field = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
            if (name == nameof(ResidentPages) || name == nameof(TotalPages))
                PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Residency)));
        }
    }

    /// <summary>VT 检查器主 ViewModel:统计面板 + 资产列表 + 页表/池占用摘要</summary>
    public class VirtualTextureInspectorViewModel : ViewModelBase
    {
        public ObservableCollection<VTAssetEntry> Assets { get; } = new();

        private VTStatsSnapshot _stats;
        private string _poolSummary = "";
        private string _hitRateSummary = "";

        public uint Registered => _stats.Registered;
        public uint ResidentPages => _stats.ResidentPages;
        public uint PendingRequests => _stats.PendingRequests;
        public uint EvictionsTotal => _stats.EvictionsTotal;
        public string PoolSummary
        {
            get => _poolSummary;
            private set => RaiseAndSetIfChanged(ref _poolSummary, value);
        }
        public string HitRateSummary
        {
            get => _hitRateSummary;
            private set => RaiseAndSetIfChanged(ref _hitRateSummary, value);
        }

        /// <summary>引擎侧每帧(或定时器)推送快照</summary>
        public void UpdateStats(in VTStatsSnapshot snapshot)
        {
            _stats = snapshot;
            PoolSummary = $"池占用 {_stats.PoolPagesUsed}/{_stats.PoolPagesTotal}" +
                          $" ({(_stats.PoolPagesTotal > 0 ? 100.0 * _stats.PoolPagesUsed / _stats.PoolPagesTotal : 0):F1}%)";
            HitRateSummary = $"命中率 {_stats.HitRate:P1}" +
                             $"(prefetch 命中 {_stats.ProducerCacheHits},feedback 命中 {_stats.FeedbackPageHits})";
            OnPropertyChanged(nameof(Registered));
            OnPropertyChanged(nameof(ResidentPages));
            OnPropertyChanged(nameof(PendingRequests));
            OnPropertyChanged(nameof(EvictionsTotal));
        }

        /// <summary>添加/更新资产条目(注册或驻留变化时由引擎侧调用)</summary>
        public void UpsertAsset(uint vtId, string name, uint pagesTop,
                                uint ptOriginX, uint ptOriginY,
                                uint resident, uint total, uint avgColor)
        {
            foreach (var a in Assets)
            {
                if (a.VtId != vtId) continue;
                a.Name = name;
                a.PagesTop = pagesTop;
                a.PtOrigin = $"({ptOriginX},{ptOriginY})";
                a.ResidentPages = resident;
                a.TotalPages = total;
                a.AvgColor = $"#{(avgColor >> 16) & 0xFF:X2}{(avgColor >> 8) & 0xFF:X2}{avgColor & 0xFF:X2}";
                return;
            }
            Assets.Add(new VTAssetEntry
            {
                VtId = vtId, Name = name, PagesTop = pagesTop,
                PtOrigin = $"({ptOriginX},{ptOriginY})",
                ResidentPages = resident, TotalPages = total,
                AvgColor = $"#{(avgColor >> 16) & 0xFF:X2}{(avgColor >> 8) & 0xFF:X2}{avgColor & 0xFF:X2}",
            });
        }

        public void RemoveAsset(uint vtId)
        {
            for (int i = Assets.Count - 1; i >= 0; --i)
                if (Assets[i].VtId == vtId) Assets.RemoveAt(i);
        }
    }
}

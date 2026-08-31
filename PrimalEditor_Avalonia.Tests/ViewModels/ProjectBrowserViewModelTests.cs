using System.Linq;
using PrimalEditor_Avalonia.ViewModels;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.ViewModels;

public sealed class ProjectBrowserViewModelTests
{
    [Fact]
    public void DefaultSelectedTab_IsOpen()
    {
        var vm = new ProjectBrowserViewModel();

        Assert.Equal(ProjectBrowserTab.Open, vm.SelectedTab);
    }

    [Fact]
    public void OpenProjectViewModel_IsExposed()
    {
        var vm = new ProjectBrowserViewModel();

        Assert.NotNull(vm.OpenProject);
    }

    [Fact]
    public void NewProjectViewModel_IsExposed()
    {
        var vm = new ProjectBrowserViewModel();

        Assert.NotNull(vm.NewProject);
    }

    [Fact]
    public void SelectedTab_RaisesPropertyChanged()
    {
        var vm = new ProjectBrowserViewModel();
        var changed = false;
        vm.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(ProjectBrowserViewModel.SelectedTab))
            {
                changed = true;
            }
        };

        vm.SelectedTab = ProjectBrowserTab.New;

        Assert.True(changed);
        Assert.Equal(ProjectBrowserTab.New, vm.SelectedTab);
    }

    [Fact]
    public void IsOpenSelected_IsTrueByDefault()
    {
        var vm = new ProjectBrowserViewModel();

        Assert.True(vm.IsOpenSelected);
        Assert.False(vm.IsNewSelected);
    }

    [Fact]
    public void IsNewSelected_ReflectsSelectedTab()
    {
        var vm = new ProjectBrowserViewModel();

        vm.SelectedTab = ProjectBrowserTab.New;

        Assert.False(vm.IsOpenSelected);
        Assert.True(vm.IsNewSelected);
    }

    [Fact]
    public void IsOpenSelected_NotifiesOnTabSwitch()
    {
        var vm = new ProjectBrowserViewModel();
        var notified = false;
        vm.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(ProjectBrowserViewModel.IsOpenSelected))
            {
                notified = true;
            }
        };

        vm.SelectedTab = ProjectBrowserTab.New;

        Assert.True(notified);
    }

    [Fact]
    public void SettingSameSelectedTab_DoesNotFire()
    {
        var vm = new ProjectBrowserViewModel();
        var count = 0;
        vm.PropertyChanged += (_, _) => count++;

        vm.SelectedTab = ProjectBrowserTab.Open;

        Assert.Equal(0, count);
    }
}
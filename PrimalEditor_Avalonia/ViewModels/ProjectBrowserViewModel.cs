namespace PrimalEditor_Avalonia.ViewModels;

public enum ProjectBrowserTab
{
    Open,
    New,
}

public class ProjectBrowserViewModel : ViewModelBase
{
    private ProjectBrowserTab _selectedTab = ProjectBrowserTab.Open;

    public OpenProjectViewModel OpenProject { get; } = new();

    public NewProjectViewModel NewProject { get; } = new();

    public ProjectBrowserTab SelectedTab
    {
        get => _selectedTab;
        set
        {
            if (_selectedTab == value) return;
            _selectedTab = value;
            OnPropertyChanged(nameof(SelectedTab));
            OnPropertyChanged(nameof(IsOpenSelected));
            OnPropertyChanged(nameof(IsNewSelected));
        }
    }

    public bool IsOpenSelected => _selectedTab == ProjectBrowserTab.Open;
    public bool IsNewSelected => _selectedTab == ProjectBrowserTab.New;
}
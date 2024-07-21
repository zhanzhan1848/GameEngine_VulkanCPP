using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Navigation;
using System.Windows.Shapes;

namespace PrimalEditor.Content
{
    /// <summary>
    /// ConfigureGeometryImportSettingView.xaml 的交互逻辑
    /// </summary>
    public partial class ConfigureGeometryImportSettingView : UserControl
    {

        private void OnImport_Button_Click(object sender, RoutedEventArgs e)
        {
            ((sender as FrameworkElement).DataContext as GeometryImportSettingsConfigurator).Import();
        }

        private void OnApplyToSelection_Button_Click(object sender, RoutedEventArgs e)
        {
            var setting = ((sender as FrameworkElement).DataContext as GeometryProxy).ImportSettings;
            var selection = geometryListBox.SelectedItems;
            foreach(GeometryProxy proxy in selection)
            {
                proxy.CopySettings(setting);
            }
        }

        private void OnApplyToAll_Button_Click(object sender, RoutedEventArgs e)
        {
            var setting = ((sender as FrameworkElement).DataContext as GeometryProxy).ImportSettings;
            var vm = DataContext as ConfigureImportSettings;
            foreach (var proxy in vm.GeometryImportSettingsConfigurator.GeometryProxies)
            {
                proxy.CopySettings(setting);
            }
        }

        private void OnListBox_Drop(object sender, DragEventArgs e)
        {
            ConfigureImportSettingsWindow.AddDroppedFiles(DataContext as ConfigureImportSettings, sender as ListBox, e);
        }

        private void OnRemove_Button_Click(object sender, RoutedEventArgs e)
        {
            var vm = DataContext as ConfigureImportSettings;
            vm.GeometryImportSettingsConfigurator.RemoveFiles((sender as FrameworkElement).DataContext as GeometryProxy);
        }

        private void OnClearImportingItems_Button_Click(object sender, RoutedEventArgs e)
        {
            ImportingItemCollection.Clear(AssetType.Animation);
            ImportingItemCollection.Clear(AssetType.Material);
            ImportingItemCollection.Clear(AssetType.Mesh);
            ImportingItemCollection.Clear(AssetType.Skeleton);
        }

        public ConfigureGeometryImportSettingView()
        {
            InitializeComponent();

            Loaded += (_, _) =>
            {
                var item = geometryListBox.ItemContainerGenerator
                    .ContainerFromIndex(geometryListBox.SelectedIndex) as ListBoxItem;
                item?.Focus();
            };
        }
    }
}

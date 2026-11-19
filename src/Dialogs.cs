// 回收站窗口、设置窗口
using System;
using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace DeskNote
{
    static class Ui
    {
        public static readonly FontFamily Font = new FontFamily("Microsoft YaHei UI");
        public static readonly Brush Muted = new SolidColorBrush(Color.FromRgb(0x77, 0x77, 0x77));
        public static readonly Brush Line = new SolidColorBrush(Color.FromRgb(0xE4, 0xE4, 0xE4));

        public static Button Btn(string text, Action click)
        {
            var b = new Button { Content = text, Padding = new Thickness(12, 3, 12, 3), Margin = new Thickness(6, 0, 0, 0), MinWidth = 64 };
            b.Click += delegate { click(); };
            return b;
        }
    }

    // ---------------------------------------------------------------------
    // 回收站：查看、恢复、彻底删除；不自动清理
    // ---------------------------------------------------------------------
    class TrashWindow : Window
    {
        readonly DeskApp app;
        readonly StackPanel list = new StackPanel();
        readonly Button emptyBtn;

        public TrashWindow(DeskApp app)
        {
            this.app = app;
            Title = "DeskNote 回收站";
            Width = 560; Height = 540;
            MinWidth = 380; MinHeight = 300;
            WindowStartupLocation = WindowStartupLocation.CenterScreen;
            FontFamily = Ui.Font;
            FontSize = 13.5;

            var root = new DockPanel { Margin = new Thickness(16, 14, 16, 14) };
            var header = new TextBlock
            {
                Text = "删掉的条目和便签都在这里，不会自动清理。恢复的条目会放回原来那张便签的原位置。",
                TextWrapping = TextWrapping.Wrap, Foreground = Ui.Muted, Margin = new Thickness(0, 0, 0, 10)
            };
            DockPanel.SetDock(header, Dock.Top);
            root.Children.Add(header);

            var bottom = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 10, 0, 0) };
            emptyBtn = Ui.Btn("清空回收站", delegate
            {
                var r = MessageBox.Show(this, "彻底删除回收站里的全部 " + app.Trash.Count + " 项？删除后不能恢复。", "清空回收站",
                    MessageBoxButton.OKCancel, MessageBoxImage.Warning);
                if (r == MessageBoxResult.OK) app.EmptyTrash();
            });
            bottom.Children.Add(emptyBtn);
            bottom.Children.Add(Ui.Btn("关闭", Close));
            DockPanel.SetDock(bottom, Dock.Bottom);
            root.Children.Add(bottom);

            root.Children.Add(new Border
            {
                BorderBrush = Ui.Line, BorderThickness = new Thickness(1),
                Child = new ScrollViewer { Content = list, VerticalScrollBarVisibility = ScrollBarVisibility.Auto }
            });
            Content = root;

            Action refresh = Refresh;
            app.TrashChanged += refresh;
            Closed += delegate { app.TrashChanged -= refresh; };
            Refresh();
        }

        void Refresh()
        {
            list.Children.Clear();
            emptyBtn.IsEnabled = app.Trash.Count > 0;
            if (app.Trash.Count == 0)
            {
                list.Children.Add(new TextBlock { Text = "回收站是空的", Foreground = Ui.Muted, Margin = new Thickness(14), HorizontalAlignment = HorizontalAlignment.Center });
                return;
            }
            foreach (var e in app.Trash.ToArray())
            {
                TrashEntry entry = e;
                string main, sub;
                if (e.Kind == "note")
                {
                    main = "便签「" + e.Note.Title + "」";
                    sub = "整张便签 · " + e.Note.Items.Count + " 条";
                }
                else
                {
                    main = e.Item.Text;
                    sub = "来自「" + e.NoteTitle + "」" + (e.Item.Done ? " · 已完成" : "");
                }
                sub += " · " + e.DeletedAt.ToString("yyyy-MM-dd HH:mm");

                var g = new Grid();
                g.ColumnDefinitions.Add(new ColumnDefinition());
                g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                var texts = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
                texts.Children.Add(new TextBlock
                {
                    Text = main, TextWrapping = TextWrapping.Wrap, MaxHeight = 60, TextTrimming = TextTrimming.CharacterEllipsis,
                    FontWeight = e.Kind == "note" ? FontWeights.SemiBold : FontWeights.Normal
                });
                texts.Children.Add(new TextBlock { Text = sub, Foreground = Ui.Muted, FontSize = 12, Margin = new Thickness(0, 3, 0, 0) });
                g.Children.Add(texts);
                var btns = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(10, 0, 0, 0) };
                btns.Children.Add(Ui.Btn("恢复", delegate { app.Restore(entry); }));
                btns.Children.Add(Ui.Btn("彻底删除", delegate { app.PurgeTrash(entry); }));
                Grid.SetColumn(btns, 1);
                g.Children.Add(btns);
                list.Children.Add(new Border
                {
                    Child = g, Padding = new Thickness(12, 9, 10, 9),
                    BorderBrush = Ui.Line, BorderThickness = new Thickness(0, 0, 0, 1)
                });
            }
        }
    }

    // ---------------------------------------------------------------------
    // 设置：开机启动、全局快捷键、数据文件夹
    // ---------------------------------------------------------------------
    class SettingsWindow : Window
    {
        readonly DeskApp app;
        readonly TextBlock hotkeyStatus = new TextBlock { Margin = new Thickness(0, 6, 0, 0), TextWrapping = TextWrapping.Wrap };

        public SettingsWindow(DeskApp app)
        {
            this.app = app;
            Title = "DeskNote 设置";
            Width = 470;
            SizeToContent = SizeToContent.Height;
            ResizeMode = ResizeMode.NoResize;
            WindowStartupLocation = WindowStartupLocation.CenterScreen;
            FontFamily = Ui.Font;
            FontSize = 13.5;

            var root = new StackPanel { Margin = new Thickness(20, 16, 20, 16) };

            // ---- 开机启动 ----
            root.Children.Add(Section("启动"));
            var auto = new CheckBox { Content = "开机自动启动", IsChecked = DeskApp.IsAutoStart() };
            if (!app.IsDefaultDataDir)
            {
                auto.IsEnabled = false;
                auto.Content = "开机自动启动（测试数据目录下不可用）";
            }
            auto.Click += delegate { DeskApp.SetAutoStart(auto.IsChecked == true); };
            root.Children.Add(auto);
            root.Children.Add(Hint("开机启动记录的是程序现在的位置，移动 exe 后要重新勾选一次。"));

            // ---- 快捷键 ----
            root.Children.Add(Section("全局快捷键"));
            var enable = new CheckBox { Content = "启用", IsChecked = app.Settings.HotkeyEnabled };
            var box = new TextBox
            {
                Text = app.Settings.Hotkey, IsReadOnly = true, Width = 200, Padding = new Thickness(4, 3, 4, 3),
                HorizontalAlignment = HorizontalAlignment.Left, Margin = new Thickness(0, 8, 0, 0),
                IsReadOnlyCaretVisible = false, Cursor = Cursors.Hand
            };
            enable.Click += delegate
            {
                app.Settings.HotkeyEnabled = enable.IsChecked == true;
                Store.SaveSettings(app.Settings);
                box.IsEnabled = app.Settings.HotkeyEnabled;
                ShowStatus(app.ApplyHotkey());
            };
            box.IsEnabled = app.Settings.HotkeyEnabled;
            // 录制时先停用当前快捷键，否则按下去会被系统直接截走
            box.GotKeyboardFocus += delegate
            {
                app.Settings.HotkeyEnabled = false; app.ApplyHotkey(); app.Settings.HotkeyEnabled = true;
                hotkeyStatus.Foreground = Ui.Muted;
                hotkeyStatus.Text = "请按下新的组合键…";
            };
            box.LostKeyboardFocus += delegate { ShowStatus(app.ApplyHotkey()); };
            box.PreviewKeyDown += delegate(object s, KeyEventArgs e)
            {
                e.Handled = true;
                Key k = e.Key == Key.System ? e.SystemKey : e.Key;
                if (k == Key.LeftCtrl || k == Key.RightCtrl || k == Key.LeftAlt || k == Key.RightAlt ||
                    k == Key.LeftShift || k == Key.RightShift || k == Key.LWin || k == Key.RWin) return;
                if (k == Key.Escape || k == Key.Tab) { Keyboard.ClearFocus(); return; }
                var mods = Keyboard.Modifiers;
                if ((mods & (ModifierKeys.Control | ModifierKeys.Alt | ModifierKeys.Windows)) == 0)
                {
                    hotkeyStatus.Foreground = Brushes.IndianRed;
                    hotkeyStatus.Text = "要配合 Ctrl、Alt 或 Win 一起按";
                    return;
                }
                app.Settings.Hotkey = Hotkeys.Format(mods, k);
                box.Text = app.Settings.Hotkey;
                Store.SaveSettings(app.Settings);
                ShowStatus(app.ApplyHotkey());
                app.Settings.HotkeyEnabled = false; app.ApplyHotkey(); app.Settings.HotkeyEnabled = true; // 还在录制中，先别生效
                hotkeyStatus.Text = "已记录：" + app.Settings.Hotkey + "，点别处后生效";
            };
            root.Children.Add(enable);
            root.Children.Add(box);
            root.Children.Add(hotkeyStatus);
            root.Children.Add(Hint("点输入框后按下新的组合键。按快捷键或单击托盘图标，会把所有便签临时提到最前面；再按一次或点别处恢复。"));

            // ---- 数据 ----
            root.Children.Add(Section("数据"));
            var dirRow = new DockPanel();
            var open = Ui.Btn("打开", delegate { Process.Start("explorer.exe", Store.Dir); });
            DockPanel.SetDock(open, Dock.Right);
            dirRow.Children.Add(open);
            dirRow.Children.Add(new TextBox { Text = Store.Dir, IsReadOnly = true, BorderThickness = new Thickness(0), Background = Brushes.Transparent, VerticalAlignment = VerticalAlignment.Center });
            root.Children.Add(dirRow);
            root.Children.Add(Hint("每张便签一个 json 文件，在 notes 文件夹里；回收站是 trash.json。"));

            var close = Ui.Btn("关闭", Close);
            close.HorizontalAlignment = HorizontalAlignment.Right;
            close.Margin = new Thickness(0, 18, 0, 0);
            close.IsDefault = false;
            root.Children.Add(close);

            Content = root;
            ShowStatus(app.ApplyHotkey());
        }

        void ShowStatus(string err)
        {
            if (!app.Settings.HotkeyEnabled) { hotkeyStatus.Foreground = Ui.Muted; hotkeyStatus.Text = "已停用"; return; }
            hotkeyStatus.Foreground = err == null ? (Brush)new SolidColorBrush(Color.FromRgb(0x2E, 0x8B, 0x57)) : Brushes.IndianRed;
            hotkeyStatus.Text = err == null ? "已生效：" + app.Settings.Hotkey : err;
        }

        static TextBlock Section(string text)
        {
            return new TextBlock { Text = text, FontWeight = FontWeights.SemiBold, FontSize = 14.5, Margin = new Thickness(0, 14, 0, 8) };
        }

        static TextBlock Hint(string text)
        {
            return new TextBlock { Text = text, Foreground = Ui.Muted, FontSize = 12, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 6, 0, 0) };
        }
    }
}

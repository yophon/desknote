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
            root.Children.Add(Hint("移动 exe 后，打开一次新位置的 exe，开机启动会自动改到新位置。"));

            // ---- 显示 ----
            root.Children.Add(Section("显示"));
            var gpu = new CheckBox { Content = "用显卡渲染", IsChecked = app.Settings.HardwareRender };
            var restartRow = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 6, 0, 0) };
            var restartNote = new TextBlock { Text = "重启 DeskNote 后生效", Foreground = Brushes.IndianRed, FontSize = 12, VerticalAlignment = VerticalAlignment.Center };
            var restartBtn = Ui.Btn("立即重启", delegate { app.Restart(); });
            restartRow.Children.Add(restartNote);
            restartRow.Children.Add(restartBtn);
            Action updateRestart = delegate
            {
                restartRow.Visibility = (gpu.IsChecked == true) != DeskApp.HardwareRenderActive ? Visibility.Visible : Visibility.Collapsed;
            };
            gpu.Click += delegate
            {
                app.Settings.HardwareRender = gpu.IsChecked == true;
                Store.SaveSettings(app.Settings);
                updateRestart();
            };
            updateRestart();
            root.Children.Add(gpu);
            root.Children.Add(restartRow);
            root.Children.Add(Hint("默认用 CPU 画，内存约 50MB。用显卡画时 Windows 要为它加载显卡驱动，会多占约 70MB，显示效果没有区别；只在发现显示不正常时再打开。"));

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
            var backupRow = new DockPanel { Margin = new Thickness(0, 8, 0, 0) };
            var openBackup = Ui.Btn("打开备份", delegate
            {
                System.IO.Directory.CreateDirectory(Store.BackupsDir);
                Process.Start("explorer.exe", Store.BackupsDir);
            });
            DockPanel.SetDock(openBackup, Dock.Right);
            backupRow.Children.Add(openBackup);
            backupRow.Children.Add(new TextBlock
            {
                Text = "每天自动备份一次，保留最近 " + Store.KeepBackups + " 天。要找回某天的内容，把备份里的文件复制回 notes 文件夹即可。",
                Foreground = Ui.Muted, FontSize = 12, TextWrapping = TextWrapping.Wrap, VerticalAlignment = VerticalAlignment.Center
            });
            root.Children.Add(backupRow);

            // ---- 同步 ----
            root.Children.Add(Section("同步到 gopan"));
            root.Children.Add(new SyncPanel(app));

            var bottom = new DockPanel { Margin = new Thickness(0, 18, 0, 0) };
            var close = Ui.Btn("关闭", Close);
            DockPanel.SetDock(close, Dock.Right);
            bottom.Children.Add(close);
            var bt = DeskApp.BuildTime;
            bottom.Children.Add(new TextBlock
            {
                Text = "DeskNote " + DeskApp.Version + (bt > DateTime.MinValue ? " · 编译于 " + bt.ToString("yyyy-MM-dd HH:mm") : ""),
                Foreground = Ui.Muted, FontSize = 12, VerticalAlignment = VerticalAlignment.Center
            });
            root.Children.Add(bottom);

            Content = root;
            ShowStatus(app.ApplyHotkey());
        }

        void ShowStatus(string err)
        {
            if (!app.Settings.HotkeyEnabled) { hotkeyStatus.Foreground = Ui.Muted; hotkeyStatus.Text = "已停用"; return; }
            hotkeyStatus.Foreground = err == null ? (Brush)new SolidColorBrush(Color.FromRgb(0x2E, 0x8B, 0x57)) : Brushes.IndianRed;
            hotkeyStatus.Text = err == null ? "已生效：" + app.Settings.Hotkey : err;
        }

        internal static TextBlock Section(string text)
        {
            return new TextBlock { Text = text, FontWeight = FontWeights.SemiBold, FontSize = 14.5, Margin = new Thickness(0, 14, 0, 8) };
        }

        internal static TextBlock Hint(string text)
        {
            return new TextBlock { Text = text, Foreground = Ui.Muted, FontSize = 12, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 6, 0, 0) };
        }
    }
    // ---------------------------------------------------------------------
    // 设置提醒：日期 + 时间，外加几个常用的快捷选项
    // ---------------------------------------------------------------------
    class ReminderDialog : Window
    {
        public DateTime? Result;
        readonly DatePicker date = new DatePicker { Width = 150, Margin = new Thickness(0, 0, 10, 0) };
        readonly TextBox time = new TextBox { Width = 70, Padding = new Thickness(4, 3, 4, 3), VerticalContentAlignment = VerticalAlignment.Center };
        readonly TextBlock error = new TextBlock { Foreground = Brushes.IndianRed, Margin = new Thickness(0, 6, 0, 0) };

        public ReminderDialog(string itemText, DateTime? current)
        {
            Title = "设置提醒";
            Width = 400;
            SizeToContent = SizeToContent.Height;
            ResizeMode = ResizeMode.NoResize;
            WindowStartupLocation = WindowStartupLocation.CenterScreen;
            Topmost = true;
            FontFamily = Ui.Font;
            FontSize = 13.5;

            var root = new StackPanel { Margin = new Thickness(20, 16, 20, 16) };
            string shown = itemText.Replace("\n", " ");
            if (shown.Length > 80) shown = shown.Substring(0, 80) + "…";
            root.Children.Add(new TextBlock { Text = shown, TextWrapping = TextWrapping.Wrap, FontWeight = FontWeights.SemiBold, Margin = new Thickness(0, 0, 0, 12) });

            var quick = new WrapPanel { Margin = new Thickness(0, 0, 0, 10) };
            var now = DateTime.Now;
            AddQuick(quick, "30 分钟后", now.AddMinutes(30));
            AddQuick(quick, "1 小时后", now.AddHours(1));
            var tonight = DateTime.Today.AddHours(20);
            if (tonight > now.AddMinutes(5)) AddQuick(quick, "今晚 20:00", tonight);
            AddQuick(quick, "明早 9:00", DateTime.Today.AddDays(1).AddHours(9));
            root.Children.Add(quick);

            var row = new StackPanel { Orientation = Orientation.Horizontal };
            row.Children.Add(date);
            row.Children.Add(time);
            root.Children.Add(row);
            root.Children.Add(error);
            root.Children.Add(SettingsWindow.Hint("到点后会弹出系统通知，条目上的提醒时间变红。程序没开着时错过的提醒，下次打开时补上。"));

            var t0 = current ?? DateTime.Now.AddHours(1);
            date.SelectedDate = t0.Date;
            time.Text = t0.ToString("HH:mm");

            var btns = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 16, 0, 0) };
            var ok = Ui.Btn("确定", delegate { Confirm(); });
            ok.IsDefault = true;
            var cancel = Ui.Btn("取消", delegate { DialogResult = false; });
            cancel.IsCancel = true;
            btns.Children.Add(ok);
            btns.Children.Add(cancel);
            root.Children.Add(btns);
            Content = root;
            Loaded += delegate { time.Focus(); time.SelectAll(); };
        }

        void AddQuick(Panel p, string label, DateTime t)
        {
            var minute = new DateTime(t.Year, t.Month, t.Day, t.Hour, t.Minute, 0);
            var b = Ui.Btn(label, delegate { Result = minute; DialogResult = true; });
            b.Margin = new Thickness(0, 0, 6, 6);
            p.Children.Add(b);
        }

        void Confirm()
        {
            DateTime tm;
            string s = time.Text.Trim().Replace("：", ":");
            if (!date.SelectedDate.HasValue) { error.Text = "请选择日期"; return; }
            if (!DateTime.TryParseExact(s, new[] { "H:mm", "HH:mm", "H", "HHmm" }, System.Globalization.CultureInfo.InvariantCulture,
                    System.Globalization.DateTimeStyles.None, out tm)) { error.Text = "时间格式像 9:30 或 21:05"; return; }
            Result = date.SelectedDate.Value.Date.Add(tm.TimeOfDay);
            DialogResult = true;
        }
    }
    // ---------------------------------------------------------------------
    // 设置里的“同步到 gopan”一栏
    // ---------------------------------------------------------------------
    class SyncPanel : StackPanel
    {
        readonly DeskApp app;
        readonly TextBlock status = new TextBlock { FontSize = 12, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 6, 0, 0) };

        public SyncPanel(DeskApp app)
        {
            this.app = app;
            var s = app.Settings;
            var enable = new CheckBox { Content = "启用同步（多台电脑共用这些便签）", IsChecked = s.SyncEnabled };
            var server = Field(s.SyncServer);
            var folder = Field(s.SyncFolder);
            var key = new PasswordBox { Padding = new Thickness(4, 3, 4, 3) };
            var keyHint = new TextBlock
            {
                Text = s.SyncKey.Length > 0 ? "已保存（不改就留空）" : "", Foreground = Ui.Muted, FontSize = 12,
                IsHitTestVisible = false, Margin = new Thickness(7, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center
            };
            key.PasswordChanged += delegate { keyHint.Visibility = key.Password.Length > 0 ? Visibility.Collapsed : Visibility.Visible; };
            var keyBox = new Grid();
            keyBox.Children.Add(key);
            keyBox.Children.Add(keyHint);

            var grid = new Grid { Margin = new Thickness(0, 8, 0, 0) };
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            grid.ColumnDefinitions.Add(new ColumnDefinition());
            AddRow(grid, 0, "服务器", server);
            AddRow(grid, 1, "密钥", keyBox);
            AddRow(grid, 2, "文件夹", folder);

            var btns = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 8, 0, 0) };
            var save = Ui.Btn("保存并立即同步", delegate
            {
                s.SyncEnabled = enable.IsChecked == true;
                s.SyncServer = server.Text.Trim().TrimEnd('/');
                if (s.SyncServer.Length > 0 && !s.SyncServer.StartsWith("http", StringComparison.OrdinalIgnoreCase)) s.SyncServer = "https://" + s.SyncServer;
                server.Text = s.SyncServer;
                s.SyncFolder = Syncer.NormFolder(folder.Text);
                folder.Text = s.SyncFolder;
                if (key.Password.Length > 0) { s.SyncKey = key.Password.Trim(); key.Password = ""; keyHint.Text = "已保存（不改就留空）"; }
                Store.SaveSettings(s);
                app.Sync.Start();
                app.Sync.SyncNow();
            });
            save.Margin = new Thickness(0);
            btns.Children.Add(save);

            Children.Add(enable);
            Children.Add(grid);
            Children.Add(btns);
            Children.Add(status);
            Children.Add(SettingsWindow.Hint(
                "在 gopan 网页的「Agent 接入」里新建一把密钥，权限勾选：浏览与搜索、读取与下载内容、新建重命名移动与复制、移入回收站与恢复。" +
                "每张便签存成文件夹里的一个 json；只同步内容和外观，窗口位置、回收站各台电脑自己管。" +
                "两台电脑同时改了同一张便签时，本地那份会另存成一张「冲突副本」，不会丢。"));

            Action refresh = delegate
            {
                status.Text = app.Sync.Status;
                status.Foreground = app.Sync.LastFailed ? (Brush)Brushes.IndianRed : Ui.Muted;
            };
            app.Sync.StatusChanged += refresh;
            Unloaded += delegate { app.Sync.StatusChanged -= refresh; };
            refresh();
        }

        static TextBox Field(string text)
        {
            return new TextBox { Text = text, Padding = new Thickness(4, 3, 4, 3) };
        }

        static void AddRow(Grid g, int row, string label, UIElement field)
        {
            g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            var l = new TextBlock { Text = label, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 3, 10, 3) };
            Grid.SetRow(l, row);
            g.Children.Add(l);
            var fe = (FrameworkElement)field;
            fe.Margin = new Thickness(0, 3, 0, 3);
            Grid.SetRow(fe, row);
            Grid.SetColumn(fe, 1);
            g.Children.Add(fe);
        }
    }
}

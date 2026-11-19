// 一张便签的窗口
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Shapes;
using System.Windows.Threading;

namespace DeskNote
{
    partial class NoteWindow : Window
    {
        readonly DeskApp app;
        public readonly NoteData Data;
        NoteData d { get { return Data; } }

        // ---- 状态 ----
        IntPtr hwnd;
        const int NotEditing = -1, TitleIndex = -2;
        int editIndex = NotEditing;   // 正在编辑：标题 / 条目下标（等于条目数表示新增）
        TextBox editBox;
        string editOrigText;          // 开始编辑时这一条的原文，合并外部修改时用来找回位置
        int menuTarget = NotEditing;  // 右键点中的条目
        int dragIndex = -1;           // 正在拖动排序的条目
        bool dragChanged;
        bool peeking;                 // 被快捷键临时提到最前
        public bool Deleted;          // 已移进回收站
        public bool AllowClose;       // 只有退出程序或删除便签时才真正关闭
        int taskbarCreatedMsg;
        readonly List<Action> menuRefresh = new List<Action>();

        // ---- 界面 ----
        readonly Border card = new Border();
        readonly ScrollViewer scroll = new ScrollViewer();
        readonly StackPanel view = new StackPanel();
        readonly StackPanel capsule = new StackPanel { Orientation = Orientation.Horizontal };
        FrameworkElement collapseBtn;

        static readonly FontFamily UiFont = new FontFamily("Microsoft YaHei UI");

        bool Editing { get { return editIndex != NotEditing; } }
        bool Collapsed { get { return d.Collapsed; } }

        public NoteWindow(DeskApp app, NoteData data)
        {
            this.app = app;
            Data = data;
            WindowStyle = WindowStyle.None;
            AllowsTransparency = true;
            Background = Brushes.Transparent;
            ShowInTaskbar = false;
            ShowActivated = false;
            ResizeMode = ResizeMode.NoResize;
            Title = string.IsNullOrEmpty(d.Title) ? "DeskNote" : d.Title;
            Icon = app.WindowIcon;
            Left = 80; Top = 80; Width = d.Width; Height = d.Height;
            MinWidth = 160; MinHeight = 120;

            BuildUi();
            if (Collapsed) EnterCollapsed(); else Render();

            SourceInitialized += OnSourceInitialized;
            PreviewKeyDown += OnPreviewKey;
            Deactivated += delegate { CommitEdit(true); SendToBottom(); };
            Closing += delegate(object s, System.ComponentModel.CancelEventArgs e)
            {
                if (!AllowClose) { e.Cancel = true; return; } // 比如编辑时按了 Alt+F4
                CommitEdit(true);
                ClosePreview();
                SaveLayout();
            };
        }

        // =====================================================================
        // 界面
        // =====================================================================
        void BuildUi()
        {
            ApplyOpacity();
            ApplyShadow();
            // 内容超出时可滚轮滚动；不显示滚动条。Focusable=false 否则它会吃掉左键、拖不动窗口
            scroll.VerticalScrollBarVisibility = ScrollBarVisibility.Hidden;
            scroll.Focusable = false;
            scroll.Content = view;
            card.Child = scroll;
            card.CornerRadius = new CornerRadius(4);
            card.Padding = new Thickness(20, 16, 20, 16);
            Content = card;

            card.MouseLeftButtonDown += OnCardMouseDown;
            card.PreviewMouseRightButtonDown += delegate(object s, MouseButtonEventArgs e)
            {
                menuTarget = TagOf(e.OriginalSource as DependencyObject);
            };
            card.MouseMove += OnCardMouseMove;
            card.MouseEnter += delegate
            {
                if (collapseBtn != null) collapseBtn.Visibility = Visibility.Visible;
                if (Collapsed) OnCapsuleEnter();
            };
            card.MouseLeave += delegate
            {
                Cursor = null;
                if (collapseBtn != null) collapseBtn.Visibility = Visibility.Hidden;
                if (Collapsed) ScheduleCapsuleHide();
            };

            // 胶囊内容变化（标题、条数）导致宽度变了，贴边的要重新贴好
            SizeChanged += delegate { if (Collapsed && !capHidden && !sliding) SnapCapsule(); };

            // 拖动排序：按住手柄后由 view 接管鼠标
            view.MouseMove += OnReorderMove;
            view.MouseLeftButtonUp += delegate { EndReorder(); };
            view.LostMouseCapture += delegate { EndReorder(); };

            hideTimer.Tick += OnHideTimer;
            BuildMenu();
        }

        Brush TextBrush { get { return new SolidColorBrush(d.TextColor); } }
        Brush DimBrush { get { return new SolidColorBrush(Color.FromArgb(0xA0, d.TextColor.R, d.TextColor.G, d.TextColor.B)); } }
        double TitleSize { get { return Math.Round(d.FontSize * 1.3, 1); } }
        double LineH { get { return Math.Round(d.FontSize * 1.45); } }
        double HandleWidth { get { return Math.Round(d.FontSize * 1.1); } }

        void Render()
        {
            Title = d.Title;
            if (Collapsed) { RenderCapsule(); return; }
            view.Children.Clear();

            // 标题行：标题 + 悬停时出现的折叠按钮
            var titleRow = new Grid { Tag = TitleIndex, Background = Brushes.Transparent, Margin = new Thickness(0, 0, 0, 12) };
            titleRow.ColumnDefinitions.Add(new ColumnDefinition());
            titleRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            UIElement titleEl;
            if (editIndex == TitleIndex)
                titleEl = MakeEditBox(d.Title, TitleSize, FontWeights.Bold);
            else
                titleEl = new TextBlock
                {
                    Text = d.Title, FontFamily = UiFont, FontSize = TitleSize, FontWeight = FontWeights.Bold,
                    Foreground = TextBrush, TextTrimming = TextTrimming.CharacterEllipsis
                };
            titleRow.Children.Add(titleEl);
            collapseBtn = MakeCollapseButton();
            Grid.SetColumn(collapseBtn, 1);
            titleRow.Children.Add(collapseBtn);
            view.Children.Add(titleRow);

            if (d.Items.Count == 0 && editIndex != 0)
                view.Children.Add(new TextBlock
                {
                    Text = "双击新增，右键设置",FontFamily = UiFont, FontSize = d.FontSize, Foreground = DimBrush
                });

            int n = d.Items.Count + (editIndex == d.Items.Count ? 1 : 0);
            for (int i = 0; i < n; i++)
            {
                NoteItem it = i < d.Items.Count ? d.Items[i] : new NoteItem();
                var row = new Grid { Margin = new Thickness(0, 0, 0, Math.Round(d.FontSize * 0.4)), Tag = i, Background = Brushes.Transparent };
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                row.ColumnDefinitions.Add(new ColumnDefinition());
                if (i == dragIndex) row.Opacity = 0.45;

                if (i < d.Items.Count) row.Children.Add(MakeHandle(i));
                else row.Children.Add(new Border { Width = HandleWidth });
                if (d.Checkboxes)
                {
                    var chk = MakeCheck(i, it.Done && i != editIndex);
                    Grid.SetColumn(chk, 1);
                    row.Children.Add(chk);
                }

                UIElement body;
                if (i == editIndex)
                    body = MakeEditBox(it.Text, d.FontSize, FontWeight.FromOpenTypeWeight(d.FontWeight));
                else
                    body = MakeItemText(it);
                Grid.SetColumn(body, 2);
                row.Children.Add(body);
                view.Children.Add(row);
            }
        }

        TextBlock MakeItemText(NoteItem it)
        {
            var tb = new TextBlock
            {
                FontFamily = UiFont, FontSize = d.FontSize,
                FontWeight = FontWeight.FromOpenTypeWeight(d.FontWeight),
                Foreground = TextBrush, TextWrapping = TextWrapping.Wrap, LineHeight = LineH
            };
            AddTextWithLinks(tb, it.Text);
            if (it.Remind.HasValue) tb.Inlines.Add(ReminderTag(it));
            if (d.Checkboxes && it.Done)
            {
                tb.Opacity = 0.5;
                tb.TextDecorations = TextDecorations.Strikethrough;
            }
            return tb;
        }

        // 六点手柄：按住上下拖动换位置
        FrameworkElement MakeHandle(int index)
        {
            double dd = Math.Max(2, Math.Round(d.FontSize * 0.17, 1)); // 点的直径
            double gap = dd * 0.9;
            var dots = new Canvas { Width = dd * 2 + gap, Height = dd * 3 + gap * 2 };
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 2; c++)
                {
                    var e = new Ellipse { Width = dd, Height = dd, Fill = DimBrush };
                    Canvas.SetLeft(e, c * (dd + gap));
                    Canvas.SetTop(e, r * (dd + gap));
                    dots.Children.Add(e);
                }
            var h = new Border
            {
                Width = HandleWidth, Height = LineH, Background = Brushes.Transparent,
                VerticalAlignment = VerticalAlignment.Top, Cursor = Cursors.SizeNS,
                ToolTip = "按住拖动换位置"
            };
            dots.HorizontalAlignment = HorizontalAlignment.Left;
            dots.VerticalAlignment = VerticalAlignment.Center;
            h.Child = dots;
            h.MouseLeftButtonDown += delegate(object s, MouseButtonEventArgs e)
            {
                e.Handled = true;
                BeginReorder(index);
            };
            return h;
        }

        // 勾选框：点一下标记完成 / 取消
        FrameworkElement MakeCheck(int index, bool done)
        {
            double size = Math.Round(d.FontSize * 0.95);
            var box = new Border
            {
                Width = size, Height = size, CornerRadius = new CornerRadius(Math.Max(2, size * 0.22)),
                BorderBrush = DimBrush, BorderThickness = new Thickness(Math.Max(1, Math.Round(d.FontSize / 10.0))),
                Background = done ? DimBrush : Brushes.Transparent
            };
            if (done)
            {
                var bg = d.BgColor;
                box.Child = new Path
                {
                    Data = Geometry.Parse("M 0.2,0.55 L 0.42,0.76 L 0.8,0.28"),
                    Stretch = Stretch.Fill, Margin = new Thickness(size * 0.18),
                    Stroke = new SolidColorBrush(Color.FromRgb(bg.R, bg.G, bg.B)),
                    StrokeThickness = Math.Max(1.5, d.FontSize / 8.0),
                    StrokeStartLineCap = PenLineCap.Round, StrokeEndLineCap = PenLineCap.Round, StrokeLineJoin = PenLineJoin.Round
                };
            }
            var hit = new Border
            {
                Height = LineH, Background = Brushes.Transparent, Child = box,
                Padding = new Thickness(0, 0, Math.Round(d.FontSize * 0.55), 0),
                VerticalAlignment = VerticalAlignment.Top, Cursor = Cursors.Hand,
                ToolTip = done ? "标记为未完成" : "标记为完成"
            };
            if (index < d.Items.Count)
                hit.MouseLeftButtonDown += delegate(object s, MouseButtonEventArgs e)
                {
                    e.Handled = true;
                    CommitEdit(true);
                    if (index < d.Items.Count) { BeginChange(); d.Items[index].Done = !d.Items[index].Done; EndChange(); Save(); Render(); }
                };
            return hit;
        }

        FrameworkElement MakeCollapseButton()
        {
            var line = new Border
            {
                Width = 12, Height = 2, Background = DimBrush,
                HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center
            };
            var b = new Border
            {
                Width = 24, Height = 24, Background = Brushes.Transparent, Child = line,
                Cursor = Cursors.Hand, ToolTip = "折叠成胶囊", VerticalAlignment = VerticalAlignment.Center,
                Visibility = card.IsMouseOver ? Visibility.Visible : Visibility.Hidden,
                Margin = new Thickness(6, 0, -6, 0)
            };
            b.MouseLeftButtonDown += delegate(object s, MouseButtonEventArgs e) { e.Handled = true; Collapse(); };
            return b;
        }

        TextBox MakeEditBox(string text, double size, FontWeight weight)
        {
            var b = new TextBox
            {
                Text = text.Replace("\n", "\r\n"), FontFamily = UiFont, FontSize = size, FontWeight = weight,
                Foreground = TextBrush, CaretBrush = TextBrush,
                Background = new SolidColorBrush(Color.FromArgb(0x40, 0, 0, 0)),
                BorderThickness = new Thickness(0), Padding = new Thickness(2, 0, 2, 0),
                Margin = new Thickness(-2, 0, -2, 0),
                TextWrapping = TextWrapping.Wrap, AcceptsReturn = false, AcceptsTab = true
            };
            b.KeyDown += delegate(object s, KeyEventArgs e)
            {
                if (e.Key == Key.Enter && Keyboard.Modifiers == ModifierKeys.Shift)
                {
                    // Shift+Enter：在这一条里换行（标题不换行）
                    if (editIndex != TitleIndex)
                    {
                        int start = b.SelectionStart;
                        b.Text = b.Text.Remove(start, b.SelectionLength).Insert(start, "\r\n");
                        b.CaretIndex = start + 2;
                    }
                    e.Handled = true;
                }
                else if (e.Key == Key.Enter)
                {
                    // 新增的条目按 Enter 后接着写下一条；空着按 Enter 就结束
                    bool continueAdding = editIndex == d.Items.Count && b.Text.Trim().Length > 0;
                    CommitEdit(true);
                    if (continueAdding) BeginEdit(d.Items.Count);
                    e.Handled = true;
                }
                else if (e.Key == Key.Escape) { CommitEdit(false); e.Handled = true; }
            };
            b.LostKeyboardFocus += delegate { if (b == editBox) CommitEdit(true); };
            b.Loaded += delegate { b.Focus(); b.CaretIndex = b.Text.Length; };
            editBox = b;
            return b;
        }

        Color BgWithAlpha(double o)
        {
            // 分层窗口里 alpha=0 的像素会被系统当成不存在，鼠标会穿透；最低留 1/255，看不出来但还能点
            byte a = (byte)Math.Max(1, Math.Round(o * 255));
            return Color.FromArgb(a, d.BgColor.R, d.BgColor.G, d.BgColor.B);
        }

        void ApplyOpacity()
        {
            // 胶囊太小，背景至少 75% 不透明，免得找不到
            card.Background = new SolidColorBrush(BgWithAlpha(Collapsed ? Math.Max(d.Opacity, 0.75) : d.Opacity));
        }

        void ApplyShadow()
        {
            System.Windows.Media.Effects.Effect fx = d.Shadow
                ? new System.Windows.Media.Effects.DropShadowEffect { ShadowDepth = 1, BlurRadius = 4, Color = Colors.Black, Opacity = 0.9 }
                : null;
            view.Effect = fx;
        }

        // 已删除的便签不再写文件（否则关窗时保存位置会把刚删掉的文件又写回来）
        void Save()
        {
            if (Deleted) return;
            Store.SaveNote(d);
            app.OnNoteSaved();
        }

        // 外部改了这张便签的文件：重新读入
        // 正在编辑、拖动或菜单开着时先存着，结束后再合并（见 CommitEdit / EndReorder / EndChange）
        NoteData pendingReload;

        // 外部改了这张便签（文件被改、或同步拉下来的新内容）。keepWindow：只换内容，不动窗口模式和折叠状态
        public void ReloadFrom(NoteData fresh, bool keepWindow)
        {
            if (keepWindow) { fresh.Mode = d.Mode; fresh.Collapsed = d.Collapsed; }
            if (Editing || dragIndex >= 0 || changeDepth > 0) { pendingReload = fresh; return; }
            ApplyExternal(fresh);
            Render();
        }

        public void ReloadFrom(NoteData fresh) { ReloadFrom(fresh, false); }

        void ApplyExternal(NoteData fresh)
        {
            bool wasCollapsed = d.Collapsed;
            d.CopyContentFrom(fresh);
            ClearUndo(); // 外部改过以后，旧的撤销记录对不上了
            ApplyOpacity();
            ApplyShadow();
            if (fresh.Mode != d.Mode) SetMode(fresh.Mode);
            if (fresh.Collapsed != wasCollapsed) { if (fresh.Collapsed) Collapse(); else Expand(); }
            Save();
        }

        // =====================================================================
        // 菜单
        // =====================================================================
        MenuItem SliderItem(string label, double min, double max, Func<double> get, Action<double> set, string unit)
        {
            var slider = new Slider
            {
                Minimum = min, Maximum = max, Width = 140, SmallChange = 1, LargeChange = 5,
                IsMoveToPointEnabled = true, IsSnapToTickEnabled = true, TickFrequency = 1,
                VerticalAlignment = VerticalAlignment.Center
            };
            var val = new TextBlock { Width = 40, TextAlignment = TextAlignment.Right, VerticalAlignment = VerticalAlignment.Center };
            slider.ValueChanged += delegate { val.Text = slider.Value + unit; set(slider.Value); };
            var row = new StackPanel { Orientation = Orientation.Horizontal };
            row.Children.Add(new TextBlock { Text = label, Width = 56, VerticalAlignment = VerticalAlignment.Center });
            row.Children.Add(slider);
            row.Children.Add(val);
            var mi = new MenuItem { Header = row, StaysOpenOnClick = true };
            menuRefresh.Add(delegate { slider.Value = get(); });
            return mi;
        }

        static MenuItem Item(string header, Action click)
        {
            var mi = new MenuItem { Header = header };
            mi.Click += delegate { click(); };
            return mi;
        }

        void BuildMenu()
        {
            var menu = new ContextMenu();

            // ---- 条目 ----
            var up = Item("上移", delegate { MoveItem(menuTarget, menuTarget - 1); });
            var down = Item("下移", delegate { MoveItem(menuTarget, menuTarget + 1); });
            var del = Item("删除这一条", delegate
            {
                if (menuTarget >= 0 && menuTarget < d.Items.Count) { TrashItemAt(menuTarget); Save(); Render(); }
            });
            var add = Item("新增一条", delegate { Expand(); BeginEdit(d.Items.Count); });
            var clearDone = Item("清除已完成的条目", ClearDone);
            var remind = Item("设置提醒…", delegate { int t = menuTarget; Dispatcher.BeginInvoke(new Action(delegate { EditReminder(t); })); });
            var unremind = Item("取消提醒", delegate
            {
                if (menuTarget >= 0 && menuTarget < d.Items.Count) { d.Items[menuTarget].Remind = null; d.Items[menuTarget].Reminded = false; Render(); }
            });
            var undoMi = Item("撤销", Undo);
            var redoMi = Item("重做", Redo);
            undoMi.InputGestureText = "Ctrl+Z";
            redoMi.InputGestureText = "Ctrl+Y";
            menu.Items.Add(undoMi);
            menu.Items.Add(redoMi);
            var undoSep = new Separator();
            menu.Items.Add(undoSep);
            menu.Items.Add(up);
            menu.Items.Add(down);
            menu.Items.Add(del);
            menu.Items.Add(add);
            menu.Items.Add(remind);
            menu.Items.Add(unremind);
            menu.Items.Add(clearDone);
            menu.Items.Add(new Separator());

            // ---- 便签 ----
            var fold = Item("", delegate { if (Collapsed) Expand(); else Collapse(); });
            menu.Items.Add(fold);
            var checks = new MenuItem { Header = "显示勾选框", IsCheckable = true };
            checks.Click += delegate { d.Checkboxes = checks.IsChecked; Save(); Render(); };
            menu.Items.Add(checks);

            var modeMenu = new MenuItem { Header = "窗口模式" };
            var modes = new[]
            {
                new KeyValuePair<string, WinMode>("置底在桌面（显示桌面时也在）", WinMode.Desktop),
                new KeyValuePair<string, WinMode>("置顶（总在最前面）", WinMode.Topmost),
                new KeyValuePair<string, WinMode>("普通窗口（出现在任务栏）", WinMode.Normal),
            };
            foreach (var m in modes)
            {
                WinMode mv = m.Value;
                var mi = new MenuItem { Header = m.Key, Tag = mv, IsCheckable = true };
                mi.Click += delegate { SetMode(mv); Save(); };
                modeMenu.Items.Add(mi);
            }
            modeMenu.SubmenuOpened += delegate { foreach (MenuItem mi in modeMenu.Items) mi.IsChecked = (WinMode)mi.Tag == d.Mode; };
            menu.Items.Add(modeMenu);
            menu.Items.Add(new Separator());

            // ---- 外观 ----
            menu.Items.Add(SliderItem("不透明度", 0, 100,
                delegate { return Math.Round(d.Opacity * 100); },
                delegate(double v) { d.Opacity = v / 100.0; ApplyOpacity(); }, "%"));
            menu.Items.Add(SliderItem("字号", 10, 40,
                delegate { return Math.Round(d.FontSize); },
                delegate(double v) { d.FontSize = v; Render(); }, ""));

            var weight = new MenuItem { Header = "粗细" };
            var weights = new[] { new KeyValuePair<string, int>("细", 300), new KeyValuePair<string, int>("常规", 400),
                                  new KeyValuePair<string, int>("中粗", 600), new KeyValuePair<string, int>("粗", 700) };
            foreach (var w in weights)
            {
                int wv = w.Value;
                var mi = new MenuItem { Header = w.Key, Tag = wv, IsCheckable = true, FontWeight = FontWeight.FromOpenTypeWeight(wv) };
                mi.Click += delegate { d.FontWeight = wv; Render(); Save(); };
                weight.Items.Add(mi);
            }
            weight.SubmenuOpened += delegate { foreach (MenuItem mi in weight.Items) mi.IsChecked = (int)mi.Tag == d.FontWeight; };
            menu.Items.Add(weight);

            menu.Items.Add(ColorMenu("文字颜色", new[]
            {
                new KeyValuePair<string, Color>("默认灰", NoteData.DefaultTextColor),
                new KeyValuePair<string, Color>("白", Color.FromRgb(0xF5, 0xF5, 0xF5)),
                new KeyValuePair<string, Color>("黑", Color.FromRgb(0x22, 0x22, 0x22)),
                new KeyValuePair<string, Color>("黄", Color.FromRgb(0xF2, 0xC9, 0x4C)),
                new KeyValuePair<string, Color>("橙", Color.FromRgb(0xF0, 0x8A, 0x3C)),
                new KeyValuePair<string, Color>("红", Color.FromRgb(0xE5, 0x5B, 0x5B)),
                new KeyValuePair<string, Color>("绿", Color.FromRgb(0x6C, 0xC2, 0x7A)),
                new KeyValuePair<string, Color>("蓝", Color.FromRgb(0x5B, 0xA8, 0xE5)),
            }, delegate { return d.TextColor; }, delegate(Color c) { d.TextColor = c; Render(); }));

            menu.Items.Add(ColorMenu("背景颜色", new[]
            {
                new KeyValuePair<string, Color>("默认蓝灰", NoteData.DefaultBgColor),
                new KeyValuePair<string, Color>("黑", Color.FromRgb(0x1E, 0x1E, 0x1E)),
                new KeyValuePair<string, Color>("白", Color.FromRgb(0xFA, 0xFA, 0xFA)),
                new KeyValuePair<string, Color>("便签黄", Color.FromRgb(0xFF, 0xF1, 0xA8)),
                new KeyValuePair<string, Color>("浅绿", Color.FromRgb(0xD4, 0xF0, 0xD0)),
                new KeyValuePair<string, Color>("浅蓝", Color.FromRgb(0xD2, 0xE6, 0xF8)),
                new KeyValuePair<string, Color>("浅粉", Color.FromRgb(0xF9, 0xD9, 0xE3)),
                new KeyValuePair<string, Color>("墨绿", Color.FromRgb(0x23, 0x3B, 0x33)),
            }, delegate { return d.BgColor; }, delegate(Color c) { d.BgColor = c; ApplyOpacity(); Render(); }));

            var shadow = new MenuItem { Header = "文字阴影（背景很透明时更清楚）", IsCheckable = true };
            shadow.Click += delegate { d.Shadow = shadow.IsChecked; ApplyShadow(); Save(); };
            menu.Items.Add(shadow);
            menu.Items.Add(new Separator());

            // ---- 全局 ----
            menu.Items.Add(Item("新建便签", delegate { app.CreateNote(this); }));
            menu.Items.Add(Item("删除这张便签…", delegate { app.DeleteNote(this); }));
            menu.Items.Add(Item("回收站…", delegate { app.ShowTrash(); }));
            menu.Items.Add(Item("设置…", delegate { app.ShowSettings(); }));
            menu.Items.Add(new Separator());
            menu.Items.Add(Item("退出 DeskNote", delegate { app.Exit(); }));

            menu.Opened += delegate
            {
                CommitEdit(true);
                ClosePreview();
                bool onItem = !Collapsed && menuTarget >= 0 && menuTarget < d.Items.Count;
                undoMi.Visibility = undoStack.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
                redoMi.Visibility = redoStack.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
                undoSep.Visibility = undoStack.Count + redoStack.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
                BeginChange(); // 菜单里做的所有改动算一步撤销（在 Closed 里结束）
                remind.Visibility = onItem ? Visibility.Visible : Visibility.Collapsed;
                remind.Header = onItem && d.Items[menuTarget].Remind.HasValue ? "修改提醒…" : "设置提醒…";
                unremind.Visibility = onItem && d.Items[menuTarget].Remind.HasValue ? Visibility.Visible : Visibility.Collapsed;
                del.Visibility = onItem ? Visibility.Visible : Visibility.Collapsed;
                up.Visibility = onItem && menuTarget > 0 ? Visibility.Visible : Visibility.Collapsed;
                down.Visibility = onItem && menuTarget < d.Items.Count - 1 ? Visibility.Visible : Visibility.Collapsed;
                bool anyDone = false;
                foreach (var it in d.Items) if (it.Done) anyDone = true;
                clearDone.Visibility = d.Checkboxes && anyDone ? Visibility.Visible : Visibility.Collapsed;
                fold.Header = Collapsed ? "展开" : "折叠成胶囊";
                checks.IsChecked = d.Checkboxes;
                shadow.IsChecked = d.Shadow;
                foreach (var r in menuRefresh) r();
            };
            menu.Closed += delegate { EndChange(); Save(); if (Collapsed) ScheduleCapsuleHide(); };
            card.ContextMenu = menu;
        }

        // 颜色子菜单：预设色块 + 自定义（Windows 调色板）
        MenuItem ColorMenu(string header, KeyValuePair<string, Color>[] swatches, Func<Color> get, Action<Color> set)
        {
            var menu = new MenuItem { Header = header };
            foreach (var sw in swatches)
            {
                Color c = sw.Value;
                var row = new StackPanel { Orientation = Orientation.Horizontal };
                row.Children.Add(new Border
                {
                    Width = 14, Height = 14, CornerRadius = new CornerRadius(2), Background = new SolidColorBrush(c),
                    BorderBrush = Brushes.Gray, BorderThickness = new Thickness(1), Margin = new Thickness(0, 0, 8, 0)
                });
                row.Children.Add(new TextBlock { Text = sw.Key, VerticalAlignment = VerticalAlignment.Center });
                var mi = new MenuItem { Header = row, Tag = c, IsCheckable = true };
                mi.Click += delegate { set(c); Save(); };
                menu.Items.Add(mi);
            }
            menu.Items.Add(new Separator());
            menu.Items.Add(Item("自定义…", delegate
            {
                Color cur = get();
                BeginChange();
                using (var dlg = new System.Windows.Forms.ColorDialog())
                {
                    dlg.FullOpen = true;
                    dlg.Color = System.Drawing.Color.FromArgb(cur.R, cur.G, cur.B);
                    if (dlg.ShowDialog() == System.Windows.Forms.DialogResult.OK)
                    {
                        set(Color.FromRgb(dlg.Color.R, dlg.Color.G, dlg.Color.B));
                        Save();
                    }
                }
                EndChange();
                SendToBottom();
            }));
            menu.SubmenuOpened += delegate
            {
                Color cur = get();
                foreach (var o in menu.Items)
                {
                    var mi = o as MenuItem;
                    if (mi != null && mi.Tag is Color) mi.IsChecked = (Color)mi.Tag == cur;
                }
            };
            return menu;
        }

        // =====================================================================
        // 编辑、排序、删除
        // =====================================================================
        // 从点击位置往上找带下标 Tag 的行（标题是 TitleIndex）
        int TagOf(DependencyObject o)
        {
            while (o != null && o != card)
            {
                var fe = o as FrameworkElement;
                if (fe != null && fe.Tag is int) return (int)fe.Tag;
                o = (o is Visual || o is System.Windows.Media.Media3D.Visual3D) ? VisualTreeHelper.GetParent(o) : LogicalTreeHelper.GetParent(o);
            }
            return NotEditing;
        }

        void TrashItemAt(int i)
        {
            var e = app.TrashItem(d, i, d.Items[i]);
            if (pendingStep != null) pendingStep.Trashed.Add(e);
            d.Items.RemoveAt(i);
        }

        void ClearDone()
        {
            for (int i = d.Items.Count - 1; i >= 0; i--)
                if (d.Items[i].Done) TrashItemAt(i);
            Save();
            Render();
        }

        // 从回收站恢复一条：尽量放回原来的位置
        public void RestoreItem(NoteItem it, int index)
        {
            CommitEdit(true);
            ClearUndo(); // 恢复后再撤销以前的操作会把恢复的条目弄丢，干脆清掉
            d.Items.Insert(Math.Max(0, Math.Min(index, d.Items.Count)), it);
            Save();
            Render();
        }

        void MoveItem(int from, int to)
        {
            if (from < 0 || from >= d.Items.Count || to < 0 || to >= d.Items.Count || from == to) return;
            var t = d.Items[from];
            d.Items.RemoveAt(from);
            d.Items.Insert(to, t);
            Save();
            Render();
        }

        void BeginReorder(int index)
        {
            CommitEdit(true);
            BeginChange();
            dragIndex = index;
            dragChanged = false;
            Render();
            view.CaptureMouse();
        }

        void OnReorderMove(object s, MouseEventArgs e)
        {
            if (dragIndex < 0) return;
            double y = e.GetPosition(view).Y;
            // 越过相邻条目的中线才换位，避免来回抖动
            foreach (UIElement child in view.Children)
            {
                var row = child as FrameworkElement;
                if (row == null || !(row.Tag is int) || (int)row.Tag < 0) continue;
                int i = (int)row.Tag;
                if (i == dragIndex || i >= d.Items.Count) continue;
                double top = row.TranslatePoint(new Point(0, 0), view).Y;
                double mid = top + row.ActualHeight / 2;
                if ((i > dragIndex && y > mid) || (i < dragIndex && y < mid))
                {
                    var t = d.Items[dragIndex];
                    d.Items.RemoveAt(dragIndex);
                    d.Items.Insert(i, t);
                    dragIndex = i;
                    dragChanged = true;
                    Render();
                    break;
                }
            }
            // 拖到上下边缘时自动滚动
            double sy = e.GetPosition(scroll).Y;
            if (sy < 20) scroll.LineUp();
            else if (sy > scroll.ActualHeight - 20) scroll.LineDown();
        }

        void EndReorder()
        {
            if (dragIndex < 0) return;
            dragIndex = -1;
            if (view.IsMouseCaptured) view.ReleaseMouseCapture();
            if (dragChanged) Save();
            EndChange(); // 拖动期间别处改了这张便签的话，这里会以外部内容为准
            Render();
            SendToBottom();
        }

        public void BeginEdit(int index)
        {
            CommitEdit(true);
            if (Collapsed) Expand();
            editIndex = index;
            editOrigText = index == TitleIndex ? d.Title : index >= 0 && index < d.Items.Count ? d.Items[index].Text : null;
            Activate();
            Render();
        }

        void CommitEdit(bool save)
        {
            if (!Editing) return;
            int idx = editIndex;
            // 条目内部换行统一存成 \n，去掉每行行尾空格（Tab 保留）
            string text = "";
            if (editBox != null)
            {
                var parts = editBox.Text.Replace("\r\n", "\n").Split('\n');
                for (int k = 0; k < parts.Length; k++) parts[k] = parts[k].TrimEnd(' ');
                text = string.Join("\n", parts).Trim('\n', ' ');
                if (idx == TitleIndex) text = text.Replace("\n", " ").Trim();
            }
            editIndex = NotEditing;
            editBox = null;
            if (pendingReload != null && changeDepth == 0)
            {
                // 编辑期间别处改了这张便签：先换成外部内容，再把这次编辑合进去
                var pr = pendingReload; pendingReload = null;
                ApplyExternal(pr);
                if (idx >= 0 && (idx >= d.Items.Count || d.Items[idx].Text != editOrigText) && editOrigText != null)
                {
                    idx = d.Items.FindIndex(x => x.Text == editOrigText);
                    if (idx < 0) idx = d.Items.Count; // 原条目在别处被改或删了：这次编辑作为新条目加上，不丢
                }
            }
            if (save)
            {
                BeginChange();
                if (idx == TitleIndex) d.Title = text.Length > 0 ? text : "ToDo";
                else if (idx < d.Items.Count)
                {
                    if (text.Trim().Length > 0) d.Items[idx].Text = text;
                    else TrashItemAt(idx); // 清空文字 = 删除，进回收站
                }
                else if (text.Trim().Length > 0) d.Items.Add(new NoteItem { Text = text });
                Save();
                EndChange();
            }
            Render();
            SendToBottom();
            if (IsActive) Focus(); // 让 Ctrl+Z 能被这张便签收到
        }

        // =====================================================================
        // 拖动 / 缩放窗口
        // =====================================================================
        const int Edge = 8;

        int HitEdge(Point p)
        {
            if (Collapsed) return 0;
            bool l = p.X < Edge, r = p.X > ActualWidth - Edge, t = p.Y < Edge, b = p.Y > ActualHeight - Edge;
            // WMSZ_*: 1 左 2 右 3 上 4 左上 5 右上 6 下 7 左下 8 右下
            if (t && l) return 4; if (t && r) return 5; if (b && l) return 7; if (b && r) return 8;
            if (l) return 1; if (r) return 2; if (t) return 3; if (b) return 6;
            return 0;
        }

        void OnCardMouseMove(object s, MouseEventArgs e)
        {
            if (dragIndex >= 0) return;
            switch (HitEdge(e.GetPosition(this)))
            {
                case 1: case 2: Cursor = Cursors.SizeWE; break;
                case 3: case 6: Cursor = Cursors.SizeNS; break;
                case 4: case 8: Cursor = Cursors.SizeNWSE; break;
                case 5: case 7: Cursor = Cursors.SizeNESW; break;
                default: Cursor = null; break;
            }
        }

        void OnCardMouseDown(object s, MouseButtonEventArgs e)
        {
            if (Collapsed)
            {
                // 胶囊：按住拖动；没拖动就是单击，展开
                ClosePreview();
                if (capHidden) { if (slideTimer != null) slideTimer.Stop(); sliding = false; Left = dockedLeft; Top = dockedTop; capHidden = false; }
                double x0 = Left, y0 = Top;
                DragMove();
                if (Math.Abs(Left - x0) < 3 && Math.Abs(Top - y0) < 3) Expand();
                else { SnapCapsule(); SaveLayout(); ScheduleCapsuleHide(); }
                SendToBottom();
                e.Handled = true;
                return;
            }
            int edge = HitEdge(e.GetPosition(this));
            if (edge != 0)
            {
                Native.ReleaseCapture();
                Native.SendMessage(hwnd, Native.WM_SYSCOMMAND, new IntPtr(0xF000 + edge), IntPtr.Zero);
                e.Handled = true;
                return;
            }
            // 正在编辑时点到别处：先保存这一条
            if (Editing) { CommitEdit(true); e.Handled = true; return; }
            Focus();
            if (TryOpenLink(e)) { e.Handled = true; return; }
            if (e.ClickCount == 2)
            {
                int tag = TagOf(e.OriginalSource as DependencyObject);
                BeginEdit(tag == NotEditing ? d.Items.Count : tag); // 双击空白处 = 新增一条
                e.Handled = true;
                return;
            }
            DragMove();
            SaveLayout();
            SendToBottom();
        }

    }
}

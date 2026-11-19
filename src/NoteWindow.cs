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
    class NoteWindow : Window
    {
        readonly DeskApp app;
        public readonly NoteData Data;
        NoteData d { get { return Data; } }

        // ---- 状态 ----
        IntPtr hwnd;
        const int NotEditing = -1, TitleIndex = -2;
        int editIndex = NotEditing;   // 正在编辑：标题 / 条目下标（等于条目数表示新增）
        TextBox editBox;
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
                Text = it.Text, FontFamily = UiFont, FontSize = d.FontSize,
                FontWeight = FontWeight.FromOpenTypeWeight(d.FontWeight),
                Foreground = TextBrush, TextWrapping = TextWrapping.Wrap, LineHeight = LineH
            };
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
                    if (index < d.Items.Count) { d.Items[index].Done = !d.Items[index].Done; Save(); Render(); }
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
        void Save() { if (!Deleted) Store.SaveNote(d); }

        // 外部改了这张便签的文件：重新读入
        public void ReloadFrom(NoteData fresh)
        {
            if (Editing || dragIndex >= 0) return; // 正在编辑或拖动时不打断
            bool wasCollapsed = d.Collapsed;
            d.Title = fresh.Title; d.Items = fresh.Items;
            d.Opacity = fresh.Opacity; d.FontSize = fresh.FontSize; d.FontWeight = fresh.FontWeight;
            d.TextColor = fresh.TextColor; d.BgColor = fresh.BgColor; d.Shadow = fresh.Shadow; d.Checkboxes = fresh.Checkboxes;
            ApplyOpacity();
            ApplyShadow();
            if (fresh.Mode != d.Mode) SetMode(fresh.Mode);
            if (fresh.Collapsed != wasCollapsed) { if (fresh.Collapsed) Collapse(); else Expand(); }
            Render();
        }

        // =====================================================================
        // 胶囊：折叠、吸边、贴边后自动缩进、悬停预览
        // =====================================================================
        void Collapse()
        {
            if (Collapsed) return;
            CommitEdit(true);
            d.Width = Width; d.Height = Height;
            // 便签本来贴着屏幕右/下边的话，折叠后胶囊也贴在那一边
            Rect wa = WorkAreaDip();
            bool keepRight = Math.Abs(wa.Right - (Left + ActualWidth)) < SnapDist;
            bool keepBottom = Math.Abs(wa.Bottom - (Top + ActualHeight)) < SnapDist;
            EnterCollapsed();
            Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(delegate
            {
                if (keepRight) Left = wa.Right - ActualWidth;
                if (keepBottom) Top = wa.Bottom - ActualHeight;
                SnapCapsule();
                SaveLayout();
                ScheduleCapsuleHide();
            }));
        }

        void EnterCollapsed()
        {
            d.Collapsed = true;
            collapseBtn = null;
            MinWidth = 0; MinHeight = 0;
            card.Child = capsule;
            card.CornerRadius = new CornerRadius(14);
            card.Padding = new Thickness(14, 5, 14, 6);
            card.ToolTip = null;
            ApplyOpacity();
            RenderCapsule();
            SizeToContent = SizeToContent.WidthAndHeight;
            Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(delegate { SnapCapsule(); ScheduleCapsuleHide(); }));
        }

        int UndoneCount()
        {
            int c = 0;
            foreach (var it in d.Items) if (!d.Checkboxes || !it.Done) c++;
            return c;
        }

        void RenderCapsule()
        {
            capsule.Children.Clear();
            double size = Math.Min(d.FontSize, 15);
            capsule.Children.Add(new TextBlock
            {
                Text = d.Title, FontFamily = UiFont, FontSize = size, FontWeight = FontWeights.Bold,
                Foreground = TextBrush, MaxWidth = 220, TextTrimming = TextTrimming.CharacterEllipsis
            });
            capsule.Children.Add(new TextBlock
            {
                Text = "  " + UndoneCount(), FontFamily = UiFont, FontSize = size, Foreground = DimBrush
            });
            if (preview != null && preview.IsOpen) FillPreview();
        }

        void Expand()
        {
            if (!Collapsed) return;
            hideTimer.Stop();
            ClosePreview();
            if (capHidden) { Left = dockedLeft; Top = dockedTop; capHidden = false; }
            // 贴在右/下边的胶囊，展开时朝屏幕里面展开
            Rect wa = WorkAreaDip();
            double capRight = Left + ActualWidth, capBottom = Top + ActualHeight;
            bool wasR = dockR, wasB = dockB;
            d.Collapsed = false;
            SizeToContent = SizeToContent.Manual;
            MinWidth = 160; MinHeight = 120;
            Width = d.Width; Height = d.Height;
            card.Child = scroll;
            card.CornerRadius = new CornerRadius(4);
            card.Padding = new Thickness(20, 16, 20, 16);
            ApplyOpacity();
            if (wasR) Left = capRight - Width;
            if (wasB) Top = capBottom - Height;
            // 不跑出当前屏幕
            if (Left + Width > wa.Right) Left = wa.Right - Width;
            if (Top + Height > wa.Bottom) Top = wa.Bottom - Height;
            if (Left < wa.Left) Left = wa.Left;
            if (Top < wa.Top) Top = wa.Top;
            Render();
            SaveLayout();
        }

        // ---- 吸边 ----
        const double SnapDist = 24;
        bool dockL, dockR, dockT, dockB;

        double DpiScale()
        {
            var src = PresentationSource.FromVisual(this);
            return src != null && src.CompositionTarget != null ? src.CompositionTarget.TransformToDevice.M11 : 1;
        }

        // 窗口中心所在显示器的工作区（不含任务栏），换算成 WPF 单位
        Rect WorkAreaDip()
        {
            double s = DpiScale();
            var center = new System.Drawing.Point((int)((Left + ActualWidth / 2) * s), (int)((Top + ActualHeight / 2) * s));
            var wa = System.Windows.Forms.Screen.FromPoint(center).WorkingArea;
            return new Rect(wa.Left / s, wa.Top / s, wa.Width / s, wa.Height / s);
        }

        // 胶囊离屏幕边缘不到 SnapDist 就贴上去；跑出屏幕的部分拉回来；贴着的一侧改成直角
        void SnapCapsule()
        {
            if (!Collapsed || capHidden) return;
            Rect wa = WorkAreaDip();
            double w = ActualWidth, h = ActualHeight;
            if (Left - wa.Left < SnapDist) Left = wa.Left;
            else if (wa.Right - (Left + w) < SnapDist) Left = wa.Right - w;
            if (Top - wa.Top < SnapDist) Top = wa.Top;
            else if (wa.Bottom - (Top + h) < SnapDist) Top = wa.Bottom - h;

            dockL = Math.Abs(Left - wa.Left) < 0.5;
            dockR = Math.Abs(Left + w - wa.Right) < 0.5;
            dockT = Math.Abs(Top - wa.Top) < 0.5;
            dockB = Math.Abs(Top + h - wa.Bottom) < 0.5;
            const double r = 14;
            card.CornerRadius = new CornerRadius(
                dockL || dockT ? 0 : r, dockR || dockT ? 0 : r,
                dockR || dockB ? 0 : r, dockL || dockB ? 0 : r);
        }

        // ---- 贴边后自动缩进，只露出一条边；鼠标移上去滑出来 ----
        const double PeekStrip = 5;
        readonly DispatcherTimer hideTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(700) };
        bool capHidden, sliding;
        double dockedLeft, dockedTop;
        DispatcherTimer slideTimer;

        void ScheduleCapsuleHide()
        {
            if (!Collapsed) return;
            hideTimer.Stop();
            hideTimer.Start();
        }

        void OnHideTimer(object s, EventArgs e)
        {
            hideTimer.Stop();
            if (!Collapsed || card.IsMouseOver || (preview != null && previewCard.IsMouseOver) || peeking) return;
            ClosePreview();
            if (capHidden || !(dockL || dockR || dockT || dockB)) return;
            Rect wa = WorkAreaDip();
            double w = ActualWidth, h = ActualHeight, x = Left, y = Top;
            // 优先左右缩进；缩进方向那边要是挨着另一块屏幕就不缩，免得跑到那块屏上
            if (dockL && !OtherScreenAt(wa.Left - 2, Top + h / 2)) x = wa.Left - w + PeekStrip;
            else if (dockR && !OtherScreenAt(wa.Right + 2, Top + h / 2)) x = wa.Right - PeekStrip;
            else if (dockT && !OtherScreenAt(Left + w / 2, wa.Top - 2)) y = wa.Top - h + PeekStrip;
            else if (dockB && !OtherScreenAt(Left + w / 2, wa.Bottom + 2)) y = wa.Bottom - PeekStrip;
            else return;
            dockedLeft = Left; dockedTop = Top;
            capHidden = true;
            SlideTo(x, y);
        }

        bool OtherScreenAt(double xDip, double yDip)
        {
            double s = DpiScale();
            var p = new System.Drawing.Point((int)(xDip * s), (int)(yDip * s));
            foreach (var sc in System.Windows.Forms.Screen.AllScreens)
                if (sc.Bounds.Contains(p)) return true;
            return false;
        }

        void OnCapsuleEnter()
        {
            hideTimer.Stop();
            if (dragIndex >= 0) return;
            if (capHidden)
            {
                capHidden = false;
                SlideTo(dockedLeft, dockedTop);
            }
            ShowPreview();
        }

        // 缓动移动窗口（约 150ms）
        void SlideTo(double x, double y)
        {
            if (slideTimer != null) slideTimer.Stop();
            double x0 = Left, y0 = Top;
            var start = DateTime.Now;
            const double dur = 150;
            sliding = true;
            slideTimer = new DispatcherTimer(DispatcherPriority.Render) { Interval = TimeSpan.FromMilliseconds(8) };
            slideTimer.Tick += delegate
            {
                double t = Math.Min(1, (DateTime.Now - start).TotalMilliseconds / dur);
                double k = 1 - Math.Pow(1 - t, 3); // ease-out
                Left = x0 + (x - x0) * k;
                Top = y0 + (y - y0) * k;
                if (t >= 1) { slideTimer.Stop(); sliding = false; }
            };
            slideTimer.Start();
        }

        // ---- 悬停预览卡片 ----
        Popup preview;
        Border previewCard;
        StackPanel previewList;

        void ShowPreview()
        {
            if (!Collapsed || dragIndex >= 0) return;
            if (preview == null)
            {
                previewList = new StackPanel();
                previewCard = new Border
                {
                    CornerRadius = new CornerRadius(6), Padding = new Thickness(16, 12, 16, 10),
                    MaxWidth = 340, MaxHeight = 460, Child = new ScrollViewer
                    {
                        Content = previewList, VerticalScrollBarVisibility = ScrollBarVisibility.Hidden, Focusable = false
                    },
                    Cursor = Cursors.Hand, ToolTip = "单击展开"
                };
                previewCard.MouseLeftButtonDown += delegate(object s, MouseButtonEventArgs e) { e.Handled = true; Expand(); };
                previewCard.MouseEnter += delegate { hideTimer.Stop(); };
                previewCard.MouseLeave += delegate { ScheduleCapsuleHide(); };
                preview = new Popup
                {
                    Child = previewCard, AllowsTransparency = true, StaysOpen = true,
                    PlacementTarget = card, PopupAnimation = PopupAnimation.Fade
                };
            }
            // 往屏幕里面的方向弹出
            preview.Placement = dockR ? PlacementMode.Left : dockL ? PlacementMode.Right : dockB ? PlacementMode.Top : PlacementMode.Bottom;
            preview.HorizontalOffset = preview.Placement == PlacementMode.Left ? -6 : preview.Placement == PlacementMode.Right ? 6 : 0;
            preview.VerticalOffset = preview.Placement == PlacementMode.Top ? -6 : preview.Placement == PlacementMode.Bottom ? 6 : 0;
            FillPreview();
            preview.IsOpen = true;
        }

        void FillPreview()
        {
            previewCard.Background = new SolidColorBrush(BgWithAlpha(Math.Max(d.Opacity, 0.92)));
            previewList.Children.Clear();
            previewList.Children.Add(new TextBlock
            {
                Text = d.Title, FontFamily = UiFont, FontSize = Math.Min(TitleSize, 18), FontWeight = FontWeights.Bold,
                Foreground = TextBrush, Margin = new Thickness(0, 0, 0, 8), TextTrimming = TextTrimming.CharacterEllipsis
            });
            if (d.Items.Count == 0)
                previewList.Children.Add(new TextBlock { Text = "（空）", FontFamily = UiFont, FontSize = d.FontSize, Foreground = DimBrush });
            foreach (var it in d.Items)
            {
                var row = new Grid { Margin = new Thickness(0, 0, 0, Math.Round(d.FontSize * 0.35)) };
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                row.ColumnDefinitions.Add(new ColumnDefinition());
                row.Children.Add(new TextBlock
                {
                    Text = d.Checkboxes && it.Done ? "✓" : "•", FontFamily = UiFont, FontSize = d.FontSize, Foreground = DimBrush,
                    Margin = new Thickness(0, 0, 8, 0), LineHeight = LineH
                });
                var tb = MakeItemText(it);
                Grid.SetColumn(tb, 1);
                row.Children.Add(tb);
                previewList.Children.Add(row);
            }
        }

        void ClosePreview()
        {
            if (preview != null) preview.IsOpen = false;
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
            menu.Items.Add(up);
            menu.Items.Add(down);
            menu.Items.Add(del);
            menu.Items.Add(add);
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
            menu.Closed += delegate { Save(); if (Collapsed) ScheduleCapsuleHide(); };
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
            app.TrashItem(d, i, d.Items[i]);
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
            Render();
            SendToBottom();
        }

        public void BeginEdit(int index)
        {
            CommitEdit(true);
            if (Collapsed) Expand();
            editIndex = index;
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
            if (save)
            {
                if (idx == TitleIndex) d.Title = text.Length > 0 ? text : "ToDo";
                else if (idx < d.Items.Count)
                {
                    if (text.Trim().Length > 0) d.Items[idx].Text = text;
                    else TrashItemAt(idx); // 清空文字 = 删除，进回收站
                }
                else if (text.Trim().Length > 0) d.Items.Add(new NoteItem { Text = text });
                Save();
            }
            Render();
            SendToBottom();
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

        // =====================================================================
        // 位置：用物理像素保存和恢复，多块屏幕缩放不同时也准
        // =====================================================================
        void RestorePosition()
        {
            if (d.X.HasValue && d.Y.HasValue)
            {
                int x = (int)d.X.Value, y = (int)d.Y.Value;
                // 位置得落在某块屏幕上（显示器可能已经断开）
                var probe = new System.Drawing.Rectangle(x, y, 40, 20);
                foreach (var sc in System.Windows.Forms.Screen.AllScreens)
                    if (sc.WorkingArea.IntersectsWith(probe))
                    {
                        Native.SetWindowPos(hwnd, IntPtr.Zero, x, y, 0, 0, Native.SWP_NOSIZE | Native.SWP_NOZORDER | Native.SWP_NOACTIVATE);
                        return;
                    }
            }
            PlaceDefault();
        }

        void PlaceDefault()
        {
            var wa = System.Windows.Forms.Screen.PrimaryScreen.WorkingArea;
            int offset = (int)(app.NoteIndex(this) * 30 * DpiScale());
            Native.SetWindowPos(hwnd, IntPtr.Zero, wa.Left + (int)(80 * DpiScale()) + offset, wa.Top + (int)(80 * DpiScale()) + offset, 0, 0,
                Native.SWP_NOSIZE | Native.SWP_NOZORDER | Native.SWP_NOACTIVATE);
        }

        // 把跑到屏幕外的便签拉回主屏
        public void BringOnScreen()
        {
            if (hwnd == IntPtr.Zero) return;
            if (capHidden) { Left = dockedLeft; Top = dockedTop; capHidden = false; }
            Native.RECT r;
            Native.GetWindowRect(hwnd, out r);
            var rect = new System.Drawing.Rectangle(r.Left, r.Top, Math.Max(1, r.Right - r.Left), Math.Max(1, r.Bottom - r.Top));
            bool visible = false;
            foreach (var sc in System.Windows.Forms.Screen.AllScreens)
            {
                var inter = System.Drawing.Rectangle.Intersect(sc.WorkingArea, rect);
                if (inter.Width >= Math.Min(rect.Width, 60) && inter.Height >= Math.Min(rect.Height, 20)) visible = true;
            }
            if (!visible) { PlaceDefault(); if (Collapsed) SnapCapsule(); }
            SaveLayout();
        }

        public void SaveLayout()
        {
            if (hwnd == IntPtr.Zero) return;
            if (!Collapsed && SizeToContent == SizeToContent.Manual) { d.Width = Width; d.Height = Height; }
            if (capHidden)
            {
                double s = DpiScale();
                d.X = dockedLeft * s; d.Y = dockedTop * s;
            }
            else
            {
                Native.RECT r;
                if (Native.GetWindowRect(hwnd, out r)) { d.X = r.Left; d.Y = r.Top; }
            }
            Save();
        }

        // =====================================================================
        // 窗口模式：置底在桌面 / 置顶 / 普通窗口；快捷键临时提到最前
        // =====================================================================
        void OnSourceInitialized(object s, EventArgs e)
        {
            hwnd = new WindowInteropHelper(this).Handle;
            HwndSource.FromHwnd(hwnd).AddHook(WndProc);
            // 资源管理器重启后会广播这条消息，借此重新挂到新的桌面窗口上
            taskbarCreatedMsg = Native.RegisterWindowMessage("TaskbarCreated");
            RestorePosition();
            ApplyMode();
        }

        void SetMode(WinMode m)
        {
            if (m == d.Mode) return;
            d.Mode = m;
            // 任务栏按钮要在窗口重新显示时才会增减
            Hide();
            ApplyMode();
            Show();
        }

        void ApplyMode()
        {
            if (hwnd == IntPtr.Zero) return;
            long ex = Native.GetWindowLongPtr(hwnd, Native.GWL_EXSTYLE).ToInt64();
            if (d.Mode == WinMode.Normal) ex = (ex & ~Native.WS_EX_TOOLWINDOW) | Native.WS_EX_APPWINDOW;   // 进任务栏和 Alt+Tab
            else ex = (ex | Native.WS_EX_TOOLWINDOW) & ~Native.WS_EX_APPWINDOW;                           // 不进任务栏和 Alt+Tab
            Native.SetWindowLongPtr(hwnd, Native.GWL_EXSTYLE, new IntPtr(ex));

            if (d.Mode == WinMode.Desktop)
            {
                Topmost = peeking;
                AttachToDesktop();
            }
            else
            {
                Native.SetWindowLongPtr(hwnd, Native.GWLP_HWNDPARENT, IntPtr.Zero);
                Topmost = d.Mode == WinMode.Topmost || peeking;
            }
            Native.SetWindowPos(hwnd, IntPtr.Zero, 0, 0, 0, 0,
                Native.SWP_NOMOVE | Native.SWP_NOSIZE | Native.SWP_NOZORDER | Native.SWP_NOACTIVATE | Native.SWP_FRAMECHANGED);
        }

        // 置底模式：让桌面(Progman)成为所有者，按 Win+D 显示桌面时不会被一起藏起来。
        // 开机自启时桌面可能还没建好，找不到就每 2 秒重试，最多 1 分钟
        void AttachToDesktop()
        {
            IntPtr progman = Native.FindWindow("Progman", null);
            if (progman != IntPtr.Zero)
            {
                Native.SetWindowLongPtr(hwnd, Native.GWLP_HWNDPARENT, progman);
                SendToBottom();
                return;
            }
            int tries = 0;
            var timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
            timer.Tick += delegate
            {
                IntPtr p = Native.FindWindow("Progman", null);
                if (d.Mode != WinMode.Desktop || p != IntPtr.Zero || ++tries >= 30)
                {
                    timer.Stop();
                    if (d.Mode == WinMode.Desktop && p != IntPtr.Zero) { Native.SetWindowLongPtr(hwnd, Native.GWLP_HWNDPARENT, p); SendToBottom(); }
                }
            };
            timer.Start();
        }

        public void BeginPeek()
        {
            peeking = true;
            hideTimer.Stop();
            if (capHidden) { if (slideTimer != null) slideTimer.Stop(); sliding = false; Left = dockedLeft; Top = dockedTop; capHidden = false; }
            // 挂在桌面窗口下的便签设不了置顶，先摘下来，结束时再挂回去
            if (hwnd != IntPtr.Zero) Native.SetWindowLongPtr(hwnd, Native.GWLP_HWNDPARENT, IntPtr.Zero);
            Topmost = false;
            Topmost = true;
        }

        public void EndPeek()
        {
            if (!peeking) return;
            peeking = false;
            CommitEdit(true);
            ApplyMode();
            if (Collapsed) ScheduleCapsuleHide();
        }

        IntPtr WndProc(IntPtr h, int msg, IntPtr w, IntPtr l, ref bool handled)
        {
            if (taskbarCreatedMsg != 0 && msg == taskbarCreatedMsg)
            {
                if (d.Mode == WinMode.Desktop) AttachToDesktop();
            }
            else if (msg == Native.WM_WINDOWPOSCHANGING && d.Mode == WinMode.Desktop && !Editing && !peeking)
            {
                // 置底模式下（非编辑时）始终压在其他窗口下面
                var pos = (Native.WINDOWPOS)Marshal.PtrToStructure(l, typeof(Native.WINDOWPOS));
                if ((pos.flags & Native.SWP_NOZORDER) == 0)
                {
                    pos.hwndInsertAfter = Native.HWND_BOTTOM;
                    Marshal.StructureToPtr(pos, l, false);
                }
            }
            else if (msg == Native.WM_EXITSIZEMOVE)
            {
                SaveLayout();
            }
            return IntPtr.Zero;
        }

        void SendToBottom()
        {
            if (hwnd != IntPtr.Zero && d.Mode == WinMode.Desktop && !Editing && !peeking)
                Native.SetWindowPos(hwnd, Native.HWND_BOTTOM, 0, 0, 0, 0, Native.SWP_NOMOVE | Native.SWP_NOSIZE | Native.SWP_NOACTIVATE);
        }
    }
}

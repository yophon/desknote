// 便签窗口：折叠成胶囊、吸边、自动缩进、悬停预览
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
    partial class NoteWindow
    {
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

        public void Expand()
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

    }
}

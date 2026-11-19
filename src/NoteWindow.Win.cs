// 便签窗口：位置保存恢复、窗口模式（置底/置顶/普通）、临时提到最前
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

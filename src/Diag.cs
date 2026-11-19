// 内存诊断（开发用）：环境变量 DESKNOTE_MEMDIAG=<结果文件> 时，启动后依次做一些常见操作，
// 每一步后记下进程内存，最后退出。只允许在 --data 测试目录下用。由 tests\memdiag.ps1 调用。
using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Threading;

namespace DeskNote
{
    static class MemDiag
    {
        static StringBuilder log;

        static void Pump(int ms)
        {
            var frame = new DispatcherFrame();
            var t = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(ms) };
            t.Tick += delegate { t.Stop(); frame.Continue = false; };
            t.Start();
            Dispatcher.PushFrame(frame);
        }

        static void Sample(string step)
        {
            Pump(1500);
            long managed = GC.GetTotalMemory(true);
            var p = Process.GetCurrentProcess();
            p.Refresh();
            log.AppendLine(string.Format("{0,-28} private={1,6:N1}MB  WS={2,6:N1}MB  managed={3,5:N1}MB  handles={4}  threads={5}",
                step, p.PrivateMemorySize64 / 1048576.0, p.WorkingSet64 / 1048576.0, managed / 1048576.0, p.HandleCount, p.Threads.Count));
        }

        public static void Run(DeskApp app, string outFile)
        {
            log = new StringBuilder();
            log.AppendLine("render=" + System.Windows.Media.RenderCapability.Tier / 0x10000 + " mode=" + System.Windows.Media.RenderOptions.ProcessRenderMode);
            Sample("启动后");
            Pump(5000);
            Sample("空闲 5 秒后");
            var w = app.Notes[0];
            for (int i = 0; i < 3; i++) { app.ShowSettings(); Pump(300); foreach (Window x in Application.Current.Windows) if (x is SettingsWindow) x.Close(); }
            Sample("开关设置窗口 3 次");
            for (int i = 0; i < 3; i++) { app.ShowTrash(); Pump(300); foreach (Window x in Application.Current.Windows) if (x is TrashWindow) x.Close(); }
            Sample("开关回收站 3 次");
            for (int i = 0; i < 3; i++) { w.TestOpenMenu(); Pump(300); w.TestCloseMenu(); }
            Sample("开关右键菜单 3 次");
            for (int i = 0; i < 200; i++) { w.TestRender(); if (i % 20 == 0) Pump(10); }
            Sample("重绘 200 次");
            app.TogglePeek(); Pump(300); app.TogglePeek();
            Sample("临时置顶一次");
            File.WriteAllText(outFile, log.ToString(), new UTF8Encoding(false));
            app.Exit();
        }
    }

    partial class NoteWindow
    {
        internal void TestOpenMenu() { card.ContextMenu.PlacementTarget = card; card.ContextMenu.IsOpen = true; }
        internal void TestCloseMenu() { card.ContextMenu.IsOpen = false; }
        internal void TestRender() { Render(); }
    }
}

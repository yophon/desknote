// 程序主体：多张便签、托盘图标、全局快捷键、回收站、单实例
using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Threading;
using System.Windows;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using Microsoft.Win32;

[assembly: System.Runtime.Versioning.TargetFramework(".NETFramework,Version=v4.8", FrameworkDisplayName = ".NET Framework 4.8")]
[assembly: AssemblyTitle("DeskNote")]
[assembly: AssemblyProduct("DeskNote")]
[assembly: AssemblyVersion(DeskNote.DeskApp.Version + ".0")]
[assembly: AssemblyFileVersion(DeskNote.DeskApp.Version + ".0")]

namespace DeskNote
{
    class DeskApp
    {
        // 改版本号就改这里（major.minor.patch）
        public const string Version = "1.2.0";
        // 编译时间：取 exe 文件的修改时间
        public static DateTime BuildTime { get { try { return File.GetLastWriteTime(Assembly.GetExecutingAssembly().Location); } catch { return DateTime.MinValue; } } }

        public readonly List<NoteWindow> Notes = new List<NoteWindow>();
        public List<TrashEntry> Trash;
        public GlobalSettings Settings;
        public ImageSource WindowIcon;
        public event Action TrashChanged;
        public Syncer Sync;
        public readonly bool IsDefaultDataDir;

        readonly Application wpf;
        System.Windows.Forms.NotifyIcon tray;
        HwndSource msgWindow;
        FileSystemWatcher watcher;
        TrashWindow trashWindow;
        SettingsWindow settingsWindow;
        bool peeking, exiting;
        const int HotkeyId = 1;
        const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";

        public DeskApp(Application wpf, bool isDefaultDataDir)
        {
            this.wpf = wpf;
            IsDefaultDataDir = isDefaultDataDir;
        }

        public void Start()
        {
            WindowIcon = LoadWindowIcon();
            Store.MigrateLegacy();
            Settings = Store.LoadSettings();
            Trash = Store.LoadTrash();

            var list = Store.LoadNotes();
            if (!Settings.AutoStartInit)
            {
                // 全新安装（还没有任何便签）默认开机自启；已有数据的老用户保持原样
                if (list.Count == 0 && IsDefaultDataDir)
                {
                    try { SetAutoStart(true); }
                    catch (Exception ex) { Store.Log("设置开机自启失败：" + ex.Message); }
                }
                Settings.AutoStartInit = true;
                Store.SaveSettings(Settings);
            }
            // exe 挪过位置后，把开机自启指向现在这个 exe（只在已打开自启时更新，不会替用户重新打开）
            if (IsDefaultDataDir) FixAutoStartPath();
            if (list.Count == 0)
            {
                var n = new NoteData { Id = NoteData.NewId() };
                Store.SaveNote(n);
                list.Add(n);
            }
            foreach (var n in list) OpenWindow(n);
            Store.DailyBackup();

            CreateTray();
            CreateMessageWindow();
            ApplyHotkey();
            WatchNotes();
            Sync = new Syncer(this, wpf.Dispatcher);
            Sync.Start();
            // 程序失去焦点（点了别处）时结束“临时提到最前”
            wpf.Deactivated += delegate { if (peeking) EndPeek(); };

            // 每 15 秒检查一次到点的提醒；顺便每天做一次备份（程序可能连开好几天）
            var tick = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromSeconds(15) };
            tick.Tick += delegate { CheckReminders(); if (DateTime.Now.Minute == 0 || lastBackupDay != DateTime.Today) { lastBackupDay = DateTime.Today; Store.DailyBackup(); } };
            tick.Start();
            CheckReminders(); // 关机期间错过的提醒，启动后马上补
        }

        DateTime lastBackupDay = DateTime.Today;

        // ---------------------------------------------------------------
        // 提醒：到点后弹系统通知，条目上的提醒标记变红
        // ---------------------------------------------------------------
        NoteWindow balloonNote;

        public void CheckReminders()
        {
            var now = DateTime.Now;
            foreach (var w in Notes.ToArray())
            {
                var due = new List<NoteItem>();
                foreach (var it in w.Data.Items)
                    if (it.Remind.HasValue && !it.Reminded && !it.Done && it.Remind.Value <= now) due.Add(it);
                if (due.Count == 0) continue;
                foreach (var it in due) it.Reminded = true;
                w.OnRemindersFired();
                var first = due[0];
                string text = first.Text.Replace("\n", " ");
                if (text.Length > 60) text = text.Substring(0, 60) + "…";
                if (due.Count > 1) text += "（另有 " + (due.Count - 1) + " 条）";
                ShowNotice("提醒 · " + w.Data.Title, text, w);
            }
        }

        public bool Silent; // 自测时不弹系统通知

        public void ShowNotice(string title, string text, NoteWindow w)
        {
            if (Silent) return;
            balloonNote = w;
            if (tray != null) tray.ShowBalloonTip(10000, title, text, System.Windows.Forms.ToolTipIcon.Info);
        }

        NoteWindow OpenWindow(NoteData n)
        {
            var w = new NoteWindow(this, n);
            Notes.Add(w);
            w.Show();
            return w;
        }

        public int NoteIndex(NoteWindow w) { return Math.Max(0, Notes.IndexOf(w)); }

        // 便签内容存盘后通知同步（内容没变的话同步器自己会忽略）
        public void OnNoteSaved() { if (Sync != null) Sync.Touch(); }

        // 同步拉下来一张本地没有的便签
        public void OpenSyncedNote(NoteData n)
        {
            Store.Revive(n.Id);
            Store.SaveNote(n);
            OpenWindow(n);
        }

        // 同步冲突时，把本地那份另存成一张新便签
        public void CreateNoteFromContent(string contentJson, string titleSuffix)
        {
            var n = NoteData.FromContentJson(contentJson, NoteData.NewId());
            n.Title += titleSuffix;
            Store.SaveNote(n);
            OpenWindow(n);
        }

        static ImageSource LoadWindowIcon()
        {
            try
            {
                using (var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("DeskNote.ico"))
                    return s == null ? null : BitmapFrame.Create(s, BitmapCreateOptions.None, BitmapCacheOption.OnLoad);
            }
            catch { return null; }
        }

        // ---------------------------------------------------------------
        // 便签增删
        // ---------------------------------------------------------------
        public void CreateNote(NoteWindow from)
        {
            var n = from != null ? from.Data.CloneStyle() : new NoteData { Id = NoteData.NewId(), Title = "新便签" };
            n.Collapsed = false;
            if (from != null)
            {
                // 放在原便签右下方一点（位置是物理像素）
                from.SaveLayout();
                if (from.Data.X.HasValue && !from.Data.Collapsed) { n.X = from.Data.X + 60; n.Y = from.Data.Y + 60; }
            }
            Store.SaveNote(n);
            var w = OpenWindow(n);
            w.BeginEdit(0);
        }

        public void DeleteNote(NoteWindow w) { DeleteNote(w, true); }

        public void DeleteNote(NoteWindow w, bool confirm)
        {
            if (confirm)
            {
                var r = MessageBox.Show("把便签「" + w.Data.Title + "」（" + w.Data.Items.Count + " 条）移到回收站？\n之后可以在回收站里恢复。",
                    "删除便签", MessageBoxButton.OKCancel, MessageBoxImage.Question);
                if (r != MessageBoxResult.OK) return;
            }
            w.SaveLayout();
            w.Deleted = true;
            Trash.Insert(0, new TrashEntry { Kind = "note", Note = w.Data });
            SaveTrash();
            Notes.Remove(w);
            Store.DeleteNoteFile(w.Data.Id);
            w.AllowClose = true;
            w.Close();
            OnNoteSaved();
        }

        // ---------------------------------------------------------------
        // 回收站
        // ---------------------------------------------------------------
        public TrashEntry TrashItem(NoteData note, int index, NoteItem item)
        {
            var e = new TrashEntry { Kind = "item", NoteId = note.Id, NoteTitle = note.Title, Index = index, Item = item.Clone() };
            Trash.Insert(0, e);
            SaveTrash();
            return e;
        }

        // 撤销删除时把对应的回收站记录拿掉；重做时放回去
        public void UntrashEntries(List<TrashEntry> list)
        {
            if (list.Count == 0) return;
            foreach (var e in list) Trash.Remove(e);
            SaveTrash();
        }

        public void RetrashEntries(List<TrashEntry> list)
        {
            if (list.Count == 0) return;
            foreach (var e in list) if (!Trash.Contains(e)) Trash.Insert(0, e);
            SaveTrash();
        }

        void SaveTrash()
        {
            Store.SaveTrash(Trash);
            if (TrashChanged != null) TrashChanged();
        }

        public NoteWindow FindNote(string id)
        {
            foreach (var w in Notes) if (w.Data.Id == id) return w;
            return null;
        }

        public void Restore(TrashEntry e)
        {
            if (e.Kind == "note")
            {
                var n = e.Note;
                if (FindNote(n.Id) != null || File.Exists(Store.NotePath(n.Id))) n.Id = NoteData.NewId();
                Store.Revive(n.Id);
                Store.SaveNote(n);
                OpenWindow(n);
            }
            else
            {
                // 原便签还在就放回原处，否则放进第一张便签，一张都没有就新建
                var w = FindNote(e.NoteId);
                if (w == null && Notes.Count > 0) w = Notes[0];
                if (w == null)
                {
                    var n = new NoteData { Id = NoteData.NewId(), Title = string.IsNullOrEmpty(e.NoteTitle) ? "ToDo" : e.NoteTitle };
                    Store.SaveNote(n);
                    w = OpenWindow(n);
                }
                w.RestoreItem(e.Item.Clone(), w.Data.Id == e.NoteId ? e.Index : w.Data.Items.Count);
            }
            Trash.Remove(e);
            SaveTrash();
        }

        public void PurgeTrash(TrashEntry e)
        {
            Trash.Remove(e);
            SaveTrash();
        }

        public void EmptyTrash()
        {
            Trash.Clear();
            SaveTrash();
        }

        public void ShowTrash()
        {
            if (trashWindow == null)
            {
                trashWindow = new TrashWindow(this) { Icon = WindowIcon };
                trashWindow.Closed += delegate { trashWindow = null; };
                trashWindow.Show();
            }
            trashWindow.Activate();
        }

        public void ShowSettings()
        {
            if (settingsWindow == null)
            {
                settingsWindow = new SettingsWindow(this) { Icon = WindowIcon };
                settingsWindow.Closed += delegate { settingsWindow = null; };
                settingsWindow.Show();
            }
            settingsWindow.Activate();
        }

        // ---------------------------------------------------------------
        // 临时把所有便签提到最前（快捷键 / 单击托盘）
        // ---------------------------------------------------------------
        public void TogglePeek()
        {
            if (peeking) { EndPeek(); return; }
            if (Notes.Count == 0) return;
            peeking = true;
            foreach (var w in Notes) w.BeginPeek();
            var last = Notes[Notes.Count - 1];
            last.Activate();
            Native.SetForegroundWindow(new WindowInteropHelper(last).Handle);
        }

        void TogglePeekIfNot() { if (!peeking) TogglePeek(); }

        void EndPeek()
        {
            peeking = false;
            foreach (var w in Notes) w.EndPeek();
        }

        public void BringAllOnScreen()
        {
            foreach (var w in Notes) w.BringOnScreen();
        }

        // ---------------------------------------------------------------
        // 托盘
        // ---------------------------------------------------------------
        void CreateTray()
        {
            tray = new System.Windows.Forms.NotifyIcon { Text = "DeskNote " + Version, Visible = true };
            try
            {
                using (var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("DeskNote.ico"))
                    tray.Icon = new System.Drawing.Icon(s, System.Windows.Forms.SystemInformation.SmallIconSize);
            }
            catch (Exception ex) { Store.Log("托盘图标：" + ex.Message); tray.Icon = System.Drawing.SystemIcons.Application; }

            var menu = new System.Windows.Forms.ContextMenuStrip();
            menu.Items.Add("显示所有便签", null, delegate { TogglePeek(); });
            menu.Items.Add("新建便签", null, delegate { CreateNote(Notes.Count > 0 ? Notes[Notes.Count - 1] : null); });
            menu.Items.Add("把便签拉回屏幕", null, delegate { BringAllOnScreen(); });
            menu.Items.Add(new System.Windows.Forms.ToolStripSeparator());
            menu.Items.Add("回收站…", null, delegate { ShowTrash(); });
            menu.Items.Add("设置…", null, delegate { ShowSettings(); });
            menu.Items.Add(new System.Windows.Forms.ToolStripSeparator());
            menu.Items.Add("退出", null, delegate { Exit(); });
            tray.ContextMenuStrip = menu;
            // 点通知：把那张便签提到最前
            tray.BalloonTipClicked += delegate { if (balloonNote != null && Notes.Contains(balloonNote)) { balloonNote.Expand(); TogglePeekIfNot(); } };
            tray.MouseClick += delegate(object s, System.Windows.Forms.MouseEventArgs e)
            {
                if (e.Button == System.Windows.Forms.MouseButtons.Left) TogglePeek();
            };
        }

        // ---------------------------------------------------------------
        // 全局快捷键
        // ---------------------------------------------------------------
        void CreateMessageWindow()
        {
            var p = new HwndSourceParameters("DeskNoteMessages") { ParentWindow = Native.HWND_MESSAGE, Width = 0, Height = 0, WindowStyle = 0 };
            msgWindow = new HwndSource(p);
            msgWindow.AddHook(delegate(IntPtr h, int msg, IntPtr w, IntPtr l, ref bool handled)
            {
                if (msg == Native.WM_HOTKEY && w.ToInt32() == HotkeyId) { TogglePeek(); handled = true; }
                return IntPtr.Zero;
            });
        }

        // 返回 null 表示成功，否则是失败原因
        public string ApplyHotkey()
        {
            Native.UnregisterHotKey(msgWindow.Handle, HotkeyId);
            if (!Settings.HotkeyEnabled) return null;
            uint mods; Key key;
            if (!Hotkeys.TryParse(Settings.Hotkey, out mods, out key)) return "快捷键格式不对";
            uint vk = (uint)KeyInterop.VirtualKeyFromKey(key);
            if (!Native.RegisterHotKey(msgWindow.Handle, HotkeyId, mods | Native.MOD_NOREPEAT, vk))
                return "注册失败，可能被其他软件占用了";
            return null;
        }

        // ---------------------------------------------------------------
        // 外部修改便签文件时重新读入
        // ---------------------------------------------------------------
        void WatchNotes()
        {
            try
            {
                watcher = new FileSystemWatcher(Store.NotesDir, "*.json");
                watcher.NotifyFilter = NotifyFilters.LastWrite | NotifyFilters.FileName | NotifyFilters.Size;
                FileSystemEventHandler onChange = delegate(object s, FileSystemEventArgs e)
                {
                    string id = Path.GetFileNameWithoutExtension(e.Name);
                    wpf.Dispatcher.BeginInvoke(new Action(delegate { OnNoteFileChanged(id); }));
                };
                watcher.Changed += onChange;
                watcher.Created += onChange;
                watcher.Renamed += delegate(object s, RenamedEventArgs e)
                {
                    string id = Path.GetFileNameWithoutExtension(e.Name);
                    wpf.Dispatcher.BeginInvoke(new Action(delegate { OnNoteFileChanged(id); }));
                };
                watcher.EnableRaisingEvents = true;
            }
            catch (Exception ex) { Store.Log("watch: " + ex.Message); }
        }

        void OnNoteFileChanged(string id)
        {
            if (exiting) return;
            var fresh = Store.ReadIfChangedExternally(id);
            if (fresh == null) return;
            var w = FindNote(id);
            if (w != null) w.ReloadFrom(fresh);
            else if (!Store.IsDeleted(id)) OpenWindow(fresh); // 外部放进来的新便签文件
        }

        // ---------------------------------------------------------------
        // 开机启动
        // ---------------------------------------------------------------
        public static bool IsAutoStart()
        {
            using (var k = Registry.CurrentUser.OpenSubKey(RunKey))
                return k != null && k.GetValue("DeskNote") != null;
        }

        static string AutoStartCommand() { return "\"" + Assembly.GetExecutingAssembly().Location + "\""; }

        static void FixAutoStartPath()
        {
            try
            {
                using (var k = Registry.CurrentUser.OpenSubKey(RunKey, true))
                {
                    if (k == null) return;
                    var cur = k.GetValue("DeskNote") as string;
                    if (cur == null) return;
                    var want = AutoStartCommand();
                    if (!string.Equals(cur, want, StringComparison.OrdinalIgnoreCase)) k.SetValue("DeskNote", want);
                }
            }
            catch (Exception ex) { Store.Log("更新开机自启路径失败：" + ex.Message); }
        }

        public static void SetAutoStart(bool on)
        {
            using (var k = Registry.CurrentUser.CreateSubKey(RunKey))
            {
                if (on) k.SetValue("DeskNote", AutoStartCommand());
                else k.DeleteValue("DeskNote", false);
            }
        }

        // ---------------------------------------------------------------
        public void Exit()
        {
            if (exiting) return;
            exiting = true;
            if (watcher != null) watcher.Dispose();
            foreach (var w in Notes.ToArray()) { w.AllowClose = true; w.Close(); }
            if (msgWindow != null) { Native.UnregisterHotKey(msgWindow.Handle, HotkeyId); msgWindow.Dispose(); }
            if (tray != null) { tray.Visible = false; tray.Dispose(); }
            wpf.Shutdown();
        }
    }

    // 快捷键文字 <-> 修饰键 + 按键
    static class Hotkeys
    {
        public static bool TryParse(string s, out uint mods, out Key key)
        {
            mods = 0; key = Key.None;
            if (string.IsNullOrEmpty(s)) return false;
            foreach (var raw in s.Split('+'))
            {
                var p = raw.Trim();
                if (p.Equals("Ctrl", StringComparison.OrdinalIgnoreCase)) mods |= Native.MOD_CONTROL;
                else if (p.Equals("Alt", StringComparison.OrdinalIgnoreCase)) mods |= Native.MOD_ALT;
                else if (p.Equals("Shift", StringComparison.OrdinalIgnoreCase)) mods |= Native.MOD_SHIFT;
                else if (p.Equals("Win", StringComparison.OrdinalIgnoreCase)) mods |= Native.MOD_WIN;
                else
                {
                    Key k;
                    if (p.Length == 1 && char.IsDigit(p[0])) p = "D" + p;
                    if (!Enum.TryParse(p, true, out k)) return false;
                    key = k;
                }
            }
            return key != Key.None && mods != 0;
        }

        public static string Format(ModifierKeys m, Key k)
        {
            var parts = new List<string>();
            if ((m & ModifierKeys.Control) != 0) parts.Add("Ctrl");
            if ((m & ModifierKeys.Alt) != 0) parts.Add("Alt");
            if ((m & ModifierKeys.Shift) != 0) parts.Add("Shift");
            if ((m & ModifierKeys.Windows) != 0) parts.Add("Win");
            string ks = k.ToString();
            if (ks.Length == 2 && ks[0] == 'D' && char.IsDigit(ks[1])) ks = ks.Substring(1);
            parts.Add(ks);
            return string.Join("+", parts);
        }
    }

    public static class Program
    {
        [STAThread]
        public static void Main(string[] args)
        {
            // 让 WPF 跟随每块屏幕的缩放比例（配合 app.manifest 里的 PerMonitorV2）。
            // 必须在用到任何 WPF 类型之前设置；不设的话 WPF 会一律按 100% 渲染
            AppContext.SetSwitch("Switch.System.Windows.DoNotScaleForDpiChanges", false);

            // --data <目录>：用别的数据目录（测试用，不影响正在用的便签）
            string dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "DeskNote");
            bool isDefault = true;
            string open = null; // --open trash|settings：启动后直接打开回收站或设置
            string testPhase = null, testOut = null, syncServer = null, syncKey = null; // --selftest <阶段> <结果文件>：见 SelfTest.cs
            for (int i = 0; i + 1 < args.Length; i++)
            {
                if (args[i] == "--data") { dir = Path.GetFullPath(args[i + 1]); isDefault = false; }
                if (args[i] == "--open") open = args[i + 1];
                if (args[i] == "--selftest" && i + 2 < args.Length) { testPhase = args[i + 1]; testOut = Path.GetFullPath(args[i + 2]); }
                if (args[i] == "--sync-server") syncServer = args[i + 1];
                if (args[i] == "--sync-key") syncKey = args[i + 1];
            }
            if (testPhase != null && isDefault) return; // 自测只许在 --data 指定的测试目录里跑
            Store.Init(dir);

            // 单实例：同一个数据目录只开一个；再次启动时让已开的那个把便签提到最前
            string key = "DeskNote_" + (uint)dir.ToLowerInvariant().GetHashCode();
            bool created;
            using (var mutex = new Mutex(true, key, out created))
            using (var signal = new EventWaitHandle(false, EventResetMode.AutoReset, key + "_show"))
            {
                if (!created) { signal.Set(); return; }

                var wpf = new Application { ShutdownMode = ShutdownMode.OnExplicitShutdown };
                // 意外错误记到 error.log，不让整个程序崩掉
                wpf.DispatcherUnhandledException += delegate(object s, System.Windows.Threading.DispatcherUnhandledExceptionEventArgs e)
                {
                    Store.Log(e.Exception.ToString());
                    e.Handled = true;
                };
                AppDomain.CurrentDomain.UnhandledException += delegate(object s, UnhandledExceptionEventArgs e)
                {
                    Store.Log("fatal: " + e.ExceptionObject);
                };

                var app = new DeskApp(wpf, isDefault);
                wpf.Startup += delegate
                {
                    app.Silent = testPhase != null;
                    app.Start();
                    if (open == "trash") app.ShowTrash();
                    else if (open == "settings") app.ShowSettings();
                    else if (open == "reminder" && app.Notes.Count > 0) app.Notes[0].TestEditReminder(0); // 调界面用
                    if (testPhase != null)
                        wpf.Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.ApplicationIdle,
                            new Action(delegate { SelfTest.Run(app, testPhase, testOut, syncServer, syncKey); }));
                };

                var listener = new Thread(delegate()
                {
                    while (true)
                    {
                        signal.WaitOne();
                        wpf.Dispatcher.BeginInvoke(new Action(app.TogglePeek));
                    }
                }) { IsBackground = true };
                listener.Start();

                wpf.Run();
            }
        }
    }
}

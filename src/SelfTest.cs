// 自测：DeskNote.exe --data <空目录> --selftest <阶段> <结果文件> [--sync-server <地址> --sync-key <密钥>]
// 在真实的程序对象上跑一遍常用操作，结果逐行写进结果文件（PASS/FAIL），跑完自动退出。
// 阶段 1 做各种操作；阶段 2 是“重启后”检查数据有没有按预期留在磁盘上。由 tests\selftest.ps1 调用。
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows.Threading;

namespace DeskNote
{
    partial class NoteWindow
    {
        // ---- 只给自测用的入口：走和鼠标键盘一样的代码路径 ----
        internal void TestTypeItem(int index, string text)
        {
            BeginEdit(index);
            editBox.Text = text;
            CommitEdit(true);
        }

        internal void TestBeginEdit(int index, string text) { BeginEdit(index); editBox.Text = text; }
        internal void TestCommit() { CommitEdit(true); }

        // 模拟右键菜单里删除一条（菜单开着期间算一步撤销）
        internal void TestMenuDelete(int index)
        {
            BeginChange();
            TrashItemAt(index);
            Save();
            EndChange();
            Render();
        }

        internal void TestToggleDone(int index) { BeginChange(); d.Items[index].Done = !d.Items[index].Done; EndChange(); Save(); }
        internal void TestSetRemind(int index, DateTime t) { BeginChange(); d.Items[index].Remind = t; d.Items[index].Reminded = false; Save(); EndChange(); }
        internal void TestEditReminder(int index) { Dispatcher.BeginInvoke(new Action(delegate { EditReminder(index); })); }
        internal int TestUndoCount { get { return undoStack.Count; } }
    }

    static class SelfTest
    {
        static StringBuilder log;
        static int fails;

        static void Check(string name, bool ok, string detail)
        {
            if (!ok) fails++;
            log.AppendLine((ok ? "PASS " : "FAIL ") + name + (ok || detail == null ? "" : "  -- " + detail));
        }
        static void Check(string name, bool ok) { Check(name, ok, null); }

        // 让界面线程跑一会儿（处理计时器、异步回调）
        static void Pump(int ms)
        {
            var frame = new DispatcherFrame();
            var t = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(ms) };
            t.Tick += delegate { t.Stop(); frame.Continue = false; };
            t.Start();
            Dispatcher.PushFrame(frame);
        }

        static bool WaitUntil(Func<bool> cond, int timeoutMs)
        {
            var end = DateTime.Now.AddMilliseconds(timeoutMs);
            while (DateTime.Now < end) { if (cond()) return true; Pump(50); }
            return cond();
        }

        static NoteData ReadFile(string id)
        {
            var p = Store.NotePath(id);
            return File.Exists(p) ? NoteData.From(Json.Parse(File.ReadAllText(p, Encoding.UTF8)), id) : null;
        }

        static string[] NoteFiles() { return Directory.GetFiles(Store.NotesDir, "*.json").Select(f => Path.GetFileNameWithoutExtension(f)).OrderBy(x => x).ToArray(); }

        public static void Run(DeskApp app, string phase, string outFile, string syncServer, string syncKey)
        {
            log = new StringBuilder();
            fails = 0;
            try
            {
                if (phase == "1") Phase1(app, syncServer, syncKey);
                else Phase2(app);
            }
            catch (Exception ex) { Check("没有异常", false, ex.ToString()); }
            Check("没有 error.log", !File.Exists(Path.Combine(Store.Dir, "error.log")),
                File.Exists(Path.Combine(Store.Dir, "error.log")) ? File.ReadAllText(Path.Combine(Store.Dir, "error.log")) : null);
            log.AppendLine(fails == 0 ? "RESULT OK" : "RESULT " + fails + " FAILED");
            File.WriteAllText(outFile, log.ToString(), new UTF8Encoding(false));
            app.Exit();
        }

        static void Phase1(DeskApp app, string syncServer, string syncKey)
        {
            // ---- 启动 ----
            Check("首次启动有一张便签", app.Notes.Count == 1, "count=" + app.Notes.Count);
            Check("启动时做了今天的备份", Directory.Exists(Path.Combine(Store.BackupsDir, DateTime.Now.ToString("yyyy-MM-dd"))));
            Check("版本号格式", System.Text.RegularExpressions.Regex.IsMatch(DeskApp.Version, @"^\d+\.\d+\.\d+$"));
            var a = app.Notes[0];

            // ---- 添加、撤销、重做 ----
            a.TestTypeItem(a.Data.Items.Count, "第一条");
            a.TestTypeItem(a.Data.Items.Count, "第二条 https://example.com/a?b=1。");
            a.TestTypeItem(a.Data.Items.Count, "第三条");
            Check("添加 3 条", a.Data.Items.Count == 3, "count=" + a.Data.Items.Count);
            Check("添加后已存盘", ReadFile(a.Data.Id).Items.Count == 3);
            a.Undo();
            Check("撤销添加", a.Data.Items.Count == 2 && ReadFile(a.Data.Id).Items.Count == 2);
            a.Redo();
            Check("重做添加", a.Data.Items.Count == 3 && a.Data.Items[2].Text == "第三条");
            a.TestTypeItem(0, "第一条（改过）");
            a.Undo();
            Check("撤销修改文字", a.Data.Items[0].Text == "第一条");

            // ---- 删除一条进回收站，撤销后回收站里也拿掉 ----
            int trash0 = app.Trash.Count;
            a.TestMenuDelete(2);
            Check("删除一条进回收站", a.Data.Items.Count == 2 && app.Trash.Count == trash0 + 1);
            a.Undo();
            Check("撤销删除：条目回来、回收站记录拿掉", a.Data.Items.Count == 3 && app.Trash.Count == trash0);
            a.Redo();
            Check("重做删除：回收站记录又放回去", a.Data.Items.Count == 2 && app.Trash.Count == trash0 + 1);
            a.Undo();
            a.TestToggleDone(1);
            a.Undo();
            Check("撤销勾选", !a.Data.Items[1].Done);

            // ---- 清空文字 = 删除 ----
            a.TestTypeItem(2, "");
            Check("清空文字删除并进回收站", a.Data.Items.Count == 2 && app.Trash.Count == trash0 + 1, "trash=" + app.Trash.Count);
            a.Undo();
            Check("撤销清空", a.Data.Items.Count == 3 && app.Trash.Count == trash0);

            // ---- 链接 ----
            var parts = NoteWindow.SplitLinks("看 https://example.com/a?b=1。和 www.qq.com，完");
            var links = parts.Where(p => p.Value != null).Select(p => p.Value).ToArray();
            Check("识别出两个链接", links.Length == 2, string.Join(" | ", links));
            Check("链接末尾的中文句号不算进去", links.Length > 0 && links[0] == "https://example.com/a?b=1");
            Check("www 开头补上 http://", links.Length > 1 && links[1] == "http://www.qq.com");
            Check("拆分后文字不丢", string.Concat(parts.Select(p => p.Key)) == "看 https://example.com/a?b=1。和 www.qq.com，完");

            // ---- 提醒 ----
            var due = DateTime.Now.AddMinutes(-1);
            due = new DateTime(due.Year, due.Month, due.Day, due.Hour, due.Minute, 0);
            a.TestSetRemind(0, due);
            a.TestSetRemind(1, DateTime.Now.AddDays(1));
            app.CheckReminders();
            Check("到点的提醒标记为已提醒", a.Data.Items[0].Reminded);
            Check("没到点的不提醒", !a.Data.Items[1].Reminded);
            var fa = ReadFile(a.Data.Id);
            Check("提醒存进文件", fa.Items[0].Remind == due && fa.Items[0].Reminded && fa.Items[1].Remind.HasValue && !fa.Items[1].Reminded);

            // ---- 编辑时外部改了文件：合并而不是覆盖 ----
            a.TestBeginEdit(1, "正在编辑的这条");
            var ext = NoteData.FromContentJson(a.Data.ContentJson(), a.Data.Id);
            ext.Items.Insert(0, new NoteItem { Text = "外部加的一条" });
            a.ReloadFrom(ext, true);
            a.TestCommit();
            var texts = a.Data.Items.Select(x => x.Text).ToList();
            Check("外部新增的条目保留", texts.Contains("外部加的一条"), string.Join(" / ", texts));
            Check("编辑内容落在原来那条上", texts.Contains("正在编辑的这条") && !texts.Contains("第二条 https://example.com/a?b=1。"), string.Join(" / ", texts));
            Check("合并后条数正确", texts.Count == 4, "count=" + texts.Count);

            // ---- 新建、删除整张便签（之前的 bug：删掉后关窗又把文件写回来） ----
            app.CreateNote(a);
            var b = app.Notes[app.Notes.Count - 1];
            b.TestCommit();
            b.TestTypeItem(0, "B 便签的内容");
            string bid = b.Data.Id;
            Check("新建便签有文件", File.Exists(Store.NotePath(bid)));
            app.DeleteNote(b, false);
            Pump(300);
            Check("删除便签后文件不在", !File.Exists(Store.NotePath(bid)));
            Check("删除的便签进了回收站", app.Trash.Count > 0 && app.Trash[0].Kind == "note" && app.Trash[0].Note.Id == bid);
            File.WriteAllText(Path.Combine(Store.Dir, "deleted-id.txt"), bid);

            // ---- 恢复再删除 ----
            app.CreateNote(a);
            var c = app.Notes[app.Notes.Count - 1];
            c.TestCommit();
            c.TestTypeItem(0, "C");
            string cid = c.Data.Id;
            app.DeleteNote(c, false);
            app.Restore(app.Trash[0]);
            Pump(300);
            Check("恢复的便签用回原 id，文件回来", File.Exists(Store.NotePath(cid)) && app.FindNote(cid) != null);
            Check("恢复后只有 2 张", app.Notes.Count == 2, "count=" + app.Notes.Count);

            if (!string.IsNullOrEmpty(syncServer)) SyncTests(app, syncServer, syncKey);
            File.WriteAllText(Path.Combine(Store.Dir, "expected-notes.txt"), string.Join(",", NoteFiles()));
        }

        // ---- 同步（连 tests\mock_gopan.py 起的假服务器） ----
        static void SyncTests(DeskApp app, string server, string key)
        {
            var s = app.Settings;
            s.SyncEnabled = true; s.SyncServer = server; s.SyncKey = key; s.SyncFolder = "/DeskNote";
            Store.SaveSettings(s);
            Check("密钥加密保存", !File.ReadAllText(Store.SettingsFile).Contains(key));
            Check("密钥能解回来", Store.LoadSettings().SyncKey == key);

            Func<bool> idle = () => app.Sync.Status != "正在同步…";
            app.Sync.Start();
            app.Sync.SyncNow();
            WaitUntil(() => app.Sync.Status.StartsWith("上次同步") || app.Sync.LastFailed, 15000);
            Check("首次同步成功", app.Sync.Status.StartsWith("上次同步") && !app.Sync.LastFailed, app.Sync.Status);

            var mcp = new McpClient(server, key);
            var list = mcp.Call("list_files", new Dictionary<string, object> { { "path", "/DeskNote" } });
            var names = Json.Arr(list, "items").Cast<IDictionary<string, object>>().Select(i => Json.Str(i, "name", "")).OrderBy(x => x).ToArray();
            Check("两张便签都传上去了", names.Length == 2, string.Join(",", names));

            var a = app.Notes[0];
            string path = "/DeskNote/" + a.Data.Id + ".json";
            var rd = mcp.Call("read_text_file", new Dictionary<string, object> { { "path", path } });
            string remoteText = Json.Str(rd, "text", "");
            Check("远端内容不含窗口位置", !remoteText.Contains("\"window\"") && remoteText.Contains("\"items\""));

            // 别的电脑改了：拉下来
            var other = NoteData.FromContentJson(remoteText, a.Data.Id);
            other.Items.Add(new NoteItem { Text = "另一台电脑加的" });
            mcp.Call("write_text_file", new Dictionary<string, object> { { "path", path }, { "content", other.ContentJson() }, { "base_sha256", Json.Str(rd, "sha256", "") } });
            app.Sync.SyncNow();
            WaitUntil(() => a.Data.Items.Any(x => x.Text == "另一台电脑加的"), 15000);
            Check("拉下另一台电脑的修改", a.Data.Items.Any(x => x.Text == "另一台电脑加的"));
            Check("拉取后没有冲突副本", app.Notes.Count == 2, "count=" + app.Notes.Count);

            // 本地改：几秒后自动推上去
            a.TestTypeItem(a.Data.Items.Count, "本地刚加的");
            WaitUntil(() => Json.Str(mcp.Call("read_text_file", new Dictionary<string, object> { { "path", path } }), "text", "").Contains("本地刚加的"), 15000);
            Check("本地修改自动推上去", Json.Str(mcp.Call("read_text_file", new Dictionary<string, object> { { "path", path } }), "text", "").Contains("本地刚加的"));
            WaitUntil(idle, 10000);

            // 两边同时改：本地那份另存为冲突副本
            rd = mcp.Call("read_text_file", new Dictionary<string, object> { { "path", path } });
            var other2 = NoteData.FromContentJson(Json.Str(rd, "text", ""), a.Data.Id);
            other2.Title = "远端改的标题";
            mcp.Call("write_text_file", new Dictionary<string, object> { { "path", path }, { "content", other2.ContentJson() }, { "base_sha256", Json.Str(rd, "sha256", "") } });
            a.Data.Title = "本地改的标题";
            a.TestTypeItem(a.Data.Items.Count, "冲突时本地加的");
            app.Sync.SyncNow();
            WaitUntil(() => app.Notes.Any(n => n.Data.Title.EndsWith("（冲突副本）")), 15000);
            WaitUntil(idle, 10000);
            var copy = app.Notes.FirstOrDefault(n => n.Data.Title.EndsWith("（冲突副本）"));
            Check("冲突时生成副本", copy != null, "titles=" + string.Join(",", app.Notes.Select(n => n.Data.Title)));
            Check("原便签是远端版本", a.Data.Title == "远端改的标题", a.Data.Title);
            Check("副本里保留本地的修改", copy != null && copy.Data.Items.Any(x => x.Text == "冲突时本地加的"));

            // 本地删除：远端也删
            app.Sync.SyncNow();
            WaitUntil(idle, 15000);
            if (copy != null)
            {
                string cid = copy.Data.Id;
                app.DeleteNote(copy, false);
                Pump(200);
                app.Sync.SyncNow();
                WaitUntil(idle, 15000);
                Pump(300);
                list = mcp.Call("list_files", new Dictionary<string, object> { { "path", "/DeskNote" } });
                names = Json.Arr(list, "items").Cast<IDictionary<string, object>>().Select(i => Json.Str(i, "name", "")).ToArray();
                Check("本地删除后远端也删掉", !names.Contains(cid + ".json"), string.Join(",", names));
            }

            // 远端删除：本地进回收站
            var b = app.Notes[app.Notes.Count - 1];
            string bid = b.Data.Id;
            list = mcp.Call("list_files", new Dictionary<string, object> { { "path", "/DeskNote" } });
            var node = Json.Arr(list, "items").Cast<IDictionary<string, object>>().FirstOrDefault(i => Json.Str(i, "name", "") == bid + ".json");
            if (node != null) mcp.Call("trash_nodes", new Dictionary<string, object> { { "node_ids", new[] { Json.Str(node, "id", "") } } });
            int trashBefore = app.Trash.Count;
            app.Sync.SyncNow();
            WaitUntil(() => app.FindNote(bid) == null, 15000);
            Check("远端删除后本地移进回收站", app.FindNote(bid) == null && app.Trash.Count == trashBefore + 1);
            WaitUntil(idle, 10000);
            Pump(500);
            Check("最后一次同步没有失败", !app.Sync.LastFailed, app.Sync.Status);
        }

        static void Phase2(DeskApp app)
        {
            string expected = File.ReadAllText(Path.Combine(Store.Dir, "expected-notes.txt"));
            string deleted = File.ReadAllText(Path.Combine(Store.Dir, "deleted-id.txt"));
            Check("重启后便签文件和退出前一致", string.Join(",", NoteFiles()) == expected, string.Join(",", NoteFiles()) + " vs " + expected);
            Check("重启后打开的便签数一致", app.Notes.Count == expected.Split(',').Length, "count=" + app.Notes.Count);
            Check("删掉的便签没有回来", app.FindNote(deleted) == null && !File.Exists(Store.NotePath(deleted)));
            Check("回收站记录保留", app.Trash.Count > 0);
            var a = app.Notes.FirstOrDefault(n => n.Data.Items.Any(x => x.Remind.HasValue));
            Check("提醒状态保留", a != null && a.Data.Items.Any(x => x.Reminded));
        }
    }
}

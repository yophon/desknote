// 同步到 gopan：每张便签的内容（标题、条目、外观，不含窗口位置）存成远端文件夹里的 <id>.json。
// 用 gopan 的 MCP 接口（Bearer 密钥）：list_files / read_text_file / write_text_file(base_sha256 防覆盖) / trash_nodes。
// 回收站和设置不同步，各台电脑自己管。
using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows.Threading;

namespace DeskNote
{
    class McpException : Exception
    {
        public readonly string Code;
        public McpException(string code, string message) : base(message) { Code = code; }
    }

    // ---------------------------------------------------------------------
    // 最小的 MCP 客户端：只发 tools/call（gopan 是无状态 JSON 模式，不用先 initialize）
    // ---------------------------------------------------------------------
    class McpClient
    {
        readonly string endpoint, key;
        static readonly JavaScriptSerializer Js = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };
        int nextId = 1;

        static McpClient()
        {
            ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
            ServicePointManager.Expect100Continue = false;
        }

        public McpClient(string server, string key)
        {
            endpoint = server.TrimEnd('/') + "/mcp";
            this.key = key;
        }

        public IDictionary<string, object> Call(string tool, Dictionary<string, object> args)
        {
            var body = new Dictionary<string, object>
            {
                { "jsonrpc", "2.0" }, { "id", nextId++ }, { "method", "tools/call" },
                { "params", new Dictionary<string, object> { { "name", tool }, { "arguments", args } } }
            };
            string text = Post(Js.Serialize(body));
            var root = Js.DeserializeObject(ExtractJson(text)) as IDictionary<string, object>;
            if (root == null) throw new McpException("BAD_RESPONSE", "服务器返回的不是 JSON");
            var err = Json.Obj(root, "error");
            if (err != null) throw new McpException("RPC", Json.Str(err, "message", "调用失败"));
            var result = Json.Obj(root, "result");
            if (result == null) throw new McpException("BAD_RESPONSE", "没有 result");

            string firstText = null;
            foreach (var c in Json.Arr(result, "content"))
            {
                var cd = c as IDictionary<string, object>;
                if (cd != null && Json.Str(cd, "type", "") == "text") { firstText = Json.Str(cd, "text", ""); break; }
            }
            if (Json.Bool(result, "isError", false))
            {
                // gopan 的业务错误是“CODE: 说明”
                string msg = firstText ?? "调用失败";
                int colon = msg.IndexOf(':');
                string code = colon > 0 && msg.Substring(0, colon).Trim().ToUpperInvariant() == msg.Substring(0, colon).Trim() ? msg.Substring(0, colon).Trim() : "TOOL_ERROR";
                throw new McpException(code, colon > 0 ? msg.Substring(colon + 1).Trim() : msg);
            }
            var sc = Json.Obj(result, "structuredContent");
            if (sc != null) return sc;
            if (firstText != null)
            {
                try { var o = Js.DeserializeObject(firstText) as IDictionary<string, object>; if (o != null) return o; } catch { }
            }
            return new Dictionary<string, object>();
        }

        // 响应可能是普通 JSON，也可能是 SSE（data: {...}）
        static string ExtractJson(string text)
        {
            string t = text.TrimStart();
            if (t.StartsWith("{")) return t;
            var sb = new StringBuilder();
            foreach (var line in text.Split('\n'))
            {
                var l = line.TrimEnd('\r');
                if (l.StartsWith("data:")) sb.Append(l.Substring(5).TrimStart());
            }
            return sb.ToString();
        }

        string Post(string json)
        {
            var req = (HttpWebRequest)WebRequest.Create(endpoint);
            req.Method = "POST";
            req.ContentType = "application/json";
            req.Accept = "application/json, text/event-stream";
            req.Headers["Authorization"] = "Bearer " + key;
            req.UserAgent = "DeskNote/" + DeskApp.Version;
            req.Timeout = 20000;
            req.ReadWriteTimeout = 20000;
            var bytes = new UTF8Encoding(false).GetBytes(json);
            req.ContentLength = bytes.Length;
            try
            {
                using (var s = req.GetRequestStream()) s.Write(bytes, 0, bytes.Length);
                using (var resp = (HttpWebResponse)req.GetResponse())
                using (var r = new StreamReader(resp.GetResponseStream(), Encoding.UTF8))
                    return r.ReadToEnd();
            }
            catch (WebException ex)
            {
                var resp = ex.Response as HttpWebResponse;
                if (resp != null)
                {
                    int code = (int)resp.StatusCode;
                    resp.Close();
                    if (code == 401 || code == 403) throw new McpException("UNAUTHORIZED", "密钥无效或权限不够（HTTP " + code + "）");
                    throw new McpException("HTTP", "服务器返回 HTTP " + code);
                }
                throw new McpException("NETWORK", "连不上服务器：" + ex.Message);
            }
        }
    }

    // ---------------------------------------------------------------------
    // 同步状态：每张便签上次同步时“本地内容的哈希”和“远端文件的 sha256”
    // ---------------------------------------------------------------------
    class SyncState
    {
        public string Server = "", Folder = "";
        public Dictionary<string, KeyValuePair<string, string>> Notes = new Dictionary<string, KeyValuePair<string, string>>(); // id -> (local, remote)
        public DateTime? LastOk;

        static string FilePath { get { return Path.Combine(Store.Dir, "sync.json"); } }

        public static SyncState Load()
        {
            var s = new SyncState();
            try
            {
                if (!File.Exists(FilePath)) return s;
                var d = Json.Parse(File.ReadAllText(FilePath, Encoding.UTF8));
                s.Server = Json.Str(d, "server", "");
                s.Folder = Json.Str(d, "folder", "");
                DateTime t;
                if (DateTime.TryParse(Json.Str(d, "lastOk", ""), System.Globalization.CultureInfo.InvariantCulture, System.Globalization.DateTimeStyles.None, out t)) s.LastOk = t;
                var notes = Json.Obj(d, "notes");
                if (notes != null)
                    foreach (var kv in notes)
                    {
                        var o = kv.Value as IDictionary<string, object>;
                        if (o != null) s.Notes[kv.Key] = new KeyValuePair<string, string>(Json.Str(o, "local", ""), Json.Str(o, "remote", ""));
                    }
            }
            catch (Exception ex) { Store.Log("读取同步状态失败：" + ex.Message); }
            return s;
        }

        public void Save()
        {
            var notes = new JObj();
            foreach (var kv in Notes) notes.Set(kv.Key, new JObj().Set("local", kv.Value.Key).Set("remote", kv.Value.Value));
            var o = new JObj().Set("server", Server).Set("folder", Folder)
                .Set("lastOk", LastOk.HasValue ? LastOk.Value.ToString("yyyy-MM-dd HH:mm:ss") : null)
                .Set("notes", notes);
            try { Store.WriteAtomic(FilePath, Json.Write(o)); }
            catch (Exception ex) { Store.Log("保存同步状态失败：" + ex.Message); }
        }
    }

    // ---------------------------------------------------------------------
    // 同步引擎。网络请求在后台线程，读写便签都回到界面线程
    // ---------------------------------------------------------------------
    class Syncer
    {
        readonly DeskApp app;
        readonly Dispatcher ui;
        readonly DispatcherTimer debounce = new DispatcherTimer { Interval = TimeSpan.FromSeconds(4) };
        // 定时拉一次别的电脑的改动（gopan 每次调用都记审计日志，别太勤）
        readonly DispatcherTimer periodic = new DispatcherTimer { Interval = TimeSpan.FromMinutes(5) };
        string readyFolder; // 这次运行里已确认存在的远端文件夹
        SyncState state;
        bool running, again;

        public string Status = "未启用";
        public bool LastFailed;
        public event Action StatusChanged;

        public Syncer(DeskApp app, Dispatcher ui)
        {
            this.app = app;
            this.ui = ui;
            state = SyncState.Load();
            debounce.Tick += delegate { debounce.Stop(); SyncNow(); };
            periodic.Tick += delegate { SyncNow(); };
        }

        GlobalSettings S { get { return app.Settings; } }
        public bool Configured { get { return S.SyncEnabled && S.SyncServer.Length > 0 && S.SyncKey.Length > 0; } }

        public void Start()
        {
            periodic.Stop();
            if (!Configured) { SetStatus(S.SyncEnabled ? "还没填服务器地址或密钥" : "未启用", false); return; }
            periodic.Start();
            debounce.Stop();
            debounce.Start();
        }

        // 便签存过盘：内容跟上次同步时不一样才安排同步（只是挪了位置不算），几秒内的连续改动只同步一次
        public void Touch()
        {
            if (!Configured) return;
            bool changed = false;
            var seen = new HashSet<string>();
            foreach (var w in app.Notes)
            {
                if (w.Deleted) continue;
                seen.Add(w.Data.Id);
                KeyValuePair<string, string> s;
                if (!state.Notes.TryGetValue(w.Data.Id, out s) || s.Key != Hash(w.Data.ContentJson())) { changed = true; break; }
            }
            if (!changed) foreach (var id in state.Notes.Keys) if (!seen.Contains(id)) { changed = true; break; }
            if (!changed) return;
            debounce.Stop();
            debounce.Start();
        }

        void SetStatus(string s, bool failed)
        {
            Status = s;
            LastFailed = failed;
            if (StatusChanged != null) StatusChanged();
        }

        public static string Hash(string s)
        {
            using (var sha = SHA256.Create())
            {
                var b = sha.ComputeHash(Encoding.UTF8.GetBytes(s));
                var sb = new StringBuilder();
                foreach (var x in b) sb.Append(x.ToString("x2"));
                return sb.ToString();
            }
        }

        // ---- 一轮同步 ----
        class RemoteFile { public string Id, NodeId, Sha; }

        class Plan
        {
            // 后台算出来、回到界面线程执行的动作
            public List<KeyValuePair<string, string>> Pulls = new List<KeyValuePair<string, string>>();      // id, 远端内容（覆盖本地）
            public List<KeyValuePair<string, string>> Copies = new List<KeyValuePair<string, string>>();     // 冲突时本地那份另存为新便签：原 id, 本地内容
            public List<string> LocalDeletes = new List<string>();                                            // 远端删了：本地移进回收站
            public Dictionary<string, KeyValuePair<string, string>> NewState = new Dictionary<string, KeyValuePair<string, string>>();
            public Dictionary<string, string> Snapshot;                                                       // 开始时的本地内容
            public int Pushed, Pulled, Deleted, Conflicts;
            public string Folder;
        }

        public void SyncNow()
        {
            if (!Configured) return;
            if (running) { again = true; return; }
            running = true;
            SetStatus("正在同步…", false);

            // 界面线程：拍下本地所有便签的内容
            var snapshot = new Dictionary<string, string>();
            foreach (var w in app.Notes) if (!w.Deleted) snapshot[w.Data.Id] = w.Data.ContentJson();
            string server = S.SyncServer, key = S.SyncKey, folder = NormFolder(S.SyncFolder);
            if (state.Server != server || state.Folder != folder)
            {
                // 换了服务器或文件夹：当作第一次同步
                state = new SyncState { Server = server, Folder = folder };
            }
            var known = new Dictionary<string, KeyValuePair<string, string>>(state.Notes);

            var th = new Thread(delegate()
            {
                Plan plan = null;
                Exception error = null;
                try { plan = Compute(new McpClient(server, key), folder, folder != readyFolder, snapshot, known); }
                catch (Exception ex) { error = ex; }
                ui.BeginInvoke(new Action(delegate { Finish(plan, error); }));
            }) { IsBackground = true, Name = "DeskNote sync" };
            th.Start();
        }

        public static string NormFolder(string f)
        {
            f = (f ?? "").Trim().Replace('\\', '/');
            if (f.Length == 0) f = "/DeskNote";
            if (!f.StartsWith("/")) f = "/" + f;
            return f.Length > 1 ? f.TrimEnd('/') : f;
        }

        static string FilePathOf(string folder, string id) { return (folder == "/" ? "" : folder) + "/" + id + ".json"; }

        // 后台线程：只碰网络和传进来的副本，不碰界面对象
        static Plan Compute(McpClient c, string folder, bool ensureFolder, Dictionary<string, string> local, Dictionary<string, KeyValuePair<string, string>> known)
        {
            var plan = new Plan { Snapshot = local, Folder = folder };
            if (ensureFolder && folder != "/") c.Call("create_directories", new Dictionary<string, object> { { "path", folder } });

            // 远端清单
            var remote = new Dictionary<string, RemoteFile>();
            string cursor = null;
            do
            {
                var args = new Dictionary<string, object> { { "path", folder } };
                if (cursor != null) args["cursor"] = cursor;
                var r = c.Call("list_files", args);
                foreach (var o in Json.Arr(r, "items"))
                {
                    var it = o as IDictionary<string, object>;
                    if (it == null || Json.Str(it, "kind", "") != "file") continue;
                    string name = Json.Str(it, "name", "");
                    if (!name.EndsWith(".json", StringComparison.OrdinalIgnoreCase)) continue;
                    string id = name.Substring(0, name.Length - 5);
                    remote[id] = new RemoteFile { Id = id, NodeId = Json.Str(it, "id", ""), Sha = Json.Str(it, "sha256", "") };
                }
                cursor = Json.Str(r, "next_cursor", null);
            } while (!string.IsNullOrEmpty(cursor));

            var ids = new HashSet<string>(local.Keys);
            ids.UnionWith(remote.Keys);
            ids.UnionWith(known.Keys);
            foreach (var id in ids)
            {
                string L; local.TryGetValue(id, out L);
                RemoteFile R; remote.TryGetValue(id, out R);
                bool hasS = known.ContainsKey(id);
                var S = hasS ? known[id] : new KeyValuePair<string, string>();
                string Lh = L != null ? Hash(L) : null;
                string path = FilePathOf(folder, id);

                if (L == null && R == null) continue; // 两边都没了，状态丢掉

                bool lc = !hasS || L == null || Lh != S.Key;
                bool rc = !hasS || R == null || R.Sha != S.Value;

                if (hasS && !lc && !rc) { plan.NewState[id] = S; continue; }

                if (hasS && lc && !rc)
                {
                    if (L == null) { Trash(c, R); plan.Deleted++; continue; }              // 本地删了 -> 远端进回收站
                    string sha = Write(c, path, L, S.Value);                                 // 本地改了 -> 推上去
                    if (sha != null) { plan.NewState[id] = new KeyValuePair<string, string>(Lh, sha); plan.Pushed++; }
                    else plan.NewState[id] = S;                                              // 冲突了，下一轮按“两边都改了”处理
                    continue;
                }
                if (hasS && !lc && rc)
                {
                    if (R == null) { plan.LocalDeletes.Add(id); plan.Deleted++; continue; } // 远端删了 -> 本地进回收站
                    string content, sha;
                    Read(c, path, out content, out sha);                                      // 远端改了 -> 拉下来
                    plan.Pulls.Add(new KeyValuePair<string, string>(id, content));
                    plan.NewState[id] = new KeyValuePair<string, string>(Hash(content), sha);
                    plan.Pulled++;
                    continue;
                }

                // 第一次见到，或两边都改了
                if (L != null && R == null)
                {
                    string sha = Write(c, path, L, null);
                    if (sha != null) { plan.NewState[id] = new KeyValuePair<string, string>(Lh, sha); plan.Pushed++; }
                    continue;
                }
                if (L == null && R != null)
                {
                    string content, sha;
                    Read(c, path, out content, out sha);
                    plan.Pulls.Add(new KeyValuePair<string, string>(id, content));
                    plan.NewState[id] = new KeyValuePair<string, string>(Hash(content), sha);
                    plan.Pulled++;
                    continue;
                }
                {
                    string content, sha;
                    Read(c, path, out content, out sha);
                    if (Norm(content) != Norm(L))
                    {
                        // 两边内容不一样：远端那份留在原便签，本地这份另存为一张新便签（下一轮推上去），谁的都不丢
                        plan.Pulls.Add(new KeyValuePair<string, string>(id, content));
                        plan.Copies.Add(new KeyValuePair<string, string>(id, L));
                        plan.Conflicts++;
                    }
                    plan.NewState[id] = new KeyValuePair<string, string>(Hash(content), sha);
                }
            }
            return plan;
        }

        static string Norm(string s) { return s.Replace("\r\n", "\n").Trim(); }

        static string Write(McpClient c, string path, string content, string baseSha)
        {
            var args = new Dictionary<string, object> { { "path", path }, { "content", content } };
            if (!string.IsNullOrEmpty(baseSha)) args["base_sha256"] = baseSha;
            try
            {
                var r = c.Call("write_text_file", args);
                string sha = Json.Str(r, "sha256", null);
                if (string.IsNullOrEmpty(sha))
                {
                    // 没给 sha 的话再查一次
                    var info = c.Call("get_file_info", new Dictionary<string, object> { { "path", path } });
                    sha = Json.Str(info, "sha256", "");
                }
                return sha;
            }
            catch (McpException ex)
            {
                if (ex.Code == "EDIT_CONFLICT" || ex.Code == "READ_FIRST") return null; // 远端在这期间被改了
                throw;
            }
        }

        static void Read(McpClient c, string path, out string content, out string sha)
        {
            var sb = new StringBuilder();
            long offset = 0;
            sha = "";
            for (int guard = 0; guard < 64; guard++)
            {
                var args = new Dictionary<string, object> { { "path", path } };
                if (offset > 0) args["offset"] = offset;
                var r = c.Call("read_text_file", args);
                sb.Append(Json.Str(r, "text", ""));
                sha = Json.Str(r, "sha256", sha);
                if (Json.Bool(r, "eof", true)) break;
                long next = (long)Json.Num(r, "next_offset", 0);
                if (next <= offset) break;
                offset = next;
            }
            content = sb.ToString();
        }

        static void Trash(McpClient c, RemoteFile r)
        {
            try { c.Call("trash_nodes", new Dictionary<string, object> { { "node_ids", new[] { r.NodeId } } }); }
            catch (McpException ex) { if (ex.Code != "NOT_FOUND") throw; }
        }

        // 界面线程：执行计划
        void Finish(Plan plan, Exception error)
        {
            running = false;
            if (error != null)
            {
                var me = error as McpException;
                SetStatus("同步失败：" + (me != null ? me.Message : error.Message), true);
                if (me == null) Store.Log("同步出错：" + error);
            }
            else
            {
                readyFolder = plan.Folder;
                try { Apply(plan); }
                catch (Exception ex) { Store.Log("应用同步结果出错：" + ex); }
                state.Notes = plan.NewState;
                state.LastOk = DateTime.Now;
                state.Save();
                var parts = new List<string>();
                if (plan.Pushed > 0) parts.Add("上传 " + plan.Pushed);
                if (plan.Pulled > 0) parts.Add("下载 " + plan.Pulled);
                if (plan.Deleted > 0) parts.Add("删除 " + plan.Deleted);
                if (plan.Conflicts > 0) parts.Add("冲突 " + plan.Conflicts + "（已另存副本）");
                SetStatus("上次同步 " + DateTime.Now.ToString("HH:mm:ss") + (parts.Count > 0 ? "：" + string.Join("，", parts) : "，没有变化"), false);
            }
            if (again) { again = false; Touch(); }
        }

        void Apply(Plan plan)
        {
            foreach (var cp in plan.Copies) app.CreateNoteFromContent(cp.Value, "（冲突副本）");
            foreach (var p in plan.Pulls)
            {
                var w = app.FindNote(p.Key);
                string snap;
                plan.Snapshot.TryGetValue(p.Key, out snap);
                if (w != null && snap != null && w.Data.ContentJson() != snap)
                {
                    // 同步期间本地又改了：这次先不覆盖，让下一轮按“两边都改了”处理（不丢本地的）
                    plan.NewState.Remove(p.Key);
                    continue;
                }
                var fresh = NoteData.FromContentJson(p.Value, p.Key);
                if (w != null) w.ReloadFrom(fresh, true);
                else app.OpenSyncedNote(fresh);
            }
            foreach (var id in plan.LocalDeletes)
            {
                var w = app.FindNote(id);
                string snap;
                plan.Snapshot.TryGetValue(id, out snap);
                if (w == null) continue;
                if (snap != null && w.Data.ContentJson() != snap) { plan.NewState.Remove(id); continue; } // 本地刚改过，留着
                app.DeleteNote(w, false);
            }
        }
    }
}

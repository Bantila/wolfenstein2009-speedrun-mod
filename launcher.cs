// SrmodLauncher: installs the mod, launches the game and edits srmod.ini (the mod reloads it live).
// Built with the .NET Framework csc.exe that ships with Windows (C# 5 syntax).
using System;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

static class Ini {
    [DllImport("kernel32", CharSet = CharSet.Unicode)]
    static extern int GetPrivateProfileString(string s, string k, string d, StringBuilder r, int n, string f);
    [DllImport("kernel32", CharSet = CharSet.Unicode)]
    static extern bool WritePrivateProfileString(string s, string k, string v, string f);
    public static string File;
    public static string Get(string s, string k, string d) {
        var b = new StringBuilder(1024); GetPrivateProfileString(s, k, d, b, b.Capacity, File); return b.ToString();
    }
    public static int GetI(string s, string k, int d) { int v; return int.TryParse(Get(s, k, ""), out v) ? v : d; }
    public static float GetF(string s, string k, float d) {
        float v; return float.TryParse(Get(s, k, ""), NumberStyles.Float, CultureInfo.InvariantCulture, out v) ? v : d;
    }
    public static void Set(string s, string k, object v) {
        string str = v is float ? ((float)v).ToString("0.####", CultureInfo.InvariantCulture) : v.ToString();
        WritePrivateProfileString(s, k, str, File);
    }
}

class Widget {
    public string Id, Label, Sample; public bool On; public float X, Y; public int Anchor, Size; public Color Color;
    public string Sec { get { return "widget." + Id; } }
}

class LauncherForm : Form {
    // UI language: [launcher] ui = en|ru in srmod.ini, default from the Windows UI culture.
    static readonly bool Ru = DetectRu();
    static bool DetectRu() {
        Ini.File = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "srmod.ini");
        string ui = Ini.Get("launcher", "ui", "");
        return ui == "" ? CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "ru" : ui == "ru";
    }
    static string L(string en, string ru) { return Ru ? ru : en; }

    // Must match the defaults in mod.cpp LoadSettings().
    static readonly string[] Ids = { "coords", "speed", "total_rta", "total_igt", "map_rta", "map_igt", "pb", "loads", "deaths", "category", "map" };
    static readonly string[] Labels = { "", "Speed ", "RTA ", "IGT ", "Map RTA ", "Map IGT ", "PB ", "Loads ", "Deaths ", "", "" };
    static readonly string[] Samples = { "X 1234.5  Y -678.9  Z 12.0", "320 u/s (max 455)", "1:23:45.67", "1:20:02.10", "4:05.33", "3:58.90", "4:01.00  -0:02.10", "12 (3:43.57)", "3", "Any%", "trainyard" };
    static readonly string[] Keys = { "toggle", "start_stop", "reset", "category", "save_pos", "teleport" };
    static readonly string[] KeyTitles = { L("Show/hide overlay", "Показать/скрыть оверлей"), L("Start/finish run", "Старт/финиш рана"), L("Reset", "Сброс"), L("Switch category", "Сменить категорию"), L("Save position", "Сохранить позицию"), L("Teleport to position", "Телепорт на позицию") };
    static readonly Keys[] KeyDefaults = { System.Windows.Forms.Keys.F6, System.Windows.Forms.Keys.F7, System.Windows.Forms.Keys.F8, System.Windows.Forms.Keys.F10, System.Windows.Forms.Keys.NumPad7, System.Windows.Forms.Keys.NumPad9 };
    // Map file name (as the mod reads it), English and Russian display names.
    static readonly string[] Missions = {
        "trainyard", "Train Station", "Вокзал", "farm", "Farm", "Укреплённая ферма", "caverns", "Caverns", "Пещеры",
        "church", "Church", "Церковь", "tavern", "Tavern", "Пивная", "hospital", "Hospital", "Госпиталь",
        "cannery", "Cannery", "Консервный завод", "digsite", "Dig Site", "Раскоп",
        "airfield_east", "Airfield East", "Аэродром — восток", "airfield_west", "Airfield West", "Аэродром — запад",
        "castle", "Castle", "Замок", "castle_top", "Castle Top", "Вершина замка", "zeppelin", "Zeppelin", "Цеппелин",
        "blacksun", "Black Sun", "Чёрное Солнце", "downtown", "Downtown", "Деловой центр",
        "downtown_radio", "Radio Station", "Радиостанция", "downtown_west", "Downtown West", "Запад делового центра",
        "mte", "Midtown East", "Исторический центр — восток", "mte_para_hq", "SS Paranormal HQ", "База исследований СС",
        "mte_ss_hq", "SS HQ", "Штаб-квартира СС", "mtw", "Midtown West", "Исторический центр — запад",
        "mtw_off", "Officer's Apartment", "Квартира офицера", "mtw_ware", "Warehouse", "Склад" };

    static readonly string[] Anchors = { L("Top left", "Левый верх"), L("Top right", "Правый верх"), L("Bottom left", "Левый низ"), L("Bottom right", "Правый низ"), L("Center", "Центр") };

    string dir = AppDomain.CurrentDomain.BaseDirectory;
    Widget[] widgets = new Widget[Ids.Length];
    CheckedListBox list = new CheckedListBox();
    Panel preview = new DoubleBufferedPanel();
    TextBox labelBox = new TextBox();
    NumericUpDown sizeBox = new NumericUpDown();
    ComboBox anchorBox = new ComboBox();
    Button colorBtn = new Button();
    Label status = new Label();
    bool loading, allowCheck;
    int dragIdx = -1; Point dragLast;

    class DoubleBufferedPanel : Panel { public DoubleBufferedPanel() { DoubleBuffered = true; } }

    string P(string f) { return Path.Combine(dir, f); }
    Widget Sel { get { return list.SelectedIndex >= 0 ? widgets[list.SelectedIndex] : null; } }

    public LauncherForm() {
        Ini.File = P("srmod.ini");
        Text = "Wolfenstein Speedrun Mod"; Width = 1100; Height = 720; StartPosition = FormStartPosition.CenterScreen;
        Font = new Font("Segoe UI", 9.5f);

        for (int i = 0; i < Ids.Length; i++) {
            var w = new Widget { Id = Ids[i], Sample = Samples[i] };
            w.On = Ini.GetI(w.Sec, "enabled", Ids[i] != "map" ? 1 : 0) != 0;
            w.X = Ini.GetF(w.Sec, "x", 0.01f); w.Y = Ini.GetF(w.Sec, "y", 0.02f + 0.03f * i);
            w.Anchor = Ini.GetI(w.Sec, "anchor", 0); w.Size = Ini.GetI(w.Sec, "size", 22);
            w.Label = Ini.Get(w.Sec, "label", Labels[i]);
            uint c; w.Color = uint.TryParse(Ini.Get(w.Sec, "color", "FFFFFFFF"), NumberStyles.HexNumber, null, out c) ? Color.FromArgb((int)c) : Color.White;
            widgets[i] = w;
        }

        // Top bar: category, language, play, install.
        var top = new FlowLayoutPanel { Dock = DockStyle.Top, Height = 46, Padding = new Padding(6) };
        var cat = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 110 };
        cat.Items.AddRange(new object[] { "Any%", "Cheat%" });
        cat.SelectedIndex = Ini.Get("general", "category", "any") == "cheat" ? 1 : 0;
        cat.SelectedIndexChanged += delegate { Ini.Set("general", "category", cat.SelectedIndex == 1 ? "cheat" : "any"); };
        var lang = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 100 };
        lang.Items.AddRange(new object[] { "english", "russian", "french", "german", "italian", "spanish", "polish" });
        lang.SelectedItem = Ini.Get("launcher", "language", "english");
        if (lang.SelectedIndex < 0) lang.SelectedIndex = 0;
        lang.SelectedIndexChanged += delegate { Ini.Set("launcher", "language", lang.SelectedItem); };
        var play = new Button { Text = L("▶ Play", "▶ Играть"), Width = 110, Height = 30, BackColor = Color.FromArgb(60, 140, 60), ForeColor = Color.White };
        play.Click += delegate { Play((string)lang.SelectedItem); };
        var inst = new Button { Text = L("Install/update mod", "Установить/обновить мод"), AutoSize = true, Height = 30 };
        inst.Click += delegate { Install(); };
        var uninst = new Button { Text = L("Uninstall mod", "Удалить мод"), AutoSize = true, Height = 30 };
        uninst.Click += delegate { Uninstall(); };
        var resetPb = new Button { Text = L("Reset PBs", "Сбросить PB"), AutoSize = true, Height = 30 };
        resetPb.Click += delegate {
            if (MessageBox.Show(L("Delete all saved per-map PBs?", "Удалить все сохранённые PB по картам?"), Text, MessageBoxButtons.YesNo) == DialogResult.Yes) File.Delete(P("srmod_pb.ini"));
        };
        var ui = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 90, Margin = new Padding(16, 3, 3, 3) };
        ui.Items.AddRange(new object[] { "English", "Русский" });
        ui.SelectedIndex = Ru ? 1 : 0;
        ui.SelectedIndexChanged += delegate { Ini.Set("launcher", "ui", ui.SelectedIndex == 1 ? "ru" : "en"); Application.Restart(); };
        top.Controls.AddRange(new Control[] { new Label { Text = L("Category:", "Категория:"), AutoSize = true, Margin = new Padding(3, 8, 0, 0) }, cat,
            new Label { Text = L("Game language:", "Язык:"), AutoSize = true, Margin = new Padding(8, 8, 0, 0) }, lang, play, inst, uninst, resetPb, ui });

        status.Dock = DockStyle.Bottom; status.Height = 24; status.Padding = new Padding(6, 4, 0, 0);

        var tabs = new TabControl { Dock = DockStyle.Fill };
        tabs.TabPages.Add(WidgetsTab());
        tabs.TabPages.Add(GeneralTab());
        tabs.TabPages.Add(KeysTab());
        tabs.TabPages.Add(CheatTab());
        Controls.Add(tabs); Controls.Add(top); Controls.Add(status);
        UpdateStatus();
    }

    // ---- Widgets tab: list + properties + draggable 16:9 preview ----
    TabPage WidgetsTab() {
        var page = new TabPage(L("Overlay", "Оверлей"));
        var left = new Panel { Dock = DockStyle.Left, Width = 260, Padding = new Padding(6) };
        list.Dock = DockStyle.Top; list.Height = 260;
        foreach (var w in widgets) list.Items.Add(w.Id, w.On);
        // Clicking the name only selects; only the checkbox glyph toggles the widget.
        list.ItemCheck += (s, e) => {
            if (!allowCheck) { e.NewValue = e.CurrentValue; return; }
            widgets[e.Index].On = e.NewValue == CheckState.Checked; SaveWidget(widgets[e.Index]);
        };
        list.MouseDown += (s, e) => {
            int i = list.IndexFromPoint(e.Location);
            if (i < 0 || e.X > 18) return;
            allowCheck = true; list.SetItemChecked(i, !list.GetItemChecked(i)); allowCheck = false;
        };
        list.SelectedIndexChanged += delegate { LoadProps(); };

        var props = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, Padding = new Padding(0, 8, 0, 0) };
        sizeBox.Minimum = 8; sizeBox.Maximum = 120;
        anchorBox.DropDownStyle = ComboBoxStyle.DropDownList; anchorBox.Items.AddRange(Anchors);
        labelBox.TextChanged += delegate { if (!loading && Sel != null) { Sel.Label = labelBox.Text; SaveWidget(Sel); } };
        sizeBox.ValueChanged += delegate { if (!loading && Sel != null) { Sel.Size = (int)sizeBox.Value; SaveWidget(Sel); } };
        anchorBox.SelectedIndexChanged += delegate { if (!loading && Sel != null) { Sel.Anchor = anchorBox.SelectedIndex; SaveWidget(Sel); } };
        colorBtn.Text = L("Color…", "Цвет…");
        colorBtn.Click += delegate {
            if (Sel == null) return;
            var d = new ColorDialog { Color = Sel.Color, FullOpen = true };
            if (d.ShowDialog() == DialogResult.OK) { Sel.Color = Color.FromArgb(255, d.Color); SaveWidget(Sel); LoadProps(); }
        };
        AddRow(props, L("Label", "Подпись"), labelBox); AddRow(props, L("Size (px)", "Размер (px)"), sizeBox);
        AddRow(props, L("Anchor", "Привязка"), anchorBox); AddRow(props, "", colorBtn);
        AddRow(props, L("Your resolution", "Твоё разрешение"), ResolutionBox());
        var hint = new Label { AutoSize = true, ForeColor = Color.Gray, Text = L(
            "Drag widgets in the preview.\nPositions are stored as screen fractions,\nso they fit any resolution.\nResolution: pick or type, e.g. 2560x1600.",
            "Тащи виджет мышью в превью.\nПозиция хранится в долях экрана,\nподходит под любое разрешение.\nРазрешение: выбери или впиши, напр. 2560x1600.") };
        props.Controls.Add(hint, 0, props.RowCount++); props.SetColumnSpan(hint, 2);
        left.Controls.Add(props); left.Controls.Add(list);

        preview.Dock = DockStyle.Fill; preview.BackColor = Color.FromArgb(40, 44, 52);
        preview.Paint += PaintPreview;
        preview.Resize += delegate { preview.Invalidate(); };
        preview.MouseDown += PreviewDown;
        preview.MouseMove += PreviewMove;
        preview.MouseUp += delegate { dragIdx = -1; };
        page.Controls.Add(preview); page.Controls.Add(left);
        list.SelectedIndex = 0;
        return page;
    }

    static void AddRow(TableLayoutPanel t, string label, Control c) {
        int r = t.RowCount++;
        t.Controls.Add(new Label { Text = label, AutoSize = true, Margin = new Padding(0, 6, 4, 0) }, 0, r);
        c.Width = 130; t.Controls.Add(c, 1, r);
    }

    void LoadProps() {
        var w = Sel; if (w == null) return;
        loading = true;
        labelBox.Text = w.Label; sizeBox.Value = Math.Max(8, Math.Min(120, w.Size));
        anchorBox.SelectedIndex = Math.Max(0, Math.Min(4, w.Anchor)); colorBtn.BackColor = w.Color;
        colorBtn.ForeColor = w.Color.GetBrightness() > 0.5 ? Color.Black : Color.White;
        loading = false;
        preview.Invalidate();
    }

    void SaveWidget(Widget w) {
        Ini.Set(w.Sec, "enabled", w.On ? 1 : 0); Ini.Set(w.Sec, "x", w.X); Ini.Set(w.Sec, "y", w.Y);
        Ini.Set(w.Sec, "anchor", w.Anchor); Ini.Set(w.Sec, "size", w.Size);
        Ini.Set(w.Sec, "color", w.Color.ToArgb().ToString("X8"));
        Ini.Set(w.Sec, "label", "\"" + w.Label + "\"");  // quotes keep leading/trailing spaces
        preview.Invalidate();
    }

    // Preview uses the player's resolution: aspect ratio for layout, height for font pixel scale.
    static readonly string[] Resolutions = {
        "1280x720", "1366x768", "1600x900", "1920x1080", "2560x1440", "3840x2160",   // 16:9
        "1280x800", "1440x900", "1680x1050", "1920x1200", "2560x1600",               // 16:10
        "2560x1080", "3440x1440", "1024x768", "1280x1024" };                          // 21:9, 4:3, 5:4
    int resW = 1920, resH = 1080;

    static string Aspect(int w, int h) {
        int a = w, b = h; while (b != 0) { int t = a % b; a = b; b = t; }
        string r = (w / a) + ":" + (h / a);
        return r == "8:5" ? "16:10" : r == "64:27" || r == "43:18" ? "21:9" : r;
    }

    ComboBox ResolutionBox() {
        var box = new ComboBox { DropDownStyle = ComboBoxStyle.DropDown };
        var scr = System.Windows.Forms.Screen.PrimaryScreen.Bounds;
        string cur = Ini.Get("launcher", "resolution", scr.Width + "x" + scr.Height);
        foreach (var r in Resolutions) box.Items.Add(r);
        box.Text = cur; ParseRes(cur);
        box.TextChanged += delegate { if (ParseRes(box.Text)) { Ini.Set("launcher", "resolution", resW + "x" + resH); preview.Invalidate(); } };
        return box;
    }

    bool ParseRes(string s) {
        var p = s.ToLower().Replace(" ", "").Split('x', '×', '*');
        int w, h;
        if (p.Length != 2 || !int.TryParse(p[0], out w) || !int.TryParse(p[1], out h) || w < 320 || h < 200) return false;
        resW = w; resH = h; return true;
    }

    Rectangle Screen16x9() {
        int W = preview.ClientSize.Width - 20, H = preview.ClientSize.Height - 40;
        if ((long)W * resH > (long)H * resW) W = H * resW / resH; else H = W * resH / resW;
        return new Rectangle((preview.ClientSize.Width - W) / 2, (preview.ClientSize.Height - H) / 2, W, H);
    }

    Rectangle WidgetRect(Graphics g, Widget w, Rectangle scr, out Font f) {
        f = new Font(Ini.Get("general", "font", "Consolas"), Math.Max(4f, w.Size * scr.Height / (float)resH), FontStyle.Bold, GraphicsUnit.Pixel);
        var sz = g.MeasureString(w.Label + w.Sample, f).ToSize();
        int x = scr.X + (int)(w.X * scr.Width), y = scr.Y + (int)(w.Y * scr.Height);
        if (w.Anchor == 1 || w.Anchor == 3) x -= sz.Width;
        if (w.Anchor == 2 || w.Anchor == 3) y -= sz.Height;
        if (w.Anchor == 4) { x -= sz.Width / 2; y -= sz.Height / 2; }
        return new Rectangle(x, y, sz.Width, sz.Height);
    }

    void PaintPreview(object s, PaintEventArgs e) {
        var g = e.Graphics; var scr = Screen16x9();
        using (var b = new System.Drawing.Drawing2D.LinearGradientBrush(scr, Color.FromArgb(70, 80, 70), Color.FromArgb(25, 28, 25), 90f)) g.FillRectangle(b, scr);
        g.DrawRectangle(Pens.Gray, scr);
        g.DrawString(resW + "x" + resH + "  (" + Aspect(resW, resH) + ")", Font, Brushes.Gray, scr.X, scr.Bottom + 4);
        for (int i = 0; i < widgets.Length; i++) {
            var w = widgets[i]; if (!w.On) continue;
            Font f; var r = WidgetRect(g, w, scr, out f);
            if (i == list.SelectedIndex) g.DrawRectangle(Pens.Yellow, r);
            g.DrawString(w.Label + w.Sample, f, Brushes.Black, r.X + 2, r.Y + 2);
            using (var br = new SolidBrush(w.Color)) g.DrawString(w.Label + w.Sample, f, br, r.X, r.Y);
            f.Dispose();
        }
    }

    void PreviewDown(object s, MouseEventArgs e) {
        var scr = Screen16x9();
        using (var g = preview.CreateGraphics())
            for (int i = widgets.Length - 1; i >= 0; i--) {
                if (!widgets[i].On) continue;
                Font f; var r = WidgetRect(g, widgets[i], scr, out f); f.Dispose();
                if (r.Contains(e.Location)) { dragIdx = i; dragLast = e.Location; list.SelectedIndex = i; return; }
            }
    }

    void PreviewMove(object s, MouseEventArgs e) {
        if (dragIdx < 0) return;
        var scr = Screen16x9(); var w = widgets[dragIdx];
        w.X = Math.Max(0f, Math.Min(1f, w.X + (e.X - dragLast.X) / (float)scr.Width));
        w.Y = Math.Max(0f, Math.Min(1f, w.Y + (e.Y - dragLast.Y) / (float)scr.Height));
        dragLast = e.Location;
        SaveWidget(w);
    }

    // ---- General / keys / cheat tabs ----
    TabPage GeneralTab() {
        var page = new TabPage(L("General", "Общие"));
        var t = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, Padding = new Padding(12), AutoScroll = true };
        var dec = new NumericUpDown { Minimum = 0, Maximum = 3, Value = Math.Max(0, Math.Min(3, Ini.GetI("general", "decimals", 2))) };
        dec.ValueChanged += delegate { Ini.Set("general", "decimals", (int)dec.Value); };
        AddRow(t, L("Decimal places (0–3)", "Знаков после точки (0–3)"), dec);
        AddCheck(t, L("Always show hours", "Всегда показывать часы"), "general", "hours_always", 0);
        AddCheck(t, L("Text shadow", "Тень под текстом"), "general", "shadow", 1);
        AddCheck(t, L("Auto start/reset run when the start mission loads", "Автостарт рана при загрузке стартовой карты"), "general", "auto_start", 1);
        var start = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList };
        for (int i = 0; i < Missions.Length; i += 3) start.Items.Add(Missions[i + (Ru ? 2 : 1)] + "  (" + Missions[i] + ")");
        int cur = Array.IndexOf(Missions, Ini.Get("general", "start_map", "trainyard"));
        start.SelectedIndex = cur >= 0 && cur % 3 == 0 ? cur / 3 : 0;
        start.SelectedIndexChanged += delegate { Ini.Set("general", "start_map", Missions[start.SelectedIndex * 3]); };
        AddRow(t, L("Start mission", "Стартовая миссия"), start); start.Width = 320;
        var font = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 220 };
        foreach (var fam in FontFamily.Families) font.Items.Add(fam.Name);
        font.SelectedItem = Ini.Get("general", "font", "Consolas");
        font.SelectedIndexChanged += delegate { Ini.Set("general", "font", "\"" + font.SelectedItem + "\""); preview.Invalidate(); };
        AddRow(t, L("Font", "Шрифт"), font); font.Width = 220;
        return page.With(t);
    }

    TabPage KeysTab() {
        var page = new TabPage(L("Hotkeys", "Хоткеи"));
        var t = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, Padding = new Padding(12) };
        for (int i = 0; i < Keys.Length; i++) {
            string key = Keys[i];
            var b = new Button { Width = 160, Text = ((Keys)Ini.GetI("keys", key, (int)KeyDefaults[i])).ToString() };
            b.Click += delegate { b.Text = L("Press a key…", "Нажми клавишу…"); b.Focus(); };
            b.PreviewKeyDown += (s, e) => e.IsInputKey = true;
            b.KeyDown += (s, e) => {
                if (b.Text != L("Press a key…", "Нажми клавишу…")) return;
                Ini.Set("keys", key, (int)e.KeyCode); b.Text = e.KeyCode.ToString(); e.Handled = true;
            };
            AddRow(t, KeyTitles[i], b);
        }
        return page.With(t);
    }

    TabPage CheatTab() {
        var page = new TabPage("Cheat%");
        var t = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, Padding = new Padding(12) };
        AddCheck(t, L("God mode always on", "Бессмертие (god) всё время"), "cheat", "god", 1);
        AddText(t, L("Commands after each map load (separated by ;)", "Команды после загрузки карты (через ;)"), "cheat", "commands", "give all;giveAllPowerUpgrades;momoney", 420);
        AddText(t, L("Teleport command (%f = x y z)", "Команда телепорта (%f = x y z)"), "cheat", "teleport_cmd", "script $player1.setOrigin('%.3f %.3f %.3f');", 420);
        t.Controls.Add(new Label { AutoSize = true, ForeColor = Color.Gray, Text =
            L("Useful game commands: give all, giveAllPowerUpgrades, givePowers, momoney, giveGold,\n", "Полезные команды игры: give all, giveAllPowerUpgrades, givePowers, momoney, giveGold,\n") +
            "giveAllIntel, notarget, undying, killActiveEnemies, gotoMission." }, 0, t.RowCount++);
        t.SetColumnSpan(t.GetControlFromPosition(0, t.RowCount - 1), 2);
        return page.With(t);
    }

    static void AddCheck(TableLayoutPanel t, string title, string sec, string key, int def) {
        var c = new CheckBox { Text = title, AutoSize = true, Checked = Ini.GetI(sec, key, def) != 0 };
        c.CheckedChanged += delegate { Ini.Set(sec, key, c.Checked ? 1 : 0); };
        t.Controls.Add(c, 0, t.RowCount++); t.SetColumnSpan(c, 2);
    }

    static void AddText(TableLayoutPanel t, string title, string sec, string key, string def, int width = 160) {
        var b = new TextBox { Text = Ini.Get(sec, key, def) };
        b.TextChanged += delegate { Ini.Set(sec, key, "\"" + b.Text + "\""); };
        AddRow(t, title, b); b.Width = width;
    }

    // ---- Install / launch ----
    bool Installed { get { return File.Exists(P("binkw32_orig.dll")); } }

    void UpdateStatus() {
        status.Text = !File.Exists(P("Wolf2.exe")) ? L("Put the launcher into the SP folder next to Wolf2.exe", "Лаунчер должен лежать в папке SP рядом с Wolf2.exe") :
                      Installed ? L("Mod installed. Settings apply to the running game instantly.", "Мод установлен. Настройки применяются в игре сразу.") : L("Mod not installed.", "Мод не установлен.");
    }

    bool Install() {
        try {
            if (!File.Exists(P("srmod.dll"))) throw new Exception(L("srmod.dll is missing next to the launcher.", "Нет srmod.dll рядом с лаунчером."));
            if (!Installed) File.Move(P("binkw32.dll"), P("binkw32_orig.dll"));
            File.Copy(P("srmod.dll"), P("binkw32.dll"), true);
            UpdateStatus(); return true;
        } catch (Exception ex) { MessageBox.Show(L("Install failed: ", "Не удалось установить: ") + ex.Message, Text); return false; }
    }

    void Uninstall() {
        try {
            if (!Installed) return;
            File.Delete(P("binkw32.dll")); File.Move(P("binkw32_orig.dll"), P("binkw32.dll"));
        } catch (Exception ex) { MessageBox.Show(L("Uninstall failed: ", "Не удалось удалить: ") + ex.Message, Text); }
        UpdateStatus();
    }

    void Play(string lang) {
        if (Process.GetProcessesByName("Wolf2").Length > 0) { MessageBox.Show(L("The game is already running.", "Игра уже запущена."), Text); return; }
        if (!Install()) return;
        Process.Start(new ProcessStartInfo(P("Wolf2.exe"), "+set com_allowconsole 1 +set com_SingleDeclFile 0 +set sys_lang \"" + lang + "\"") { WorkingDirectory = dir });
    }

    [STAThread]
    static void Main() { Application.EnableVisualStyles(); Application.Run(new LauncherForm()); }
}

static class Ext {
    public static TabPage With(this TabPage p, Control c) { p.Controls.Add(c); return p; }
}

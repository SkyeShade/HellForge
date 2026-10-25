using System.Diagnostics;
using System.IO.Compression;
using HellForge.Core;

namespace HellForge.Setup;

// HellForge-Setup.exe: unpacks HellForge into a folder and starts its launcher, whose setup wizard copies the game
// from the user's own Steam library. Run on an existing installation, it replaces the app and keeps the game, user
// data, profiles and logs. The app travels inside this program as payload.zip (scripts/Package-HellForge.ps1).
sealed class SetupForm : Form
{
    const string ProgramName = "HellForge.exe";
    readonly TextBox folder = new() { Dock = DockStyle.Fill };
    readonly Button browse = new() { Text = "Browse...", AutoSize = true };
    readonly Label note = new() { AutoSize = true, MaximumSize = new(560, 0), ForeColor = Color.DarkOrange };
    readonly ProgressBar progress = new() { Dock = DockStyle.Fill, Maximum = 1000 };
    readonly Label status = new() { AutoSize = true, MaximumSize = new(560, 0) };
    readonly Button install = new() { Text = "Install", AutoSize = true, MinimumSize = new(96, 0) };
    readonly Button cancel = new() { Text = "Cancel", AutoSize = true, MinimumSize = new(96, 0) };
    bool busy;

    public SetupForm()
    {
        Text = "HellForge Setup";
        Icon = Environment.ProcessPath is { } program ? Icon.ExtractAssociatedIcon(program) : null; // the exe's own (ApplicationIcon)
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        AutoSize = true;
        AutoSizeMode = AutoSizeMode.GrowAndShrink;
        Padding = new(18);
        Font = new Font("Segoe UI", 9.5f);

        var layout = new TableLayoutPanel { AutoSize = true, ColumnCount = 2, Width = 600 };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        void Row(Control control, int span = 2) { layout.Controls.Add(control); layout.SetColumnSpan(control, span); }
        Row(new Label { Text = "Install HellForge", AutoSize = true, Font = new Font(Font.FontFamily, 14f, FontStyle.Bold), Margin = new(0, 0, 0, 8) });
        Row(new Label
        {
            AutoSize = true, MaximumSize = new(560, 0), Margin = new(0, 0, 0, 12),
            Text = "HellForge runs HELLDIVERS 2 locally against a backend on this PC, with co-op over Steam. Setup unpacks it into the folder below; " +
                "the launcher then copies the game from your own Steam library (about 24 GB) and finishes setting up. Everything " +
                "HellForge stores stays inside this folder."
        });
        Row(new Label { Text = "Install folder", AutoSize = true });
        layout.Controls.Add(folder);
        layout.Controls.Add(browse);
        Row(note);
        Row(progress);
        Row(status);
        // The bottom row: the version this setup installs and the game build it needs on the left, the buttons on the right.
        var bottom = new TableLayoutPanel { ColumnCount = 2, AutoSize = true, Dock = DockStyle.Fill, Margin = new(0, 12, 0, 0) };
        bottom.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        bottom.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        bottom.Controls.Add(new Label
        {
            Text = $"HellForge {AppVersion.Text}  ·  HELLDIVERS 2 build {SupportedBuild.Current.BuildId}",
            AutoSize = true, ForeColor = SystemColors.GrayText, Anchor = AnchorStyles.Left, Margin = new(0, 0, 12, 0)
        });
        var buttons = new FlowLayoutPanel { FlowDirection = FlowDirection.RightToLeft, AutoSize = true, WrapContents = false, Anchor = AnchorStyles.Right, Margin = new(0) };
        buttons.Controls.Add(cancel);
        buttons.Controls.Add(install);
        bottom.Controls.Add(buttons);
        Row(bottom);
        Controls.Add(layout);
        AcceptButton = install;
        CancelButton = cancel;

        // An installation from before the rename (HD2 Beyond The Stars) is updated where it is. Documents is the default
        // unless OneDrive (or another link) holds it: then the user's own folder.
        var documents = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
        var earlier = Path.Combine(documents, "HD2 Beyond The Stars");
        var home = Usable(documents) ? documents : Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        folder.Text = !Directory.Exists(Path.Combine(documents, "HellForge")) && new InstallLayout(earlier).Exists ? earlier : Path.Combine(home, "HellForge");
        folder.TextChanged += (_, _) => Check();
        browse.Click += (_, _) => Browse();
        install.Click += async (_, _) => await Install();
        cancel.Click += (_, _) => Close();
        FormClosing += (_, e) => e.Cancel = busy;
        Check();
    }

    string Target => LocalInstall.FullPath(folder.Text.Trim());

    void Browse()
    {
        using var dialog = new FolderBrowserDialog { Description = "Choose where to put the HellForge folder", UseDescriptionForTitle = true };
        if (dialog.ShowDialog(this) == DialogResult.OK)
            folder.Text = new InstallLayout(dialog.SelectedPath).Exists ? dialog.SelectedPath
                : Path.Combine(dialog.SelectedPath, "HellForge");
    }

    // Validates the folder; returns false when installing there is not possible.
    bool Check()
    {
        string message = "", error = "";
        try
        {
            var target = Target;
            var existing = new InstallLayout(target).Exists;
            install.Text = existing ? "Update" : "Install";
            if (target.Split(Path.DirectorySeparatorChar).Any(part => part.Equals("steamapps", StringComparison.OrdinalIgnoreCase)))
                error = "Pick a folder outside Steam's steamapps folder.";
            else if (!Usable(target))
                error = "This folder is inside OneDrive or a linked folder, which HellForge cannot use (and OneDrive would upload " +
                    "about 24 GB of game files). Pick a folder outside it, such as " + Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "HellForge") + ".";
            else if (!existing && Directory.Exists(target) && Directory.EnumerateFileSystemEntries(target).Any())
                error = "This folder is not empty. Pick a new or empty folder.";
            else if (existing)
                message = "HellForge is already installed here. Setup updates the app and keeps your game copy, profiles and settings.";
            var root = Path.GetPathRoot(target);
            if (error.Length == 0 && !existing && root != null && new DriveInfo(root).IsReady && new DriveInfo(root).AvailableFreeSpace < 26L << 30)
                message = $"Drive {root} has {new DriveInfo(root).AvailableFreeSpace / 1e9:0} GB free; the game copy needs about 25 GB.";
        }
        catch (Exception exception) when (exception is ArgumentException or IOException or NotSupportedException or UnauthorizedAccessException)
        {
            error = "Enter a valid folder path.";
        }
        note.Text = Privacy.Scrub(error.Length > 0 ? error : message);
        note.ForeColor = error.Length > 0 ? Color.Firebrick : Color.DarkOrange;
        install.Enabled = error.Length == 0 && !busy;
        return error.Length == 0;
    }

    // Outside OneDrive and free of links: an installation refuses linked paths (LocalInstall.NoLinks), and OneDrive's
    // folders are links (cloud files) on most PCs.
    static bool Usable(string path)
    {
        if (new[] { "OneDrive", "OneDriveConsumer", "OneDriveCommercial" }.Select(Environment.GetEnvironmentVariable)
            .Any(root => !string.IsNullOrEmpty(root) && LocalInstall.Within(path, root))) return false;
        try { LocalInstall.NoLinks(path); return true; }
        catch (InvalidDataException) { return false; }
    }

    async Task Install()
    {
        if (busy || !Check()) return;
        using var payload = typeof(SetupForm).Assembly.GetManifestResourceStream("payload.zip");
        if (payload == null)
        {
            MessageBox.Show(this, "This setup program was built without the app. Build it with scripts/Package-HellForge.ps1.", Text, MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }
        var target = Target;
        if (Process.GetProcesses().Any(process => IsFrom(process, target)))
        {
            MessageBox.Show(this, "Close HellForge (launcher and any session) first.", Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }
        busy = true;
        install.Enabled = browse.Enabled = folder.Enabled = cancel.Enabled = false;
        try
        {
            await Task.Run(() => Unpack(payload, target, (done, total) => BeginInvoke(() =>
            {
                progress.Value = done * 1000 / total;
                status.Text = $"Unpacking {done} of {total} files...";
            })));
            status.Text = "Starting the launcher...";
            Process.Start(new ProcessStartInfo(Path.Combine(target, ProgramName)) { WorkingDirectory = target, UseShellExecute = true });
            busy = false;
            Close();
        }
        catch (Exception exception) when (exception is IOException or InvalidDataException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
        {
            busy = false;
            status.Text = "";
            MessageBox.Show(this, "Setup could not finish: " + exception.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Error);
            browse.Enabled = folder.Enabled = cancel.Enabled = true;
            Check();
        }
    }

    public static int UnpackQuietly(string folder)
    {
        using var payload = typeof(SetupForm).Assembly.GetManifestResourceStream("payload.zip");
        if (payload == null) return 2;
        try
        {
            Unpack(payload, LocalInstall.FullPath(folder), null);
            return 0;
        }
        catch (Exception exception) when (exception is IOException or InvalidDataException or UnauthorizedAccessException)
        {
            Console.Error.WriteLine(exception.Message);
            return 1;
        }
    }

    // The new app is unpacked next to the installation's folders and swapped in at the end.
    static void Unpack(Stream payload, string target, Action<int, int>? report)
    {
        var layout = new InstallLayout(target);
        if (!layout.Exists) layout.Create(SupportedBuild.Current.BuildId);
        var staging = Path.Combine(layout.Temp, "setup-staging");
        if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true);
        using (var archive = new ZipArchive(payload, ZipArchiveMode.Read))
        {
            var entries = archive.Entries.Where(entry => entry.Name.Length > 0).ToList();
            for (var i = 0; i < entries.Count; i++)
            {
                var path = Path.GetFullPath(Path.Combine(staging, entries[i].FullName));
                if (!LocalInstall.Within(path, staging)) throw new InvalidDataException("The app archive contains an unsafe path.");
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                entries[i].ExtractToFile(path);
                report?.Invoke(i + 1, entries.Count);
            }
        }
        if (Directory.Exists(layout.App)) Directory.Delete(layout.App, recursive: true);
        Directory.Move(Path.Combine(staging, "app"), layout.App);
        File.Move(Path.Combine(staging, ProgramName), Path.Combine(target, ProgramName), overwrite: true);
        Directory.Delete(staging, recursive: true);
    }

    static bool IsFrom(Process process, string root)
    {
        try { return process.MainModule?.FileName is { } file && LocalInstall.Within(file, root); }
        catch (Exception exception) when (exception is System.ComponentModel.Win32Exception or InvalidOperationException) { return false; }
        finally { process.Dispose(); }
    }
}

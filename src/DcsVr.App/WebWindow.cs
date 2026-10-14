using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Shell;
using DcsVr.Core;
using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.Wpf;

namespace DcsVr.App;

internal sealed class WebWindow : Window
{
    private readonly WebView2 _view = new();
    private WebBridge? _bridge;
    public WebWindow()
    {
        Title = "DCS Control"; Width = 1320; Height = 920; MinWidth = 850; MinHeight = 620;
        Icon = BitmapFrame.Create(new Uri("pack://application:,,,/Assets/app-icon.ico", UriKind.Absolute));
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        Width = Math.Min(Width, SystemParameters.WorkArea.Width - 40); Height = Math.Min(Height, SystemParameters.WorkArea.Height - 40);
        MinWidth = Math.Min(MinWidth, Width); MinHeight = Math.Min(MinHeight, Height);
        _view.DefaultBackgroundColor = System.Drawing.Color.FromArgb(255, 38, 38, 38);
        // No Windows title bar, like the DCS launcher: the page draws its own window buttons and marks its drag areas
        // with app-region. The 4 px frame around the browser keeps the resize edges, which the browser window would cover.
        Background = new SolidColorBrush(Color.FromRgb(17, 17, 17));
        WindowChrome.SetWindowChrome(this, new WindowChrome { CaptionHeight = 0, ResizeBorderThickness = new Thickness(5), GlassFrameThickness = new Thickness(0, 0, 0, 1), CornerRadius = new CornerRadius(0), UseAeroCaptionButtons = false });
        var frame = new Border { Child = _view, Padding = new Thickness(4) };
        Content = frame;
        // Without a frame Windows would maximize the window past the screen edges and over the taskbar; the size is
        // limited to the monitor's work area instead, and the resize frame is not needed while maximized.
        SourceInitialized += (_, _) => HwndSource.FromHwnd(new WindowInteropHelper(this).Handle)?.AddHook(LimitMaximizedToWorkArea);
        StateChanged += (_, _) => frame.Padding = WindowState == WindowState.Maximized ? new Thickness(0) : new Thickness(4);
        Loaded += async (_, _) =>
        {
            try
            {
                var service = new ControlService(AppContext.BaseDirectory, ControlService.DefaultStateRoot);
                var environment = await CoreWebView2Environment.CreateAsync(null, Path.Combine(service.StateRoot, "webview"));
                await _view.EnsureCoreWebView2Async(environment);
                _view.CoreWebView2.Settings.IsNonClientRegionSupportEnabled = true;
                _bridge = new WebBridge(service, this);
                WebHost.Configure(_view.CoreWebView2, _bridge);
                _view.CoreWebView2.Navigate(WebHost.Address);
            }
            catch (Exception e)
            {
                Content = new TextBox { Text = "The web interface could not start.\n\n" + e.Message + "\n\nInstall Microsoft Edge WebView2 Evergreen Runtime, then reopen DCS Control.\nhttps://developer.microsoft.com/microsoft-edge/webview2/", IsReadOnly = true, TextWrapping = TextWrapping.Wrap, Foreground = Brushes.White, Background = Background, Margin = new Thickness(32) };
            }
        };
        StateChanged += async (_, _) =>
        {
            if (_view.CoreWebView2 is not { } core) return;
            try
            {
                if (WindowState == WindowState.Minimized) { _view.Visibility = Visibility.Hidden; await core.TrySuspendAsync(); }
                else { core.Resume(); _view.Visibility = Visibility.Visible; }
            }
            catch (Exception e) when (e is InvalidOperationException or System.Runtime.InteropServices.COMException) { /* Closing or recreating the browser. */ }
        };
        // Alt+F4 or the taskbar would otherwise close the app halfway through writing DCS's files (the title-bar
        // button already waits for the command to finish).
        Closing += (_, e) =>
        {
            if (_bridge?.Writing is not { } writing) return;
            e.Cancel = true;
            MessageBox.Show(this, (writing == "restore" ? "Back to stock DCS" : writing == "launch" ? "Launch DCS" : "DCS Control") + " is still changing files. Close DCS Control when it has finished.",
                "DCS Control", MessageBoxButton.OK, MessageBoxImage.Information);
        };
        Closed += (_, _) => _view.Dispose();
    }

    private static IntPtr LimitMaximizedToWorkArea(IntPtr hwnd, int message, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        const int WM_GETMINMAXINFO = 0x0024, MONITOR_DEFAULTTONEAREST = 2;
        if (message != WM_GETMINMAXINFO) return IntPtr.Zero;
        var monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        var info = new MonitorInfo { Size = Marshal.SizeOf<MonitorInfo>() };
        if (monitor == IntPtr.Zero || !GetMonitorInfo(monitor, ref info)) return IntPtr.Zero;
        var limits = Marshal.PtrToStructure<MinMaxInfo>(lParam);
        limits.MaxPosition = new(info.Work.Left - info.Monitor.Left, info.Work.Top - info.Monitor.Top);
        limits.MaxSize = new(info.Work.Right - info.Work.Left, info.Work.Bottom - info.Work.Top);
        Marshal.StructureToPtr(limits, lParam, false);
        return IntPtr.Zero;
    }
    [StructLayout(LayoutKind.Sequential)] private record struct Point32(int X, int Y);
    [StructLayout(LayoutKind.Sequential)] private struct Rect32 { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] private struct MinMaxInfo { public Point32 Reserved, MaxSize, MaxPosition, MinTrackSize, MaxTrackSize; }
    [StructLayout(LayoutKind.Sequential)] private struct MonitorInfo { public int Size; public Rect32 Monitor, Work; public uint Flags; }
    [DllImport("user32.dll")] private static extern IntPtr MonitorFromWindow(IntPtr hwnd, int flags);
    [DllImport("user32.dll")] private static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo info);
}

internal static class WebHost
{
    public const string Address = "https://dcs-vr.example/index.html";
    public static void Configure(CoreWebView2 core, WebBridge bridge)
    {
        // .example avoids the mDNS lookup delay WebView2 documents for .local host names.
        core.SetVirtualHostNameToFolderMapping("dcs-vr.example", Path.Combine(AppContext.BaseDirectory, "ui"), CoreWebView2HostResourceAccessKind.DenyCors);
        core.Settings.AreDefaultContextMenusEnabled = false; core.Settings.AreDevToolsEnabled = false;
        core.Settings.AreBrowserAcceleratorKeysEnabled = false; core.Settings.IsStatusBarEnabled = false;
        core.Settings.IsGeneralAutofillEnabled = false; core.Settings.IsPasswordAutosaveEnabled = false;
        core.NavigationStarting += (_, e) => { if (e.Uri != Address) e.Cancel = true; };
        core.NewWindowRequested += (_, e) => e.Handled = true;
        core.DownloadStarting += (_, e) => e.Cancel = true;
        core.PermissionRequested += (_, e) => e.State = CoreWebView2PermissionState.Deny;
        // An async void handler: anything it lets through would end the app, so every step is caught, logged and
        // answered as an error reply.
        core.WebMessageReceived += async (_, e) =>
        {
            string? json = null, response;
            try
            {
                if (e.Source != Address) return;
                json = e.WebMessageAsJson;
                using (var document = System.Text.Json.JsonDocument.Parse(json))
                    if (document.RootElement.ValueKind != System.Text.Json.JsonValueKind.Object || !document.RootElement.TryGetProperty("action", out System.Text.Json.JsonElement _)) return;
                response = await bridge.Handle(json);
            }
            catch (Exception error)
            {
                AppLog.Error("Interface message", error);
                response = JsonData.Serialize(new { id = json is null ? null : WebBridge.RequestId(json), ok = false, error = "Unexpected error: " + error.Message });
            }
            try { core.PostWebMessageAsJson(response); }
            catch (Exception exception) when (exception is InvalidOperationException or System.Runtime.InteropServices.COMException or ObjectDisposedException) { /* Window closed while a command finished. */ }
        };
    }
}

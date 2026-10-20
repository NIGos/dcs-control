using System.Net;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace DcsVr.Core;

/// <summary>A release version as tagged on GitHub: v&lt;major&gt;.&lt;minor&gt;.&lt;patch&gt;[-suffix] (the v is optional).
/// Ordered like semantic versions: a release without a suffix comes after its pre-releases (0.5.6 &gt; 0.5.6-preview), and
/// suffixes compare part by part, numbers as numbers (preview.10 &gt; preview.9).</summary>
public sealed partial record ReleaseVersion(int Major, int Minor, int Patch, string Suffix) : IComparable<ReleaseVersion>
{
    // ASCII digits only: \d also matches other scripts' digits, which int.Parse would then reject for the whole check.
    [GeneratedRegex(@"^v?([0-9]{1,6})\.([0-9]{1,6})\.([0-9]{1,6})(?:-([0-9A-Za-z]+(?:\.[0-9A-Za-z]+)*))?$", RegexOptions.CultureInvariant)]
    private static partial Regex Pattern();

    /// <summary>The version of a tag or product version; null when it is not one (a malformed tag is ignored, never guessed).</summary>
    public static ReleaseVersion? Parse(string? text)
    {
        if (text is null || text.Length > 80) return null;
        var match = Pattern().Match(text.Trim());
        if (!match.Success) return null;
        return new(int.Parse(match.Groups[1].Value, System.Globalization.CultureInfo.InvariantCulture), int.Parse(match.Groups[2].Value, System.Globalization.CultureInfo.InvariantCulture),
            int.Parse(match.Groups[3].Value, System.Globalization.CultureInfo.InvariantCulture), match.Groups[4].Value);
    }

    /// <summary>Major.minor.patch without the suffix, as the rail and the banner show it.</summary>
    public string Short => $"{Major}.{Minor}.{Patch}";
    public override string ToString() => Suffix.Length == 0 ? Short : Short + "-" + Suffix;

    public int CompareTo(ReleaseVersion? other)
    {
        if (other is null) return 1;
        var numbers = (Major, Minor, Patch).CompareTo((other.Major, other.Minor, other.Patch));
        if (numbers != 0) return numbers;
        if (Suffix.Length == 0 || other.Suffix.Length == 0) return (Suffix.Length == 0).CompareTo(other.Suffix.Length == 0);
        var mine = Suffix.Split('.'); var theirs = other.Suffix.Split('.');
        for (var i = 0; i < Math.Min(mine.Length, theirs.Length); i++)
        {
            var a = mine[i]; var b = theirs[i];
            var aNumber = a.All(char.IsAsciiDigit); var bNumber = b.All(char.IsAsciiDigit);
            var order = aNumber && bNumber ? a.TrimStart('0').Length != b.TrimStart('0').Length ? a.TrimStart('0').Length.CompareTo(b.TrimStart('0').Length) : string.CompareOrdinal(a.TrimStart('0'), b.TrimStart('0'))
                : aNumber != bNumber ? (aNumber ? -1 : 1) : string.Compare(a, b, StringComparison.OrdinalIgnoreCase);
            if (order != 0) return order;
        }
        return mine.Length.CompareTo(theirs.Length);
    }

    /// <summary>Whether <paramref name="candidate"/> is a later release than <paramref name="current"/>; false when either is not a version.</summary>
    public static bool IsNewer(string? candidate, string? current) => Parse(candidate) is { } a && Parse(current) is { } b && a.CompareTo(b) > 0;
}

/// <summary>A newer release found on GitHub. <paramref name="Notes"/> is the release's markdown body, untrusted text; the
/// setup and its signature are null when the release has no installer to download (then only its page can be opened).</summary>
public sealed record UpdateInfo(string Version, string Tag, string PageUrl, string Notes, DateTimeOffset? Published,
    string? SetupName, string? SetupUrl, long SetupBytes, string? SignatureUrl)
{
    [System.Text.Json.Serialization.JsonIgnore] public string Short => ReleaseVersion.Parse(Version)?.Short ?? Version;
    [System.Text.Json.Serialization.JsonIgnore] public bool CanInstall => SetupName is not null && SetupUrl is not null && SignatureUrl is not null;
}

/// <summary>The last check (state root\updates.json): when, for which installed version, what it found and, when GitHub
/// could not be reached, why. An update found earlier is kept through a failed check.</summary>
public sealed record UpdateCheck(DateTimeOffset CheckedAt, string CurrentVersion, UpdateInfo? Update, string? Error);

/// <summary>
/// Looks for a newer DCS Control on GitHub and downloads its setup. Network goes only to api.github.com (the release list)
/// and to the release's own asset URLs on github.com (and the githubusercontent.com hosts GitHub redirects downloads to);
/// redirects are followed by hand so no other host is ever contacted. A check runs at most once every 6 hours unless the
/// pilot asks (Check now), and never throws: offline simply means no update. A downloaded setup is kept only when its
/// ECDSA P-256 signature (SHA-256, IEEE P1363 r‖s, base64 in the .sig asset) verifies with the release key below.
/// </summary>
public sealed partial class Updates
{
    public const string ReleasesUrl = "https://api.github.com/repos/NIGos/dcs-control/releases?per_page=10";
    public const string ReleasesPage = "https://github.com/NIGos/dcs-control/releases";
    /// <summary>The public half of the release signing key. scripts/sign-release.ps1 signs each release's setup and ZIP with
    /// the private half, which never leaves the release machine; scripts/verify-web-release.py reads this constant.</summary>
    public const string PublicKeyPem = """
        -----BEGIN PUBLIC KEY-----
        MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE/4DiOD0+w2x0KP7/AbUoQkQyJyHD
        DcE8ZmbDQ1WbKHq7iSScQ/IMj1uyElcchVCv9IK4PWfxwtndZJ1uSKpm3A==
        -----END PUBLIC KEY-----
        """;
    public static readonly TimeSpan Interval = TimeSpan.FromHours(6);
    private static readonly TimeSpan ApiTimeout = TimeSpan.FromSeconds(10);
    /// <summary>A check that failed (offline, GitHub's rate limit) is tried again sooner than a successful one.</summary>
    public static readonly TimeSpan RetryInterval = TimeSpan.FromMinutes(30);
    /// <summary>A download that receives nothing for this long stops (HttpClient's timeout covers only the headers).</summary>
    private static readonly TimeSpan IdleTimeout = TimeSpan.FromSeconds(60);
    private const long MaxSetupBytes = 1L << 30, MaxListBytes = 4 << 20, MaxSignatureBytes = 4096;
    private const string AssetPath = "/NIGos/dcs-control/releases/download/";
    /// <summary>Hosts a download may be redirected to: GitHub's release asset storage.</summary>
    private static readonly string[] DownloadHosts = ["github.com", "objects.githubusercontent.com", "release-assets.githubusercontent.com"];

    private readonly string _stateRoot;
    private readonly HttpMessageHandler? _handler;
    private readonly Func<DateTimeOffset> _clock;
    private readonly string _publicKeyPem;

    /// <param name="handler">Replaces the network (tests and the offline verification); null uses HTTPS.</param>
    /// <param name="currentVersion">The installed version; <see cref="ProductInfo.Version"/> by default.</param>
    /// <param name="publicKeyPem">The release key; <see cref="PublicKeyPem"/> by default (the offline verification signs its fixture with its own).</param>
    /// <param name="downloadRoot">Where setups are downloaded; %TEMP%\DcsControl-update by default.</param>
    public Updates(string stateRoot, HttpMessageHandler? handler = null, Func<DateTimeOffset>? clock = null, string? currentVersion = null, string? publicKeyPem = null, string? downloadRoot = null)
    {
        _stateRoot = Path.GetFullPath(stateRoot); _handler = handler; _clock = clock ?? (() => DateTimeOffset.UtcNow);
        CurrentVersion = currentVersion ?? ProductInfo.Version; _publicKeyPem = publicKeyPem ?? PublicKeyPem;
        DownloadRoot = Path.GetFullPath(downloadRoot ?? Path.Combine(Path.GetTempPath(), "DcsControl-update"));
    }
    public string CurrentVersion { get; }
    public string DownloadRoot { get; }
    public string CachePath => Path.Combine(_stateRoot, "updates.json");

    /// <summary>The last check for this installed version, with an update that is no longer newer dropped; null when there is none.</summary>
    public UpdateCheck? Cached()
    {
        try
        {
            if (!File.Exists(CachePath)) return null;
            var check = JsonData.Deserialize<UpdateCheck>(File.ReadAllText(CachePath));
            if (check is null || check.CurrentVersion != CurrentVersion) return null;
            return check.Update is { } u && !ReleaseVersion.IsNewer(u.Version, CurrentVersion) ? check with { Update = null } : check;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException or NotSupportedException or ArgumentException) { return null; }
    }

    /// <summary>Asks GitHub for the releases, unless the last check is under 6 hours old (<paramref name="force"/> asks anyway).
    /// Never throws: a failure is returned as the check's error, with the update found earlier kept.</summary>
    public async Task<UpdateCheck> Check(bool force = false, CancellationToken cancel = default)
    {
        var now = _clock(); var cached = Cached();
        if (!force && cached is not null && cached.CheckedAt <= now && now - cached.CheckedAt < (cached.Error is null ? Interval : RetryInterval)) return cached;
        UpdateCheck result;
        try
        {
            using var client = Client(ApiTimeout);
            // The whole check, the answer's body included, within the timeout.
            using var whole = CancellationTokenSource.CreateLinkedTokenSource(cancel);
            whole.CancelAfter(ApiTimeout);
            using var response = await Send(client, new Uri(ReleasesUrl), ["api.github.com"], whole.Token);
            var json = Encoding.UTF8.GetString(await ReadLimited(response, MaxListBytes, whole.Token));
            result = new(now, CurrentVersion, Select(json, CurrentVersion), null);
        }
        catch (Exception e) when (e is not OutOfMemoryException)
        {
            result = new(now, CurrentVersion, cached?.Update, e is OperationCanceledException ? "GitHub did not answer within 10 s." : "GitHub could not be reached: " + e.Message);
        }
        try { AtomicFile.WriteText(CachePath, JsonData.Serialize(result)); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return result;
    }

    /// <summary>The highest release newer than <paramref name="current"/> in GitHub's release list: drafts and malformed
    /// tags are ignored, pre-releases count. Its setup (DcsControl-…-Setup.exe) and signature (the same name + .sig) are
    /// taken from its assets when both are on the release's own download path.</summary>
    public static UpdateInfo? Select(string json, string current)
    {
        var installed = ReleaseVersion.Parse(current);
        using var document = JsonDocument.Parse(json);
        if (document.RootElement.ValueKind != JsonValueKind.Array) throw new InvalidDataException("GitHub's release list is not a list.");
        (ReleaseVersion Version, JsonElement Release)? best = null;
        foreach (var release in document.RootElement.EnumerateArray())
        {
            if (release.ValueKind != JsonValueKind.Object || Bool(release, "draft") || Text(release, "tag_name") is not { } tag || ReleaseVersion.Parse(tag) is not { } version) continue;
            if (installed is not null && version.CompareTo(installed) <= 0) continue;
            if (best is null || version.CompareTo(best.Value.Version) > 0) best = (version, release);
        }
        if (best is not { } chosen) return null;
        var r = chosen.Release;
        string? setupName = null, setupUrl = null, signatureUrl = null; long setupBytes = 0;
        if (r.TryGetProperty("assets", out var assets) && assets.ValueKind == JsonValueKind.Array)
        {
            var list = assets.EnumerateArray().Where(a => a.ValueKind == JsonValueKind.Object && Text(a, "name") is not null && Text(a, "browser_download_url") is { } url && AllowedAssetUrl(url)).ToList();
            if (list.FirstOrDefault(a => SetupName().IsMatch(Text(a, "name")!)) is { ValueKind: JsonValueKind.Object } setup
                && list.FirstOrDefault(a => string.Equals(Text(a, "name"), Text(setup, "name") + ".sig", StringComparison.OrdinalIgnoreCase)) is { ValueKind: JsonValueKind.Object } signature)
            {
                setupName = Text(setup, "name"); setupUrl = Text(setup, "browser_download_url"); signatureUrl = Text(signature, "browser_download_url");
                setupBytes = setup.TryGetProperty("size", out var size) && size.TryGetInt64(out var bytes) ? bytes : 0;
            }
        }
        var page = Text(r, "html_url") is { } html && html.StartsWith(ReleasesPage + "/", StringComparison.Ordinal) && Uri.TryCreate(html, UriKind.Absolute, out _) ? html : ReleasesPage;
        var notes = Text(r, "body") ?? "";
        if (notes.Length > 64 * 1024) notes = notes[..(64 * 1024)];
        var published = Text(r, "published_at") is { } at && DateTimeOffset.TryParse(at, System.Globalization.CultureInfo.InvariantCulture, System.Globalization.DateTimeStyles.AssumeUniversal, out var p) ? p : (DateTimeOffset?)null;
        return new(chosen.Version.ToString(), Text(r, "tag_name")!, page, notes, published, setupName, setupUrl, setupBytes, signatureUrl);
    }
    [GeneratedRegex(@"^DcsControl-[0-9A-Za-z.-]+-Setup\.exe$", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant)]
    private static partial Regex SetupName();
    private static string? Text(JsonElement e, string name) => e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;
    private static bool Bool(JsonElement e, string name) => e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.True;

    /// <summary>A release asset of this repository on github.com (https only).</summary>
    public static bool AllowedAssetUrl(string url) =>
        Uri.TryCreate(url, UriKind.Absolute, out var uri) && uri.Scheme == Uri.UriSchemeHttps && uri.Host.Equals("github.com", StringComparison.OrdinalIgnoreCase)
        && uri.IsDefaultPort && uri.AbsolutePath.StartsWith(AssetPath, StringComparison.Ordinal) && !uri.AbsolutePath.Contains("..", StringComparison.Ordinal);

    /// <summary>
    /// Downloads the update's setup and signature into DownloadRoot\&lt;version&gt;\ and verifies the signature. A setup whose
    /// signature does not verify is deleted with its signature and InvalidDataException is thrown; nothing else is kept.
    /// Returns the verified setup's path.
    /// </summary>
    public async Task<string> Download(UpdateInfo update, IProgress<(long Done, long Total)>? progress = null, CancellationToken cancel = default)
    {
        if (!update.CanInstall || !AllowedAssetUrl(update.SetupUrl!) || !AllowedAssetUrl(update.SignatureUrl!) || !SetupName().IsMatch(update.SetupName!))
            throw new InvalidOperationException("This release has no installer to download. Open its page on GitHub instead.");
        if (ReleaseVersion.Parse(update.Version) is not { } version) throw new InvalidDataException("The update has no valid version.");
        var folder = Path.Combine(DownloadRoot, version.ToString());
        Directory.CreateDirectory(folder);
        var setup = Path.Combine(folder, update.SetupName!); var signaturePath = setup + ".sig"; var partial = setup + ".partial";
        using var client = Client(TimeSpan.FromMinutes(30));
        byte[] signature;
        using (var response = await Send(client, new Uri(update.SignatureUrl!), DownloadHosts, cancel))
            signature = ParseSignature(await ReadLimited(response, MaxSignatureBytes, cancel));
        try
        {
            using (var response = await Send(client, new Uri(update.SetupUrl!), DownloadHosts, cancel))
            {
                var total = response.Content.Headers.ContentLength ?? update.SetupBytes;
                if (total > MaxSetupBytes) throw new InvalidDataException("The update is larger than expected.");
                await using var input = await response.Content.ReadAsStreamAsync(cancel);
                await using var output = new FileStream(partial, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 16, FileOptions.Asynchronous);
                var buffer = new byte[1 << 16]; long done = 0; int read;
                using var idle = CancellationTokenSource.CreateLinkedTokenSource(cancel);
                while (true)
                {
                    idle.CancelAfter(IdleTimeout);
                    try { read = await input.ReadAsync(buffer, idle.Token); }
                    catch (OperationCanceledException) when (!cancel.IsCancellationRequested) { throw new IOException("The download stalled (nothing received for 60 s). Try again."); }
                    if (read <= 0) break;
                    done += read;
                    if (done > MaxSetupBytes) throw new InvalidDataException("The update is larger than expected.");
                    await output.WriteAsync(buffer.AsMemory(0, read), cancel);
                    progress?.Report((done, total));
                }
                if (update.SetupBytes > 0 && done != update.SetupBytes) throw new IOException("The download was incomplete. Try again.");
            }
            bool valid;
            await using (var stream = File.OpenRead(partial)) valid = Verify(stream, signature, _publicKeyPem);
            if (!valid) throw new InvalidDataException("The downloaded update's signature does not match DCS Control's release key. It was deleted; nothing was installed.");
            File.Move(partial, setup, overwrite: true);
            File.WriteAllText(signaturePath, Convert.ToBase64String(signature), Encoding.ASCII);
            return setup;
        }
        catch
        {
            foreach (var path in new[] { partial, setup, signaturePath }) try { File.Delete(path); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            throw;
        }
    }

    /// <summary>Verifies a setup downloaded earlier against its .sig file again (just before it is started).</summary>
    public bool VerifyFile(string path)
    {
        try
        {
            var signature = ParseSignature(File.ReadAllBytes(path + ".sig"));
            using var stream = File.OpenRead(path);
            return Verify(stream, signature, _publicKeyPem);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException) { return false; }
    }

    /// <summary>A .sig file: the base64 of the 64-byte IEEE P1363 signature (r‖s).</summary>
    public static byte[] ParseSignature(byte[] file)
    {
        try
        {
            var signature = Convert.FromBase64String(Encoding.ASCII.GetString(file).Trim());
            return signature.Length == 64 ? signature : throw new InvalidDataException("The update's signature has the wrong length.");
        }
        catch (FormatException) { throw new InvalidDataException("The update's signature is not readable."); }
    }

    /// <summary>ECDSA P-256 with SHA-256 over the whole file, signature in IEEE P1363 form. A key that is not P-256 never verifies.</summary>
    public static bool Verify(Stream data, byte[] signature, string publicKeyPem)
    {
        using var key = ECDsa.Create();
        key.ImportFromPem(publicKeyPem);
        return key.KeySize == 256 && key.VerifyData(data, signature, HashAlgorithmName.SHA256, DSASignatureFormat.IeeeP1363FixedFieldConcatenation);
    }

    /// <summary>Removes setups downloaded for earlier updates (best effort: a setup still running is left for next time).</summary>
    public void CleanDownloads()
    {
        try { if (Directory.Exists(DownloadRoot)) foreach (var folder in Directory.EnumerateDirectories(DownloadRoot)) try { Directory.Delete(folder, true); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { } }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }

    /// <summary>The section of RELEASE_NOTES.md for <paramref name="version"/>: from its "# 0.5.6 preview" heading to the
    /// next "# " heading, trimmed; null when there is none.</summary>
    public static string? ReleaseNotesSection(string markdown, string version)
    {
        if (ReleaseVersion.Parse(version) is not { } v) return null;
        var heading = "# " + v.Short + (v.Suffix.Length > 0 ? " " + v.Suffix.Replace('.', ' ') : "");
        var lines = markdown.Replace("\r\n", "\n", StringComparison.Ordinal).Split('\n');
        var start = Array.FindIndex(lines, l => l.TrimEnd().Equals(heading, StringComparison.OrdinalIgnoreCase));
        if (start < 0) return null;
        var end = Array.FindIndex(lines, start + 1, l => l.StartsWith("# ", StringComparison.Ordinal));
        var text = string.Join("\n", lines[(start + 1)..(end < 0 ? lines.Length : end)]).Trim();
        return text.Length == 0 ? null : text;
    }

    private HttpClient Client(TimeSpan timeout)
    {
        var client = _handler is null ? new HttpClient(new SocketsHttpHandler { AllowAutoRedirect = false, AutomaticDecompression = DecompressionMethods.All }, disposeHandler: true) : new HttpClient(_handler, disposeHandler: false);
        client.Timeout = timeout;
        client.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("DcsControl", ReleaseVersion.Parse(CurrentVersion)?.ToString() ?? "0"));
        return client;
    }

    /// <summary>GET with redirects followed by hand, each hop https and on <paramref name="hosts"/> only.</summary>
    private static async Task<HttpResponseMessage> Send(HttpClient client, Uri uri, IReadOnlyCollection<string> hosts, CancellationToken cancel)
    {
        for (var hop = 0; hop < 6; hop++)
        {
            if (uri.Scheme != Uri.UriSchemeHttps || !hosts.Contains(uri.Host, StringComparer.OrdinalIgnoreCase)) throw new InvalidDataException("Refused to contact " + uri.Host + ".");
            using var request = new HttpRequestMessage(HttpMethod.Get, uri);
            if (uri.Host.Equals("api.github.com", StringComparison.OrdinalIgnoreCase)) request.Headers.Accept.Add(new("application/vnd.github+json"));
            var response = await client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cancel);
            if ((int)response.StatusCode is 301 or 302 or 303 or 307 or 308 && response.Headers.Location is { } location)
            {
                uri = location.IsAbsoluteUri ? location : new Uri(uri, location); response.Dispose(); continue;
            }
            if (!response.IsSuccessStatusCode) { var code = (int)response.StatusCode; response.Dispose(); throw new HttpRequestException("HTTP " + code + " from " + uri.Host + "."); }
            return response;
        }
        throw new HttpRequestException("Too many redirects.");
    }

    private static async Task<byte[]> ReadLimited(HttpResponseMessage response, long limit, CancellationToken cancel)
    {
        await using var stream = await response.Content.ReadAsStreamAsync(cancel);
        using var memory = new MemoryStream();
        var buffer = new byte[16384]; int read;
        while ((read = await stream.ReadAsync(buffer, cancel)) > 0)
        {
            if (memory.Length + read > limit) throw new InvalidDataException("The answer is larger than expected.");
            memory.Write(buffer, 0, read);
        }
        return memory.ToArray();
    }
}

/// <param name="CheckForUpdates">Look for a newer release when DCS Control starts (Diagnostics page); on by default.</param>
/// <param name="LastSeenVersion">The version whose "What's new" was last shown at start.</param>
public sealed record AppSettings(bool CheckForUpdates = true, string? LastSeenVersion = null);

/// <summary>The app's own settings (state root\settings.json), apart from profiles. Read leniently: a missing or unreadable
/// file, or keys a later version added, give the defaults for what is missing.</summary>
public sealed class AppSettingsStore(string stateRoot)
{
    public string Path { get; } = System.IO.Path.Combine(stateRoot, "settings.json");

    public AppSettings Load() => Read() ?? new();

    /// <summary>The settings, or null when the file exists but cannot be read now (then nothing is saved over it, so a
    /// briefly locked or damaged file never loses the pilot's choices).</summary>
    public AppSettings? Read()
    {
        try
        {
            if (!File.Exists(Path)) return new();
            using var document = JsonDocument.Parse(File.ReadAllText(Path));
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object) return new();
            return new(!root.TryGetProperty("checkForUpdates", out var check) || check.ValueKind != JsonValueKind.False,
                root.TryGetProperty("lastSeenVersion", out var seen) && seen.ValueKind == JsonValueKind.String ? seen.GetString() : null);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException) { return null; }
    }

    public void Save(AppSettings settings) => AtomicFile.WriteText(Path, JsonData.Serialize(settings));

    /// <summary>Whether this start should show <paramref name="version"/>'s release notes, and records it as seen so they
    /// show once. A first start without a version on record shows them only when the app was used before
    /// (<paramref name="usedBefore"/>: an update from a version that did not record it), not on a new installation.</summary>
    public bool TakeWhatsNew(string version, bool usedBefore)
    {
        if (Read() is not { } settings || settings.LastSeenVersion == version) return false;
        Save(settings with { LastSeenVersion = version });
        return settings.LastSeenVersion is not null || usedBefore;
    }
}

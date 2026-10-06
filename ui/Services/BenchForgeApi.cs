using System.Net.Http.Json;
using System.Globalization;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace BenchForge.UI.Services;

public sealed class BenchForgeApi(HttpClient http)
{
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web);

    public async Task<bool> IsHealthyAsync(CancellationToken cancellationToken = default)
    {
        using var response = await http.GetAsync("health", cancellationToken);
        if (!response.IsSuccessStatusCode)
            return false;
        using var document = await JsonDocument.ParseAsync(
            await response.Content.ReadAsStreamAsync(cancellationToken),
            cancellationToken: cancellationToken);
        return document.RootElement.TryGetProperty("status", out var status) &&
               status.GetString() == "ok";
    }

    public async Task<IReadOnlyList<AdapterInfo>> GetAdaptersAsync(
        CancellationToken cancellationToken = default)
    {
        var response = await GetAsync<AdapterList>("api/adapters", cancellationToken);
        return response.Adapters;
    }

    public async Task<IReadOnlyList<RunInfo>> GetRunsAsync(
        CancellationToken cancellationToken = default)
    {
        var response = await GetAsync<RunList>("api/runs", cancellationToken);
        return response.Runs;
    }

    public Task<RunInfo> GetRunAsync(string runId,
        CancellationToken cancellationToken = default) =>
        GetAsync<RunInfo>($"api/runs/{Uri.EscapeDataString(runId)}", cancellationToken);

    public async Task<RunInfo> StartRunAsync(RunConfiguration run,
        CancellationToken cancellationToken = default)
    {
        var fields = new Dictionary<string, string>
        {
            ["adapter"] = run.Adapter,
            ["scenario"] = run.Scenario,
            ["mode"] = run.Mode,
            ["offered_rate_ops_sec"] = Number(run.OfferedRateOpsSec),
            ["seed"] = Number(run.Seed),
            ["workers"] = Number(run.Workers),
            ["warmup_ms"] = Number(run.WarmupMs),
            ["duration_ms"] = Number(run.DurationMs),
            ["users"] = Number(run.Users),
            ["posts"] = Number(run.Posts),
            ["follows"] = Number(run.Follows),
            ["hashtags"] = Number(run.Hashtags),
            ["celebrity_post_percent"] = Number(run.CelebrityPostPercent),
            ["weight.timeline_read"] = Number(run.TimelineReadWeight),
            ["weight.user_profile_read"] = Number(run.UserProfileReadWeight),
            ["weight.post_like"] = Number(run.PostLikeWeight),
            ["weight.post_create"] = Number(run.PostCreateWeight),
            ["weight.hashtag_search"] = Number(run.HashtagSearchWeight),
            ["weight.user_follow"] = Number(run.UserFollowWeight)
        };

        using var content = new FormUrlEncodedContent(fields);
        using var response = await http.PostAsync("api/runs", content, cancellationToken);
        return await ReadResponseAsync<RunInfo>(response, cancellationToken);
    }

    public async Task<RunInfo> CancelRunAsync(string runId,
        CancellationToken cancellationToken = default)
    {
        using var response = await http.PostAsync(
            $"api/runs/{Uri.EscapeDataString(runId)}/cancel", null, cancellationToken);
        return await ReadResponseAsync<RunInfo>(response, cancellationToken);
    }

    public async Task<RunSummary> GetSummaryAsync(string runId,
        CancellationToken cancellationToken = default)
    {
        return await GetAsync<RunSummary>(
            $"api/runs/{Uri.EscapeDataString(runId)}/results", cancellationToken);
    }

    private async Task<T> GetAsync<T>(string path, CancellationToken cancellationToken)
    {
        using var response = await http.GetAsync(path, cancellationToken);
        return await ReadResponseAsync<T>(response, cancellationToken);
    }

    private static string Number<T>(T value) where T : IFormattable =>
        value.ToString(null, CultureInfo.InvariantCulture) ?? "";

    private static async Task<T> ReadResponseAsync<T>(HttpResponseMessage response,
        CancellationToken cancellationToken)
    {
        var body = await response.Content.ReadAsStringAsync(cancellationToken);
        if (!response.IsSuccessStatusCode)
        {
            var message = $"Control API returned {(int)response.StatusCode}.";
            try
            {
                using var document = JsonDocument.Parse(body);
                if (document.RootElement.TryGetProperty("error", out var error))
                    message = error.GetString() ?? message;
            }
            catch (JsonException)
            {
                // Keep the status-based message when the response is not JSON.
            }
            throw new InvalidOperationException(message);
        }

        var value = JsonSerializer.Deserialize<T>(body, JsonOptions);
        return value ?? throw new InvalidOperationException("Control API returned an empty response.");
    }
}

public sealed class RunConfiguration
{
    public string Adapter { get; set; } = "noop";
    public string Scenario { get; set; } = "normal";
    public string Mode { get; set; } = "closed_loop";
    public ulong OfferedRateOpsSec { get; set; } = 1_000;
    public ulong Seed { get; set; } = 42;
    public int Workers { get; set; } = 4;
    public int WarmupMs { get; set; } = 250;
    public int DurationMs { get; set; } = 1000;
    public ulong Users { get; set; } = 10_000;
    public ulong Posts { get; set; } = 100_000;
    public ulong Follows { get; set; } = 200_000;
    public ulong Hashtags { get; set; } = 1_000;
    public int CelebrityPostPercent { get; set; } = 10;
    public int TimelineReadWeight { get; set; } = 40;
    public int UserProfileReadWeight { get; set; } = 25;
    public int PostLikeWeight { get; set; } = 15;
    public int PostCreateWeight { get; set; } = 10;
    public int HashtagSearchWeight { get; set; } = 5;
    public int UserFollowWeight { get; set; } = 5;

    [JsonIgnore]
    public int OperationWeightTotal => TimelineReadWeight + UserProfileReadWeight +
        PostLikeWeight + PostCreateWeight + HashtagSearchWeight + UserFollowWeight;
}

public sealed class AdapterList
{
    public List<AdapterInfo> Adapters { get; set; } = [];
}

public sealed class AdapterInfo
{
    public string Name { get; set; } = "";
}

public sealed class RunList
{
    public List<RunInfo> Runs { get; set; } = [];
}

public sealed class RunInfo
{
    public string RunId { get; set; } = "";
    public string Status { get; set; } = "";
    public string Adapter { get; set; } = "";
    public string Scenario { get; set; } = "";
    public string CreatedAtUtc { get; set; } = "";
    public int Workers { get; set; }
    public int DurationMs { get; set; }
    public int? ExitCode { get; set; }
    public string Error { get; set; } = "";
}

public sealed class RunSummary
{
    [JsonPropertyName("run_id")]
    public string RunId { get; set; } = "";

    public string Adapter { get; set; } = "";
    public string Scenario { get; set; } = "";
    public string Mode { get; set; } = "";
    [JsonPropertyName("cleanup_status")]
    public string CleanupStatus { get; set; } = "";
    public ulong Seed { get; set; }
    public int Workers { get; set; }

    [JsonPropertyName("warmup_ms")]
    public long WarmupMs { get; set; }

    [JsonPropertyName("measured_ms")]
    public long MeasuredMs { get; set; }

    [JsonPropertyName("total_operations")]
    public long TotalOperations { get; set; }

    [JsonPropertyName("total_errors")]
    public long TotalErrors { get; set; }

    [JsonPropertyName("total_timeouts")]
    public long TotalTimeouts { get; set; }

    [JsonPropertyName("offered_rate_ops_sec")]
    public ulong OfferedRateOpsSec { get; set; }

    public bool Valid { get; set; }

    [JsonPropertyName("invalid_reasons")]
    public List<string> InvalidReasons { get; set; } = [];

    [JsonPropertyName("telemetry_dropped")]
    public long TelemetryDropped { get; set; }

    [JsonPropertyName("histogram_resolution")]
    public string HistogramResolution { get; set; } = "";

    public EnvironmentSummary Environment { get; set; } = new();

    [JsonPropertyName("transport_calibration")]
    public TransportCalibrationSummary TransportCalibration { get; set; } = new();

    public List<OperationSummary> Operations { get; set; } = [];
}

public sealed class OperationSummary
{
    public string Type { get; set; } = "";
    public long Count { get; set; }
    public long Errors { get; set; }
    public long Timeouts { get; set; }

    [JsonPropertyName("ops_per_sec")]
    public double OperationsPerSecond { get; set; }

    [JsonPropertyName("p50_ns")]
    public long P50Ns { get; set; }

    [JsonPropertyName("p95_ns")]
    public long P95Ns { get; set; }

    [JsonPropertyName("p99_ns")]
    public long P99Ns { get; set; }

    [JsonPropertyName("p999_ns")]
    public long P999Ns { get; set; }

    [JsonPropertyName("min_ns")]
    public long MinNs { get; set; }

    [JsonPropertyName("max_ns")]
    public long MaxNs { get; set; }

    [JsonPropertyName("mean_ns")]
    public double MeanNs { get; set; }

    [JsonPropertyName("send_lag_p95_ns")]
    public long SendLagP95Ns { get; set; }
}

public sealed class EnvironmentSummary
{
    [JsonPropertyName("host_name")]
    public string HostName { get; set; } = "";

    [JsonPropertyName("operating_system")]
    public string OperatingSystem { get; set; } = "";

    public string Architecture { get; set; } = "";

    [JsonPropertyName("logical_processors")]
    public int LogicalProcessors { get; set; }

    [JsonPropertyName("total_memory_bytes")]
    public ulong TotalMemoryBytes { get; set; }
}

public sealed class TransportCalibrationSummary
{
    public string Kind { get; set; } = "";
    public long Samples { get; set; }

    [JsonPropertyName("p50_ns")]
    public long P50Ns { get; set; }

    [JsonPropertyName("p95_ns")]
    public long P95Ns { get; set; }

    [JsonPropertyName("mean_ns")]
    public long MeanNs { get; set; }
}

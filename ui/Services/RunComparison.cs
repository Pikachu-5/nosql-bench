using System.Globalization;
namespace BenchForge.UI.Services;

public sealed record ComparisonDifference(string Field, string Baseline, string Candidate);
public sealed record ComparisonAssessment(List<string> Issues, List<ComparisonDifference> Differences)
{
    public bool CanCompare => Issues.Count == 0 && Differences.Count == 0;
}

public static class RunComparison
{
    private static readonly HashSet<string> OperationTypes = ["timeline_read", "user_profile_read", "post_like", "post_create", "hashtag_search", "user_follow"];
    public static ComparisonAssessment Assess(RunSummary a, RunSummary b)
    {
        var issues = new List<string>();
        var differences = new List<ComparisonDifference>();
        if (a.RunId == b.RunId) issues.Add("Choose two different runs.");
        foreach (var (run, side) in new[] { (a, "Baseline"), (b, "Candidate") })
        {
            if (!run.HasReadableShape()) return new(["A capture has unreadable metadata. Choose another saved run."], []);
            if (run.Adapter == "noop") issues.Add($"{side} is harness calibration; it does not measure a database.");
            if (!run.Valid || run.TotalErrors != 0 || run.TotalTimeouts != 0 || run.TelemetryDropped != 0 ||
                run.TotalOperations <= 0 || run.MeasuredMs <= 0 || run.InvalidReasons.Count != 0 ||
                run.CleanupStatus.Length == 0 || run.CleanupStatus.Contains("failed", StringComparison.OrdinalIgnoreCase))
                issues.Add($"{side} has invalid or incomplete measurement evidence.");
            if (run.SchemaVersion != 2 || run.Config is null || run.Config.OperationWeights.Count != 6 ||
                run.Environment.HostName.Length == 0 || run.AdapterVersion.Length == 0 || run.StorageConfiguration.Length == 0)
                issues.Add($"{side} is missing configuration or environment metadata.");
            if (run.Operations.Count != 6 || run.Operations.Select(op => op.Type).Distinct().Count() != 6 ||
                run.Operations.Sum(op => (decimal)op.Count) != run.TotalOperations || run.Operations.Any(op =>
                    !OperationTypes.Contains(op.Type) ||
                    op.Count < 0 || op.Errors < 0 || op.Errors > op.Count || op.Timeouts < 0 || op.Timeouts > op.Errors ||
                    op.SendLagP95Ns < 0 || op.MinNs < 0 || op.P50Ns < op.MinNs || op.P95Ns < op.P50Ns ||
                    op.P99Ns < op.P95Ns || op.P999Ns < op.P99Ns || op.MaxNs < op.MinNs ||
                    !double.IsFinite(op.OperationsPerSecond) || op.OperationsPerSecond < 0))
                issues.Add($"{side} has inconsistent operation counts or latency values.");
            if (run.Operations.Sum(op => (decimal)op.Errors) != run.TotalErrors ||
                run.Operations.Sum(op => (decimal)op.Timeouts) != run.TotalTimeouts)
                issues.Add($"{side} has inconsistent error or timeout totals.");
        }
        void Match<T>(string field, T left, T right)
        {
            if (!EqualityComparer<T>.Default.Equals(left, right))
                differences.Add(new(field, Convert.ToString(left, CultureInfo.InvariantCulture) ?? "", Convert.ToString(right, CultureInfo.InvariantCulture) ?? ""));
        }
        Match("Scenario", a.Scenario, b.Scenario); Match("Seed", a.Seed, b.Seed);
        Match("Workers", a.Workers, b.Workers); Match("Warm-up (ms)", a.WarmupMs, b.WarmupMs);
        Match("Load model", a.Mode, b.Mode); Match("Histogram resolution", a.HistogramResolution, b.HistogramResolution);
        Match("Measurement method", a.MeasurementMethod, b.MeasurementMethod);
        Match("Database resource profile", a.ResourceProfile, b.ResourceProfile);
        if (a.Mode == "open_loop" || b.Mode == "open_loop") Match("Offered rate (ops/s)", a.OfferedRateOpsSec, b.OfferedRateOpsSec);
        Match("Host", a.Environment.HostName, b.Environment.HostName);
        Match("Operating system", a.Environment.OperatingSystem, b.Environment.OperatingSystem);
        Match("Architecture", a.Environment.Architecture, b.Environment.Architecture);
        Match("Logical processors", a.Environment.LogicalProcessors, b.Environment.LogicalProcessors);
        Match("Physical memory (bytes)", a.Environment.TotalMemoryBytes, b.Environment.TotalMemoryBytes);
        if (a.Config is { } ac && b.Config is { } bc)
        {
            Match("Duration (ms)", ac.DurationMs, bc.DurationMs);
            Match("Users", ac.Users, bc.Users); Match("Posts", ac.Posts, bc.Posts);
            Match("Follows", ac.Follows, bc.Follows); Match("Hashtags", ac.Hashtags, bc.Hashtags);
            Match("Celebrity post bias (%)", ac.CelebrityPostPercent, bc.CelebrityPostPercent);
            foreach (var key in ac.OperationWeights.Keys.Union(bc.OperationWeights.Keys).Order())
                Match($"Weight: {key}", ac.OperationWeights.GetValueOrDefault(key, -1), bc.OperationWeights.GetValueOrDefault(key, -1));
        }
        return new(issues, differences);
    }
    public static double Throughput(RunSummary run) => run.MeasuredMs > 0 ? run.TotalOperations * 1000d / run.MeasuredMs : 0;
    public static string Latency(long ns) => ns < 0 ? "Unavailable" : ns >= 1_000_000 ? $"{ns / 1_000_000d:0.###} ms" : ns >= 1_000 ? $"{ns / 1_000d:0.###} µs" : $"{ns} ns";
    public static string Change(double baseline, double candidate) => baseline > 0 ? $"{(candidate / baseline - 1) * 100:+0.0;-0.0;0.0}%" : "—";
}

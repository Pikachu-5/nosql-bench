namespace BenchForge.UI.Services;

public sealed record MetricRange(double Median, double Minimum, double Maximum)
{
    public static MetricRange From(IEnumerable<double> values)
    {
        var sorted = values.Order().ToArray();
        if (sorted.Length == 0 || sorted.Any(value => !double.IsFinite(value)))
            throw new ArgumentException("A range needs finite measurements.");
        var middle = sorted.Length / 2;
        return new(sorted.Length % 2 == 0 ? sorted[middle-1]/2 + sorted[middle]/2 : sorted[middle], sorted[0], sorted[^1]);
    }
}
public sealed record RepetitionGroup(List<RunSummary> Runs, List<string> Excluded)
{
    public bool Ready => Runs.Count >= 3;
    public MetricRange Throughput => MetricRange.From(Runs.Select(RunComparison.Throughput));
    public MetricRange Operation(string type, Func<OperationSummary,double> metric) =>
        MetricRange.From(Runs.Select(run => metric(run.Operations.Single(op => op.Type == type))));
}
public static class RunRepetitions
{
    public static RepetitionGroup Group(RunSummary anchor, IEnumerable<RunSummary> captures)
    {
        var accepted = new List<RunSummary>(); var excluded = new List<string>();
        var seen = new HashSet<string>();
        foreach (var run in captures)
        {
            if (!seen.Add(run.RunId)) { excluded.Add($"{run.RunId}: duplicate run ID"); continue; }
            // Assess with a distinct identity to validate the anchor itself as well.
            var check = RunComparison.Assess(anchor,run);
            var issues = check.Issues.Where(issue => issue != "Choose two different runs.").ToArray();
            static string Storage(RunSummary value) => System.Text.RegularExpressions.Regex.Replace(
                value.StorageConfiguration.Replace($"benchforge:{value.RunId}:","benchforge:<run>:"),
                "benchforge_run_[0-9a-f]{16}","benchforge_run_<run>");
            if (anchor.ExperimentId.Length == 0 || run.ExperimentId != anchor.ExperimentId ||
                run.ExperimentProfile != anchor.ExperimentProfile || run.Adapter != anchor.Adapter ||
                run.AdapterVersion != anchor.AdapterVersion || run.ResourceProfile.Length == 0 ||
                Storage(run) != Storage(anchor) ||
                run.ResourceProfile != anchor.ResourceProfile || issues.Length != 0 || check.Differences.Count != 0)
                excluded.Add($"{run.RunId}: invalid capture or different experiment conditions");
            else accepted.Add(run);
        }
        return new(accepted,excluded);
    }
}

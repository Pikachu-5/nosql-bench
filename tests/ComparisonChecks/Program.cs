using BenchForge.UI.Services;
using System.Text.Json;

static void Check(bool value, string message) { if (!value) throw new Exception(message); }
static RunSummary Fixture(string id) => new() {
    RunId=id, SchemaVersion=2, Adapter="neo4j", AdapterVersion="Neo4j 5.26", StorageConfiguration="durable", CleanupStatus="run graph removed", Valid=true,
    Mode="open_loop", Scenario="normal", Seed=42, Workers=2, MeasuredMs=1000, WarmupMs=100, OfferedRateOpsSec=100,
    TotalOperations=60, Config=new() { Users=32,Posts=128,Follows=64,Hashtags=8,DurationMs=1000,
        OperationWeights=new() { ["timeline_read"]=40,["user_profile_read"]=25,["post_like"]=15,["post_create"]=10,["hashtag_search"]=5,["user_follow"]=5 } },
    Environment=new() {HostName="fixture",OperatingSystem="test",Architecture="x64",LogicalProcessors=4,TotalMemoryBytes=16_000_000_000},
    Operations=new[] { "timeline_read","user_profile_read","post_like","post_create","hashtag_search","user_follow" }
        .Select(type=>new OperationSummary {Type=type,Count=10,MinNs=100,P50Ns=100,P95Ns=150,P99Ns=200,P999Ns=200,MaxNs=190,OperationsPerSecond=10}).ToList()
};
var a=Fixture("a"); var b=Fixture("b");
Check(RunComparison.Assess(a,b).CanCompare,"matching conditions rejected approximate histogram bounds");
b.Adapter="noop"; Check(!RunComparison.Assess(a,b).CanCompare,"noop accepted as database"); b.Adapter="neo4j";
b.Valid=false; Check(!RunComparison.Assess(a,b).CanCompare,"invalid run accepted"); b.Valid=true;
b.Config!.Posts++; Check(RunComparison.Assess(a,b).Differences.Any(d=>d.Field=="Posts"),"dataset mismatch lost"); b.Config.Posts--;
b.Config.OperationWeights["post_like"]++; Check(!RunComparison.Assess(a,b).CanCompare,"operation mix mismatch ignored"); b.Config.OperationWeights["post_like"]--;
b.Mode="closed_loop"; Check(!RunComparison.Assess(a,b).CanCompare,"load mode mismatch ignored"); b.Mode="open_loop";
b.Environment.HostName="other"; Check(!RunComparison.Assess(a,b).CanCompare,"host mismatch ignored"); b.Environment.HostName="fixture";
b.Config=null; Check(!RunComparison.Assess(a,b).CanCompare,"missing metadata accepted"); b=Fixture("b");
b.Operations[0].Count--; Check(!RunComparison.Assess(a,b).CanCompare,"inconsistent counts accepted");
b=Fixture("b"); b.Operations[0].Count=long.MaxValue; Check(!RunComparison.Assess(a,b).CanCompare,"overflow-sized sample counts accepted");
b=Fixture("b"); b.Environment=null!; Check(!RunComparison.Assess(a,b).CanCompare,"null metadata accepted");
Check(RunComparison.Change(0,10)=="—","division by zero not guarded");
Check(RunComparison.Throughput(a)==60,"achieved throughput denominator incorrect");
if (args.Length==2) {
    var options=new JsonSerializerOptions(JsonSerializerDefaults.Web);
    var realA=JsonSerializer.Deserialize<RunSummary>(File.ReadAllText(args[0]),options)!;
    var realB=JsonSerializer.Deserialize<RunSummary>(File.ReadAllText(args[1]),options)!;
    var result=RunComparison.Assess(realA,realB);
    Check(result.CanCompare,"real matching smoke capture conditions rejected: "+string.Join(";",result.Issues.Concat(result.Differences.Select(d=>d.Field))));
}
Console.WriteLine("Comparison eligibility checks passed");

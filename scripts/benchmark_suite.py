"""Sequential local Docker experiment; synthetic data, standard-library only."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shutil
import socket
import statistics
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
IMAGES = {"feedkv":"benchforge-feedkv:local",
    "redis":"redis:7.4-bookworm@sha256:4fa24486b8bcca8eec45ee0eb166edc674795e53a2b53d1a9ef263eecebaac85",
    "mongo":"mongo:8.0@sha256:d0d926f94df099bff534b7ee5b5986458131a22489dfff8664509af0c1e2ca9c",
    "cassandra":"cassandra:4.1@sha256:57e5dd97a964fa97f96269c821941c910f9fc7ee905ea4cba586b7b63eb18883",
    "neo4j":"neo4j:5.26-community@sha256:c7d25c0eeebfe125718d58b72ed5d663c85d0479047733845eb2237c67ce8069"}
PORTS = {"feedkv":16380,"redis":16379,"mongo":27018,"cassandra":9042,"neo4j":7474}
PROFILES = [("normal","closed_loop"),("normal","open_loop"),("celebrity","closed_loop"),("celebrity","open_loop")]

def execute(args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, capture_output=True, text=True, timeout=kwargs.pop("timeout",120), **kwargs).stdout.strip()

def docker(*args, **kwargs): return execute(["docker",*args], **kwargs)

def write_json(path, value):
    path.parent.mkdir(parents=True,exist_ok=True)
    temporary=path.with_suffix(path.suffix+".tmp")
    temporary.write_text(json.dumps(value,indent=2)+"\n",encoding="utf-8")
    temporary.replace(path)

def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()

def summarize(results):
    rows=[]
    for adapter in dict.fromkeys(run["adapter"] for run in results):
        for scenario,mode in PROFILES:
            runs=[run for run in results if run["adapter"]==adapter and run["scenario"]==scenario and run["mode"]==mode]
            valid=[run for run in runs if run["valid"] and run["total_errors"]==0 and run["total_timeouts"]==0 and run["telemetry_dropped"]==0 and run["measured_ms"]>0 and "failed" not in run["cleanup_status"]]
            def distribution(values): return {"median":statistics.median(values),"minimum":min(values),"maximum":max(values)} if values else None
            row={"adapter":adapter,"scenario":scenario,"mode":mode,"valid_repetitions":len(valid),"excluded_run_ids":[run["run_id"] for run in runs if run not in valid],"ops_per_sec":distribution([run["total_operations"]*1000/run["measured_ms"] for run in valid]),"operations":{}}
            for op in (valid[0]["operations"] if valid else []):
                measurements=[next(item for item in run["operations"] if item["type"]==op["type"]) for run in valid]
                row["operations"][op["type"]]={field:distribution([item[field] for item in measurements]) for field in ["count","ops_per_sec","p50_ns","p95_ns","p99_ns","p999_ns","send_lag_p95_ns"]}
            rows.append(row)
    return rows

def config(path,adapter,output,duration,warmup,scenario,mode,rate,small=False):
    values={"adapter":adapter,"database_host":"127.0.0.1","database_port":PORTS[adapter],"scenario":scenario,"mode":mode,"offered_rate_ops_sec":rate,"seed":42,"workers":2,"warmup_ms":warmup,"duration_ms":duration,"users":8 if small else 64,"posts":16 if small else 256,"follows":16 if small else 128,"hashtags":4 if small else 8,"celebrity_post_percent":10,"output_dir":output.as_posix()}
    for name,value in zip(["timeline_read","user_profile_read","post_like","post_create","hashtag_search","user_follow"],[40,25,15,10,5,5]): values["weight."+name]=value
    path.write_text("\n".join(f"{key} = {value}" for key,value in values.items())+"\n",encoding="utf-8")
    return values

def sample_resources(container,stop,samples):
    # Container observations cover the whole worker lifetime, including loading.
    while not stop.is_set():
        try:
            value=json.loads(docker("stats","--no-stream","--format","{{json .}}",container,timeout=10))
            samples.append({"at_utc":datetime.now(timezone.utc).isoformat(),"docker_stats":value})
        except (subprocess.SubprocessError,ValueError): pass
        stop.wait(.5)

def run_suite(args):
    if not re.fullmatch(r"[a-zA-Z0-9_-]{1,24}",args.experiment): raise ValueError("experiment ID must be 1–24 safe characters")
    if args.repetitions<3 or args.repetitions>30: raise ValueError("use 3–30 repetitions")
    if args.duration_ms<1000 or args.duration_ms>600000: raise ValueError("duration must be 1000–600000 ms")
    if args.rate<1 or args.rate>1000000: raise ValueError("rate must be 1–1000000 ops/s")
    evidence=ROOT/"evidence"/"benchmarks"/args.experiment
    if evidence.exists(): raise ValueError("experiment already exists; use a new ID")
    evidence.mkdir(parents=True)
    output=ROOT/"runs"/"experiments"; output.mkdir(parents=True,exist_ok=True)
    executable=Path(args.executable).resolve()
    resource_profile="docker_linux_cpu2_mem2g_single_server"
    manifest={"experiment_id":args.experiment,"state":"running","started_at_utc":datetime.now(timezone.utc).isoformat(),"source_commit":execute(["git","rev-parse","HEAD"]),"source_dirty":bool(execute(["git","status","--porcelain"])),"executable_sha256":digest(executable),"docker_version":json.loads(docker("version","--format","{{json .}}")),"resource_profile":resource_profile,"cpu_limit":2,"memory_limit_bytes":2147483648,"dataset":{"users":64,"posts":256,"follows":128,"hashtags":8},"duration_ms":args.duration_ms,"warmup_ms":1000,"repetitions":args.repetitions,"offered_rate_ops_sec":args.rate,"adapters":{},"runs":[],"limitations":["Single laptop, single-node servers and a small synthetic dataset; no universal engine ranking.","Different native models, protocols and durability settings; only FeedKV/Redis share RESP and volatile configuration.","Resource samples cover loading and measurement, not exact measurement-only CPU; GC pauses are not isolated.","Closed-loop run operation counts vary; open-loop runs use an identical seeded offered workload.","Only the current adapter container runs during capture; ordinary host activity is uncontrolled."]}
    results=[]
    manifest["build"]={"cmake_cache":(executable.parent/"CMakeCache.txt").read_text(encoding="utf-8") if (executable.parent/"CMakeCache.txt").exists() else "not available", "feedkv_container_compiler":"GCC 14, C++20, -O3 -DNDEBUG -pthread"}
    write_json(evidence/"experiment.json",manifest)
    try:
        for adapter in args.adapters:
            port=PORTS[adapter]
            with socket.socket() as check:
                check.bind(("127.0.0.1",port))
            name=f"bf-{args.experiment}-{adapter}"
            options=["run","-d","--name",name,"--label",f"benchforge.experiment={args.experiment}","--cpus","2","--memory","2g","--memory-swap","2g","-p",f"127.0.0.1:{port}:{6380 if adapter=='feedkv' else 6379 if adapter=='redis' else 27017 if adapter=='mongo' else port}"]
            if adapter=="cassandra": options += ["-e","MAX_HEAP_SIZE=768M","-e","HEAP_NEWSIZE=128M"]
            if adapter=="neo4j": options += ["-e","NEO4J_AUTH=none","-e","NEO4J_server_memory_heap_initial__size=512m","-e","NEO4J_server_memory_heap_max__size=512m","-e","NEO4J_server_memory_pagecache_size=256m"]
            options += [IMAGES[adapter]]
            if adapter=="redis": options += ["redis-server","--save","","--appendonly","no","--maxmemory","1536mb","--maxmemory-policy","noeviction"]
            container=docker(*options)
            try:
                deadline=time.monotonic()+180; probe_count=0
                probe_conf=evidence/f"{adapter}-probe.conf"
                config(probe_conf,adapter,ROOT/"runs"/"verification",100,0,"normal","open_loop",20,small=True)
                while True:
                    probe_count+=1
                    probe_id=f"{args.experiment}_{adapter}_probe{probe_count}"
                    probe=subprocess.run([str(executable),"run","--config",str(probe_conf),"--run-id",probe_id],capture_output=True,text=True,timeout=45)
                    if probe.returncode==0: break
                    if time.monotonic()>deadline: raise RuntimeError(f"{adapter} readiness failed: {probe.stderr} {probe.stdout}")
                    time.sleep(2)
                # Integration programs verify seeds and state through independent native clients.
                integration=executable.parent / ("resp_integration" if adapter in ("feedkv","redis") else f"{adapter}_integration")
                if executable.suffix: integration=integration.with_suffix(executable.suffix)
                if integration.exists():
                    command=[integration,adapter,port] if adapter in ("feedkv","redis","mongo") else [integration]
                    verification=execute(command,timeout=180)
                    (evidence/f"{adapter}-verification.txt").write_text(verification+"\n")
                inspection=json.loads(docker("inspect",container))[0]
                manifest["adapters"][adapter]={"image":IMAGES[adapter],"image_id":inspection["Image"],"image_metadata":json.loads(docker("image","inspect",inspection["Image"]))[0],"start_arguments":options,"host_config":{"NanoCpus":inspection["HostConfig"]["NanoCpus"],"Memory":inspection["HostConfig"]["Memory"],"MemorySwap":inspection["HostConfig"]["MemorySwap"]}}
                for scenario,mode in PROFILES:
                    profile=f"{scenario}_{mode}"
                    conf=evidence/f"{adapter}-{profile}.conf"
                    settings=config(conf,adapter,output,args.duration_ms,1000,scenario,mode,args.rate)
                    for repetition in range(1,args.repetitions+1):
                        run_id=f"{args.experiment}_{adapter}_{profile}_{repetition}"
                        if len(run_id)>64: raise ValueError("generated run ID exceeds 64 characters")
                        stop=threading.Event(); samples=[]
                        sampler=threading.Thread(target=sample_resources,args=(container,stop,samples),daemon=True)
                        sampler.start()
                        try:
                            capture=subprocess.run([str(executable),"run","--config",str(conf),"--run-id",run_id],capture_output=True,text=True,timeout=180)
                        finally: stop.set();sampler.join(timeout=12)
                        directory=output/run_id
                        if not (directory/"summary.json").exists(): raise RuntimeError(f"capture failed: {capture.stderr}")
                        summary=json.loads((directory/"summary.json").read_text())
                        summary.update(experiment_id=args.experiment,experiment_profile=profile,resource_profile=resource_profile,repetition=repetition,experiment_status="running")
                        write_json(directory/"summary.json",summary)
                        destination=evidence/run_id; destination.mkdir()
                        for filename in ["summary.json","operations.csv"]:
                            shutil.copy2(directory/filename,destination/filename)
                        write_json(destination/"resources.json",{"sampling_scope":"entire worker lifetime including load and warmup","samples":samples,"exit_code":capture.returncode,"configuration":settings})
                        results.append(summary)
                        manifest["runs"].append({"run_id":run_id,"valid":summary["valid"],"summary_sha256":digest(destination/"summary.json"),"resources_sha256":digest(destination/"resources.json")})
                        write_json(evidence/"experiment.json",manifest)
                        write_json(evidence/"aggregate.json",summarize(results))
                        print(f"{run_id}: valid={summary['valid']} operations={summary['total_operations']} cleanup={summary['cleanup_status']}",flush=True)
                        if capture.returncode!=0: raise RuntimeError(f"invalid capture {run_id}; evidence preserved")
                        time.sleep(.5)
            finally:
                try:
                    logs=subprocess.run(["docker","logs",container],capture_output=True,text=True,timeout=15)
                    (evidence/f"{adapter}-server.log").write_text(logs.stdout+logs.stderr,encoding="utf-8")
                    if adapter in ("cassandra","neo4j"):
                        gc_path="/var/log/cassandra/gc.log" if adapter=="cassandra" else "/logs/gc.log"
                        try: (evidence/f"{adapter}-gc.log").write_text(docker("exec",container,"cat",gc_path),encoding="utf-8")
                        except subprocess.SubprocessError: manifest["adapters"].setdefault(adapter,{})["gc_observation"]="No readable default GC log; pauses not isolated."
                finally:
                    # Exact container ID returned here; its anonymous volumes only.
                    docker("rm","-f","-v",container)
        manifest["state"]="completed"
    except BaseException as error:
        manifest["state"]="failed";manifest["error"]=str(error)
        raise
    finally:
        manifest["finished_at_utc"]=datetime.now(timezone.utc).isoformat()
        for record in manifest["runs"]:
            path=evidence/record["run_id"]/"summary.json"
            summary=json.loads(path.read_text(encoding="utf-8"));summary["experiment_status"]=manifest["state"]
            write_json(path,summary);write_json(output/record["run_id"]/"summary.json",summary)
            record["summary_sha256"]=digest(path)
        write_json(evidence/"experiment.json",manifest)
        write_json(evidence/"aggregate.json",summarize(results))
    return evidence

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable",default=str(ROOT/"build"/"benchforge.exe"))
    parser.add_argument("--experiment",default=datetime.now(timezone.utc).strftime("bf_%Y%m%d_%H%M%S"))
    parser.add_argument("--adapters",nargs="+",choices=list(IMAGES),default=list(IMAGES))
    parser.add_argument("--repetitions",type=int,default=3)
    parser.add_argument("--duration-ms",type=int,default=5000)
    parser.add_argument("--rate",type=int,default=100)
    print(run_suite(parser.parse_args()))

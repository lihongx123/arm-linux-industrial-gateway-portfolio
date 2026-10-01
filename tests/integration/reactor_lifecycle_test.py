#!/usr/bin/env python3
"""Short local regression: partial startup cleanup and serial endpoint loss."""
import argparse, json, os, pathlib, pty, socket, subprocess, time
ap=argparse.ArgumentParser()
ap.add_argument("--binary",required=True)
ap.add_argument("--output",type=pathlib.Path,required=True)
args=ap.parse_args()
args.output.mkdir(parents=True,exist_ok=False)
bad=subprocess.run([args.binary,"--rtu-device=/no/such/resume-alignment-serial"],
    stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=10)
(args.output/"startup-failure.log").write_text(bad.stdout)
assert bad.returncode==1, bad.returncode
master,slave=pty.openpty()
with socket.socket() as probe:
    probe.bind(("127.0.0.1",0))
    port=probe.getsockname()[1]
broker_log=(args.output/"broker.log").open("w")
broker=subprocess.Popen(["mosquitto","-p",str(port)],stdout=broker_log,stderr=subprocess.STDOUT)
for attempt in range(100):
    try:
        with socket.create_connection(("127.0.0.1",port),timeout=.1):break
    except OSError:
        time.sleep(.02)
log=(args.output/"disconnect.log").open("w")
proc=subprocess.Popen([args.binary,"--mqtt-port="+str(port),"--rtu-device="+os.ttyname(slave),
    "--metrics-file="+str(args.output/"metrics.json")],stdout=log,stderr=subprocess.STDOUT)
try:
    deadline=time.monotonic()+5
    while "SOUTHBOUND" not in (args.output/"disconnect.log").read_text():
        assert proc.poll() is None
        if time.monotonic()>deadline: raise TimeoutError("reactor startup")
        time.sleep(.02)
    os.close(master);master=-1
    code=proc.wait(timeout=10)
    assert code==1, code
    assert "southbound reactor failed:" in (args.output/"disconnect.log").read_text()
    result={"startup_failure_exit":bad.returncode,"serial_disconnect_exit":code,
            "aborted_by_signal":code<0,"result":"PASS"}
    (args.output/"summary.json").write_text(json.dumps(result,indent=2))
    print(json.dumps(result))
finally:
    if proc.poll() is None: proc.terminate();proc.wait(timeout=10)
    broker.terminate();broker.wait(timeout=5);broker_log.close()
    if master>=0:os.close(master)
    os.close(slave);log.close()

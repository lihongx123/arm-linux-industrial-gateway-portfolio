#!/usr/bin/env python3
"""One real gateway process: PTY RTU transactions + SocketCAN + independent MQTT observer."""
import argparse, collections, json, os, pathlib, pty, select, socket, struct, subprocess, tempfile, threading, time
import paho.mqtt.client as mqtt
def crc(data):
    value=0xffff
    for byte in data:
        value ^= byte
        for _ in range(8): value=(value>>1)^0xa001 if value&1 else value>>1
    return struct.pack("<H",value)
def frame(data): return data+crc(data)
def free_port():
    with socket.socket() as s: s.bind(("127.0.0.1",0)); return s.getsockname()[1]
def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--binary",required=True);ap.add_argument("--output",type=pathlib.Path,required=True)
    ap.add_argument("--duration",type=int,default=30);ap.add_argument("--can-rate",type=int,default=100)
    ap.add_argument("--interface",default="vcan0");ap.add_argument("--inject-faults",action="store_true")
    args=ap.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    port=free_port();master,slave=pty.openpty();path=os.ttyname(slave);os.set_blocking(master,False)
    stopped=threading.Event();lock=threading.Lock();error=[]
    rt_sent={};can_sent={};rt_seen=collections.Counter();can_seen=collections.Counter()
    progress=collections.defaultdict(lambda:[0,0]);lat=[];statuses=[];writes=[];samples=[];requests=0
    can_command=threading.Event();subscribed=threading.Event();broker=gateway=None
    observer=mqtt.Client(client_id="mixed-observer-"+str(os.getpid()),clean_session=True)
    start=time.monotonic()
    def on_connect(c,u,f,rc):
        if rc==0:c.subscribe("device/#",1)
    def on_subscribe(*_):subscribed.set()
    def on_message(c,u,m):
        try:
            data=json.loads(m.payload)
            now=time.monotonic()
            if m.topic.endswith("/telemetry"):
                raw=bytes.fromhex(data["payload"])
                with lock:
                    if data["protocol"]=="modbus_rtu":
                        seq=int.from_bytes(raw[3:5],"big");rt_seen[seq]+=1;progress[int((now-start)//5)][0]+=1
                        if seq in rt_sent:lat.append((now-rt_sent[seq])*1000)
                    elif data["protocol"]=="can" and data["address"]==0x123:
                        seq=struct.unpack("!Q",raw)[0];can_seen[seq]+=1;progress[int((now-start)//5)][1]+=1
                        if seq in can_sent:lat.append((now-can_sent[seq])*1000)
            elif m.topic.endswith("/status"):
                with lock:statuses.append((m.topic,data))
        except Exception as e:error.append(str(e))
    observer.on_connect=on_connect;observer.on_subscribe=on_subscribe;observer.on_message=on_message
    def serial_slave():
        nonlocal requests
        buffer=b"";seq=0
        try:
            while not stopped.is_set():
                if not select.select([master],[],[],.05)[0]:continue
                try:chunk=os.read(master,512)
                except BlockingIOError:continue
                buffer+=chunk
                while len(buffer)>=8:
                    request=buffer[:8];buffer=buffer[8:]
                    if crc(request[:-2])!=request[-2:]:raise RuntimeError("gateway sent bad RTU CRC")
                    if request[1]==3:
                        requests+=1
                        if args.inject_faults and requests==30:continue
                        seq+=1;response=frame(bytes([1,3,2])+seq.to_bytes(2,"big"))
                        with lock:rt_sent[seq]=time.monotonic()
                        if args.inject_faults and requests==10:
                            bad=response[:-1]+bytes([response[-1]^0xff])
                            os.write(master,b"\xfe\x00\xaa"+bad+response[:3]);time.sleep(.005);os.write(master,response[3:])
                        else:os.write(master,response)
                    elif request[1]==6:
                        writes.append(request.hex());os.write(master,request)
                    else:raise RuntimeError("unexpected function")
        except Exception as e:error.append("serial: "+str(e))
    def resource():
        ticks=os.sysconf("SC_CLK_TCK")
        prev=None
        while not stopped.wait(.25):
            try:
                fields=pathlib.Path(f"/proc/{gateway.pid}/stat").read_text().split()
                cpu=int(fields[13])+int(fields[14]);now=time.monotonic()
                status=pathlib.Path(f"/proc/{gateway.pid}/status").read_text().splitlines()
                rss=int(next(x for x in status if x.startswith("VmRSS:")).split()[1])
                samples.append({"time":now-start,"rss_kib":rss,"cpu_percent":0 if prev is None else (cpu-prev[1])/ticks/(now-prev[0])*100})
                prev=(now,cpu)
            except (FileNotFoundError,StopIteration):return
    threads=[];files=[]
    try:
        broker_log=(args.output/"broker.log").open("w");gateway_log=(args.output/"gateway.log").open("w");files=[broker_log,gateway_log]
        config=args.output/"broker.conf";config.write_text(f"listener {port} 127.0.0.1\nallow_anonymous true\npersistence false\n")
        broker=subprocess.Popen(["mosquitto","-c",str(config)],stdout=broker_log,stderr=subprocess.STDOUT)
        for _ in range(100):
            try:observer.connect("127.0.0.1",port,10);break
            except OSError:time.sleep(.05)
        observer.loop_start()
        if not subscribed.wait(5):raise RuntimeError("observer subscription timeout")
        command=[args.binary,f"--mqtt-port={port}",f"--can-interface={args.interface}",f"--rtu-device={path}",
            "--rtu-baud=115200","--rtu-poll-ms=100","--rtu-slave=1","--workers=2","--queue-capacity=1024",
            "--pipeline-metrics=1",f"--metrics-file={args.output/'gateway-metrics.json'}"]
        (args.output/"run-parameters.json").write_text(json.dumps({"command":command,"duration":args.duration,"can_rate":args.can_rate,"rtu_poll_ms":100,"fault_injection":args.inject_faults},indent=2))
        gateway=subprocess.Popen(command,stdout=gateway_log,stderr=subprocess.STDOUT)
        (args.output/"pid.txt").write_text(str(gateway.pid))
        threads=[threading.Thread(target=serial_slave),threading.Thread(target=resource)]
        for t in threads:t.start()
        with socket.socket(socket.AF_CAN,socket.SOCK_RAW,socket.CAN_RAW) as tx, socket.socket(socket.AF_CAN,socket.SOCK_RAW,socket.CAN_RAW) as rx:
            tx.bind((args.interface,));rx.bind((args.interface,));rx.setblocking(False)
            time.sleep(.3);start=time.monotonic();count=args.duration*args.can_rate;sent_command=False
            for seq in range(1,count+1):
                due=start+(seq-1)/args.can_rate
                if due>time.monotonic():time.sleep(due-time.monotonic())
                with lock:can_sent[seq]=time.monotonic()
                tx.send(struct.pack("=IB3x8s",0x123,8,struct.pack("!Q",seq)))
                if not sent_command and time.monotonic()-start>args.duration/2:
                    observer.publish("device/rtu-1/cmd/modbus_write",json.dumps({"slave":1,"register":7,"value":1234}),qos=1)
                    observer.publish("device/can-1792/cmd/can_tx",json.dumps({"can_id":0x700,"data":"deadbeef"}),qos=1);sent_command=True
                while True:
                    try:
                        ident,length,payload=struct.unpack("=IB3x8s",rx.recv(16))
                        if ident==0x700 and payload[:length]==bytes.fromhex("deadbeef"):can_command.set()
                    except BlockingIOError:break
            time.sleep(.5)
            gateway.terminate();gateway.wait(timeout=10);time.sleep(.5)
        stopped.set()
        for t in threads:t.join(timeout=2)
        observer.loop_stop();observer.disconnect()
        metrics=json.loads((args.output/"gateway-metrics.json").read_text())
        with lock:
            rmissing=set(rt_sent)-set(rt_seen);cmissing=set(can_sent)-set(can_seen)
            duplicates=sum(v-1 for v in list(rt_seen.values())+list(can_seen.values()) if v>1)
            lat.sort()
            percentile=lambda p:lat[min(len(lat)-1,int((len(lat)-1)*p))] if lat else None
            result={"gateway_pid":gateway.pid,"single_process":True,"duration_seconds":args.duration,
                "expected_modbus":len(rt_sent),"received_modbus_unique":len(rt_seen),"missing_modbus":len(rmissing),
                "expected_can":len(can_sent),"received_can_unique":len(can_seen),"missing_can":len(cmissing),"duplicates":duplicates,
                "rtu_command_confirmed":bool(writes) and any(t=="device/rtu-1/status" and d.get("status")=="ok" for t,d in statuses),
                "can_command_confirmed":can_command.is_set() and any(t=="device/can-1792/status" and d.get("status")=="ok" for t,d in statuses),
                "progress_5s":dict(progress),"no_protocol_starvation":all(all(progress[i]) for i in range(args.duration//5)),
                "p50_ms":percentile(.5),"p95_ms":percentile(.95),"p99_ms":percentile(.99),
                "latency_definition":"PTY response write / CAN send initiation -> independent MQTT callback; combined samples",
                "cpu_avg_percent_one_core":sum(x["cpu_percent"] for x in samples[1:])/max(1,len(samples)-1),
                "peak_rss_kib":max((x["rss_kib"] for x in samples),default=0),"metrics":metrics,"errors":error,"gateway_exit":gateway.returncode}
            checks=[not rmissing,not cmissing,not duplicates,len(rt_sent)>args.duration*5,result["rtu_command_confirmed"],
                result["can_command_confirmed"],result["no_protocol_starvation"],not error,metrics["rejected"]==0,metrics["publish_failures"]==0,gateway.returncode==0]
            if not args.inject_faults:checks += [metrics["rtu"]["timeouts"]==0,metrics["rtu"]["crc_candidates_rejected"]==0]
            else:checks += [metrics["rtu"]["timeouts"]>=1,metrics["rtu"]["writes_confirmed"]>=1,
                           metrics["rtu"]["crc_candidates_rejected"]>0,metrics["rtu"]["bytes_discarded"]>0]
            result["result"]="PASS" if all(checks) else "FAIL"
        (args.output/"resource-samples.json").write_text(json.dumps(samples))
        (args.output/"summary.json").write_text(json.dumps(result,indent=2))
        print(json.dumps(result,indent=2));return 0 if result["result"]=="PASS" else 1
    finally:
        stopped.set()
        for t in threads:t.join(timeout=2)
        observer.loop_stop()
        for proc in (gateway,broker):
            if proc and proc.poll() is None:proc.terminate();proc.wait(timeout=10)
        os.close(master);os.close(slave)
        for f in files:f.close()
if __name__=="__main__":raise SystemExit(main())

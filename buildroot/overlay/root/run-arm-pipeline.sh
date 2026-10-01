#!/bin/sh
set -eu
duration="${1:-30}"
rates="${2:-1000,1500,2000}"
variants="${3:-1:0:20,1:1:20,0:1:20}"
broker_inflight="${4:-20}"
workers="${5:-2}"
queue="${6:-1024}"
devices="${7:-100}"
sample_processes="${8:-0}"
result_dir=/root/pipeline-results
mkdir -p "$result_dir"
cleanup() {
    if [ -n "${sampler_pid:-}" ]; then kill "$sampler_pid" 2>/dev/null || true; fi
    if [ -n "${probe_pid:-}" ]; then kill "$probe_pid" 2>/dev/null || true; fi
    if [ -n "${gateway_pid:-}" ]; then kill "$gateway_pid" 2>/dev/null || true; fi
    if [ -n "${broker_pid:-}" ]; then kill "$broker_pid" 2>/dev/null || true; fi
}
trap cleanup EXIT INT TERM
ip link show vcan0 >/dev/null 2>&1 || ip link add dev vcan0 type vcan
ip link set up vcan0
killall mosquitto 2>/dev/null || true
sleep 1
printf 'rate,qos,observability,inflight,probe_exit\n' > "$result_dir/index.csv"
for rate in $(echo "$rates" | tr ',' ' '); do
    # Same gateway binary; observer overhead is measured at QoS 1.
    for variant in $(echo "$variants" | tr ',' ' '); do
        qos="${variant%%:*}"
        suffix="${variant#*:}"
        observe="${suffix%%:*}"
        inflight="${suffix#*:}"
        case_dir="$result_dir/rate-$rate-qos-$qos-observe-$observe-window-$inflight"
        mkdir "$case_dir"
        rm -f /tmp/pipeline-broker.log
        sed '/^log_dest /d; /^max_inflight_messages /d' /root/mosquitto.conf > "$case_dir/broker.conf"
        printf 'log_dest file /tmp/pipeline-broker.log\nmax_inflight_messages %s\n' "$broker_inflight" >> "$case_dir/broker.conf"
        mosquitto -c "$case_dir/broker.conf" >"$case_dir/broker-stdout.log" 2>&1 & broker_pid=$!
        sleep 1
        kill -0 "$broker_pid"
        metrics="$case_dir/gateway-metrics.json"
        client="pipeline-$rate-$qos-$observe"
        mqmgateway_iot --mqtt-host=127.0.0.1 --mqtt-port=1883 --client-id="$client" \
            --can-interface=vcan0 --queue-capacity="$queue" --workers="$workers" --heartbeat-ms=1000 \
            --telemetry-qos="$qos" --pipeline-metrics="$observe" \
            --mqtt-inflight="$inflight" \
            --metrics-file="$metrics" >"$case_dir/gateway.log" 2>&1 & gateway_pid=$!
        mosquitto_sub -h 127.0.0.1 -W 15 -C 1 -t "gateway/$client/status" >"$case_dir/online.json"
        sleep 1
        printf 'duration_seconds=%s\nrate=%s\ndevices=%s\nqueue=%s\nworkers=%s\nqos=%s\nobserve=%s\ninflight=%s\n' \
            "$duration" "$rate" "$devices" "$queue" "$workers" "$qos" "$observe" "$inflight" > "$case_dir/parameters.txt"
        printf 'broker_inflight=%s\n' "$broker_inflight" >> "$case_dir/parameters.txt"
        set +e
        mqmgateway_arm64_stress --rate="$rate" --duration="$duration" --devices="$devices" \
            --gateway-pid="$gateway_pid" --metrics-file="$metrics" --output="$case_dir/result.json" \
            >"$case_dir/probe.log" 2>&1 & probe_pid=$!
        if [ "$sample_processes" = 1 ]; then
            sh /root/sample-pipeline-processes.sh "$gateway_pid" "$broker_pid" "$probe_pid" "$metrics" \
                >"$case_dir/process-samples.txt" 2>"$case_dir/sampler-errors.log" & sampler_pid=$!
        fi
        wait "$probe_pid"
        probe_exit=$?
        set -e
        probe_pid=
        if [ -n "${sampler_pid:-}" ]; then
            kill "$sampler_pid" 2>/dev/null || true
            wait "$sampler_pid" || true
            sampler_pid=
        fi
        cp "$metrics" "$case_dir/metrics-before-stop.json"
        kill -TERM "$gateway_pid"
        wait "$gateway_pid" || true
        gateway_pid=
        kill -TERM "$broker_pid"
        wait "$broker_pid" || true
        broker_pid=
        cp /tmp/pipeline-broker.log "$case_dir/broker.log" 2>/dev/null || true
        printf '%s,%s,%s,%s,%s\n' "$rate" "$qos" "$observe" "$inflight" "$probe_exit" >> "$result_dir/index.csv"
        echo "case rate=$rate qos=$qos observe=$observe window=$inflight exit=$probe_exit"
        if [ "$probe_exit" -ne 0 ] && [ "$probe_exit" -ne 2 ] && [ "$probe_exit" -ne 3 ]; then exit 4; fi
    done
done
# Zero means the matrix completed; individual cases retain STABLE/DEGRADED/FAIL.

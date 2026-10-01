#!/usr/bin/env python3
"""Sequential configuration-only scalability experiment; no gateway recompilation."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import time

PROFILES = {
    'base': {},
    'workers1': {'workers': 1}, 'workers4': {'workers': 4}, 'workers8': {'workers': 8},
    'cpu4': {'vcpus': 4}, 'cpu4-workers4': {'vcpus': 4, 'workers': 4},
    'cpu8': {'vcpus': 8},
    'queue256': {'queue': 256}, 'queue4096': {'queue': 4096},
    'devices10': {'devices': 10}, 'devices1000': {'devices': 1000},
    'memory512': {'memory-mb': 512}, 'memory2048': {'memory-mb': 2048},
}


def process_summary(path):
    if not path.exists():
        return {'error': 'no process samples'}
    roles, records, threads, rss = {}, {}, {}, {}
    timestamp, current, hz = None, None, 100
    for line in path.read_text().splitlines():
        if line.startswith('PIDS '):
            roles = {int(pid): name for name, pid in re.findall(r'(gateway|broker|probe)=(\d+)', line)}
        elif line.startswith('CLK_TCK='):
            hz = int(line.split()[0].split('=')[1])
        elif line.startswith('SAMPLE '):
            timestamp = float(line.split()[1])
        elif line.startswith('PROCESS '):
            current = int(line.split()[1])
        elif line.startswith(('VmRSS:', 'VmHWM:')):
            rss[current] = max(rss.get(current, 0), int(line.split()[1]))
        elif re.match(r'^(THREAD )?\d+ \(', line):
            is_thread = line.startswith('THREAD ')
            raw = line[7:] if is_thread else line
            pid = int(raw.split(' ', 1)[0])
            fields = raw.rsplit(') ', 1)[1].split()
            sample = (timestamp, int(fields[11]) + int(fields[12]))
            target = threads if is_thread else records
            key = (current, pid) if is_thread else pid
            target.setdefault(key, []).append(sample)
    def cpu(samples):
        start, end = samples[0], samples[-1]
        duration = end[0] - start[0]
        return {'cpu_percent': 100 * (end[1] - start[1]) / hz / duration if duration else None,
                'sample_span_seconds': duration, 'samples': len(samples)}
    result = {}
    for pid, name in roles.items():
        if pid not in records:
            result[name] = {'error': 'process not sampled'}
            continue
        result[name] = {**cpu(records[pid]), 'pid': pid, 'sampled_hwm_kib': rss.get(pid),
                       'threads': [{'tid': tid, **cpu(samples)} for (parent, tid), samples in threads.items() if parent == pid]}
    return result


def collect(directory):
    rows = []
    for manifest_path in sorted(directory.glob('*/manifest.json')):
        run = manifest_path.parent
        manifest = json.loads(manifest_path.read_text())
        for result_path in sorted(run.glob('guest-results/pipeline-results/*/result.json')):
            case = result_path.parent
            result = json.loads(result_path.read_text())
            metrics = json.loads((case / 'metrics-before-stop.json').read_text())
            processes = process_summary(case / 'process-samples.txt')
            row = {'profile': run.name, 'result_path': str(result_path.relative_to(directory)),
                   'config': {key: manifest.get(key) for key in ['workers', 'queue', 'devices', 'qemu_vcpus', 'qemu_memory_mb', 'variants']},
                   'result': result, 'gateway_metrics': metrics, 'processes': processes}
            broker_log = case / 'broker.log'
            row['broker_drop_log_lines'] = sum('Outgoing messages are being dropped' in line for line in broker_log.read_text().splitlines()) if broker_log.exists() else None
            rows.append(row)
    (directory / 'summary.json').write_text(json.dumps(rows, indent=2))
    lines = ['# Configuration-only ARM64 scalability results', '',
             'QoS1, same gateway/probe binaries. CPU is guest process CPU, one core = 100%.',
             'Loss is delivery-count deficit after at most 5 s drain, without sequence deduplication.', '',
             '| Profile | Target | Actual send/s | Load receive/s | Final lost | P99 ms | Gateway CPU% | Broker CPU%* | Probe CPU%* | RSS MiB | Queue | Class |',
             '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |']
    for row in rows:
        r, ps = row['result'], row['processes']
        def sampled(name):
            value = ps.get(name, {}).get('cpu_percent')
            return f'{value:.2f}' if value is not None else 'N/A'
        lines.append(f"| {row['profile']} | {r['target_rate']:.0f} | {r['actual_send_rate']:.2f} | {r['receive_rate_during_load']:.2f} | {r['lost']} | {r['p99_ms']:.1f} | {r['cpu_percent']:.2f} | {sampled('broker')} | {sampled('probe')} | {r['peak_rss_mb']:.2f} | {r['queue_peak']} | {r['classification']} |")
    lines.extend(['', '*Broker/probe CPU use the recorded 5-second sampling span; gateway CPU column uses the original probe load+drain interval. See JSON for comparable sampled gateway CPU and spans.',
                  'STABLE is the original probe threshold (loss <= 0.1%, receive/target >= 95%, P99 <= 100 ms, no malformed/rejected); it is not a zero-loss or long-duration certificate.',
                  'Sample overhead is present in all cases. Profile cpu4-workers4 is a two-factor interaction, not a single-factor attribution.'])
    (directory / 'summary.md').write_text('\n'.join(lines) + '\n')
    return rows


def source_hashes(repo):
    return {str(p.relative_to(repo)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted((repo / 'src').rglob('*')) if p.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gateway', required=True)
    parser.add_argument('--results', type=pathlib.Path, required=True)
    parser.add_argument('--profiles', default=','.join(PROFILES))
    parser.add_argument('--seconds', type=int, default=60)
    parser.add_argument('--rates', default='4000')
    args = parser.parse_args()
    names = args.profiles.split(',')
    if len(set(names)) != len(names) or any(name not in PROFILES for name in names):
        parser.error('unique known profile names required')
    repo = pathlib.Path(__file__).resolve().parents[1]
    args.results.mkdir(parents=True, exist_ok=False)
    before = source_hashes(repo)
    plan = {'started_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
            'seconds_per_case': args.seconds, 'rates': args.rates,
            'defaults': {'workers': 2, 'queue': 1024, 'devices': 100, 'vcpus': 2, 'memory-mb': 1024},
            'profiles': {name: PROFILES[name] for name in names}, 'source_before': before}
    (args.results / 'plan.json').write_text(json.dumps(plan, indent=2))
    for name in names:
        config = {**plan['defaults'], **PROFILES[name]}
        command = [sys.executable, str(repo / 'scripts/run_pipeline_experiment.py'), '--gateway', args.gateway,
                   '--results', str(args.results / name), '--seconds', str(args.seconds), '--rates', args.rates,
                   '--variants', '1:1:20', '--sample-processes']
        for key, value in config.items():
            command.extend(['--' + key, str(value)])
        print('START', name, config, flush=True)
        with (args.results / (name + '-host.log')).open('w') as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
        rows = collect(args.results)
        for row in rows:
            if row['profile'] == name:
                r = row['result']
                print('RESULT', name, r['target_rate'], r['classification'], 'rx/s', r['receive_rate_during_load'], 'lost', r['lost'], 'p99_ms', r['p99_ms'], flush=True)
        if result.returncode:
            raise RuntimeError('Infrastructure/evidence error in ' + name + '; inspect preserved host log')
    unchanged = before == source_hashes(repo)
    (args.results / 'completion.json').write_text(json.dumps({'status': 'COMPLETED', 'source_unchanged': unchanged,
        'completed_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}, indent=2))
    if not unchanged:
        raise RuntimeError('Gateway source changed during suite')


if __name__ == '__main__':
    main()

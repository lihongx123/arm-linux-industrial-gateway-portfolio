#!/usr/bin/env python3
"""Summarize saved diagnostic evidence without rewriting raw measurements."""
import argparse
import json
import pathlib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=pathlib.Path)
    args = parser.parse_args()
    cases = []
    for path in sorted((args.run / "guest-results/pipeline-results").glob("*/result.json")):
        result = json.loads(path.read_text())
        metrics = json.loads((path.parent / "metrics-before-stop.json").read_text())
        mqtt = metrics.get("mqtt_pipeline", {})
        cases.append({"case": path.parent.name, "classification": result["classification"],
                      "rate": result["target_rate"], "receive_rate": result["receive_rate_during_load"],
                      "lost": result["lost"], "loss_rate": result["loss_rate"], "p99_ms": result["p99_ms"],
                      "cpu_percent": result["cpu_percent"], "peak_rss_mb": result["peak_rss_mb"],
                      "queue_peak": metrics["peak_depth"], "mqtt": mqtt,
                      "can": metrics.get("can_pipeline", {}),
                      "queue_wait": metrics.get("telemetry_queue_wait", {})})
    if not cases: raise RuntimeError("No completed case results")
    (args.run / "summary.json").write_text(json.dumps(cases, indent=2))
    lines = ["# Diagnostic matrix (read raw case JSON for scope)", "",
             "| Case | Class | Receive/s | Lost | P99 ms | CPU % | RSS MiB | Queue peak | Pending peak | ACK P99 upper us |",
             "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for c in cases:
        m = c["mqtt"]
        lines.append(f'| {c["case"]} | {c["classification"]} | {c["receive_rate"]:.2f} | {c["lost"]} | {c["p99_ms"]:.3f} | {c["cpu_percent"]:.2f} | {c["peak_rss_mb"]:.2f} | {c["queue_peak"]} | {m.get("pending_peak", "N/A")} | {m.get("telemetry_puback", {}).get("p99_upper_us", "N/A")} |')
    lines += ["", "ACK histogram quantiles are bucket upper bounds. QoS0 has no PUBACK.",
              "pending_tracked is not wire inflight. Incomplete tracking cannot establish exact conservation.",
              "Original probe counts deliveries without sequence deduplication; observed loss is not a guaranteed unique-message loss measure."]
    (args.run / "summary.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()

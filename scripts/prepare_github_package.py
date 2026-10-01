#!/usr/bin/env python3
"""Export an auditable source/evidence snapshot; never publish or change Git remotes."""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import zipfile

DENY_DIRS = {".git", ".claude", ".vscode", "__pycache__", ".cache", "node_modules"}
DENY_SUFFIXES = {".ext4", ".img", ".qcow2", ".key", ".p12", ".pfx", ".pyc", ".o", ".a", ".so"}
SECRET = re.compile(rb"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----|(?:gh[pousr]_[A-Za-z0-9]{30,})|(?:AKIA[0-9A-Z]{16})")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=pathlib.Path, required=True)
    args = parser.parse_args()
    repo = pathlib.Path(__file__).resolve().parents[1]
    target = args.destination.resolve()
    if target == repo or repo in target.parents or target.exists():
        parser.error("destination must be a new directory outside the source repository")
    names = subprocess.check_output(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"], cwd=repo)
    include, excluded, findings = [], [], []
    for name in sorted(set(names.decode().split("\0")) - {""}):
        relative = pathlib.PurePosixPath(name)
        source = repo / relative
        reason = None
        if any(p in DENY_DIRS for p in relative.parts): reason = "local configuration/cache"
        if source.suffix.lower() in DENY_SUFFIXES: reason = "binary/private artifact"
        if source.name.startswith(".env") and source.name != ".env.example": reason = "private environment"
        if name.startswith(".github/") and name != ".github/workflows/extension-ci.yml":
            reason = "upstream automation excluded from extension publication"
        if source.is_symlink(): reason = "symlink requires manual review"
        if not source.is_file(): continue
        if source.stat().st_size > 25 * 1024 * 1024: reason = "larger than 25 MiB; keep raw artifact locally"
        if reason:
            excluded.append({"path": name, "reason": reason})
            continue
        content = source.read_bytes()
        if SECRET.search(content): findings.append(name)
        include.append((name, content))
    if findings:
        raise RuntimeError("Potential credentials; inspect locally before exporting: " + ", ".join(findings))
    target.mkdir(parents=True)
    manifest = []
    for name, content in include:
        destination = target / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(content)
        manifest.append({"path": name, "bytes": len(content), "sha256": hashlib.sha256(content).hexdigest()})
    # Disk artifacts ignored by Git are recorded too, without copying them.
    for source in (repo / "results").rglob("*"):
        if source.is_file() and source.suffix.lower() in DENY_SUFFIXES:
            entry = {"path": source.relative_to(repo).as_posix(), "reason": "ignored runtime artifact retained locally"}
            if entry not in excluded: excluded.append(entry)
    report = {"upstream": "https://github.com/BlackZork/mqmgateway",
              "baseline_commit": "b6f24d3b2c4596a19082e584326df70adffedbb6",
              "file_count": len(manifest), "total_bytes": sum(f["bytes"] for f in manifest),
              "files": manifest, "excluded": excluded,
              "secret_scan": "No high-confidence token/private-key pattern found; not an exhaustive secret audit",
              "git_history_included": False, "remote_published": False}
    (target / "PACKAGE_MANIFEST.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    archive = target.with_suffix(".zip")
    if archive.exists(): raise RuntimeError("archive already exists")
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED) as output:
        for source in sorted(target.rglob("*")):
            if source.is_file(): output.write(source, pathlib.Path(target.name) / source.relative_to(target))
    with zipfile.ZipFile(archive) as check:
        if check.testzip() is not None: raise RuntimeError("archive integrity failed")
    print(json.dumps({"directory": str(target), "zip": str(archive), "files": len(manifest),
                      "bytes": report["total_bytes"], "zip_bytes": archive.stat().st_size}, indent=2))


if __name__ == "__main__":
    main()

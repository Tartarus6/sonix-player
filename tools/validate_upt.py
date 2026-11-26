#!/usr/bin/env python3
"""Independent, read-only validator for Sonix Player .upt firmware packages.

This tool does NOT trust the build script. It opens the .upt as an ISO 9660
container, reads the OTA metadata, verifies the per-chunk MD5 chain against the
manifest, reassembles each component and checks its whole-file MD5, then reads
the device identity from the rootfs. It is deliberately conservative: any
missing check is reported as FAIL or NOT APPLICABLE, never as PASS.

Usage:
    python3 tools/validate_upt.py IMAGE.upt --expect-device r1
    python3 tools/validate_upt.py IMAGE.upt --expect-device r3proii
    python3 tools/validate_upt.py IMAGE.upt --expect-device v1

Exit status: 0 only when every mandatory check passes. The JSON report is
written to stdout (or to --report).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

CHUNK_SIZE = 524288
DEVICE_NAMES = {
    "r1": "HiBy R1",
    "r3proii": "HiBy R3 Pro II",
    "v1": "TempoTec V1",
}
REQUIRED_META = (
    "ota_config.in",
    "ota_v0/ota_update.in",
    "ota_v0/ota_compatibility.in",
    "ota_v0/ota_v0.ok",
)


class Report:
    def __init__(self, image: str, expect: str | None):
        self.data = {
            "image": image,
            "expect_device": expect,
            "checks": {},
            "observed_device": None,
            "compatibility": None,
            "components": [],
            "result": "PENDING",
        }

    def set(self, name: str, status: str, evidence: str) -> None:
        self.data["checks"][name] = {"status": status, "evidence": evidence}

    def fail(self, name: str, evidence: str) -> None:
        self.set(name, "FAIL", evidence)

    def finalize(self) -> str:
        statuses = [c["status"] for c in self.data["checks"].values()]
        if "FAIL" in statuses:
            self.data["result"] = "REJECTED"
        elif any(s in ("NOT TESTED", "PENDING", "INCOMPLETE") for s in statuses):
            self.data["result"] = "INCOMPLETE"
        elif statuses and all(s in ("PASS", "NOT APPLICABLE") for s in statuses):
            # NOT APPLICABLE is neutral: e.g. no --expect-device means the
            # wrong-device check cannot be performed, not that it failed.
            self.data["result"] = "STRUCTURAL PASS (flashability NOT established)"
        else:
            self.data["result"] = "INCOMPLETE"
        return self.data["result"]


def run(cmd: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def iso_list(path: Path) -> tuple[dict[str, int], str | None]:
    cp = run(["7z", "l", "-slt", str(path)])
    text = cp.stdout.decode("utf-8", "replace") + cp.stderr.decode("utf-8", "replace")
    if cp.returncode != 0:
        return {}, text
    files: dict[str, int] = {}
    cur_path = None
    for line in text.splitlines():
        if line.startswith("Path = "):
            cur_path = line[7:].strip()
        elif line.startswith("Size = ") and cur_path:
            try:
                files[cur_path] = int(line[7:].strip())
            except ValueError:
                pass
            cur_path = None
    return files, None


def iso_read(path: Path, member: str) -> bytes | None:
    cp = run(["7z", "x", "-so", str(path), member])
    return cp.stdout if cp.returncode == 0 else None


def parse_metadata(text: bytes) -> dict[str, dict[str, str]]:
    blocks: dict[str, dict[str, str]] = {}
    cur = None
    for raw in text.decode("utf-8", "replace").splitlines():
        line = raw.strip()
        if not line or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key, value = key.strip(), value.strip()
        if key == "img_type":
            cur = value
            blocks[cur] = {}
        elif cur and key.startswith("img_"):
            blocks[cur][key] = value
    return blocks


def verify_component(path: Path, name: str, meta: dict[str, str], files: dict[str, int],
                     report: Report) -> bytes | None:
    """Verify one component's chunk chain and reassemble it. Returns bytes or None."""
    try:
        expected_size = int(meta["img_size"])
    except (KeyError, ValueError):
        report.fail(f"{name}_metadata", "img_size missing or invalid")
        return None
    expected_md5 = meta.get("img_md5", "").lower()
    if not re.fullmatch(r"[0-9a-f]{32}", expected_md5):
        report.fail(f"{name}_metadata", "img_md5 missing or invalid")
        return None
    # The chunk prefix is img_name (e.g. "rootfs.squashfs"), not img_type.
    chunk_name = meta.get("img_name", name)

    pattern = re.compile(rf"^ota_v0/{re.escape(chunk_name)}\.(\d{{4}})\.([0-9a-f]{{32}})$")
    chunks = []
    for member in files:
        m = pattern.match(member)
        if m:
            chunks.append((int(m.group(1)), member, m.group(2)))
    chunks.sort()
    n_expected = (expected_size + CHUNK_SIZE - 1) // CHUNK_SIZE
    if len(chunks) != n_expected:
        report.fail(f"{name}_chunk_count",
                    f"expected {n_expected} chunks, found {len(chunks)}")
        return None
    if [c[0] for c in chunks] != list(range(n_expected)):
        report.fail(f"{name}_chunk_order", "chunk indices are not contiguous from 0000")
        return None

    # The first filename suffix is the whole-file MD5; each later suffix is the
    # MD5 of the PREVIOUS chunk's content (the chain the recovery follows).
    if chunks[0][2] != expected_md5:
        report.fail(f"{name}_chain_head",
                    f"first chunk suffix {chunks[0][2]} != declared md5 {expected_md5}")
        return None

    manifest_name = f"ota_v0/ota_md5_{chunk_name}.{expected_md5}"
    manifest = iso_read(path, manifest_name)
    if manifest is None:
        report.fail(f"{name}_manifest", f"missing {manifest_name}")
        return None
    manifest_md5s = re.findall(rb"[0-9a-f]{32}", manifest)
    manifest_md5s = [m.decode() for m in manifest_md5s]

    assembled = bytearray()
    actual_chunk_md5s = []
    prev_md5 = expected_md5
    chain_ok = True
    for idx, member, suffix in chunks:
        data = iso_read(path, member)
        if data is None:
            chain_ok = False
            break
        digest = hashlib.md5(data).hexdigest()
        actual_chunk_md5s.append(digest)
        if suffix != prev_md5:
            chain_ok = False
        prev_md5 = digest
        assembled.extend(data)
    if not chain_ok:
        report.fail(f"{name}_chain", "a chunk suffix did not match the previous chunk MD5")
        return None
    if len(assembled) != expected_size:
        report.fail(f"{name}_size", f"reassembled {len(assembled)} != declared {expected_size}")
        return None
    whole_md5 = hashlib.md5(assembled).hexdigest()
    if whole_md5 != expected_md5:
        report.fail(f"{name}_md5", f"reassembled md5 {whole_md5} != declared {expected_md5}")
        return None
    if actual_chunk_md5s != manifest_md5s:
        report.fail(f"{name}_manifest_match", "manifest MD5s differ from computed chunk MD5s")
        return None

    report.set(f"{name}_integrity", "PASS",
               f"{n_expected} chunks, size {expected_size}, md5 {whole_md5}, chain and manifest verified")
    report.data["components"].append(
        {"name": name, "size": expected_size, "md5": whole_md5, "chunks": n_expected})
    return bytes(assembled)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image", type=Path)
    ap.add_argument("--expect-device", choices=sorted(DEVICE_NAMES), default=None)
    ap.add_argument("--report", type=Path, default=None)
    args = ap.parse_args()

    rep = Report(str(args.image), args.expect_device)

    def emit() -> None:
        rep.finalize()
        payload = json.dumps(rep.data, indent=2, ensure_ascii=False)
        if args.report:
            args.report.write_text(payload + "\n", encoding="utf-8")
        print(payload)

    missing_tools = [t for t in ("7z", "unsquashfs") if shutil.which(t) is None]
    if missing_tools:
        rep.fail("toolchain", f"missing required tools: {', '.join(missing_tools)}")
        emit()
        return 2

    path = args.image
    if not path.is_file():
        rep.fail("file_exists", "path is not a regular file")
        emit()
        return 2
    size = path.stat().st_size
    rep.set("file_exists", "PASS", f"{size} bytes")

    with path.open("rb") as fh:
        fh.seek(0x8001)
        magic = fh.read(5)
    if magic != b"CD001":
        rep.fail("iso_magic", f"no CD001 at 0x8001 (got {magic!r}); not an ISO 9660 image")
        emit()
        return 2
    rep.set("iso_magic", "PASS", "CD001 at 0x8001")

    files, err = iso_list(path)
    if err:
        rep.fail("iso_readable", err.strip()[:400])
        emit()
        return 2
    rep.set("iso_readable", "PASS", f"{len(files)} ISO members")
    for member in REQUIRED_META:
        if member not in files:
            rep.fail("required_metadata", f"missing {member}")
    if "required_metadata" not in rep.data["checks"]:
        rep.set("required_metadata", "PASS", ", ".join(REQUIRED_META))

    update_raw = iso_read(path, "ota_v0/ota_update.in")
    compat_raw = iso_read(path, "ota_v0/ota_compatibility.in")
    if update_raw is None:
        rep.fail("ota_update", "cannot read ota_v0/ota_update.in")
        emit()
        return 2
    meta = parse_metadata(update_raw)
    if set(meta) != {"kernel", "rootfs"}:
        rep.fail("ota_components", f"unexpected components: {sorted(meta)}")
    else:
        rep.set("ota_components", "PASS", "kernel and rootfs declared")

    if compat_raw is not None:
        m = re.search(rb"compatibility=(.+)", compat_raw)
        rep.data["compatibility"] = m.group(1).decode(errors="replace").strip() if m else None
    rep.set("compatibility_present", "PASS" if rep.data["compatibility"] else "FAIL",
            repr(rep.data["compatibility"]))

    rootfs_bytes = None
    for name in ("kernel", "rootfs"):
        if name in meta:
            assembled = verify_component(path, name, meta[name], files, rep)
            if name == "rootfs":
                rootfs_bytes = assembled

    # Device identity lives inside the rootfs, so inspect it independently of
    # whatever the build script wrote into the metadata.
    observed = None
    if rootfs_bytes is not None:
        with tempfile.TemporaryDirectory(prefix="sonix-upt-") as td:
            sq = Path(td) / "rootfs.squashfs"
            sq.write_bytes(rootfs_bytes)
            cp = None
            for candidate in (
                "squashfs-root/usr/resource/sonix/components/system-info.json",
                "usr/resource/sonix/components/system-info.json",
            ):
                cp = run(["unsquashfs", "-cat", str(sq), candidate])
                if cp.returncode == 0:
                    break
            if cp is not None and cp.returncode == 0:
                try:
                    info = json.loads(cp.stdout.decode("utf-8", "replace"))
                    for key, value in info.items():
                        if key.lower() == "device-name":
                            observed = value
                            break
                except (ValueError, TypeError):
                    pass
    rep.data["observed_device"] = observed
    if observed is None:
        rep.fail("device_identity", "system-info.json device-name not readable from rootfs")
    else:
        rep.set("device_identity", "PASS", f"device-name={observed}")
        expected_name = DEVICE_NAMES.get(args.expect_device) if args.expect_device else None
        if expected_name is None:
            rep.set("wrong_device_rejection", "NOT APPLICABLE", "no --expect-device supplied")
        elif observed == expected_name:
            rep.set("wrong_device_rejection", "PASS",
                    f"observed {observed!r} matches expected {expected_name!r}")
        else:
            rep.fail("wrong_device_rejection",
                     f"observed {observed!r} != expected {expected_name!r}; "
                     "this image must not be installed on the requested device")

    result = rep.finalize()
    payload = json.dumps(rep.data, indent=2, ensure_ascii=False)
    if args.report:
        args.report.write_text(payload + "\n", encoding="utf-8")
    print(payload)
    return 0 if result.startswith("STRUCTURAL PASS") else 2


if __name__ == "__main__":
    sys.exit(main())

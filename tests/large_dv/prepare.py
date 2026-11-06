# SPDX-License-Identifier: MIT
"""Prepare opt-in, official OpenTitan DV EDAM inputs without modifying sources.

Requires PyYAML in the setup-only FuseSoC environment. This script does not fetch,
compile or simulate external projects and is not registered with CTest.
"""

from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess
import yaml

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--setup-root", type=Path, required=True)
parser.add_argument("--output-root", type=Path, required=True)
parser.add_argument("--uvm", type=Path, required=True)
parser.add_argument("--repository-commit", required=True)
parser.add_argument("--repository-root", type=Path, required=True)
parser.add_argument("--profiles", nargs="+", help="Prepare only these named profiles")
args = parser.parse_args()
args.setup_root = args.setup_root.resolve()
args.repository_root = args.repository_root.resolve()
assert (
    subprocess.check_output(
        ["git", "-C", str(args.repository_root), "rev-parse", "HEAD"], text=True
    ).strip()
    == args.repository_commit
), "repository revision differs from the requested input pin"
S = args.output_root.resolve()
S.mkdir(parents=True, exist_ok=True)
U = args.uvm.resolve()
assert (U / "uvm_pkg.sv").is_file()
cases = []
receipts = []
specs = [
    (
        "opentitan-spi-host",
        "spi_host_env_cfg.sv",
        "ok_to_end_delay_ns = 5000;",
        "ok_to_end_delay_ns = 5001;",
        "m_spi_agent_cfg",
        "m_spi_agent_cfg.if_mode",
    ),
    (
        "opentitan-otbn",
        "otbn_env_cfg.sv",
        "num_edn = 2;",
        "num_edn = 3;",
        "num_edn",
        "num_edn = ",
    ),
    (
        "opentitan-spi-device",
        "spi_device_base_test.sv",
        "max_quit_count  = 50;",
        "max_quit_count  = 51;",
        "max_quit_count",
        "max_quit_count  = ",
    ),
    (
        "opentitan-chip",
        "chip_base_test.sv",
        "test_timeout_ns = 50_000_000;",
        "test_timeout_ns = 50_000_001;",
        "test_timeout_ns",
        "test_timeout_ns = ",
    ),
    (
        "opentitan-dma",
        "dma_base_vseq.sv",
        "sem_control = new(1);",
        "sem_control = new(2);",
        "sem_control",
        "sem_control = new(",
    ),
    (
        "opentitan-usbdev",
        "usbdev_env_cfg.sv",
        "max_outstanding_req = 1;",
        "max_outstanding_req = 2;",
        "m_tl_agent_cfgs",
        "m_tl_agent_cfgs[RAL_T::type_name]",
    ),
]
assert not args.profiles or set(args.profiles) <= {s[0] for s in specs}
for name, targetname, anchor, alternate, query, marker in specs:
    if args.profiles and name not in args.profiles:
        continue
    eda = next((args.setup_root / f"{name}-setup").rglob("*.eda.yml"))
    d = yaml.safe_load(eda.read_text())
    entries = [
        f
        for f in d["files"]
        if f.get("file_type") in ("systemVerilogSource", "verilogSource")
    ]
    files = [(eda.parent / f["name"]).resolve() for f in entries]
    roots = [p for f, p in zip(entries, files) if not f.get("is_include_file")]
    assert all(p.is_file() for p in files)
    target = next(p for p in files if p.name == targetname)
    assert target.read_text().count(anchor) == 1
    root = S / name
    root.mkdir(exist_ok=True)
    defines = [
        "UVM",
        "UVM_NO_DEPRECATED",
        "UVM_REGEX_NO_DPI",
        "UVM_REG_ADDR_WIDTH=32",
        "UVM_REG_DATA_WIDTH=32",
        "UVM_REG_BYTENABLE_WIDTH=4",
        "SIMULATION",
        "DUT_HIER=tb.dut",
    ]
    # Official DVSIM build options supplied outside the FuseSoC core target.
    if name == "opentitan-spi-device":
        defines.append("SRAM_TYPE=spi_device_pkg::SramType2p")
    if name == "opentitan-chip":
        defines.append("TOP_LEVEL_DV")
    (root / "design.f").write_text(
        "\n".join(
            ["+define+" + v for v in defines]
            + ["+incdir+" + p for p in dict.fromkeys(str(p.parent) for p in files)]
            + list(map(str, roots))
        )
        + "\n"
    )
    case = {
        "id": name,
        "repo": "opentitan",
        "root": str(root),
        "top": "tb",
        "dut": "tb.dut",
        "target": str(target),
        "anchor": anchor,
        "alternate": alternate,
        "query": query,
        "hoverAnchor": marker,
        "structuralEnd": "endclass",
        "freeze": {"instances": ["tb.dut"], "packages": ["uvm_pkg"]},
        "config": {
            "flags": "--single-unit --threads 1 --timescale 1ns/1ps --relax-string-conversions --top tb",
            "uvm": {"sourceDirectory": str(U)},
            "index": [{"dirs": [str(eda.parent / "src"), str(root), str(U)]}],
        },
    }
    cases.append(case)
    unique = set(files)
    dv = {
        p
        for f, p in zip(entries, files)
        if f.get("core", "").split(":")[1].startswith("dv")
    }
    profile_config = {
        "opentitan-chip": "hw/top_earlgrey/dv/chip_sim_cfg.hjson",
        "opentitan-spi-device": "hw/ip/spi_device/dv/spi_device_2p_sim_cfg.hjson",
    }.get(name)

    def scale(ps):
        return {
            "files": len(ps),
            "bytes": sum(p.stat().st_size for p in ps),
            "lines": sum(p.read_bytes().count(b"\n") for p in ps),
            "lexicalClassDeclarations": sum(
                len(
                    re.findall(
                        r"(?m)^\s*(?:virtual\s+)?class\s+\w+",
                        p.read_text(errors="replace"),
                    )
                )
                for p in ps
            ),
        }

    receipts.append(
        {
            "id": name,
            "repositoryCommit": args.repository_commit,
            "officialTarget": d["name"],
            "edamSha256": hashlib.sha256(eda.read_bytes()).hexdigest(),
            "profileBuildOptionsSource": profile_config,
            "profileBuildOptionsSha256": hashlib.sha256(
                (args.repository_root / profile_config).read_bytes()
            ).hexdigest()
            if profile_config
            else None,
            "analysisDefines": defines,
            "hdlRoots": len(roots),
            "physicalInputScale": scale(unique),
            "dvInputScale": scale(dv),
            "nonDvInputScale": scale(unique - dv),
            "scaleMeaning": "EDAM physical HDL inputs, lexical class counts; not elaborated instance counts",
            "inputHashes": {
                str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in sorted(unique)
            },
            "sourceEdits": False,
            "stubs": False,
            "diagnosticSuppression": False,
            "heldOut": name == "opentitan-usbdev",
        }
    )
(S / "large-matrix.json").write_text(json.dumps(cases, indent=2) + "\n")
(S / "large-inputs.json").write_text(json.dumps(receipts, indent=2) + "\n")
print([{k: v for k, v in r.items() if k != "inputHashes"} for r in receipts])

"""Smoke tests for the Python config loader and partition store."""
import os
import sys
import textwrap

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "python"))

from config_loader import load as load_cfg  # noqa: E402
from partition_store import PartitionStore, parse_ymd_any  # noqa: E402


def test_config_load(tmp_path):
    p = tmp_path / "topology.conf"
    p.write_text(textwrap.dedent("""
        node A host=127.0.0.1 port=50051 lang=cpp root=true
        node B host=127.0.0.1 port=50052 lang=python
        edge A B
        partition A data/part_A.csv
        partition B data/part_B.csv
        param initial_chunk_rows 256
    """))
    cfg = load_cfg(str(p))
    assert "A" in cfg.nodes and "B" in cfg.nodes
    assert cfg.nodes["A"].is_root is True
    assert cfg.nodes["B"].lang == "python"
    assert cfg.peers["A"] == {"B"}
    assert cfg.peers["B"] == {"A"}
    assert cfg.partition_path("A").endswith("part_A.csv")
    assert cfg.param_int("initial_chunk_rows", 0) == 256
    assert cfg.root_id() == "A"


def test_partition_store_load(tmp_path):
    csv = tmp_path / "part.csv"
    csv.write_text(
        "unique_key,created_ymd,borough,latitude,longitude,complaint_type\n"
        "100,20240115,BROOKLYN,40.7,-73.95,Noise - Residential\n"
        "200,20240220,QUEENS,40.75,-73.85,Illegal Parking\n"
    )
    s = PartitionStore.empty()
    s.load(csv)
    assert len(s) == 2
    assert s.keys[0] == 100
    assert s.dates[0] == 20240115
    assert s.boroughs[0] == 2  # BROOKLYN


def test_parse_ymd_any():
    assert parse_ymd_any("2024-01-15T01:00:00") == 20240115
    assert parse_ymd_any("20240115") == 20240115
    assert parse_ymd_any("") == 0

"""Parser for the mini2 topology.conf format. Same grammar as the C++ side.

Format:
    node <id> host=<h> port=<p> lang=<cpp|python> [root=true]
    edge <a> <b>
    partition <id> <csv_path>
    param <key> <value>
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Set


@dataclass
class NodeEntry:
    id: str
    host: str
    port: int
    lang: str
    is_root: bool = False


@dataclass
class Config:
    nodes: Dict[str, NodeEntry] = field(default_factory=dict)
    peers: Dict[str, Set[str]] = field(default_factory=dict)
    partitions: Dict[str, str] = field(default_factory=dict)
    params: Dict[str, str] = field(default_factory=dict)

    def neighbors(self, node_id: str) -> Set[str]:
        return self.peers.get(node_id, set())

    def all_node_ids(self) -> List[str]:
        return list(self.nodes.keys())

    def root_id(self) -> str:
        for nid, n in self.nodes.items():
            if n.is_root:
                return nid
        raise RuntimeError("no node marked root=true")

    def param_int(self, key: str, default: int) -> int:
        return int(self.params.get(key, default))

    def partition_path(self, node_id: str) -> str:
        return self.partitions.get(node_id, "")


def load(path: str | Path) -> Config:
    cfg = Config()
    with open(path, "r") as fh:
        for lineno, raw in enumerate(fh, start=1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            toks = line.split()
            kw = toks[0]
            if kw == "node":
                if len(toks) < 2:
                    raise ValueError(f"bad node line {lineno}")
                e = NodeEntry(id=toks[1], host="", port=0, lang="cpp", is_root=False)
                for tok in toks[2:]:
                    if "=" not in tok:
                        continue
                    k, v = tok.split("=", 1)
                    if k == "host":
                        e.host = v
                    elif k == "port":
                        e.port = int(v)
                    elif k == "lang":
                        e.lang = v
                    elif k == "root":
                        e.is_root = v.lower() in ("true", "1", "yes")
                if not e.host or not e.port:
                    raise ValueError(f"node {e.id} missing host/port")
                cfg.nodes[e.id] = e
                cfg.peers.setdefault(e.id, set())
            elif kw == "edge":
                if len(toks) != 3:
                    raise ValueError(f"bad edge line {lineno}")
                cfg.peers.setdefault(toks[1], set()).add(toks[2])
                cfg.peers.setdefault(toks[2], set()).add(toks[1])
            elif kw == "partition":
                if len(toks) != 3:
                    raise ValueError(f"bad partition line {lineno}")
                cfg.partitions[toks[1]] = toks[2]
            elif kw == "param":
                if len(toks) != 3:
                    raise ValueError(f"bad param line {lineno}")
                cfg.params[toks[1]] = toks[2]
            else:
                raise ValueError(f"unknown directive '{kw}' on line {lineno}")
    return cfg

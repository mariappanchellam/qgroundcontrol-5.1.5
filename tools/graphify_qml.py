#!/usr/bin/env python3
"""Add the QML side of the app to graphify's code graph.

graphify (https://github.com/Graphify-Labs/graphify) parses C++ but not QML, so its graph has no QML
files and no QML-to-C++ links. This adds, to graphify-out/graph.json:

- a node per QML file, contained in a node for its QML module (qt_add_qml_module URI)
- a node per Q_PROPERTY of every C++ class, linked to its getter, setter and change signal
- QML -> C++ links: types registered with QML_ELEMENT / QML_NAMED_ELEMENT / QML_SINGLETON that a QML
  file instantiates or uses, properties it binds to (Singleton.prop, id.prop, chains such as
  QGroundControl.videoManager.decoding) and Q_INVOKABLE functions it calls
- QML -> QML links: components a QML file instantiates, and modules it imports

then reclusters the graph and regenerates the report. Nodes and links it adds carry _origin "qml" and
are replaced on every run.

Examples:
    graphify update . && ./tools/graphify_qml.py
    ./tools/graphify_qml.py --wait            # first wait for a graph rebuild started by a git hook
    ./tools/graphify_qml.py --install-hooks   # run --wait after graphify's git hooks
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

ORIGIN = "qml"
GRAPH = Path("graphify-out") / "graph.json"
HOOKS = ("post-commit", "post-checkout", "post-merge", "post-rewrite")
HOOK_START = "# dhaksha-qml-links-start"
HOOK_END = "# dhaksha-qml-links-end"
HOOK_BLOCK = f"""{HOOK_START}
# Adds the QML files and their C++ links once graphify's background rebuild has finished
_DQL_TOP=$(git rev-parse --show-toplevel 2>/dev/null)
if [ -n "$_DQL_TOP" ] && [ -f "$_DQL_TOP/tools/graphify_qml.py" ]; then
    mkdir -p "$HOME/.cache"
    (cd "$_DQL_TOP" && nohup python3 tools/graphify_qml.py --wait >> "$HOME/.cache/graphify-rebuild.log" 2>&1 &)
fi
{HOOK_END}
"""
WAIT_TIMEOUT_S = 900

QML_TYPE_MACRO = re.compile(
    r"\bQML_(?:NAMED_ELEMENT\s*\(\s*(\w+)\s*\)|(ELEMENT|SINGLETON|UNCREATABLE|ANONYMOUS)\b)"
)
CLASS_DECL = re.compile(r"\b(?:class|struct)\s+(?:\w+_EXPORT\s+)?(\w+)\b[^;{()]*\{")
Q_PROPERTY = re.compile(r"\bQ_PROPERTY\s*\(([^()]*)\)", re.S)
Q_INVOKABLE = re.compile(r"\bQ_INVOKABLE\s+(?:virtual\s+|static\s+)?[\w:<>,\s*&]+?\b(\w+)\s*\(")
PROPERTY_KEYWORDS = {
    "READ", "WRITE", "MEMBER", "NOTIFY", "RESET", "CONSTANT", "FINAL", "BINDABLE",
    "STORED", "DESIGNABLE", "SCRIPTABLE", "USER", "REQUIRED", "REVISION",
}  # fmt: skip
QML_MODULE = re.compile(r"\bqt_add_qml_module\s*\(\s*\S+\s+URI\s+([\w.]+)", re.S)
QML_STRING = re.compile(r"\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'")
QML_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
QML_LINE_COMMENT = re.compile(r"//[^\n]*")
QML_IMPORT = re.compile(r"^\s*import\s+([A-Za-z_][\w.]*)", re.M)
QML_TYPE_USE = re.compile(r"(?<![\w.])([A-Z]\w*)\s*\{")
QML_ID = re.compile(r"(?<![\w.])([A-Z]\w*)\s*\{[^{}]*?\bid\s*:\s*(\w+)", re.S)
QML_MEMBER_CHAIN = re.compile(r"(?<![\w.])([A-Za-z_]\w*)((?:\s*\.\s*[A-Za-z_]\w*)+)")


@dataclass
class CppClass:
    name: str
    header: str
    node_id: str | None = None
    qml_name: str | None = None
    properties: dict[str, dict[str, str]] = field(default_factory=dict)
    invokables: dict[str, int] = field(default_factory=dict)


def _git_files(root: Path, pattern: str) -> list[str]:
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z", "--", pattern],
        capture_output=True,
        check=True,
    )
    return [path for path in result.stdout.decode().split("\0") if path]


def _slug(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "_", text.lower()).strip("_")


def _line(text: str, offset: int) -> str:
    return f"L{text.count(chr(10), 0, offset) + 1}"


def _base_type(type_text: str) -> str:
    names = re.findall(r"[A-Za-z_]\w*", type_text.replace("const", " "))
    return names[-1] if names else ""


def _parse_headers(root: Path, class_nodes: dict[tuple[str, str], str]) -> dict[str, CppClass]:
    """Q_PROPERTY, Q_INVOKABLE and QML registration of every C++ class, by class name."""
    classes: dict[str, CppClass] = {}
    for header in _git_files(root, "*.h"):
        try:
            text = (root / header).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if "Q_PROPERTY" not in text and "QML_" not in text and "Q_INVOKABLE" not in text:
            continue
        starts = [(match.start(), match.group(1)) for match in CLASS_DECL.finditer(text)]

        def owner(
            offset: int, header: str = header, starts: list[tuple[int, str]] = starts
        ) -> CppClass | None:
            name = None
            for start, class_name in starts:
                if start > offset:
                    break
                name = class_name
            if not name:
                return None
            cls = classes.get(name)
            if cls is None or cls.header != header:
                cls = CppClass(name, header, class_nodes.get((header, name)))
                classes[name] = cls
            return cls

        for match in QML_TYPE_MACRO.finditer(text):
            cls = owner(match.start())
            if cls and match.group(2) != "ANONYMOUS":
                cls.qml_name = match.group(1) or cls.qml_name or cls.name
        for match in Q_PROPERTY.finditer(text):
            cls = owner(match.start())
            tokens = match.group(1).replace("*", " * ").replace("&", " & ").split()
            keyword = next(
                (i for i, token in enumerate(tokens) if token in PROPERTY_KEYWORDS), None
            )
            if not cls or keyword is None or keyword < 2:
                continue
            accessors = {
                "type": _base_type(" ".join(tokens[: keyword - 1])),
                "line": _line(text, match.start()),
            }
            for index in range(keyword, len(tokens) - 1):
                if tokens[index] in ("READ", "WRITE", "NOTIFY", "RESET"):
                    accessors[tokens[index].lower()] = tokens[index + 1]
            cls.properties[tokens[keyword - 1]] = accessors
        for match in Q_INVOKABLE.finditer(text):
            cls = owner(match.start())
            if cls:
                cls.invokables.setdefault(match.group(1), text.count("\n", 0, match.start()) + 1)
    return classes


def _qml_modules(root: Path) -> dict[str, str]:
    """QML module URI by directory, from qt_add_qml_module calls."""
    modules: dict[str, str] = {}
    for cmake in _git_files(root, "*CMakeLists.txt"):
        try:
            text = (root / cmake).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for match in QML_MODULE.finditer(text):
            modules[str(Path(cmake).parent)] = match.group(1)
    return modules


def _strip_qml(text: str) -> str:
    """Strings and comments blanked, keeping line numbers."""
    text = QML_STRING.sub('""', text)
    text = QML_BLOCK_COMMENT.sub(lambda match: "\n" * match.group(0).count("\n"), text)
    return QML_LINE_COMMENT.sub("", text)


class GraphBuilder:
    def __init__(self, graph: dict) -> None:
        self.graph = graph
        self.node_ids = {node["id"] for node in graph["nodes"]}
        self.nodes: list[dict] = []
        self.links: dict[tuple[str, str, str], dict] = {}

    def node(self, node_id: str, label: str, source_file: str, location: str = "L1") -> str:
        if node_id not in self.node_ids:
            self.node_ids.add(node_id)
            self.nodes.append(
                {
                    "id": node_id,
                    "label": label,
                    "_origin": ORIGIN,
                    "file_type": "code",
                    "norm_label": label.lower(),
                    "source_file": source_file,
                    "source_location": location,
                }
            )
        return node_id

    def link(
        self,
        source: str,
        target: str,
        relation: str,
        source_file: str,
        location: str,
        inferred: bool = False,
    ) -> None:
        key = (source, target, relation)
        if key in self.links:
            self.links[key]["weight"] += 1.0
            return
        self.links[key] = {
            "source": source,
            "target": target,
            "relation": relation,
            "_origin": ORIGIN,
            "confidence": "INFERRED" if inferred else "EXTRACTED",
            "confidence_score": 0.8 if inferred else 1.0,
            "context": "qml",
            "source_file": source_file,
            "source_location": location,
            "weight": 1.0,
        }


def _member_target(builder: GraphBuilder, cls: CppClass, member: str) -> tuple[str, str] | None:
    """Node a QML reference to cls.member resolves to, and the link relation."""
    if not cls.node_id:
        return None
    prop = cls.properties.get(member)
    if prop is not None:
        prop_id = builder.node(
            f"{cls.node_id}__qprop_{member.lower()}",
            f"{cls.name}.{member} (Q_PROPERTY)",
            cls.header,
            prop["line"],
        )
        builder.link(cls.node_id, prop_id, "defines_property", cls.header, prop["line"])
        for accessor, relation in (
            ("read", "read_by"),
            ("write", "written_by"),
            ("notify", "notified_by"),
        ):
            method_id = f"{cls.node_id}_{prop.get(accessor, '').lower()}"
            if accessor in prop and method_id in builder.node_ids:
                builder.link(prop_id, method_id, relation, cls.header, prop["line"])
        return prop_id, "binds"
    if member in cls.invokables:
        method_id = f"{cls.node_id}_{member.lower()}"
        if method_id in builder.node_ids:
            return method_id, "calls"
    return None


def _add_qml(root: Path, builder: GraphBuilder, classes: dict[str, CppClass]) -> int:
    modules = _qml_modules(root)
    qml_types = {cls.qml_name: cls for cls in classes.values() if cls.qml_name and cls.node_id}
    files = _git_files(root, "*.qml")
    components: dict[str, list[str]] = {}
    file_nodes: dict[str, str] = {}
    for path in files:
        components.setdefault(Path(path).stem, []).append(path)
        file_nodes[path] = builder.node(f"qml_{_slug(path)}", Path(path).name, path)

    def module_node(uri: str, source_file: str) -> str:
        return builder.node(f"qml_module_{_slug(uri)}", f"{uri} (QML module)", source_file)

    for path in files:
        try:
            text = _strip_qml((root / path).read_text(encoding="utf-8", errors="replace"))
        except OSError:
            continue
        this = file_nodes[path]
        directory = Path(path).parent
        for candidate in (directory, *directory.parents):
            uri = modules.get(str(candidate))
            if uri:
                builder.link(module_node(uri, path), this, "contains", path, "L1")
                break
        for match in QML_IMPORT.finditer(text):
            if match.group(1) in modules.values():
                builder.link(
                    this,
                    module_node(match.group(1), path),
                    "imports",
                    path,
                    _line(text, match.start()),
                )

        ids: dict[str, CppClass] = {}
        for match in QML_ID.finditer(text):
            if match.group(1) in qml_types:
                ids[match.group(2)] = qml_types[match.group(1)]

        for match in QML_TYPE_USE.finditer(text):
            name = match.group(1)
            location = _line(text, match.start())
            cls = qml_types.get(name)
            if cls and cls.node_id:
                builder.link(this, cls.node_id, "instantiates", path, location)
                continue
            # A component from the same directory wins over one of the same name elsewhere
            candidates = components.get(name, [])
            local = [candidate for candidate in candidates if Path(candidate).parent == directory]
            if local or len(candidates) == 1:
                builder.link(
                    this, file_nodes[(local or candidates)[0]], "instantiates", path, location
                )

        for match in QML_MEMBER_CHAIN.finditer(text):
            first = match.group(1)
            cls = qml_types.get(first) if first[0].isupper() else ids.get(first)
            if cls is None:
                continue
            location = _line(text, match.start())
            if cls.node_id:
                builder.link(this, cls.node_id, "uses", path, location)
            for depth, member in enumerate(re.findall(r"[A-Za-z_]\w*", match.group(2))):
                target = _member_target(builder, cls, member)
                if target is None:
                    break
                builder.link(this, target[0], target[1], path, location, inferred=depth > 0)
                prop = cls.properties.get(member)
                next_cls = classes.get(prop["type"]) if prop else None
                if next_cls is None:
                    break
                cls = next_cls
    return len(files)


def _wait_for_rebuild() -> None:
    """Wait while a graphify rebuild started by a git hook is running."""
    deadline = time.monotonic() + WAIT_TIMEOUT_S
    while time.monotonic() < deadline:
        running = False
        for cmdline in Path("/proc").glob("[0-9]*/cmdline"):
            try:
                if b"_rebuild_code" in cmdline.read_bytes() and cmdline.parent.name != str(
                    os.getpid()
                ):
                    running = True
                    break
            except OSError:
                continue
        if not running:
            return
        time.sleep(2)


def _install_hooks(root: Path) -> int:
    hooks_dir = Path(
        subprocess.run(["git", "-C", str(root), "rev-parse", "--git-path", "hooks"], capture_output=True, text=True,
                       check=True).stdout.strip()
    )  # fmt: skip
    if not hooks_dir.is_absolute():
        hooks_dir = root / hooks_dir
    for name in HOOKS:
        hook = hooks_dir / name
        if not hook.exists() or not re.search(
            r"# graphify-(?:\w+-)?hook-end", text := hook.read_text()
        ):
            print(f"{name}: no graphify hook, run `graphify hook install` first")
            continue
        if HOOK_START not in text:
            hook.write_text(text.rstrip("\n") + "\n" + HOOK_BLOCK)
        print(f"{name}: QML links added after the graphify rebuild")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--wait", action="store_true", help="wait for a running graphify hook rebuild first"
    )
    parser.add_argument(
        "--install-hooks", action="store_true", help="run --wait after graphify's git hooks"
    )
    parser.add_argument(
        "--no-cluster", action="store_true", help="only add the QML nodes and links"
    )
    args = parser.parse_args()

    root = Path(
        subprocess.run(
            ["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, check=True
        ).stdout.strip()
    )
    if args.install_hooks:
        return _install_hooks(root)
    if args.wait:
        _wait_for_rebuild()

    graph_path = root / GRAPH
    if not graph_path.exists():
        print(f"{GRAPH} not found: run `graphify update .` first")
        return 1
    graph = json.loads(graph_path.read_text(encoding="utf-8"))
    graph["nodes"] = [node for node in graph["nodes"] if node.get("_origin") != ORIGIN]
    graph["links"] = [link for link in graph["links"] if link.get("_origin") != ORIGIN]

    class_nodes = {
        (node["source_file"], node["label"]): node["id"]
        for node in graph["nodes"]
        if node.get("_callable_class") and str(node.get("source_file", "")).endswith(".h")
    }
    classes = _parse_headers(root, class_nodes)
    builder = GraphBuilder(graph)
    qml_files = _add_qml(root, builder, classes)
    graph["nodes"].extend(builder.nodes)
    graph["links"].extend(builder.links.values())
    graph_path.write_text(json.dumps(graph), encoding="utf-8")

    relations: dict[str, int] = {}
    for link in builder.links.values():
        relations[link["relation"]] = relations.get(link["relation"], 0) + 1
    print(
        f"graphify QML: {qml_files} QML files, {len(builder.nodes)} nodes and {len(builder.links)} links added"
    )
    print("  " + ", ".join(f"{relation} {count}" for relation, count in sorted(relations.items())))

    if args.no_cluster:
        return 0
    env = dict(
        os.environ, GRAPHIFY_BACKEND=os.environ.get("GRAPHIFY_BACKEND", "none"), PYTHONHASHSEED="0"
    )
    return subprocess.run(["graphify", "cluster-only", str(root)], env=env, check=False).returncode


if __name__ == "__main__":
    sys.exit(main())

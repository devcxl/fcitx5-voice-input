from __future__ import annotations
import hashlib, json, os, re, shutil, subprocess, tempfile
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any
import yaml

CABBAGE_DIR = ".cabbage"
LEGACY_PLACEHOLDERS = (
    "Describe the architectural decision context.",
    "Record the chosen decision.",
    "Record positive and negative consequences.",
    "Describe endpoints/events, inputs, outputs and errors.",
    "Describe versioning and backward compatibility.",
    "Describe schema changes.",
    "Describe forward migration.",
    "Describe rollback and data-safety constraints.",
    "Describe the artifact.",
    "Describe material risks and mitigations.",
    "Describe user/business impact.",
    "Record key timestamps and actions.",
    "Record containment and recovery.",
    "Describe the root cause.",
    "Describe systemic contributing factors.",
    "List owned corrective actions.",
    "Replace this text with the product goal.",
    "Describe in-scope and out-of-scope behavior.",
    "Define observable acceptance criteria.",
    "Describe deployment steps and ordering.",
    "Describe rollback triggers and steps.",
    "Describe post-deploy verification.",
    "Describe the problem.",
    "Describe the proposal.",
    "Describe alternatives and trade-offs.",
    "Describe trust boundaries, abuse cases and sensitive assets.",
    "Describe authorization, validation, secrets, audit and mitigations.",
    "Describe current state and constraints.",
    "Describe the proposed design. Use Mermaid for reviewable diagrams.",
    "Describe failure handling and recovery.",
    "Describe incremental rollout and compatibility.",
    "Describe unit, integration, E2E, regression and non-functional coverage.",
    "Define the critical test cases.",
)

class CabbageError(RuntimeError):
    pass

def write_text_atomic(path: Path, text: str, encoding: str = "utf-8") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", dir=path.parent, delete=False, encoding=encoding) as tf:
        tf.write(text)
        temp_name = tf.name
    os.replace(temp_name, path)

def load_yaml(path: Path) -> dict:
    if not path.exists():
        raise CabbageError(f"missing file: {path}")
    try:
        data = yaml.safe_load(path.read_text(encoding="utf-8"))
    except yaml.YAMLError as e:
        raise CabbageError(f"invalid YAML in {path}: {e}")
    return data or {}

def dump_yaml(path: Path, data: dict) -> None:
    content = yaml.safe_dump(data, sort_keys=False, allow_unicode=True)
    write_text_atomic(path, content, encoding="utf-8")

def dump_json(path: Path, data: dict) -> None:
    content = json.dumps(data, indent=2, ensure_ascii=False) + "\n"
    write_text_atomic(path, content, encoding="utf-8")

def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes()) if path.exists() else "MISSING"

def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()

def project_root(start: Path | None = None) -> Path:
    p = (start or Path.cwd()).resolve()
    for cur in [p, *p.parents]:
        if (cur / CABBAGE_DIR / "config.yaml").exists():
            return cur
    raise CabbageError("not a cabbage project; run `cabbage init`")

def load_config(root: Path) -> dict:
    return load_yaml(root / CABBAGE_DIR / "config.yaml")

def change_dir(root: Path, change_id: str) -> Path:
    return root / CABBAGE_DIR / "changes" / change_id

def change_spec(root: Path, change_id: str) -> dict:
    return load_yaml(change_dir(root, change_id) / "change.yaml")

def workflow(root: Path, change_type: str) -> tuple[dict, Path]:
    p = root / CABBAGE_DIR / "workflows" / f"{change_type}.yaml"
    return load_yaml(p), p

def state_path(root: Path, change_id: str) -> Path:
    return change_dir(root, change_id) / "state.json"

def load_state(root: Path, change_id: str) -> dict:
    p = state_path(root, change_id)
    if not p.exists():
        return {"version": 1, "completed": {}}
    try:
        data = json.loads(p.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        raise CabbageError(f"invalid state.json: {e}")
    data.setdefault("completed", {})
    return data

def save_state(root: Path, change_id: str, state: dict) -> None:
    dump_json(state_path(root, change_id), state)

def stage_context(spec: dict, stage: dict) -> dict:
    ctx={"type": spec.get("type")}
    cond=stage.get("when")
    if isinstance(cond,str) and cond.startswith("impact."):
        key=cond.split(".",1)[1]
        ctx["condition"]={key: bool(spec.get("impact",{}).get(key,False))}
    return ctx

def condition_enabled(stage: dict, spec: dict) -> bool:
    cond = stage.get("when")
    if not cond or cond == "always":
        return True
    if cond.startswith("impact."):
        return bool(spec.get("impact", {}).get(cond.split(".", 1)[1], False))
    raise CabbageError(f"unsupported condition: {cond}")

def stage_map(wf: dict) -> dict[str, dict]:
    return {s["id"]: s for s in wf.get("stages", [])}

def artifact_path(root: Path, change_id: str, stage: dict) -> Path | None:
    artifact = stage.get("artifact")
    return change_dir(root, change_id) / artifact if artifact else None

def current_signature(root: Path, change_id: str, stage_id: str, memo: dict | None = None, visiting: set[str] | None = None) -> str:
    memo = memo if memo is not None else {}
    if stage_id in memo:
        return memo[stage_id]
    visiting = visiting if visiting is not None else set()
    if stage_id in visiting:
        raise CabbageError(f"dependency cycle detected involving stage: `{stage_id}`")
    visiting.add(stage_id)

    spec = change_spec(root, change_id)
    wf, wf_path = workflow(root, spec["type"])
    smap = stage_map(wf)
    if stage_id not in smap:
        raise CabbageError(f"unknown stage: {stage_id}")
    stage = smap[stage_id]
    ap = artifact_path(root, change_id, stage)
    payload = {
        "workflow": sha256_file(wf_path),
        "context": stage_context(spec, stage),
        "stage": stage,
        "artifact": sha256_file(ap) if ap else None,
        "dependencies": {d: current_signature(root, change_id, d, memo, visiting.copy()) for d in stage.get("depends_on", []) if d in smap and condition_enabled(smap[d], spec)},
    }
    sig = sha256_bytes(json.dumps(payload, sort_keys=True, ensure_ascii=False).encode())
    memo[stage_id] = sig
    visiting.remove(stage_id)
    return sig

def stage_statuses(root: Path, change_id: str) -> list[dict]:
    spec = change_spec(root, change_id)
    wf, _ = workflow(root, spec["type"])
    state = load_state(root, change_id)
    completed = state.get("completed", {})
    out=[]
    memo={}
    for stage in wf.get("stages", []):
        sid=stage["id"]
        enabled=condition_enabled(stage, spec)
        if not enabled:
            status="skipped"
        elif sid not in completed:
            status="pending"
        else:
            status="done" if completed[sid].get("signature") == current_signature(root, change_id, sid, memo) else "stale"
        out.append({"id": sid, "status": status, "artifact": stage.get("artifact"), "description": stage.get("description", "")})
    return out

def parse_frontmatter(text: str) -> tuple[dict, str]:
    if not text.startswith("---\n"):
        return {}, text
    end=text.find("\n---\n", 4)
    if end < 0:
        return {}, text
    try:
        meta=yaml.safe_load(text[4:end]) or {}
    except yaml.YAMLError as e:
        raise CabbageError(f"invalid frontmatter YAML: {e}")
    return meta, text[end+5:]

def extract_headings_and_slugs(markdown_text: str) -> set[str]:
    slugs = set()
    for line in markdown_text.splitlines():
        m = re.match(r"^#+\s+(.+)$", line)
        if m:
            title = m.group(1).strip()
            slug = re.sub(r"[^\w\s-]", "", title).strip().lower()
            slug = re.sub(r"[\s_]+", "-", slug)
            if slug:
                slugs.add(slug)
            slugs.add(title.lower())
    return slugs

def validate_markdown(root: Path, change_id: str, stage: dict, verification: bool=False) -> list[str]:
    errors=[]
    ap=artifact_path(root, change_id, stage)
    if not ap:
        return errors
    if not ap.exists():
        return [f"{stage['id']}: missing artifact {ap.relative_to(root)}"]
    text=ap.read_text(encoding="utf-8")
    meta, body=parse_frontmatter(text)
    if meta.get("change") != change_id:
        errors.append(f"{stage['id']}: frontmatter `change` must be `{change_id}`")
    if meta.get("cabbage_stage") != stage["id"]:
        errors.append(f"{stage['id']}: frontmatter `cabbage_stage` must be `{stage['id']}`")
    for heading in stage.get("required_headings", []):
        if not re.search(rf"^#+\s+{re.escape(heading)}\s*$", body, flags=re.M|re.I):
            errors.append(f"{stage['id']}: missing heading `{heading}`")
    if verification:
        if re.search(r"\b(TODO|TBD|FIXME)\b", body, flags=re.I):
            errors.append(f"{stage['id']}: unresolved TODO/TBD/FIXME")
        body_lines = {
            re.sub(r"^\s*[-*]\s+", "", line).strip() for line in body.splitlines()
        }
        has_placeholder = re.search(r"<!--\s*CABBAGE:", body, flags=re.I) or any(
            placeholder in body_lines for placeholder in LEGACY_PLACEHOLDERS
        )
        if has_placeholder:
            errors.append(
                f"{stage['id']}: placeholder content remains; replace all template prompts"
            )

    # Mermaid verification
    in_mermaid = False
    mermaid_blocks = []
    cur_block = []
    for line in text.splitlines():
        stripped = line.strip()
        if not in_mermaid and stripped.startswith("```mermaid"):
            in_mermaid = True
            cur_block = []
        elif in_mermaid and stripped == "```":
            in_mermaid = False
            mermaid_blocks.append("\n".join(cur_block))
        elif in_mermaid:
            cur_block.append(line)
    if in_mermaid:
        errors.append(f"{stage['id']}: unclosed Mermaid block")

    MERMAID_TYPES = (
        "graph", "flowchart", "sequenceDiagram", "classDiagram", "stateDiagram",
        "stateDiagram-v2", "erDiagram", "journey", "gantt", "pie", "quadrantChart",
        "requirementDiagram", "gitGraph", "c4Context", "mindmap", "timeline", "sankey-beta", "xychart-beta"
    )
    for block in mermaid_blocks:
        clean_lines = [l.strip() for l in block.splitlines() if l.strip() and not l.strip().startswith("%%")]
        if not clean_lines:
            errors.append(f"{stage['id']}: empty Mermaid diagram block")
        elif not any(clean_lines[0].startswith(mtype) for mtype in MERMAID_TYPES):
            errors.append(f"{stage['id']}: Mermaid block missing recognized diagram type (e.g. flowchart, sequenceDiagram)")

    # Local markdown links and anchors
    for target in re.findall(r"\[[^\]]*\]\(([^)]+)\)", text):
        target=target.strip().split()[0].strip("<>")
        if not target or target.startswith(("http://", "https://", "mailto:")):
            continue
        raw, *anchor = target.split("#", 1)
        anchor_name = anchor[0].strip().lower() if anchor else None

        if raw:
            p=(ap.parent/raw).resolve()
            try:
                p.relative_to(root.resolve())
            except ValueError:
                errors.append(f"{stage['id']}: link escapes project root: {target}")
                continue
            if not p.exists():
                errors.append(f"{stage['id']}: broken local link: {target}")
                continue
            if p.is_dir():
                if not (p / "README.md").exists() and not (p / "index.md").exists():
                    errors.append(f"{stage['id']}: directory link missing README.md/index.md: {target}")
            elif anchor_name and p.suffix.lower() in {".md", ".mdx"}:
                target_slugs = extract_headings_and_slugs(p.read_text(encoding="utf-8"))
                if anchor_name not in target_slugs:
                    errors.append(f"{stage['id']}: broken anchor in link: {target}")
        elif anchor_name:
            current_slugs = extract_headings_and_slugs(text)
            if anchor_name not in current_slugs:
                errors.append(f"{stage['id']}: broken internal anchor: #{anchor_name}")

    if stage.get("validator") == "checklist":
        boxes=re.findall(r"^\s*[-*]\s+\[([ xX\-\/])\]", body, flags=re.M)
        if not boxes:
            errors.append(f"{stage['id']}: expected at least one task checkbox")
        if verification and any(x.strip() in {"", " "} for x in boxes):
            errors.append(f"{stage['id']}: unchecked implementation tasks remain")
    return errors

def dependency_errors(root: Path, change_id: str, stage: dict) -> list[str]:
    spec=change_spec(root, change_id); wf,_=workflow(root,spec["type"]); smap=stage_map(wf)
    statuses={x["id"]:x["status"] for x in stage_statuses(root,change_id)}
    errors=[]
    for dep in stage.get("depends_on", []):
        if dep in smap and condition_enabled(smap[dep], spec) and statuses.get(dep) != "done":
            errors.append(f"{stage['id']}: dependency `{dep}` is {statuses.get(dep)}")
    return errors

def verify_stage(root: Path, change_id: str, stage_id: str) -> None:
    spec=change_spec(root,change_id); wf,_=workflow(root,spec["type"]); smap=stage_map(wf)
    if stage_id not in smap: raise CabbageError(f"unknown stage: {stage_id}")
    st=smap[stage_id]
    if not condition_enabled(st,spec): raise CabbageError(f"stage `{stage_id}` is skipped by current impact")
    errors=dependency_errors(root,change_id,st)+validate_markdown(root,change_id,st,verification=True)
    if errors: raise CabbageError("\n".join(errors))
    state=load_state(root,change_id)
    state["completed"][stage_id]={"signature":current_signature(root,change_id,stage_id),"verified_at":now_iso(),"completed_at":now_iso()}
    save_state(root,change_id,state)

DEFAULT_STAGE_DOCS_MAPPING = {
    "requirement": "01-product/{change_id}.md",
    "design": "03-architecture/system-design/{change_id}.md",
    "adr": "03-architecture/adr/{change_id}.md",
    "rfc": "03-architecture/rfc/{change_id}.md",
    "api": "05-api/{change_id}.md",
    "database": "04-data/{change_id}.md",
    "security": "09-security/{change_id}.md",
    "tests": "08-testing/{change_id}.md",
    "release": "12-release/{change_id}.md",
    "incident": "15-incidents/{change_id}.md",
    "postmortem": "15-incidents/{change_id}-postmortem.md",
}

def stage_docs_mapping(root: Path) -> dict[str, str]:
    cfg = load_config(root)
    custom = cfg.get("docs", {}).get("mapping", {})
    mapping = dict(DEFAULT_STAGE_DOCS_MAPPING)
    mapping.update(custom)
    return mapping

def sync_change_to_docs(root: Path, change_id: str) -> list[str]:
    spec=change_spec(root, change_id); wf,_=workflow(root, spec["type"])
    docs_name=load_config(root).get("docs",{}).get("dir","docs")
    docs_root=root/docs_name
    mapping=stage_docs_mapping(root)
    synced=[]
    for st in wf.get("stages",[]):
        sid=st["id"]
        if not condition_enabled(st, spec):
            continue
        rel_target_tmpl=mapping.get(sid)
        if not rel_target_tmpl:
            continue
        ap=artifact_path(root, change_id, st)
        if not ap or not ap.exists():
            continue
        text=ap.read_text(encoding="utf-8")
        meta, body=parse_frontmatter(text)
        dest_meta={
            "origin_change": change_id,
            "change_type": spec.get("type", "feature"),
            "cabbage_stage": sid,
            "synced_at": now_iso(),
        }
        dest_text=f"---\n{yaml.safe_dump(dest_meta, sort_keys=False, allow_unicode=True)}---\n\n{body.lstrip()}"
        dest_path=docs_root/rel_target_tmpl.format(change_id=change_id)
        write_text_atomic(dest_path, dest_text, encoding="utf-8")
        synced.append(str(dest_path.relative_to(root)).replace("\\","/"))
    return synced

def validate_change(root: Path, change_id: str, verification: bool=False) -> list[str]:
    errors=[]
    spec=change_spec(root,change_id)
    if spec.get("id") != change_id: errors.append(f"change.yaml id must be `{change_id}`")
    if spec.get("status") not in {"active","archived"}: errors.append("change.yaml status must be active or archived")
    cfg=load_config(root); known=set(cfg.get("impact_fields",[])); actual=set(spec.get("impact",{}))
    missing=known-actual
    if missing: errors.append("missing impact fields: "+", ".join(sorted(missing)))
    wf,_=workflow(root,spec["type"])
    for st in wf.get("stages",[]):
        if condition_enabled(st,spec):
            errors.extend(validate_markdown(root,change_id,st,verification=verification))
    return errors

def gate(root: Path, change_id: str, target: str) -> list[str]:
    spec=change_spec(root,change_id); wf,_=workflow(root,spec["type"])
    statuses=stage_statuses(root,change_id)
    order=[x["id"] for x in statuses]
    if target == "implementation":
        if "implementation" not in order: return ["workflow has no implementation stage"]
        required=statuses[:order.index("implementation")]
    elif target in {"merge","archive"}:
        required=statuses
    else:
        return [f"unknown gate target: {target}"]
    return [f"{x['id']} is {x['status']}" for x in required if x["status"] not in {"done","skipped"}]

def git_changed_files(root: Path, base: str) -> list[str]:
    proc=subprocess.run(["git","diff","--name-only",f"{base}...HEAD"],cwd=root,text=True,capture_output=True)
    if proc.returncode != 0:
        raise CabbageError(proc.stderr.strip() or f"git diff failed against {base}")
    return [x.strip() for x in proc.stdout.splitlines() if x.strip()]

KNOWN_EXCLUDE_FILES = {
    "README.md", "README_zh.md", "LICENSE", "LICENSE.md", "LICENSE.txt",
    ".gitignore", ".gitattributes", ".editorconfig", ".prettierrc", ".prettierignore",
    ".eslintignore", ".eslintrc", "package.json", "pnpm-lock.yaml", "pnpm-workspace.yaml",
    "requirements.txt", "pyproject.toml", "Makefile", "Dockerfile", "docker-compose.yml"
}

def is_code_change(path: str, cfg: dict) -> bool:
    normalized=path.replace("\\","/")
    excludes=tuple(cfg.get("ci",{}).get("exclude_prefixes",["docs/",".cabbage/",".github/"]))
    if any(normalized.startswith(x) for x in excludes): return False
    filename = Path(normalized).name
    if filename in KNOWN_EXCLUDE_FILES: return False
    if normalized.endswith((".md",".mdx",".txt")): return False
    return True

def ci_check(root: Path, base: str) -> list[str]:
    cfg=load_config(root); files=git_changed_files(root,base); errors=[]
    changed_changes=set(); archived_changes=set()
    rx=re.compile(r"^\.cabbage/changes/([^/]+)/")
    arx=re.compile(r"^\.cabbage/archive/[^/]+/([^/]+)/")
    for f in files:
        m=rx.match(f)
        if m: changed_changes.add(m.group(1))
        am=arx.match(f)
        if am: archived_changes.add(am.group(1))
    code=[f for f in files if is_code_change(f,cfg)]
    existing_changes={cid for cid in changed_changes if change_dir(root,cid).exists()}
    if code and cfg.get("ci",{}).get("require_change_for_code",True) and not existing_changes:
        errors.append("code changed but no active .cabbage/changes/<id>/ entry changed")
    for cid in sorted(changed_changes-existing_changes):
        if cid not in archived_changes:
            errors.append(f"change record `{cid}` was removed from active changes; use `cabbage archive` rather than deleting workflow history")
    for cid in sorted(existing_changes):
        errors.extend(f"{cid}: {e}" for e in validate_change(root,cid))
        errors.extend(f"{cid}: merge gate: {e}" for e in gate(root,cid,"merge"))
        if cfg.get("ci",{}).get("require_current_state_docs",True):
            spec=change_spec(root,cid)
            rules=cfg.get("ci",{}).get("current_state_rules",{})
            for area,prefixes in rules.items():
                if spec.get("impact",{}).get(area,False) and not any(any(f.startswith(prefix) for prefix in prefixes) for f in files):
                    errors.append(f"{cid}: impact `{area}=true` requires a current-state docs change under: {', '.join(prefixes)}")
    return errors

def run(cmd: list[str], cwd: Path) -> int:
    return subprocess.call(cmd,cwd=cwd)

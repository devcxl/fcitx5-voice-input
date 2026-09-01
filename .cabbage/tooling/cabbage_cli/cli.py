from __future__ import annotations
import argparse, json, os, shutil, subprocess, sys
from pathlib import Path
from . import __version__
from .core import *
from .scaffold import init_project, new_change, ensure_artifacts, adopt_project, discard_change

def emit(data, as_json=False):
    if as_json: print(json.dumps(data,ensure_ascii=False,indent=2))
    elif isinstance(data,str): print(data)
    else:
        for x in data: print(x)

def cmd_init(a):
    root=Path.cwd().resolve(); init_project(root,a.force,not a.no_vendor_cli)
    print("initialized cabbage")
    print("next: cabbage new feature <change-id>")
    print("existing docs to onboard? run: cabbage adopt")

def cmd_adopt(a):
    root=project_root(); data=adopt_project(root, apply=getattr(a, "apply", False))
    if a.json: emit(data,True); return 0
    if data.get("applied"):
        print(f"migrated {len(data['applied'])} document(s):")
        for item in data["applied"]:
            print(f"  {item['from']}  ->  {item['to']}")
    counts=data["counts"]
    print(f"adoption report written to {data['report']}")
    print(f"documents: {sum(counts.values())} total; keep={counts['keep']} migrate={counts['migrate']} import={counts['import']} review={counts['review']}")
    for r in data["documents"]:
        if r["action"] in {"migrate","import","review"}:
            target=r["target"] or "-"
            print(f"  {r['action']:8} {r['path']}  ->  {target}")
    print("next: resolve review rows, then follow references/adoption.md")
    return 0

def cmd_discard(a):
    root=project_root(); discard_change(root, a.change)
    print(f"discarded change `{a.change}`")
    return 0

def cmd_doctor(a):
    results = {}
    # 1. Python
    py_ok = sys.version_info >= (3, 10)
    results["python"] = {
        "ok": py_ok,
        "version": f"{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}",
        "message": "Python >= 3.10 required" if not py_ok else "OK",
    }
    # 2. PyYAML
    try:
        import yaml
        results["pyyaml"] = {"ok": True, "version": getattr(yaml, "__version__", "unknown")}
    except ImportError:
        results["pyyaml"] = {"ok": False, "message": "PyYAML is not installed"}

    # 3. Git
    git_which = shutil.which("git")
    if git_which:
        try:
            p = subprocess.run(["git", "--version"], capture_output=True, text=True)
            results["git"] = {"ok": True, "version": p.stdout.strip()}
        except Exception as e:
            results["git"] = {"ok": False, "message": str(e)}
    else:
        results["git"] = {"ok": False, "message": "git executable not found in PATH"}

    # 4. pnpm
    pnpm_which = shutil.which("pnpm")
    if pnpm_which:
        try:
            p = subprocess.run(["pnpm", "--version"], capture_output=True, text=True)
            results["pnpm"] = {"ok": True, "version": p.stdout.strip()}
        except Exception as e:
            results["pnpm"] = {"ok": False, "message": str(e)}
    else:
        results["pnpm"] = {"ok": True, "version": "not installed (optional for docs dev/build)"}

    # 5. Cabbage Project
    try:
        r = project_root()
        cfg = load_config(r)
        results["project"] = {"ok": True, "path": str(r), "version": cfg.get("version", 1)}
    except Exception as e:
        results["project"] = {"ok": False, "message": str(e)}

    all_ok = results["python"]["ok"] and results["pyyaml"]["ok"] and results["git"]["ok"]
    if a.json:
        emit({"ok": all_ok, "checks": results}, True)
    else:
        print("Cabbage Doctor Diagnosis:")
        for name, info in results.items():
            status = "✓" if info["ok"] else "✗"
            ver = f"({info.get('version', '')})" if info.get('version') else ""
            msg = f"- {info.get('message')}" if not info["ok"] or 'message' in info else ""
            print(f"  {status} {name:10} {ver} {msg}".rstrip())
        if all_ok:
            print("\nAll essential dependencies and environment checks passed.")
        else:
            print("\nSome environment checks failed.")
    return 0 if all_ok else 1

def cmd_new(a):
    root=project_root(); new_change(root,a.type,a.change); print(f"created {a.change}"); cmd_status(argparse.Namespace(change=a.change,json=False))

def cmd_status(a):
    root=project_root()
    if a.change:
        items=stage_statuses(root,a.change)
        if a.json: emit({"change":a.change,"stages":items},True)
        else:
            for x in items: print(f"{x['status']:7} {x['id']:16} {x.get('artifact') or ''}")
    else:
        dirs=root/".cabbage/changes"; rows=[]
        for p in sorted(dirs.iterdir() if dirs.exists() else []):
            if p.is_dir() and (p/"change.yaml").exists():
                st=stage_statuses(root,p.name); rows.append({"change":p.name,"done":sum(x['status']=='done' for x in st),"total":sum(x['status']!='skipped' for x in st),"stale":sum(x['status']=='stale' for x in st)})
        emit(rows,a.json)

def cmd_next(a):
    root=project_root(); sts=stage_statuses(root,a.change); status={x['id']:x['status'] for x in sts}; spec=change_spec(root,a.change); wf,_=workflow(root,spec['type']); sm=stage_map(wf)
    ready=[]
    for x in sts:
        if x['status'] not in {'pending','stale'}: continue
        deps=[d for d in sm[x['id']].get('depends_on',[]) if d in sm and condition_enabled(sm[d],spec)]
        if all(status.get(d)=='done' for d in deps): ready.append(x)
    data={"change":a.change,"ready":ready,"blocked":[x for x in sts if x['status'] in {'pending','stale'} and x not in ready]}
    emit(data,a.json)
    return 0 if ready or all(x['status'] in {'done','skipped'} for x in sts) else 2

def cmd_impact(a):
    root=project_root(); p=change_dir(root,a.change)/"change.yaml"; spec=load_yaml(p); cfg=load_config(root)
    for pair in a.set or []:
        if '=' not in pair: raise CabbageError("--set expects field=true|false")
        k,v=pair.split('=',1)
        if k not in cfg.get('impact_fields',[]): raise CabbageError(f"unknown impact field: {k}")
        if v.lower() not in {'true','false'}: raise CabbageError("impact value must be true or false")
        spec.setdefault('impact',{})[k]=v.lower()=='true'
    if a.set:
        dump_yaml(p,spec); ensure_artifacts(root,a.change)
    emit({"change":a.change,"impact":spec.get('impact',{})},a.json)

def cmd_validate(a):
    root=project_root(); errors=[]
    ids=[]
    if a.all:
        ids=[p.name for p in (root/'.cabbage/changes').iterdir() if p.is_dir() and (p/'change.yaml').exists()]
    elif a.change: ids=[a.change]
    else: raise CabbageError("provide a change id or --all")
    for cid in ids: errors.extend(f"{cid}: {e}" for e in validate_change(root,cid))
    if a.json: emit({"ok":not errors,"errors":errors},True)
    elif errors:
        print("INVALID"); [print(f"- {e}") for e in errors]
    else: print("VALID")
    return 1 if errors else 0

def cmd_verify(a):
    root=project_root(); verify_stage(root,a.change,a.stage); print(f"verified {a.change}:{a.stage}")

def cmd_sync(a):
    root=project_root(); synced=sync_change_to_docs(root,a.change)
    if a.json: emit({"change":a.change,"synced":synced},True)
    else:
        if synced:
            print(f"synced {len(synced)} document(s) to current-state docs:")
            for path in synced: print(f"  {path}")
        else:
            print("no documents to sync")

def cmd_gate(a):
    root=project_root(); errors=gate(root,a.change,a.target)
    if a.json: emit({"ok":not errors,"errors":errors},True)
    elif errors:
        print("BLOCKED"); [print(f"- {e}") for e in errors]
    else: print("ALLOWED")
    return 1 if errors else 0

def cmd_archive(a):
    root=project_root(); errors=gate(root,a.change,'archive')
    if errors: raise CabbageError("archive gate blocked:\n"+'\n'.join(errors))
    synced=sync_change_to_docs(root,a.change)
    d=change_dir(root,a.change); spec=load_yaml(d/'change.yaml'); spec['status']='archived'; dump_yaml(d/'change.yaml',spec)
    from datetime import datetime
    dest=root/'.cabbage/archive'/datetime.now().strftime('%Y')/a.change; dest.parent.mkdir(parents=True,exist_ok=True)
    if dest.exists(): raise CabbageError(f"archive destination exists: {dest}")
    shutil.move(str(d),str(dest))
    if synced:
        print(f"synced {len(synced)} document(s) to current-state docs")
    print(f"archived to {dest.relative_to(root)}")

def cmd_ci(a):
    root=project_root(); errors=ci_check(root,a.base)
    if errors:
        print("CABBAGE CI FAILED"); [print(f"- {e}") for e in errors]; return 1
    print("CABBAGE CI PASSED"); return 0

def cmd_docs(a):
    root=project_root(); docs=root/load_config(root).get('docs',{}).get('dir','docs')
    if a.action=='install': cmd=['pnpm','install']
    elif a.action=='dev': cmd=['pnpm','run','dev']
    else: cmd=['pnpm','run','build']
    if shutil.which('pnpm') is None: raise CabbageError('pnpm not found')
    return subprocess.call(cmd,cwd=docs)

def parser():
    p=argparse.ArgumentParser(prog='cabbage'); p.add_argument('--version',action='version',version=__version__); sp=p.add_subparsers(dest='cmd',required=True)
    x=sp.add_parser('init'); x.add_argument('--force',action='store_true'); x.add_argument('--no-vendor-cli',action='store_true'); x.set_defaults(func=cmd_init)
    x=sp.add_parser('adopt'); x.add_argument('--apply',action='store_true'); x.add_argument('--json',action='store_true'); x.set_defaults(func=cmd_adopt)
    x=sp.add_parser('doctor'); x.add_argument('--json',action='store_true'); x.set_defaults(func=cmd_doctor)
    x=sp.add_parser('new'); x.add_argument('type'); x.add_argument('change'); x.set_defaults(func=cmd_new)
    x=sp.add_parser('discard'); x.add_argument('change'); x.set_defaults(func=cmd_discard)
    x=sp.add_parser('status'); x.add_argument('change',nargs='?'); x.add_argument('--json',action='store_true'); x.set_defaults(func=cmd_status)
    x=sp.add_parser('next'); x.add_argument('change'); x.add_argument('--json',action='store_true'); x.set_defaults(func=cmd_next)
    x=sp.add_parser('impact'); x.add_argument('change'); x.add_argument('--set',action='append'); x.add_argument('--json',action='store_true'); x.set_defaults(func=cmd_impact)
    x=sp.add_parser('validate'); x.add_argument('change',nargs='?'); x.add_argument('--all',action='store_true'); x.add_argument('--json',action='store_true'); x.set_defaults(func=cmd_validate)
    x=sp.add_parser('verify'); x.add_argument('change'); x.add_argument('stage'); x.set_defaults(func=cmd_verify)
    x=sp.add_parser('sync'); x.add_argument('change'); x.add_argument('--json',action='store_true'); x.set_defaults(func=cmd_sync)
    x=sp.add_parser('gate'); x.add_argument('change'); x.add_argument('target',choices=['implementation','merge','archive']); x.add_argument('--json',action='store_true'); x.set_defaults(func=cmd_gate)
    x=sp.add_parser('archive'); x.add_argument('change'); x.set_defaults(func=cmd_archive)
    x=sp.add_parser('ci'); x.add_argument('--base',required=True); x.set_defaults(func=cmd_ci)
    x=sp.add_parser('docs'); x.add_argument('action',choices=['install','dev','build']); x.set_defaults(func=cmd_docs)
    return p

def main(argv=None):
    try:
        a=parser().parse_args(argv); return a.func(a) or 0
    except CabbageError as e:
        print(f"cabbage: {e}",file=sys.stderr); return 2
    except KeyboardInterrupt:
        print("\naborted by user",file=sys.stderr); return 130

if __name__=='__main__': raise SystemExit(main())

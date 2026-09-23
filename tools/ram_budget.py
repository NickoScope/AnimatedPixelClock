#!/usr/bin/env python3
"""Hold the line on static internal RAM (AGENTS.md section 7).

Internal SRAM is the radio's memory: the Wi-Fi driver takes its 1,626 B receive
buffers from the internal DMA-capable heap, and on 2.5.5 that pool fell to 172 B
in ordinary running and the panel lost the network. 2.5.6 moved 34.8 KB of page
and effect state to PSRAM (src/util/psram_state.h) and the pool went to 47-50 KB
free. This check keeps it from creeping back one array at a time.

It reads the Waveshare build's ELF and fails when
  - .dram0.bss + .dram0.data grew past the budget in tools/ram_budget.json
    plus its slack (this total covers everything, lib/ and the framework too), or
  - our code (the object files of src/, not lib/) has a static object of
    big_symbol_bytes or more in internal RAM that is not on the budget's list of
    ones that must stay there.
The fix is nearly always PSRAM_ARRAY() or a heap_caps_malloc(..., MALLOC_CAP_SPIRAM)
at boot. Only what DMA, an interrupt or a task stack touches has to be internal;
such an object goes on the list with its reason:

    python3 tools/ram_budget.py --allow "net/net_broker:(anonymous namespace)::s_stack" --reason "task stack"
    python3 tools/ram_budget.py --update --reason "library bump to X"   # new total, recorded

The pre-commit hook and release.py build first and pass --just-built. Run by
hand, an ELF older than a source file is reported, not refused: touching a file
(a checkout, a README) does not make SCons relink, so the age alone proves
nothing.
"""
import argparse, datetime, json, os, pathlib, re, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUDGET = ROOT / "tools" / "ram_budget.json"


def tool(name):
    for base in (pathlib.Path.home() / ".platformio" / "packages" / "toolchain-xtensa-esp32s3" / "bin",):
        p = base / f"xtensa-esp32s3-elf-{name}"
        if p.exists():
            return str(p)
    sys.exit(f"ram_budget: xtensa-esp32s3-elf-{name} not found (PlatformIO's toolchain-xtensa-esp32s3)")


def dram_total(elf):
    out = subprocess.run([tool("size"), "-A", str(elf)], capture_output=True, text=True, check=True).stdout
    sizes = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 2 and p[0] in (".dram0.bss", ".dram0.data") and p[1].isdigit():
            sizes[p[0]] = int(p[1])
    return sizes


def our_internal_symbols(build, minimum):
    """Our static objects in internal RAM: .bss/.data/.noinit of src/ objects."""
    found = {}
    for obj in sorted((build / "src").rglob("*.o")):
        # Only objects whose source still exists: a deleted or renamed file's
        # stale .o would otherwise keep reporting (and keep its list entry alive).
        if not (ROOT / "src" / str(obj.relative_to(build / "src"))[:-2]).exists():
            continue
        out = subprocess.run([tool("objdump"), "-t", "-C", str(obj)], capture_output=True, text=True).stdout
        for line in out.splitlines():
            # value flags section size name
            m = re.match(r"^([0-9a-f]+)\s+.{7}\s+(\S+)\s+([0-9a-f]+)\s+(.+)$", line)
            if not m:
                continue
            section, name = m.group(2), m.group(4).strip()
            # A common symbol (an uninitialised global in a .c file, gcc 8 without
            # -fno-common) has no section yet: its size is in the value column and
            # the size column holds its alignment. It lands in .bss at link time.
            if section == "*COM*":
                size = int(m.group(1), 16)
            elif re.match(r"^\.(bss|data|noinit|dram)", section):
                size = int(m.group(3), 16)
            else:
                continue
            if size < minimum:
                continue
            rel = str(obj.relative_to(build / "src")).removesuffix(".cpp.o").removesuffix(".c.o")
            found[f"{rel}:{name}"] = size
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--update", action="store_true", help="take the current total as the budget")
    ap.add_argument("--allow", help="put this symbol key on the must-stay-internal list")
    ap.add_argument("--reason", help="why (required with --update and --allow)")
    ap.add_argument("--just-built", action="store_true", help="the caller has just run pio run: skip the age check")
    a = ap.parse_args()
    b = json.loads(BUDGET.read_text())
    build = ROOT / ".pio" / "build" / b["env"]
    elf = build / "firmware.elf"
    if not elf.exists():
        sys.exit(f"ram_budget: no {elf.relative_to(ROOT)} - run  pio run -e {b['env']}")
    if not a.just_built:
        code = (".c", ".cpp", ".h", ".hpp", ".S")
        newest = max((p.stat().st_mtime for p in (ROOT / "src").rglob("*") if p.suffix in code), default=0)
        if elf.stat().st_mtime < newest:
            print(f"ram_budget: note - {elf.relative_to(ROOT)} is older than a source file; if you changed code, "
                  f"run  pio run -e {b['env']}  first")

    sizes = dram_total(elf)
    total = sizes.get(".dram0.bss", 0) + sizes.get(".dram0.data", 0)
    syms = our_internal_symbols(build, b["big_symbol_bytes"])
    today = datetime.date.today().isoformat()

    if a.update or a.allow:
        if not a.reason:
            sys.exit("ram_budget: say why with --reason")
        if a.allow:
            if a.allow not in syms:
                sys.exit(f"ram_budget: {a.allow} is not an internal symbol of {b['big_symbol_bytes']} B or more in this build")
            b["internal_allowed"][a.allow] = a.reason
        if a.update:
            b["history"].append({"date": today, "from": b["dram_bytes"], "to": total, "reason": a.reason})
            b["dram_bytes"] = total
        BUDGET.write_text(json.dumps(b, indent=2, ensure_ascii=False) + "\n")
        print(f"ram_budget: {BUDGET.relative_to(ROOT)} updated - stage it with the change")
        return 0

    fails = []
    limit = b["dram_bytes"] + b["slack_bytes"]
    if total > limit:
        fails.append(f"static internal RAM is {total:,} B (.dram0.bss {sizes.get('.dram0.bss', 0):,} + .dram0.data "
                     f"{sizes.get('.dram0.data', 0):,}), over the budget {b['dram_bytes']:,} + slack {b['slack_bytes']:,}")
    new = {k: v for k, v in syms.items() if k not in b["internal_allowed"]}
    for k, v in sorted(new.items(), key=lambda kv: -kv[1]):
        fails.append(f"{v:,} B in internal RAM, not on the list: {k}")
    if fails:
        print("ram_budget: FAIL")
        for f in fails:
            print("  " + f)
        print("\n  Page and effect state belongs in PSRAM: PSRAM_ARRAY() in src/util/psram_state.h, or")
        print("  heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) at boot. Only what DMA, an")
        print("  interrupt or a task stack touches must be internal: --allow it with --reason.")
        print("  A real, reasoned rise of the total: --update --reason. Rules: AGENTS.md section 7.")
        return 1
    gone = [k for k in b["internal_allowed"] if k not in syms]
    print(f"ram_budget: ok - static internal RAM {total:,} B of {b['dram_bytes']:,} (+{b['slack_bytes']:,} slack); "
          f"{len(syms)} objects of {b['big_symbol_bytes']} B or more, all on the list"
          + (f"; {len(gone)} listed no longer present" if gone else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())

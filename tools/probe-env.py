"""What this machine can do, before a template is chosen.

The skill routes on capability: a CUDA template needs a compiler and a device, the
inference template needs an ONNX Runtime tree, the Resolve suite needs a Studio
seat with external scripting. Asking a person is how a run starts with the wrong
template and ends with a build error two steps later, so this tool asks the
machine and writes down what it answered.

Three rules, because a probe that guesses is worse than no probe:

* **Every fact carries the command that produced it** (`sources`), or `null` and a
  reason in `unknown`. Nothing is inferred from a path that merely looks right.
* **`null` is not `false`.** A capability that could not be probed - Resolve
  Studio without `--connect`, external scripting while the host is closed - stays
  `null` and blocks the suites that need it, instead of passing them.
* **The decision logic is testable on its own** (`--self-test`), because the
  interesting failure is a machine that reports "ready" for something it cannot
  build.

Usage:

    python3 tools/probe-env.py                  # human summary
    python3 tools/probe-env.py --json           # the same, as a machine contract
    python3 tools/probe-env.py --connect        # also ask a running Resolve who it is
    python3 tools/probe-env.py --need compile.cuda,run.cuda   # exit 1 unless both

Exit codes: 0 report written, 1 a `--need` capability is not satisfied, 2 usage.
"""

import argparse
import importlib.metadata
import json
import os
import platform
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

# Which repository suite needs which capability. This table is the point of the
# tool: a capability nobody consumes is trivia, and a suite whose requirement is
# not listed is one somebody will run blind.
SUITES = (
    ("templates/minimal-ofx/test/run-tests", ("compile.cpu",)),
    ("templates/minimal-cuda-ofx/test/run-tests (CPU_ONLY=1)", ("compile.cpu",)),
    ("templates/minimal-cuda-ofx/test/gpu-kernel-test.cu", ("compile.cuda", "run.cuda")),
    ("templates/onnx-inference-ofx/test/run-tests", ("compile.cpu", "build.onnx")),
    ("tests/resolve/poc.py --build-project",
     ("host.resolve", "host.resolve.studio", "host.resolve.scripting", "decode.image")),
)


def run(command, timeout=25):
    """(ok, output, command) - never raises, and always names what it ran."""
    shown = " ".join(str(part) for part in command)
    try:
        finished = subprocess.run(command, capture_output=True, text=True,
                                  timeout=timeout)
        output = ((finished.stdout or "") + (finished.stderr or "")).strip()
        return finished.returncode == 0, output, shown
    except Exception as error:              # missing binary, timeout, permissions
        return False, f"{type(error).__name__}: {error}", shown


def first_line(text):
    return text.strip().splitlines()[0].strip() if text and text.strip() else ""


def parse_nvcc(text):
    """The CUDA release out of `nvcc --version`, or None.

    The line reads `Cuda compilation tools, release 12.9, V12.9.41`; older
    toolkits spell it the same way, which is why the number is taken from
    `release` and not from the `V` token.
    """
    for line in text.splitlines():
        marker = "release "
        index = line.find(marker)
        if index < 0:
            continue
        tail = line[index + len(marker):].strip()
        version = tail.split(",")[0].strip()
        if version and version[0].isdigit():
            return version
    return None


def parse_gpu_csv(text):
    """`nvidia-smi --format=csv,noheader` rows into dicts.

    `compute_cap` is only asked for when the driver supports the field, so the
    parser has to survive both shapes rather than assume four columns.
    """
    gpus = []
    for line in text.splitlines():
        parts = [part.strip() for part in line.split(",")]
        if len(parts) < 3 or not parts[0]:
            continue
        gpu = {"name": parts[0], "driver": parts[1], "memory": parts[2]}
        if len(parts) >= 4 and parts[3]:
            gpu["compute_cap"] = parts[3]
        gpus.append(gpu)
    return gpus


def evaluate(capabilities, suites=SUITES):
    """Each suite's status from the capability table: ready, blocked or unknown.

    `unknown` is deliberately distinct from `blocked`: a machine that cannot tell
    whether Resolve is Studio has not shown that it is not.

    A capability name that does not exist is an error, not an `unknown`: a typo in
    `SUITES` would otherwise block nothing and read as "could not probe", which is
    the one failure this table exists to prevent.
    """
    unknown_needs = sorted({need for _, needs in suites for need in needs
                            if need not in capabilities})
    if unknown_needs:
        raise KeyError("SUITES names capabilities that do not exist: "
                       + ", ".join(unknown_needs))
    report = {}
    for name, needs in suites:
        missing = [need for need in needs if capabilities[need] is False]
        unprobed = [need for need in needs if capabilities[need] is None]
        entry = {"needs": list(needs)}
        if missing:
            entry.update({"status": "blocked", "missing": missing})
        elif unprobed:
            entry.update({"status": "unknown", "unprobed": unprobed})
        else:
            entry["status"] = "ready"
        report[name] = entry
    return report


def probe_platform():
    info = {"system": platform.system(), "release": platform.release(),
            "version": platform.version(), "machine": platform.machine()}
    if info["system"] == "Windows":
        # platform.release() answers "10" on Windows 11 as well; the build number
        # is the only honest discriminator (22000 and up is 11 for client SKUs).
        try:
            build = int(info["version"].split(".")[2])
            info["name"] = f"Windows {11 if build >= 22000 else 10} (build {build})"
        except (IndexError, ValueError):
            info["name"] = "Windows " + info["version"]
    if info["system"] == "Linux":
        release = Path("/etc/os-release")
        if release.is_file():
            for line in release.read_text(errors="replace").splitlines():
                if line.startswith("PRETTY_NAME="):
                    info["distro"] = line.split("=", 1)[1].strip('"')
    return info


def probe_python():
    packages = {}
    for name in ("numpy", "onnx", "onnxruntime"):
        try:
            packages[name] = importlib.metadata.version(name)
        except Exception:
            packages[name] = None
    return {"executable": sys.executable, "version": platform.python_version(),
            "bits": 64 if sys.maxsize > 2 ** 32 else 32,
            "venv": sys.prefix != sys.base_prefix, "packages": packages}


def probe_tools(sources):
    tools = {}
    for name, arguments in (("cmake", ["--version"]), ("ninja", ["--version"]),
                            ("gcc", ["--version"]), ("g++", ["--version"]),
                            ("clang", ["--version"]), ("clang++", ["--version"]),
                            ("ffmpeg", ["-version"]), ("ffprobe", ["-version"])):
        path = shutil.which(name)
        if not path:
            tools[name] = None
            continue
        ok, output, command = run([path] + arguments)
        sources[f"tools.{name}"] = command
        version = first_line(output) if ok else None
        if version and name.startswith("ff"):
            # ffmpeg's first line carries a build id, a URL and a copyright; the
            # first three tokens are the part a report can use.
            version = " ".join(version.split()[:3])
        tools[name] = {"path": path, "version": version}

    if os.name == "nt":
        vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) \
            / "Microsoft Visual Studio/Installer/vswhere.exe"
        if vswhere.is_file():
            ok, output, command = run([
                str(vswhere), "-latest", "-products", "*",
                "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                "-property", "installationVersion"])
            sources["tools.msvc"] = command
            if ok and output.strip():
                version = output.strip().splitlines()[0]
                ok_path, path_output, command_path = run([
                    str(vswhere), "-latest", "-products", "*",
                    "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                    "-property", "installationPath"])
                sources["tools.msvc.path"] = command_path
                tools["msvc"] = {"version": version,
                                 "path": path_output.strip().splitlines()[0]
                                 if ok_path and path_output.strip() else None}
            else:
                tools["msvc"] = None
        else:
            tools["msvc"] = None
    return tools


def probe_cuda(sources):
    """Every toolkit that has an `nvcc`, plus the device the driver reports."""
    candidates = []
    for variable in ("CUDA_HOME", "CUDA_PATH"):
        configured = os.environ.get(variable)
        if configured:
            candidates.append(Path(configured))
    if os.name == "nt":
        base = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) \
            / "NVIDIA GPU Computing Toolkit/CUDA"
        if base.is_dir():
            candidates.extend(sorted(base.glob("v*")))
    else:
        candidates.append(Path("/usr/local/cuda"))
        if Path("/usr/local").is_dir():
            candidates.extend(sorted(Path("/usr/local").glob("cuda-*")))

    toolkits, seen = [], set()
    for root in candidates:
        nvcc = root / ("bin/nvcc.exe" if os.name == "nt" else "bin/nvcc")
        if not nvcc.is_file() or str(root) in seen:
            continue
        seen.add(str(root))
        ok, output, command = run([str(nvcc), "--version"])
        sources[f"cuda.toolkit:{root.name}"] = command
        toolkits.append({"path": str(root), "nvcc": str(nvcc),
                         "version": parse_nvcc(output) if ok else None})

    ok, output, command = run(["nvidia-smi", "--query-gpu=name,driver_version,"
                               "memory.total,compute_cap", "--format=csv,noheader"])
    sources["cuda.gpus"] = command
    if not ok:
        ok, output, command = run(["nvidia-smi", "--query-gpu=name,driver_version,"
                                   "memory.total", "--format=csv,noheader"])
        sources["cuda.gpus"] = command + "   (compute_cap field unsupported)"
    gpus = parse_gpu_csv(output) if ok else []

    # The toolkit `nvcc` on PATH wins, because that is the one a bare `nvcc`
    # invocation would use; otherwise the newest version found.
    on_path = shutil.which("nvcc")
    active = None
    if on_path:
        for toolkit in toolkits:
            if toolkit["nvcc"] == on_path:
                active = toolkit["version"]
        if active is None:
            ok, output, command = run([on_path, "--version"])
            sources["cuda.nvcc"] = command
            active = parse_nvcc(output) if ok else None
    if active is None and toolkits:
        active = max((t["version"] for t in toolkits if t["version"]), default=None)

    return {"toolkits": toolkits, "active": active, "gpus": gpus,
            "nvcc_on_path": on_path}


def probe_resolve(sources):
    candidates = []
    configured = os.environ.get("RESOLVE_EXE")
    if configured:
        candidates.append(Path(configured))
    for name in ("Resolve", "resolve"):
        found = shutil.which(name)
        if found:
            candidates.append(Path(found))
    if os.name == "nt":
        # The install path is the one thing that is never the same twice: it sat
        # on E: on the machine this was written on, and neither the registry nor
        # PATH named it. Scanning the default location per drive is what found it.
        for letter in "CDEFGHIJKLMNOPQRSTUVWXYZ":
            drive = Path(f"{letter}:/")
            if not drive.exists():
                continue
            candidates.append(drive / "Program Files/Blackmagic Design/"
                                      "DaVinci Resolve/Resolve.exe")
    elif sys.platform == "darwin":
        candidates.append(Path("/Applications/DaVinci Resolve/DaVinci Resolve.app/"
                               "Contents/MacOS/Resolve"))
    else:
        candidates.append(Path("/opt/resolve/bin/resolve"))

    executable = next((path for path in candidates if path.is_file()), None)
    sources["resolve.executable"] = ("RESOLVE_EXE, PATH, then the default install "
                                     "location on each drive")

    if os.name == "nt":
        sdk = Path(os.environ.get("PROGRAMDATA", r"C:\ProgramData")) / \
            "Blackmagic Design/DaVinci Resolve/Support/Developer/Scripting"
    elif sys.platform == "darwin":
        sdk = Path("/Library/Application Support/Blackmagic Design/DaVinci Resolve/"
                   "Developer/Scripting")
    else:
        sdk = Path("/opt/resolve/Developer/Scripting")
    configured_sdk = os.environ.get("RESOLVE_SCRIPT_API")
    if configured_sdk and Path(configured_sdk).is_dir():
        sdk = Path(configured_sdk)
    if not sdk.is_dir():
        sdk = None
    sources["resolve.sdk"] = "RESOLVE_SCRIPT_API, else the documented location"

    library = None
    configured_library = os.environ.get("RESOLVE_SCRIPT_LIB")
    if configured_library and Path(configured_library).is_file():
        library = Path(configured_library)
    elif executable:
        beside = executable.parent / ("fusionscript.dll" if os.name == "nt"
                                      else "fusionscript.so")
        if beside.is_file():
            library = beside
    sources["resolve.library"] = "RESOLVE_SCRIPT_LIB, else beside the executable"

    return {"installed": executable is not None,
            "executable": str(executable) if executable else None,
            "sdk": str(sdk) if sdk else None,
            "library": str(library) if library else None,
            "studio": None, "product": None, "version": None,
            "scripting": None}


def connect(resolve):
    """Ask a running host who it is. Does not start one: that costs 18 s and a GPU."""
    if not resolve["sdk"]:
        return {"connected": False,
                "reason": "no SDK module (RESOLVE_SCRIPT_API and the documented "
                          "location are both absent)"}
    if resolve["library"]:
        os.environ.setdefault("RESOLVE_SCRIPT_LIB", resolve["library"])
    modules = str(Path(resolve["sdk"]) / "Modules")
    if modules not in sys.path:
        sys.path.insert(0, modules)
    try:
        import DaVinciResolveScript as dvr
    except Exception as error:
        return {"connected": False, "reason": f"{type(error).__name__}: {error}"}
    try:
        application = dvr.scriptapp("Resolve")
    except Exception as error:
        return {"connected": False, "reason": f"scriptapp raised "
                                              f"{type(error).__name__}: {error}"}
    if not application:
        return {"connected": False,
                "reason": "scriptapp returned None: the host is not running, it is "
                          "the free version (no listener), or External scripting "
                          "is None"}
    answer = {"connected": True, "product": application.GetProductName(),
              "version": application.GetVersionString()}
    try:
        answer["studio"] = bool(application.IsStudio())
    except Exception:
        answer["studio"] = None          # older API without IsStudio
    return answer


def probe_ofx(sources):
    directories = []
    if os.name == "nt":
        directories.append(Path(os.environ.get("COMMONPROGRAMFILES",
                                               r"C:\Program Files\Common Files")) / "OFX/Plugins")
    else:
        directories.extend([Path("/usr/OFX/Plugins"), Path("/Library/OFX/Plugins")])
        home = os.environ.get("HOME")
        if home:
            directories.append(Path(home) / ".OFX/Plugins")
    found = []
    for directory in directories:
        if not directory.is_dir():
            continue
        bundles = sorted(path.name for path in directory.glob("*.ofx.bundle"))
        found.append({"path": str(directory), "bundles": len(bundles)})
    sources["ofx.dirs"] = "the platform's standard OFX plugin directories"
    return {"directories": found}


def probe_onnx(sources):
    root = os.environ.get("ONNXRUNTIME_ROOT")
    if not root:
        sources["onnx.root"] = "ONNXRUNTIME_ROOT is not set"
        return {"root": None, "headers": False, "api_version": None, "libraries": []}
    base = Path(root)
    header = base / "include/onnxruntime_cxx_api.h"
    sources["onnx.root"] = "ONNXRUNTIME_ROOT"
    api_version = None
    api_header = base / "include/onnxruntime_c_api.h"
    if api_header.is_file():
        for line in api_header.read_text(errors="replace").splitlines():
            if line.strip().startswith("#define ORT_API_VERSION"):
                api_version = line.split()[-1]
    libraries = sorted(path.name for path in (base / "lib").glob("onnxruntime*")
                       ) if (base / "lib").is_dir() else []
    if not libraries and (base / "lib").is_dir():
        libraries = sorted(path.name for path in (base / "lib").glob("*.so*"))
    return {"root": str(base), "headers": header.is_file(),
            "api_version": api_version, "libraries": libraries}


def capabilities_of(facts, connected):
    tools = facts["tools"]
    cuda = facts["cuda"]
    resolve = facts["resolve"]
    onnx = facts["onnx"]
    compiler = any(tools.get(name) for name in ("msvc", "gcc", "g++", "clang", "clang++"))
    scripting = None
    # A connect that never reached a host has answered nothing: both Resolve
    # capabilities stay unprobed. Reading `scripting` as False would report the
    # suite as `blocked`, which claims the machine cannot do it, when all that is
    # known is that nobody was listening.
    studio = None
    scripting = None
    if connected is not None and connected.get("connected"):
        scripting = True
        studio = connected.get("studio")
    return {
        "compile.cpu": bool(tools.get("cmake")) and compiler,
        "compile.cuda": any(toolkit["version"] for toolkit in cuda["toolkits"]),
        "run.cuda": bool(cuda["gpus"]),
        "host.resolve": resolve["installed"],
        "host.resolve.studio": studio,
        "host.resolve.scripting": scripting,
        "build.onnx": bool(onnx["headers"] and onnx["libraries"]),
        "decode.image": bool(tools.get("ffmpeg") and tools.get("ffprobe")),
    }


def collect(connect_host):
    sources = {}
    facts = {
        "platform": probe_platform(),
        "python": probe_python(),
        "tools": probe_tools(sources),
        "cuda": probe_cuda(sources),
        "resolve": probe_resolve(sources),
        "ofx": probe_ofx(sources),
        "onnx": probe_onnx(sources),
    }
    connected = connect(facts["resolve"]) if connect_host else None
    if connect_host:
        facts["resolve"].update({
            "studio": connected.get("studio"),
            "product": connected.get("product"),
            "version": connected.get("version"),
            "scripting": connected.get("connected"),
        })
        facts["resolve"]["connect"] = connected
    capabilities = capabilities_of(facts, connected)
    unknown = []
    unprobed_resolve = [name for name in ("host.resolve.studio",
                                          "host.resolve.scripting")
                        if capabilities[name] is None]
    if unprobed_resolve:
        reason = (facts["resolve"].get("connect") or {}).get("reason")
        if reason:
            detail = str(reason)
        elif connected is not None and connected.get("connected"):
            detail = "the host answered but its API has no IsStudio()"
        else:
            detail = ("pass --connect while Resolve runs (a free install has no "
                      "listener, so it cannot answer)")
        unknown.append(", ".join(unprobed_resolve) + ": " + detail)
    if capabilities["run.cuda"] is False and facts["cuda"]["nvcc_on_path"]:
        unknown.append("run.cuda: nvcc exists but nvidia-smi reported no device")
    return {"generated": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            "facts": facts, "capabilities": capabilities,
            "suites": evaluate(capabilities), "sources": sources, "unknown": unknown}


def verdict(capabilities):
    templates = []
    if capabilities["compile.cpu"]:
        templates.append("templates/minimal-ofx")
    if capabilities["compile.cuda"]:
        templates.append("templates/minimal-cuda-ofx"
                         + ("" if capabilities["run.cuda"] else " (CPU_ONLY=1)"))
    if capabilities["compile.cpu"] and capabilities["build.onnx"]:
        templates.append("templates/onnx-inference-ofx")
    return templates


def mark(value):
    return "ok" if value is True else ("--" if value is False else "??")


def status_mark(status):
    return {"ready": "ok", "blocked": "--", "unknown": "??"}.get(status, "--")


def show(report):
    facts = report["facts"]
    platform_line = facts["platform"].get("name") or (
        facts["platform"]["system"] + " " + facts["platform"]["release"])
    if facts["platform"].get("distro"):
        platform_line = facts["platform"]["distro"]
    python = facts["python"]
    print(f"environment probe ({report['generated']})")
    print(f"  platform        {platform_line} / {facts['platform']['machine']}")
    print(f"  python          {python['version']} ({python['bits']}-bit)"
          f"{', venv' if python['venv'] else ''}")
    print("  packages        " + ", ".join(
        f"{name} {version or 'missing'}" for name, version in python["packages"].items()))
    for name in ("cmake", "msvc", "gcc", "g++", "clang", "clang++", "ffmpeg"):
        tool = facts["tools"].get(name)
        print(f"  {name:<15} {tool['version'] if tool else 'not found'}")
    cuda = facts["cuda"]
    versions = ", ".join(toolkit["version"] or "unversioned"
                         for toolkit in cuda["toolkits"]) or "no nvcc found"
    print(f"  cuda toolkits   {versions}   (active: {cuda['active'] or 'none'})")
    if cuda["gpus"]:
        for gpu in cuda["gpus"]:
            capability = f", sm_{gpu['compute_cap'].replace('.', '')}" \
                if gpu.get("compute_cap") else ""
            print(f"  gpu             {gpu['name']}, driver {gpu['driver']}, "
                  f"{gpu['memory']}{capability}")
    else:
        print("  gpu             no device reported by nvidia-smi")
    resolve = facts["resolve"]
    print(f"  resolve         {resolve['executable'] or 'not installed'}")
    if resolve["installed"]:
        print(f"  resolve sdk     {resolve['sdk'] or 'missing'}")
        studio = resolve["studio"]
        if studio is True:
            answer = "yes"
        elif studio is False:
            answer = "no"
        else:
            answer = "unknown (pass --connect while Resolve runs)"
        attempt = resolve.get("connect") or {}
        if attempt and not attempt.get("connected"):
            answer = "unknown - " + str(attempt.get("reason"))
        print(f"  resolve studio  {answer}")
    ofx = facts["ofx"]["directories"]
    print("  ofx             " + (", ".join(
        f"{entry['bundles']} bundles in {entry['path']}" for entry in ofx) or "no scan directory"))
    onnx = facts["onnx"]
    print(f"  onnx runtime    {onnx['root'] or 'not found (set ONNXRUNTIME_ROOT)'}"
          + (f", ORT_API_VERSION {onnx['api_version']}" if onnx["api_version"] else ""))

    print("\ncapabilities")
    for name, value in report["capabilities"].items():
        print(f"  [{mark(value)}] {name}")
    print("\nwhat this machine can run")
    for name, entry in report["suites"].items():
        detail = ""
        if entry["status"] == "blocked":
            detail = "   missing: " + ", ".join(entry["missing"])
        elif entry["status"] == "unknown":
            detail = "   unprobed: " + ", ".join(entry["unprobed"])
        print(f"  [{status_mark(entry['status'])}] {name}{detail}")
    templates = verdict(report["capabilities"])
    print("\ntemplates that fit this machine")
    print("  " + (", ".join(templates) if templates else "none: no C++ toolchain found"))
    if report["unknown"]:
        print("\nnot answered here")
        for note in report["unknown"]:
            print(f"  - {note}")


def selftest():
    """The parsers and the decision logic, against canned inputs.

    The interesting case is the last group: a missing capability must block the
    suite that needs it, and an unprobed one must not read as ready.
    """
    cases = []

    def case(name, got, want):
        cases.append((name, got, want))

    case("nvcc 12.9", parse_nvcc("nvcc: NVIDIA (R) Cuda compiler\n"
                                "Cuda compilation tools, release 12.9, V12.9.41"),
         "12.9")
    case("nvcc 13.0", parse_nvcc("Cuda compilation tools, release 13.0, V13.0.48"),
         "13.0")
    case("nvcc 11.8 without V token", parse_nvcc("release 11.8"), "11.8")
    case("nvcc absent", parse_nvcc("nvcc: command not found"), None)

    case("gpu row with compute_cap",
         parse_gpu_csv("NVIDIA GeForce RTX 3060, 621.40, 12288 MiB, 8.6"),
         [{"name": "NVIDIA GeForce RTX 3060", "driver": "621.40",
           "memory": "12288 MiB", "compute_cap": "8.6"}])
    case("gpu row without compute_cap",
         parse_gpu_csv("Tesla T4, 470.82, 15360 MiB"),
         [{"name": "Tesla T4", "driver": "470.82", "memory": "15360 MiB"}])
    case("gpu row that is a message", parse_gpu_csv("No devices were found"), [])

    all_yes = {"compile.cpu": True, "compile.cuda": True, "run.cuda": True,
               "host.resolve": True, "host.resolve.studio": True,
               "host.resolve.scripting": True, "build.onnx": True,
               "decode.image": True}
    case("everything present, everything ready",
         {name: entry["status"] for name, entry in evaluate(all_yes).items()},
         {name: "ready" for name, _ in SUITES})

    no_cuda = dict(all_yes, **{"compile.cuda": False})
    case("no nvcc blocks the device suite only",
         {name: entry["status"] for name, entry in evaluate(no_cuda).items()},
         {"templates/minimal-ofx/test/run-tests": "ready",
          "templates/minimal-cuda-ofx/test/run-tests (CPU_ONLY=1)": "ready",
          "templates/minimal-cuda-ofx/test/gpu-kernel-test.cu": "blocked",
          "templates/onnx-inference-ofx/test/run-tests": "ready",
          "tests/resolve/poc.py --build-project": "ready"})

    unprobed = dict(all_yes, **{"host.resolve.studio": None})
    case("an unprobed Studio is unknown, not ready",
         evaluate(unprobed)["tests/resolve/poc.py --build-project"]["status"],
         "unknown")

    no_onnx = dict(all_yes, **{"build.onnx": False})
    case("no ONNX Runtime blocks the inference suite",
         evaluate(no_onnx)["templates/onnx-inference-ofx/test/run-tests"],
         {"needs": ["compile.cpu", "build.onnx"], "status": "blocked",
          "missing": ["build.onnx"]})

    def raised(function):
        try:
            function()
        except Exception as error:
            return type(error).__name__
        return None

    case("a suite naming an unknown capability is refused, not silently unknown",
         raised(lambda: evaluate(all_yes, (("x", ("no.such.capability",)),))),
         "KeyError")

    def canned(connected):
        # ffmpeg and ffprobe are present so that `decode.image` does not block the
        # Resolve suite for a reason the case is not about.
        facts = {"tools": {"ffmpeg": {"version": "canned"},
                           "ffprobe": {"version": "canned"}},
                 "cuda": {"toolkits": [], "gpus": []},
                 "resolve": {"installed": True, "sdk": "canned", "library": None},
                 "onnx": {"headers": False, "libraries": []}}
        return capabilities_of(facts, connected)

    case("a connect that reached no host leaves both Resolve capabilities unprobed",
         {name: canned({"connected": False, "reason": "nobody listening"})[name]
          for name in ("host.resolve.studio", "host.resolve.scripting")},
         {"host.resolve.studio": None, "host.resolve.scripting": None})
    case("an unreached host leaves the Resolve suite unknown, not blocked",
         evaluate(canned({"connected": False, "reason": "nobody listening"})
                  )["tests/resolve/poc.py --build-project"]["status"],
         "unknown")
    case("a connected Studio answers both Resolve capabilities",
         {name: canned({"connected": True, "studio": True})[name]
          for name in ("host.resolve.studio", "host.resolve.scripting")},
         {"host.resolve.studio": True, "host.resolve.scripting": True})

    failed = 0
    for index, (name, got, want) in enumerate(cases, 1):
        ok = got == want
        print(f"case {index}: {name} ... {'ok' if ok else 'FAILED'}"
              + ("" if ok else f" (got {got!r}, wanted {want!r})"))
        failed += 0 if ok else 1
    print(f"self-test: {failed} case(s) failed")
    return 1 if failed else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="what this machine can do")
    parser.add_argument("--json", action="store_true", help="print the report as JSON")
    parser.add_argument("--connect", action="store_true",
                        help="also ask a running Resolve whether it is Studio")
    parser.add_argument("--need", help="comma-separated capabilities that must be true")
    parser.add_argument("--self-test", action="store_true",
                        help="check the parsers and the decision table")
    args = parser.parse_args(argv)

    if args.self_test:
        return selftest()

    report = collect(args.connect)
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        show(report)

    if args.need:
        wanted = [name.strip() for name in args.need.split(",") if name.strip()]
        unknown_names = [name for name in wanted
                         if name not in report["capabilities"]]
        if unknown_names:
            print(f"not a capability: {', '.join(unknown_names)}", file=sys.stderr)
            print("known: " + ", ".join(sorted(report["capabilities"])),
                  file=sys.stderr)
            return 2
        missing = [name for name in wanted
                   if report["capabilities"][name] is not True]
        if missing:
            print(f"\nmissing: {', '.join(missing)}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

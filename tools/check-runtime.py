#!/usr/bin/env python3
"""Inspect an ONNX Runtime root: which API version its header declares, whether
the entry points this repository uses are in that header, and what its provider
libraries export and import.

    python3 tools/check-runtime.py .validation/ort-gpu-1.32.0
    python3 tools/check-runtime.py <root> --json
    python3 tools/check-runtime.py <root> --require-api 22 --require-plugin-provider
    python3 tools/check-runtime.py <root-or-bundle> --require-cuda-major 12
    python3 tools/check-runtime.py --self-test

Why a tool and not a paragraph: the two facts that decide whether a code path can
exist are in different places and both are cheap to read but expensive to
remember. The header says which API version the build targets, and a provider
DLL says - only in its export table - whether it can be registered as a plugin
EP. ONNX Runtime 1.20.1 is `ORT_API_VERSION 20` with one provider export
(`OrtGetApiBase`); 1.32.0 is 32 with three, including `CreateEpFactories` and
`ReleaseEpFactory`. A migration written from documentation alone got that wrong
in this repository once already (see references/improvement-backlog.md row 13).

The third fact is the one a host reports as `Error 1114` and nothing else: which
CUDA major a provider expects. Its import names carry it (`cublas64_13.dll`,
`libcublas.so.12`), so `--require-cuda-major` answers it against a host that ships
12 or 13 - a 1.20.1 provider needs CUDA 12, which Resolve 21.2 ships; a 1.32.0
provider is not.

No ONNX Runtime install, no numpy, no compiler: it reads the files. PE and ELF
are parsed in the standard library so the same command works on a Windows zip, a
Linux tarball and a wheel. A Mach-O `.dylib` is not parsed and is reported as
unreadable rather than guessed at - a silence there would read as "no provider". Anything it cannot read is named as unread rather than
skipped, because a silent skip is how a missing export becomes a surprise in a
host.


A `--self-test` builds a PE and an ELF in a temporary directory and checks
this tool's own reading - the export and import tables, the section types an
ELF is selected by, the two gates and the bundle-without-headers case. No ONNX
Runtime is needed: the fixtures are the few hundred bytes of table that make
those decisions visible.

Exit status: 0 everything present, 1 something asked for is missing, 2 bad usage.
"""

import argparse
import contextlib
import io
import json
import os
import re
import shutil
import struct
import sys
import tempfile

# The entry points this repository's template calls, with the API version that
# introduced each. `since` is used to say "present, but not in this runtime"
# rather than a bare missing, which is the difference between "your root is old"
# and "your root's header is truncated".
API_SYMBOLS = [
    ("CreateCUDAProviderOptions", 11),
    ("UpdateCUDAProviderOptions", 11),
    ("GetCUDAProviderOptionsAsString", 11),
    ("ReleaseCUDAProviderOptions", 11),
    ("SessionOptionsAppendExecutionProvider_CUDA_V2", 11),
    ("RegisterExecutionProviderLibrary", 22),
    ("UnregisterExecutionProviderLibrary", 22),
    ("GetEpDevices", 22),
    ("SessionOptionsAppendExecutionProvider_V2", 22),
    ("EpDevice_EpName", 22),
]

# Exports that mark a provider as a plugin EP: the loader looks for exactly these.
PLUGIN_EXPORTS = ("CreateEpFactories", "ReleaseEpFactory")

# The majors that have to equal the host's. cuBLAS, cuBLASLt and the CUDA runtime
# move together with the toolkit (12 for CUDA 12, 13 for CUDA 13); cuDNN (9) and
# cuFFT (11) carry their own numbering and identify no toolkit major, which is why
# they are reported and never compared.
CUDA_MAJOR_LABELS = ("cuBLAS", "cuBLASLt", "CUDA runtime")

# Measured pairs, for the message only - the check itself reads the provider, not
# a version table: 1.20.1's provider imports cuBLAS 12 and a host with CUDA 12
# loads it; 1.32.0's imports cuBLAS 13, so it loads only where CUDA 13 is
# error 1114 (evidence/2026-11-26-windows/resolve-provider-only.log).
CUDA_MAJOR_PAIRS = "1.20.1 pairs with CUDA 12, 1.32.0 wants CUDA 13"

# What a DLL or .so name implies about the CUDA stack it wants.
REDISTRIBUTABLES = {
    "cudnn": ("cuDNN", 9),
    "cublasLt": ("cuBLASLt", None),
    "cublas": ("cuBLAS", None),
    "cufft": ("cuFFT", None),
    "cudart": ("CUDA runtime", None),
}


def read(path):
    with open(path, "rb") as handle:
        return handle.read()


# ---------------------------------------------------------------------------
# PE
# ---------------------------------------------------------------------------


def _pe_sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("no PE signature")
    count = struct.unpack_from("<H", data, pe + 6)[0]
    option_size = struct.unpack_from("<H", data, pe + 20)[0]
    option = pe + 24
    magic = struct.unpack_from("<H", data, option)[0]
    if magic not in (0x10B, 0x20B):
        raise ValueError("unknown optional-header magic %#x" % magic)
    directories = option + (112 if magic == 0x20B else 96)
    sections = []
    start = option + option_size
    for i in range(count):
        name, vsize, va, rsize, raw = struct.unpack_from("<8sIIII", data, start + i * 40)
        sections.append((va, max(vsize, rsize), raw))
    return directories, sections


def _offset(sections, rva):
    for va, size, raw in sections:
        if va <= rva < va + size:
            return raw + (rva - va)
    raise ValueError("rva %#x is in no section" % rva)


def pe_exports(path):
    data = read(path)
    directories, sections = _pe_sections(data)
    rva = struct.unpack_from("<I", data, directories)[0]
    if rva == 0:
        return []
    table = _offset(sections, rva)
    names_rva = struct.unpack_from("<I", data, table + 32)[0]
    count = struct.unpack_from("<I", data, table + 24)[0]
    names = []
    for i in range(count):
        entry = struct.unpack_from("<I", data, _offset(sections, names_rva) + i * 4)[0]
        start = _offset(sections, entry)
        names.append(data[start:data.index(b"\0", start)].decode("ascii", "replace"))
    return names


def pe_imports(path):
    data = read(path)
    directories, sections = _pe_sections(data)
    rva = struct.unpack_from("<I", data, directories + 8)[0]
    if rva == 0:
        return []
    names = []
    cursor = _offset(sections, rva)
    while True:
        entry = data[cursor:cursor + 20]
        if len(entry) < 20 or entry == b"\0" * 20:
            break
        name_rva = struct.unpack_from("<I", entry, 12)[0]
        start = _offset(sections, name_rva)
        names.append(data[start:data.index(b"\0", start)].decode("ascii", "replace"))
        cursor += 20
    return names


# ---------------------------------------------------------------------------
# ELF
# ---------------------------------------------------------------------------


def _elf_tables(data):
    if data[:4] != b"\x7fELF":
        raise ValueError("no ELF signature")
    is64 = data[4] == 2
    endian = "<" if data[5] == 1 else ">"
    if is64:
        shoff = struct.unpack_from(endian + "Q", data, 0x28)[0]
        shentsize, shnum = struct.unpack_from(endian + "HH", data, 0x3A)
        fmt, link_fmt = endian + "IIQQQQIIQQ", endian + "IIQQQQ"
    else:
        shoff = struct.unpack_from(endian + "I", data, 0x20)[0]
        shentsize, shnum = struct.unpack_from(endian + "HH", data, 0x2E)
        fmt, link_fmt = endian + "IIIIIIIIII", endian + "IIIIII"
    sections = []
    for i in range(shnum):
        raw = struct.unpack_from(fmt, data, shoff + i * shentsize)
        # name, type, flags, addr, offset, size, link, info, align, entsize
        sections.append(
            {"name": raw[0], "type": raw[1], "offset": raw[4], "size": raw[5],
             "link": raw[6], "entsize": raw[9]}
        )
    return endian, sections


def _elf_strings(data, sections, index, start, limit=None):
    section = sections[index]
    offset = section["offset"] + start
    end = data.index(b"\0", offset) if limit is None else offset + limit
    return data[offset:end].decode("ascii", "replace")


def elf_exports(path):
    data = read(path)
    endian, sections = _elf_tables(data)
    dynsym = next((s for s in sections if s["type"] == 11), None)  # SHT_DYNSYM
    if dynsym is None:
        return []
    strtab = dynsym["link"]
    names = []
    for i in range(dynsym["size"] // dynsym["entsize"]):
        base = dynsym["offset"] + i * dynsym["entsize"]
        name, info, _other, shndx = struct.unpack_from(endian + "IBBH", data, base)
        if name == 0 or shndx == 0:  # unnamed or undefined: not an export
            continue
        kind = info & 0xF  # STT_FUNC
        if kind not in (1, 2, 10):
            continue
        names.append(_elf_strings(data, sections, strtab, name))
    return names


def elf_needed(path):
    data = read(path)
    endian, sections = _elf_tables(data)
    dynamic = next((s for s in sections if s["type"] == 6), None)  # SHT_DYNAMIC
    if dynamic is None:
        return []
    strtab = dynamic["link"]
    names = []
    for i in range(dynamic["size"] // dynamic["entsize"]):
        base = dynamic["offset"] + i * dynamic["entsize"]
        tag, value = struct.unpack_from(endian + "qQ" if data[4] == 2 else endian + "iI",
                                        data, base)
        if tag == 0:
            break
        if tag == 1:  # DT_NEEDED
            names.append(_elf_strings(data, sections, strtab, value))
    return names


# ---------------------------------------------------------------------------
# The root
# ---------------------------------------------------------------------------


def binary_facts(path):
    """Exports and imports of one library, or a reason it could not be read."""
    try:
        with open(path, "rb") as handle:
            magic = handle.read(4)
        if magic[:2] == b"MZ":
            exports, imports = pe_exports(path), pe_imports(path)
        elif magic == b"\x7fELF":
            exports, imports = elf_exports(path), elf_needed(path)
        else:
            return {"error": "unrecognised format (neither PE nor ELF)"}
    except (ValueError, struct.error, OSError) as exc:
        return {"error": str(exc)}
    return {"exports": exports, "imports": imports}


def redistributables(names):
    """cudnn64_9.dll -> ('cuDNN', 9), deduplicated and ordered.

    Two spellings reach this: the Windows one carries the major inside the base
    name (`cudnn64_9.dll`) and the Linux one in the SONAME suffix
    (`libcudnn.so.9`). Both are normalised before matching, or the Linux column
    would silently print nothing - a blank that reads like "no CUDA" instead of
    "not looked at".
    """
    found = []
    for name in names:
        base = re.split(r"[._]", name, 1)[0].lower()
        stem = re.sub(r"^lib", "", base)
        stem = re.sub(r"\d+$", "", stem)
        for prefix, (label, _version) in REDISTRIBUTABLES.items():
            if stem.startswith(prefix.lower()):
                match = re.search(r"[._](\d+)", name)
                major = int(match.group(1)) if match else None
                item = (label, major)
                if item not in found:
                    found.append(item)
                break
    return found


def provider_cuda_major(entry):
    """The CUDA toolkit major a provider's imports name, or None.

    None means "not readable from this provider", not "no CUDA": a provider that
    imports neither cuBLAS nor the runtime cannot be placed against a host, and a
    gate that asked for a major has to say so instead of passing it.
    """
    majors = set()
    for label, major in entry.get("redistributables") or []:
        if label in CUDA_MAJOR_LABELS and major is not None:
            majors.add(major)
    return majors.pop() if len(majors) == 1 else None


def library_files(root):
    """Core and provider libraries anywhere in the root, by kind."""
    core, providers, others = [], [], []
    for base, _dirs, files in os.walk(root):
        for name in sorted(files):
            lower = name.lower()
            if not (lower.endswith(".dll") or ".so" in lower or lower.endswith(".dylib")):
                continue
            path = os.path.join(base, name)
            # Linux and macOS put a `lib` in front and the version after the
            # suffix (`libonnxruntime_providers_cuda.so.1`); without stripping it
            # the provider was classified as neither core nor provider, and a
            # Linux root reported "no CUDA provider" for a provider sitting in
            # its own lib/ directory.
            stem = re.sub(r"^lib", "", lower.split(".so")[0])
            if stem.startswith("onnxruntime_providers"):
                if "cuda" in stem or "tensorrt" in stem:
                    providers.append(path)
                else:
                    others.append(path)
            elif stem.startswith("onnxruntime") and "providers" not in stem:
                core.append(path)
            elif stem.startswith("libonnxruntime") and "providers" not in stem:
                core.append(path)
    return core, providers, others


def header_facts(root):
    include = os.path.join(root, "include")
    candidates = [os.path.join(include, "onnxruntime_c_api.h"),
                  os.path.join(root, "onnxruntime", "capi", "onnxruntime_c_api.h")]
    for path in candidates:
        if os.path.exists(path):
            text = read(path).decode("utf-8", "replace")
            match = re.search(r"#define\s+ORT_API_VERSION\s+(\d+)", text)
            version = int(match.group(1)) if match else None
            present = {name: (name in text) for name, _since in API_SYMBOLS}
            return {"path": path, "api_version": version, "symbols": present}
    return {"path": None, "api_version": None, "symbols": {}}


def inspect(root):
    report = {"root": os.path.abspath(root), "header": header_facts(root)}
    core, providers, others = library_files(root)
    report["core"] = []
    for path in core:
        facts = binary_facts(path)
        facts["name"] = os.path.basename(path)
        facts["path"] = path
        report["core"].append(facts)
    report["providers"] = []
    for path in providers:
        facts = binary_facts(path)
        facts["name"] = os.path.basename(path)
        facts["path"] = path
        facts["plugin"] = bool(facts.get("exports") and
                               all(e in facts["exports"] for e in PLUGIN_EXPORTS))
        facts["imports_core"] = any(
            i.lower().startswith(("onnxruntime.dll", "libonnxruntime.so"))
            for i in (facts.get("imports") or [])
        )
        facts["redistributables"] = redistributables(facts.get("imports") or [])
        report["providers"].append(facts)
    report["other_providers"] = others
    return report


def blocked(report, require_api, require_plugin, require_cuda_major=None):
    """The gates that did not pass, as sentences."""
    problems = []
    header = report["header"]
    if header["api_version"] is None:
        # A bundle is a legitimate target: it has no headers and the provider
        # question is still answerable. Missing headers are only a failure when a
        # gate asked for an API version, which is the run that needs them.
        if require_api is not None:
            problems.append("no include/onnxruntime_c_api.h with ORT_API_VERSION, "
                            "but --require-api was given")
    elif require_api is not None and header["api_version"] < require_api:
        problems.append("API version %d, but %d was required"
                        % (header["api_version"], require_api))
    if require_plugin:
        # The gate is about the CUDA provider, not about every provider in the
        # package: TensorRT still ships as a built-in provider in 1.32 (its own
        # EP, not the removed NvTensorRT RTX bridge), so gating on it would fail
        # a root that is perfectly able to run CUDA as a plugin.
        cuda = [e for e in report["providers"] if "cuda" in e["name"].lower()]
        if not cuda:
            problems.append("no CUDA provider library in this root")
        for entry in cuda:
            if entry.get("error"):
                problems.append("%s: %s" % (entry["name"], entry["error"]))
            elif not entry["plugin"]:
                problems.append("%s does not export %s"
                                % (entry["name"], " and ".join(PLUGIN_EXPORTS)))
    if require_cuda_major is not None:
        # The question the host answers at load time, answered before a plug-in is
        # installed into a host that will refuse it (error 1114, DLL init failed).
        cuda = [e for e in report["providers"] if "cuda" in e["name"].lower()]
        if not cuda:
            problems.append("no CUDA provider library, so --require-cuda-major "
                            "%d cannot be checked" % require_cuda_major)
        for entry in cuda:
            if entry.get("error"):
                continue                    # already named by the plugin gate above
            major = provider_cuda_major(entry)
            if major is None:
                problems.append(
                    "%s: no cuBLAS or CUDA-runtime import names a toolkit major, so "
                    "--require-cuda-major %d cannot be checked"
                    % (entry["name"], require_cuda_major))
            elif major != require_cuda_major:
                problems.append(
                    "%s imports cuBLAS/CUDA runtime %d, but the host ships %d: a host "
                    "refuses such a provider at load (error 1114, the DLL "
                    "initialisation routine failed). Build the bundle against an "
                    "ONNX Runtime whose CUDA provider targets %d (%s), or bundle the "
                    "CUDA %d runtime as well and accept a second stack in the host's "
                    "process."
                    % (entry["name"], major, require_cuda_major, require_cuda_major,
                       CUDA_MAJOR_PAIRS, major))
    return problems


def describe(report):
    lines = ["root: %s" % report["root"]]
    header = report["header"]
    if header["api_version"] is None:
        lines.append("header: none in this root (a bundle, or a package shipped "
                     "without headers); the provider facts below still apply")
    else:
        lines.append("header: ORT_API_VERSION %d  (%s)"
                     % (header["api_version"], header["path"]))
        missing = [name for name, present in header["symbols"].items() if not present]
        if not missing:
            lines.append("        every entry point this repository uses is declared")
        else:
            since = dict(API_SYMBOLS)
            for name in missing:
                lines.append("        missing: %s (since %d)" % (name, since[name]))
    cores = report["core"]
    lines.append("core: %s" % (", ".join(e["name"] for e in cores) or "none found"))
    for entry in report["providers"]:
        if entry.get("error"):
            lines.append("provider %s: unreadable (%s)" % (entry["name"], entry["error"]))
            continue
        exports = entry["exports"]
        mark = "plugin EP" if entry["plugin"] else "built-in provider"
        lines.append("provider %s: %d exports, %s" % (entry["name"], len(exports), mark))
        notable = [e for e in exports if e in PLUGIN_EXPORTS or e == "OrtGetApiBase"]
        if notable:
            lines.append("        exports: %s" % ", ".join(notable))
        needs = entry["redistributables"]
        if needs:
            rendered = [(label + " " + str(major)).strip() for label, major in needs]
            lines.append("        imports: %s" % ", ".join(rendered))
        lines.append("        imports the core by name: %s"
                     % ("yes" if entry["imports_core"] else "no"))
        major = provider_cuda_major(entry)
        if major is not None:
            lines.append("        CUDA major: %d (cuBLAS and the CUDA runtime agree; "
                         "a host ships that major or refuses this provider)" % major)
    if report["other_providers"]:
        lines.append("other providers: %s"
                     % ", ".join(os.path.basename(p) for p in report.get("other_providers") or []))
    if not report["providers"]:
        lines.append("provider: none in this root (a CPU package has no CUDA provider)")
    return lines


# ---------------------------------------------------------------------------
# --self-test
# ---------------------------------------------------------------------------


def _fixture_pe(exports=(), imports=()):
    """A PE file with an export and an import directory and nothing else.

    Only the tables this tool reads are here: a DOS header, one section at RVA
    0x1000 mapped to file offset 0x400, an export directory with names, and an
    import directory of DLL names. Enough to prove that the parser walks RVAs
    through the section table; a linker does the rest.
    """
    payload = bytearray()

    def place(blob, align=8):
        while len(payload) % align:
            payload.append(0)
        rva = 0x1000 + len(payload)
        payload.extend(blob)
        return rva

    export_names = export_table = 0
    if exports:
        names = [place(name.encode("ascii") + b"\0", 1) for name in exports]
        export_names = place(b"".join(struct.pack("<I", rva) for rva in names))
        export_table = place(b"\0" * 40)
        struct.pack_into("<I", payload, export_table - 0x1000 + 24, len(exports))
        struct.pack_into("<I", payload, export_table - 0x1000 + 32, export_names)
    import_table = 0
    if imports:
        names = [place(name.encode("ascii") + b"\0", 1) for name in imports]
        rows = b"".join(struct.pack("<IIIII", 0, 0, 0, rva, 0) for rva in names)
        import_table = place(rows + b"\0" * 20)

    data = bytearray(0x400)
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x40)      # e_lfanew
    data[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<H", data, 0x44, 0x8664)    # Machine: x64
    struct.pack_into("<H", data, 0x46, 1)         # NumberOfSections
    struct.pack_into("<H", data, 0x54, 240)       # SizeOfOptionalHeader
    struct.pack_into("<H", data, 0x58, 0x20B)     # PE32+
    struct.pack_into("<I", data, 0x58 + 112, export_table)
    struct.pack_into("<I", data, 0x58 + 120, import_table)
    struct.pack_into("<8sIIII", data, 0x148, b"rdata", len(payload), 0x1000,
                     len(payload), 0x400)
    return bytes(data) + bytes(payload)


def _fixture_elf(exports=(), needed=()):
    """A 64-bit little-endian ELF with .dynsym, .dynstr and .dynamic.

    The export names are STT_FUNC entries in the symbol table, the imports are
    DT_NEEDED tags in the dynamic section. Section names are deliberately absent:
    the parser has to select sections by type, not by name.
    """
    strings = bytearray(b"\0")

    def string(name):
        offset = len(strings)
        strings.extend(name.encode("ascii") + b"\0")
        return offset

    symbols = [string(name) for name in exports]
    needed_offsets = [string(name) for name in needed]
    dynsym = bytearray(24)  # the null entry
    for offset in symbols:
        # st_name, st_info (global, STT_FUNC), st_other, st_shndx, value, size
        dynsym.extend(struct.pack("<IBBHQQ", offset, (1 << 4) | 2, 0, 1,
                                  0x1000, 8))
    dynamic = bytearray()
    for offset in needed_offsets:
        dynamic.extend(struct.pack("<qQ", 1, offset))  # DT_NEEDED
    dynamic.extend(struct.pack("<qQ", 0, 0))           # DT_NULL

    data = bytearray(64)

    def append(blob, align=8):
        while len(data) % align:
            data.append(0)
        offset = len(data)
        data.extend(blob)
        return offset

    strtab = append(bytes(strings), 1)
    symtab = append(bytes(dynsym))
    dynamic_offset = append(bytes(dynamic))
    shoff = append(b"\0" * (64 * 4))
    data[0:4] = b"\x7fELF"
    data[4] = 2  # ELFCLASS64
    data[5] = 1  # ELFDATA2LSB
    data[6] = 1  # EV_CURRENT
    struct.pack_into("<Q", data, 0x28, shoff)
    struct.pack_into("<HH", data, 0x3A, 64, 4)
    # name, type, flags, addr, offset, size, link, info, align, entsize
    sections = [
        (0, 0, 0, 0, 0, 0),
        (0, 11, symtab, len(dynsym), 2, 24),          # SHT_DYNSYM -> .dynstr
        (0, 3, strtab, len(strings), 0, 0),           # SHT_STRTAB
        (0, 6, dynamic_offset, len(dynamic), 2, 16),  # SHT_DYNAMIC -> .dynstr
    ]
    for index, (name, kind, offset, size, link, entsize) in enumerate(sections):
        struct.pack_into("<IIQQQQIIQQ", data, shoff + index * 64, name, kind,
                         0, 0, offset, size, link, 0, 8, entsize)
    return bytes(data)


def _fixture_header(version, symbols):
    """An ONNX Runtime header with one ORT_API_VERSION and the named names."""
    lines = ["/* synthetic ONNX Runtime header: nothing but the version and names */",
             "#define ORT_API_VERSION %d" % version]
    lines.extend("ORT_API2_STATUS %s(void);" % name for name in symbols)
    return "\n".join(lines) + "\n"


def self_test():
    """Every behaviour the evidence logs assert, on fixtures built here.

    Not coverage of the parser: the point is that a change to it cannot pass
    unnoticed. The two gates, the bundle-without-headers case and the Linux name
    mapping were each verified by hand once, in a session that is gone, and a
    checker whose own behaviour is hand-verified rots.
    """
    base = tempfile.mkdtemp(prefix="check-runtime-selftest-")
    checks = []

    def write(relative, blob):
        path = os.path.join(base, relative)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if isinstance(blob, bytes):
            with open(path, "wb") as handle:
                handle.write(blob)
        else:
            with open(path, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(blob)

    def expect(label, got, want):
        checks.append((label, got, want))

    def quiet(argv):
        with contextlib.redirect_stdout(io.StringIO()) as out:
            status = main(argv)
        return status, out.getvalue()

    def first_provider(report):
        """The provider entry, or an empty one: a parser that returns nothing
        has to be a reported mismatch, not a KeyError two lines later."""
        entries = report.get("providers") or []
        return entries[0] if entries else {}

    modern = [name for name, _since in API_SYMBOLS]
    legacy = [name for name, since in API_SYMBOLS if since < 22]
    v2 = [name for name, since in API_SYMBOLS if since >= 22]

    try:
        # A 1.32 provider: three exports, imports the core and cuDNN.
        plugin_root = os.path.join(base, "plugin")
        write("plugin/bin/onnxruntime.dll", _fixture_pe(["OrtGetApiBase"]))
        write("plugin/bin/onnxruntime_providers_cuda.dll",
              _fixture_pe(["OrtGetApiBase", *PLUGIN_EXPORTS],
                          ["onnxruntime.dll", "cudnn64_9.dll"]))
        write("plugin/include/onnxruntime_c_api.h", _fixture_header(32, modern))
        report = inspect(plugin_root)
        provider = first_provider(report)
        expect("PE export names", provider.get("exports"),
               ["OrtGetApiBase", "CreateEpFactories", "ReleaseEpFactory"])
        expect("PE plugin flag", provider.get("plugin"), True)
        expect("PE imports the core by name", provider.get("imports_core"), True)
        expect("PE redistributables", provider.get("redistributables"),
               [("cuDNN", 9)])
        expect("core library found", [e.get("name") for e in report.get("core") or []],
               ["onnxruntime.dll"])
        expect("header version", (report.get("header") or {}).get("api_version"), 32)
        expect("every entry point this repository uses is declared",
               [n for n, present in ((report.get("header") or {}).get("symbols") or {}).items()
                if not present], [])
        expect("both gates on a plugin provider", blocked(report, 22, True), [])
        expect("exit status with both gates",
               quiet([plugin_root, "--require-api", "22",
                      "--require-plugin-provider"])[0], 0)
        expect("verdict line",
               "verdict: every gate asked for passed"
               in quiet([plugin_root, "--require-api", "22"])[1], True)

        # The CUDA major gate: what a host answers at load time, read from the
        # provider's own import names. 1.32.0 imports cuBLAS 13, 1.20.1 imports 12.
        for major, folder in ((13, "cuda13"), (12, "cuda12")):
            write("%s/bin/onnxruntime.dll" % folder, _fixture_pe(["OrtGetApiBase"]))
            write("%s/bin/onnxruntime_providers_cuda.dll" % folder,
                  _fixture_pe(["OrtGetApiBase"],
                              ["onnxruntime.dll",
                               "cublas64_%d.dll" % major,
                               "cublasLt64_%d.dll" % major,
                               "cudart64_%d.dll" % major]))
        report13 = inspect(os.path.join(base, "cuda13"))
        expect("the provider's CUDA major is read from its imports",
               provider_cuda_major(first_provider(report13)), 13)
        expect("a CUDA 13 provider against a CUDA 13 host passes",
               blocked(report13, None, False, 13), [])
        mismatches = blocked(report13, None, False, 12)
        expect("a CUDA 13 provider names the CUDA 12 host it will be refused by",
               bool(mismatches) and "cuBLAS/CUDA runtime 13" in mismatches[0]
               and "ships 12" in mismatches[0], True)
        expect("the mismatch names the error the host raises",
               any("1114" in problem for problem in mismatches), True)
        expect("the mismatch names the versions that pair",
               any(CUDA_MAJOR_PAIRS in problem for problem in mismatches), True)
        expect("a CUDA 12 provider against a CUDA 12 host passes",
               blocked(inspect(os.path.join(base, "cuda12")), None, False, 12), [])
        expect("the report states the CUDA major it read",
               any("CUDA major: 13" in line for line in describe(report13)), True)
        expect("exit status on a CUDA major mismatch",
               quiet([os.path.join(base, "cuda13"), "--require-cuda-major", "12"])[0],
               1)
        expect("exit status when the majors agree",
               quiet([os.path.join(base, "cuda12"), "--require-cuda-major", "12"])[0],
               0)

        # A 1.20.1 provider: one export, and a header that predates the V2 API.
        legacy_root = os.path.join(base, "legacy")
        write("legacy/bin/onnxruntime.dll", _fixture_pe(["OrtGetApiBase"]))
        write("legacy/bin/onnxruntime_providers_cuda.dll",
              _fixture_pe(["OrtGetApiBase"]))
        write("legacy/include/onnxruntime_c_api.h", _fixture_header(20, legacy))
        report = inspect(legacy_root)
        expect("1.20.1 provider is not a plugin EP",
               first_provider(report).get("plugin"), False)
        expect("old header misses the V2 entry points",
               [n for n, present in ((report.get("header") or {}).get("symbols") or {}).items()
                if not present], v2)
        expect("plugin gate fails on 1.20.1", blocked(report, None, True),
               ["onnxruntime_providers_cuda.dll does not export "
                "CreateEpFactories and ReleaseEpFactory"])
        expect("api gate fails on 1.20.1", blocked(report, 22, False),
               ["API version 20, but 22 was required"])
        expect("exit status on a failing gate",
               quiet([legacy_root, "--require-api", "22"])[0], 1)
        expect("the failure is printed with FAIL",
               "FAIL: API version 20, but 22 was required"
               in quiet([legacy_root, "--require-api", "22"])[1], True)

        # A bundle: no headers, and the provider question still answerable.
        bundle_root = os.path.join(base, "bundle")
        write("bundle/Contents/Win64/onnxruntime.dll",
              _fixture_pe(["OrtGetApiBase"]))
        write("bundle/Contents/Win64/onnxruntime_providers_cuda.dll",
              _fixture_pe(list(PLUGIN_EXPORTS), ["onnxruntime.dll"]))
        report = inspect(bundle_root)
        expect("a bundle passes the provider gate",
               blocked(report, None, True), [])
        expect("--require-api names the missing header",
               blocked(report, 22, True),
               ["no include/onnxruntime_c_api.h with ORT_API_VERSION, "
                "but --require-api was given"])
        expect("the report says the header is absent",
               any(line.startswith("header: none in this root")
                   for line in describe(report)), True)
        expect("a plain run over a bundle exits 0",
               quiet([bundle_root])[0], 0)

        # A Mach-O provider: unreadable, and said so rather than skipped.
        mac_root = os.path.join(base, "mac")
        write("mac/libonnxruntime.1.dylib", _fixture_pe(["OrtGetApiBase"]))
        write("mac/libonnxruntime_providers_cuda.dylib",
              b"\xcf\xfa\xed\xfe" + b"\0" * 60)
        report = inspect(mac_root)
        expect("Mach-O is reported unreadable",
               first_provider(report).get("error"),
               "unrecognised format (neither PE nor ELF)")
        expect("the gate passes the read error on", blocked(report, None, True),
               ["libonnxruntime_providers_cuda.dylib: unrecognised format "
                "(neither PE nor ELF)"])

        # A Linux root: the same facts through the ELF name mapping.
        linux_root = os.path.join(base, "linux")
        write("linux/lib/libonnxruntime.so.1",
              _fixture_elf(["OrtGetApiBase"]))
        write("linux/lib/libonnxruntime_providers_cuda.so.1",
              _fixture_elf(["OrtGetApiBase", *PLUGIN_EXPORTS],
                           ["libonnxruntime.so.1", "libcudnn.so.9",
                            "libcublasLt.so.12", "libcublas.so.12",
                            "libcufft.so.11", "libcudart.so.12"]))
        write("linux/lib/libonnxruntime_providers_shared.so",
              _fixture_elf(["OrtGetApiBase"]))
        write("linux/include/onnxruntime_c_api.h", _fixture_header(32, modern))
        report = inspect(linux_root)
        provider = first_provider(report)
        expect("ELF export names", provider.get("exports"),
               ["OrtGetApiBase", "CreateEpFactories", "ReleaseEpFactory"])
        expect("ELF DT_NEEDED is read", provider.get("imports"),
               ["libonnxruntime.so.1", "libcudnn.so.9", "libcublasLt.so.12",
                "libcublas.so.12", "libcufft.so.11", "libcudart.so.12"])
        expect("Linux redistributable names", provider.get("redistributables"),
               [("cuDNN", 9), ("cuBLASLt", 12), ("cuBLAS", 12), ("cuFFT", 11),
                ("CUDA runtime", 12)])
        expect("a versioned .so is the core library",
               [e.get("name") for e in report.get("core") or []], ["libonnxruntime.so.1"])
        expect("the shared provider is not the CUDA one",
               [os.path.basename(p) for p in report.get("other_providers") or []],
               ["libonnxruntime_providers_shared.so"])
        expect("ELF plugin gate", blocked(report, 22, True), [])
    finally:
        shutil.rmtree(base, ignore_errors=True)

    failed = [entry for entry in checks if entry[1] != entry[2]]
    for label, got, want in failed:
        print("FAIL %s" % label)
        print("      got:  %r" % (got,))
        print("      want: %r" % (want,))
    print("self-test: %d checks, %d failed" % (len(checks), len(failed)))
    return 0 if not failed else 1


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Inspect an ONNX Runtime root: API version, used entry points, "
                    "provider exports and CUDA imports.")
    parser.add_argument("root", nargs="?", help="extracted ONNX Runtime package or bundle root")
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    parser.add_argument("--require-api", type=int, metavar="N",
                        help="fail unless ORT_API_VERSION is at least N")
    parser.add_argument("--require-plugin-provider", action="store_true",
                        help="fail unless the CUDA provider exports "
                             "CreateEpFactories and ReleaseEpFactory")
    parser.add_argument("--require-cuda-major", type=int, metavar="N",
                        help="fail unless the CUDA provider imports cuBLAS and the "
                             "CUDA runtime of major N, i.e. the major the host ships "
                             "(Resolve 21.2: 12)")
    parser.add_argument("--self-test", action="store_true",
                        help="build PE and ELF fixtures in a temp dir and "
                             "check this tool's own reading and gates; "
                             "needs no ONNX Runtime")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()
    if args.root is None:
        parser.error("a root is required unless --self-test is given")

    if not os.path.isdir(args.root):
        sys.stderr.write("error: %s is not a directory\n" % args.root)
        return 2
    report = inspect(args.root)
    problems = blocked(report, args.require_api, args.require_plugin_provider,
                       args.require_cuda_major)
    report["problems"] = problems

    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        for line in describe(report):
            print(line)
        for problem in problems:
            print("FAIL: %s" % problem)
        if not problems:
            if (args.require_api is None and not args.require_plugin_provider
                    and args.require_cuda_major is None):
                # A read-only run has nothing to pass: saying "everything is
                # present" over a header that is missing five entry points would
                # be the one line a reader remembers.
                print("verdict: inspection only, no gate asked for")
            else:
                print("verdict: every gate asked for passed")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())

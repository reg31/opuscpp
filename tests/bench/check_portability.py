import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SRC_DIR = REPO_ROOT / "src"

INTRINSIC_HEADERS = (
    "immintrin.h", "xmmintrin.h", "emmintrin.h", "pmmintrin.h", "tmmintrin.h",
    "smmintrin.h", "nmmintrin.h", "wmmintrin.h", "avxintrin.h", "x86intrin.h",
    "intrin.h", "arm_neon.h", "arm_sve.h", "arm_acle.h", "altivec.h",
    "wasm_simd128.h", "riscv_vector.h",
)

INCLUDE_RE = re.compile(
    r'^\s*#\s*include\s*[<"](?:[^>"]*/)?(' + "|".join(re.escape(h) for h in INTRINSIC_HEADERS) + r')[>"]'
)

PREPROC_COND_RE = re.compile(r"^\s*#\s*(?:if|ifdef|ifndef|elif)\b")

ARCH_MACRO_RE = re.compile(
    r"\b(?:__SSE\w*|__AVX\w*|__ARM_NEON\w*|__aarch64__|__x86_64__|_M_X64|_M_IX86|_M_ARM\w*"
    r"|__i386__|__arm__|__riscv\w*|__wasm_simd128__|__FMA__)\b"
)

CODE_PATTERNS = (
    ("intrinsic-identifier", re.compile(r"\b_mm(?:256|512)?_\w+|\b__m(?:128|256|512)\w*")),
    ("neon-identifier", re.compile(r"\bv[a-z]+[0-9]*q?_(?:n_)?[suf](?:8|16|32|64)\b")),
    ("builtin", re.compile(r"__builtin_(?:ia32_|neon_|aarch64_)\w*")),
    ("inline-asm", re.compile(r"\b__asm\w*|\basm\s*(?:volatile\s*)?\(")),
    ("target-attribute", re.compile(r"__attribute__\s*\(\(\s*(?:__target__|target)\b|\[\[\s*gnu::\s*target\b")),
    ("cpu-dispatch", re.compile(r"__builtin_cpu_(?:supports|init)|\b_*cpuid\b")),
)

TOKEN_RE = re.compile(
    r"//[^\n]*"
    r"|/\*.*?(?:\*/|\Z)"
    r"|\"(?:\\.|[^\"\\\n])*\""
    r"|'(?:\\.|[^'\\\n])*'",
    re.S,
)


def blank(token):
    return re.sub(r"[^\n]", " ", token)


def mask(text, keep_strings):
    def replace(match):
        token = match.group(0)
        if keep_strings and token[0] in "\"'":
            return token
        return blank(token)

    return TOKEN_RE.sub(replace, text)


def scan_text(name, text):
    code_lines = mask(text, keep_strings=False).split("\n")
    kept_lines = mask(text, keep_strings=True).split("\n")
    findings = []
    for number, line in enumerate(code_lines, 1):
        for category, pattern in CODE_PATTERNS:
            for match in pattern.finditer(line):
                findings.append((name, number, category, match.group(0)))
        if PREPROC_COND_RE.match(line):
            for match in ARCH_MACRO_RE.finditer(line):
                findings.append((name, number, "arch-macro-conditional", match.group(0)))
        include = INCLUDE_RE.match(kept_lines[number - 1])
        if include:
            findings.append((name, number, "intrinsic-header", include.group(1)))
    return findings


def run_scan(root):
    files = sorted(path for path in root.rglob("*") if path.is_file())
    findings = []
    for path in files:
        text = path.read_bytes().decode("utf-8", errors="replace")
        findings.extend(scan_text(path.relative_to(REPO_ROOT).as_posix(), text))
    return len(files), findings


def report(file_count, findings):
    for name, number, category, matched in findings:
        print(f"{name}:{number}: [{category}] {matched}")
    if findings:
        print(f"portability=FAIL ({file_count} files scanned, {len(findings)} findings)")
        return 1
    print(f"portability=PASS ({file_count} files scanned)")
    return 0


SELF_TEST_CASES = (
    ("clean", "#include <vector>\n#if defined(OPUS_DEBUG)\nint twice(int x) { return x * 2; }\n#endif\n", set()),
    ("intrinsic-header", "#include <immintrin.h>\n", {"intrinsic-header"}),
    ("intrinsic-identifier", "__m256 v = _mm256_add_ps(a, b);\n", {"intrinsic-identifier"}),
    ("neon-identifier", "float32x4_t r = vaddq_f32(a, b);\nint32x4_t q = vld1q_s32(p);\n", {"neon-identifier"}),
    ("builtin", "int x = __builtin_ia32_pause();\n", {"builtin"}),
    ("arch-macro-conditional", "#ifdef __AVX2__\nint y;\n#endif\n", {"arch-macro-conditional"}),
    ("inline-asm", "void f() { asm volatile(\"nop\"); }\n", {"inline-asm"}),
    ("target-attribute", "__attribute__((target(\"avx2\"))) void f();\n", {"target-attribute"}),
    ("cpu-dispatch", "if (__builtin_cpu_supports(\"avx2\")) {}\n__cpuid(info, 1);\n", {"cpu-dispatch"}),
    ("string-then-code", "const char* u = \"http://x\"; __m128 v;\n", {"intrinsic-identifier"}),
    (
        "comment-only",
        "// _mm256_add_ps __m256 asm(\"nop\") #include <immintrin.h>\n"
        "/* __builtin_cpu_supports cpuid __AVX2__ */\n"
        "#if 0 /* __ARM_NEON */\n#endif\n"
        "const char* s = \"__asm__ __builtin_ia32_ vaddq_f32 _mm_add_ps\";\n",
        set(),
    ),
)


def self_test():
    failures = 0
    for name, snippet, expected in SELF_TEST_CASES:
        actual = {category for _, _, category, _ in scan_text(name, snippet)}
        ok = actual == expected
        failures += not ok
        print(f"self-test {name}: {'ok' if ok else f'FAILED expected {sorted(expected)} got {sorted(actual)}'}")
    if failures:
        print(f"self-test=FAIL ({failures} of {len(SELF_TEST_CASES)} cases failed)")
        return 1
    print(f"self-test=PASS ({len(SELF_TEST_CASES)} cases)")
    return 0


def main(argv):
    if "--self-test" in argv:
        return self_test()
    if not SRC_DIR.is_dir():
        print(f"error: source directory not found: {SRC_DIR}")
        return 2
    file_count, findings = run_scan(SRC_DIR)
    return report(file_count, findings)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

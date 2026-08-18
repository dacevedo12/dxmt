#!/usr/bin/env python3

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]

# Scoped to src/d3d9, the code this fork adds. The modules DXMT already shipped
# have declarations that predate this rule, and fixing them here would put
# unrelated changes in every diff; they are upstream's to make.
SRC = ROOT / "src" / "d3d9"

# WMTTextureInfo and WMTBufferInfo are wire structs shared with the C unix side,
# which is compiled as Objective-C, so they cannot carry default member
# initializers of their own. WMTTextureInfo also holds fields a caller has no
# reason to think about: reserved feeds WMTTextureInfoFlag bits straight into
# the texture descriptor, and mach_port selects between creating a shared
# texture and importing one. A site that assigns only the fields it needs
# therefore hands Metal whatever the stack held, which fails far from the
# declaration and only on whichever architecture leaves a nonzero byte there.
# WMTBufferInfo has no such field today and is held to the same rule anyway: a
# per-type carve-out is where the next instance of this would hide.

EXCLUDED_DIRS = ()
SEARCH_SUFFIXES = (".cpp", ".hpp", ".h")

GUARDED_TYPES = ("WMTTextureInfo", "WMTBufferInfo")

QUALIFIERS = r"(?:(?:static|mutable|thread_local|inline|constexpr|alignas\([^)]*\))\s+)*"
DECLARATION = re.compile(
    r"^[^\S\n]*" + QUALIFIERS + r"(?:struct\s+)?("
    + "|".join(GUARDED_TYPES) + r")\s+([^;\n]*);",
    re.MULTILINE,
)

# Known gap: an aggregate such as std::array<WMTTextureInfo, 4> is a real
# hazard (no value initialization at block scope) that this deliberately does
# not try to parse. No such site exists today.
COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)


def strip_comments(text: str) -> str:
    # Blank out comment bodies but keep their newlines, so a declaration quoted
    # in prose cannot trip the policy while reported line numbers stay true.
    # Does not track string literals: a `/*` inside one would swallow code to
    # the next `*/`. That can only hide a violation, never invent one, and no
    # file in the tree does it.
    return COMMENT.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


def uninitialized_declarators(declarator_list: str):
    # Each declarator carries its own initializer, so a list is only as safe as
    # its least initialized member. Pointers, references and a parameter in a
    # wrapped signature are not declarations of an object.
    for piece in declarator_list.split(","):
        piece = piece.strip()
        if not piece or piece[0] in "*&" or "(" in piece or ")" in piece:
            continue
        if "=" not in piece and "{" not in piece:
            yield piece


def sources():
    for path in sorted(SRC.rglob("*")):
        if path.suffix not in SEARCH_SUFFIXES:
            continue
        if any(part in EXCLUDED_DIRS for part in path.relative_to(SRC).parts):
            continue
        yield path


def offenders_in(text: str):
    for match in DECLARATION.finditer(text):
        line = text.count("\n", 0, match.start()) + 1
        for declarator in uninitialized_declarators(match.group(2)):
            yield line, match.group(1), declarator


class DescriptorInitializationPolicyTest(unittest.TestCase):
    def test_descriptor_declarations_are_zero_initialized(self):
        offenders = []
        for path in sources():
            for line, kind, declarator in offenders_in(strip_comments(path.read_text())):
                offenders.append(
                    f"{path.relative_to(ROOT)}:{line}: "
                    f"{kind} {declarator} declared without an initializer"
                )
        self.assertEqual(
            offenders,
            [],
            "Metal descriptor structs must be declared `= {}` (or `{}`) so unset "
            "fields (WMTTextureInfo::reserved, ::mach_port) are not stack "
            "garbage:\n" + "\n".join(offenders),
        )

    def test_policy_sees_the_declaration_forms_it_claims_to_guard(self):
        # Pins the pattern itself. Without this the policy above could pass
        # vacuously: a matcher that matches nothing reports no offenders.
        flagged = (
            "  WMTTextureInfo info;",
            "  struct WMTBufferInfo info;",
            "  WMTTextureInfo a, b;",
            "  WMTTextureInfo a = {}, b;",
            "  WMTTextureInfo a, b = {};",
            "  WMTTextureInfo infos[4];",
            "  WMTTextureInfo info[2][3];",
            "  static WMTTextureInfo info;",
            "  mutable WMTBufferInfo info_;",
            "  alignas(16) WMTTextureInfo info;",
            "WMTTextureInfo info;",
        )
        allowed = (
            "  WMTTextureInfo info = {};",
            "  WMTTextureInfo info{};",
            "  WMTTextureInfo info = info_;",
            "  WMTTextureInfo &info;",
            "  WMTTextureInfo *info;",
            "    WMTTextureInfo info);",
            "  WMTTextureInfo a = {}, b = {};",
        )
        for source in flagged:
            self.assertTrue(
                list(offenders_in(source)), f"should be flagged: {source}"
            )
        for source in allowed:
            self.assertEqual(
                list(offenders_in(source)), [], f"should be allowed: {source}"
            )

    def test_policy_ignores_declarations_quoted_in_comments(self):
        self.assertEqual(list(offenders_in(strip_comments("/* WMTTextureInfo x; */"))), [])
        self.assertEqual(list(offenders_in(strip_comments("// WMTTextureInfo x;"))), [])

    def test_policy_reports_the_line_the_declaration_sits_on(self):
        text = strip_comments("/* a\n b\n c */\nint pad;\n  WMTTextureInfo info;\n")
        self.assertEqual([line for line, _, _ in offenders_in(text)], [5])

    def test_policy_covers_every_guarded_type(self):
        for kind in GUARDED_TYPES:
            self.assertTrue(
                any(kind in path.read_text() for path in sources()),
                f"{kind} no longer appears in the searched tree",
            )


if __name__ == "__main__":
    unittest.main()

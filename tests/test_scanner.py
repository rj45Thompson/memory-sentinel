"""The static pass, held to the test game both ways, and to edge cases one snippet at a time (UC-1).

The test game's planted files mark each bug with `PLANTED: <rule>` on its line. The scan must report exactly one finding of
that rule on every marked line, one suppressed finding on every `memsentinel: ignore` line, and nothing else anywhere: a
missing finding is a miss, an extra one is a false alarm. The Clean folder holds the same patterns written correctly and
must produce nothing.
"""
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "unreal" / "MemorySentinel" / "Content" / "Python"))

from memsentinel.cli import main as cli_main  # noqa: E402
from memsentinel.scanner import Scanner, scan_text  # noqa: E402

GAME_SOURCE = ROOT / "unreal" / "SentinelTest" / "Source"


def rules_at(ff):
    return sorted((f.where["file"], f.where["line"], f.rule, bool(f.suppressed)) for f in ff.findings)


class TestGameBothWays(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ff = Scanner(GAME_SOURCE).run()

    def expected(self):
        want = []
        for p in sorted(GAME_SOURCE.rglob("*")):
            if p.suffix not in (".h", ".cpp"):
                continue
            rel = p.relative_to(GAME_SOURCE).as_posix()
            for n, line in enumerate(p.read_text(encoding="utf-8").splitlines(), 1):
                m = re.search(r"PLANTED: ([a-z-]+)", line)
                if m and not line.lstrip().startswith("//"):
                    want.append((rel, n, m.group(1), False))
                m = re.search(r"memsentinel: ignore", line)
                if m:
                    want.append((rel, n, "uobject-member-unseen", True))
        return sorted(want)

    def test_every_plant_found_once_and_nothing_else(self):
        self.assertTrue(self.ff.ok)
        want = self.expected()
        self.assertGreaterEqual(len(want), 17, "the planted files lost their markers")
        got = rules_at(self.ff)
        missing = [w for w in want if w not in got]
        extra = [g for g in got if g not in want]
        self.assertEqual(missing, [], "planted bugs the scan missed")
        self.assertEqual(extra, [], "findings on lines that carry no plant (false alarms)")

    def test_clean_folder_is_silent(self):
        clean = [f for f in self.ff.findings if "/Clean/" in "/" + f.where["file"]]
        self.assertEqual(clean, [])

    def test_every_finding_says_how_it_is_known(self):
        for f in self.ff.findings:
            self.assertTrue(f.how_known.startswith("READ:"), f.how_known)
            self.assertTrue(f.fix and f.why)

    def test_cycle_names_the_side_to_weaken(self):
        hud = next(f for f in self.ff.findings if f.rule == "shared-cycle" and "FHud" in f.title)
        self.assertEqual(hud.evidence["weaken"], "FPanel::Host")
        self.assertIn("FHud derives from IPanelHost", hud.how_known)
        self.assertEqual(hud.evidence["cycle"], ["FHud", "FPanel", "FHud"])


class Snippets(unittest.TestCase):
    def one(self, code, rule=None):
        ff = scan_text(code)
        return [(f.rule, f.where["line"]) for f in ff.findings if rule is None or f.rule == rule]

    def test_multiline_uproperty_is_seen(self):
        code = "UCLASS()\nclass UA : public UObject\n{\n GENERATED_BODY()\n UPROPERTY(EditAnywhere,\n  meta=(X=\"a;b\"))\n UObject* P;\n};\n"
        self.assertEqual(self.one(code), [])

    def test_comment_and_string_do_not_count(self):
        code = ("UCLASS()\nclass UA : public UObject\n{\n GENERATED_BODY()\n // UObject* InComment;\n"
                " FString S = TEXT(\"UObject* InString;\");\n};\n")
        self.assertEqual(self.one(code), [])

    def test_elaborated_forward_type(self):
        code = "UCLASS()\nclass AB : public AActor\n{\n GENERATED_BODY()\n class UStaticMesh* Mesh;\n};\n"
        self.assertEqual(self.one(code), [("uobject-member-unseen", 5)])

    def test_functions_and_params_are_not_members(self):
        code = ("UCLASS()\nclass AB : public AActor\n{\n GENERATED_BODY()\npublic:\n UObject* Find(UObject* Key) const;\n"
                " void Set(UObject* V) { Cached = V; }\n virtual UObject* Make() override { return nullptr; }\n"
                " UPROPERTY() TObjectPtr<UObject> Cached;\n};\n")
        self.assertEqual(self.one(code), [])

    def test_editor_only_block_members_are_read(self):
        code = ("UCLASS()\nclass AB : public AActor\n{\n GENERATED_BODY()\n#if WITH_EDITORONLY_DATA\n UObject* EdOnly;\n"
                "#endif\n};\n")
        self.assertEqual(self.one(code), [("uobject-member-unseen", 6)])

    def test_char_pointers_and_unknown_types_are_not_uobjects(self):
        code = ("UCLASS()\nclass AB : public AActor\n{\n GENERATED_BODY()\n UTF8CHAR* Text;\n ANSICHAR* A;\n"
                " FMyThing* Thing;\n int32* Count;\n};\n")
        self.assertEqual(self.one(code), [])

    def test_plain_type_named_like_uobject_is_read_not_guessed(self):
        code = "struct UNotAnObject { int X; };\nUCLASS()\nclass AB : public AActor\n{\n GENERATED_BODY()\n UNotAnObject* P;\n};\n"
        self.assertEqual(self.one(code), [])

    def test_naming_rule_is_stated(self):
        ff = scan_text("UCLASS()\nclass AB : public AActor\n{\n GENERATED_BODY()\n UMaterialInterface* M;\n};\n")
        self.assertIn("naming rule", ff.findings[0].how_known)

    def test_containers_and_optional(self):
        code = ("UCLASS()\nclass AB : public AActor\n{\n GENERATED_BODY()\n TOptional<UObject*> O;\n TPair<int32, AActor*> P;\n"
                " TArray<TWeakObjectPtr<AActor>> W;\n TFunction<void(UObject*)> F;\n};\n")
        self.assertEqual(self.one(code), [("uobject-member-unseen", 5), ("uobject-member-unseen", 6)])

    def test_script_interface_and_native_interface(self):
        code = ("UINTERFACE()\nclass UUsable : public UInterface { GENERATED_BODY() };\nclass IUsable { GENERATED_BODY() };\n"
                "UCLASS()\nclass AB : public AActor\n{\n GENERATED_BODY()\n TScriptInterface<IUsable> S;\n IUsable* Raw;\n};\n")
        self.assertEqual(self.one(code), [("uobject-member-unseen", 8), ("uobject-member-unseen", 9)])

    def test_multiple_declarators(self):
        code = "UCLASS()\nclass AB : public AActor\n{\n GENERATED_BODY()\n AActor *A, *B;\n};\n"
        self.assertEqual(len(self.one(code)), 2)

    def test_nested_struct_member_is_its_own_type(self):
        code = ("UCLASS()\nclass AB : public AActor\n{\n GENERATED_BODY()\n struct FInner { UObject* P; };\n"
                " UPROPERTY() int32 N;\n};\n")
        self.assertEqual(self.one(code), [("uobject-in-plain-type", 5)])

    def test_deprecated_macro_prefix(self):
        code = ("UCLASS()\nclass AB : public AActor\n{\n GENERATED_BODY()\n UE_DEPRECATED(5.0, \"x\")\n UObject* Old;\n};\n")
        self.assertEqual(self.one(code), [("uobject-member-unseen", 6)])

    def test_brace_initialized_member(self):
        code = "UCLASS()\nclass AB : public AActor\n{\n GENERATED_BODY()\n UObject* P{nullptr};\n int32 N{3};\n};\n"
        self.assertEqual(self.one(code), [("uobject-member-unseen", 5)])

    def test_lambda_default_capture_uses_member(self):
        code = ("UCLASS()\nclass AB : public AActor\n{\n GENERATED_BODY()\n int32 Hits = 0;\n void Go();\n};\n"
                "void AB::Go()\n{\n FTimerHandle H;\n GetWorldTimerManager().SetTimer(H, [=]() { ++Hits; }, 1.f, false);\n"
                " int32 Local = 1;\n GetWorldTimerManager().SetTimer(H, [=]() { UE_LOG(LogTemp, Log, TEXT(\"%d\"), Local); }, 1.f, false);\n}\n")
        self.assertEqual(self.one(code, "lambda-captures-uobject"), [("lambda-captures-uobject", 11)])

    def test_lambda_capturing_uobject_local(self):
        code = ("void Run(UWorld* World)\n{\n AActor* Hero = nullptr;\n"
                " FTimerHandle H;\n World->GetTimerManager().SetTimer(H, [Hero]() { Hero->Destroy(); }, 1.f, false);\n}\n")
        self.assertEqual(self.one(code, "lambda-captures-uobject"), [("lambda-captures-uobject", 5)])

    def test_three_way_cycle_and_weak_break(self):
        code = ("struct FA { TSharedPtr<struct FB> B; };\nstruct FB { TSharedPtr<struct FC> C; };\n"
                "struct FC { TSharedPtr<FA> A; };\nstruct FX { TSharedPtr<struct FY> Y; };\nstruct FY { TWeakPtr<FX> X; };\n")
        ff = scan_text(code)
        cyc = [f for f in ff.findings if f.rule == "shared-cycle"]
        self.assertEqual(len(cyc), 1)
        self.assertEqual(cyc[0].evidence["cycle"], ["FA", "FB", "FC", "FA"])

    def test_inherited_member_closes_a_cycle(self):
        code = ("struct FBase { TSharedPtr<struct FOther> O; };\nstruct FChild : public FBase { };\n"
                "struct FOther { TSharedPtr<FChild> C; };\n")
        ff = scan_text(code)
        self.assertEqual([f.rule for f in ff.findings], ["shared-cycle"])


class Exits(unittest.TestCase):
    def test_nothing_to_scan_is_a_failure_not_a_pass(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(cli_main(["scan", d, "-o", os.path.join(d, "out.json")]), 2)
            ff = Scanner(Path(d)).run()
            self.assertFalse(ff.ok)
            self.assertIn("nothing to scan", ff.failure)

    def test_exit_codes(self):
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "o.json")
            self.assertEqual(cli_main(["scan", str(GAME_SOURCE), "-o", out]), 1)
            self.assertEqual(cli_main(["scan", str(GAME_SOURCE / "SentinelTest" / "Clean"), "-o", out, "--fail-on", "info"]), 0)


if __name__ == "__main__":
    unittest.main()

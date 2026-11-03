"""Generator checks that do not use compiler output as their oracle."""
import tempfile
import tomllib
import unittest
from pathlib import Path

from model import oracle
from suite import make_case, write_program


class GeneratorTests(unittest.TestCase):
    def test_control_flow_observations_cover_both_branches(self):
        for rule, values in {
            "short_circuit_scan": {-7: (-1, 5), 0: (4, 7), 13: (19, 7)},
            "large_function": {-7: (-261, 256), 0: (2, 256), 13: (479, 256)},
        }.items():
            for leaf, (value, calls) in values.items():
                self.assertEqual(oracle(make_case((rule,), leaf, "inline")), [value, calls, 0, 0, 0])

    def test_interleaved_calls_have_independent_observations(self):
        cases = {
            "operator_overload": (122, 3, 0, 0),
            "operator_generic": (-5, 3, 0, 0),
            "index_overload": (26, 3, 0, 0),
            "pointer_index_owner": (-7, 0, 0, 0),
            "unary_operator": (2, 1, 0, 0),
            "overloaded_iterator": (-11, 3, 0, 0),
            "overloaded_array_clone": (-13, 2, 4, 0),
            "dynamic_overload": (44, 6, 0, 0),
            "await_ready": (-11, 2, 0, 0),
            "generic_function_value": (-13, 3, 0, 0),
        }
        for name, observation in cases.items():
            self.assertEqual(oracle(make_case((name,), -7, "inline")), list(observation) + [0])

    def test_library_does_not_inline_its_sources_in_the_consumer(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            case = make_case(("operator_generic", "pointer_index_owner"), 13, "bound")
            write_program(root, [case], "library")
            consumer = tomllib.loads((root / "Vyx.toml").read_text(encoding="utf-8"))
            library = tomllib.loads((root / "library/Vyx.toml").read_text(encoding="utf-8"))
            self.assertEqual(consumer["dependencies"]["support"], {"path": "library", "target": "support"})
            self.assertEqual(consumer["target"]["probe"]["sources"], ["cases.vyx", "main.vyx"])
            self.assertEqual(library["target"]["support"]["type"], "staticlib")
            self.assertNotIn("class GenericOperator", (root / "cases.vyx").read_text(encoding="utf-8"))
            self.assertIn("class GenericOperator", (root / "library/support.vyx").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()

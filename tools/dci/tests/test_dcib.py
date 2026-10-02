import unittest

from tools.dci import dcib


class DcibTests(unittest.TestCase):
    def test_round_trip_and_canonical_map_order(self):
        document = {"z": [None, True, False, -7, 8, 1.25], "a": {"name": "DCI"}}
        encoded = dcib.encode(document)
        self.assertEqual(encoded[:4], b"DCIB")
        self.assertEqual(dcib.decode(encoded), document)
        self.assertEqual(encoded, dcib.encode({"a": {"name": "DCI"}, "z": [None, True, False, -7, 8, 1.25]}))

    def test_rejects_corruption(self):
        encoded = bytearray(dcib.encode({"dci": "1.0"}))
        encoded[-1] ^= 1
        with self.assertRaises(dcib.DcibError): dcib.decode(bytes(encoded))

    def test_rejects_non_json_scalars(self):
        with self.assertRaises(dcib.DcibError): dcib.encode({"value": float("nan")})
        with self.assertRaises(dcib.DcibError): dcib.encode({"value": "a\x00b"})

    def test_cjson_integer_range_is_explicit(self):
        dcib.validate_cjson_compatible({"min": -(1 << 53), "max": 1 << 53})
        with self.assertRaises(dcib.DcibError): dcib.validate_cjson_compatible({"value": (1 << 53) + 1})

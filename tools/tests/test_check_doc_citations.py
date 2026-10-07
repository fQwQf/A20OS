import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import check_doc_citations


class ResolveCitationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.old_root = check_doc_citations.ROOT
        check_doc_citations.ROOT = self.temp.name

    def tearDown(self):
        check_doc_citations.ROOT = self.old_root
        self.temp.cleanup()

    def add_file(self, path, contents="\n"):
        full = os.path.join(check_doc_citations.ROOT, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w", encoding="utf-8") as out:
            out.write(contents)

    def test_external_standard_header_does_not_fall_back_to_same_basename(self):
        self.add_file("kernel/include/drivers/net/virtio_net.h", "\n" * 15)

        target, ambiguous = check_doc_citations.resolve(
            "ds/include/standard-headers/linux/virtio_net.h")

        self.assertIsNone(target)
        self.assertFalse(ambiguous)

    def test_real_in_tree_path_still_resolves_before_external_marker(self):
        path = "kernel/include/standard-headers/linux/virtio_net.h"
        self.add_file(path, "\n" * 20)

        target, ambiguous = check_doc_citations.resolve(path)

        self.assertEqual(target, path)
        self.assertFalse(ambiguous)

    def test_unqualified_citation_still_uses_basename_lookup(self):
        self.add_file("kernel/include/drivers/net/virtio_net.h", "\n" * 15)

        target, ambiguous = check_doc_citations.resolve("virtio_net.h")

        self.assertEqual(target, "kernel/include/drivers/net/virtio_net.h")
        self.assertFalse(ambiguous)


if __name__ == "__main__":
    unittest.main()

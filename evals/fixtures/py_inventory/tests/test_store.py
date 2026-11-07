import unittest

from inventory import Store


class StoreTest(unittest.TestCase):
    def test_add_and_count(self):
        s = Store()
        s.add("apple", 3)
        s.add("apple")
        self.assertEqual(s.count("apple"), 4)

    def test_remove_partial(self):
        s = Store()
        s.add("pear", 5)
        s.remove("pear", 2)
        self.assertEqual(s.count("pear"), 3)

    def test_remove_all_drops_the_name(self):
        s = Store()
        s.add("plum", 2)
        s.remove("plum", 2)
        self.assertEqual(s.names(), [])

    def test_remove_too_many(self):
        s = Store()
        s.add("fig", 1)
        with self.assertRaises(ValueError):
            s.remove("fig", 2)

    def test_total(self):
        s = Store()
        s.add("a", 2)
        s.add("b", 3)
        self.assertEqual(s.total(), 5)


if __name__ == "__main__":
    unittest.main()

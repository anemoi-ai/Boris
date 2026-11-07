import unittest

from pricing import calc_total
from pricing.cart import Cart
from pricing.report import summary


class PricingTest(unittest.TestCase):
    def test_total(self):
        self.assertEqual(calc_total([1.0, 2.0], 0.5), 4.5)

    def test_cart(self):
        c = Cart()
        c.add(10)
        c.add(5)
        self.assertEqual(c.total(), 15)

    def test_report(self):
        self.assertEqual(summary([[10]]), ["order 1: 12.00"])


if __name__ == "__main__":
    unittest.main()

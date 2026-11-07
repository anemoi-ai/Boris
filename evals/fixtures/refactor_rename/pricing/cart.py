from .core import calc_total


class Cart:
    def __init__(self):
        self.prices = []

    def add(self, price):
        self.prices.append(price)

    def total(self, tax_rate=0.0):
        return calc_total(self.prices, tax_rate)

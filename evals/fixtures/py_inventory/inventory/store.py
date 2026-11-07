"""A tiny stock store."""


class Store:
    def __init__(self):
        self._items = {}

    def add(self, name, qty=1):
        if qty <= 0:
            raise ValueError("qty must be positive")
        self._items[name] = self._items.get(name, 0) + qty

    def remove(self, name, qty=1):
        if name not in self._items:
            raise KeyError(name)
        if qty > self._items[name]:
            raise ValueError("not enough stock")
        self._items[name] -= qty
        if self._items[name] < 0:
            del self._items[name]

    def count(self, name):
        return self._items.get(name, 0)

    def total(self):
        return sum(self._items.values())

    def names(self):
        return sorted(self._items)

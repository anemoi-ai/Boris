from .core import calc_total


def summary(orders):
    """One line per order: its total with 20% tax."""
    return [f"order {i}: {calc_total(o, 0.2):.2f}" for i, o in enumerate(orders, 1)]

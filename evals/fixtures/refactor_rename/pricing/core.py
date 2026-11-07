def calc_total(prices, tax_rate=0.0):
    """Sum prices and add tax."""
    subtotal = sum(prices)
    return round(subtotal * (1 + tax_rate), 2)

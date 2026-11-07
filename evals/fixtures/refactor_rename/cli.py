import sys

from pricing import calc_total

if __name__ == "__main__":
    print(calc_total([float(a) for a in sys.argv[1:]]))

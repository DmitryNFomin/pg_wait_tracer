#!/usr/bin/env python3
"""Host controls for the coverage live gate; needs no daemon or BPF."""
from test_coverage_live import self_test

if __name__ == "__main__":
    self_test()

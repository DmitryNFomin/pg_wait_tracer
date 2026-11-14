"""Bounded export of check_props.py's fourteen thematic alphabets (size 0..4).

The source generator additionally exercises symbolic validation annotations.
Those are not byte recipes. Byte faults, compression, mixed rows, all cuts and
link alternatives are constructed explicitly by the concrete harness instead.
Every multiset and unique permutation within this exported bound is tested.
"""
import ast
import itertools
from pathlib import Path

POOLS = [
    ('prefix_and_clean_ties', ['A','At1','At3','Ag','Ag′','Acrc3']),
    ('header_flags_footer', ['A','Af','Ao','Aft','Ah','Z']),
    ('conflict_both_vs_one_valid', ['A','Ac2t1','Ac2t3','Au','Auc3']),
    ('sample_block_conflicts', ['M','Mt3','Mc1t3']),
    ('chain_and_seal', ['A','A+','N','L','At3','Ag']),
    ('bad_and_nonboundary_links', ['A','A+','N','Nb','Ni']),
    ('reversed_chain_interleaved_captures', ['A','N','L','X']),
    ('fault_before_later_conflict', ['Au','Auc3','R','Rc3','A']),
    ('second_boot_and_nonroot_reuse', ['A','A2','N2','Z2','X','A2t0']),
    ('running_void_baseline', ['A','V','Q','Q0','N1']),
    ('unknown_baselines', ['A','Vu','Tu','T0']),
    ('frontier_moves_baseline', ['A','W','Qw','Qw0']),
    ('unqualified_ignored_baseline', ['Wi','Qwi0','Qwi1']),
    ('unqualified_explicit_void', ['Vi','Qvi']),
]
MAX_SIZE = 4


def multisets(reference_directory):
    source = ast.parse((Path(reference_directory)/'check_props.py').read_text())
    declared = [ast.literal_eval(n.value) for n in ast.walk(source)
                if isinstance(n, ast.Assign) and any(isinstance(t,ast.Name) and t.id=='pools' for t in n.targets)]
    assert declared == [POOLS], 'refresh this explicit generator export when its source alphabets change'
    for tag, names in POOLS:
        for size in range(MAX_SIZE+1):
            for choice in itertools.combinations_with_replacement(names,size):
                yield tag, choice

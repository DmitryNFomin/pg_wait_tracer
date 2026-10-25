#!/usr/bin/env python3
"""Browser-free regression/RED checks for the real replay walk's comparator."""
import copy
import json
from pathlib import Path
import struct
import tempfile
import unittest

from replay_ui import trace_headers
from replay_ui_compare import compare_chart, compare_table, ReplayProbe, fixed


class ReplayComparisonTests(unittest.TestCase):
    def overview(self):
        data = {'rows': [dict(name='DB Time',ms=1234,pct=100,aas=1.23,indent=0)]}
        table = dict(headers=['Stat Name','Time','%DB Time','AAS'],
                     rows=[['DB Time','1.2s','100.0%','1.23']])
        return table, data

    def test_painted_value_mutation_is_red(self):
        table, data = self.overview()
        self.assertEqual(compare_table('overview',table,data,0), 3)
        for index, wrong in [(1,'9.9s'), (1,'1.2ms'), (2,'0.0%'), (3,'99.00')]:
            bad = copy.deepcopy(table)
            bad['rows'][0][index] = wrong
            with self.subTest(value=wrong), self.assertRaisesRegex(AssertionError, 'displayed='):
                compare_table('overview',bad,data,0)

    def test_empty_and_missing_row_are_red(self):
        table, data = self.overview()
        with self.assertRaisesRegex(AssertionError, 'empty WS'):
            compare_table('overview',table,{'rows':[]},0)
        table['rows'] = []
        with self.assertRaisesRegex(AssertionError, 'row count'):
            compare_table('overview',table,data,0)

    def test_extra_duplicate_wrong_identity_and_units_are_red(self):
        table, data = self.overview()
        for bad in [dict(headers=table['headers'],rows=table['rows']*2),
                    dict(headers=table['headers'],rows=[['stranger','1.2s','100.0%','1.23']]),
                    dict(headers=['Stat Name','seconds','%DB Time','AAS'],rows=table['rows'])]:
            with self.subTest(table=bad), self.assertRaises(AssertionError):
                compare_table('overview',bad,data,0)

    def test_remainder_is_derived_from_parent_minus_children(self):
        table, data = self.overview()
        data['rows'] += [dict(name='IO',ms=100,pct=10,aas=.1,indent=1),
                         dict(name='IO:read',ms=70,pct=7,aas=.07,indent=2)]
        table['rows'] += [['IO','100.0ms','10.0%','0.10'],
                          ['IO:read','70.0ms','7.0%','0.07'],
                          ['… other (not shown)','30.0ms','—','—']]
        self.assertEqual(compare_table('overview',table,data,0), 12)
        table['rows'][-1][1] = '40.0ms'
        with self.assertRaisesRegex(AssertionError, 'other'):
            compare_table('overview',table,data,0)

    def test_histogram_count_and_units_mutations_are_red(self):
        import math
        data = dict(cells=[[0,1,9]],labels=['1ms'])
        option = dict(series=[dict(data=[[0,1,math.log1p(9),9]])], yAxis=[dict(data=['1ms'])])
        self.assertEqual(compare_chart('histogram',option,data,{}, {},0), 1)
        option['series'][0]['data'][0][3] = 8
        with self.assertRaisesRegex(AssertionError, 'counts'):
            compare_chart('histogram',option,data,{}, {},0)
        option['series'][0]['data'][0][3] = 9
        option['yAxis'][0]['data'] = ['1s']
        with self.assertRaisesRegex(AssertionError, 'units'):
            compare_chart('histogram',option,data,{}, {},0)

    def test_empty_trace_directory_is_red(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(ValueError, 'empty trace directory'):
                trace_headers(tmp)

    def test_header_identity_is_exact_decimal_not_float(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'current.trace'
            start = 1792846167123456789
            path.write_bytes(struct.pack('<IHHIQQ',0x54574750,2,0,17,start,123))
            self.assertEqual(trace_headers(tmp)[0]['start_wall_ns'],str(start))
            path.write_bytes(b'bad')
            with self.assertRaisesRegex(ValueError, 'unreadable'):
                trace_headers(tmp)

    def test_raw_frames_are_retained_without_payload_changes(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp)/'overview').mkdir()
            probe = ReplayProbe('overview',[],10,20,tmp)
            payload = dict(id=3,rows=[dict(ms=1234)])
            probe._frame('received',json.dumps(payload))
            self.assertEqual(probe.received[3],payload)
            record = json.loads((Path(tmp)/'overview/websocket.jsonl').read_text())
            self.assertEqual(record['frame'],payload)

    def test_stranger_header_and_unconsumed_frames_are_red(self):
        from unittest.mock import Mock
        table, data = self.overview()
        expected = [dict(start_wall_ns='1792846167123456789')]
        snapshot = dict(tables={'table-container':table}, option=None,
            live=False, **{'from':10, 'to':20}, mount={'id':'overview'},
            consumed=[dict(id=1,cmd='info'),dict(id=2,cmd='time_model')])
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp)/'overview').mkdir()
            probe = ReplayProbe('overview',expected,10,20,tmp)
            probe.connections = 1
            probe.sent = {1:dict(id=1,cmd='info'),2:dict(id=2,cmd='time_model', **{'from':10,'to':20})}
            probe.received = {1:dict(id=1,from_ns=10,num_cpus=4), 2:dict(id=2,**data)}
            page = Mock()
            page.evaluate.side_effect = lambda js, *args: snapshot if args else None
            owner_page = Mock()
            owner_page.evaluate.return_value = snapshot['consumed']
            with self.assertRaisesRegex(AssertionError, 'ownership trace header'):
                probe.assert_owner(owner_page)
            with self.assertRaisesRegex(AssertionError, 'ownership trace header'):
                probe.verify(page)
            probe.received[1]['_replay_trace_headers'] = [dict(start_wall_ns='1792846167123456788')]
            with self.assertRaisesRegex(AssertionError, 'ownership trace header'):
                probe.verify(page)
            probe.received[1]['_replay_trace_headers'] = expected
            snapshot['consumed'] = [dict(id=1,cmd='info')]
            with self.assertRaisesRegex(AssertionError, 'no consumed WS response'):
                probe.verify(page)
            snapshot['consumed'].append(dict(id=99,cmd='time_model'))
            with self.assertRaisesRegex(AssertionError, 'not captured'):
                probe.verify(page)
            probe.connections = 2
            with self.assertRaisesRegex(AssertionError, 'connection count'):
                probe.verify(page)

    def test_js_rounding_contract(self):
        self.assertEqual(fixed(12.5,0),'13')
        self.assertEqual(fixed(1.25,1),'1.3')
        self.assertEqual(fixed(1.005,2),'1.00')


def suite():
    return unittest.defaultTestLoader.loadTestsFromTestCase(ReplayComparisonTests)


if __name__ == '__main__':
    unittest.main()

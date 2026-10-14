"""Fast recorder refusal, coverage and crop tests; no browser or server."""
import importlib.util
import base64
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"scripts"))
import demo_walk_record_lib as lib
from PIL import Image

spec=importlib.util.spec_from_file_location("demo_walk_record",ROOT/"scripts/demo-walk-record.py")
driver=importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)


def frames(times):
    return [{"index":i,"timestamp_s":t,"path":f"frames/{i:06d}.jpg"} for i,t in enumerate(times)]


class RecorderTests(unittest.TestCase):
    def test_exact_cdp_duplicate_is_acknowledged_but_not_counted(self):
        class Events:
            def on(self,*args): pass
            def send(self,*args): self.acks += 1
            acks = 0
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); (root/"frames").mkdir()
            session=Events()
            capture=driver.Capture(Events(),session,root,12)
            def event(color,timestamp):
                buf=io.BytesIO()
                Image.new("RGB",(driver.WIDTH,driver.HEIGHT),color).save(buf,format="JPEG")
                return {"metadata":{"timestamp":timestamp},"sessionId":1,
                        "data":base64.b64encode(buf.getvalue()).decode()}
            same=event("red",100)
            capture.frame(same); capture.frame(same)
            capture.guard()
            self.assertEqual(len(capture.frames),1)
            self.assertEqual(session.acks,2)
            capture.frame(event("blue",100))
            with self.assertRaises(lib.Refusal) as failed:
                capture.guard()
            self.assertEqual(failed.exception.code,5)

    def test_frame_count_cannot_hide_a_hole(self):
        with self.assertRaises(lib.Refusal) as failed:
            lib.coverage(frames([0.1,0.2,0.3,0.4,2.1,2.2,2.3,2.4,2.5,2.6,2.7,2.8]),0,3,4)
        self.assertEqual(failed.exception.code,5)
        self.assertIn("gap",str(failed.exception))

    def test_edge_coverage_and_starvation(self):
        result=lib.coverage(frames([0.1,0.35,0.6,0.85]),0,1,4)
        self.assertEqual(result["actual_frames"],4)
        with self.assertRaises(lib.Refusal):
            lib.coverage(frames([0.1,0.4,0.7]),0,1,4)
        with self.assertRaises(lib.Refusal):
            lib.coverage(frames([0.6,0.7,0.8,0.9]),0,1,4)

    def test_sampling_uses_timestamps_instead_of_event_count(self):
        source=frames([10,10.01,10.24,10.26,10.50,10.99,11.0])
        indices=lib.sample_indices(source,{"start_frame":0,"end_frame":6,"fps":4})
        self.assertEqual(indices,[0,2,4,6])

    def test_crop_join_keeps_source_text_scale(self):
        image=Image.new("RGB",(100,60),"red")
        image.paste("blue",(50,0,100,60))
        crop=[[[0,0,20,20],[50,0,20,20]],[[0,20,40,20]]]
        result=lib.compose(image,crop,40)
        self.assertEqual(result.size,(40,40))
        self.assertEqual(result.getpixel((25,5)),(0,0,255))
        with self.assertRaises(lib.Refusal):
            lib.compose(image,[90,0,20,20],40)

    def test_gif_animation_and_strict_byte_limit(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);(root/"frames").mkdir()
            for i in range(41):
                color = "red" if i%2 == 0 and i != 40 else "blue"
                Image.new("RGB",(100,60),color).save(root/f"frames/{i:06d}.jpg")
            source=frames([i/10 for i in range(41)])
            definition={"path":"clip.gif","start_frame":0,"end_frame":40,"fps":10,"width":100,"crop":[0,0,100,60]}
            lib.write_gif(root,source,definition)
            self.assertEqual(definition["decoded_frame_count"],40)
            self.assertEqual(definition["bytes"],(root/"clip.gif").stat().st_size)
            with patch.object(lib,"GIF_LIMIT",definition["bytes"]), self.assertRaises(lib.Refusal) as failed:
                lib.write_gif(root,source,definition)
            self.assertEqual(failed.exception.code,6)
            Image.new("RGB",(100,60),"red").save(root/"frames/000001.jpg")
            with self.assertRaises(lib.Refusal) as failed:
                lib.write_gif(root,source,definition)
            self.assertEqual(failed.exception.code,8)
            self.assertIn("consecutive",str(failed.exception))

    def test_identical_frames_refused_before_writing_gif(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); (root/"frames").mkdir()
            source=frames([i/10 for i in range(41)])
            for f in source:
                Image.new("RGB",(100,60),"red").save(root/f["path"])
            definition={"path":"still.gif","start_frame":0,"end_frame":40,
                        "fps":10,"width":100,"crop":[0,0,100,60]}
            with self.assertRaises(lib.Refusal) as failed:
                lib.write_gif(root,source,definition)
            self.assertEqual(failed.exception.code,8)
            self.assertIn("motion",str(failed.exception))
            self.assertFalse((root/"still.gif").exists())

    def test_short_moving_clip_refused_before_writing_gif(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); (root/"frames").mkdir()
            source=frames([i/10 for i in range(21)])
            for f in source:
                Image.new("RGB",(100,60),"red" if f["index"]%2 else "blue").save(root/f["path"])
            definition={"path":"short.gif","start_frame":0,"end_frame":20,
                        "fps":10,"width":100,"crop":[0,0,100,60]}
            with self.assertRaises(lib.Refusal) as failed:
                lib.write_gif(root,source,definition)
            self.assertEqual(failed.exception.code,7)
            self.assertIn("frame count",str(failed.exception))
            self.assertFalse((root/"short.gif").exists())

    def test_two_states_and_return_to_start_are_motion_refusals(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); (root/"frames").mkdir()
            source=frames([i/10 for i in range(41)])
            definition={"path":"clip.gif","start_frame":0,"end_frame":40,
                        "fps":10,"width":100,"crop":[0,0,100,60]}
            for mode,reason in [("two states","consecutive"),("return","first/last")]:
                for f in source:
                    i=f["index"]
                    color = ("red" if i<20 else "blue") if mode=="two states" else (
                        "red" if i==40 else ["red","blue","green"][i%3])
                    Image.new("RGB",(100,60),color).save(root/f["path"])
                with self.assertRaises(lib.Refusal) as failed:
                    lib.write_gif(root,source,definition)
                self.assertEqual(failed.exception.code,8)
                self.assertIn(reason,str(failed.exception))
                self.assertFalse((root/"clip.gif").exists())

    def test_recorded_chart_must_visibly_change(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);(root/"frames").mkdir()
            source=frames([0,0.5,1])
            for i in range(3):
                Image.new("RGB",(100,60),"red").save(root/f"frames/{i:06d}.jpg")
            with self.assertRaises(lib.Refusal) as failed:
                lib.chart_change(root,source,[0,1,2],[0,0,100,60])
            self.assertEqual(failed.exception.code,2)
            Image.new("RGB",(100,60),"blue").save(root/"frames/000002.jpg")
            result=lib.chart_change(root,source,[0,1,2],[0,0,100,60])
            self.assertEqual(result["changed_frame"],2)
            self.assertEqual(result["changed_pixel_fraction"],1)

    def test_failure_never_publishes_partial_media(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            for code in (2,3,4,5,6,7,8):
                def refuse(args,staging):
                    (staging/"partial.gif").write_bytes(b"partial")
                    raise lib.Refusal(code,f"test refusal {code}")
                with patch.object(driver,"RESULTS",root),patch.object(driver,"record",refuse),patch.object(sys,"argv",["recorder","--mock","--run-id",f"refused-{code}"]):
                    self.assertEqual(driver.main(),code)
                self.assertFalse((root/f"refused-{code}").exists())
                self.assertFalse(list(root.glob(".*")))
                self.assertEqual(json.loads((root/f"refused-{code}.refusal.json").read_text())["exit_code"],code)

    def _minimal_walk(self, window=None):
        observation = {"panel": {"detail": "ok"}, "headlines": [], "table_rows": [], "readouts": []}
        tab = {"tab": "overview", "dwell": {"start_s": 0.0, "end_s": 1.0}, "frame_indices": [0],
               "observations": [observation], "console_errors": []}
        walk = {"source": "test", "tabs": [tab],
                "drill": {"query_row": ["q"], "execution_row": ["e"], "wait_readout": "CPU"},
                "runbook": {"ambiguities": []}}
        if window is not None:
            walk["window"] = window
        return walk

    def test_write_analysis_labels_archived_window(self):
        # Archived window info present -> analysis.md must say so explicitly,
        # distinguishing it from the live-demo gesture, with the real span.
        walk = self._minimal_walk({"mode": "archived-full-span", "from_ns": 10, "to_ns": 2_600_000_010,
                                    "span_s": 2.6})
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            lib.write_analysis(root, walk)
            text = (root/"analysis.md").read_text()
            self.assertIn("ARCHIVED capture", text)
            self.assertIn("NOT the live", text)
            self.assertIn("2.6s", text)

    def test_write_analysis_says_nothing_about_window_for_live_walk(self):
        # Absent (missing-key default, as a real pre-flag walk.json would
        # have) and explicit live-15min window must both produce NO
        # "Window:" line -- the live path stays textually unchanged.
        for window in (None, {"mode": "live-15min"}):
            walk = self._minimal_walk(window)
            with tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                lib.write_analysis(root, walk)
                text = (root/"analysis.md").read_text()
                self.assertNotIn("Window:", text, window)

    def test_archived_flag_against_mock_is_refused_before_any_browser_work(self):
        # --archived only makes sense against a real bridge: --mock always
        # serves data anchored to wall-clock "now", so the input that must
        # be refused -- cheaply, before touching Chrome -- is --mock plus
        # --archived together.
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with patch.object(driver, "RESULTS", root), \
                 patch.object(sys, "argv", ["recorder", "--mock", "--archived", "--run-id", "x"]):
                self.assertEqual(driver.main(), 6)
            self.assertFalse((root/"x").exists())

    # --archived: smoke._navigate_to_tab resumes live mode on any tab switch
    # that landed paused, which re-anchors the window to real wall-clock NOW
    # (web/static/app.js initLiveMode) -- empty panels for an archived
    # capture whose data ends in the past. navigate() must restore the
    # trace's own full-span window ("All", data-range="0") after every
    # successful navigation when archived=True, and must NOT touch the time
    # picker at all for a live walk (archived=False, the default).
    class _ClickRecorder:
        def __init__(self, calls):
            self.calls = calls

        def locator(self, selector):
            self.calls.append(selector)
            return self

        def click(self):
            pass

        def wait_for_timeout(self, ms):
            pass

    def test_navigate_reasserts_archived_window_after_every_tab(self):
        calls = []
        class Smoke:
            def _navigate_to_tab(self, page, tab, timeout):
                pass
        class Capture:
            current = None
            def guard(self): pass
        driver.navigate(self._ClickRecorder(calls), "events", Smoke(), Capture(), 1, archived=True)
        self.assertEqual(calls, ["#time-range", '#time-picker button[data-range="0"]'])

    def test_navigate_never_touches_time_picker_for_a_live_walk(self):
        calls = []
        class Smoke:
            def _navigate_to_tab(self, page, tab, timeout):
                pass
        class Capture:
            current = None
            def guard(self): pass
        driver.navigate(self._ClickRecorder(calls), "events", Smoke(), Capture(), 1)
        self.assertEqual(calls, [])

    def test_unchanged_wait_mix_refuses_a_live_walk(self):
        same = {"table_rows": [["CPU*", "x", "50.0%"], ["IO", "x", "50.0%"]]}
        with self.assertRaises(lib.Refusal) as failed:
            driver.check_overview_wait_mix([same, dict(same)], archived=False)
        self.assertEqual(failed.exception.code, 2)

    def test_changed_wait_mix_passes_a_live_walk(self):
        first = {"table_rows": [["CPU*", "x", "50.0%"], ["IO", "x", "50.0%"]]}
        second = {"table_rows": [["CPU*", "x", "40.0%"], ["IO", "x", "60.0%"]]}
        driver.check_overview_wait_mix([first, second], archived=False)  # must not raise

    def test_unchanged_wait_mix_is_expected_and_allowed_when_archived(self):
        # An archived walk's window is pinned to the trace's own fixed span
        # (reassert_archived_window) -- nothing in it advances, so the same
        # input that refuses a live walk above must NOT refuse here.
        same = {"table_rows": [["CPU*", "x", "50.0%"], ["IO", "x", "50.0%"]]}
        driver.check_overview_wait_mix([same, dict(same)], archived=True)  # must not raise

    def test_observed_empty_is_distinct_from_failed_observation(self):
        class Capture:
            current="events"
            def guard(self): pass
        class Page:
            def evaluate(self,code): return {"ok":False,"detail":"no rows"}
        class Smoke:
            lib=type("Lib",(),{"render_check_ok":staticmethod(lambda r:(r["ok"],r["detail"]))})
            PANEL_CHECKS={"events":"check"}
            def _navigate_to_tab(self,*args): raise TimeoutError("never ready")
            def _view_mount(self,page): return self.mount
        smoke=Smoke()
        for mount,expected in [({"id":"events"},2),({"id":"overview"},3)]:
            smoke.mount=mount
            with self.assertRaises(lib.Refusal) as failed:
                driver.navigate(Page(),"events",smoke,Capture(),1)
            self.assertEqual(failed.exception.code,expected)


if __name__ == "__main__":
    unittest.main()

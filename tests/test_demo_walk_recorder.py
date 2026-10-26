"""Fast recorder refusal, coverage and crop tests; no browser or server."""
import importlib.util
import base64
import io
import json
import math
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

    def test_any_recorded_error_still_refuses(self):
        # No error kind is exempt (#401's favicon 404 is handled by serving
        # the asset -- web/main.go -- never by teaching this gate to look
        # away from a message). Capture.error() fires for exactly TWO kinds
        # -- "console" and "pageerror" (Capture.__init__'s two page.on
        # registrations) -- and BOTH are exercised here, nothing favicon-
        # shaped about either: a test named "any" that covered only one kind
        # would demonstrate the name for one instance and claim it for all,
        # which is how a guard goes quietly vacuous the day a third kind is
        # added and nothing here would have caught it staying unguarded.
        # Each kind fails for TWO distinct reasons if the gate is weakened,
        # both load-bearing on this test's name, not just the one most
        # recently seen:
        #   - guard() stops raising entirely (the gate is gone)
        #   - guard() starts excluding a specific error kind (the gate grew
        #     a hole, e.g. an overbroad future fix for one noisy message)
        # This test is about a RECORDED error refusing; it says nothing
        # about an error that is never recorded at all -- #571 (a dead
        # daemon reading as stale-but-populated data) is exactly that case
        # and this test does not, and cannot, cover it.
        class Events:
            def on(self,*args): pass
        for kind, message in (("pageerror","TypeError: x is not a function"),
                              ("console","Failed to load resource: the server "
                               "responded with a status of 404 (Not Found)")):
            with tempfile.TemporaryDirectory() as temp:
                capture=driver.Capture(Events(),Events(),Path(temp),12)
                capture.error(kind,message)
                with self.assertRaises(lib.Refusal) as failed:
                    capture.guard()
                self.assertEqual(failed.exception.code,4,kind)

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

    def test_rect_refuses_an_off_screen_element_naming_it(self):
        """issue #413: a negative width/height must never be constructed by
        rect() -- it used to surface three steps downstream in compose() as
        an opaque "crop outside captured frame" with no element name. Must
        refuse HERE, naming the selector and its off-screen position."""
        class Box:
            def __init__(self,box): self.box=box
            def bounding_box(self): return self.box
        class Page:
            def __init__(self,box): self.box=box
            def locator(self,selector):
                self.selector=selector
                return Box(self.box)
        # Below the fold: top (1048) alone exceeds HEIGHT (981).
        page=Page({"x":20,"y":1048,"width":1670,"height":34})
        with self.assertRaises(lib.Refusal) as failed:
            driver.rect(page,"#waterfall-readout")
        self.assertEqual(failed.exception.code,3)
        self.assertIn("#waterfall-readout",str(failed.exception))
        self.assertIn("off-screen",str(failed.exception))

    def test_rect_still_works_for_an_on_screen_element(self):
        class Box:
            def __init__(self,box): self.box=box
            def bounding_box(self): return self.box
        class Page:
            def locator(self,selector): return Box({"x":20,"y":100,"width":200,"height":50})
        self.assertEqual(driver.rect(Page(),"#aas-chart-container"),[20,100,200,50])

    def test_select_varying_query_prefers_spread_over_raw_duration(self):
        """drill-down composition fix: a query whose p95 ~= average (fixed-duration, like
        this demo's pg_advisory_lock(id) holds) must lose to one with real
        spread, even when the fixed-duration one has the bigger raw number
        -- the old rule picked exactly the wrong one here (ten identical
        3.0s rows), which is the defect this replaces."""
        candidates = [
            {"index": 0, "p95": "3.0s", "avg": "3.0s"},       # fixed, huge, no spread
            {"index": 1, "p95": "1.0s", "avg": "500.7ms"},    # real workload shape: ~2x spread
            {"index": 2, "p95": "100.8ms", "avg": "100.3ms"}, # fixed, small, no spread
        ]
        selected = driver.select_varying_query(candidates)
        self.assertEqual(selected["index"], 1)

    def test_select_varying_query_refuses_when_nothing_varies(self):
        """drill-down composition fix: if EVERY visible candidate is fixed-duration (p95
        within 20% of its own average), that is a real workload-variety
        finding, not something to paper over by picking the biggest
        number anyway -- must refuse distinctly."""
        candidates = [
            {"index": 0, "p95": "3.0s", "avg": "3.0s"},
            {"index": 1, "p95": "100.8ms", "avg": "100.3ms"},
        ]
        with self.assertRaises(lib.Refusal) as failed:
            driver.select_varying_query(candidates)
        self.assertEqual(failed.exception.code, 2)
        self.assertIn("varying", str(failed.exception))

    def test_rect_contains_tooltip_fully_inside(self):
        outer = [100, 100, 500, 300]  # x,y,w,h -> right=600, bottom=400
        inner = {"top": 150, "left": 150, "right": 550, "bottom": 350}
        self.assertTrue(driver._rect_contains(outer, inner))

    def test_rect_contains_refuses_right_edge_clip(self):
        """issue found live: the tooltip clipped on the RIGHT when the sweep
        reached the end of its range -- the clip moved, it did not go away.
        A tooltip whose right edge is past the crop's own right edge must
        be detected, regardless of how well it fits on the left/top."""
        outer = [100, 100, 500, 300]  # right edge at 600
        inner = {"top": 150, "left": 550, "right": 650, "bottom": 350}
        self.assertFalse(driver._rect_contains(outer, inner))

    def test_rect_contains_refuses_left_edge_clip(self):
        outer = [100, 100, 500, 300]  # left edge at 100
        inner = {"top": 150, "left": 50, "right": 300, "bottom": 350}
        self.assertFalse(driver._rect_contains(outer, inner))

    def test_video_verify_timeout_scales_with_duration(self):
        """issue found live: encode_video()'s own decode/seek verification
        used a flat 10s budget for an artifact (walk.webm) whose size
        varies with the walk -- three live runs each ~100-150 frames
        longer than ones that passed failed this SAME check 3/3 times.
        A longer video needs a proportionally larger budget, not a number
        sized for whatever walk happened to be running when it was
        written."""
        self.assertEqual(driver.video_verify_timeout_ms(140_900), 14_090)
        self.assertEqual(driver.video_verify_timeout_ms(300_000), 30_000)

    def test_video_verify_timeout_never_shrinks_below_the_original_floor(self):
        """A short clip's budget must never be LESS than the flat 10s that
        has always worked -- the floor means this change can only add
        margin, never remove it."""
        for duration_ms in [0, 1000, 50_000, 99_999]:
            self.assertGreaterEqual(driver.video_verify_timeout_ms(duration_ms),
                                    driver.VERIFY_MIN_MS)

    def test_video_verify_timeout_is_always_finite_and_bounded(self):
        """The check must keep its teeth: whatever the formula computes is
        always a concrete, finite number of milliseconds -- a genuinely
        stuck/undecodable video still times out eventually; it is never
        given an unbounded budget."""
        for duration_ms in [0, 140_900, 1_000_000]:
            t = driver.video_verify_timeout_ms(duration_ms)
            self.assertTrue(math.isfinite(t))
            self.assertLess(t, 10_000_000)  # sanity ceiling: never absurdly large

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
            # issue #411/#413: a SINGLE corrupted/duplicated frame among 40
            # otherwise-alternating ones trips neither gate any more. The
            # motion gate doesn't care (best-single-step and first/last are
            # both still satisfied by the other 39 frames). The frame-count
            # gate doesn't either, now that it is re-expressed as "enough
            # DISTINCT frames for a watchable animation" (DECODED_MIN_FPS)
            # rather than "every sampled frame must survive encoding": one
            # coalesced pair out of 40 leaves plenty of real animation. This
            # IS the correct outcome -- the content genuinely still moves.
            Image.new("RGB",(100,60),"red").save(root/"frames/000001.jpg")
            lib.write_gif(root,source,definition)
            self.assertGreaterEqual(definition["decoded_frame_count"],8)

    def test_frame_count_gate_still_refuses_a_genuine_slideshow(self):
        """issue #413 explicitly requires the re-expressed frame-count gate
        keep its teeth: a clip that is genuinely just two static blocks (a
        real slideshow, not an animation -- passes the MOTION gate, since
        first/last and the one real step both clear it) must still refuse
        on frame count, because Pillow correctly coalesces each block down
        to one frame: 2 distinct frames is not a watchable 10-second clip,
        at any floor worth having."""
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); (root/"frames").mkdir()
            n = 101  # 10 seconds at 10fps
            source=frames([i/10 for i in range(n)])
            for f in source:
                color = "red" if f["index"]<50 else "blue"
                Image.new("RGB",(100,60),color).save(root/f["path"])
            definition={"path":"slideshow.gif","start_frame":0,"end_frame":n-1,
                        "fps":10,"width":100,"crop":[0,0,100,60]}
            with self.assertRaises(lib.Refusal) as failed:
                lib.write_gif(root,source,definition)
            self.assertEqual(failed.exception.code,7)
            self.assertIn("distinct decoded frames",str(failed.exception))
            self.assertFalse((root/"slideshow.gif").exists())

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

    def test_return_to_start_is_still_a_motion_refusal_end_to_end(self):
        """A clip with plenty of frame-to-frame movement but whose LAST
        frame matches its FIRST (nothing net visible happened) must still
        refuse -- issue #411 changed WHICH pairs the gate checks, not
        whether a clip that shows nothing overall can pass."""
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); (root/"frames").mkdir()
            source=frames([i/10 for i in range(41)])
            definition={"path":"clip.gif","start_frame":0,"end_frame":40,
                        "fps":10,"width":100,"crop":[0,0,100,60]}
            for f in source:
                i=f["index"]
                color = "red" if i==40 else ["red","blue","green"][i%3]
                Image.new("RGB",(100,60),color).save(root/f["path"])
            with self.assertRaises(lib.Refusal) as failed:
                lib.write_gif(root,source,definition)
            self.assertEqual(failed.exception.code,8)
            self.assertIn("first/last",str(failed.exception))
            self.assertFalse((root/"clip.gif").exists())

    def test_motion_gate_two_states_with_one_real_step_now_passes(self):
        """issue #411: the product ticks every 5s, so a clip's real shape
        is two static states joined by ONE jump (the tick landing) -- the
        OLD per-consecutive-pair rule refused exactly this correct shape
        (measured live: ~0.016% per pair vs. a 0.100% floor, on four
        independent live runs). The new first/last + best-single-step rule
        must accept it."""
        pictures = ([Image.new("RGB",(10,10),"red")]*20
                    + [Image.new("RGB",(10,10),"blue")]*20)
        result = lib.motion_gate(pictures, "clip.gif", "sampled crop")
        self.assertAlmostEqual(result["first_last_changed_fraction"], 1.0)
        self.assertAlmostEqual(result["best_consecutive_changed_fraction"], 1.0)
        self.assertEqual(result["minimum_consecutive_changed_fraction"], 0.0)

    def test_motion_gate_entirely_static_clip_still_refuses(self):
        """issue #411 explicitly requires the re-expressed gate keep its
        teeth: a genuinely frozen clip -- nothing ticks, nothing moves,
        every frame identical -- must still refuse."""
        pictures = [Image.new("RGB",(10,10),"red")]*10
        with self.assertRaises(lib.Refusal) as failed:
            lib.motion_gate(pictures,"clip.gif","sampled crop")
        self.assertEqual(failed.exception.code,8)
        self.assertIn("first/last",str(failed.exception))

    def test_motion_gate_tolerates_one_corrupted_frame_in_a_moving_clip(self):
        """A single flickered/corrupted frame must not sink an otherwise
        genuinely moving clip -- the whole point of best-single-step
        alongside first/last instead of demanding every pair move."""
        pictures = [Image.new("RGB",(10,10),"red" if i%2==0 else "blue") for i in range(40)]
        pictures[1] = pictures[0].copy()  # corrupt one frame to match its neighbor
        result = lib.motion_gate(pictures,"clip.gif","sampled crop")
        self.assertGreater(result["best_consecutive_changed_fraction"],0.5)

    def test_young_capture_refusal_is_none_when_capture_covers_the_window_start(self):
        window_from_ns = 1_000_000_000_000
        for server_from_ns in (window_from_ns - 42_000_000_000, window_from_ns):
            with self.subTest(server_from_ns=server_from_ns):
                self.assertIsNone(lib.young_capture_refusal(server_from_ns, window_from_ns))

    def test_young_capture_refusal_names_the_age_when_capture_starts_after_window(self):
        window_from_ns = 1_000_000_000_000
        server_from_ns = window_from_ns + 42_000_000_000
        refusal = lib.young_capture_refusal(server_from_ns, window_from_ns)
        self.assertIsInstance(refusal, lib.Refusal)
        self.assertEqual(refusal.code, 9)
        self.assertIn("42.0s", str(refusal))

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
                    (staging/"frames").mkdir()
                    (staging/"frames"/"000000.jpg").write_bytes(b"fake-frame")
                    raise lib.Refusal(code,f"test refusal {code}")
                with patch.object(driver,"RESULTS",root),patch.object(driver,"record",refuse),patch.object(sys,"argv",["recorder","--mock","--run-id",f"refused-{code}"]):
                    self.assertEqual(driver.main(),code)
                self.assertFalse((root/f"refused-{code}").exists())
                self.assertFalse(list(root.glob(".*")))
                self.assertTrue((root/f"refused-{code}.refused"/"frames"/"000000.jpg").exists())
                self.assertEqual(json.loads((root/f"refused-{code}.refused"/"refusal.json").read_text())["exit_code"],code)

    def test_repeated_refusals_preserve_both_attempts(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            run_id="repeated-refusal"
            def first_refusal(args,staging):
                (staging/"frames").mkdir()
                (staging/"frames"/"000000.jpg").write_bytes(b"first-frame")
                raise lib.Refusal(2,"first message")
            def second_refusal(args,staging):
                (staging/"frames").mkdir()
                (staging/"frames"/"000000.jpg").write_bytes(b"second-frame")
                raise lib.Refusal(3,"second message")
            with patch.object(driver,"RESULTS",root), \
                 patch.object(sys,"argv",["recorder","--mock","--run-id",run_id]), \
                 patch.object(sys,"stderr",io.StringIO()):
                with patch.object(driver,"record",first_refusal):
                    self.assertEqual(driver.main(),2)
                first=root/f"{run_id}.refused"
                self.assertTrue(first.is_dir())
                self.assertEqual((first/"frames"/"000000.jpg").read_bytes(),b"first-frame")
                refusal=json.loads((first/"refusal.json").read_text())
                self.assertEqual(refusal["exit_code"],2)
                self.assertEqual(refusal["reason"],"first message")
                original={p.relative_to(first):p.read_bytes() for p in first.rglob("*") if p.is_file()}

                with patch.object(driver,"record",second_refusal):
                    self.assertEqual(driver.main(),3)

            self.assertEqual({p.relative_to(first):p.read_bytes() for p in first.rglob("*") if p.is_file()},original)
            attempts=[p for p in root.glob(f"{run_id}.refused*") if p.is_dir()]
            self.assertEqual(len(attempts),2)
            second=next(p for p in attempts if p != first)
            self.assertEqual((second/"frames"/"000000.jpg").read_bytes(),b"second-frame")
            refusal=json.loads((second/"refusal.json").read_text())
            self.assertEqual(refusal["exit_code"],3)
            self.assertEqual(refusal["reason"],"second message")
            self.assertFalse((root/run_id).exists())
            self.assertFalse(list(root.glob(".*")))

    def test_preservation_failure_leaves_staging_intact(self):
        for step in ("write_text","rename"):
            with self.subTest(step=step), tempfile.TemporaryDirectory() as temp:
                root=Path(temp)
                staging_paths=[]
                before={}
                def contents(staging):
                    return {p.relative_to(staging):None if p.is_dir() else p.read_bytes()
                            for p in staging.rglob("*")}
                def refuse(args,staging):
                    staging_paths.append(staging)
                    (staging/"frames").mkdir()
                    (staging/"frames"/"000000.jpg").write_bytes(b"diagnostic-frame")
                    raise lib.Refusal(4,"recording failed")
                def fail_preservation(*args,**kwargs):
                    before.update(contents(staging_paths[0]))
                    raise OSError(f"simulated {step} failure")
                failure=patch.object(Path,"write_text",fail_preservation) if step == "write_text" else \
                        patch.object(driver.os,"rename",fail_preservation)
                with patch.object(driver,"RESULTS",root),patch.object(driver,"record",refuse), \
                     patch.object(sys,"argv",["recorder","--mock","--run-id","preservation-failure"]), \
                     patch.object(sys,"stderr",io.StringIO()) as stderr, failure:
                    self.assertEqual(driver.main(),4)

                self.assertEqual(len(staging_paths),1)
                staging=staging_paths[0]
                self.assertTrue(staging.is_dir())
                self.assertEqual(contents(staging),before)
                self.assertEqual((staging/"frames"/"000000.jpg").read_bytes(),b"diagnostic-frame")
                if step == "rename":
                    refusal=json.loads((staging/"refusal.json").read_text())
                    self.assertEqual(refusal["reason"],"recording failed")
                    self.assertEqual(refusal["exit_code"],4)
                self.assertIn("WARNING",stderr.getvalue())
                self.assertIn(str(staging),stderr.getvalue())
                self.assertIn(f"simulated {step} failure",stderr.getvalue())

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
        result = driver.check_overview_wait_mix([first, second], archived=False)
        self.assertEqual(result, driver.WAIT_MIX_CHECKED_LIVE)

    def test_unchanged_wait_mix_is_expected_and_allowed_when_archived(self):
        # An archived walk's window is pinned to the trace's own fixed span
        # (reassert_archived_window) -- nothing in it advances, so the same
        # input that refuses a live walk above must NOT refuse here. The
        # skip must also be RECORDED (not merely "not raise"), so a reader
        # of walk.json/analysis.md never mistakes it for a check that ran.
        same = {"table_rows": [["CPU*", "x", "50.0%"], ["IO", "x", "50.0%"]]}
        result = driver.check_overview_wait_mix([same, dict(same)], archived=True)
        self.assertEqual(result, driver.WAIT_MIX_SKIPPED_ARCHIVED)
        self.assertIn("skipped", result)
        self.assertIn("archived", result)

    def test_write_analysis_states_a_skipped_wait_mix_check_explicitly(self):
        # A skipped assertion that reads like a passed one is the defect
        # class this project has hit repeatedly. The overview tab's
        # recorded "wait_mix_check" marker must appear verbatim in
        # analysis.md, not be inferable only from the window mode or from
        # "stayed unchanged" (which also describes a genuinely-live walk
        # that happened to sample two identical ticks).
        walk = self._minimal_walk({"mode": "archived-full-span", "from_ns": 10,
                                    "to_ns": 2_600_000_010, "span_s": 2.6})
        walk["tabs"][0]["wait_mix_check"] = driver.WAIT_MIX_SKIPPED_ARCHIVED
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            lib.write_analysis(root, walk)
            text = (root/"analysis.md").read_text()
            self.assertIn(driver.WAIT_MIX_SKIPPED_ARCHIVED, text)

    def test_write_analysis_omits_wait_mix_line_when_absent(self):
        # Backward compatibility: an older walk.json with no wait_mix_check
        # key (or a live walk where nothing stored one) must not grow a
        # phantom line.
        walk = self._minimal_walk(None)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            lib.write_analysis(root, walk)
            text = (root/"analysis.md").read_text()
            self.assertNotIn("wait-mix assertion", text)

    def test_pulsing_wait_pulses_every_iteration_not_once(self):
        """issue #409 regression. The real defect only shows against a
        genuinely remote bridge (latency is the whole mechanism; the mock
        has none), so this cannot reproduce IT -- what it can and does
        check is the fixed CONTRACT: pulsing_wait must call capture.pulse()
        on every poll iteration, not once before a long wait the way the
        old `capture.pulse(); <single blocking call>` shape in drill() did.
        A predicate that only turns true on its 5th check must see at
        least 5 pulses -- proving the capture gets a chance at a fresh
        compositor frame throughout the wait, not just at its start. No
        real sleeping: wait_for_timeout is a no-op, so this is instant and
        cannot be runner-timing-flaky."""
        class FakeCapture:
            fps = 12
            def __init__(self): self.pulses = 0
            def pulse(self): self.pulses += 1
            def guard(self): pass
        class FakePage:
            def wait_for_timeout(self, ms): pass
        capture = FakeCapture()
        calls = []
        def predicate():
            calls.append(1)
            return len(calls) >= 5
        result = driver.pulsing_wait(FakePage(), capture, predicate, timeout_s=5.0)
        self.assertTrue(result)
        self.assertEqual(capture.pulses, len(calls))
        self.assertGreaterEqual(capture.pulses, 5)

    def test_pulsing_wait_times_out_but_keeps_pulsing_throughout(self):
        """A predicate that never comes true must raise after timeout_s,
        having pulsed repeatedly (not just once) right up to the deadline
        -- the failure path must not regress to the old single-pulse
        shape either."""
        class FakeCapture:
            fps = 12
            def __init__(self): self.pulses = 0
            def pulse(self): self.pulses += 1
            def guard(self): pass
        class FakePage:
            def wait_for_timeout(self, ms): pass
        capture = FakeCapture()
        with self.assertRaises(TimeoutError):
            driver.pulsing_wait(FakePage(), capture, lambda: False, timeout_s=0.01)
        self.assertGreater(capture.pulses, 1)

    def test_pulsing_wait_propagates_predicate_exception_immediately(self):
        """_view_mount's missing-hook case must still fail LOUD, never be
        treated as 'not ready yet' and deferred until the timeout (see
        _view_mount's own docstring in tests/ui_live_smoke.py) -- the
        pulsing poll loop must not swallow it."""
        class FakeCapture:
            fps = 12
            def pulse(self): pass
            def guard(self): pass
        class FakePage:
            def wait_for_timeout(self, ms): pass
        def predicate():
            raise RuntimeError("hook missing")
        with self.assertRaises(RuntimeError):
            driver.pulsing_wait(FakePage(), FakeCapture(), predicate, timeout_s=5.0)

    def test_fresh_mount_reuses_mount_is_fresh_not_a_reimplementation(self):
        """_fresh_mount is a thin predicate adapter for pulsing_wait, not a
        second copy of the freshness rule -- it must return exactly what
        mount_is_fresh says, both when fresh and when stale."""
        class FreshLib:
            mount_is_fresh = staticmethod(lambda mount, tab_id, tick_ts_ms, min_seq: mount["seq"] > 1)
        class Smoke:
            lib = FreshLib
            def _view_mount(self, page): return self.mount
        smoke = Smoke()
        smoke.mount = {"id": "waterfall", "seq": 2, "at": 100}
        self.assertEqual(driver._fresh_mount(smoke, None, "waterfall", 0, None), smoke.mount)
        smoke.mount = {"id": "waterfall", "seq": 1, "at": 100}
        self.assertIsNone(driver._fresh_mount(smoke, None, "waterfall", 0, None))

    def test_best_headline_window_picks_the_moving_span_not_a_fixed_offset(self):
        """issue #409 follow-up: three live runs refused the headline GIF's
        motion gate at a FIXED early 0.3-5.3s offset while chart_change()'s
        much looser check (best later frame vs frame 0) had already passed
        on the same dwell -- meaning real motion existed somewhere in the
        dwell that the fixed window simply was not looking at.
        best_headline_window must find it: given a pool where only the
        SECOND half of the recording changes frame to frame (first half is
        a flat run of identical frames, like the fixed window hit), it must
        return a span with a real score -- not silently fall back to the
        flat start, where motion_gate()'s own first/last AND best-step
        checks (the score this function maximizes, matching #411's
        re-expressed gate exactly) would both be 0."""
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); (root/"frames").mkdir()
            colors = ["red","red","red","red","blue","green","yellow","white","cyan"]
            frames = []
            for i, color in enumerate(colors):
                path = f"frames/{i:06d}.jpg"
                Image.new("RGB",(20,20),color).save(root/path, format="JPEG")
                frames.append({"index": i, "timestamp_s": float(i), "path": path})
            start, end, score = driver.best_headline_window(
                root, frames, list(range(len(frames))), fps=1, crop=[0,0,20,20],
                width=20, window_s=3.0)
            # A window entirely inside the flat run (any span within 0-3) has
            # first==last and every consecutive pair identical -- score 0.0.
            self.assertGreater(score, 0.5)
            self.assertFalse(start == 0 and end <= 3, "picked the pure-flat window")

    def test_best_headline_window_reports_none_when_pool_too_short(self):
        """A pool shorter than the requested window cannot be scanned --
        must say so (None), never invent a false positive, falls back to
        the whole pool's own bounds."""
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); (root/"frames").mkdir()
            frames = []
            for i in range(3):
                path = f"frames/{i:06d}.jpg"
                Image.new("RGB",(10,10),"red").save(root/path, format="JPEG")
                frames.append({"index": i, "timestamp_s": float(i)*0.5, "path": path})
            start, end, worst = driver.best_headline_window(
                root, frames, [0,1,2], fps=1, crop=[0,0,10,10], width=10, window_s=5.0)
            self.assertIsNone(worst)
            self.assertEqual((start, end), (0, 2))

    def test_best_headline_window_rejects_a_spike_that_returns_to_start(self):
        """issue #411 follow-up, found live: an earlier version of this
        function scored only the MINIMUM consecutive-pair delta, matching
        the OLD per-pair gate. After #411 re-expressed motion_gate() as
        first/last AND best-single-step (both required), that old scoring
        could still pick a window with a big middle spike that returns to
        its starting color -- first/last near 0 -- which motion_gate()
        then correctly refused on "first/last" on a real live run. The
        score here must be min(endpoint, best_step), so a window that
        spikes and returns (good best_step, zero endpoint) loses to one
        that is good on BOTH axes, even though the spike-and-return window
        has an identical or larger raw consecutive jump."""
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); (root/"frames").mkdir()
            # 0-3: red,red,blue,red -- big spike (red->blue), but ends back at
            # red: first==last==red, endpoint=0, even though best_step=1.0.
            # 3-6: red,red,green,yellow -- ends on a genuinely different
            # color: both endpoint and best_step are strong.
            colors = ["red","red","blue","red","red","green","yellow"]
            frames = []
            for i, color in enumerate(colors):
                path = f"frames/{i:06d}.jpg"
                Image.new("RGB",(20,20),color).save(root/path, format="JPEG")
                frames.append({"index": i, "timestamp_s": float(i), "path": path})
            # fps=10 (dense relative to these 1s-apart frames, matching the
            # real headline's own fps=10) so each candidate's sampled
            # sequence actually contains the spike frame, not just the
            # window's two endpoints.
            start, end, score = driver.best_headline_window(
                root, frames, list(range(len(frames))), fps=10, crop=[0,0,20,20],
                width=20, window_s=3.0)
            self.assertGreater(score, 0.5)
            self.assertFalse(start == 0 and end == 3, "picked the spike-and-return window")

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

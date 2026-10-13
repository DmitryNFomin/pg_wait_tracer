#!/usr/bin/env python3
"""Record the pinned Chrome demo; see docs/DEMO_WALK_RECORDER.md."""
import argparse
import base64
from contextlib import ExitStack
import hashlib
import io
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))

from demo_walk_record_lib import (Refusal, chart_change, coverage, frame_indices, save_walk,
                                  write_analysis, write_gif)

WIDTH, HEIGHT, DPR = 1710, 981, 2
RESULTS = ROOT / "tests/results/demo_walk"
AMBIGUITIES = [
    "Section 4 specifies no per-tab dwell times; explicit default is 10 seconds per tab, overridable by --dwells JSON.",
    "Section 4 says 'in the order the UI lists them' but Timeline requires a return to Sessions after Histogram; the numbered order is retained and that return is a navigation step.",
    "Section 4 mentions Queries → Waterfall's query_id pivot; ordinary row click goes to Events. The visible Executions → cell opens Waterfall.",
]

OBSERVE_JS = """() => {
    const visible = e => {
        const r = e.getBoundingClientRect(), s = getComputedStyle(e);
        return s.visibility !== 'hidden' && s.display !== 'none' &&
            r.width > 0 && r.height > 0 && r.top < innerHeight && r.bottom > 0 &&
            r.left < innerWidth && r.right > 0;
    };
    const text = e => e.innerText.trim().replace(/\\s+/g, ' ');
    const headlines = [...document.querySelectorAll('#summary-bar .metric')]
        .filter(visible).map(e => ({label:text(e.querySelector('.metric-label')),
                                  value:text(e.querySelector('.metric-value'))}));
    const table = document.querySelector('#table-container table');
    const table_rows = table ? [...table.querySelectorAll('tbody tr')].filter(visible)
        .map(r => [...r.querySelectorAll('td')].filter(visible).map(text)) : [];
    const headers = table ? [...table.querySelectorAll('th')].filter(visible).map(text) : [];
    const readouts = [...document.querySelectorAll('#table-container .view-title, '
        + '#table-container .chart-readout, #table-container [id$="banner"], '
        + '#executions-counts-label, #executions-sort-label')].filter(visible).map(text);
    const issues = [...document.querySelectorAll('.pane-error, #version-skew, #degraded-overlay')]
        .filter(visible).map(text);
    return {timestamp_s:Date.now()/1000, headlines, headers, table_rows, readouts, issues,
            status:document.querySelector('#status')?.innerText,
            tab:window.__pgwt?.activeTab, range:document.querySelector('#time-range')?.innerText,
            live:document.querySelector('#live-btn')?.classList.contains('active')};
}"""


def runbook_plan(tabs, overrides):
    body = (ROOT / "docs/DEMO_RUNBOOK.md").read_text()
    section = body.split("## 4. The walk itself", 1)[1].split("## 5.", 1)[0]
    listed = re.findall(r"^\d+\. (?:\*\*)?([A-Z][a-z]+)", section, re.M)
    if [s.lower() for s in listed] != tabs:
        raise Refusal(6, f"runbook order differs from readiness helper: {listed}")
    # Do not silently substitute our defaults if the runbook later gains timings.
    if re.search(r"\bdwell\b|\b\d+\s*(?:seconds?|secs?)\s+(?:on|per|each)\b", section, re.I):
        raise Refusal(6, "runbook has new dwell instructions: update timing data explicitly")
    dwells = {tab: 10.0 for tab in tabs}
    if overrides:
        supplied = json.loads(Path(overrides).read_text())
        if set(supplied) != set(tabs):
            raise Refusal(6, "--dwells must specify exactly all eleven tabs")
        dwells = {k: float(v) for k, v in supplied.items()}
    if any(not math.isfinite(v) or v < 5 for v in dwells.values()):
        raise Refusal(6, "every dwell must be finite and at least five seconds")
    return dwells, {"path": "docs/DEMO_RUNBOOK.md", "sha256": hashlib.sha256(body.encode()).hexdigest(),
                   "tab_order": listed, "dwell_seconds": dwells, "ambiguities": AMBIGUITIES}


class Capture:
    def __init__(self, page, session, root, fps):
        self.page, self.session, self.root, self.fps = page, session, root, fps
        self.frames, self.errors, self.capture_errors = [], [], []
        self.current = "startup"
        self.stopped = False
        page.on("console", lambda m: self.error("console", m.text) if m.type == "error" else None)
        page.on("pageerror", lambda e: self.error("pageerror", str(e)))
        session.on("Page.screencastFrame", self.frame)

    def error(self, kind, message):
        # CDP timestamps and Python wall time are both UTC epoch seconds.
        self.errors.append({"timestamp_s": time.time(), "tab": self.current,
                            "kind": kind, "message": message})

    def frame(self, event):
        from PIL import Image
        try:
            metadata = event["metadata"]
            timestamp = metadata["timestamp"]
            data = base64.b64decode(event["data"], validate=True)
            with Image.open(io.BytesIO(data)) as image:
                dims = image.size
            if dims != (WIDTH, HEIGHT):
                raise ValueError(f"screencast size {dims}, expected {(WIDTH, HEIGHT)}")
            digest = hashlib.sha256(data).hexdigest()
            # A cadence restart and a compositor update can deliver the same
            # native frame twice. Acknowledge that exact duplicate, but never
            # count it toward coverage or invent a later capture timestamp.
            if (self.frames and timestamp == self.frames[-1]["timestamp_s"]
                    and digest == self.frames[-1]["sha256"]):
                return
            if self.frames and timestamp <= self.frames[-1]["timestamp_s"]:
                raise ValueError("screencast timestamp missing, duplicated or regressed")
            index = len(self.frames)
            path = f"frames/{index:06d}.jpg"
            (self.root / path).write_bytes(data)
            self.frames.append({"index": index, "timestamp_s": timestamp, "path": path,
                                "size": list(dims), "metadata": metadata,
                                "sha256": digest})
        except Exception as exc:
            self.capture_errors.append(str(exc))
        finally:
            self.session.send("Page.screencastFrameAck", {"sessionId": event["sessionId"]})

    def pulse(self):
        self.session.send("Page.stopScreencast")
        self.session.send("Page.startScreencast", {"format": "jpeg", "quality": 95,
                                                  "maxWidth": WIDTH, "maxHeight": HEIGHT,
                                                  "everyNthFrame": 1})

    def stop(self):
        if self.stopped:
            return
        self.stopped = True
        self.session.remove_listener("Page.screencastFrame",self.frame)
        try:
            self.session.send("Page.stopScreencast")
        except Exception as exc:
            self.capture_errors.append(f"could not stop screencast: {exc}")

    def guard(self):
        if self.errors:
            raise Refusal(4, f"{self.current}: {self.errors[-1]['kind']}: {self.errors[-1]['message']}")
        if self.capture_errors:
            raise Refusal(5, f"{self.current}: {self.capture_errors[-1]}")


def observe(page, tab, smoke, capture):
    capture.guard()
    ok, detail = smoke._poll_render_check(page, tab)
    result = page.evaluate(OBSERVE_JS)
    result["panel"] = {"ok": ok, "detail": detail}
    capture.guard()
    if result["issues"]:
        raise Refusal(3, f"{tab}: cannot inspect healthy panel: {result['issues']}")
    if not ok:
        raise Refusal(2, f"{tab}: inspected panel was empty: {detail}")
    if result["tab"] != tab:
        raise Refusal(3, f"{tab}: active tab is {result['tab']}")
    return result


def navigate(page, tab, smoke, capture, timeout):
    capture.current = tab
    try:
        smoke._navigate_to_tab(page, tab, timeout)
    except Exception as exc:
        capture.guard()
        # A mounted empty panel was LOOKED AT; missing mount/browser is unobserved.
        mount = smoke._view_mount(page)
        if mount and mount.get("id") == tab:
            ok, detail = smoke.lib.render_check_ok(page.evaluate(smoke.PANEL_CHECKS[tab]))
            if not ok:
                raise Refusal(2, f"{tab}: inspected mounted panel was empty: {detail}") from exc
        raise Refusal(3, f"{tab}: never became ready: {exc}") from exc
    capture.guard()


def dwell(page, tab, seconds, smoke, capture, inspect_path=None):
    observations = [observe(page, tab, smoke, capture)]
    start = page.evaluate("Date.now()/1000")
    deadline = time.monotonic()+seconds
    began_dwell = time.monotonic()
    next_sample = time.monotonic()+1
    while time.monotonic() < deadline:
        began = time.monotonic()
        if inspect_path:
            x0, x1, y = inspect_path
            progress = (began-began_dwell)/seconds
            page.mouse.move(x0+(x1-x0)*progress, y)
        capture.pulse()
        # Capture above the required minimum to cover DOM inspection overhead;
        # the frame count and gap gates still use the requested fps unchanged.
        page.wait_for_timeout(max(1, min(deadline-time.monotonic(), 1/(capture.fps*2)-(time.monotonic()-began))*1000))
        capture.guard()
        if time.monotonic() >= next_sample:
            observations.append(observe(page, tab, smoke, capture))
            next_sample = time.monotonic()+1
    end = page.evaluate("Date.now()/1000")
    observations.append(observe(page, tab, smoke, capture))
    checked = coverage(capture.frames, start, end, capture.fps)
    return {"tab": tab, "dwell": {"start_s": start, "end_s": end, "requested_s": seconds},
            "inspection_path": inspect_path,
            "observations": observations, "console_errors": [], **checked}


def verify_compositor(page, session, timeout):
    """Chrome's outer window can clip CDP while innerHeight remains correct.

    Probe real CDP frames, then resize only the outer window by the measured
    difference. Browser viewport and DPR never change.
    """
    dimensions = []
    def probe(event):
        dimensions.append((event["metadata"]["deviceWidth"], event["metadata"]["deviceHeight"]))
        session.send("Page.screencastFrameAck", {"sessionId": event["sessionId"]})
    session.on("Page.screencastFrame", probe)
    try:
        for attempt in range(3):
            dimensions.clear()
            session.send("Page.startScreencast", {"format": "jpeg"})
            deadline = time.monotonic()+timeout
            while not dimensions and time.monotonic() < deadline:
                page.wait_for_timeout(50)
            session.send("Page.stopScreencast")
            if not dimensions:
                raise Refusal(5, "Chrome compositor probe received no screencast frames")
            w, h = dimensions[-1]
            if (w, h) == (WIDTH, HEIGHT):
                return
            window = session.send("Browser.getWindowForTarget")
            bounds = window["bounds"]
            session.send("Browser.setWindowBounds", {"windowId": window["windowId"],
                "bounds": {"width": bounds["width"]+WIDTH-w, "height": bounds["height"]+HEIGHT-h}})
        raise Refusal(5, f"Chrome compositor did not reach pinned geometry: {dimensions[-1]}")
    finally:
        session.remove_listener("Page.screencastFrame", probe)


def table_crop(page, keys, title_selector=None):
    """A horizontal band of visible table columns, with their original pixels."""
    return page.evaluate("""({keys,title}) => {
        const table = document.querySelector('#table-container table');
        const box = table.getBoundingClientRect();
        let top = title ? document.querySelector(title).getBoundingClientRect().top : box.top;
        top = Math.max(0, Math.floor(top));
        const bottom = Math.min(innerHeight, Math.ceil(box.bottom));
        return keys.map(key => {
            const th = table.querySelector('th[data-sort="'+key+'"]') ||
                (key === '_executions' ? [...table.querySelectorAll('th')].find(e=>e.innerText==='Waterfall') : null);
            if (!th) throw new Error('missing table column: '+key);
            const r = th.getBoundingClientRect();
            const widths = {name:340,text:450,ms:130,aas:90,
                p95_exec_ms:100,avg_exec_ms:100,_executions:120,
                pid:110,query_id:260,duration_ms:140};
            const width = Math.min(Math.ceil(r.width), widths[key] || Math.ceil(r.width));
            const left = th.classList.contains('num') ? Math.ceil(r.right)-width : Math.floor(r.left);
            return [Math.max(0,left),top,Math.min(innerWidth-Math.max(0,left),width),bottom-top];
        });
    }""", {"keys": keys, "title": title_selector})


def rect(page, selector):
    box = page.locator(selector).bounding_box()
    if not box:
        raise Refusal(3, f"could not inspect bounds of {selector}")
    x, y = max(0, math.floor(box["x"])), max(0, math.floor(box["y"]))
    return [x, y, min(WIDTH-x, math.ceil(box["width"])), min(HEIGHT-y, math.ceil(box["height"]))]


def duration_ms(text):
    match = re.fullmatch(r"\s*([\d.]+)\s*(ms|µs|us|s)\s*", text)
    return float(match[1])*{"ms":1,"µs":0.001,"us":0.001,"s":1000}[match[2]] if match else -1


def aas_inspection_path(page):
    box = page.locator('#aas-chart-container').bounding_box()
    return [box['x']+80, box['x']+box['width']*0.80, box['y']+box['height']*0.70]


def drill(page, smoke, capture, timeout):
    capture.current = "drill-setup"
    # Start the supplementary clip without the walk's session filter.
    if page.locator("#breadcrumb .crumb-clear").count():
        page.locator("#breadcrumb .crumb-clear").click()
        navigate(page,"overview",smoke,capture,timeout)
    navigate(page, "queries", smoke, capture, timeout)
    start = page.evaluate("Date.now()/1000")
    before = dwell(page, "queries", 2, smoke, capture, aas_inspection_path(page))
    # Rank actual DOM p95 execution duration, then average, never aggregate DB Time.
    candidates = page.evaluate("""() => {
        const table=document.querySelector('#table-container table');
        const heads=[...table.querySelectorAll('th')];
        const p95=heads.findIndex(e=>e.dataset.sort==='p95_exec_ms');
        const avg=heads.findIndex(e=>e.dataset.sort==='avg_exec_ms');
        return [...table.querySelectorAll('tbody tr.clickable')].map((r,i)=>({
            index:i,p95:r.cells[p95]?.innerText,avg:r.cells[avg]?.innerText,
            cells:[...r.cells].map(e=>e.innerText.trim())}));
    }""")
    if not candidates:
        raise Refusal(2, "drill: inspected Queries had no clickable rows")
    selected = max(candidates, key=lambda r: (duration_ms(r["p95"] or ""), duration_ms(r["avg"] or "")))
    if max(duration_ms(selected["p95"] or ""), duration_ms(selected["avg"] or "")) <= 0:
        raise Refusal(2, "drill: no visible query execution duration to select a slow query")
    query_crop = [[rect(page, "#aas-chart-container")],
                  table_crop(page, ["query_id", "text", "_executions", "p95_exec_ms", "avg_exec_ms"])]
    row = page.locator("#table-container tbody tr.clickable").nth(selected["index"])
    row.locator(".query-executions-action").hover()
    capture.pulse()
    at = page.evaluate("Date.now()*1.0")
    row.locator(".query-executions-action").click()
    capture.current = "drill-waterfall"
    try:
        page.wait_for_selector(smoke._READY_SELECTOR["waterfall"], timeout=timeout*1000)
        smoke._wait_for_mount_at_or_after(page, "waterfall", at, timeout)
    except Exception as exc:
        capture.guard()
        raise Refusal(3, f"drill: per-execution Waterfall never became ready: {exc}") from exc
    observe(page, "waterfall", smoke, capture)
    landed = page.evaluate("Date.now()/1000")
    selection = page.locator("#executions-table tr.selected-execution")
    if selection.count() != 1:
        raise Refusal(3, "drill: could not identify one selected execution")
    mount = smoke._view_mount(page)
    selected_at = page.evaluate("Date.now()*1.0")
    selection.click()  # Explicit selection is visible in the continuous recording.
    smoke._wait_for_mount_at_or_after(page,"waterfall",selected_at,timeout,min_seq=mount["seq"])
    execution_row = selection.locator("td").all_inner_texts()
    # Click an actual drawn non-CPU wait bar, rather than asserting from payload.
    point = page.evaluate("""() => {
        const el=document.querySelector('#waterfall-chart');
        const c=echarts.getInstanceByDom(el), opt=c.getOption();
        const data=opt.series.flatMap(s=>s.data||[]);
        const wait=data.find(d=>Array.isArray(d) && typeof d[3]==='string' && d[3].includes(':'));
        if(!wait) return null;
        const pixel = t => c.convertToPixel({gridIndex:0},[t,wait[2]]);
        const xy=pixel((Number(wait[0])+Number(wait[1]))/2);
        const b=el.getBoundingClientRect();return {x:b.left+xy[0],y:b.top+xy[1],wait:wait[3],
            x0:b.left+pixel(Number(wait[0]))[0],x1:b.left+pixel(Number(wait[1]))[0]};
    }""")
    if not point:
        raise Refusal(2, "drill: inspected execution has no specific drawable wait")
    wait_at = page.evaluate("Date.now()/1000")
    page.mouse.click(point["x"], point["y"])
    readout = page.locator("#waterfall-readout").inner_text().strip()
    if point["wait"] not in readout or "PID" not in readout or "Duration" not in readout:
        raise Refusal(3, f"drill: could not read the selected wait from DOM: {readout}")
    execution_crop = [table_crop(page, ["pid", "query_id", "duration_ms"]),
                      [rect(page, "#waterfall-chart")], [rect(page, "#waterfall-readout")]]
    execution_crop[-1][0][2] = min(1000, execution_crop[-1][0][2])
    margin = (point["x1"]-point["x0"])*0.10
    after = dwell(page, "waterfall", 5, smoke, capture,
                  [point["x0"]+margin,point["x1"]-margin,point["y"]])
    end = page.evaluate("Date.now()/1000")
    checked = coverage(capture.frames, start, end, capture.fps)
    return {"start_s":start,"end_s":end,"landed_s":landed,
            "query_row":selected["cells"], "execution_row":execution_row,
            "actions":[{"action":"query-executions click","timestamp_s":at/1000},
                       {"action":"execution selection","timestamp_s":selected_at/1000},
                       {"action":"wait bar click","timestamp_s":wait_at,"wait":point["wait"]}],
            "wait_readout":readout, "before":before,"after":after, **checked}, query_crop, execution_crop


def encode_video(context, root, frames):
    # A separate page encodes the saved frames; the demo page is never replaced.
    page = context.new_page()
    page.route("**/__demo_encoder__", lambda route: route.fulfill(status=200, content_type="text/html", body="<!doctype html>"))
    page.goto("http://127.0.0.1/__demo_encoder__")
    page.evaluate((ROOT / "scripts/demo-walk-video.js").read_text())
    page.evaluate("([w,h])=>demoVideoStart(w,h)", [WIDTH, HEIGHT])
    origin = frames[0]["timestamp_s"]
    timestamps = []
    for f in frames:
        at = round((f["timestamp_s"]-origin)*1_000_000)
        timestamps.append(at)
        data = base64.b64encode((root/f["path"]).read_bytes()).decode()
        page.evaluate("([b,t,k])=>demoVideoFrame(b,t,k)", [data, at, f["index"] % 20 == 0])
    duration_ms = (frames[-1]["timestamp_s"]-origin)*1000+250
    info = page.evaluate("([w,h,d])=>demoVideoFinish(w,h,d)", [WIDTH, HEIGHT, duration_ms])
    if info["frames"] != len(frames):
        raise Refusal(5, "WebCodecs dropped recorded frames")
    with (root / "walk.webm").open("wb") as output:
        for start in range(0, info["bytes"], 256_000):
            output.write(base64.b64decode(page.evaluate("([s,e])=>demoVideoBytes(s,e)", [start,start+256_000])))
    # Verify Chrome can actually load and decode the encoded file.
    def serve_video(route):
        path = root/"walk.webm"
        total = path.stat().st_size
        match = re.fullmatch(r"bytes=(\d+)-(\d*)",route.request.headers.get("range",""))
        if match:
            start,end = int(match[1]),min(total-1,int(match[2]) if match[2] else total-1)
            with path.open("rb") as source:
                source.seek(start); data=source.read(end-start+1)
            route.fulfill(status=206,content_type="video/webm",body=data,
                          headers={"Accept-Ranges":"bytes","Content-Range":f"bytes {start}-{end}/{total}"})
        else:
            route.fulfill(path=str(path),content_type="video/webm",headers={"Accept-Ranges":"bytes"})
    page.route("**/__demo_video__",serve_video)
    actual = page.evaluate("""async () => {
        const v=document.createElement('video');v.src='/__demo_video__';
        document.body.append(v);
        await new Promise((resolve,reject)=>{
            const timer=setTimeout(()=>reject(new Error('WebM load timed out')),10000);
            v.onloadeddata=()=>{clearTimeout(timer);resolve();};
            v.onerror=()=>{clearTimeout(timer);reject(new Error('WebM decode failed'));};
        });
        const decoded=[];
        for(const at of [v.duration*0.5,Math.max(0,v.duration-0.5)]) {
            await new Promise((resolve,reject)=>{
                const timer=setTimeout(()=>reject(new Error('WebM seek timed out')),10000);
                v.onseeked=()=>{clearTimeout(timer);resolve();};v.currentTime=at;
            });
            decoded.push(v.currentTime);
        }
        return {width:v.videoWidth,height:v.videoHeight,duration:v.duration,decoded_seek_times_s:decoded};
    }""")
    desired=[duration_ms/2000,max(0,duration_ms/1000-0.5)]
    if ((actual["width"],actual["height"]) != (WIDTH,HEIGHT)
            or abs(actual["duration"]-duration_ms/1000)>0.1
            or any(abs(a-b)>0.1 for a,b in zip(actual["decoded_seek_times_s"],desired))):
        raise Refusal(6, f"video decode verification failed: {actual}")
    page.close()
    return {"path":"walk.webm","bytes":info["bytes"],"codec":"VP8", "frame_timestamps_us":timestamps,
            "origin_timestamp_s":origin,"verified":actual}


def record(args, staging):
    import ui_live_smoke as smoke
    from free_ports import find_free_base
    from playwright.sync_api import sync_playwright
    dwells, runbook = runbook_plan(smoke.lib.TABS, args.dwells)
    mock = None
    capture = None
    try:
        if args.mock:
            port = find_free_base(2)
            log = (staging/"mock.log").open("w")
            mock = subprocess.Popen([sys.executable, str(ROOT/"tests/demo_walk_mock.py"), "--port",str(port)], stdout=log,stderr=log)
            if not smoke._wait_ports(mock,(port,port+1)):
                raise Refusal(3, f"mock server never became ready on {port}/{port+1}")
            url = smoke.mock_app_url(port)
        else:
            url = args.url
        with sync_playwright() as p, ExitStack() as cleanup:
            browser = p.chromium.launch(channel="chrome", headless=not args.headed)
            context = browser.new_context(viewport={"width":WIDTH,"height":HEIGHT}, device_scale_factor=DPR)
            context.add_init_script("window.addEventListener('unhandledrejection', e=>console.error('Unhandled rejection:',String(e.reason)))")
            page = context.new_page()
            page.set_default_timeout(args.timeout*1000)
            session = context.new_cdp_session(page)
            session.send("Page.enable")
            verify_compositor(page, session, args.timeout)
            (staging/"frames").mkdir()
            capture = Capture(page, session, staging, args.fps)
            cleanup.callback(capture.stop)
            page.goto(url)
            page.wait_for_selector("#status.connected",timeout=args.timeout*1000)  # Same connected gate as run_tab.
            geometry = []
            for _ in range(2):
                actual = page.evaluate("({w:innerWidth,h:innerHeight,dpr:devicePixelRatio})")
                reason = smoke.lib.viewport_mismatch_reason(WIDTH,HEIGHT,DPR,actual["w"],actual["h"],actual["dpr"])
                if reason:
                    raise Refusal(3, f"pinned viewport not achieved: {reason}")
                geometry.append(actual); page.wait_for_timeout(200)
            # Keep the runbook's 15-minute range, with compare off.
            page.locator("#time-range").click()
            page.locator('#time-picker button[data-range="900"]').click()
            if page.evaluate("Boolean(window.__pgwt?.compareSnapshot()?.enabled)"):
                raise Refusal(3,"compare mode must be off")
            if page.evaluate("window.__pgwt.timeRange.to-window.__pgwt.timeRange.from") != 900_000_000_000:
                raise Refusal(3,"15-minute range was not achieved")
            capture.pulse()
            walk = {"schema_version":1,"complete":False,"run_id":args.run_id,
                "source":"mock_server.py with changing Overview fixture" if args.mock else "real bridge",
                "url":url,"sha":subprocess.check_output(["git","rev-parse","HEAD"],cwd=ROOT,text=True).strip(),
                "browser_version":browser.version,"geometry_readings":geometry,
                "capture":{"method":"CDP Page.startScreencast","clock":"UTC epoch seconds; CDP metadata.timestamp and DOM Date.now()/1000",
                           "fps_min":args.fps,"still_frame_policy":"restart screencast on cadence; retain actual timestamps; never pad missing frames",
                           "resolution":[WIDTH,HEIGHT],"device_scale_factor":DPR},
                "runbook":runbook,"tabs":[],"console_errors":capture.errors,"frames":capture.frames}
            headline_crop = None
            for tab in smoke.lib.TABS:
                print(f"Recording {tab}: {dwells[tab]:g}s",flush=True)
                navigate(page,tab,smoke,capture,args.timeout)
                if tab == "overview":
                    headline_crop = [[rect(page,"#aas-chart-container")],
                                     table_crop(page,["name","ms","pct","aas"])]
                result = dwell(page,tab,dwells[tab],smoke,capture,
                               aas_inspection_path(page) if tab == 'overview' else None)
                walk["tabs"].append(result)
            print("Recording continuous Queries → execution → wait interaction",flush=True)
            interaction, query_crop, execution_crop = drill(page,smoke,capture,args.timeout)
            walk["drill"] = interaction
            capture.stop(); capture.guard()
            overview = walk["tabs"][0]
            def mix(observation):
                return [(r[0],r[2]) for r in observation["table_rows"] if len(r)>2
                        and r[0] in ("CPU (running)","CPU (waiting for a core)","CPU*","IO","Lock","LWLock")]
            if not any(mix(o) != mix(overview["observations"][0]) for o in overview["observations"][1:]):
                raise Refusal(2,"Overview: inspected wait mix did not change during the recorded stretch")
            overview["chart_change"] = chart_change(staging,capture.frames,overview["frame_indices"],headline_crop[0][0])
            # Five seconds of native crosshair inspection, including the live tick.
            headline_frames = frame_indices(capture.frames,overview["dwell"]["start_s"]+0.3,
                                            overview["dwell"]["start_s"]+5.3)
            first,last=headline_frames[0],headline_frames[-1]
            landed_indices=frame_indices(capture.frames,interaction["landed_s"],interaction["end_s"])
            walk["gifs"]=[
                {"name":"headline","path":"headline.gif","start_frame":first,"end_frame":last,
                 "fps":10,"width":1000,"crop":headline_crop},
                {"name":"drill-down","path":"drill-down.gif","start_frame":interaction["frame_indices"][0],
                 "end_frame":interaction["frame_indices"][-1],"fps":10,"width":1000,"crop":query_crop,
                 "crop_keyframes":[{"start_frame":landed_indices[0],"crop":execution_crop}]},
            ]
            print(f"Encoding {len(capture.frames)} timestamped frames in Chrome",flush=True)
            try:
                walk["video"]=encode_video(context,staging,capture.frames)
            except Refusal:
                raise
            except Exception as exc:
                raise Refusal(6,f"Chrome video encoding/decoding failed: {exc}") from exc
            for definition in walk["gifs"]:
                write_gif(staging,capture.frames,definition)
            capture.guard()
            walk["complete"]=True
            save_walk(staging,walk); write_analysis(staging,walk)
            context.close(); browser.close()
            return walk
    except Refusal as exc:
        exc.errors = capture.errors if capture else []
        raise
    except Exception as exc:
        if capture and capture.errors:
            failure = Refusal(4,f"{capture.current}: {capture.errors[-1]['message']}")
        elif isinstance(exc,(OSError,ValueError,KeyError,TypeError)):
            failure = Refusal(6,f"artifact/configuration failure: {exc}")
        else:
            failure = Refusal(3,f"could not inspect/record: {type(exc).__name__}: {exc}")
        failure.errors = capture.errors if capture else []
        raise failure from exc
    finally:
        if mock:
            smoke.stop_mock_server(mock)
            log.close()


def recut(path):
    root = path.resolve().parent
    walk=json.loads(path.read_text())
    if not walk.get("complete") or len(walk.get("gifs",[])) != 2:
        raise Refusal(6,"re-cut needs a complete walk with exactly two GIF definitions")
    with tempfile.TemporaryDirectory(prefix=".recut-",dir=root) as temp:
        staging=Path(temp)
        (staging/"frames").symlink_to(root/"frames",target_is_directory=True)
        for definition in walk["gifs"]:
            write_gif(staging,walk["frames"],definition)
        for definition in walk["gifs"]:
            os.replace(staging/definition["path"],root/definition["path"])
        save_walk(root,walk)
    return walk,root


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    mode=parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--mock",action="store_true")
    mode.add_argument("--url",help="real bridge UI URL, including optional ws query parameter")
    mode.add_argument("--recut",type=Path,help="edit walk.json GIF data, then re-cut without Chrome")
    parser.add_argument("--run-id",default=time.strftime("%Y%m%dT%H%M%SZ",time.gmtime())+f"-{os.getpid()}")
    parser.add_argument("--dwells",help="JSON object of all eleven per-tab durations, >=5 seconds each")
    parser.add_argument("--fps",type=int,default=12,help="minimum capture cadence (1–16), default 12; GIFs use 10")
    parser.add_argument("--timeout",type=float,default=60,help="readiness timeout in seconds")
    parser.add_argument("--headed",action="store_true",help="show the dedicated Chrome window")
    args=parser.parse_args()
    staging=None
    try:
        if args.recut:
            walk,output=recut(args.recut)
        else:
            if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*",args.run_id) or not (1<=args.fps<=16) or not (0<args.timeout<=300):
                raise Refusal(6,"invalid run-id, fps or timeout")
            RESULTS.mkdir(parents=True,exist_ok=True)
            output=RESULTS/args.run_id
            if output.exists():
                raise Refusal(6,f"refusing to overwrite existing run: {output}")
            staging=Path(tempfile.mkdtemp(prefix=f".{args.run_id}-",dir=RESULTS))
            walk=record(args,staging)
            os.rename(staging,output); staging=None
        print(f"Complete: {output}")
        for definition in walk["gifs"]:
            print(f"{definition['path']}: {definition['bytes']} bytes, frames {definition['start_frame']}–{definition['end_frame']}")
        return 0
    except Refusal as exc:
        print(f"REFUSED ({exc.code}): {exc}",file=sys.stderr)
        if staging:
            (RESULTS/f"{args.run_id}.refusal.json").write_text(json.dumps({"complete":False,"exit_code":exc.code,
                "reason":str(exc),"console_errors":getattr(exc,"errors",[])},indent=2)+"\n")
        return exc.code
    except (ValueError,OSError,KeyError,TypeError) as exc:
        print(f"REFUSED (6): artifact/configuration failure: {exc}",file=sys.stderr)
        return 6
    finally:
        if staging:
            shutil.rmtree(staging)


if __name__ == "__main__":
    sys.exit(main())

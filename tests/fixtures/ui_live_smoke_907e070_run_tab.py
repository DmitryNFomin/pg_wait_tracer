# Frozen executable run_tab from 907e070311753bd5aef6ec0c3589bf77c0c24d23.
# Generated with ast.unparse, omitting only comments and its docstring.
# Loaded into the driver namespace by test_ui_live_smoke_footage.py;
# capture helper source hashes are pinned separately. Never update this
# oracle to make a candidate pass: it represents the pre-footage path.
def run_tab(browser, tab_id, url, out_dir, ticks, first_data_timeout, blink_threshold, viewport=None, device_scale_factor=None, inject_blink_ms=None, inject_tick=None, inject_blink_frac=DEFAULT_INJECT_BLINK_FRAC, inject_frame_index=lib.INJECT_FRAME_INDEX, diag_data=False):
    req_width = viewport['width'] if viewport else 1280
    req_height = viewport['height'] if viewport else 900
    req_dsf = device_scale_factor if device_scale_factor is not None else 1
    tab_dir = os.path.join(out_dir, tab_id)
    os.makedirs(tab_dir, exist_ok=True)
    diag_dir = os.path.join(tab_dir, 'diag')
    if diag_data:
        os.makedirs(diag_dir, exist_ok=True)
    context = browser.new_context(viewport={'width': req_width, 'height': req_height}, device_scale_factor=req_dsf, record_video_dir=tab_dir, record_video_size={'width': req_width, 'height': req_height})
    context.add_init_script(UNHANDLED_REJECTION_HOOK)
    page = context.new_page()
    guard = ConsoleErrorGuard(page)
    console_errors = []
    pgwt_errors = []
    tick_paths = []
    video_path = None
    ttfp_ms = None
    try:
        page.goto(url)
        actual = page.evaluate('() => ({w: window.innerWidth, h: window.innerHeight, dpr: window.devicePixelRatio})')
        viewport_reason = lib.viewport_mismatch_reason(req_width, req_height, req_dsf, actual['w'], actual['h'], actual['dpr'])
        if viewport_reason is not None:
            raise SmokeFailure(f'viewport not achieved as requested: {viewport_reason}')
        try:
            page.wait_for_selector('#status.connected', timeout=first_data_timeout * 1000)
        except PWTimeoutError:
            raise SmokeFailure(f'WebSocket never reached #status.connected within {first_data_timeout}s')
        deadline = time.monotonic() + first_data_timeout
        while page.query_selector('#table-container table tbody tr') is None:
            if time.monotonic() > deadline:
                raise SmokeFailure(f'UI showed no data within {first_data_timeout}s')
            page.wait_for_timeout(200)
        page.evaluate(TICK_HOOK_JS)
        ttfp_ms = _navigate_to_tab(page, tab_id, first_data_timeout)
        leak_before, leak_before_settle_s = _settled_leak_probe(page)
        tick_baseline = page.evaluate('window.__uiLiveTicks.length')
        legend_ticks = []
        blink_ratios = []
        blink_pair_offsets_ms = []
        blink_sweep_ticks = []
        blink_not_measured = []
        self_test_result = None
        last_consumed_mount_seq = None
        pre_mount_diagnostics = []
        diag_ticks = []
        render_ok, render_detail = (False, 'never checked')
        for i in range(1, ticks + 1):
            if not _wait_for_tick(page, tick_baseline + i, TICK_TIMEOUT_S):
                raise SmokeFailure(f'tick {i}/{ticks} (hook index {tick_baseline + i - 1}, baseline {tick_baseline}) did not arrive within {TICK_TIMEOUT_S}s')
            tick_ts_ms = page.evaluate('idx => window.__uiLiveTicks[idx]', lib.tick_hook_index(tick_baseline, i))
            if tick_ts_ms is None:
                raise SmokeFailure(f'tick {i}/{ticks}: __uiLiveTicks[{lib.tick_hook_index(tick_baseline, i)}] is absent after _wait_for_tick reported {tick_baseline + i} ticks -- the tick hook was replaced or reset mid-walk')
            pre_mount_frame = None
            pre_mount_seq_after = None
            if tab_id == 'timeline':
                pre_mount_frame, _pre_mount_dims = _safe_panel_screenshot(page, tab_id)
                m_after = _view_mount(page)
                pre_mount_seq_after = m_after['seq'] if m_after is not None else None
            page.wait_for_timeout(100)
            ok, reason = lib.blind_window_ok(_atomic_panel_snapshot(page, tab_id))
            if not ok:
                raise SmokeFailure(f'tick {i}: {reason} ~100ms after the tick (CONTINUITY teardown-to-blank)')
            render_ok, render_detail = _poll_render_check(page, tab_id)
            if not render_ok:
                raise SmokeFailure(f'tick {i}: {render_detail}')
            mount = _wait_for_mount_at_or_after(page, tab_id, tick_ts_ms, TICK_TIMEOUT_S, min_seq=last_consumed_mount_seq)
            mount_at_ms = mount['at']
            last_consumed_mount_seq = mount['seq']
            achieved_a_ms = mount_at_ms - tick_ts_ms
            blink_pair_offsets_ms.append(achieved_a_ms)
            seq_before_sweep = mount['seq']
            sweep_offsets = list(lib.SWEEP_OFFSETS_MS)
            sweep_clip, sweep_dims = _panel_clip_and_dims(page, tab_id)
            injector = None
            if inject_blink_ms is not None and inject_tick is not None and (i == inject_tick) and (sweep_clip is not None):
                injector = _BlinkInjector(page, tab_id, sweep_clip, inject_blink_frac, inject_blink_ms, inject_frame_index)
            sweep_raw_frames = []
            sweep_achieved_ms = []
            sweep_capture_ms = []
            sweep_frame_windows_ms = []
            diag_frames = []
            for frame_index, offset in enumerate(sweep_offsets):
                if injector is not None:
                    injector.before_frame(frame_index)
                probe_before = _panel_data_probe(page, tab_id) if diag_data else None
                frame, achieved, capture_ms, end_clock_ms = _capture_at_offset(page, tab_id, mount_at_ms, offset, sweep_clip, read_end_clock=injector is not None or diag_data)
                probe_after = _panel_data_probe(page, tab_id) if diag_data else None
                if injector is not None:
                    injector.after_frame(frame_index)
                sweep_raw_frames.append(frame)
                sweep_achieved_ms.append(achieved)
                sweep_capture_ms.append(capture_ms)
                window = None if frame is None or end_clock_ms is None else (mount_at_ms + achieved, end_clock_ms)
                sweep_frame_windows_ms.append(window)
                if diag_data:
                    frame_png = None
                    if frame is not None:
                        frame_png = os.path.join(diag_dir, f'tick-{i}-frame-{frame_index}.png')
                        with open(frame_png, 'wb') as fh:
                            fh.write(frame)
                    diag_frames.append({'frame_index': frame_index, 'target_offset_ms': offset, 'achieved_ms': achieved, 'capture_ms': capture_ms, 'window_ms': window, 'png': frame_png, 'probe_before': probe_before, 'probe_after': probe_after})
            after_sweep = _view_mount(page)
            seq_after_sweep = after_sweep['seq'] if after_sweep is not None else None
            sweep_arrays = [lib.png_bytes_to_array(f) if f is not None else None for f in sweep_raw_frames]
            if tab_id == 'timeline':
                pre_mount_arr = lib.png_bytes_to_array(pre_mount_frame) if pre_mount_frame is not None else None
                sweep_first_arr = sweep_arrays[0] if sweep_arrays else None
                sig = lib.pre_mount_diagnostic_verdict(pre_mount_arr, sweep_first_arr, pre_mount_seq_after, mount['seq'])
                pre_mount_diagnostics.append({'tick': i, **sig})
            ratio, blink_note = lib.blink_sweep_gate_verdict(sweep_arrays, seq_before_sweep, seq_after_sweep)
            if blink_note:
                print(f'  note [{tab_id}] tick {i}: {blink_note}')
            if ratio is None:
                blink_not_measured.append({'tick': i, 'reason': blink_note})
            else:
                blink_ratios.append(ratio)
            if inject_blink_ms is not None and inject_tick is not None and (i == inject_tick):
                stimulus = injector.stimulus if injector is not None else None
                stimulus_ok, stimulus_reason = lib.inject_stimulus_verdict(stimulus, sweep_frame_windows_ms, inject_frame_index)
                pair_ratios = [r for r, _n in lib.sweep_consecutive_diff_ratios(sweep_arrays)]
                caught, boundary, boundary_ratios = lib.inject_detection_verdict(ratio, pair_ratios, inject_frame_index, blink_threshold)
                self_test_result = {'tick': i, 'ratio': ratio, 'measured': ratio is not None, 'note': blink_note, 'inject_blink_ms': inject_blink_ms, 'inject_frame_index': inject_frame_index, 'stimulus': stimulus, 'stimulus_ok': stimulus_ok, 'stimulus_reason': stimulus_reason, 'frame_windows_ms': sweep_frame_windows_ms, 'pair_ratios': pair_ratios, 'boundary_pair_indices': boundary, 'boundary_pair_ratios': boundary_ratios, 'caught': caught}
            if diag_data:
                pair_ratios_diag = [r for r, _n in lib.sweep_consecutive_diff_ratios(sweep_arrays)]
                diag_rec = {'tick': i, 'tab': tab_id, 'mount_seq': mount['seq'], 'mount_at_ms': mount_at_ms, 'tick_ts_ms': tick_ts_ms, 'seq_before_sweep': seq_before_sweep, 'seq_after_sweep': seq_after_sweep, 'sweep_ratio': ratio, 'sweep_note': blink_note, 'frames': diag_frames, 'pairs': lib.sweep_data_pair_reports(diag_frames, pair_ratios_diag, threshold=blink_threshold)}
                diag_ticks.append({k: v for k, v in diag_rec.items() if k != 'frames'})
                with open(os.path.join(diag_dir, f'tick-{i}.json'), 'w') as fh:
                    json.dump(diag_rec, fh, indent=1, sort_keys=True)
                    fh.write('\n')
                for pr in diag_rec['pairs']:
                    print(f"  diag [{tab_id}] tick {i} pair {pr['pair_index']} ({pr['frame_a_achieved_ms']:.0f}->{pr['frame_b_achieved_ms']:.0f}ms): ratio={pr['image_ratio']} verdict={pr['verdict']} data={pr['data'].get('reason') or ''}cells_differing={pr['data'].get('cells_differing')} sum {pr['data'].get('sum_a')}->{pr['data'].get('sum_b')}")
            blink_sweep_ticks.append(lib.build_sweep_tick_record(sweep_achieved_ms, sweep_arrays, target_offsets_ms=sweep_offsets, capture_ms=sweep_capture_ms, mount_seq=mount['seq'], panel_dims=sweep_dims))
            if sweep_raw_frames and sweep_raw_frames[0] is not None:
                tick_path = os.path.join(tab_dir, f'tick-{i}.png')
                with open(tick_path, 'wb') as f:
                    f.write(sweep_raw_frames[0])
                tick_paths.append(tick_path)
            legend = page.evaluate(LEGEND_COLORS_JS)
            if legend:
                legend_ticks.append(legend)
            for e in guard.drain():
                if _is_pgwt_error(e):
                    print(f'  note [{tab_id}] tick {i}: {e}')
                    pgwt_errors.append(e)
                else:
                    console_errors.append(e)
        leak_after, leak_after_settle_s = _settled_leak_probe(page)
        color_violations = lib.color_stability_violations(legend_ticks) if legend_ticks else []
        worst_blink = max(blink_ratios) if blink_ratios else None
        result = lib.build_tab_result(tab_id, render_ok, render_detail, len(tick_paths), console_errors, worst_blink, color_violations, leak_before, leak_after, artifacts={}, blink_threshold=blink_threshold, pgwt_console_errors=pgwt_errors, leak_before_settle_s=leak_before_settle_s, leak_after_settle_s=leak_after_settle_s, blink_pair_offsets_ms=blink_pair_offsets_ms, blink_sweep_ticks=blink_sweep_ticks, blink_not_measured=blink_not_measured, pre_mount_diagnostics=pre_mount_diagnostics, ttfp_ms=ttfp_ms)
        if diag_data:
            result['diag_data'] = diag_ticks
        if self_test_result is not None:
            result['self_test'] = self_test_result
    except SmokeFailure as e:
        print(f'  FAIL [{tab_id}]: {e}', file=sys.stderr)
        result = lib.build_failed_tab_result(tab_id, str(e), ticks_observed=len(tick_paths), pgwt_console_errors=pgwt_errors, ttfp_ms=ttfp_ms)
    except Exception:
        traceback.print_exc()
        result = lib.build_failed_tab_result(tab_id, 'unhandled exception (see stderr traceback)', ticks_observed=len(tick_paths), pgwt_console_errors=pgwt_errors, ttfp_ms=ttfp_ms)
    finally:
        video = page.video
        context.close()
        if video is not None:
            try:
                src = video.path()
                dst = os.path.join(tab_dir, f'{tab_id}.webm')
                os.replace(src, dst)
                video_path = dst
            except Exception as e:
                print(f'  note: could not finalize video for {tab_id}: {e}', file=sys.stderr)
    result['artifacts'] = {'frames': tick_paths, 'video': video_path}
    return result

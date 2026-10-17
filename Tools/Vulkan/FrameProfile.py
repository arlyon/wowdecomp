"""Summarise opt-in Vulkan frame CSVs without capturing game/account state."""
import argparse
import csv
import json
import math
import statistics
from pathlib import Path

TIMINGS = ('interval_ms', 'record_ms', 'fence_wait_ms', 'acquire_ms', 'gpu_ms')
COUNTERS = ('draws', 'triangles', 'upload_bytes', 'textures', 'pipelines')
OPTIONAL_COUNTERS = ('resident_upload_bytes', 'resident_bytes', 'retired_buffer_bytes',
                     'geometry_lock_scans', 'geometry_lock_visits', 'geometry_dirty_marks',
                     'stream_locks', 'image_allocations', 'resident_staging_allocations',
                     'resident_device_allocations', 'resident_staging_reuses',
                     'staging_arena_allocations', 'staging_arena_bytes',
                     'texture_cache_hits', 'texture_cache_lookups',
                     'image_cache_reuses', 'resource_cache_bytes', 'resource_cache_evictions',
                     'resident_device_reuses')


def percentile(values, fraction):
    values = sorted(values)
    position = (len(values) - 1) * fraction
    low = math.floor(position)
    high = math.ceil(position)
    return values[low] + (values[high] - values[low]) * (position - low)


def summary(values):
    if not values:
        return {'samples': 0}
    return dict(samples=len(values), minimum=min(values), mean=statistics.mean(values),
                median=statistics.median(values), p95=percentile(values, .95),
                p99=percentile(values, .99), maximum=max(values))


def analyse(rows, min_frame=121):
    samples = []
    previous = 0
    skipped_readbacks = 0
    skipped_warmup = 0
    for row in rows:
        frame = int(row['frame'])
        if frame <= previous:
            raise ValueError('Frame IDs must increase within one capture')
        previous = frame
        parsed = {'frame': frame, 'width': int(row['width']), 'height': int(row['height'])}
        if parsed['width'] <= 0 or parsed['height'] <= 0:
            raise ValueError('Invalid presentation dimensions')
        for key in TIMINGS + COUNTERS + OPTIONAL_COUNTERS:
            text = row.get(key, '') if key in OPTIONAL_COUNTERS else row[key]
            unavailable = (key == 'gpu_ms' or key in OPTIONAL_COUNTERS) and text == ''
            value = None if unavailable else float(text)
            if value is not None and (not math.isfinite(value) or value < 0):
                raise ValueError('Invalid profile value: ' + key)
            # The first frame has no preceding interval, not a zero-time frame.
            parsed[key] = None if key == 'interval_ms' and value == 0 else value
        if frame < min_frame:
            skipped_warmup += 1
        elif int(row['readbacks']):
            skipped_readbacks += 1
        else:
            samples.append(parsed)
    if not samples:
        raise ValueError('No steady-state frames; select a valid frame range')
    timings = {key: summary([r[key] for r in samples if r[key] is not None]) for key in TIMINGS}
    interval_mean = timings['interval_ms'].get('mean')
    return dict(frames=len(samples), first_frame=samples[0]['frame'], last_frame=samples[-1]['frame'],
                skipped_warmup=skipped_warmup, skipped_readbacks=skipped_readbacks,
                viewports=sorted({(r['width'], r['height']) for r in samples}),
                fps_from_mean_interval=1000 / interval_mean if interval_mean else None,
                timings_ms=timings,
                counters={key: summary([r[key] for r in samples if r[key] is not None])
                          for key in COUNTERS + OPTIONAL_COUNTERS})


def read_rows(source):
    lines = source.read().splitlines(keepends=True)
    incomplete = bool(lines and not lines[-1].endswith(('\n', '\r')))
    if incomplete:
        lines.pop()
    return list(csv.DictReader(lines)), int(incomplete)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('csv', type=Path)
    parser.add_argument('--min-frame', type=int, default=121, help='Discard startup frames before this ID')
    parser.add_argument('--max-frame', type=int, help='Last frame of the selected scene')
    args = parser.parse_args()
    with args.csv.open(newline='') as source:
        rows, incomplete = read_rows(source)
    if args.max_frame is not None:
        rows = [row for row in rows if int(row['frame']) <= args.max_frame]
    result = analyse(rows, args.min_frame)
    result['incomplete_final_rows_omitted'] = incomplete
    result['scope'] = 'Selected CSV frame range only; scene identity requires separate runtime evidence'
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()

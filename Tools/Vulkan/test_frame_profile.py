import io
import unittest
from FrameProfile import analyse, percentile, read_rows


def sample(frame, **changes):
    row = dict(frame=str(frame), width='1920', height='1080', readbacks='0',
               interval_ms='16', record_ms='3', fence_wait_ms='1', acquire_ms='2',
               gpu_ms='4', draws='100', triangles='1200', upload_bytes='4096',
               textures='20', pipelines='10')
    row.update(changes)
    return row


class FrameProfileTest(unittest.TestCase):
    def test_statistics_and_fps(self):
        result = analyse([sample(121), sample(122, interval_ms='24')])
        self.assertEqual(result['frames'], 2)
        self.assertEqual(result['fps_from_mean_interval'], 50)
        self.assertEqual(result['timings_ms']['interval_ms']['median'], 20)
        self.assertAlmostEqual(result['timings_ms']['interval_ms']['p95'], 23.6)

    def test_warmup_and_readback_are_explicitly_excluded(self):
        result = analyse([sample(1), sample(121, readbacks='1'), sample(122)])
        self.assertEqual(result['skipped_warmup'], 1)
        self.assertEqual(result['skipped_readbacks'], 1)
        self.assertEqual(result['first_frame'], 122)

    def test_unavailable_gpu_is_not_invented(self):
        result = analyse([sample(121, gpu_ms='')])
        self.assertEqual(result['timings_ms']['gpu_ms'], {'samples': 0})

    def test_first_interval_is_not_zero_duration(self):
        result = analyse([sample(1, interval_ms='0')], min_frame=1)
        self.assertIsNone(result['fps_from_mean_interval'])
        self.assertEqual(result['timings_ms']['interval_ms'], {'samples': 0})

    def test_resize_dimensions_are_retained(self):
        result = analyse([sample(121), sample(122, width='629', height='1236')])
        self.assertEqual(result['viewports'], [(629, 1236), (1920, 1080)])

    def test_bad_samples_are_rejected(self):
        for changes in [dict(gpu_ms='nan'), dict(record_ms='-1'), dict(width='0')]:
            with self.assertRaises(ValueError):
                analyse([sample(121, **changes)])
        with self.assertRaises(ValueError):
            analyse([sample(121), sample(121)])
        with self.assertRaises(ValueError):
            analyse([sample(1)])

    def test_live_capture_omits_incomplete_final_row(self):
        rows, omitted = read_rows(io.StringIO('frame,gpu_ms\n1,2.5\n2,3.'))
        self.assertEqual(rows, [dict(frame='1', gpu_ms='2.5')])
        self.assertEqual(omitted, 1)

    def test_residency_metrics_are_optional_not_invented(self):
        old = analyse([sample(121)])
        self.assertEqual(old['counters']['resident_upload_bytes'], {'samples': 0})
        new = analyse([sample(121, resident_upload_bytes='1048576', resident_bytes='4194304', retired_buffer_bytes='1048576'),
                       sample(122, resident_upload_bytes='0', resident_bytes='4194304', retired_buffer_bytes='0')])
        self.assertEqual(new['counters']['resident_upload_bytes']['maximum'], 1048576)
        self.assertEqual(new['counters']['resident_bytes']['median'], 4194304)
        self.assertEqual(new['counters']['retired_buffer_bytes']['minimum'], 0)
        with self.assertRaises(ValueError):
            analyse([sample(121, resident_bytes='-1')])

    def test_allocation_and_lock_counters_are_optional(self):
        fields = ('geometry_lock_scans', 'geometry_lock_visits', 'geometry_dirty_marks',
                  'stream_locks', 'image_allocations', 'resident_staging_allocations',
                  'resident_device_allocations', 'resident_staging_reuses',
                  'staging_arena_allocations', 'staging_arena_bytes')
        old = analyse([sample(121)])
        new = analyse([sample(121, **dict.fromkeys(fields, '3')),
                       sample(122, **dict.fromkeys(fields, '0'))])
        for field in fields:
            self.assertEqual(old['counters'][field], {'samples': 0})
            self.assertEqual(new['counters'][field]['median'], 1.5)
            for value in ('-1', 'nan', 'inf'):
                with self.assertRaises(ValueError):
                    analyse([sample(121, **{field: value})])

    def test_texture_cache_metrics_are_optional_and_validated(self):
        old = analyse([sample(121)])
        for key in ('texture_cache_hits', 'texture_cache_lookups'):
            self.assertEqual(old['counters'][key], {'samples': 0})
        result = analyse([sample(121, texture_cache_hits='500', texture_cache_lookups='3'),
                          sample(122, texture_cache_hits='700', texture_cache_lookups='0')])
        self.assertEqual(result['counters']['texture_cache_hits']['median'], 600)
        self.assertEqual(result['counters']['texture_cache_lookups']['maximum'], 3)
        for key in ('texture_cache_hits', 'texture_cache_lookups'):
            for value in ('-1', 'nan', 'inf'):
                with self.assertRaises(ValueError):
                    analyse([sample(121, **{key: value})])

    def test_resource_cache_metrics_are_optional_and_validated(self):
        fields = ('image_cache_reuses', 'resource_cache_bytes', 'resource_cache_evictions',
                  'resident_device_reuses')
        old = analyse([sample(121)])
        new = analyse([sample(121, **dict.fromkeys(fields, '4'))])
        for key in fields:
            self.assertEqual(old['counters'][key], {'samples': 0})
            self.assertEqual(new['counters'][key]['median'], 4)
            for value in ('-1', 'nan', 'inf'):
                with self.assertRaises(ValueError):
                    analyse([sample(121, **{key: value})])

    def test_percentile_single_sample(self):
        self.assertEqual(percentile([3], .99), 3)


if __name__ == '__main__':
    unittest.main()

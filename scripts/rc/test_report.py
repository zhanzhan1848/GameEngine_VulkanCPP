#!/usr/bin/env python3
"""Report integrity contracts; all input data is synthetic, no GPU required."""
import contextlib
import io
import json
import math
import pathlib
import struct
import tempfile
import unittest

import report


class ReportTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='rc-report-test-')
        self.directory = pathlib.Path(self.temporary.name)
        self.rows = []

    def tearDown(self):
        self.temporary.cleanup()

    def sample(self, frame=120, baseline=None, rc=None):
        baseline = baseline or [(1., .5, .25, 1.), (9., 8., 7., 0.)]
        rc = rc or [(1., .5, .25, 1.), (4., 3., 2., 0.)]
        row = dict(frame=frame, width=2, height=1, rc_gpu_ms=2.,
                   coverage=sum(pixel[3] for pixel in rc)/2,
                   fully_known_pixels=sum(pixel[3]>.99 for pixel in rc),
                   baseline_vma_allocated_bytes=100, with_rc_vma_allocated_bytes=300,
                   baseline_vma_reserved_bytes=1024, with_rc_vma_reserved_bytes=2048,
                   rc_requested_bytes=160, source_scope='synthetic fixture')
        self.rows.append(row)
        for suffix, fmt, pixels in [('-gi.rgba16f','<4e',baseline), ('-rc.rgba32f','<4f',rc)]:
            (self.directory/f'frame-{frame}{suffix}').write_bytes(b''.join(struct.pack(fmt,*p) for p in pixels))
        return row

    def render(self):
        (self.directory/'samples.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in self.rows))
        with contextlib.redirect_stdout(io.StringIO()):
            return report.render(self.directory)

    def test_common_exposure_is_pixelwise_and_not_image_normalized(self):
        self.assertEqual(report.display_rgb([(1.,.5,.25,1)]),
                         report.display_rgb([(1.,.5,.25,1),(100.,100.,100.,1)])[:3])
        self.sample()
        result=self.render()
        self.assertEqual(result['paired_metrics'][0]['jointly_valid_pixels'],1)
        self.assertEqual(result['paired_metrics'][0]['irradiance_difference_rmse_joint'],0)
        self.assertEqual(result['vma_allocated_delta_bytes'],200)
        self.assertEqual(result['vma_reserved_delta_bytes'],1024)
        self.assertIsNone(result['rc_gpu_ms_median_warm'])
        self.assertTrue((self.directory/'comparison.html').read_text().count('data:image/png;base64,')==3)

    def test_incomplete_capture_rejected(self):
        self.sample()
        path=self.directory/'frame-120-rc.rgba32f'
        path.write_bytes(path.read_bytes()[:-1])
        with self.assertRaisesRegex(ValueError,'Incomplete capture'):self.render()

    def test_nonfinite_capture_rejected(self):
        self.sample(rc=[(math.nan,0,0,1),(0,0,0,0)])
        with self.assertRaisesRegex(ValueError,'Nonfinite capture'):self.render()

    def test_reported_valid_count_cannot_override_gpu_mask(self):
        row=self.sample();row['fully_known_pixels']=2
        with self.assertRaisesRegex(ValueError,'known-pixel count'):self.render()

    def test_reported_coverage_cannot_override_gpu_mask(self):
        row=self.sample();row['coverage']=1
        with self.assertRaisesRegex(ValueError,'Reported coverage'):self.render()

    def test_eight_frames_and_low_coverage_stay_unverified(self):
        for frame in range(120,128):self.sample(frame,rc=[(1,.5,.25,.5),(0,0,0,0)])
        result=self.render()
        self.assertTrue(result['eight_consecutive_samples'])
        self.assertTrue(result['coverage_acceptance'].startswith('INSUFFICIENT'))
        self.assertTrue(result['quality_acceptance'].startswith('UNVERIFIED'))
        self.assertIsNone(result['paired_metrics'][0]['irradiance_difference_rmse_joint'])

    def test_single_frame_and_nonconsecutive_frames_are_incomplete(self):
        self.sample()
        self.assertFalse(self.render()['eight_consecutive_samples'])
        for frame in range(122,129):self.sample(frame)
        self.assertFalse(self.render()['eight_consecutive_samples'])

    def test_invalid_timing_and_duplicate_frame_rejected(self):
        row=self.sample();row['rc_gpu_ms']=-1
        with self.assertRaisesRegex(ValueError,'numeric field'):self.render()
        row['rc_gpu_ms']=1;self.rows.append(dict(row))
        with self.assertRaisesRegex(ValueError,'Duplicate frame'):self.render()


if __name__=='__main__':
    unittest.main()

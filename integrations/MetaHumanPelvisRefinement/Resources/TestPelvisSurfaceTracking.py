"""Synthetic image-motion checks for the optional OpenCV tracking helper."""
import tempfile
import unittest
from pathlib import Path
import cv2
import numpy as np
from TrackPelvisSurface import track


class SurfaceTrackingTest(unittest.TestCase):
    def run_sequence(self, empty=False, occluded=False, arm=False):
        with tempfile.TemporaryDirectory(prefix='pelvis-tracking-test-') as folder:
            width, height, count = 160, 160, 9
            rng = np.random.default_rng(13)
            texture = rng.integers(0, 255, (height, width), dtype=np.uint8)
            texture = cv2.GaussianBlur(texture, (3, 3), .6)
            points = [(x, y) for y in range(65, 106, 10) for x in range(60, 101, 10)]
            images, masks, frames = [], [], []
            for frame in range(count):
                dx, dy = frame * .6, frame * .25
                matrix = np.float32([[1, 0, dx], [0, 1, dy]])
                images.append(cv2.warpAffine(texture, matrix, (width, height)))
                mask = np.zeros((height, width), np.uint8)
                if not empty and not (occluded and frame >= 4):
                    mask[40:130, 40:130] = 255
                masks.append(mask)
                frames.append(dict(valid=True, hips=[[60, 80], [100, 80]], points=[dict(visible=True, projected=[x + dx, y + dy]) for x, y in points]))
            np.asarray(images).tofile(Path(folder) / 'gray.bin')
            np.asarray(masks).tofile(Path(folder) / 'mask.bin')
            document = dict(width=width, height=height, directory=folder, image_scale=[1, 1], frames=frames)
            if arm:
                landmarks = np.zeros((count, 25, 3), np.float32)
                # Two arms crossing the entire textured region from frame 4.
                for frame in range(4, count):
                    landmarks[frame, 2:5] = [[50, 65, 1], [110, 65, 1], [50, 105, 1]]
                    landmarks[frame, 5:8] = [[110, 105, 1], [50, 105, 1], [110, 65, 1]]
                document['keypoints'] = landmarks.reshape(-1).tolist()
            return track(document)

    def test_known_image_motion(self):
        result = self.run_sequence()
        self.assertGreater(result['frames_with_tracks'], 5)
        errors = [np.linalg.norm(np.asarray(o[3:5]) - np.asarray([.6, .25]) * (o[0] - o[1])) for o in result['observations']]
        self.assertLess(float(np.median(errors)), .4)

    def test_empty_foreground_falls_back(self):
        self.assertEqual(self.run_sequence(empty=True)['observations'], [])

    def test_occluded_region_stops_tracking(self):
        result = self.run_sequence(occluded=True)
        self.assertGreater(len(result['observations']), 0)
        self.assertTrue(all(o[0] < 4 for o in result['observations']))

    def test_crossing_arms_are_not_pelvis_features(self):
        result = self.run_sequence(arm=True)
        self.assertGreater(len(result['observations']), 0)
        self.assertTrue(all(o[0] < 4 for o in result['observations']))


if __name__ == '__main__':
    unittest.main()

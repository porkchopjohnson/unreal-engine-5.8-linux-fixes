"""Track visible pelvis-surface anchors with masked, bidirectional optical flow.

Child-process CLI: system python3 TrackPelvisSurface.py input.json output.json.
Requires numpy and OpenCV. No network, asset writes, or model downloads.
"""
import json
import sys
from pathlib import Path
import cv2
import numpy as np


def track(document):
    width, height = document['width'], document['height']
    count = len(document['frames'])
    assert 1 <= width <= 1024 and 1 <= height <= 1024 and count > 0
    folder = Path(document['directory'])
    gray = np.memmap(folder / 'gray.bin', dtype=np.uint8, mode='r', shape=(count, height, width))
    masks = np.memmap(folder / 'mask.bin', dtype=np.uint8, mode='r', shape=(count, height, width))
    scale = np.asarray(document['image_scale'], dtype=np.float32)
    assert np.isfinite(scale).all() and (scale > 0).all()
    keypoints = document.get('keypoints')
    keypoints = np.asarray(keypoints, dtype=np.float32).reshape(count, 25, 3) if keypoints is not None else None
    observations = []
    debug_tracks = []
    active = []
    previous_points = None
    reference = -1
    lk = dict(winSize=(19, 19), maxLevel=3,
              criteria=(cv2.TERM_CRITERIA_EPS | cv2.TERM_CRITERIA_COUNT, 25, .01),
              minEigThreshold=1e-4)

    for frame, row in enumerate(document['frames']):
        if not row['valid']:
            active, previous_points, reference = [], None, -1
            continue
        image = np.asarray(gray[frame])
        safe_mask = cv2.erode(np.asarray(masks[frame]), np.ones((5, 5), np.uint8))
        if keypoints is not None:
            # Foreground masks include arms in front of the hips. Optical flow
            # on a crossing arm must never be interpreted as pelvis motion.
            for a, b in [(2, 3), (3, 4), (5, 6), (6, 7)]:
                endpoints = keypoints[frame, [a, b]]
                if not np.isfinite(endpoints).all() or np.min(endpoints[:, 2]) < .3:
                    continue
                length = np.linalg.norm(endpoints[0, :2] - endpoints[1, :2])
                radius = float(np.clip(length * .18, 8, 40) * np.max(scale))
                start, end = [tuple(np.rint(p[:2] * scale).astype(int)) for p in endpoints]
                cv2.line(safe_mask, start, end, 0, max(1, int(np.ceil(radius * 2))))
                if b in (4, 7):
                    cv2.circle(safe_mask, end, max(1, int(np.ceil(radius * 1.5))), 0, -1)
        hips = np.asarray(row['hips'], np.float32)
        hip_center = hips.mean(axis=0)
        candidates = row['points']
        current = None
        if active and frame > 0:
            current, forward, error = cv2.calcOpticalFlowPyrLK(np.asarray(gray[frame-1]), image, previous_points, None, **lk)
            backward, reverse, _ = cv2.calcOpticalFlowPyrLK(image, np.asarray(gray[frame-1]), current, None, **lk)
            fb = np.linalg.norm(backward.reshape(-1, 2) - previous_points.reshape(-1, 2), axis=1)
            keep = []
            for i, item in enumerate(active):
                point = current[i, 0]
                if not forward[i, 0] or not reverse[i, 0] or not np.isfinite(point).all() or fb[i] > .8 or error[i, 0] > 30:
                    continue
                x, y = np.rint(point).astype(int)
                if not (0 <= x < width and 0 <= y < height and safe_mask[y, x]):
                    continue
                candidate = candidates[item['candidate']]
                if not candidate['visible']:
                    continue
                # Both endpoints must remain in the pelvis neighborhood, not
                # migrate along an arm or onto the background.
                spread = max(float(np.linalg.norm((hips[0] - hips[1]) * scale)), 12.)
                if np.linalg.norm((point / scale - hip_center) * scale) > spread * 1.2:
                    continue
                quality = float(max(0., 1. - fb[i] / .8) * max(0., 1. - error[i, 0] / 30.))
                if quality < .15:
                    continue
                item = dict(item, current=point.tolist(), quality=min(item['quality'], quality))
                keep.append(item)
            active = keep
            # A single moving hand/garment fold should not dominate. Reject
            # gross displacement outliers, while retaining rotation gradients.
            if len(active) >= 6:
                displacements = np.asarray([np.asarray(a['current']) - np.asarray(a['anchor']) for a in active])
                median = np.median(displacements, axis=0)
                distances = np.linalg.norm(displacements - median, axis=1)
                limit = max(4., float(np.median(distances)) * 3.)
                active = [a for a, distance in zip(active, distances) if distance <= limit]
            if len(active) >= 4:
                points = np.asarray([a['current'] for a in active])
                # Vertical spread is important for observing pitch, rather than
                # treating several points on the hip line as a dense surface.
                if np.ptp(points[:, 1]) >= 5 and np.ptp(points[:, 0]) >= 5:
                    reference_center = np.asarray(document['frames'][reference]['hips']).mean(axis=0)
                    for item in active:
                        displacement = (np.asarray(item['current']) / scale - hip_center) - (np.asarray(item['anchor']) / scale - reference_center)
                        observations.append([frame, reference, item['candidate'], float(displacement[0]), float(displacement[1]), item['quality']])
                        debug_tracks.append(dict(frame=frame, reference=reference, candidate=item['candidate'],
                                                 anchor=item['anchor'], current=item['current'], quality=item['quality']))
            previous_points = np.asarray([a['current'] for a in active], np.float32).reshape(-1, 1, 2) if active else None

        # Short anchors limit drift. The fitter links each anchor's own estimated
        # correction, so restarting a track does not reset the rotation to zero.
        if not active or len(active) < 6 or frame - reference >= 12:
            reference = frame
            active = []
            features = cv2.goodFeaturesToTrack(image, maxCorners=200, qualityLevel=.008, minDistance=3, mask=safe_mask, blockSize=5)
            if features is not None:
                features = features.reshape(-1, 2)
                used = set()
                for index, candidate in enumerate(candidates):
                    if not candidate['visible']:
                        continue
                    point = np.asarray(candidate['projected'], np.float32) * scale
                    distance = np.linalg.norm(features - point, axis=1)
                    nearest = int(np.argmin(distance))
                    if nearest in used or distance[nearest] > 6:
                        continue
                    selected = features[nearest]
                    used.add(nearest)
                    active.append(dict(candidate=index, anchor=selected.tolist(), current=selected.tolist(), quality=1.))
                    if len(active) == 16:
                        break
            previous_points = np.asarray([a['current'] for a in active], np.float32).reshape(-1, 1, 2) if active else None
    return {'observations': observations, 'frames_with_tracks': len({o[0] for o in observations}), 'debug_tracks': debug_tracks}


if __name__ == '__main__':
    assert len(sys.argv) == 3
    result = track(json.loads(Path(sys.argv[1]).read_text()))
    Path(sys.argv[2]).write_text(json.dumps(result, allow_nan=False))
    print(f'Pelvis surface tracking: {len(result["observations"])} observations across {result["frames_with_tracks"]} frames')

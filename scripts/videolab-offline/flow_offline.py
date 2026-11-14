#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
"""Offline NumPy model of the enhancement worker's flow interpolation backend.

It replays the worker's dumped NVOFA grids (--dump-flow) on frames decoded from the
same recording and scores every generated phase against the skipped source frame,
so an interpolation change can be compared before it is written as a shader, and
the GPU result can be checked per frame against this model afterwards.

    python flow_offline.py --clip C02=E:/clips/C02.mkv --flow C02=E:/dump/flow/C02 \
        --frames C02=185,193,201 --out offline.csv

Frame numbering follows the worker with --decimate 4: for worker frame N the current
real frame is source frame 4*(N-1), the previous one 4*(N-2) and phase p (1..3) is
scored against source frame 4*(N-2)+p. Source frames are counted from 0.

Variants:
    projected  matchMain, splatMain, resolveMain, fillMain, medianFieldMain, heldMain,
               weightMain and interpolateProjectedMain of worker_shaders.h.in
    direct     interpolateDirectMain
    blend      cross-fade of the two real frames

Requires NumPy, SciPy and an ffmpeg executable. PSNR is over RGB, 100 dB when
identical, as in the video lab. The model mirrors the shaders' arithmetic but is not
bit exact: SciPy's cubic spline stands in for the shader's Catmull-Rom filter and
rounding differs in ties.
"""
import argparse
import csv
import os
import subprocess

import numpy as np
from scipy.ndimage import map_coordinates, median_filter, uniform_filter

LUMA = np.array([0.299, 0.587, 0.114], np.float32)
QUAD = [(-0.25, -0.25), (0.25, -0.25), (-0.25, 0.25), (0.25, 0.25)]
FILL_STRIDES = (32, 16, 8, 4, 2, 1, 1)
MATCH_THRESHOLD = 0.10      # splatMain: vectors matching worse than this are dropped
AGREE = (0.06, 0.20)        # blend the two samples below, use the nearer one above
HELD_MARGIN = 0.01          # weightMain / interpolateDirectMain
DEAD_ZONE = 0.2             # interpolateDirectMain, pixels


def decode(path, width, height, wanted, ffmpeg):
    """Return {source frame: uint8 RGB} for the requested 0-based source frames."""
    lo, hi = min(wanted), max(wanted)
    command = [ffmpeg, '-v', 'error', '-i', path, '-an', '-sn', '-vf',
               f"fps=120,scale={width}:{height}:flags=bicubic,select='between(n\\,{lo}\\,{hi})'",
               '-fps_mode', 'passthrough', '-pix_fmt', 'rgb24', '-f', 'rawvideo', '-']
    process = subprocess.Popen(command, stdout=subprocess.PIPE)
    size, frames, index = width * height * 3, {}, lo
    while True:
        data = process.stdout.read(size)
        if len(data) < size:
            break
        if index in wanted:
            frames[index] = np.frombuffer(data, np.uint8).reshape(height, width, 3).copy()
        index += 1
    process.wait()
    missing = sorted(set(wanted) - set(frames))
    if missing:
        raise SystemExit(f'{path}: source frames {missing[:5]} could not be decoded')
    return frames


def load_flow(directory, frame):
    """Return (vectors in flow pixels, invalid mask) of flow_<frame>_<w>x<h>.s16."""
    names = [n for n in os.listdir(directory) if n.startswith(f'flow_{frame}_') and n.endswith('.s16')]
    if len(names) != 1:
        raise SystemExit(f'{directory}: expected one flow file for worker frame {frame}, found {len(names)}')
    grid_w, grid_h = (int(v) for v in names[0][:-4].split('_')[2].split('x'))
    raw = np.fromfile(os.path.join(directory, names[0]), dtype='<i2').reshape(grid_h, grid_w, 2)
    invalid = (raw[..., 0] == -32768) | (raw[..., 1] == -32768)
    return np.where(invalid[..., None], 0, raw).astype(np.float32) / 32.0, invalid


def sample(image, x, y, order=1):
    """Sample at pixel-centre coordinates (0.5 is the first texel centre), clamped."""
    cx = np.clip(x - 0.5, 0, image.shape[1] - 1)
    cy = np.clip(y - 0.5, 0, image.shape[0] - 1)
    if image.ndim == 2:
        return map_coordinates(image, [cy, cx], order=order, mode='nearest')
    return np.stack([map_coordinates(image[..., c], [cy, cx], order=order, mode='nearest')
                     for c in range(image.shape[2])], axis=-1)


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


def psnr(a, b):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return 100.0 if mse < 1e-9 else min(100.0, 10 * np.log10(255.0 ** 2 / mse))


def to8(image):
    return np.clip(np.rint(image * 255.0), 0, 255).astype(np.uint8)


class Pair:
    """Two real frames, their flow grid and the output pixel grid."""

    def __init__(self, prev, cur, grid, invalid, order):
        self.prev, self.cur, self.grid, self.invalid, self.order = prev, cur, grid, invalid, order
        self.h, self.w = cur.shape[:2]
        self.gh, self.gw = grid.shape[:2]
        ys, xs = np.mgrid[0:self.h, 0:self.w].astype(np.float32)
        self.px, self.py = xs + 0.5, ys + 0.5
        gy, gx = np.mgrid[0:self.gh, 0:self.gw].astype(np.float32)
        self.cell_w, self.cell_h = self.w / self.gw, self.h / self.gh
        self.cx, self.cy = (gx + 0.5) * self.cell_w, (gy + 0.5) * self.cell_h

    def upsample(self, field):
        """Bilinear, clamped: a grid texture sampled at normalised output coordinates."""
        return sample(field, self.px * self.gw / self.w, self.py * self.gh / self.h)

    def block_error(self, vx, vy, t_cur, t_prev):
        """Four bilinear taps per cell: current at X - t_cur*v against previous at X + t_prev*v."""
        e = np.zeros((self.gh, self.gw), np.float32)
        for ox, oy in QUAD:
            qx, qy = self.cx + ox * self.cell_w, self.cy + oy * self.cell_h
            e += np.abs(sample(self.cur, qx - t_cur * vx, qy - t_cur * vy) -
                        sample(self.prev, qx + t_prev * vx, qy + t_prev * vy)) @ LUMA
        return e * 0.25

    def finish(self, g, t, use_held):
        """interpolateProjectedMain: g and use_held are per output pixel."""
        cx, cy = self.px - (1 - t) * g[..., 0], self.py - (1 - t) * g[..., 1]
        qx, qy = self.px + t * g[..., 0], self.py + t * g[..., 1]
        current = np.clip(sample(self.cur, cx, cy, self.order), 0, 1)
        previous = np.clip(sample(self.prev, qx, qy, self.order), 0, 1)
        cin = (cx >= 0) & (cx <= self.w) & (cy >= 0) & (cy <= self.h)
        pin = (qx >= 0) & (qx <= self.w) & (qy >= 0) & (qy <= self.h)
        mismatch = np.abs(current - previous) @ LUMA
        nearer = 1.0 if t >= 0.5 else 0.0
        w = t + (nearer - t) * smoothstep(AGREE[0], AGREE[1], mismatch)
        only_c, only_p = cin & ~pin, pin & ~cin
        w = np.where(only_c, 1.0, np.where(only_p, 0.0, w))
        moved = previous * (1 - w[..., None]) + current * w[..., None]
        if use_held is None:
            still = np.abs(self.cur - self.prev) @ LUMA
            use_held = smoothstep(-HELD_MARGIN, 2 * HELD_MARGIN, mismatch - still)
        use_held = np.where(only_c | only_p, 0.0, use_held)
        held = self.prev * (1 - t) + self.cur * t
        return moved * (1 - use_held[..., None]) + held * use_held[..., None]


def blend(pair, t):
    return pair.prev * (1 - t) + pair.cur * t


def direct(pair, t):
    """interpolateDirectMain: the field vector followed to this pixel, per-pixel hypotheses."""
    motion = pair.upsample(pair.grid)
    g = motion
    for _ in range(2):
        g = sample(motion, pair.px - (1 - t) * g[..., 0], pair.py - (1 - t) * g[..., 1])
    out = pair.finish(g, t, None)
    dead = (g ** 2).sum(-1) < DEAD_ZONE ** 2
    return np.where(dead[..., None], blend(pair, t), out)


def projected(pair, t):
    gh, gw = pair.gh, pair.gw
    # matchMain, quantised like the 12-bit error packed by splatMain.
    error = pair.block_error(pair.grid[..., 0], pair.grid[..., 1], 0.0, 1.0)
    error = np.minimum(np.rint(np.clip(error, 0, 1) * 4095), 4095) / 4095
    keep = ~pair.invalid & (error < MATCH_THRESHOLD)
    # splatMain / resolveMain: the best matching vector wins a cell.
    ys, xs = np.mgrid[0:gh, 0:gw]
    tx = np.floor(xs + (1 - t) * pair.grid[..., 0] / pair.cell_w + 0.5).astype(np.int64)
    ty = np.floor(ys + (1 - t) * pair.grid[..., 1] / pair.cell_h + 0.5).astype(np.int64)
    inside = keep & (tx >= 0) & (tx < gw) & (ty >= 0) & (ty < gh)
    target, source, cost = (ty * gw + tx)[inside], (ys * gw + xs)[inside], error[inside]
    order = np.lexsort((-source, -cost))            # worst first; the best is written last
    field = np.zeros((gh * gw, 2), np.float32)
    valid = np.zeros(gh * gw, bool)
    field[target[order]] = pair.grid.reshape(-1, 2)[source[order]]
    valid[target[order]] = True
    field, valid = field.reshape(gh, gw, 2), valid.reshape(gh, gw)
    # fillMain: holes take the mean of valid neighbours at each stride.
    for stride in FILL_STRIDES:
        total, count = np.zeros_like(field), np.zeros((gh, gw), np.float32)
        for dy in (-stride, 0, stride):
            for dx in (-stride, 0, stride):
                if dx == 0 and dy == 0:
                    continue
                rows = np.clip(np.arange(gh) + dy, 0, gh - 1)
                columns = np.clip(np.arange(gw) + dx, 0, gw - 1)
                neighbour_valid = valid[rows][:, columns]
                total += np.where(neighbour_valid[..., None], field[rows][:, columns], 0)
                count += neighbour_valid
        grow = ~valid & (count > 0)
        field[grow] = total[grow] / count[grow][:, None]
        valid = valid | grow
    field[~valid] = 0
    # medianFieldMain
    field = np.stack([median_filter(field[..., c], 3, mode='nearest') for c in range(2)], -1)
    # heldMain / weightMain
    moved = uniform_filter(pair.block_error(field[..., 0], field[..., 1], 1 - t, t), 3, mode='nearest')
    still = uniform_filter(pair.block_error(0 * field[..., 0], 0 * field[..., 1], 0.0, 0.0), 3, mode='nearest')
    weight = smoothstep(-HELD_MARGIN, 2 * HELD_MARGIN, moved - still).astype(np.float32)
    return pair.finish(pair.upsample(field), t, pair.upsample(weight))


VARIANTS = {'blend': blend, 'direct': direct, 'projected': projected}


def mapping(values, convert=str):
    out = {}
    for value in values or []:
        key, _, rest = value.partition('=')
        out[key] = convert(rest)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--clip', action='append', required=True, metavar='ID=FILE')
    parser.add_argument('--flow', action='append', required=True, metavar='ID=DIRECTORY')
    parser.add_argument('--frames', action='append', required=True, metavar='ID=N,N,...', help='worker frame ids')
    parser.add_argument('--variants', default='blend,direct,projected')
    parser.add_argument('--bilinear', action='store_true', help='sample moved pixels bilinearly (--flow-bilinear)')
    parser.add_argument('--width', type=int, default=1920)
    parser.add_argument('--height', type=int, default=1080)
    parser.add_argument('--ffmpeg', default='ffmpeg')
    parser.add_argument('--out', required=True, help='per-frame CSV')
    parser.add_argument('--save', help='directory for the generated frames as PNG (needs Pillow)')
    args = parser.parse_args()
    clips, flows = mapping(args.clip), mapping(args.flow)
    frames = mapping(args.frames, lambda text: [int(v) for v in text.split(',')])
    names = args.variants.split(',')
    order = 1 if args.bilinear else 3
    rows = []
    for clip, numbers in frames.items():
        wanted = {4 * (n - 2) + k for n in numbers for k in range(5)}
        source = decode(clips[clip], args.width, args.height, wanted, args.ffmpeg)
        for n in numbers:
            grid, invalid = load_flow(flows[clip], n)
            pair = Pair(source[4 * (n - 2)].astype(np.float32) / 255, source[4 * (n - 1)].astype(np.float32) / 255,
                        grid, invalid, order)
            for phase in (1, 2, 3):
                truth = source[4 * (n - 2) + phase]
                row = {'clip': clip, 'worker_frame': n, 'phase': phase, 'previous_source_frame': 4 * (n - 2),
                       'current_source_frame': 4 * (n - 1), 'truth_source_frame': 4 * (n - 2) + phase,
                       'sampling': 'bilinear' if args.bilinear else 'cubic'}
                for name in names:
                    image = to8(VARIANTS[name](pair, phase / 4))
                    row['psnr_' + name] = round(psnr(image, truth), 4)
                    if args.save:
                        from PIL import Image
                        os.makedirs(args.save, exist_ok=True)
                        Image.fromarray(image).save(os.path.join(args.save, f'{clip}_{n}_p{phase}_{name}.png'))
                rows.append(row)
                print(row, flush=True)
    with open(args.out, 'w', newline='', encoding='utf-8') as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    for name in names:
        print(f"{name:10s} mean PSNR {np.mean([r['psnr_' + name] for r in rows]):.3f} dB over {len(rows)} frames")


if __name__ == '__main__':
    main()

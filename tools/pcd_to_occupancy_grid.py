#!/usr/bin/env python3

import argparse
import math
from pathlib import Path

import numpy as np


def read_pcd_xyz(path):
    with path.open("rb") as stream:
        header = {}
        while True:
            line = stream.readline()
            if not line:
                raise ValueError("Invalid PCD file: missing DATA line")
            text = line.decode("ascii").strip()
            if not text or text.startswith("#"):
                continue
            key, *values = text.split()
            header[key.upper()] = values
            if key.upper() == "DATA":
                break

        fields = header["FIELDS"]
        sizes = [int(value) for value in header["SIZE"]]
        types = header["TYPE"]
        counts = [int(value) for value in header.get("COUNT", ["1"] * len(fields))]
        points = int(header["POINTS"][0])
        data_type = header["DATA"][0].lower()

        if any(count != 1 for count in counts):
            raise ValueError("PCD fields with COUNT != 1 are not supported")

        numpy_types = {
            ("F", 4): "<f4",
            ("F", 8): "<f8",
            ("I", 1): "<i1",
            ("I", 2): "<i2",
            ("I", 4): "<i4",
            ("U", 1): "<u1",
            ("U", 2): "<u2",
            ("U", 4): "<u4",
        }
        dtype = np.dtype([
            (field, numpy_types[(field_type, size)])
            for field, field_type, size in zip(fields, types, sizes)
        ])

        if data_type == "binary":
            cloud = np.fromfile(stream, dtype=dtype, count=points)
        elif data_type == "ascii":
            values = np.loadtxt(stream, dtype=np.float64, max_rows=points)
            cloud = np.empty(points, dtype=dtype)
            for index, field in enumerate(fields):
                cloud[field] = values[:, index]
        else:
            raise ValueError(f"Unsupported PCD DATA type: {data_type}")

    return np.column_stack((cloud["x"], cloud["y"], cloud["z"]))


def dilate(mask, iterations):
    result = mask
    for _ in range(iterations):
        padded = np.pad(result, 1, mode="constant")
        neighbors = [
            padded[row:row + result.shape[0], col:col + result.shape[1]]
            for row in range(3)
            for col in range(3)
        ]
        result = np.logical_or.reduce(neighbors)
    return result


def write_pgm(path, image):
    with path.open("wb") as stream:
        stream.write(f"P5\n{image.shape[1]} {image.shape[0]}\n255\n".encode("ascii"))
        stream.write(image.astype(np.uint8).tobytes())


def main():
    parser = argparse.ArgumentParser(
        description="Project a LIO-SAM PCD map into a Nav2 occupancy grid")
    parser.add_argument("pcd", type=Path)
    parser.add_argument("output", type=Path, help="Output path without extension")
    parser.add_argument("--resolution", type=float, default=0.05)
    parser.add_argument("--min-z", type=float, default=0.15)
    parser.add_argument("--max-z", type=float, default=2.0)
    parser.add_argument("--padding", type=float, default=0.5)
    parser.add_argument("--min-points", type=int, default=2)
    parser.add_argument("--dilate", type=int, default=1)
    args = parser.parse_args()

    xyz = read_pcd_xyz(args.pcd)
    finite = np.isfinite(xyz).all(axis=1)
    height = (xyz[:, 2] >= args.min_z) & (xyz[:, 2] <= args.max_z)
    obstacles = xyz[finite & height]
    if obstacles.size == 0:
        raise RuntimeError("No obstacle points remain after the height filter")

    min_x = math.floor((obstacles[:, 0].min() - args.padding) / args.resolution) * args.resolution
    min_y = math.floor((obstacles[:, 1].min() - args.padding) / args.resolution) * args.resolution
    max_x = math.ceil((obstacles[:, 0].max() + args.padding) / args.resolution) * args.resolution
    max_y = math.ceil((obstacles[:, 1].max() + args.padding) / args.resolution) * args.resolution
    width = int(round((max_x - min_x) / args.resolution)) + 1
    height_cells = int(round((max_y - min_y) / args.resolution)) + 1

    columns = np.floor((obstacles[:, 0] - min_x) / args.resolution).astype(np.int64)
    rows = np.floor((obstacles[:, 1] - min_y) / args.resolution).astype(np.int64)
    valid = (
        (columns >= 0) & (columns < width) &
        (rows >= 0) & (rows < height_cells)
    )
    counts = np.zeros((height_cells, width), dtype=np.uint32)
    np.add.at(counts, (rows[valid], columns[valid]), 1)

    occupied = counts >= args.min_points
    occupied = dilate(occupied, args.dilate)

    grid = np.full((height_cells, width), 254, dtype=np.uint8)
    grid[occupied] = 0
    image = np.flipud(grid)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    pgm_path = args.output.with_suffix(".pgm")
    yaml_path = args.output.with_suffix(".yaml")
    write_pgm(pgm_path, image)
    yaml_path.write_text(
        "\n".join([
            f"image: {pgm_path.name}",
            "mode: trinary",
            f"resolution: {args.resolution}",
            f"origin: [{min_x:.6f}, {min_y:.6f}, 0.0]",
            "negate: 0",
            "occupied_thresh: 0.65",
            "free_thresh: 0.25",
            "",
        ]),
        encoding="ascii",
    )

    print(f"Obstacle points: {len(obstacles)}")
    print(f"Grid: {width} x {height_cells}, resolution {args.resolution} m")
    print(f"Origin: [{min_x:.3f}, {min_y:.3f}, 0.0]")
    print(f"Saved: {pgm_path}")
    print(f"Saved: {yaml_path}")


if __name__ == "__main__":
    main()

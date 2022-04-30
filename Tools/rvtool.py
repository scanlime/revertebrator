#!/usr/bin/env python3

from tqdm import tqdm
import argparse
import json
import librosa
import multiprocessing
import numpy as np
import os
import random
import soundfile
import tempfile
import time
import warnings
import zipfile


def file_scan_worker(work):
    # the soundfile loading fallback warning on every mp3 file gets old
    warnings.filterwarnings("ignore", module=librosa.__name__)

    path, sr, fmin, fmax, res, vprob = work
    y, _ = librosa.load(path, sr=sr)
    f0, _, vp = librosa.pyin(
        y, sr=sr, fmin=fmin, fmax=fmax, fill_na=None, resolution=res
    )
    times = librosa.times_like(f0)
    voiced = vp >= vprob
    f0_v = f0[voiced]
    times_v = times[voiced]
    return (y, f0_v, times_v)


def do_scan(args):
    paths = []
    for srcdir in args.inputs:
        paths.extend(
            os.path.join(srcdir, d)
            for d in os.listdir(srcdir)[: args.file_limit]
            if not d.startswith(".")
        )
    random.shuffle(paths)
    tqdm.write(f"Processing {len(paths)} files")

    grain_f0 = []
    grain_x = []
    samples = []
    offset = 0
    with tqdm(total=len(paths), unit="file", unit_scale=True) as progress:
        with multiprocessing.pool.Pool(args.parallelism) as pool:
            work = [
                (p, args.sr, args.fmin, args.fmax, args.res, args.vprob) for p in paths
            ]
            results = pool.imap_unordered(file_scan_worker, work)
            for (sample, f0, times) in results:
                grain_f0.append(f0)
                grain_x.append(librosa.time_to_samples(times) + offset)
                samples.append(sample)
                offset += sample.shape[0]
                progress.update()

    filename = args.output or time.strftime(f"grains-%Y%m%d%H%M%S-{len(paths)}")
    if os.path.splitext(filename)[1] != ".npz":
        filename += ".npz"
    tqdm.write(f"Writing output to {filename}")
    if os.path.lexists(filename):
        raise IOError(f"Will not overwrite existing file at '{filename}'")

    grain_f0 = np.concatenate(grain_f0)
    grain_x = np.concatenate(grain_x)
    samples = np.concatenate(samples)
    sortkey = np.lexsort((grain_x, grain_f0))
    grain_f0 = grain_f0[sortkey]
    grain_x = grain_x[sortkey]
    np.savez(
        filename,
        y=samples,
        f0=grain_f0,
        x=grain_x,
        ylen=len(samples),
        sr=args.sr,
        vprob=args.vprob,
        fmin=args.fmin,
        fmax=args.fmax,
    )


def find_common_sample_rate(inputs):
    rates = [npz["sr"] for npz in inputs]
    if max(rates) != min(rates):
        raise ValueError(f"Inputs have inconsistent sample rates: {rates}")
    return int(rates[0])


def do_pack(args):
    inputs = [np.load(f) for f in args.inputs]
    sr = find_common_sample_rate(inputs)
    width_in_samples = int(args.width * sr)

    def build_combined_grain_index():
        f0, gx, ii, total_grains = [], [], [], 0
        for i, npz in enumerate(inputs):
            if0, igx, iylen = npz["f0"], npz["x"], int(npz["ylen"])
            assert len(if0) == len(igx)
            total_grains += len(igx)

            # Discard grains that will touch the boundary between inputs
            a = (igx > width_in_samples) & (igx < (iylen - width_in_samples - 1))
            if0, igx = if0[a], igx[a]

            f0.append(if0)
            gx.append(igx)
            ii.append(np.full(if0.shape, i))

        # Sorting, first by f0 and then by the final grain location (i, x)
        f0, gx, ii = map(np.concatenate, (f0, gx, ii))
        a = np.lexsort((gx, ii, f0))
        f0, gx, ii = f0[a], gx[a], ii[a]
        return f0, gx, ii, total_grains

    f0, gx, ii, total_grains = build_combined_grain_index()
    tqdm.write(f"Indexing {total_grains} total grains")

    def build_compact_index():
        rng = np.random.default_rng()

        # Allocate small frequency bins spaced evenly in the mel scale
        f0_range = (f0[0] - 0.1, f0[-1] + 0.1)
        max_bins = int(np.ceil(12 * f0_range[1] / f0_range[0] / args.res))
        mel_range = librosa.hz_to_mel(f0_range, htk=True)
        mel_bins = np.linspace(mel_range[0], mel_range[1], max_bins + 1)
        bins = librosa.mel_to_hz(mel_bins, htk=True)
        bin_x = np.searchsorted(f0, bins)

        cf0, cgx, cii, bf0, bx = [], [], [], [], []
        next_bx = 0
        for bin_id in range(len(bins) - 1):

            # Choose grains randomly if we have more than requested
            choices = np.arange(bin_x[bin_id], bin_x[bin_id + 1])
            rng.shuffle(choices)
            choices = choices[: args.max]
            choices.sort()

            # Skip empty or underfull bins
            if len(choices) < max(1, args.min):
                continue

            # Save chosen grains
            for c, o in ((cf0, f0), (cgx, gx), (cii, ii)):
                c.append(o[choices])

            # Update bin index
            bf0.append(cf0[-1].mean())
            bx.append(next_bx)
            next_bx += len(choices)

        if not len(cgx):
            raise ValueError("No grains left in compacted data")
        bx.append(next_bx)
        cgx, cii = map(np.concatenate, (cgx, cii))
        return cgx, cii, bf0, bx

    cgx, cii, bf0, bx = build_compact_index()
    tqdm.write(
        f"Using {len(cgx)} grains ({len(cgx)/total_grains:.2%}) in {len(bf0)} bins"
    )
    assert len(cgx) == len(cii)
    assert len(bf0) + 1 == len(bx)

    def collect_audio_data(file):
        # The compact index we have is sorted in f0 order.
        # Figure out a new ordering sorted by input file, and collect those grains

        yy_ptr = 0
        yygx = np.full(cgx.shape, -1)
        ixsort = np.lexsort((cgx, cii))
        imemo = {}
        samples_per_input = {}
        marker = (pow(-1, np.arange(0, args.mark)) * 0x7FFF).astype(np.int16)

        for grain in tqdm(ixsort, unit="grain", unit_scale=True):
            i, x = cii[grain], cgx[grain]
            if i != imemo.get("i"):
                tqdm.write(f"Reading {args.inputs[i]}")
                imemo = dict(i=i, y=inputs[i]["y"], end=0)

            # Copy the portion that doesn't overlap what we already copied
            grain_begin = x - width_in_samples
            grain_end = x + width_in_samples
            assert grain_begin >= 0 and grain_end < len(imemo["y"])
            grain_begin = max(grain_begin, imemo["end"])

            if grain_begin != imemo["end"] or 0 == imemo["end"]:
                # Mark discontinuities between non-adjacent grains, and across input edges
                file.write(marker)
                yy_ptr += len(marker)

            # Updated locations in the grain index
            assert width_in_samples <= x and x < grain_end
            yygx[grain] = yy_ptr + x - grain_begin

            y = np.round(imemo["y"][grain_begin:grain_end] * 0x7FFF).astype(np.int16)
            file.write(y)
            yy_ptr += len(y)
            samples_per_input[i] = samples_per_input.get(i, 0) + len(y)
            imemo["end"] = grain_end

        # We already marked the beginning of every input file, mark the end too
        file.write(marker)
        yy_ptr += len(marker)

        for i, name in enumerate(args.inputs):
            active = samples_per_input[i]
            total = inputs[i]["ylen"]
            tqdm.write(f"{name} using {active} samples, {active/total:.2%}")

        return yygx, {
            "sound_len": yy_ptr,
            "max_grain_width": args.width,
            "sample_rate": sr,
            "bin_x": list(map(int, bx)),
            "bin_f0": list(map(float, bf0)),
        }

    filename = args.output or time.strftime("voice-%Y%m%d%H%M%S")
    if os.path.splitext(filename)[1] != ".rvv":
        filename += ".rvv"
    if os.path.lexists(filename):
        raise IOError(f"Will not overwrite existing file at '{filename}'")

    with zipfile.ZipFile(filename, "x", zipfile.ZIP_STORED) as z:
        with tempfile.TemporaryFile(dir=os.path.dirname(filename)) as tmp:
            with soundfile.SoundFile(tmp, "w", sr, 1, "PCM_16", format="flac") as sound:
                yygx, index = collect_audio_data(sound)

            tmp.seek(0, os.SEEK_END)
            tmpLen = tmp.tell()
            tmp.seek(0)

            z.writestr(
                zipfile.ZipInfo("index.json"),
                json.dumps(index) + "\n",
                zipfile.ZIP_DEFLATED,
                9,
            )
            z.writestr(
                zipfile.ZipInfo("grains.u64"),
                yygx.astype("<u8").tobytes(),
                zipfile.ZIP_DEFLATED,
                9,
            )
            with z.open(zipfile.ZipInfo("sound.flac"), "w", force_zip64=True) as f:
                with tqdm(total=tmpLen, unit="byte", unit_scale=True) as progress:
                    while True:
                        block = tmp.read(1024 * 1024)
                        if not block:
                            break
                        f.write(block)
                        progress.update(len(block))

    tqdm.write(f"Completed {filename}")


def args_for_scan(subparsers):
    default_sr = 24000
    default_vprob = 0.99
    default_fmin = librosa.note_to_hz("C1")
    default_fmax = librosa.note_to_hz("C6")
    default_res = 0.05

    parser = subparsers.add_parser(
        "scan",
        description="Run pitch detection on batches of audio files, output an uncompressed grain database",
    )
    parser.set_defaults(func=do_scan)

    parser.add_argument(
        "inputs",
        metavar="SRC",
        nargs="+",
        help="directory with sound files to choose from",
    )
    parser.add_argument(
        "-o",
        metavar="DEST",
        dest="output",
        help="name of output file to generate [grains-###.npz]",
    )

    parser.add_argument(
        "-n",
        metavar="N",
        dest="file_limit",
        type=int,
        help="maximum number of input files per directory",
    )
    parser.add_argument(
        "-P",
        metavar="N",
        dest="parallelism",
        type=int,
        help="number of parallel jobs to run [#CPUs]",
    )

    parser.add_argument(
        "--sr",
        dest="sr",
        metavar="HZ",
        type=int,
        default=default_sr,
        help=f"sample rate to process and store at [{default_sr}]",
    )
    parser.add_argument(
        "--vprob",
        dest="vprob",
        metavar="P",
        type=float,
        default=default_vprob,
        help=f"minimum voicing probability [{default_vprob}]",
    )
    parser.add_argument(
        "--fmin",
        dest="fmin",
        metavar="HZ",
        type=float,
        default=default_fmin,
        help=f"lowest pitch to detect, in hz [{default_fmin}]",
    )
    parser.add_argument(
        "--fmax",
        dest="fmax",
        metavar="HZ",
        type=float,
        default=default_fmax,
        help=f"highest pitch to detect, in hz [{default_fmax}]",
    )
    parser.add_argument(
        "--res",
        dest="res",
        metavar="ST",
        type=float,
        default=default_res,
        help=f"pitch detection resolution in semitones [{default_res}]",
    )


def args_for_pack(subparsers):
    default_res = 0.01
    default_min = 3
    default_width = 3.0
    default_mark = 8

    parser = subparsers.add_parser(
        "pack",
        description="Build a compressed grain database from multiple uncompressed inputs",
    )
    parser.set_defaults(func=do_pack)

    parser.add_argument(
        "inputs",
        metavar="SRC",
        nargs="+",
        help="one or more npz files produced by the scan command",
    )
    parser.add_argument(
        "-o",
        metavar="DEST",
        dest="output",
        help="name of packed output file [voice-###.rvv]",
    )

    parser.add_argument(
        "-w",
        metavar="S",
        dest="width",
        type=float,
        default=default_width,
        help=f"maximum grain width in seconds [{default_width}]",
    )
    parser.add_argument(
        "-n",
        metavar="N",
        dest="max",
        type=int,
        help="maximum number of grains per bin [no limit]",
    )

    parser.add_argument(
        "--res",
        dest="res",
        metavar="ST",
        type=float,
        default=default_res,
        help=f"pitch binning resolution in semitones [{default_res}]",
    )
    parser.add_argument(
        "--min",
        dest="min",
        metavar="N",
        type=int,
        default=default_min,
        help=f"discard bins with fewer than this minimum number of grains [{default_min}]",
    )
    parser.add_argument(
        "--mark",
        dest="mark",
        metavar="N",
        type=int,
        default=default_mark,
        help=f"mark discontinuities with an N sample long noise [{default_mark}]",
    )


def main():
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(required=True)
    args_for_scan(subparsers)
    args_for_pack(subparsers)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    if hasattr(os, "nice"):
        os.nice(5)
    main()

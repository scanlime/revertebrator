#!/usr/bin/env python3

import librosa
import os, random, time, warnings
import multiprocessing, argparse
import numpy as np
from tqdm import tqdm

default_sr = 24000
default_vprob = 0.99
default_fmin = librosa.note_to_hz("C1")
default_fmax = librosa.note_to_hz("C6")
default_res = 0.05

parser = argparse.ArgumentParser(
    description="Run pitch detection on batches of audio files, output an uncompressed grain database"
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

parser.add_argument(
    "inputs", metavar="SRC", nargs="+", help="directory with sound files to choose from"
)
parser.add_argument(
    "-o",
    metavar="DEST",
    dest="output",
    help="name of output file to generate [grains-###.npz]",
)
parser.add_argument(
    "-P",
    metavar="N",
    dest="parallelism",
    type=int,
    help="number of parallel jobs to run [#CPUs]",
)
parser.add_argument(
    "-n",
    metavar="N",
    dest="file_limit",
    type=int,
    help="maximum number of input files per directory",
)
args = parser.parse_args()

# the soundfile loading fallback warning on every mp3 file gets old
warnings.filterwarnings("ignore", module=librosa.__name__)

# Load and pre-process samples in parallel
def load_one(p):
    y, sr = librosa.load(p, sr=args.sr)
    assert sr == args.sr
    f0, voiced_flag, voiced_probs = librosa.pyin(
        y, sr=args.sr, fmin=args.fmin, fmax=args.fmax, fill_na=None, resolution=args.res
    )
    times = librosa.times_like(f0)
    voiced = voiced_probs >= args.vprob
    f0_v = f0[voiced]
    times_v = times[voiced]
    return (y, f0_v, times_v)


paths = []
for srcdir in args.inputs:
    paths.extend(
        os.path.join(srcdir, d)
        for d in os.listdir(srcdir)[: args.file_limit]
        if not d.startswith(".")
    )
random.shuffle(paths)

grain_filename = args.output or time.strftime(f"grains-%Y%m%d%H%M-{len(paths)}.npz")
tqdm.write(f"Processing {len(paths)} files")

grain_f0 = []
grain_x = []
samples = []
offset = 0
with tqdm(total=len(paths), unit="file", unit_scale=True) as progress:
    with multiprocessing.pool.Pool(args.parallelism) as pool:
        for (sample, f0, times) in pool.imap_unordered(load_one, paths):
            grain_f0.append(f0)
            grain_x.append(librosa.time_to_samples(times) + offset)
            samples.append(sample)
            offset += sample.shape[0]
            progress.update()

tqdm.write("Collecting results")
grain_f0 = np.concatenate(grain_f0)
grain_x = np.concatenate(grain_x)
samples = np.concatenate(samples)
sortkey = np.lexsort((grain_x, grain_f0))
grain_f0 = grain_f0[sortkey]
grain_x = grain_x[sortkey]

tqdm.write(f"Writing output to {grain_filename}")
np.savez(
    grain_filename,
    y=samples,
    f0=grain_f0,
    x=grain_x,
    ylen=len(samples),
    sr=args.sr,
    vprob=args.vprob,
    fmin=args.fmin,
    fmax=args.fmax,
)

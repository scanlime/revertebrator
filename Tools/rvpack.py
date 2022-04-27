#!/usr/bin/env python3

import librosa, soundfile
import os, argparse, json, time, zipfile, tempfile, shutil
import numpy as np
from tqdm import tqdm

default_res = 0.01
default_width = 3.0

parser = argparse.ArgumentParser(description=
        'Build a compressed grain database from multiple uncompressed inputs')

parser.add_argument('--res', dest='res', metavar='ST', type=float, default=default_res,
                    help=f'pitch binning resolution in semitones [{default_res}]')
parser.add_argument('--min', dest='min', metavar='N', type=int, default=1,
                    help='discard bins with fewer than this minimum number of grains')

parser.add_argument('inputs', metavar='SRC', nargs='+',
                    help='one or more npz files produced by rvscan')
parser.add_argument('-w', metavar='S', dest='width', type=float, default=default_width,
                    help=f'maximum grain width in seconds [{default_width}]')
parser.add_argument('-n', metavar='N', dest='max', type=int,
                    help='maximum number of grains per bin [no limit]')
parser.add_argument('-o', metavar='DEST', dest='output',
                    help='name of packed output file [voice-###.rvv]')
args = parser.parse_args()

inputs = [np.load(f, mmap_mode='r') for f in args.inputs]

def find_common_sample_rate():
    rates = [npz['sr'] for npz in inputs]
    if max(rates) != min(rates):
        raise ValueError(f'Inputs have inconsistent sample rates: {rates}')
    return int(rates[0])

sr = find_common_sample_rate()
width_in_samples = int(args.width * sr)

def build_combined_grain_index():
    f0, gx, ii, total_grains = [], [], [], 0
    for i, npz in enumerate(tqdm(inputs)):
        if0, igx, iylen = npz['f0'], npz['x'], int(npz['ylen'])
        assert len(if0) == len(igx)
        total_grains += len(igx)

        # Discard grains that will touch the boundary between inputs
        a = (igx > width_in_samples) & (igx < (iylen - width_in_samples - 1));
        if0, igx = if0[a], igx[a]

        f0.append(if0)
        gx.append(igx)
        ii.append(np.full(if0.shape, i))

    f0, gx, ii = map(np.concatenate, (f0, gx, ii))
    a = np.lexsort((gx, ii, f0))
    f0, gx, ii = f0[a], gx[a], ii[a]
    return f0, gx, ii, total_grains

f0, gx, ii, total_grains = build_combined_grain_index()
tqdm.write(f'Indexing {total_grains} total grains')

def build_compact_index():
    rng = np.random.default_rng()

    # Allocate small frequency bins spaced evenly in the mel scale
    f0_range = (f0[0] - .1, f0[-1] + .1)
    max_bins = int(np.ceil(12 * f0_range[1] / f0_range[0] / args.res))
    mel_range = librosa.hz_to_mel(f0_range, htk=True)
    mel_bins = np.linspace(mel_range[0], mel_range[1], max_bins+1)
    bins = librosa.mel_to_hz(mel_bins, htk=True)
    bin_x = np.searchsorted(f0, bins)

    cf0, cgx, cii, bf0, bx = [], [], [], [], []
    next_bx = 0
    for bin_id in range(len(bins)-1):

        # Choose grains randomly if we have more than requested
        choices = np.arange(bin_x[bin_id], bin_x[bin_id+1])
        rng.shuffle(choices)
        choices = choices[:args.max]
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

    bx.append(next_bx)
    cf0, cgx, cii = map(np.concatenate, (cf0, cgx, cii))
    return cf0, cgx, cii, bf0, bx

cf0, cgx, cii, bf0, bx = build_compact_index()
tqdm.write(f'Using {len(cf0)} grains ({len(cf0)/total_grains:.2%}) in {len(bf0)} bins')
assert len(cf0) == len(cgx)
assert len(cf0) == len(cii)
assert len(bf0)+1 == len(bx)

# The next step is to collect audio data in order and make another table
# with the final location of each grain. This is where the final compressed
# output is written to disk, as 16-bit FLAC inside an uncompressed ZIP wrapper.
# The grain data is a binary file in this ZIP, and everything else goes into JSON.

def collect_audio_data(file):
    yy_ptr = 0
    yygx = np.full(cgx.shape, -1)

    for i, npz in enumerate(tqdm(inputs)):
        # Look at grains that are active in one input file at a time
        iact = cii == i
        iylen = int(npz['ylen'])
        igx = cgx[iact]
        igx_sort = np.argsort(igx)

        # Skip input files with zero grains in use
        if len(igx) < 1:
            continue

        # Figure out which audio samples are actually in use by looking
        # at the distance between each sample and its closest grain.
        chunk_size = 256*1024
        xxact = []
        for chunk in tqdm(range(0, iylen, chunk_size)):
            xact = np.arange(chunk, min(iylen, chunk+chunk_size), dtype=np.int64)
            nearest_grains = igx_sort.take(np.searchsorted(igx, xact, sorter=igx_sort)[:,np.newaxis] + [[-1,0]], mode='clip')
            distance = np.amin(np.abs(xact[:, np.newaxis] - igx[nearest_grains]), axis=1)
            xact = xact[distance <= width_in_samples]
            xxact.append(xact)
        xxact = np.concatenate(xxact)

        # Updated locations in the grain index
        yygx[iact] = np.searchsorted(xxact, igx) + yy_ptr
        yy_ptr += len(xxact)
        tqdm.write(f'{args.inputs[i]} using {len(xxact)} samples, {len(xxact)/iylen:.2%}')

        # Writing samples
        tqdm.write('writing samples..', end=' ')
        file.write(np.round(npz['y'][xxact] * 0x7fff).astype(np.int16))
        tqdm.write('ok')

    tqdm.write(f'{yy_ptr} samples total')
    return yygx, {
        'sound_len': yy_ptr,
        'max_grain_width': args.width,
        'sample_rate': sr,
        'bin_x': list(map(int, bx)),
        'bin_f0': list(map(float, bf0))
    }

filename = args.output or time.strftime('voice-%Y%m%d%H%M.rvv')
with zipfile.ZipFile(filename, 'x', zipfile.ZIP_STORED) as z:
    with tempfile.TemporaryFile(dir=os.path.dirname(filename)) as tmp:
        with soundfile.SoundFile(tmp, 'w', sr, 1, 'PCM_16', format='flac') as sound:
            yygx, index = collect_audio_data(sound)
        z.writestr(zipfile.ZipInfo('index.json'), json.dumps(index)+'\n', zipfile.ZIP_DEFLATED, 9)
        z.writestr(zipfile.ZipInfo('grains.u64'), yygx.astype('<u8').tobytes(), zipfile.ZIP_DEFLATED, 9)
        with z.open(zipfile.ZipInfo('sound.flac'), 'w', force_zip64=True) as f:
            tmp.seek(0)
            shutil.copyfileobj(tmp, f, 1024 * 1024)
tqdm.write(f'Completed {filename}')

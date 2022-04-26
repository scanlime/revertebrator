#!/usr/bin/env python3

import librosa
import argparse, json, glob, base64, time
import matplotlib.pyplot as plt
import numpy as np
from glob import glob
from tqdm import tqdm

parser = argparse.ArgumentParser(description=
        'Build a compressed grain database from multiple uncompressed inputs')

parser.add_argument('--res', dest='res', metavar='ST', type=float, default=0.01,
                    help='pitch binning resolution in semitones [0.01]')
parser.add_argument('--min', dest='min', metavar='N', type=int, default=1,
                    help='discard bins with fewer than this minimum number of grains')

parser.add_argument('inputs', metavar='SRC', nargs='+',
                    help='one or more npz files produced by rvscan')
parser.add_argument('-w', metavar='S', dest='width', type=float, default=3.0,
                    help='maximum grain width in seconds [3.0]')
parser.add_argument('-n', metavar='N', dest='max', type=int, default=128,
                    help='maximum number of grains per bin [128]')
parser.add_argument('-o', metavar='DEST', dest='output',
                    help='name of packed output file [voice-###.rvv]')
args = parser.parse_args()
output_name = args.output or time.strftime('voice-%Y%m%d%H%M.rvv')

# Build combined grain index
voices = [np.load(f, mmap_mode='r') for f in args.inputs]
sr = int(voices[0]['sr'])
max_grain_sr = int(args.width * sr)

gf0, gv, gx = [], [], []
for v, grains in enumerate(tqdm(voices)):
    print(voice_names[v])
    vgx = grains['x']
    vgf0 = grains['f0']
    vylen = int(grains['ylen'])
    
    # Just for evaluation purposes, show the spectral contribution from each input file
    plt.figure()
    plt.title(voice_names[v])
    plt.xlabel('hz')
    plt.ylabel('grains')
    plt.hist(vgf0, 1000, histtype='stepfilled', log=True) 
    
    # Discard grains that will cross the boundary between voices
    range_filter = (vgx > max_grain_sr) & (vgx < (vylen - max_grain_sr - 1))
    vgx = vgx[range_filter]
    vgf0 = vgf0[range_filter]
    
    gx.append(vgx)
    gf0.append(vgf0)
    gv.append(np.full(vgf0.shape, v))

gf0, gv, gx = map(np.concatenate, (gf0, gv, gx))
k = np.lexsort((gx, gv, gf0))
gf0, gv, gx = gf0[k], gv[k], gx[k]

# Examine the frequency distribution of the combined index.
# These buckets will be smaller than the pitch buckets in pyin()
plt.title('Combined f0 spectrum')
plt.xlabel('mels')
max_frequency_bins = int(np.ceil(12 * gf0[-1]/gf0[0] / args.res))
hist, bins, _ = plt.hist(librosa.hz_to_mel(gf0, htk=True), max_frequency_bins, histtype='step', log=True)
bins = librosa.mel_to_hz(bins, htk=True)

# Sample at most max_grains_per_bin for a compacted grain index.
# Each bin is assigned a single f0, and the different grains within
# that bin can be selected at playback time.

# To do: might want to intentionally choose a particular distribution
# of grains to keep, right now it is a uniform sample from the input,
# so the relative space taken up by different voices will depend on
# how many grains we have saved from those voices.

# Combined grain index: f0, voice selector, sample location
cgf0, cgv, cgx = [], [], []

# Bin index: f0, first grain, number of grains
bf0, bx = [], []

rng = np.random.default_rng()
bin_x = np.searchsorted(gf0, bins)
next_bx = 0

for bin_id, bin_size in enumerate(bin_x[1:] - bin_x[:-1]):
    # Choose grains
    choices = np.arange(bin_x[bin_id], bin_x[bin_id+1])
    rng.shuffle(choices)
    choices = choices[:args.max]
    choices.sort()
    
    # Skip empty or underfull bins
    if len(choices) < args.min:
        continue
    
    # Save chosen grains
    for c, o in ((cgf0, gf0), (cgv, gv), (cgx, gx)):
        c.append(o[choices])

    # Update bin index
    bf0.append(cgf0[-1].mean())
    bx.append(next_bx)
    next_bx += len(choices)
bx.append(next_bx)
    
bx = np.asarray(bx)
cgf0, cgv, cgx = map(np.concatenate, (cgf0, cgv, cgx))

plt.figure()
plt.title('Grains per bin')
plt.xlabel('grains')
plt.hist(bx[1:] - bx[:-1], 100, log=True)

# This should give us a frequency distribution with the peaks shaved down
plt.figure()
plt.title('Compacted f0 spectrum')
plt.xlabel('mels')
plt.hist(librosa.hz_to_mel(cgf0, htk=True), max_frequency_bins, histtype='step')

print(f'Using {len(cgf0)} grains out of {len(gf0)} ({len(cgf0)/len(gf0):.2%})')
print(f'Using {len(bf0)} frequency bins')

# Collect audio data

yy_ptr = 0
yygx = np.full(cgx.shape, -1)
distance_maps = []

with sf.SoundFile(output_sound_file, 'w', sr, 1, 'PCM_16') as yy:
    
    # One input file at a time
    for v, grains in enumerate(tqdm(voices)):
        vact = cgv == v
        vylen = int(grains['ylen'])
        vgx = cgx[vact]
        vgx_sort = np.argsort(vgx)

        # Skip input files with zero grains in use
        if len(vgx) < 1:
            continue

        # Figure out which audio samples are actually in use by looking
        # at the distance between each sample and its closest grain.
        chunk_size = 256*1024
        xxact = []
        for chunk in tqdm(range(0, vylen, chunk_size)):
            xact = np.arange(chunk, min(vylen, chunk+chunk_size), dtype=np.int64)
            nearest_grains = vgx_sort.take(np.searchsorted(vgx, xact, sorter=vgx_sort)[:,np.newaxis] + [[-1,0]], mode='clip')
            distance = np.amin(np.abs(xact[:, np.newaxis] - vgx[nearest_grains]), axis=1)
            xact = xact[distance <= max_grain_sr]
            distance_maps.append(distance.min())
            xxact.append(xact)
            del distance
            del nearest_grains
            del xact
        xxact = np.concatenate(xxact)
    
        # Updated locations in the grain index
        yygx[vact] = np.searchsorted(xxact, vgx) + yy_ptr
        yy_ptr += len(xxact)
        print(f'{voice_names[v]} using {len(xxact)} samples, {len(xxact)/vylen:.2%}')

        # Writing samples
        print('writing samples..', end=' ')
        yy.write(np.round(grains['y'][xxact] * 0x7fff).astype(np.int16))
        print('ok')
        del xxact
    
print(f'{yy_ptr} samples total')


# Write out the index to JSON
index = {
    'sound_file': output_sound_file,
    'sound_len': yy_ptr,
    'max_grain_width': max_grain_width,
    'sample_rate': sr,
    'bin_x': list(map(int, bx)),
    'bin_f0': list(map(float, bf0)),
    'grain_x': base64.b64encode(yygx.astype('<u8').tobytes()).decode('ascii'),
}

json.dump(index, open(output_index_file, 'w'))
print('Saved')


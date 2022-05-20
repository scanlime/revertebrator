#!/usr/bin/env python3

import tqdm
import argparse
import json
import queue
import audioread
import librosa
import multiprocessing
import multiprocessing.managers
import numpy as np
import os
import random
import soundfile
import tempfile
import time
import sqlite3
import warnings
import zipfile


class Database:
    def arguments(parser):
        databaseFile = "rvtool.db"
        parser.add_argument(
            "-f",
            metavar="FILE",
            dest="databaseFile",
            default=databaseFile,
            help=f"name of database file to use [{databaseFile}]",
        )

    def __init__(self, args):
        self.con = sqlite3.connect(args.databaseFile)
        self.con.executescript(
            """
            create table if not exists files
                (id integer primary key autoincrement, path varchar unique,
                samplerate real, duration real);
            create table if not exists pitch_features
                (file integer, f0 real, probability real, time real);
            create index if not exists file_path on files(path);
            create index if not exists pitch_features_loc on pitch_features(file, time);
            create index if not exists pitch_features_f0 on pitch_features(f0);
        """
        )

    def hasFile(self, path):
        cur = self.con.cursor()
        cur.execute("select count(id) from files where path = ?", (path,))
        return cur.fetchone()[0] == 1

    def storeFile(self, path, audio, blocks):
        cur = self.con.cursor()
        cur.execute("begin")
        cur.execute(
            "insert into files(path, samplerate, duration) values (?,?,?)",
            (path, audio.samplerate, audio.duration),
        )
        fileId = cur.lastrowid
        rows = []
        for (f0, v, times) in blocks:
            assert len(f0) == len(v)
            assert len(f0) == len(times)
            rows.extend((fileId, f0[i], v[i], times[i]) for i in range(len(f0)))
        cur.executemany(
            "insert into pitch_features(file, f0, probability, time) values (?,?,?,?)",
            rows,
        )
        cur.execute("commit")


class FileScanner:
    def arguments(parser):
        parallelism = os.cpu_count()
        secondsPerBlock = 8
        secondsOverlap = 1
        freqMinHz = librosa.note_to_hz("C1")
        freqMaxHz = librosa.note_to_hz("C6")
        resolution = 0.05
        parser.set_defaults(factory=FileScanner)
        parser.add_argument(
            "-P",
            metavar="N",
            dest="parallelism",
            type=int,
            default=parallelism,
            help="number of parallel jobs to run [{parallelism}]",
        )
        parser.add_argument(
            "--block-size",
            metavar="SEC",
            dest="secondsPerBlock",
            type=float,
            default=secondsPerBlock,
            help="length of audio blocks to process, in seconds [{secondsPerBlock}]",
        )
        parser.add_argument(
            "--block-overlap",
            metavar="SEC",
            dest="secondsOverlap",
            type=float,
            default=secondsOverlap,
            help="amount of overlap between audio blocks, in seconds [{secondsOverlap}]",
        )
        parser.add_argument(
            "--fmin",
            dest="freqMinHz",
            metavar="HZ",
            type=float,
            default=freqMinHz,
            help=f"lowest pitch to detect, in hz [{freqMinHz}]",
        )
        parser.add_argument(
            "--fmax",
            dest="freqMaxHz",
            metavar="HZ",
            type=float,
            default=freqMaxHz,
            help=f"highest pitch to detect, in hz [{freqMaxHz}]",
        )
        parser.add_argument(
            "--res",
            dest="resolution",
            metavar="ST",
            type=float,
            default=resolution,
            help=f"pitch detection resolution in semitones [{resolution}]",
        )
        parser.add_argument(
            "inputs",
            metavar="SRC",
            nargs="+",
            help="files and directories to scan for audio",
        )

    def __init__(self, args):
        self.db = Database(args)
        self.args = args
        self.pendingFiles = []
        self.pendingBlocks = []

    def _start(self):
        self.manager = multiprocessing.managers.SharedMemoryManager()
        self.manager.start()
        self.pool = multiprocessing.Pool(self.args.parallelism)

    def _stop(self):
        self.pool.close()
        self.pool.join()
        self.manager.shutdown()

    def run(self):
        self._start()
        try:
            self._visitInputs()
        finally:
            self._stop()
            self._storeCompletedFiles()

    def _visitInputs(self):
        files = set()
        for input in tqdm.tqdm(self.args.inputs, unit="input"):
            for (dirpath, dirnames, filenames) in os.walk(input):
                for filename in filenames:
                    files.add(os.path.realpath(os.path.join(dirpath, filename)))
        for path in tqdm.tqdm(files, unit="file"):
            self._visitFile(path)
            self._storeCompletedFiles()

    def _visitFile(self, path):
        if not self.db.hasFile(path):
            try:
                with audioread.audio_open(path) as audio:
                    self._readAudio(path, audio)
            except (EOFError, audioread.exceptions.NoBackendError):
                pass

    def _enqueueBlock(self, sampleRate, sampleOffset, i16Samples):
        # Do the mixdown to mono and the int to float conversion as we copy
        # from the audioread output buffer to a shared memory buffer
        numSamples = i16Samples.shape[0]
        shm = self.manager.SharedMemory(numSamples * 8)
        shmArray = np.ndarray((numSamples,), dtype=float, buffer=shm.buf)
        np.sum(i16Samples, dtype=float, out=shmArray, axis=-1)
        shmArray *= 1 / 0x7FFF
        block = self.pool.apply_async(
            self.__class__._blockWorker, (self.args, sampleRate, sampleOffset, shm)
        )
        self.pendingBlocks.append(block)
        return block

    def _blockWorker(args, sampleRate, sampleOffset, shm):
        analysisRate = 22050
        minProbability = 0.5
        numSamples = shm.size // 8
        samples = np.ndarray(numSamples, dtype=float, buffer=shm.buf)
        resampled = librosa.resample(
            samples, orig_sr=sampleRate, target_sr=analysisRate
        )
        shm.unlink()
        f0, _, v = librosa.pyin(
            resampled,
            sr=analysisRate,
            fmin=args.freqMinHz,
            fmax=args.freqMaxHz,
            fill_na=None,
            resolution=args.resolution,
        )
        times = librosa.times_like(f0, sr=analysisRate) + (sampleOffset / sampleRate)
        filter = v >= minProbability
        return (f0[filter], v[filter], times[filter])

    def _waitForPendingBlocks(self):
        maxQueueDepth = self.args.parallelism * 3
        while len(self.pendingBlocks) > maxQueueDepth:
            self.pendingBlocks = [b for b in self.pendingBlocks if not b.ready()]
            time.sleep(1)

    def _storeCompletedFiles(self):
        while self.pendingFiles:
            path, audio, blocks = self.pendingFiles[0]
            for block in blocks:
                if not block.ready():
                    return
            self.db.storeFile(path, audio, [b.get() for b in blocks])
            del self.pendingFiles[0]

    def _readAudio(self, path, audio):
        bufOffset = 0
        bufLength = 0
        bufChunks = []
        blocks = []
        samplesPerBlock = int(audio.samplerate * self.args.secondsPerBlock)
        samplesBetweenBlocks = samplesPerBlock - int(
            audio.samplerate * self.args.secondsOverlap
        )
        for chunk in audio:
            chunk = np.ndarray(
                (len(chunk) // audio.channels // 2, audio.channels),
                dtype=np.int16,
                buffer=chunk,
            )
            bufChunks.append(chunk)
            bufLength += chunk.shape[0]
            while bufLength > samplesPerBlock:
                self._waitForPendingBlocks()
                bufChunks = [np.vstack(bufChunks)]
                blocks.append(
                    self._enqueueBlock(
                        audio.samplerate, bufOffset, bufChunks[0][:samplesPerBlock]
                    )
                )
                bufChunks = [bufChunks[0][samplesBetweenBlocks:]]
                bufOffset += samplesBetweenBlocks
                bufLength = bufChunks[0].shape[0]
        blocks.append(
            self._enqueueBlock(audio.samplerate, bufOffset, np.vstack(bufChunks))
        )
        self.pendingFiles.append((path, audio, blocks))


# def find_common_sample_rate(inputs):
#     rates = [npz["sr"] for npz in inputs]
#     if max(rates) != min(rates):
#         raise ValueError(f"Inputs have inconsistent sample rates: {rates}")
#     return int(rates[0])
#
#
# def do_pack(args):
#     inputs = [np.load(f) for f in args.inputs]
#     sr = find_common_sample_rate(inputs)
#     width_in_samples = int(args.width * sr)
#
#     def build_combined_grain_index():
#         f0, gx, ii, total_grains = [], [], [], 0
#         for i, npz in enumerate(inputs):
#             if0, igx, iylen = npz["f0"], npz["x"], int(npz["ylen"])
#             assert len(if0) == len(igx)
#             total_grains += len(igx)
#
#             # Discard grains that will touch the boundary between inputs
#             a = (igx > width_in_samples) & (igx < (iylen - width_in_samples - 1))
#             if0, igx = if0[a], igx[a]
#
#             f0.append(if0)
#             gx.append(igx)
#             ii.append(np.full(if0.shape, i))
#
#         # Sorting, first by f0 and then by the final grain location (i, x)
#         f0, gx, ii = map(np.concatenate, (f0, gx, ii))
#         a = np.lexsort((gx, ii, f0))
#         f0, gx, ii = f0[a], gx[a], ii[a]
#         return f0, gx, ii, total_grains
#
#     f0, gx, ii, total_grains = build_combined_grain_index()
#     tqdm.write(f"Indexing {total_grains} total grains")
#
#     def build_compact_index():
#         rng = np.random.default_rng()
#
#         # Allocate small frequency bins spaced evenly in the mel scale
#         f0_range = (f0[0] - 0.1, f0[-1] + 0.1)
#         max_bins = int(np.ceil(12 * f0_range[1] / f0_range[0] / args.res))
#         mel_range = librosa.hz_to_mel(f0_range, htk=True)
#         mel_bins = np.linspace(mel_range[0], mel_range[1], max_bins + 1)
#         bins = librosa.mel_to_hz(mel_bins, htk=True)
#         bin_x = np.searchsorted(f0, bins)
#
#         cf0, cgx, cii, bf0, bx = [], [], [], [], []
#         next_bx = 0
#         for bin_id in range(len(bins) - 1):
#
#             # Choose grains randomly if we have more than requested
#             choices = np.arange(bin_x[bin_id], bin_x[bin_id + 1])
#             rng.shuffle(choices)
#             choices = choices[: args.max]
#             choices.sort()
#
#             # Skip empty or underfull bins
#             if len(choices) < max(1, args.min):
#                 continue
#
#             # Save chosen grains
#             for c, o in ((cf0, f0), (cgx, gx), (cii, ii)):
#                 c.append(o[choices])
#
#             # Update bin index
#             bf0.append(cf0[-1].mean())
#             bx.append(next_bx)
#             next_bx += len(choices)
#
#         if not len(cgx):
#             raise ValueError("No grains left in compacted data")
#         bx.append(next_bx)
#         cgx, cii = map(np.concatenate, (cgx, cii))
#         return cgx, cii, bf0, bx
#
#     cgx, cii, bf0, bx = build_compact_index()
#     tqdm.write(
#         f"Using {len(cgx)} grains ({len(cgx)/total_grains:.2%}) in {len(bf0)} bins"
#     )
#     assert len(cgx) == len(cii)
#     assert len(bf0) + 1 == len(bx)
#
#     def collect_audio_data(file):
#         # The compact index we have is sorted in f0 order.
#         # Figure out a new ordering sorted by input file, and collect those grains
#
#         yy_ptr = 0
#         yygx = np.full(cgx.shape, -1)
#         ixsort = np.lexsort((cgx, cii))
#         imemo = {}
#         samples_per_input = {}
#         marker = (pow(-1, np.arange(0, args.mark)) * 0x7FFF).astype(np.int16)
#
#         for grain in tqdm(ixsort, unit="grain", unit_scale=True):
#             i, x = cii[grain], cgx[grain]
#             if i != imemo.get("i"):
#                 tqdm.write(f"Reading {args.inputs[i]}")
#                 imemo = dict(i=i, y=inputs[i]["y"], end=0)
#
#             # Copy the portion that doesn't overlap what we already copied
#             grain_begin = x - width_in_samples
#             grain_end = x + width_in_samples
#             assert grain_begin >= 0 and grain_end < len(imemo["y"])
#             grain_begin = max(grain_begin, imemo["end"])
#
#             if grain_begin != imemo["end"] or 0 == imemo["end"]:
#                 # Mark discontinuities between non-adjacent grains, and across input edges
#                 file.write(marker)
#                 yy_ptr += len(marker)
#
#             # Updated locations in the grain index
#             assert width_in_samples <= x and x < grain_end
#             yygx[grain] = yy_ptr + x - grain_begin
#
#             y = np.round(imemo["y"][grain_begin:grain_end] * 0x7FFF).astype(np.int16)
#             file.write(y)
#             yy_ptr += len(y)
#             samples_per_input[i] = samples_per_input.get(i, 0) + len(y)
#             imemo["end"] = grain_end
#
#         # We already marked the beginning of every input file, mark the end too
#         file.write(marker)
#         yy_ptr += len(marker)
#
#         for i, name in enumerate(args.inputs):
#             active = samples_per_input.get(i, 0)
#             total = inputs[i]["ylen"]
#             tqdm.write(f"{name} using {active} samples, {active/total:.2%}")
#
#         return yygx, {
#             "sound_len": yy_ptr,
#             "max_grain_width": args.width,
#             "sample_rate": sr,
#             "bin_x": list(map(int, bx)),
#             "bin_f0": list(map(float, bf0)),
#         }
#
#     filename = args.output or time.strftime("voice-%Y%m%d%H%M%S")
#     if os.path.splitext(filename)[1] != ".rvv":
#         filename += ".rvv"
#     if os.path.lexists(filename):
#         raise IOError(f"Will not overwrite existing file at '{filename}'")
#
#     with zipfile.ZipFile(filename, "x", zipfile.ZIP_STORED) as z:
#         with tempfile.TemporaryFile(dir=os.path.dirname(filename)) as tmp:
#             with soundfile.SoundFile(tmp, "w", sr, 1, "PCM_16", format="flac") as sound:
#                 yygx, index = collect_audio_data(sound)
#
#             tmp.seek(0, os.SEEK_END)
#             tmpLen = tmp.tell()
#             tmp.seek(0)
#
#             z.writestr(
#                 zipfile.ZipInfo("index.json"),
#                 json.dumps(index) + "\n",
#                 zipfile.ZIP_DEFLATED,
#                 9,
#             )
#             z.writestr(
#                 zipfile.ZipInfo("grains.u64"),
#                 yygx.astype("<u8").tobytes(),
#                 zipfile.ZIP_DEFLATED,
#                 9,
#             )
#             with z.open(zipfile.ZipInfo("sound.flac"), "w", force_zip64=True) as f:
#                 with tqdm(total=tmpLen, unit="byte", unit_scale=True) as progress:
#                     while True:
#                         block = tmp.read(1024 * 1024)
#                         if not block:
#                             break
#                         f.write(block)
#                         progress.update(len(block))
#
#     tqdm.write(f"Completed {filename}")


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
    Database.arguments(parser)
    subparsers = parser.add_subparsers(required=True)
    FileScanner.arguments(
        subparsers.add_parser(
            "scan",
            description="Run pitch detection on batches of audio files, output an uncompressed grain database",
        )
    )
    args = parser.parse_args()
    args.factory(args).run()


if __name__ == "__main__":
    if hasattr(os, "nice"):
        os.nice(5)
    main()

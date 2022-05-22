#!/usr/bin/env python3

import audioread
import argparse
import json
import librosa
import multiprocessing
import multiprocessing.managers
import numpy as np
import os
import queue
import random
import soundfile
import sqlite3
import tempfile
import time
import tqdm
import zipfile


class Database:
    def arguments(parser):
        databaseFile = os.path.expanduser("~/.local/share/rvtool.db")
        parser.add_argument(
            "-F",
            metavar="FILE",
            dest="databaseFile",
            default=databaseFile,
            help=f"name of database file to use [{databaseFile}]",
        )

    def queryArguments(parser):
        default_order = "path"
        default_filter = "1"
        parser.add_argument(
            "-r",
            metavar="HZ",
            dest="queryRate",
            help=f"select files of a specified sample rate only",
        )
        parser.add_argument(
            "-c",
            metavar="N",
            dest="queryChannels",
            help=f"select files of a specified channel count only",
        )
        parser.add_argument(
            "-p",
            metavar="GLOB",
            dest="queryPath",
            help=f"select file paths that match a glob pattern",
        )
        parser.add_argument(
            "-s",
            metavar="SQL",
            dest="queryOrder",
            default=default_order,
            help=f"comma-separated list of expressions to sort files by [{default_order}]",
        )
        parser.add_argument(
            "-f",
            metavar="SQL",
            dest="queryFilter",
            default=default_filter,
            help=f"additional expression to filter results with [{default_filter}]",
        )

    def iterFiles(self, args):
        sql = f"""
            select id, path, samplerate, channels, duration,
                (select count(pitch_features.time)
                from pitch_features where files.id == pitch_features.file)
                    as numPitchFeatures
            from files where ({args.queryFilter})
        """
        params = []
        if args.queryRate is not None:
            sql += " and samplerate = ?"
            params.append(args.queryRate)
        if args.queryChannels is not None:
            sql += " and channels = ?"
            params.append(args.queryChannels)
        if args.queryPath is not None:
            sql += " and path glob ?"
            params.append(args.queryPath)
        sql += " order by " + args.queryOrder
        cur = self.con.cursor()
        for row in cur.execute(sql, params):
            yield dict(zip((d[0] for d in cur.description), row))

    def pitchFeaturesArray(self, fileId):
        cur = self.con.cursor()
        cur.execute(
            "select time, f0, probability from pitch_features "
            "where file = ? order by time asc",
            (fileId,),
        )
        return np.asarray(cur.fetchall()).reshape((-1, 3))

    def __init__(self, args):
        self.con = sqlite3.connect(args.databaseFile)
        self.con.executescript(
            """
            create table if not exists files
                (id integer primary key autoincrement, path varchar unique,
                samplerate real, channels integer, duration real);
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

    def storeFile(self, audio, blocks):
        cur = self.con.cursor()
        cur.execute("begin")
        cur.execute(
            "insert into files(path, samplerate, channels, duration) values (?,?,?,?)",
            (audio.path, audio.samplerate, audio.channels, audio.duration),
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
        tqdm.tqdm.write(f"Finished {audio.path}")


class UnrecoverableAudioError(Exception):
    pass


class BackwardSeek(Exception):
    pass


class BufferedAudioReader:
    def __init__(self, path):
        self.path = path
        self._open()

    def _open(self):
        tqdm.tqdm.write(f"Reading {self.path}")
        self._reader = audioread.audio_open(self.path)
        self.samplerate = self._reader.samplerate
        self.duration = self._reader.duration
        self.channels = self._reader.channels
        self.numSamples = int(self.samplerate * self.duration)
        self._buffer = [np.zeros((0, self.channels), dtype=np.int16)]
        self._begin = 0
        self._end = 0

    def read(self, sampleOffset, numSamples, retries=4):
        # audioread can fail intermittently due to timeouts in its ffmpeg
        # backend.. allow a few retries by reopening the reader.
        while True:
            try:
                return self._readInternal(sampleOffset, numSamples)
            except (
                BackwardSeek,
                EOFError,
                audioread.exceptions.NoBackendError,
                audioread.ffdec.ReadTimeoutError,
            ) as e:
                if retries > 0:
                    self._open()
                    retries -= 1
                else:
                    raise UnrecoverableAudioError(e)

    def _readInternal(self, sampleOffset, numSamples):
        if sampleOffset < self._begin:
            raise BackwardSeek()

        for chunk in self._reader:
            chunk = np.ndarray(
                (len(chunk) // self.channels // 2, self.channels),
                dtype=np.int16,
                buffer=chunk,
            )
            self._buffer.append(chunk)
            self._end += chunk.shape[0]
            if self._end >= sampleOffset + numSamples:
                break

        discardLen = sampleOffset - self._begin
        self._buffer = [np.vstack(self._buffer)[discardLen:]]
        self._begin += discardLen
        assert self._begin == sampleOffset
        return self._buffer[0][:numSamples]


class FileScanner:
    _ignoreExtensions = (
        ".jpg .png .gif .txt .vtt .json .description .gz .zip .rvv".split()
    )

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
                    if (
                        not filename.startswith(".")
                        and not os.path.splitext(filename)[1] in self._ignoreExtensions
                    ):
                        files.add(os.path.realpath(os.path.join(dirpath, filename)))
        files = list(files)
        random.shuffle(files)
        for path in tqdm.tqdm(files, unit="file"):
            self._waitForPendingBlocks()
            self._visitFile(path)
            self._storeCompletedFiles()

    def _visitFile(self, path):
        if not self.db.hasFile(path):
            try:
                self._readAudio(BufferedAudioReader(path))
            except UnrecoverableAudioError as e:
                tqdm.tqdm.write(f"Giving up on {path} because of error, {e}")

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
        niceness = 10
        analysisRate = 22050
        minProbabilityToStore = 0.5

        if hasattr(os, "nice") and os.nice(0) < niceness:
            os.nice(niceness)

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
        filter = v >= minProbabilityToStore
        return (f0[filter], v[filter], times[filter])

    def _waitForPendingBlocks(self):
        maxPending = self.args.parallelism * 2
        while len(self.pendingBlocks) >= maxPending:
            assert len(self.pendingFiles) <= maxPending
            self.pendingBlocks = [b for b in self.pendingBlocks if not b.ready()]
            tqdm.tqdm.write(
                f"Queued blocks: {len(self.pendingBlocks)}, files: {len(self.pendingFiles)}"
            )
            time.sleep(30)
            self._storeCompletedFiles()

    def _storeCompletedFiles(self):
        while self.pendingFiles:
            audio, blocks = self.pendingFiles[0]
            for block in blocks:
                if not block.ready():
                    return
            self.db.storeFile(audio, [b.get() for b in blocks])
            del self.pendingFiles[0]

    def _readAudio(self, audio):
        samplesPerBlock = int(audio.samplerate * self.args.secondsPerBlock)
        samplesBetweenBlocks = samplesPerBlock - int(
            audio.samplerate * self.args.secondsOverlap
        )
        blocks = []
        for x in range(0, audio.numSamples, samplesBetweenBlocks):
            blocks.append(
                self._enqueueBlock(audio.samplerate, x, audio.read(x, samplesPerBlock))
            )
        self.pendingFiles.append((audio, blocks))


class FileListing:
    def arguments(parser):
        Database.queryArguments(parser)
        parser.set_defaults(factory=FileListing)

    def __init__(self, args):
        self.db = Database(args)
        self.args = args

    def run(self):
        for row in self.db.iterFiles(self.args):
            print(
                f"{row['path']}\t"
                f"{row['samplerate']} Hz\t"
                f"{row['channels']} ch\t"
                f"{row['duration']} seconds\t"
                f"{row['numPitchFeatures']} pitch features"
            )


class FilePacker:
    def arguments(parser):
        default_output = time.strftime("voice-%Y%m%d%H%M%S.rvv")
        default_res = 0.01
        default_min = 3
        default_max = 500
        default_width = 3.0
        default_mark = 8
        default_vprob = 0.99
        Database.queryArguments(parser)
        parser.set_defaults(factory=FilePacker)
        parser.add_argument(
            "-o",
            metavar="DEST",
            dest="output",
            default=default_output,
            help=f"name of packed output file [{default_output}]",
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
            default=default_max,
            help=f"maximum number of grains per bin [{default_max}]",
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

    def __init__(self, args):
        self.args = args
        self.db = Database(args)
        self.files = list(self.db.iterFiles(args))
        self.samplerate = self._singleValuedFileProperty("samplerate")
        self.channels = self._singleValuedFileProperty("channels")
        self.widthInSamples = int(args.width * self.samplerate)
        self.discontinuityMarker = (
            (pow(-1, np.arange(0, args.mark)) * 0x7FFF)
            .astype(np.int16)
            .repeat(self.channels)
            .reshape((-1, self.channels))
        )
        self.filename = args.output
        if os.path.splitext(self.filename)[1] != ".rvv":
            self.filename += ".rvv"
        if os.path.lexists(self.filename):
            raise IOError(f"Will not overwrite existing file at '{self.filename}'")

    def _singleValuedFileProperty(self, column):
        values = set()
        for row in self.files:
            values.add(row[column])
        if len(values) == 0:
            raise ValueError("No input files selected")
        elif len(values) != 1:
            raise ValueError(f"Inputs have inconsistent values for {column}, {values}")
        return values.pop()

    def _buildCombinedIndex(self):
        f0, gx, ii = [], [], []
        for i, row in enumerate(self.files):
            assert self.samplerate == row["samplerate"]
            features = self.db.pitchFeaturesArray(row["id"])

            igx = (features[:, 0] * self.samplerate).astype(int)
            if0 = features[:, 1]
            iv = features[:, 2]
            iylen = int(self.samplerate * row["duration"])

            # Discard features that are below the minimum probability
            a = iv >= self.args.vprob
            igx, if0 = igx[a], if0[a]

            # Discard grains that would touch the boundaries between inputs
            a = (igx > self.widthInSamples) & (igx < (iylen - self.widthInSamples - 1))
            if0, igx = if0[a], igx[a]

            f0.append(if0)
            gx.append(igx)
            ii.append(np.full(if0.shape, i))

        # Sorting, first by f0 and then by the final grain location (i, x)
        f0, gx, ii = map(np.concatenate, (f0, gx, ii))
        a = np.lexsort((gx, ii, f0))
        self.f0, self.gx, self.ii = f0[a], gx[a], ii[a]
        if not len(self.f0):
            raise ValueError("No grains selected")

    def _buildCompactIndex(self):
        rng = np.random.default_rng()

        # Allocate small frequency bins spaced evenly in the mel scale
        f0_range = (self.f0[0] - 0.1, self.f0[-1] + 0.1)
        max_bins = int(np.ceil(12 * f0_range[1] / f0_range[0] / self.args.res))
        mel_range = librosa.hz_to_mel(f0_range, htk=True)
        mel_bins = np.linspace(mel_range[0], mel_range[1], max_bins + 1)
        bins = librosa.mel_to_hz(mel_bins, htk=True)
        bin_x = np.searchsorted(self.f0, bins)

        cf0, cgx, cii, self.bf0, self.bx = [], [], [], [], []
        next_bx = 0
        for bin_id in range(len(bins) - 1):

            # Choose grains randomly if we have more than requested
            choices = np.arange(bin_x[bin_id], bin_x[bin_id + 1])
            rng.shuffle(choices)
            choices = choices[: self.args.max]
            choices.sort()

            # Skip empty or underfull bins
            if len(choices) < max(1, self.args.min):
                continue

            # Save chosen grains
            for c, o in ((cf0, self.f0), (cgx, self.gx), (cii, self.ii)):
                c.append(o[choices])

            # Update bin index
            self.bf0.append(cf0[-1].mean())
            self.bx.append(next_bx)
            next_bx += len(choices)

        if not len(cgx):
            raise ValueError("No grains left in compacted data")
        self.bx.append(next_bx)
        self.cgx, self.cii = map(np.concatenate, (cgx, cii))
        assert len(self.cgx) == len(self.cii)
        assert len(self.bf0) + 1 == len(self.bx)
        tqdm.tqdm.write(f"Using {len(self.cgx)} grains in {len(self.bf0)} bins")

    def _collectAudioData(self, file):
        # The compact index we have is sorted in f0 order.
        # Figure out a new ordering sorted by input file, and collect those grains
        yygx = np.full(self.cgx.shape, -1)
        ixsort = np.lexsort((self.cgx, self.cii))
        imemo = {}
        writerOffset = 0
        for grain in tqdm.tqdm(ixsort, unit="grain", unit_scale=True):
            i, x = self.cii[grain], self.cgx[grain]
            if i != imemo.get("i"):
                path = self.files[i]["path"]
                imemo = dict(i=i, grainEnd=0, reader=BufferedAudioReader(path))

            # Copy the portion that doesn't overlap what we already copied
            grainBegin = max(x - self.widthInSamples, imemo["grainEnd"])
            grainEnd = x + self.widthInSamples

            if grainBegin != imemo["grainEnd"] or 0 == imemo["grainEnd"]:
                # Mark discontinuities between non-adjacent grains, and across input edges
                file.write(self.discontinuityMarker)
                writerOffset += len(self.discontinuityMarker)

            # Updated locations in the grain index
            assert self.widthInSamples <= x and x < grainEnd
            yygx[grain] = writerOffset + x - grainBegin

            grainData = imemo["reader"].read(grainBegin, grainEnd - grainBegin)
            file.write(grainData)
            writerOffset += grainData.shape[0]
            imemo["grainEnd"] = grainEnd

        # We already marked the beginning of every input file, mark the end too
        file.write(self.discontinuityMarker)
        writerOffset += len(self.discontinuityMarker)

        return yygx, {
            "sound_len": writerOffset,
            "max_grain_width": self.args.width,
            "sample_rate": self.samplerate,
            "channels": self.channels,
            "bin_x": list(map(int, self.bx)),
            "bin_f0": list(map(float, self.bf0)),
        }

    def run(self):
        self._buildCombinedIndex()
        self._buildCompactIndex()
        with zipfile.ZipFile(self.filename, "x", zipfile.ZIP_STORED) as z:
            with tempfile.TemporaryFile(dir=os.path.dirname(self.filename)) as tmp:
                with soundfile.SoundFile(
                    tmp,
                    "w",
                    int(self.samplerate),
                    self.channels,
                    "PCM_16",
                    format="flac",
                ) as sound:
                    yygx, index = self._collectAudioData(sound)

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
                    with tqdm.tqdm(
                        total=tmpLen, unit="byte", unit_scale=True
                    ) as progress:
                        while True:
                            block = tmp.read(1024 * 1024)
                            if not block:
                                break
                            f.write(block)
                            progress.update(len(block))

        tqdm.tqdm.write(f"Completed {self.filename}")


def main():
    parser = argparse.ArgumentParser()
    Database.arguments(parser)
    subparsers = parser.add_subparsers(required=True)
    FileScanner.arguments(
        subparsers.add_parser(
            "scan",
            description="Run pitch detection on batches of audio files, updating the feature database",
        )
    )
    FileListing.arguments(
        subparsers.add_parser(
            "list",
            description="Show information about the files in the audio feature database",
        )
    )
    FilePacker.arguments(
        subparsers.add_parser(
            "pack",
            description="Compact a portion of the feature database into a self-contained archive",
        )
    )
    args = parser.parse_args()
    args.factory(args).run()


if __name__ == "__main__":
    main()

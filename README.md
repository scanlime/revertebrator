# Revertebrator

A weird granular synthesizer that uses large databases of pitch-sorted samples that stream from disk.

![Screenshot of the synthesizer window](screenshot.jpeg)

The instrument / plugin needs JUCE 6 to build.

The packed data files (`.rvv` extension) are actually ZIP archives containing FLAC audio data as well as index data for locating grains quickly. These packed files are built using `rvtool`, which needs Python 3.8 or later, either locally or via Docker.


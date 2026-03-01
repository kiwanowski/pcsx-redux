/***************************************************************************
 *   Copyright (C) 2024 PCSX-Redux authors                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

#include "flags.h"
#include "fmt/format.h"
#include "support/binstruct.h"
#include "support/file.h"
#include "support/typestring-wrapper.h"
#include "supportpsx/adpcm.h"

// ---------------------------------------------------------------------------
// MOD file structures
// ---------------------------------------------------------------------------

typedef PCSX::BinStruct::Field<PCSX::BinStruct::CString<20>, TYPESTRING("Title")> ModTitle;

typedef PCSX::BinStruct::Field<PCSX::BinStruct::CString<22>, TYPESTRING("Name")> SampleName;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::BEUInt16, TYPESTRING("Length")> SampleLength;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::UInt8, TYPESTRING("FineTune")> SampleFineTune;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::UInt8, TYPESTRING("Volume")> SampleVolume;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::BEUInt16, TYPESTRING("LoopStart")> SampleLoopStart;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::BEUInt16, TYPESTRING("LoopLength")> SampleLoopLength;

typedef PCSX::BinStruct::Struct<TYPESTRING("ModSample"), SampleName, SampleLength, SampleFineTune, SampleVolume,
                                SampleLoopStart, SampleLoopLength>
    ModSample;
typedef PCSX::BinStruct::RepeatedStruct<ModSample, TYPESTRING("ModSamples"), 31> ModSamples;

typedef PCSX::BinStruct::Field<PCSX::BinStruct::UInt8, TYPESTRING("Positions")> Positions;
typedef PCSX::BinStruct::Field<PCSX::BinStruct::UInt8, TYPESTRING("RestartPosition")> RestartPosition;
typedef PCSX::BinStruct::RepeatedField<PCSX::BinStruct::UInt8, TYPESTRING("PatternTable"), 128> PatternTable;

typedef PCSX::BinStruct::Field<PCSX::BinStruct::CString<4>, TYPESTRING("Signature")> Signature;

typedef PCSX::BinStruct::Struct<TYPESTRING("ModFile"), ModTitle, ModSamples, Positions, RestartPosition, PatternTable,
                                Signature>
    ModFile;

// ---------------------------------------------------------------------------
// S3M file structures (ScreamTracker 3, little-endian)
// All offsets follow the official S3M specification.
// Only PCM instruments with 16-bit signed samples are processed.
// ---------------------------------------------------------------------------

// S3M file header – first 96 bytes
#pragma pack(push, 1)
struct S3MHeader {
    char     songName[28];       // 0x00 – null-terminated song title
    uint8_t  sig1;               // 0x1C – always 0x1A
    uint8_t  type;               // 0x1D – file type (0x10 = S3M)
    uint16_t reserved1;          // 0x1E
    uint16_t orderCount;         // 0x20 – number of orders
    uint16_t instrumentCount;    // 0x22 – number of instruments
    uint16_t patternCount;       // 0x24 – number of patterns
    uint16_t flags;              // 0x26
    uint16_t trackerVersion;     // 0x28
    uint16_t sampleFormat;       // 0x2A – 1=signed, 2=unsigned
    char     magic[4];           // 0x2C – "SCRM"
    uint8_t  globalVolume;       // 0x30
    uint8_t  initialSpeed;       // 0x31
    uint8_t  initialTempo;       // 0x32
    uint8_t  masterVolume;       // 0x33
    uint8_t  ultraClickRemoval;  // 0x34
    uint8_t  defaultPan;         // 0x35 – 0xFC means use included pan table
    uint8_t  reserved2[8];       // 0x36
    uint16_t specialPtr;         // 0x3E – parapointer to special data
    uint8_t  channelSettings[32];// 0x40 – 0xFF=unused, 0x80=disabled, 0-15=PCM, 16-31=FM
};

// S3M PCM sample instrument header (type == 1)
struct S3MInstrument {
    uint8_t  type;               // 1 = PCM sample, 2-7 = AdLib
    char     filename[12];       // DOS 8.3 filename
    uint8_t  dataParaHigh;       // high byte of sample data parapointer (Amiga-style high seg)
    uint16_t dataParaLow;        // low word of sample data parapointer
    uint32_t length;             // length in samples
    uint32_t loopBegin;          // loop start in samples
    uint32_t loopEnd;            // loop end in samples
    uint8_t  defaultVolume;      // 0-64
    uint8_t  diskNumber;
    uint8_t  packing;            // must be 0 (uncompressed)
    uint8_t  flags;              // bit 0: loop, bit 2: stereo (not supported here), bit 3: 16-bit
    uint32_t c2spd;              // C-4 frequency in Hz
    uint8_t  reserved[12];
    char     sampleName[28];     // instrument name
    char     magic[4];           // "SCRS" for PCM samples
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint32_t s3mParaToOffset(uint8_t paraHigh, uint16_t paraLow) {
    // S3M parapointers: offset = (paraHigh * 65536 + paraLow) * 16
    return (static_cast<uint32_t>(paraHigh) * 65536 + paraLow) * 16;
}

// ---------------------------------------------------------------------------
// S3M sample encoder
// Converts 16-bit signed LE PCM samples to SPU ADPCM blocks and writes them
// into encodedSamples.  Returns encoded byte count, or 0 on error.
// loopBegin and loopEnd are in samples.
// ---------------------------------------------------------------------------
static unsigned encodeS3MSample(PCSX::IO<PCSX::File>& file, uint32_t dataOffset, uint32_t lengthSamples,
                                uint32_t loopBegin, uint32_t loopEnd, bool hasLoop, bool signedSamples,
                                unsigned amplification, PCSX::ADPCM::Encoder* encoder,
                                PCSX::IO<PCSX::File>& encodedSamples) {
    constexpr uint8_t silentLoopBlock[16] = {0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    file->seek(dataOffset, SEEK_SET);

    uint32_t remaining = lengthSamples;
    unsigned position = 0;
    unsigned encodedLength = 0;
    int16_t inBuf[28];
    uint8_t spuBlock[16];

    while (remaining > 0) {
        unsigned blockSize = std::min(remaining, 28u);

        // Read up to 28 samples
        for (unsigned j = 0; j < blockSize; j++) {
            int16_t raw = file->read<int16_t>();
            if (!signedSamples) {
                // Unsigned 16-bit: convert to signed by subtracting 32768
                raw = static_cast<int16_t>(static_cast<uint16_t>(raw) - 32768u);
            }
            // Apply amplification (clamp to int16 range)
            int32_t amplified = static_cast<int32_t>(raw) * static_cast<int32_t>(amplification) / 175;
            inBuf[j] = static_cast<int16_t>(std::clamp(amplified, -32768, 32767));
        }
        // Zero-pad the rest of the block
        for (unsigned j = blockSize; j < 28; j++) {
            inBuf[j] = 0;
        }

        remaining -= blockSize;

        encoder->processSPUBlock(inBuf, spuBlock, PCSX::ADPCM::Encoder::BlockAttribute::OneShot);

        uint8_t blockAttribute = 0;
        bool isLast = (remaining == 0);
        if (isLast) {
            blockAttribute |= 1;  // end flag
        }
        if (hasLoop && (static_cast<uint32_t>(position) >= loopBegin) &&
            (static_cast<uint32_t>(position) < loopEnd)) {
            blockAttribute |= 2;  // loop body
            if (static_cast<uint32_t>(position) < loopBegin + 28) {
                blockAttribute |= 4;  // loop start
            }
        }
        spuBlock[1] = blockAttribute;
        position += blockSize;

        encodedSamples->write(spuBlock, 16);
        encodedLength += 16;
    }

    if (!hasLoop) {
        encodedSamples->write(silentLoopBlock, 16);
        encodedLength += 16;
    }

    return encodedLength;
}

// ---------------------------------------------------------------------------
// S3M conversion entry point
// ---------------------------------------------------------------------------
static int convertS3M(const std::string& inputPath, const std::string& outputPath,
                      const std::optional<std::string>& samplesFile, unsigned amplification) {
    PCSX::IO<PCSX::File> file(new PCSX::PosixFile(inputPath));
    if (file->failed()) {
        fmt::print("Unable to open file: {}\n", inputPath);
        return -1;
    }

    // Read main header
    S3MHeader hdr;
    file->read(&hdr, sizeof(hdr));

    // Validate magic
    if (std::memcmp(hdr.magic, "SCRM", 4) != 0) {
        fmt::print("{} does not have a valid S3M file signature.\n", inputPath);
        return -1;
    }
    if (hdr.type != 0x10) {
        fmt::print("{} is not an S3M file (type byte = 0x{:02X}).\n", inputPath, hdr.type);
        return -1;
    }

    bool signedSamples = (hdr.sampleFormat == 1);

    // Determine active PCM channel count (channels 0-15 in channelSettings are PCM)
    unsigned channels = 0;
    for (unsigned i = 0; i < 32; i++) {
        uint8_t cs = hdr.channelSettings[i];
        if (cs != 0xFF && cs != 0x80 && (cs & 0x7F) < 16) {
            channels++;
        }
    }

    if (channels == 0) {
        fmt::print("{} has no active PCM channels.\n", inputPath);
        return -1;
    }
    if (channels > 24) {
        fmt::print("{} has too many channels ({}). The maximum is 24.\n", inputPath, channels);
        return -1;
    }

    if (hdr.instrumentCount > 31) {
        fmt::print("Warning: S3M has {} instruments; only the first 31 will be used.\n", hdr.instrumentCount);
    }
    unsigned numInstruments = std::min(static_cast<unsigned>(hdr.instrumentCount), 31u);

    fmt::print("Title:       {:.28s}\n", hdr.songName);
    fmt::print("Channels:    {}\n", channels);
    fmt::print("Orders:      {}\n", hdr.orderCount);
    fmt::print("Instruments: {}\n", hdr.instrumentCount);
    fmt::print("Patterns:    {}\n", hdr.patternCount);
    fmt::print("Sample fmt:  {}\n", signedSamples ? "signed" : "unsigned");

    // Read order list
    std::vector<uint8_t> orders(hdr.orderCount);
    file->read(orders.data(), hdr.orderCount);

    // Read instrument parapointers
    std::vector<uint16_t> instPtrs(hdr.instrumentCount);
    file->read(instPtrs.data(), hdr.instrumentCount * 2);

    // Read pattern parapointers
    std::vector<uint16_t> patPtrs(hdr.patternCount);
    file->read(patPtrs.data(), hdr.patternCount * 2);

    // Optional default-pan table
    if (hdr.defaultPan == 0xFC) {
        file->skip(32);
    }

    // ---------------------------------------------------------------------------
    // Read instrument headers
    // ---------------------------------------------------------------------------
    std::vector<S3MInstrument> instruments(hdr.instrumentCount);
    for (unsigned i = 0; i < hdr.instrumentCount; i++) {
        uint32_t offset = static_cast<uint32_t>(instPtrs[i]) * 16;
        file->seek(offset, SEEK_SET);
        file->read(&instruments[i], sizeof(S3MInstrument));
    }

    // ---------------------------------------------------------------------------
    // Encode samples
    // ---------------------------------------------------------------------------
    PCSX::IO<PCSX::File> encodedSamples =
        samplesFile.has_value()
            ? reinterpret_cast<PCSX::File*> (
                  new PCSX::PosixFile(samplesFile.value().c_str(), PCSX::FileOps::TRUNCATE))
            : reinterpret_cast<PCSX::File*>(new PCSX::BufferFile(PCSX::FileOps::READWRITE));

    std::unique_ptr<PCSX::ADPCM::Encoder> encoder(new PCSX::ADPCM::Encoder);

    // We will repurpose the MOD file structure for the HIT header output.
    // Map S3M instruments → MOD sample headers (first 31 slots).
    ModFile modFile;

    // Copy title (up to 20 chars)
    std::strncpy(modFile.get<ModTitle>().value, hdr.songName, 20);
    modFile.get<ModTitle>().value[19] = '\0';

    modFile.get<Positions>().value = static_cast<uint8_t>(std::min(static_cast<unsigned>(hdr.orderCount), 128u));
    modFile.get<RestartPosition>().value = 0;

    // Build pattern table from orders (skip 0xFE=end-of-song, 0xFF=skip markers)
    unsigned pos = 0;
    for (unsigned i = 0; i < hdr.orderCount && pos < 128; i++) {
        if (orders[i] < 0xFE) {
            modFile.get<PatternTable>()[pos++] = orders[i];
        }
    }
    for (unsigned i = pos; i < 128; i++) {
        modFile.get<PatternTable>()[i] = 0;
    }
    modFile.get<Positions>().value = static_cast<uint8_t>(pos);

    // Update the HIT signature
    if (channels >= 10) {
        modFile.get<Signature>().value[0] = 'H';
        modFile.get<Signature>().value[1] = 'M';
        modFile.get<Signature>().value[2] = (channels / 10) + '0';
        modFile.get<Signature>().value[3] = (channels % 10) + '0';
    } else {
        modFile.get<Signature>().value[0] = 'H';
        modFile.get<Signature>().value[1] = 'I';
        modFile.get<Signature>().value[2] = 'T';
        modFile.get<Signature>().value[3] = channels + '0';
    }

    fmt::print("Converting samples...\n");
    unsigned fullLength = 0;

    for (unsigned i = 0; i < 31; i++) {
        encoder->reset();
        auto& modSample = modFile.get<ModSamples>()[i];
        // Clear MOD sample defaults
        modSample.get<SampleName>().value[0] = '\0';
        modSample.get<SampleLength>().value = 0;
        modSample.get<SampleFineTune>().value = 0;
        modSample.get<SampleVolume>().value = 0;
        modSample.get<SampleLoopStart>().value = 0;
        modSample.get<SampleLoopLength>().value = 1;

        if (i >= numInstruments) {
            continue;
        }

        const auto& inst = instruments[i];

        fmt::print("Sample {:2} [{:22.22s}] - ", i + 1, inst.sampleName);

        // Skip non-PCM (AdLib) and invalid instruments
        if (inst.type != 1) {
            fmt::print("AdLib/unsupported (skipped)\n");
            continue;
        }
        if (std::memcmp(inst.magic, "SCRS", 4) != 0) {
            fmt::print("Not a PCM sample (skipped)\n");
            continue;
        }
        if (!(inst.flags & 0x08)) {
            fmt::print("Not 16-bit (skipped)\n");
            continue;
        }
        if (inst.length == 0) {
            fmt::print("Empty\n");
            continue;
        }

        bool hasLoop = (inst.flags & 0x01) != 0;
        uint32_t loopBegin = inst.loopBegin;
        uint32_t loopEnd = inst.loopEnd;
        if (hasLoop && (loopEnd <= loopBegin || loopEnd > inst.length)) {
            // Sanity check: disable invalid loop
            hasLoop = false;
        }

        // Compute data file offset from parapointer
        uint32_t dataOffset = s3mParaToOffset(inst.dataParaHigh, inst.dataParaLow);

        unsigned encodedLength =
            encodeS3MSample(file, dataOffset, inst.length, loopBegin, loopEnd, hasLoop,
                            signedSamples, amplification, encoder.get(), encodedSamples);

        if (encodedLength == 0) {
            fmt::print("Error encoding sample.\n");
            return -1;
        }
        if (encodedLength >= 65536) {
            fmt::print("Sample too big ({} bytes encoded).\n", encodedLength);
            return -1;
        }

        fmt::print("Size {} -> {}\n", inst.length * 2, encodedLength);

        // Copy name into MOD sample header (22 chars)
        std::strncpy(modSample.get<SampleName>().value, inst.sampleName, 22);
        modSample.get<SampleName>().value[21] = '\0';
        modSample.get<SampleLength>().value = static_cast<uint16_t>(encodedLength);
        modSample.get<SampleVolume>().value = std::min(static_cast<unsigned>(inst.defaultVolume), 64u);
        modSample.get<SampleLoopStart>().value = hasLoop ? static_cast<uint16_t>(loopBegin / 2) : 0;
        modSample.get<SampleLoopLength>().value =
            hasLoop ? static_cast<uint16_t>((loopEnd - loopBegin) / 2) : 1;

        fullLength += encodedLength;
    }

    // ---------------------------------------------------------------------------
    // Read and convert pattern data
    // S3M patterns are packed; we convert them to the MOD 4-byte-per-note format.
    // Pattern size in MOD: channels * 64 rows * 4 bytes = channels * 256 bytes.
    // ---------------------------------------------------------------------------
    unsigned maxPatternID = 0;
    for (unsigned i = 0; i < 128; i++) {
        maxPatternID = std::max(maxPatternID, static_cast<unsigned>(modFile.get<PatternTable>()[i]));
    }
    unsigned numPatterns = maxPatternID + 1;

    // Build a channel map: S3M logical channel index → output channel slot (0-based)
    // S3M channel setting values 0–15 are left PCM channels 0–7, right PCM channels 8–15
    uint8_t channelMap[32];
    std::memset(channelMap, 0xFF, sizeof(channelMap));
    unsigned slot = 0;
    for (unsigned i = 0; i < 32 && slot < channels; i++) {
        uint8_t cs = hdr.channelSettings[i];
        if (cs != 0xFF && cs != 0x80 && (cs & 0x7F) < 16) {
            channelMap[i] = static_cast<uint8_t>(slot++);
        }
    }

    // Output pattern buffer: numPatterns * channels * 64 rows * 4 bytes
    std::vector<uint8_t> patternData(numPatterns * channels * 64 * 4, 0);

    for (unsigned p = 0; p < numPatterns; p++) {
        if (p >= hdr.patternCount || patPtrs[p] == 0) {
            continue;  // empty/missing pattern → stays zeroed
        }
        uint32_t patOffset = static_cast<uint32_t>(patPtrs[p]) * 16;
        file->seek(patOffset, SEEK_SET);

        // First 2 bytes = packed data length (not counting these 2 bytes)
        uint16_t packedLen = 0;
        file->read(&packedLen, 2);

        // Base pointer in output for this pattern
        uint8_t* patBase = patternData.data() + p * channels * 64 * 4;

        for (unsigned row = 0; row < 64; row++) {
            for (;;) {
                uint8_t what = file->read<uint8_t>();
                if (what == 0) break;  // end of row

                unsigned chanIdx = what & 0x1F;

                uint8_t note = 0, instrument = 0, volume = 0xFF, command = 0, cmdParam = 0;

                if (what & 0x20) {  // note and instrument follow
                    note = file->read<uint8_t>();
                    instrument = file->read<uint8_t>();
                }
                if (what & 0x40) {  // volume follows
                    volume = file->read<uint8_t>();
                }
                if (what & 0x80) {  // effect + parameter follow
                    command = file->read<uint8_t>();
                    cmdParam = file->read<uint8_t>();
                }

                if (chanIdx >= 32 || channelMap[chanIdx] == 0xFF) {
                    continue;  // channel not active in output
                }

                unsigned outChan = channelMap[chanIdx];
                uint8_t* cell = patBase + (row * channels + outChan) * 4;

                // Convert S3M note to MOD period
                // S3M note byte: high nibble = octave (0-7), low nibble = semitone (0-11)
                // MOD periods: standard Protracker table
                // We store as instrument + sample period bytes matching MOD layout
                static const uint16_t kModPeriods[12] = {
                    // C    C#   D    D#   E    F    F#   G    G#   A    A#   B
                    1712, 1616, 1524, 1440, 1356, 1280, 1208, 1140, 1076, 1016, 960, 907
                };

                if (note != 0 && note != 0xFE && note != 0xFF) {
                    // S3M note: low nibble = semitone, high nibble = octave
                    unsigned semitone = note & 0x0F;
                    unsigned octave = (note >> 4) & 0x0F;
                    if (semitone < 12) {
                        // MOD periods are for octave 0 (the table above).
                        // Each octave doubles/halves frequency → halves/doubles period.
                        // S3M octave 4 ≈ MOD octave 0 for C4 = 1712 at base
                        // Standard MOD: C-3 = octave 1 in display but period 856; the table
                        // above is octave 0 (C-0 = 1712). S3M octave 3 maps to C-3 (period 428).
                        // S3M octave 0 → shift = 3 down from MOD octave 0 (divide by 8)
                        // We use: period = kModPeriods[semitone] >> octave
                        uint16_t period = static_cast<uint16_t>(kModPeriods[semitone] >> octave);
                        cell[0] = (instrument & 0xF0) | ((period >> 8) & 0x0F);
                        cell[1] = period & 0xFF;
                    }
                } else if (note == 0xFE) {
                    // Note cut – encode as a 'E' effect (cut) if we have room;
                    // for simplicity just leave the period as 0 (silence)
                    cell[0] = 0;
                    cell[1] = 0;
                }

                // Instrument number (1-based)
                cell[2] = ((instrument & 0x0F) << 4);

                // Volume → MOD volume effect (Cxx)
                if (volume != 0xFF && volume <= 64) {
                    // MOD effect C = set volume; parameter = 0-64
                    cell[2] |= 0x0C;  // effect C
                    cell[3] = static_cast<uint8_t>(volume);
                }
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Check SPU memory
    // ---------------------------------------------------------------------------
    constexpr unsigned spuMemory = 512 * 1024 - 0x1010;
    if (fullLength >= spuMemory) {
        fmt::print("Not enough SPU memory to store all samples; {} bytes required but only {} available.\n",
                   fullLength, spuMemory);
        return -1;
    } else {
        fmt::print("Used {} bytes of SPU memory, {} still available.\n", fullLength, spuMemory - fullLength);
    }

    // ---------------------------------------------------------------------------
    // Write output
    // ---------------------------------------------------------------------------
    PCSX::IO<PCSX::File> out(new PCSX::PosixFile(outputPath.c_str(), PCSX::FileOps::TRUNCATE));
    modFile.serialize(out);
    out->write(patternData.data(), patternData.size());
    if (!samplesFile.has_value()) {
        out->write(std::move(encodedSamples.asA<PCSX::BufferFile>()->borrow()));
    }

    out->close();
    encodedSamples->close();
    if (samplesFile.has_value()) {
        fmt::print("All done, files {} and {} written out.\n", outputPath, samplesFile.value());
    } else {
        fmt::print("All done, file {} written out.\n", outputPath);
    }

    return 0;
}

// ---------------------------------------------------------------------------
// MOD conversion (original logic, unchanged in behavior)
// ---------------------------------------------------------------------------
static int convertMOD(const std::string& inputPath, const std::string& outputPath,
                      const std::optional<std::string>& samplesFile, unsigned amplification) {
    PCSX::IO<PCSX::File> file(new PCSX::PosixFile(inputPath));
    if (file->failed()) {
        fmt::print("Unable to open file: {}\n", inputPath);
        return -1;
    }

    ModFile modFile;
    modFile.deserialize(file);

    std::string_view signature(modFile.get<Signature>().value, 4);

    unsigned channels = 0;
    if (signature == "M.K." || signature == "M!K!") {
        channels = 4;
    } else if (std::isdigit(signature[0]) && (signature[1] == 'C') && (signature[2] == 'H') &&
               (signature[3] == 'N')) {
        channels = signature[0] - '0';
    } else if (std::isdigit(signature[0]) && std::isdigit(signature[1]) && (signature[2] == 'C') &&
               (signature[3] == 'H')) {
        channels = (signature[0] - '0') * 10 + signature[1] - '0';
    }

    if (channels == 0) {
        fmt::print("{} doesn't have a recognized MOD file format.\n", inputPath);
        return -1;
    }

    if (channels > 24) {
        fmt::print("{} has too many channels ({}). The maximum is 24.\n", inputPath, channels);
        return -1;
    }

    unsigned maxPatternID = 0;
    for (unsigned i = 0; i < 128; i++) {
        maxPatternID = std::max(maxPatternID, unsigned(modFile.get<PatternTable>()[i]));
    }

    auto patternData = file->read(channels * (maxPatternID + 1) * 256);

    fmt::print("Title:     {}\n", modFile.get<ModTitle>().value);
    fmt::print("Channels:  {}\n", channels);
    fmt::print("Positions: {}\n", modFile.get<Positions>().value);
    fmt::print("Patterns:  {}\n", maxPatternID + 1);
    fmt::print("Converting samples...\n");

    PCSX::IO<PCSX::File> encodedSamples =
        samplesFile.has_value()
            ? reinterpret_cast<PCSX::File*>(
                  new PCSX::PosixFile(samplesFile.value().c_str(), PCSX::FileOps::TRUNCATE))
            : reinterpret_cast<PCSX::File*>(new PCSX::BufferFile(PCSX::FileOps::READWRITE));

    constexpr uint8_t silentLoopBlock[16] = {0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    std::unique_ptr<PCSX::ADPCM::Encoder> encoder(new PCSX::ADPCM::Encoder);
    for (unsigned i = 0; i < 31; i++) {
        encoder->reset();
        auto& sample = modFile.get<ModSamples>()[i];
        fmt::print("Sample {:2} [{:22}] - ", i + 1, sample.get<SampleName>().value);
        auto length = sample.get<SampleLength>().value;
        auto loopStart = sample.get<SampleLoopStart>().value;
        auto loopLength = sample.get<SampleLoopLength>().value;
        bool hasLoop = (loopStart > 0) && (loopLength > 1);
        if (length == 0) {
            fmt::print("Empty\n");
            continue;
        }
        int16_t input[28];
        uint8_t spuBlock[16];
        file->skip<uint16_t>();
        length--;
        length *= 2;
        unsigned position = 2;
        loopStart *= 2;
        unsigned loopEnd = loopStart + loopLength * 2;
        unsigned encodedLength = 0;
        while (length >= 28) {
            for (unsigned j = 0; j < 28; j++) {
                input[j] = int16_t(file->read<int8_t>()) * amplification;
            }
            length -= 28;
            encoder->processSPUBlock(input, spuBlock, PCSX::ADPCM::Encoder::BlockAttribute::OneShot);
            uint8_t blockAttribute = 0;
            if (length == 0) {
                blockAttribute |= 1;
            }
            if (hasLoop && (loopStart <= position)) {
                blockAttribute |= 2;
                if (position < (loopStart + 28)) {
                    blockAttribute |= 4;
                }
            }
            spuBlock[1] = blockAttribute;
            position += 28;
            encodedSamples->write(spuBlock, 16);
            encodedLength += 16;
        }
        if (length != 0) {
            for (unsigned j = 0; j < length; j++) {
                input[j] = int16_t(file->read<int8_t>()) * amplification;
            }
            for (unsigned j = length; j < 28; j++) {
                input[j] = 0;
            }
            encoder->processSPUBlock(input, spuBlock, PCSX::ADPCM::Encoder::BlockAttribute::OneShot);
            uint8_t blockAttribute = 0;
            if (hasLoop) {
                blockAttribute = 3;
                if (position < (loopStart + 28)) {
                    blockAttribute |= 4;
                }
            }
            spuBlock[1] = blockAttribute;
            position += 28;
            encodedSamples->write(spuBlock, 16);
            encodedLength += 16;
        }
        if (!hasLoop) {
            encodedSamples->write(silentLoopBlock, 16);
            encodedLength += 16;
        }
        fmt::print("Size {} -> {}\n", sample.get<SampleLength>().value * 2 - 2, encodedLength);
        sample.get<SampleLength>().value = encodedLength;
        if (encodedLength >= 65536) {
            fmt::print("Sample too big.\n");
            return -1;
        }
    }

    if (channels >= 10) {
        modFile.get<Signature>().value[0] = 'H';
        modFile.get<Signature>().value[1] = 'M';
        modFile.get<Signature>().value[2] = (channels / 10) + '0';
        modFile.get<Signature>().value[3] = (channels % 10) + '0';
    } else {
        modFile.get<Signature>().value[0] = 'H';
        modFile.get<Signature>().value[1] = 'I';
        modFile.get<Signature>().value[2] = 'T';
        modFile.get<Signature>().value[3] = channels + '0';
    }

    unsigned fullLength = 0;
    for (unsigned i = 0; i < 31; i++) {
        auto& sample = modFile.get<ModSamples>()[i];
        fullLength += sample.get<SampleLength>().value;
    }

    constexpr unsigned spuMemory = 512 * 1024 - 0x1010;

    if (fullLength >= spuMemory) {
        fmt::print("Not enough SPU memory to store all samples; {} bytes required but only {} available.\n",
                   fullLength, spuMemory);
        return -1;
    } else {
        fmt::print("Used {} bytes of SPU memory, {} still available.\n", fullLength, spuMemory - fullLength);
    }

    PCSX::IO<PCSX::File> out(new PCSX::PosixFile(outputPath.c_str(), PCSX::FileOps::TRUNCATE));
    modFile.serialize(out);
    out->write(std::move(patternData));
    if (!samplesFile.has_value()) {
        out->write(std::move(encodedSamples.asA<PCSX::BufferFile>()->borrow()));
    }

    out->close();
    encodedSamples->close();
    if (samplesFile.has_value()) {
        fmt::print("All done, files {} and {} written out.\n", outputPath, samplesFile.value());
    } else {
        fmt::print("All done, file {} written out.\n", outputPath);
    }

    return 0;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    CommandLine::args args(argc, argv);
    const auto output = args.get<std::string>("o");

    fmt::print(R"(
modconv by Nicolas "Pixel" Noble
https://github.com/grumpycoders/pcsx-redux/tree/main/tools/modconv/

)");

    const auto inputs = args.positional();
    const bool asksForHelp = args.get<bool>("h").value_or(false);
    const bool hasOutput = output.has_value();
    const bool oneInput = inputs.size() == 1;
    const auto samplesFile = args.get<std::string>("s");
    const auto amplification = args.get<unsigned>("a").value_or(175);
    if (asksForHelp || !oneInput || !hasOutput) {
        fmt::print(R"(
Usage: {} input.[mod|s3m] [-h] [-s output.smp] [-a amp] -o output.hit
  input.mod/.s3m    mandatory: specify the input mod or s3m file
  -o output.hit     mandatory: name of the output hit file.
  -h                displays this help information and exit.
  -s output.smp     optional: name of the output sample file.
  -a amplification  optional: value of sample amplification. Defaults to 175.

Supported input formats:
  MOD  – 4/8/... channel ProTracker/FastTracker module (8-bit samples)
  S3M  – ScreamTracker 3 module (16-bit signed samples only)

If the -s option is specified, the .hit file will only contain the pattern data,
and the .smp file will contain the sample data which can be loaded into the SPU
memory separately. If the -s option is not specified, the .hit file will contain
both the pattern and sample data.
)" , argv[0]);
        return -1;
    }

    const auto& input = inputs[0];

    // Detect format by file extension (case-insensitive) or by peeking at magic
    std::string lowerInput = input;
    std::transform(lowerInput.begin(), lowerInput.end(), lowerInput.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    bool isS3M = false;
    if (lowerInput.size() >= 4 && lowerInput.substr(lowerInput.size() - 4) == ".s3m") {
        isS3M = true;
    } else {
        // Peek at offset 0x2C for "SCRM" magic to auto-detect
        PCSX::IO<PCSX::File> probe(new PCSX::PosixFile(input));
        if (!probe->failed()) {
            char magic[4] = {};
            probe->seek(0x2C, SEEK_SET);
            probe->read(magic, 4);
            if (std::memcmp(magic, "SCRM", 4) == 0) {
                isS3M = true;
            }
        }
    }

    if (isS3M) {
        fmt::print("Detected S3M format.\n");
        return convertS3M(input, output.value(), samplesFile, amplification);
    } else {
        fmt::print("Detected MOD format.\n");
        return convertMOD(input, output.value(), samplesFile, amplification);
    }
}
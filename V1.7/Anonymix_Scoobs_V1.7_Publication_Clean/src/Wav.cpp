// WAV file naming and writing. File size fields must agree with the supplied PCM16 byte count and channel count.

#include "Wav.h"
#include <cerrno>
#include <limits>

// Create recordings/ beside the executable and write a PCM16 WAV file. The caller must supply
// data matching numChannels. Extract a portable distribution into a writable folder.
bool SaveWAV_Adaptive(const string& filename, const vector<Uint8>& audioData, int numChannels, float sampleRate)
{
    // Reject an empty recording before creating a file or computing duration.
    if (audioData.empty()) {
        return false;
    }
    static_assert(sizeof(WAVHeader) == 44, "PCM WAV requires a 44-byte header");
    if (numChannels <= 0 || audioData.size() % (2u * numChannels) != 0 ||
        audioData.size() > static_cast<size_t>(numeric_limits<uint32_t>::max()) -
            (sizeof(WAVHeader) - 8)) {
        return false;
    }


    // SDL_GetBasePath ends with a directory separator. A path beside the executable keeps WAV
    // output inside the extracted portable distribution regardless of its working directory.
    const char* basePath = SDL_GetBasePath();
    if (!basePath) {
        return false;
    }

    string folderPath = string(basePath) + "recordings";

#ifdef _WIN32
    int result = _mkdir(folderPath.c_str());
#else
    int result = mkdir(folderPath.c_str(), 0755);
#endif

    if (result != 0 && errno != EEXIST) {
        return false;
    }

    string fullPath = folderPath + "/" + filename;

    // Open in binary mode so the RIFF header and raw PCM bytes are not altered by text newline
    // conversion.
    FILE* file = nullptr;
    errno_t err = fopen_s(&file, fullPath.c_str(), "wb");
    if (err != 0 || !file) {
        return false;
    }

    // Write a fixed PCM16 RIFF header followed by the exact supplied byte vector.
    WAVHeader header;

    memcpy(header.riffID, "RIFF", 4);
    // RIFF fileSize omits the first eight bytes. Check size overflow for recordings beyond 4 GiB.
    header.fileSize = static_cast<uint32_t>(audioData.size() + sizeof(WAVHeader) - 8);
    memcpy(header.riffType, "WAVE", 4);

    memcpy(header.fmtID, "fmt ", 4);
    header.fmtSize = 16;
    header.audioFormat = 1;
    header.numChannels = numChannels;
    header.sampleRate = static_cast<uint32_t>(sampleRate);
    header.bitsPerSample = 16;
    // The header advertises interleaved frames; caller data must actually contain numChannels
    // samples per frame.
    header.byteRate = header.sampleRate * header.numChannels * header.bitsPerSample / 8;
    header.blockAlign = header.numChannels * header.bitsPerSample / 8;

    memcpy(header.dataID, "data", 4);
    header.dataSize = static_cast<uint32_t>(audioData.size());

    // Verify both writes independently and close the file on a partial write.
    size_t written = fwrite(&header, 1, sizeof(WAVHeader), file);
    if (written != sizeof(WAVHeader)) {
        fclose(file);
        return false;
    }

    written = fwrite(audioData.data(), 1, audioData.size(), file);
    if (written != audioData.size()) {
        fclose(file);
        return false;
    }

    fclose(file);

    return true;
}

// Append local date and time to a prefix; two saves with the same prefix within one second can
// collide.
string GenerateWAVFilename(const string& prefix)
{
    time_t now = time(nullptr);

    tm timeinfo;
    // Use local time for filenames. A repeated prefix in the same second targets the same name.
    errno_t err = localtime_s(&timeinfo, &now);
    if (err != 0) {
        return prefix + "_error.wav";
    }

    char buffer[64];
    strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &timeinfo);

    return prefix + "_" + string(buffer) + ".wav";
}

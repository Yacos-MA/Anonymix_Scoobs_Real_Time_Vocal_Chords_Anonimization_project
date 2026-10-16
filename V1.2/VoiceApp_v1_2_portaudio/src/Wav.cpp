// WAV file naming and writing. File size fields must agree with the supplied PCM16 byte count and channel count.

#include "Wav.h"
#include <cerrno>

// Create recordings/ beside the executable and write a PCM16 WAV file. The caller must supply
// data matching numChannels. Extract a portable distribution into a writable folder.
bool SaveWAV_Adaptive(const string& filename, const vector<Uint8>& audioData, int numChannels, float sampleRate)
{
    // Reject an empty recording before creating a file or computing duration.
    if (audioData.empty()) {
        SDL_Log("ERROR SaveWAV_Adaptive: empty audio buffer");
        return false;
    }

    SDL_Log("=== START SaveWAV_Adaptive ===");
    SDL_Log("  File: %s", filename.c_str());
    SDL_Log("  Buffer size: %zu bytes", audioData.size());
    SDL_Log("  Format: %s (channels: %d)", numChannels == 2 ? "STEREO" : "MONO", numChannels);
    SDL_Log("  Sample rate: %.0f Hz", sampleRate);

    // SDL_GetBasePath ends with a directory separator. A path beside the executable keeps WAV
    // output inside the extracted portable distribution regardless of its working directory.
    const char* basePath = SDL_GetBasePath();
    if (!basePath) {
        SDL_Log("ERROR: Cannot obtain SDL_GetBasePath()");
        return false;
    }

    string folderPath = string(basePath) + "recordings";

#ifdef _WIN32
    int result = _mkdir(folderPath.c_str());
#else
    int result = mkdir(folderPath.c_str(), 0755);
#endif

    if (result == 0) {
        SDL_Log("  'recordings' directory created: %s", folderPath.c_str());
    }
    else if (errno == EEXIST) {
        SDL_Log("  'recordings' directory already exists: %s", folderPath.c_str());
    }
    else {
        SDL_Log("ERROR: Cannot create recordings directory %s (error code: %d)", folderPath.c_str(), errno);
        return false;
    }

    string fullPath = folderPath + "/" + filename;

    // Open in binary mode so the RIFF header and raw PCM bytes are not altered by text newline
    // conversion.
    FILE* file = nullptr;
    errno_t err = fopen_s(&file, fullPath.c_str(), "wb");
    if (err != 0 || !file) {
        SDL_Log("ERROR: Cannot create file %s (error code: %d)", fullPath.c_str(), err);
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
        SDL_Log("ERROR: Failed to write WAV header");
        fclose(file);
        return false;
    }

    written = fwrite(audioData.data(), 1, audioData.size(), file);
    if (written != audioData.size()) {
        SDL_Log("ERROR: Failed to write audio data");
        fclose(file);
        return false;
    }

    fclose(file);

    size_t totalFileSize = sizeof(WAVHeader) + audioData.size();
    size_t totalSamples = audioData.size() / sizeof(int16_t);
    // Duration uses declared channel count; a mono buffer labeled stereo would report half the
    // real duration.
    float durationSeconds = static_cast<float>(totalSamples / numChannels) / sampleRate;

    SDL_Log(">>> WAV FILE SAVED SUCCESSFULLY <<<");
    SDL_Log("  Path: %s", fullPath.c_str());
    SDL_Log("  Total size: %zu bytes (%.2f KB)", totalFileSize, totalFileSize / 1024.0f);
    SDL_Log("  Total samples: %zu (%zu per channel)", totalSamples, totalSamples / numChannels);
    SDL_Log("  Duration: %.2f seconds", durationSeconds);
    SDL_Log("  Format: PCM 16-bit %s @ %.0f Hz",
        numChannels == 2 ? "STEREO" : "MONO", sampleRate);
    SDL_Log("=== END SaveWAV_Adaptive ===");

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
        SDL_Log("ERROR: localtime_s failed");
        return prefix + "_error.wav";
    }

    char buffer[64];
    strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &timeinfo);

    return prefix + "_" + string(buffer) + ".wav";
}

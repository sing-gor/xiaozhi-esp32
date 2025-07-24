#ifndef AUDIO_RECORDER_H
#define AUDIO_RECORDER_H

#include <string>
#include <cstdint>
#include <cstdio>

// 我们不再需要包含 audio_codec.h
// #include "audio_codec.h" 

class AudioRecorder {
public:
    // 构造函数不再需要 AudioCodec*
    AudioRecorder(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t num_channels);
    ~AudioRecorder();

    bool StartRecording(const std::string& filename);
    void StopRecording();
    bool IsRecording() const;

    // 新增：这是核心修改，允许外部代码向录音器写入数据
    void WriteAudioData(const uint8_t* data, size_t size);

private:
    // WAV文件头结构体
    struct WavHeader {
        char riff[4] = {'R', 'I', 'F', 'F'};
        uint32_t file_size;
        char wave[4] = {'W', 'A', 'V', 'E'};
        char fmt[4] = {'f', 'm', 't', ' '};
        uint32_t fmt_chunk_size = 16;
        uint16_t audio_format = 1; // 1 for PCM
        uint16_t num_channels;
        uint32_t sample_rate;
        uint32_t byte_rate;
        uint16_t block_align;
        uint16_t bits_per_sample;
        char data_chunk_header[4] = {'d', 'a', 't', 'a'};
        uint32_t data_chunk_size;
    };

    void WriteWavHeader();
    void UpdateWavHeader();

    // 移除了 AudioCodec* codec_;
    FILE* recording_file_ = nullptr;
    bool is_recording_ = false;
    WavHeader header_;
    uint32_t recorded_data_size_ = 0;
};

#endif // AUDIO_RECORDER_H
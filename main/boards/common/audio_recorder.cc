#include "audio_recorder.h"
#include "esp_log.h"
#include <inttypes.h>
#include <cerrno>   // 添加cerrno头文件以使用 errno
#include <cstring>  // 添加cstring头文件以使用 strerror

#define TAG "AudioRecorder"

// 构造函数不再接收 AudioCodec*
AudioRecorder::AudioRecorder(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t num_channels) {
    // 根据参数配置WAV头    
    header_.sample_rate = sample_rate;
    header_.bits_per_sample = bits_per_sample;
    header_.num_channels = num_channels;
    header_.byte_rate = sample_rate * num_channels * (bits_per_sample / 8);
    header_.block_align = num_channels * (bits_per_sample / 8);
}

AudioRecorder::~AudioRecorder() {
    StopRecording();
}

bool AudioRecorder::StartRecording(const std::string& filename) {
    if (is_recording_) {
        ESP_LOGE(TAG, "Recording already in progress");
        return false;
    }

    // 文件路径前需要加上挂载点，例如 "/sdcard/"
    recording_file_ = fopen(filename.c_str(), "wb");
    if (!recording_file_) {
        ESP_LOGE(TAG, "Failed to open file for recording: %s", filename.c_str());
        // 关键修改：打印出详细的错误码和错误信息
        ESP_LOGE(TAG, "fopen failed with errno %d: %s", errno, strerror(errno));
        return false;
    }

    // 先写入一个占位的WAV头
    WriteWavHeader();

    is_recording_ = true;
    recorded_data_size_ = 0;
    ESP_LOGI(TAG, "Recording started, file: %s", filename.c_str());
    return true;
}

// 新增的核心方法实现
void AudioRecorder::WriteAudioData(const uint8_t* data, size_t size) {
    if (is_recording_ && recording_file_ && data != nullptr && size > 0) {
        size_t written = fwrite(data, 1, size, recording_file_);
        if (written > 0) {
            recorded_data_size_ += written;
        }
    }
}

void AudioRecorder::StopRecording() {
    if (!is_recording_) return;

    is_recording_ = false;
    
    if (recording_file_) {
        ESP_LOGI(TAG, "Recording stopped, total data size: %" PRIu32 " bytes", recorded_data_size_);
        UpdateWavHeader();
        fclose(recording_file_);
        recording_file_ = nullptr;
    }

    ESP_LOGI(TAG, "Recording file saved.");
}

bool AudioRecorder::IsRecording() const {
    return is_recording_;
}

void AudioRecorder::WriteWavHeader() {
    // 写入一个临时的头，文件大小和数据大小后续更新
    header_.file_size = 36; // Placeholder (Header size - 8)
    header_.data_chunk_size = 0; // Placeholder
    fwrite(&header_, 1, sizeof(WavHeader), recording_file_);
}

void AudioRecorder::UpdateWavHeader() {
    // 计算最终的大小并更新
    header_.data_chunk_size = recorded_data_size_;
    header_.file_size = recorded_data_size_ + sizeof(WavHeader) - 8;
    
    // 移动文件指针到文件开头
    fseek(recording_file_, 0, SEEK_SET);
    
    // 重新写入完整的WAV头
    fwrite(&header_, 1, sizeof(WavHeader), recording_file_);
}
#include "meeting_recorder.h"
#include "esp_log.h"
#include "driver/i2s_std.h" // 需要I2S驱动来读取数据

#define TAG "MeetingRecorder"
#define REC_BUFFER_SIZE 2048

// 1. 定义一个静态计数器
static int recording_counter = 0;

// 声明我们在 compact_wifi_board.cc 中定义的全局函数，用于获取句柄
extern "C" i2s_chan_handle_t get_mic2_handle();

MeetingRecorder::MeetingRecorder() {
    // 构造函数可以保持为空
}

MeetingRecorder::~MeetingRecorder() {
    Stop(); // 确保在对象销毁时停止录音并释放资源
}

bool MeetingRecorder::Start(const std::string& base_path) {
    if (is_running_) {
        ESP_LOGW(TAG, "Meeting recording is already running.");
        return false;
    }

    // 创建 AudioRecorder 实例
    recorder_ = new AudioRecorder(16000, 16, 1);

    // 2. 使用计数器生成文件名
    char filename[128];
    snprintf(filename, sizeof(filename), "%s/meeting_%d.wav", base_path.c_str(), recording_counter++);
    ESP_LOGI(TAG, "Generated filename: %s", filename); // 添加日志以确认文件名

    // 启动 AudioRecorder
    if (!recorder_->StartRecording(filename)) {
        delete recorder_;
        recorder_ = nullptr;
        return false;
    }

    // 创建并启动后台任务（子线程）
    is_running_ = true;
    xTaskCreate(
        recording_task,         // 任务函数
        "Meeting Rec Task",     // 任务名称
        4096,                   // 栈大小
        this,                   // 传递给任务的参数 (MeetingRecorder实例本身)
        5,                      // 任务优先级
        &task_handle_           // 任务句柄
    );

    ESP_LOGI(TAG, "Meeting recording task started.");
    return true;
}

void MeetingRecorder::Stop() {
    if (!is_running_) return;

    is_running_ = false; // 向任务发出停止信号

    // 等待一小段时间，以允许任务完成其当前循环
    vTaskDelay(pdMS_TO_TICKS(200)); 

    if (task_handle_ != NULL) {
        vTaskDelete(task_handle_); // 删除任务
        task_handle_ = NULL;
    }

    if (recorder_ != nullptr) {
        recorder_->StopRecording(); // 停止并保存WAV文件
        delete recorder_;
        recorder_ = nullptr;
    }

    ESP_LOGI(TAG, "Meeting recording task stopped and file saved.");
}

bool MeetingRecorder::IsRecording() const {
    return is_running_;
}

// 静态任务函数 - 真正的“子线程”
void MeetingRecorder::recording_task(void* pvParameters) {
    MeetingRecorder* self = (MeetingRecorder*)pvParameters;
    uint8_t* buffer = (uint8_t*)malloc(REC_BUFFER_SIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer for recording task");
        if(self) {
            self->is_running_ = false;
        }
        vTaskDelete(NULL);
        return;
    }

    size_t bytes_read = 0;
    i2s_chan_handle_t mic_handle = get_mic2_handle(); // 获取麦克风句柄

    if (mic_handle == nullptr) {
        ESP_LOGE(TAG, "Failed to get Mic2 handle in recording task!");
        free(buffer);
        if(self) {
           self->is_running_ = false;
        }
        vTaskDelete(NULL);
        return;
    }

    while (self->is_running_) {
        // 从I2S硬件读取音频数据
        esp_err_t result = i2s_channel_read(mic_handle, buffer, REC_BUFFER_SIZE, &bytes_read, pdMS_TO_TICKS(1000));

        if (result == ESP_OK && bytes_read > 0) {
            // 将读取到的数据喂给 AudioRecorder
            if (self->recorder_) {
                self->recorder_->WriteAudioData(buffer, bytes_read);
            }
        } else if (result == ESP_ERR_TIMEOUT) {
            // 超时是正常的，继续循环
            continue;
        } else {
            ESP_LOGE(TAG, "I2S read error: %s", esp_err_to_name(result));
            self->is_running_ = false; 
        }
    }

    // 退出循环，清理资源
    free(buffer);
    vTaskDelete(NULL); // 任务自我销毁
}
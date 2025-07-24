#ifndef MEETING_RECORDER_H
#define MEETING_RECORDER_H

#include "audio_recorder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string>

class MeetingRecorder {
public:
    MeetingRecorder();
    ~MeetingRecorder();

    // 控制录音开始与结束的接口
    bool Start(const std::string& base_path = "/sdcard");
    void Stop();
    bool IsRecording() const;

private:
    // 录音任务（子线程）的静态函数
    static void recording_task(void* pvParameters);

    AudioRecorder* recorder_ = nullptr;     // 指向我们重构好的WAV文件写入工具
    TaskHandle_t task_handle_ = NULL;       // 指向我们创建的子线程
    bool is_running_ = false;               // 录音状态标志
};

#endif // MEETING_RECORDER_H
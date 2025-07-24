#include "wifi_board.h"
#include "audio_codecs/no_audio_codec.h" 
#include "display/oled_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "iot/thing_manager.h"
#include "led/single_led.h"
#include "assets/lang_config.h"

#include <wifi_station.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "driver/i2s_std.h"
#include "driver/sdmmc_host.h"

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "CompactWifiBoard"

LV_FONT_DECLARE(font_puhui_14_1);
LV_FONT_DECLARE(font_awesome_14_1);

// 创建一个文件内静态变量来保存第二个麦克风的I2S句柄
static i2s_chan_handle_t g_mic2_rx_handle = nullptr;

// 使用 extern "C" 代码块确保函数以C语言风格导出
extern "C" {
    i2s_chan_handle_t get_mic2_handle() {
        return g_mic2_rx_handle;
    }
}

class CompactWifiBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    Button touch_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    sdmmc_card_t* sd_card_ = nullptr;

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeSsd1306Display() {
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C, .on_color_trans_done = nullptr, .user_ctx = nullptr,
            .control_phase_bytes = 1, .dc_bit_offset = 6, .lcd_cmd_bits = 8,
            .lcd_param_bits = 8, .flags = { .dc_low_on_data = 0, .disable_control_phase = 0, },
            .scl_speed_hz = 400 * 1000,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(display_i2c_bus_, &io_config, &panel_io_));
        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.bits_per_pixel = 1;
        esp_lcd_panel_ssd1306_config_t ssd1306_config = { .height = static_cast<uint8_t>(DISPLAY_HEIGHT), };
        panel_config.vendor_config = &ssd1306_config;
#ifdef SH1106
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif
        ESP_LOGI(TAG, "SSD1306 driver installed");
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));
        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
            {&font_puhui_14_1, &font_awesome_14_1});
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });
        touch_button_.OnPressDown([this]() { Application::GetInstance().StartListening(); });
        touch_button_.OnPressUp([this]() { Application::GetInstance().StopListening(); });
        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec(); auto volume = codec->output_volume() + 10;
            if (volume > 100) { volume = 100; } codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });
        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100); GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });
        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec(); auto volume = codec->output_volume() - 10;
            if (volume < 0) { volume = 0; } codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });
        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0); GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    void InitializeIot() {
#if CONFIG_IOT_PROTOCOL_XIAOZHI
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        thing_manager.AddThing(iot::CreateThing("Lamp"));
#elif CONFIG_IOT_PROTOCOL_MCP
        static LampController lamp(LAMP_GPIO);
#endif
    }
    
 // 请用这个新函数完整替换掉 compact_wifi_board.cc 中旧的 InitializeSDCard 函数
void InitializeSDCard() {
    ESP_LOGI(TAG, "Initializing SD card using SDMMC peripheral...");

    // 挂载文件系统的配置
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false, // 如果挂载失败，不要格式化SD卡 [cite: 96]
        .max_files = 10,                 // 最大打开文件数
        .allocation_unit_size = 16 * 1024
    };
    
    sdmmc_card_t* sd_card_ = nullptr;
    const char *mount_point = "/sdcard";

    // SDMMC 主机接口配置，使用默认设置 [cite: 96]
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    // SDMMC 插槽配置 [cite: 96]
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

    // 根据文档，设置为1线SD模式 [cite: 9, 96, 220]
    slot_config.width = 1;

    // 启用内部上拉电阻 [cite: 96, 276]
    // 文档提示：内部上拉可能不足，建议外部连接10k上拉电阻 [cite: 251]
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    // 从 config.h 配置GPIO引脚
    slot_config.clk = SD_MMC_CLK_GPIO;
    slot_config.cmd = SD_MMC_CMD_GPIO;
    slot_config.d0 = SD_MMC_D0_GPIO;
    // 1线模式下，d1, d2, d3不使用

    // 调用 esp_vfs_fat_sdmmc_mount 函数挂载SD卡 [cite: 100]
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &sd_card_);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. If you want to format card on error, set format_if_mount_failed = true.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). Make sure SD card lines have pull-up resistors.", esp_err_to_name(ret));
        }
        return;
    }

    ESP_LOGI(TAG, "SD card filesystem mounted successfully at %s", mount_point);
    // 打印SD卡信息 [cite: 108]
    sdmmc_card_print_info(stdout, sd_card_);
}

    void InitializeSecondMic() {
        ESP_LOGI(TAG, "Initializing Second Microphone on I2S_NUM_1...");
        i2s_std_config_t i2s_config = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
            .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = AUDIO_I2S_MIC2_GPIO_SCK,
                .ws = AUDIO_I2S_MIC2_GPIO_WS,
                .dout = I2S_GPIO_UNUSED,
                .din = AUDIO_I2S_MIC2_GPIO_DIN,
                .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
            },
        };
        i2s_chan_config_t chan_cfg = {
            .id = I2S_NUM_1,
            .role = I2S_ROLE_MASTER,
            .dma_desc_num = 4,
            .dma_frame_num = 256,
            .auto_clear = true,
        };
        
        // 调用esp-idf函数创建I2S通道，并将句柄保存在全局变量中
        esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &g_mic2_rx_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create I2S_NUM_1 channel: %s", esp_err_to_name(ret));
            return;
        }
        
        ret = i2s_channel_init_std_mode(g_mic2_rx_handle, &i2s_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to init I2S_NUM_1 channel: %s", esp_err_to_name(ret));
            return;
        }

        ret = i2s_channel_enable(g_mic2_rx_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enable I2S_NUM_1 channel: %s", esp_err_to_name(ret));
            return;
        }
        ESP_LOGI(TAG, "Second Microphone on I2S_NUM_1 initialized successfully.");
    }

public:
    CompactWifiBoard() :
        boot_button_(BOOT_BUTTON_GPIO),
        touch_button_(TOUCH_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        //InitializeDisplayI2c();
        //InitializeSsd1306Display();
       //InitializeButtons();
        //InitializeIot();
        InitializeSDCard(); 
       // InitializeSecondMic(); // 调用第二个麦克风的初始化
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(CompactWifiBoard);
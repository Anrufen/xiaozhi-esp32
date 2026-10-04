#ifndef CUSTOM_LCD_DISPLAY_H
#define CUSTOM_LCD_DISPLAY_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <memory>
#include <string>
#include <vector>

#include "display/lcd_display.h"
#include "dlna_controller.h"
#include "service_config.h"

enum class ServiceStatus { kUnconfigured, kOk, kConnected = kOk, kAuthError, kNetworkError };

struct TomorrowWeather {
    std::string city = "成都市 · 武侯区";
    std::string weather = "多云 · 微风";
    std::string temp = "24";
    std::string temp_range = "17° ~ 26°";
    std::string humidity = "72%";
    std::string wind = "东北风 2级";
    std::string aqi = "AQI 25 优";
    std::string tips = "气候温润，体感舒适";
    std::string feels_like = "23°";
    std::string wind_dir = "东北风";
    std::string wind_level = "2级";
    std::string wind_speed = "10 km/h";
    std::string uv_level = "弱";
    std::string uv_val = "UV 2";
    std::string pressure = "1012";
};

struct StockData {
    std::string code = "00992.HK";
    std::string name = "联想集团";
    std::string en_symbol = "LNVGY";
    std::string currency = "HK$ ";
    std::string price = "34.32";
    std::string change_pct = "-0.64% (-0.22)";
    std::string change_val = "(-0.22)";
    std::string range = "33.74 - 35.24";
    std::string volume = "42.8M";
    bool is_up = false;
    bool is_down = true;
    bool loaded = false;
};

struct MusicTrack {
    std::string id = "";
    std::string title = "Cyber Dreams";
    std::string artist = "Synthwave · 128kbps";
    std::string source = "LOCAL";
    uint32_t duration_sec = 180;
    std::string stream_url = "";  // 局域网第三方设备直链 (format=raw)
};

#if CONFIG_WS185C_ENABLE_BESZEL
struct VpsNode {
    std::string name = "";
    std::string host = "";
    std::string status = "up";
    float load = 0.0f;           // 1分钟平均系统负载 (info.la[0])
    float net_bytes_sec = 0.0f;  // 实时网络带宽 bytes/s (info.bb)
    float mem = 0.0f;            // 内存占用率 % (info.mp)
    float disk = 0.0f;           // 磁盘占用率 % (info.dp)
    float cpu = 0.0f;
};
#endif

class CustomLcdDisplay : public SpiLcdDisplay {
public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                     int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                     bool swap_xy);
    virtual ~CustomLcdDisplay();

    virtual void SetupUI() override;
    virtual void SetTheme(Theme* theme) override;
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetStatus(const char* status) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;

    void UpdateTomorrowWeather(const TomorrowWeather& weather);
    void ShowWeatherPage();
    void ShowHomePage();
    void ShowPlayerPage();
    void ShowSettingsPage();
    void ShowServerPage();

    // 全屏语音交互界面控制
    void ShowWakeupOverlay();
    void HideWakeupOverlay();

    // 触发刷新股票与天气（供外部/MCP或定时逻辑调用）
    void FetchStockData();
    void FetchWeatherData();

    std::shared_ptr<waveshare185c::ServiceConfig> GetServiceConfig() const {
        return service_config_;
    }

#if CONFIG_WS185C_ENABLE_NAVIDROME
    bool ConfigureNavidrome(const std::string& url, const std::string& user,
                            const std::string& pass, std::string& err_msg);
    void FetchNavidromePlaylist();

    // 播放器控制（屏幕与语音MCP通用）
    void OnPlayerPlayPauseClicked();
    void OnPlayerPrevClicked();
    void OnPlayerNextClicked();
    bool IsPlaying() const { return is_playing_; }
    void StopNavidromeStream();

    // 直链获取与 DLNA 设备控制
    std::string GetCurrentTrackDirectUrl(bool for_dlna = true);
    std::string GetTrackDirectUrl(size_t track_idx, bool for_dlna = true);
    void SwitchPlaybackTarget(int target_idx);
    void ScanDlnaDevices(bool force = false);
    void OnVinylClicked();
    std::vector<waveshare185c::DlnaDevice> GetDlnaDevices() const;
    int GetCurrentPlaybackTarget() const;
    std::string GetCurrentPlaybackTargetName() const;

    // 智能曲库检索与场景播放（方案 B：大语言模型语义改写与定向点歌）
    std::string SearchAndPlayMusic(const std::string& keyword, const std::string& artist = "",
                                   const std::string& title = "",
                                   const std::string& candidates = "");

    // UI 刷新
    void UpdatePlayerUI();
#endif

#if CONFIG_WS185C_ENABLE_BESZEL
    bool ConfigureBeszel(const std::string& url, const std::string& user, const std::string& pass,
                         int32_t fetch_interval_s, int32_t rotate_interval_s, std::string& err_msg);
    // VPS 节点控制与数据同步
    void NextVpsNode();
    void PrevVpsNode();
    void TriggerBeszelFetch();
    void FetchBeszelData();
#endif

private:
    std::shared_ptr<waveshare185c::ServiceConfig> service_config_;

    // 首屏定制：赛博朋克极客 HUD 表盘（Clock & Stocks）
    void SetupHomeDashboardUI();
    void UpdateHomeClock();
    void CheckAndTriggerStockFetch();
    void CheckAndTriggerWeatherFetch();
    void ParseAndApplyStock(const std::string& body);
    void ApplyStockUI(const StockData& data);

    // 懒加载第二屏：气象机能监测 HUD 表盘（Weather Telemetry）
    void EnsureWeatherUI();
    void UpdateWeatherLabels();
    void UpdateWeatherClock();
    void UpdateIndicator(int active_page);

    // 首屏组件（Clock & Stocks Cyber HUD）
    lv_obj_t* home_dashboard_ = nullptr;
    lv_obj_t* home_hud_box_ = nullptr;
    lv_obj_t* home_sync_pill_ = nullptr;
    lv_obj_t* home_time_label_ = nullptr;
    lv_obj_t* home_sec_label_ = nullptr;
    lv_obj_t* home_date_label_ = nullptr;

    lv_obj_t* stock_card_ = nullptr;
    lv_obj_t* stock_name_label_ = nullptr;
    lv_obj_t* stock_code_label_ = nullptr;
    lv_obj_t* stock_badge_label_ = nullptr;
    lv_obj_t* stock_price_label_ = nullptr;
    lv_obj_t* stock_change_badge_ = nullptr;
    lv_obj_t* stock_change_label_ = nullptr;
    lv_obj_t* stock_sparkline_ = nullptr;
    lv_obj_t* stock_range_label_ = nullptr;
    lv_obj_t* stock_volume_label_ = nullptr;

    lv_obj_t* home_countdown_label_ = nullptr;
    lv_obj_t* home_progress_bar_ = nullptr;

    // 天气页面（Weather Telemetry 覆盖层方式，懒加载）
    lv_obj_t* weather_overlay_ = nullptr;
    bool weather_ui_created_ = false;
    int current_page_ = 0;

    // 天气界面组件（懒加载）
    lv_obj_t* weather_loc_label_ = nullptr;
    lv_obj_t* weather_cond_label_ = nullptr;
    lv_obj_t* weather_aqi_badge_ = nullptr;
    lv_obj_t* weather_temp_label_ = nullptr;
    lv_obj_t* weather_hum_label_ = nullptr;
    lv_obj_t* weather_sub_metrics_label_ = nullptr;
    lv_obj_t* weather_wind_val_ = nullptr;
    lv_obj_t* weather_wind_sub_ = nullptr;
    lv_obj_t* weather_uv_val_ = nullptr;
    lv_obj_t* weather_uv_sub_ = nullptr;
    lv_obj_t* weather_pres_val_ = nullptr;
    lv_obj_t* fore_cards_[3] = {nullptr, nullptr, nullptr};
    lv_obj_t* fore_times_[3] = {nullptr, nullptr, nullptr};
    lv_obj_t* fore_temps_[3] = {nullptr, nullptr, nullptr};
    void UpdateWeatherHourlyForecast();

    // 页面指示点（底部三页联动微圆点：播放器、主表盘、天气面板）
    lv_obj_t* indicator_container_ = nullptr;
    lv_obj_t* dot_player_ = nullptr;
    lv_obj_t* dot_home_ = nullptr;
    lv_obj_t* dot_weather_ = nullptr;

    // 懒加载第四屏：下拉快捷控制中心（Cyber Quick Settings）
    void EnsureSettingsUI();
    void UpdateSettingsValues();

    lv_obj_t* settings_overlay_ = nullptr;
    bool settings_ui_created_ = false;
    lv_obj_t* brightness_slider_ = nullptr;
    lv_obj_t* brightness_val_label_ = nullptr;
    lv_obj_t* volume_slider_ = nullptr;
    lv_obj_t* volume_val_label_ = nullptr;
    lv_obj_t* settings_wifi_label_ = nullptr;
    lv_obj_t* settings_navidrome_label_ = nullptr;

#if CONFIG_WS185C_ENABLE_NAVIDROME
    // 懒加载第三屏：极客机能音乐播放器（Cyber HUD Music Player）
    void EnsurePlayerUI();

    // 播放器页面（覆盖层方式，懒加载）
    lv_obj_t* player_overlay_ = nullptr;
    bool player_ui_created_ = false;

    // 播放器控件
    lv_obj_t* player_header_label_ = nullptr;
    lv_obj_t* player_device_label_ = nullptr;
    lv_obj_t* player_arc_ = nullptr;
    lv_obj_t* player_disc_icon_ = nullptr;
    lv_obj_t* player_title_label_ = nullptr;
    lv_obj_t* player_artist_label_ = nullptr;
    lv_obj_t* player_play_btn_ = nullptr;
    lv_obj_t* player_play_icon_ = nullptr;
    lv_obj_t* player_prev_btn_ = nullptr;
    lv_obj_t* player_next_btn_ = nullptr;
    lv_obj_t* player_time_label_ = nullptr;

    // 动态屏幕动效控件（黑胶唱片大尺寸声学律动频谱仪）
    static constexpr size_t kEqBarCount = 13;
    lv_obj_t* eq_bars_[kEqBarCount] = {nullptr};
    lv_obj_t* vinyl_inner_ = nullptr;
    lv_timer_t* player_anim_timer_ = nullptr;
    int anim_step_ = 0;
    void UpdatePlayerAnimation();
    void SetPlayerAnimationActive(bool active);

    // Navidrome 配置与流媒体
    ServiceStatus navidrome_status_ = ServiceStatus::kUnconfigured;
    std::string navidrome_server_;
    std::string navidrome_user_;
    std::string navidrome_pass_;
    std::string navidrome_token_ = "";
    std::string navidrome_salt_ = "";
    bool navidrome_fetching_ = false;

    // 流媒体播放任务控制
    void StartNavidromeStream(size_t track_idx);
    TaskHandle_t stream_task_handle_ = nullptr;
    bool stream_stop_requested_ = false;
    uint32_t current_playback_id_ = 0;

    // 播放器状态（支持日常默认随机歌单与 AI 场景歌单隔离）
    std::vector<MusicTrack> playlist_;
    std::vector<MusicTrack> default_playlist_;
    size_t current_track_idx_ = 0;
    bool is_playing_ = false;
    bool is_search_playlist_ = false;
    uint32_t play_elapsed_sec_ = 0;
#else
    inline void SetPlayerAnimationActive(bool /*active*/) {}
#endif

    TomorrowWeather current_weather_;
    StockData current_stock_;
    std::vector<StockData> stock_list_;
    size_t current_stock_idx_ = 0;
    uint32_t stock_carousel_counter_ = 0;
    lv_timer_t* clock_timer_ = nullptr;

    // 股票与天气刷新控制：避开连网激活握手阶段，待机平稳后才拉取，拉取完立即销毁任务栈
    int64_t idle_start_sec_ = 0;
    int64_t last_stock_fetch_sec_ = 0;
    bool stock_fetching_ = false;
    int64_t last_weather_fetch_sec_ = 0;
    bool weather_fetching_ = false;
    bool in_config_mode_cached_ = false;

#if CONFIG_WS185C_ENABLE_BESZEL
    // 懒加载第五屏：Beszel VPS 集群监控看板（Cyber Server Telemetry）
    ServiceStatus beszel_status_ = ServiceStatus::kUnconfigured;
    void EnsureServerUI();
    void UpdateServerUI();
    void CheckAndTriggerServerFetch();
    void ParseAndApplyBeszel(const std::string& body);

    lv_obj_t* server_overlay_ = nullptr;
    bool server_ui_created_ = false;

    // 同心三环 HUD 控件
    lv_obj_t* server_arc_load_ = nullptr;
    lv_obj_t* server_arc_ram_ = nullptr;
    lv_obj_t* server_arc_disk_ = nullptr;

    // 核心信息区控件
    lv_obj_t* server_status_dot_ = nullptr;
    lv_obj_t* server_name_label_ = nullptr;
    lv_obj_t* server_counter_label_ = nullptr;
    lv_obj_t* server_load_val_ = nullptr;
    lv_obj_t* server_ram_val_ = nullptr;
    lv_obj_t* server_disk_val_ = nullptr;
    lv_obj_t* server_net_val_ = nullptr;
    lv_obj_t* server_footer_label_ = nullptr;

    std::vector<VpsNode> vps_nodes_;
    size_t current_vps_idx_ = 0;
    uint32_t vps_carousel_counter_ = 0;

    std::string beszel_hub_url_;
    std::string beszel_user_;
    std::string beszel_pass_;
    int32_t beszel_fetch_interval_s_ = 15;
    int32_t beszel_rotate_interval_s_ = 5;
    std::string beszel_token_ = "";
    int64_t last_server_fetch_sec_ = 0;
    bool server_fetching_ = false;
#endif

    // 全屏赛博语音交互界面（Cyber Voice HUD）
    void SetupWakeupOverlay();
    void UpdateWakeupVuAnimation();
    void StartCountdown(int seconds = 10);
    void StopCountdown();
    void UpdateCountdownDisplay();
    const lv_font_t* GetMainTextFont16();
    bool voice_input_detected_ = false;

    // 配网与设备引导界面（WiFi Provisioning Screen）
    void SetupWifiConfigOverlay();
    void ShowWifiConfigOverlay(const std::string& ssid = "", const std::string& url = "",
                               const std::string& code = "");
    void HideWifiConfigOverlay();

    lv_obj_t* wakeup_overlay_ = nullptr;      // 360x360 全屏黑底容器
    lv_obj_t* led_eye_left_ = nullptr;        // 虚拟 LED 形象左眼
    lv_obj_t* led_eye_right_ = nullptr;       // 虚拟 LED 形象右眼
    lv_obj_t* wakeup_title_label_ = nullptr;  // “正在聆听” / “小智思考中” / “正在回答”
    lv_obj_t* wakeup_icon_label_ = nullptr;   // 状态专属 Material 图标

    // 动态互斥展示区：频谱 VS 播报文本
    lv_obj_t* wakeup_vu_container_ = nullptr;    // 频谱与分贝容器
    lv_obj_t* wakeup_vu_bars_[10] = {nullptr};   // 10 根声压条
    lv_obj_t* wakeup_db_label_ = nullptr;        // 分贝数值
    lv_obj_t* wakeup_text_container_ = nullptr;  // 播报文本透明容器
    lv_obj_t* wakeup_text_label_ = nullptr;      // 文本内容标签

    // 流式连续播报文本拼接缓存
    std::string assistant_stream_text_;
    bool is_new_assistant_turn_ = true;

    // 10 秒倒计时与律动定时器
    lv_timer_t* wakeup_timer_ = nullptr;     // 40ms 高帧率律动定时器
    lv_timer_t* countdown_timer_ = nullptr;  // 1s 真实倒计时定时器
    int auto_hide_seconds_left_ = 0;
    std::string current_title_base_ = "正在聆听";

    // 配网模式界面控件
    lv_obj_t* wifi_config_overlay_ = nullptr;
    lv_obj_t* wifi_config_ssid_val_ = nullptr;
    lv_obj_t* wifi_config_url_val_ = nullptr;
    lv_obj_t* wifi_config_code_box_ = nullptr;
    lv_obj_t* wifi_config_code_val_ = nullptr;
};

#endif  // CUSTOM_LCD_DISPLAY_H

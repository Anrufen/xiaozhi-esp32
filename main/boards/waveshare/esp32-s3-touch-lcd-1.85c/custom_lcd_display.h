#ifndef CUSTOM_LCD_DISPLAY_H
#define CUSTOM_LCD_DISPLAY_H

#include <string>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "display/lcd_display.h"

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
};

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

    void UpdateTomorrowWeather(const TomorrowWeather& weather);
    void ShowWeatherPage();
    void ShowHomePage();
    void ShowPlayerPage();
    void ShowSettingsPage();

    // 触发刷新股票与天气（供外部/MCP或定时逻辑调用）
    void FetchStockData();
    void FetchWeatherData();
    void SetNavidromeServer(const std::string& url);
    void FetchNavidromePlaylist();

    // 播放器控制（屏幕与语音MCP通用）
    void OnPlayerPlayPauseClicked();
    void OnPlayerPrevClicked();
    void OnPlayerNextClicked();

private:
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

    // 懒加载第三屏：极客机能音乐播放器（Cyber HUD Music Player）
    void EnsurePlayerUI();
    void UpdatePlayerUI();

    // 播放器页面（覆盖层方式，懒加载）
    lv_obj_t* player_overlay_ = nullptr;
    bool player_ui_created_ = false;

    // 播放器控件
    lv_obj_t* player_header_label_ = nullptr;
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

    // Navidrome 配置与流媒体
    std::string navidrome_server_ = "http://192.168.2.14:1011";
    std::string navidrome_user_ = "admin";
    std::string navidrome_pass_ = "music.123.123.z";
    std::string navidrome_token_ = "";
    std::string navidrome_salt_ = "";
    bool navidrome_fetching_ = false;

    // 流媒体播放任务控制
    void StartNavidromeStream(size_t track_idx);
    void StopNavidromeStream();
    TaskHandle_t stream_task_handle_ = nullptr;
    bool stream_stop_requested_ = false;
    uint32_t current_playback_id_ = 0;

    // 播放器状态
    std::vector<MusicTrack> playlist_;
    size_t current_track_idx_ = 0;
    bool is_playing_ = false;
    uint32_t play_elapsed_sec_ = 0;

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
};

#endif  // CUSTOM_LCD_DISPLAY_H

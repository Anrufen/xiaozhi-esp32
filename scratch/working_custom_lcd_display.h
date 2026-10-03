#ifndef CUSTOM_LCD_DISPLAY_H
#define CUSTOM_LCD_DISPLAY_H

#include "display/lcd_display.h"
#include <string>

struct TomorrowWeather {
    std::string city = "深圳";
    std::string weather = "多云";
    std::string temp = "23°";
    std::string temp_range = "20° ~ 28°";
    std::string humidity = "65%";
    std::string wind = "微风 2级";
    std::string aqi = "优";
    std::string tips = "适宜出行";
};

class CustomLcdDisplay : public SpiLcdDisplay {
public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                     int width, int height, int offset_x, int offset_y,
                     bool mirror_x, bool mirror_y, bool swap_xy);
    virtual ~CustomLcdDisplay();

    virtual void SetupUI() override;

    void UpdateTomorrowWeather(const TomorrowWeather& weather);
    void ShowWeatherPage();
    void ShowHomePage();

private:
    // 懒加载天气 UI：只在首次切换到天气页时创建 LVGL 对象，节省启动时内存
    void EnsureWeatherUI();
    void UpdateWeatherLabels();
    void UpdateClockAndBattery();
    void UpdateIndicator(int active_page);

    lv_obj_t* weather_overlay_ = nullptr;
    bool weather_ui_created_ = false;  // 标记天气 UI 是否已创建
    int current_page_ = 0;

    // 天气界面组件（懒加载）
    lv_obj_t* time_label_ = nullptr;
    lv_obj_t* ampm_label_ = nullptr;
    lv_obj_t* weekday_label_ = nullptr;
    lv_obj_t* date_label_ = nullptr;
    lv_obj_t* weather_icon_label_ = nullptr;
    lv_obj_t* weather_text_label_ = nullptr;
    lv_obj_t* temp_label_ = nullptr;
    lv_obj_t* battery_percent_label_ = nullptr;
    lv_obj_t* battery_icon_label_ = nullptr;
    lv_obj_t* mini_forecast_label_ = nullptr;

    // 页面指示点
    lv_obj_t* indicator_container_ = nullptr;
    lv_obj_t* dot_home_ = nullptr;
    lv_obj_t* dot_weather_ = nullptr;

    TomorrowWeather current_weather_;
    lv_timer_t* clock_timer_ = nullptr;
};

#endif // CUSTOM_LCD_DISPLAY_H

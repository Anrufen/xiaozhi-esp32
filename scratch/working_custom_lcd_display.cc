#include "custom_lcd_display.h"
#include "board.h"
#include "config.h"
#include "application.h"
#include "lvgl_theme.h"
#include <material_symbols.h>

#include <esp_log.h>
#include <ctime>

#define TAG "CustomLcdDisplay"

LV_FONT_DECLARE(font_noto_sans_basic_30_4);

CustomLcdDisplay::CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                   int width, int height, int offset_x, int offset_y,
                                   bool mirror_x, bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {}

CustomLcdDisplay::~CustomLcdDisplay() {
    if (clock_timer_) {
        lv_timer_delete(clock_timer_);
        clock_timer_ = nullptr;
    }
}

void CustomLcdDisplay::SetupUI() {
    // 调用基类 SetupUI 初始化核心屏幕组件
    // 重要：绝不 set_parent 基类创建的任何对象
    SpiLcdDisplay::SetupUI();

    DisplayLockGuard lock(this);
    auto screen = lv_screen_active();

    // ========================================================
    // 创建天气覆盖层容器（初始隐藏）
    // 内部子组件延迟到首次显示时创建，减少启动时内存占用
    // 这是解决配网崩溃的关键：WiFi AP 启动时需要大量内存
    // ========================================================
    weather_overlay_ = lv_obj_create(screen);
    lv_obj_set_size(weather_overlay_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(weather_overlay_, 0, 0);
    lv_obj_set_style_bg_color(weather_overlay_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(weather_overlay_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(weather_overlay_, 0, 0);
    lv_obj_set_style_border_width(weather_overlay_, 0, 0);
    lv_obj_set_style_radius(weather_overlay_, 0, 0);
    lv_obj_set_scrollbar_mode(weather_overlay_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(weather_overlay_, LV_DIR_NONE);
    lv_obj_add_flag(weather_overlay_, LV_OBJ_FLAG_HIDDEN);

    // ========================================================
    // 底部页面指示点
    // ========================================================
    indicator_container_ = lv_obj_create(screen);
    lv_obj_set_size(indicator_container_, 60, 20);
    lv_obj_align(indicator_container_, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_opa(indicator_container_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(indicator_container_, 0, 0);
    lv_obj_set_style_pad_all(indicator_container_, 0, 0);
    lv_obj_set_flex_flow(indicator_container_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(indicator_container_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(indicator_container_, 6, 0);
    lv_obj_set_scrollbar_mode(indicator_container_, LV_SCROLLBAR_MODE_OFF);

    dot_home_ = lv_obj_create(indicator_container_);
    lv_obj_set_size(dot_home_, 16, 5);
    lv_obj_set_style_radius(dot_home_, 3, 0);
    lv_obj_set_style_bg_color(dot_home_, lv_color_hex(0x4FC3F7), 0);
    lv_obj_set_style_border_width(dot_home_, 0, 0);
    lv_obj_add_flag(dot_home_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(dot_home_, [](lv_event_t* e) {
        auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
        self->ShowHomePage();
    }, LV_EVENT_CLICKED, this);

    dot_weather_ = lv_obj_create(indicator_container_);
    lv_obj_set_size(dot_weather_, 6, 5);
    lv_obj_set_style_radius(dot_weather_, 3, 0);
    lv_obj_set_style_bg_color(dot_weather_, lv_color_hex(0x4A5568), 0);
    lv_obj_set_style_border_width(dot_weather_, 0, 0);
    lv_obj_add_flag(dot_weather_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(dot_weather_, [](lv_event_t* e) {
        auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
        self->ShowWeatherPage();
    }, LV_EVENT_CLICKED, this);

    // ========================================================
    // 手势滑动切换页面
    // 在配网/启动等非正常运行状态下禁止手势切换，避免竞争
    // ========================================================
    lv_obj_add_event_cb(screen, [](lv_event_t* e) {
        auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
        // 仅在正常运行状态下允许手势切换
        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();
        if (state == kDeviceStateWifiConfiguring || state == kDeviceStateStarting) {
            return;
        }
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir == LV_DIR_LEFT && self->current_page_ == 0) {
            self->ShowWeatherPage();
        } else if (dir == LV_DIR_RIGHT && self->current_page_ == 1) {
            self->ShowHomePage();
        }
    }, LV_EVENT_GESTURE, this);

    lv_obj_add_event_cb(weather_overlay_, [](lv_event_t* e) {
        auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir == LV_DIR_RIGHT && self->current_page_ == 1) {
            self->ShowHomePage();
        }
    }, LV_EVENT_GESTURE, this);

    lv_obj_move_foreground(indicator_container_);
}

// 懒加载：只在首次显示天气页时创建所有 LVGL 子对象
void CustomLcdDisplay::EnsureWeatherUI() {
    if (weather_ui_created_) return;
    weather_ui_created_ = true;

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();

    // ==========================================
    // 上半部分：极简表盘（时间、星期、日期）
    // 圆形屏 360x360，内容需避开四角不可见区域
    // ==========================================
    lv_obj_t* top_info_box = lv_obj_create(weather_overlay_);
    lv_obj_set_size(top_info_box, 280, 140);
    lv_obj_align(top_info_box, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_opa(top_info_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_info_box, 0, 0);
    lv_obj_set_style_pad_all(top_info_box, 0, 0);
    lv_obj_set_flex_flow(top_info_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(top_info_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(top_info_box, 2, 0);
    lv_obj_set_scrollbar_mode(top_info_box, LV_SCROLLBAR_MODE_OFF);

    // 时间行
    lv_obj_t* time_row = lv_obj_create(top_info_box);
    lv_obj_set_size(time_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(time_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(time_row, 0, 0);
    lv_obj_set_style_pad_all(time_row, 0, 0);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(time_row, 4, 0);

    time_label_ = lv_label_create(time_row);
    lv_obj_set_style_text_font(time_label_, &font_noto_sans_basic_30_4, 0);
    lv_obj_set_style_text_color(time_label_, lv_color_hex(0x1A1D20), 0);
    lv_label_set_text(time_label_, "--:--");

    ampm_label_ = lv_label_create(time_row);
    lv_obj_set_style_text_font(ampm_label_, text_font, 0);
    lv_obj_set_style_text_color(ampm_label_, lv_color_hex(0x5F6672), 0);
    lv_obj_set_style_pad_bottom(ampm_label_, 4, 0);
    lv_label_set_text(ampm_label_, "");

    weekday_label_ = lv_label_create(top_info_box);
    lv_obj_set_style_text_font(weekday_label_, text_font, 0);
    lv_obj_set_style_text_color(weekday_label_, lv_color_hex(0x1A1D20), 0);
    lv_label_set_text(weekday_label_, "");

    date_label_ = lv_label_create(top_info_box);
    lv_obj_set_style_text_font(date_label_, text_font, 0);
    lv_obj_set_style_text_color(date_label_, lv_color_hex(0x8C929D), 0);
    lv_label_set_text(date_label_, "");

    // ==========================================
    // 下半部分：拱形深黑底座
    // ==========================================
    lv_obj_t* bottom_deck = lv_obj_create(weather_overlay_);
    lv_obj_set_size(bottom_deck, 340, 175);
    lv_obj_align(bottom_deck, LV_ALIGN_BOTTOM_MID, 0, 5);
    lv_obj_set_style_radius(bottom_deck, 100, 0);
    lv_obj_set_style_bg_color(bottom_deck, lv_color_hex(0x111317), 0);
    lv_obj_set_style_bg_opa(bottom_deck, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bottom_deck, lv_color_hex(0x282C35), 0);
    lv_obj_set_style_border_width(bottom_deck, 1, 0);
    lv_obj_set_style_pad_all(bottom_deck, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_deck, LV_SCROLLBAR_MODE_OFF);

    // 底座内容容器
    lv_obj_t* weather_content = lv_obj_create(bottom_deck);
    lv_obj_set_size(weather_content, 280, 100);
    lv_obj_align(weather_content, LV_ALIGN_TOP_MID, 0, 25);
    lv_obj_set_style_bg_opa(weather_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(weather_content, 0, 0);
    lv_obj_set_style_pad_all(weather_content, 0, 0);
    lv_obj_set_flex_flow(weather_content, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(weather_content, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(weather_content, LV_SCROLLBAR_MODE_OFF);

    // A. 左侧：天气概况
    lv_obj_t* left_box = lv_obj_create(weather_content);
    lv_obj_set_size(left_box, 120, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(left_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left_box, 0, 0);
    lv_obj_set_style_pad_all(left_box, 0, 0);
    lv_obj_set_flex_flow(left_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(left_box, 2, 0);

    weather_icon_label_ = lv_label_create(left_box);
    lv_obj_set_style_text_font(weather_icon_label_, text_font, 0);
    lv_obj_set_style_text_color(weather_icon_label_, lv_color_hex(0xF6B819), 0);
    lv_label_set_text(weather_icon_label_, current_weather_.weather.c_str());

    weather_text_label_ = lv_label_create(left_box);
    lv_obj_set_style_text_font(weather_text_label_, text_font, 0);
    lv_obj_set_style_text_color(weather_text_label_, lv_color_hex(0xA0AEC0), 0);
    std::string desc = "明天 · " + current_weather_.city;
    lv_label_set_text(weather_text_label_, desc.c_str());

    // B. 分割线
    lv_obj_t* divider = lv_obj_create(weather_content);
    lv_obj_set_size(divider, 1, 50);
    lv_obj_set_style_bg_color(divider, lv_color_hex(0x353B47), 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_pad_all(divider, 0, 0);

    // C. 右侧：温度和电量
    lv_obj_t* right_box = lv_obj_create(weather_content);
    lv_obj_set_size(right_box, 120, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_box, 0, 0);
    lv_obj_set_style_pad_all(right_box, 0, 0);
    lv_obj_set_flex_flow(right_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(right_box, 4, 0);

    // 温度 + 电量行
    lv_obj_t* temp_row = lv_obj_create(right_box);
    lv_obj_set_size(temp_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(temp_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(temp_row, 0, 0);
    lv_obj_set_style_pad_all(temp_row, 0, 0);
    lv_obj_set_flex_flow(temp_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(temp_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(temp_row, 4, 0);

    temp_label_ = lv_label_create(temp_row);
    lv_obj_set_style_text_font(temp_label_, &font_noto_sans_basic_30_4, 0);
    lv_obj_set_style_text_color(temp_label_, lv_color_hex(0xFFFFFF), 0);
    std::string t_str = current_weather_.temp;
    if (t_str.find("°") == std::string::npos) t_str += "°";
    lv_label_set_text(temp_label_, t_str.c_str());

    battery_icon_label_ = lv_label_create(temp_row);
    lv_obj_set_style_text_font(battery_icon_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_icon_label_, lv_color_hex(0xF6B819), 0);
    lv_label_set_text(battery_icon_label_, MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_FULL);

    battery_percent_label_ = lv_label_create(temp_row);
    lv_obj_set_style_text_font(battery_percent_label_, text_font, 0);
    lv_obj_set_style_text_color(battery_percent_label_, lv_color_hex(0xD0D7DE), 0);
    lv_label_set_text(battery_percent_label_, "");

    // 温度范围
    mini_forecast_label_ = lv_label_create(right_box);
    lv_obj_set_style_text_font(mini_forecast_label_, text_font, 0);
    lv_obj_set_style_text_color(mini_forecast_label_, lv_color_hex(0xF6B819), 0);
    lv_label_set_text(mini_forecast_label_, current_weather_.temp_range.c_str());

    // 立即刷新一次时间和电量
    UpdateClockAndBattery();

    // 启动 LVGL 定时器更新时钟（只在天气 UI 创建后才启动）
    if (!clock_timer_) {
        clock_timer_ = lv_timer_create([](lv_timer_t* timer) {
            auto self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
            self->UpdateClockAndBattery();
        }, 1000, this);
    }
}

void CustomLcdDisplay::UpdateWeatherLabels() {
    if (!weather_ui_created_ || !temp_label_) return;

    std::string t_str = current_weather_.temp;
    if (t_str.find("°") == std::string::npos) {
        t_str += "°";
    }
    lv_label_set_text(temp_label_, t_str.c_str());

    if (weather_icon_label_) {
        lv_label_set_text(weather_icon_label_, current_weather_.weather.c_str());
    }
    if (weather_text_label_) {
        std::string desc_str = "明天 · " + current_weather_.city;
        lv_label_set_text(weather_text_label_, desc_str.c_str());
    }
    if (mini_forecast_label_ && !current_weather_.temp_range.empty()) {
        lv_label_set_text(mini_forecast_label_, current_weather_.temp_range.c_str());
    }
}

void CustomLcdDisplay::UpdateClockAndBattery() {
    if (!time_label_) return;

    time_t now = time(NULL);
    struct tm* tm_now = localtime(&now);

    if (tm_now && tm_now->tm_year >= (2025 - 1900)) {
        int hour12 = tm_now->tm_hour % 12;
        if (hour12 == 0) hour12 = 12;
        char time_buf[16];
        snprintf(time_buf, sizeof(time_buf), "%d:%02d", hour12, tm_now->tm_min);
        lv_label_set_text(time_label_, time_buf);
        lv_label_set_text(ampm_label_, tm_now->tm_hour >= 12 ? "PM" : "AM");

        static const char* const weekdays[] = {"SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"};
        lv_label_set_text(weekday_label_, weekdays[tm_now->tm_wday]);

        static const char* const months[] = {"JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE", "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"};
        char date_buf[32];
        snprintf(date_buf, sizeof(date_buf), "%s %d, %d", months[tm_now->tm_mon], tm_now->tm_mday, tm_now->tm_year + 1900);
        lv_label_set_text(date_label_, date_buf);
    }

    int battery_level = 0;
    bool charging = false, discharging = false;
    auto& board = Board::GetInstance();
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        if (battery_percent_label_) {
            char batt_buf[16];
            snprintf(batt_buf, sizeof(batt_buf), "%d%%", battery_level);
            lv_label_set_text(battery_percent_label_, batt_buf);
        }
        if (battery_icon_label_) {
            lv_label_set_text(battery_icon_label_,
                charging ? MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_BOLT
                         : MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_FULL);
        }
    }
}

void CustomLcdDisplay::UpdateIndicator(int active_page) {
    if (!dot_home_ || !dot_weather_) return;

    if (active_page == 1) {
        lv_obj_set_size(dot_home_, 6, 5);
        lv_obj_set_style_bg_color(dot_home_, lv_color_hex(0x4A5568), 0);
        lv_obj_set_size(dot_weather_, 16, 5);
        lv_obj_set_style_bg_color(dot_weather_, lv_color_hex(0x4FC3F7), 0);
    } else {
        lv_obj_set_size(dot_home_, 16, 5);
        lv_obj_set_style_bg_color(dot_home_, lv_color_hex(0x4FC3F7), 0);
        lv_obj_set_size(dot_weather_, 6, 5);
        lv_obj_set_style_bg_color(dot_weather_, lv_color_hex(0x4A5568), 0);
    }
}

void CustomLcdDisplay::UpdateTomorrowWeather(const TomorrowWeather& weather) {
    DisplayLockGuard lock(this);
    current_weather_ = weather;
    UpdateWeatherLabels();
}

void CustomLcdDisplay::ShowWeatherPage() {
    DisplayLockGuard lock(this);
    if (!weather_overlay_) return;

    // 懒加载：首次切换时才创建天气 UI 子组件
    EnsureWeatherUI();

    current_page_ = 1;
    lv_obj_remove_flag(weather_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(weather_overlay_);
    lv_obj_move_foreground(indicator_container_);
    UpdateIndicator(1);
}

void CustomLcdDisplay::ShowHomePage() {
    DisplayLockGuard lock(this);
    if (!weather_overlay_) return;

    current_page_ = 0;
    lv_obj_add_flag(weather_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(indicator_container_);
    UpdateIndicator(0);
}

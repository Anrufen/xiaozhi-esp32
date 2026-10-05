#include "custom_lcd_display.h"
#include <material_symbols.h>
#include "application.h"
#include "assets/lang_config.h"
#include "audio/demuxer/ogg_demuxer.h"
#include "board.h"
#include "config.h"
#include "dlna_controller.h"
#include "lvgl_theme.h"
#include "player_icons.h"
#include "wifi_manager.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <random>
#include <set>
#include <vector>

#define TAG "CustomLcdDisplay"
using waveshare185c::ServiceConfig;

// 编译期常驻 Flash ROData 的大号与小号字体
LV_FONT_DECLARE(font_maison_neue_book_14);
LV_FONT_DECLARE(font_maison_neue_book_26);
LV_FONT_DECLARE(font_noto_sans_basic_30_4);
LV_FONT_DECLARE(font_noto_sans_basic_20_4);
LV_FONT_DECLARE(font_noto_sans_basic_16_4);
LV_FONT_DECLARE(font_material_symbols_16_4);
LV_FONT_DECLARE(font_material_symbols_30_4);

// 走势图采样点（宽幅展开至 260px，高 26px）
static const lv_point_precise_t kSparklinePoints[] = {{0, 20},   {36, 17}, {74, 22},  {112, 14},
                                                      {150, 16}, {188, 8}, {226, 12}, {260, 4}};

// 流式文本清洗：过滤前后空白并智能去重、规范化连续逗号标点
static std::string SanitizeDisplayText(const std::string& input) {
    if (input.empty())
        return "";
    std::string out;
    out.reserve(input.size());

    // 1. 去除首尾空白字符
    size_t start = 0;
    while (start < input.size() && (input[start] == ' ' || input[start] == '\t' ||
                                    input[start] == '\r' || input[start] == '\n')) {
        start++;
    }
    size_t end = input.size();
    while (end > start && (input[end - 1] == ' ' || input[end - 1] == '\t' ||
                           input[end - 1] == '\r' || input[end - 1] == '\n')) {
        end--;
    }

    // 2. 清洗连续逗号 (英文逗号 ',' 或 中文全角逗号 '，' 对应的 UTF-8: EF BC 8C)
    bool prev_is_comma = false;
    for (size_t i = start; i < end;) {
        unsigned char c = (unsigned char)input[i];
        if (c == ',') {
            if (!prev_is_comma) {
                out += "，";
                prev_is_comma = true;
            }
            i++;
        } else if (i + 2 < end && (unsigned char)input[i] == 0xEF &&
                   (unsigned char)input[i + 1] == 0xBC && (unsigned char)input[i + 2] == 0x8C) {
            if (!prev_is_comma) {
                out.append(input, i, 3);
                prev_is_comma = true;
            }
            i += 3;
        } else {
            out.push_back(input[i]);
            prev_is_comma = false;
            i++;
        }
    }
    return out;
}

CustomLcdDisplay::CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                   int width, int height, int offset_x, int offset_y, bool mirror_x,
                                   bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                    swap_xy) {
    service_config_ = waveshare185c::CreateDefaultServiceConfig();
#if CONFIG_WS185C_ENABLE_NAVIDROME
    if (service_config_) {
        auto navi = service_config_->GetNavidromeConfig();
        navidrome_server_ = navi.url;
        navidrome_user_ = navi.user;
        navidrome_pass_ = navi.pass;
        navidrome_status_ =
            navi.IsConfigured() ? ServiceStatus::kConnected : ServiceStatus::kUnconfigured;
    }
#endif
#if CONFIG_WS185C_ENABLE_BESZEL
    if (service_config_) {
        auto bsz = service_config_->GetBeszelConfig();
        beszel_hub_url_ = bsz.url;
        beszel_user_ = bsz.user;
        beszel_pass_ = bsz.pass;
        beszel_fetch_interval_s_ = bsz.fetch_interval_s;
        beszel_rotate_interval_s_ = bsz.rotate_interval_s;
        beszel_status_ =
            bsz.IsConfigured() ? ServiceStatus::kConnected : ServiceStatus::kUnconfigured;
    }
#endif
}

CustomLcdDisplay::~CustomLcdDisplay() {
    StopCountdown();
    if (countdown_timer_) {
        lv_timer_delete(countdown_timer_);
        countdown_timer_ = nullptr;
    }
    if (wakeup_timer_) {
        lv_timer_delete(wakeup_timer_);
        wakeup_timer_ = nullptr;
    }
    if (clock_timer_) {
        lv_timer_delete(clock_timer_);
        clock_timer_ = nullptr;
    }
#if CONFIG_WS185C_ENABLE_NAVIDROME
    if (player_anim_timer_) {
        lv_timer_delete(player_anim_timer_);
        player_anim_timer_ = nullptr;
    }
#endif
}

void CustomLcdDisplay::SetupUI() {
    // 1. 调用基类 SetupUI 初始化核心屏幕组件
    SpiLcdDisplay::SetupUI();

    DisplayLockGuard lock(this);
    auto screen = lv_screen_active();

    // 屏幕深邃极夜黑蓝底色 (#0A0E16)
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0A0E16), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    // 隐藏小智原生状态栏与顶栏，彻底消除顶部药丸和时间重叠冲突
    if (status_bar_) {
        lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    }
    if (top_bar_) {
        lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    }

    // 关键优化：清除基类容器滚动，设置透明并开启事件全穿透冒泡
    if (container_) {
        lv_obj_remove_flag(container_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(container_, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(container_, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_set_style_bg_opa(container_, LV_OPA_TRANSP, 0);
    }
    if (content_) {
        lv_obj_remove_flag(content_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(content_, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(content_, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);
    }

    // 2. 首屏定制：宽幅通透表盘（Clock & Stocks）
    SetupHomeDashboardUI();

    // 3. 创建第二屏天气全屏覆盖层容器（初始隐藏，首次滑动时懒加载，保护开机内存）
    weather_overlay_ = lv_obj_create(screen);
    lv_obj_set_size(weather_overlay_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(weather_overlay_, 0, 0);
    lv_obj_set_style_bg_color(weather_overlay_, lv_color_hex(0x0A0E16), 0);
    lv_obj_set_style_bg_opa(weather_overlay_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(weather_overlay_, 0, 0);
    lv_obj_set_style_border_width(weather_overlay_, 0, 0);
    lv_obj_set_style_radius(weather_overlay_, 0, 0);
    lv_obj_set_scrollbar_mode(weather_overlay_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(weather_overlay_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(weather_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(weather_overlay_, LV_OBJ_FLAG_CLICKABLE);

    // 4. 底部页面三圆点指示器（播放器、主表盘、天气面板）
    indicator_container_ = lv_obj_create(screen);
    lv_obj_set_size(indicator_container_, 120, 32);
    lv_obj_align(indicator_container_, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_opa(indicator_container_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(indicator_container_, 0, 0);
    lv_obj_set_style_pad_all(indicator_container_, 0, 0);
    lv_obj_set_flex_flow(indicator_container_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(indicator_container_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(indicator_container_, 10, 0);
    lv_obj_set_scrollbar_mode(indicator_container_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(indicator_container_, LV_OBJ_FLAG_SCROLLABLE);

#if CONFIG_WS185C_ENABLE_NAVIDROME
    dot_player_ = lv_obj_create(indicator_container_);
    lv_obj_set_size(dot_player_, 6, 5);
    lv_obj_set_style_radius(dot_player_, 3, 0);
    lv_obj_set_style_bg_color(dot_player_, lv_color_hex(0x31353E), 0);
    lv_obj_set_style_border_width(dot_player_, 0, 0);
    lv_obj_add_flag(dot_player_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        dot_player_,
        [](lv_event_t* e) {
            auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
            self->ShowPlayerPage();
        },
        LV_EVENT_CLICKED, this);
#endif

    dot_home_ = lv_obj_create(indicator_container_);
    lv_obj_set_size(dot_home_, 18, 5);
    lv_obj_set_style_radius(dot_home_, 3, 0);
    lv_obj_set_style_bg_color(dot_home_, lv_color_hex(0x00D2FF), 0);
    lv_obj_set_style_border_width(dot_home_, 0, 0);
    lv_obj_add_flag(dot_home_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        dot_home_,
        [](lv_event_t* e) {
            auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
            self->ShowHomePage();
        },
        LV_EVENT_CLICKED, this);

    dot_weather_ = lv_obj_create(indicator_container_);
    lv_obj_set_size(dot_weather_, 6, 5);
    lv_obj_set_style_radius(dot_weather_, 3, 0);
    lv_obj_set_style_bg_color(dot_weather_, lv_color_hex(0x31353E), 0);
    lv_obj_set_style_border_width(dot_weather_, 0, 0);
    lv_obj_add_flag(dot_weather_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        dot_weather_,
        [](lv_event_t* e) {
            auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
            self->ShowWeatherPage();
        },
        LV_EVENT_CLICKED, this);

    // 5. 全局多向手势滑动监听器（Player <-> Home <-> Weather, 下拉 Settings）
    auto on_gesture_cb = [](lv_event_t* e) {
        auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();
        if (state == kDeviceStateWifiConfiguring) {
            return;
        }
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (self->current_page_ == 0) {
            if (dir == LV_DIR_LEFT) {
                self->ShowWeatherPage();
#if CONFIG_WS185C_ENABLE_NAVIDROME
            } else if (dir == LV_DIR_RIGHT) {
                self->ShowPlayerPage();
#endif
            } else if (dir == LV_DIR_BOTTOM) {
                self->ShowSettingsPage();
#if CONFIG_WS185C_ENABLE_BESZEL
            } else if (dir == LV_DIR_TOP) {
                self->ShowServerPage();
#endif
            }
        } else if (self->current_page_ == 1) {
            if (dir == LV_DIR_RIGHT) {
                self->ShowHomePage();
            } else if (dir == LV_DIR_BOTTOM) {
                self->ShowSettingsPage();
#if CONFIG_WS185C_ENABLE_BESZEL
            } else if (dir == LV_DIR_TOP) {
                self->ShowServerPage();
#endif
            }
#if CONFIG_WS185C_ENABLE_NAVIDROME
        } else if (self->current_page_ == -1) {
            if (dir == LV_DIR_LEFT) {
                self->ShowHomePage();
            } else if (dir == LV_DIR_BOTTOM) {
                self->ShowSettingsPage();
#if CONFIG_WS185C_ENABLE_BESZEL
            } else if (dir == LV_DIR_TOP) {
                self->ShowServerPage();
#endif
            }
#endif
        } else if (self->current_page_ == 2) {
            if (dir == LV_DIR_TOP) {
                self->ShowHomePage();
            }
#if CONFIG_WS185C_ENABLE_BESZEL
        } else if (self->current_page_ == 3) {
            if (dir == LV_DIR_BOTTOM) {
                self->ShowHomePage();
            } else if (dir == LV_DIR_LEFT) {
                self->NextVpsNode();
            } else if (dir == LV_DIR_RIGHT) {
                self->PrevVpsNode();
            }
#endif
        }
    };

    lv_obj_add_event_cb(screen, on_gesture_cb, LV_EVENT_GESTURE, this);
    lv_obj_add_event_cb(weather_overlay_, on_gesture_cb, LV_EVENT_GESTURE, this);

    lv_obj_move_foreground(indicator_container_);

    // 6. 启动每秒执行的时钟、股票与播放器调度定时器
    clock_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
            self->UpdateHomeClock();
            if (self->weather_ui_created_) {
                self->UpdateWeatherClock();
            }
#if CONFIG_WS185C_ENABLE_NAVIDROME
            if (self->is_playing_ && self->player_ui_created_ && !self->playlist_.empty()) {
                self->play_elapsed_sec_++;
                const auto& cur_track = self->playlist_[self->current_track_idx_];
                if (self->play_elapsed_sec_ >= cur_track.duration_sec) {
                    self->OnPlayerNextClicked();
                } else {
                    self->UpdatePlayerUI();
                }
            }
#endif
            self->CheckAndTriggerStockFetch();
            self->CheckAndTriggerWeatherFetch();
        },
        1000, this);

    // 5. 初始化全局顶层唤醒交互与呼吸弧线
    SetupWakeupOverlay();
    // 6. 初始化配网全屏指引界面
    SetupWifiConfigOverlay();
}

void CustomLcdDisplay::SetTheme(Theme* theme) {
    LcdDisplay::SetTheme(theme);

    // 保持容器透明
    if (container_) {
        lv_obj_set_style_bg_opa(container_, LV_OPA_TRANSP, 0);
    }
    if (content_) {
        lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);
    }
    // 确保基类状态栏隐藏不遮挡表盘
    if (status_bar_) {
        lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    if (top_bar_) {
        lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_image_) {
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_label_) {
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

void CustomLcdDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    // 表盘模式下必须严格隐藏基类的黄色表情大脸与 AI 图标，绝不允许黄色表情叠加污染表盘
    if (emoji_image_) {
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_label_) {
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

const lv_font_t* CustomLcdDisplay::GetMainTextFont16() {
    if (current_theme_ && current_theme_->GetTextFont()) {
        auto* f = current_theme_->GetTextFont()->font();
        if (f)
            return f;
    }
    return &font_noto_sans_basic_16_4;
}

void CustomLcdDisplay::SetupWakeupOverlay() {
    // 1. 全屏纯黑科技底座 (360x360 挂载在全局最高图层 lv_layer_top())
    wakeup_overlay_ = lv_obj_create(lv_layer_top());
    lv_obj_set_size(wakeup_overlay_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(wakeup_overlay_, 0, 0);
    lv_obj_set_style_bg_color(wakeup_overlay_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(wakeup_overlay_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wakeup_overlay_, 0, 0);
    lv_obj_set_style_pad_all(wakeup_overlay_, 0, 0);
    lv_obj_set_scrollbar_mode(wakeup_overlay_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(wakeup_overlay_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wakeup_overlay_, LV_OBJ_FLAG_HIDDEN);

    // 随时轻触屏幕空白处，退出交互界面
    lv_obj_add_event_cb(
        wakeup_overlay_,
        [](lv_event_t* e) {
            auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
            if (self) {
                self->HideWakeupOverlay();
            }
        },
        LV_EVENT_CLICKED, this);

    // 2. 背景 HUD 刻度与十字标尺圈 (细圆盘与极坐标准星)
    lv_obj_t* hud_outer = lv_obj_create(wakeup_overlay_);
    lv_obj_set_size(hud_outer, 352, 352);
    lv_obj_align(hud_outer, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(hud_outer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hud_outer, 1, 0);
    lv_obj_set_style_border_color(hud_outer, lv_color_hex(0x1C2028), 0);
    lv_obj_set_style_radius(hud_outer, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(hud_outer, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* hud_inner = lv_obj_create(wakeup_overlay_);
    lv_obj_set_size(hud_inner, 344, 344);
    lv_obj_align(hud_inner, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(hud_inner, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hud_inner, 1, 0);
    lv_obj_set_style_border_color(hud_inner, lv_color_hex(0x31353E), 0);
    lv_obj_set_style_border_opa(hud_inner, 120, 0);
    lv_obj_set_style_radius(hud_inner, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(hud_inner, LV_OBJ_FLAG_CLICKABLE);

    // 四极准星微刻度
    lv_obj_t* m_top = lv_obj_create(wakeup_overlay_);
    lv_obj_set_size(m_top, 2, 8);
    lv_obj_align(m_top, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_bg_color(m_top, lv_color_hex(0x00D2FF), 0);
    lv_obj_remove_flag(m_top, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* m_right = lv_obj_create(wakeup_overlay_);
    lv_obj_set_size(m_right, 8, 2);
    lv_obj_align(m_right, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(m_right, lv_color_hex(0x4EDEA3), 0);
    lv_obj_remove_flag(m_right, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* m_bottom = lv_obj_create(wakeup_overlay_);
    lv_obj_set_size(m_bottom, 2, 8);
    lv_obj_align(m_bottom, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(m_bottom, lv_color_hex(0x859399), 0);
    lv_obj_remove_flag(m_bottom, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* m_left = lv_obj_create(wakeup_overlay_);
    lv_obj_set_size(m_left, 8, 2);
    lv_obj_align(m_left, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_color(m_left, lv_color_hex(0x859399), 0);
    lv_obj_remove_flag(m_left, LV_OBJ_FLAG_CLICKABLE);

    // 3. 顶部虚拟发光 LED 双眼形象 (轻量化、灵动科技感、极致平衡性能)
    lv_obj_t* led_face = lv_obj_create(wakeup_overlay_);
    lv_obj_set_size(led_face, 140, 64);
    lv_obj_align(led_face, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_bg_opa(led_face, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(led_face, 0, 0);
    lv_obj_set_style_pad_all(led_face, 0, 0);
    lv_obj_remove_flag(led_face, LV_OBJ_FLAG_SCROLLABLE);

    // 左眼
    led_eye_left_ = lv_obj_create(led_face);
    lv_obj_set_size(led_eye_left_, 22, 36);
    lv_obj_set_pos(led_eye_left_, 36, 14);
    lv_obj_set_style_bg_color(led_eye_left_, lv_color_hex(0x00D2FF), 0);
    lv_obj_set_style_radius(led_eye_left_, 11, 0);
    lv_obj_set_style_border_width(led_eye_left_, 0, 0);
    lv_obj_set_style_shadow_width(led_eye_left_, 14, 0);
    lv_obj_set_style_shadow_color(led_eye_left_, lv_color_hex(0x00D2FF), 0);
    lv_obj_set_style_shadow_opa(led_eye_left_, 200, 0);
    lv_obj_remove_flag(led_eye_left_, LV_OBJ_FLAG_SCROLLABLE);

    // 右眼
    led_eye_right_ = lv_obj_create(led_face);
    lv_obj_set_size(led_eye_right_, 22, 36);
    lv_obj_set_pos(led_eye_right_, 82, 14);
    lv_obj_set_style_bg_color(led_eye_right_, lv_color_hex(0x00D2FF), 0);
    lv_obj_set_style_radius(led_eye_right_, 11, 0);
    lv_obj_set_style_border_width(led_eye_right_, 0, 0);
    lv_obj_set_style_shadow_width(led_eye_right_, 14, 0);
    lv_obj_set_style_shadow_color(led_eye_right_, lv_color_hex(0x00D2FF), 0);
    lv_obj_set_style_shadow_opa(led_eye_right_, 200, 0);
    lv_obj_remove_flag(led_eye_right_, LV_OBJ_FLAG_SCROLLABLE);

    // 4. 状态标题区（使用中文字体，去除竖线光标，换成 Material 状态图标）
    lv_obj_t* title_box = lv_obj_create(wakeup_overlay_);
    lv_obj_set_size(title_box, 240, 32);
    lv_obj_align(title_box, LV_ALIGN_TOP_MID, 0, 158);
    lv_obj_set_style_bg_opa(title_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_box, 0, 0);
    lv_obj_set_style_pad_all(title_box, 0, 0);
    lv_obj_remove_flag(title_box, LV_OBJ_FLAG_SCROLLABLE);

    wakeup_title_label_ = lv_label_create(title_box);
    lv_obj_set_style_text_font(wakeup_title_label_, GetMainTextFont16(), 0);
    lv_obj_set_style_text_color(wakeup_title_label_, lv_color_hex(0xE0F2FE), 0);
    lv_label_set_text(wakeup_title_label_, "正在聆听");
    lv_obj_align(wakeup_title_label_, LV_ALIGN_TOP_MID, -12, 0);

    wakeup_icon_label_ = lv_label_create(title_box);
    lv_obj_set_style_text_font(wakeup_icon_label_, &font_material_symbols_16_4, 0);
    lv_obj_set_style_text_color(wakeup_icon_label_, lv_color_hex(0x00D2FF), 0);
    lv_label_set_text(wakeup_icon_label_, MATERIAL_SYMBOLS_MIC);
    lv_obj_align_to(wakeup_icon_label_, wakeup_title_label_, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    // 5. 核心互斥区域（频谱跳柱 VS 纯文本展示）
    // 5.1 频谱与分贝区 (VU-Meter)
    wakeup_vu_container_ = lv_obj_create(wakeup_overlay_);
    lv_obj_set_size(wakeup_vu_container_, 240, 72);
    lv_obj_align(wakeup_vu_container_, LV_ALIGN_TOP_MID, 0, 208);
    lv_obj_set_style_bg_opa(wakeup_vu_container_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wakeup_vu_container_, 0, 0);
    lv_obj_set_style_pad_all(wakeup_vu_container_, 0, 0);
    lv_obj_remove_flag(wakeup_vu_container_, LV_OBJ_FLAG_SCROLLABLE);

    int bar_w = 4;
    int bar_gap = 6;
    int total_vu_w = 10 * bar_w + 9 * bar_gap;
    int start_x = (240 - total_vu_w) / 2;

    for (int i = 0; i < 10; ++i) {
        wakeup_vu_bars_[i] = lv_obj_create(wakeup_vu_container_);
        lv_obj_set_size(wakeup_vu_bars_[i], bar_w, 8);
        lv_obj_set_pos(wakeup_vu_bars_[i], start_x + i * (bar_w + bar_gap), 22);
        lv_obj_set_style_radius(wakeup_vu_bars_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(wakeup_vu_bars_[i], lv_color_hex(0x00D2FF), 0);
        lv_obj_set_style_border_width(wakeup_vu_bars_[i], 0, 0);
        lv_obj_remove_flag(wakeup_vu_bars_[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    wakeup_db_label_ = lv_label_create(wakeup_vu_container_);
    lv_obj_set_style_text_font(wakeup_db_label_, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(wakeup_db_label_, lv_color_hex(0x47D6FF), 0);
    lv_label_set_text(wakeup_db_label_, "DECIBEL  -24.6 dB");
    lv_obj_align(wakeup_db_label_, LV_ALIGN_BOTTOM_MID, 0, -2);

    // 5.2 播报文本区（去边框、去背景、纯透明，支持流式长文本平滑滚动）
    wakeup_text_container_ = lv_obj_create(wakeup_overlay_);
    lv_obj_set_size(wakeup_text_container_, 280, 96);
    lv_obj_align(wakeup_text_container_, LV_ALIGN_TOP_MID, 0, 204);
    lv_obj_set_style_bg_opa(wakeup_text_container_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wakeup_text_container_, 0, 0);
    lv_obj_set_style_pad_all(wakeup_text_container_, 0, 0);
    lv_obj_set_scrollbar_mode(wakeup_text_container_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(wakeup_text_container_, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏

    wakeup_text_label_ = lv_label_create(wakeup_text_container_);
    lv_obj_set_width(wakeup_text_label_, 280);
    lv_obj_set_style_text_font(wakeup_text_label_, GetMainTextFont16(), 0);
    lv_label_set_long_mode(wakeup_text_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(wakeup_text_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(wakeup_text_label_, lv_color_hex(0xF1F5F9), 0);
    lv_label_set_text(wakeup_text_label_, "");
    lv_obj_align(wakeup_text_label_, LV_ALIGN_TOP_MID, 0, 0);

    // 6. 底部能量地平线
    lv_obj_t* horizon = lv_obj_create(wakeup_overlay_);
    lv_obj_set_size(horizon, 120, 2);
    lv_obj_align(horizon, LV_ALIGN_TOP_MID, 0, 314);
    lv_obj_set_style_bg_color(horizon, lv_color_hex(0x00D2FF), 0);
    lv_obj_set_style_radius(horizon, 1, 0);
    lv_obj_set_style_shadow_width(horizon, 14, 0);
    lv_obj_set_style_shadow_color(horizon, lv_color_hex(0x00D2FF), 0);
    lv_obj_set_style_shadow_opa(horizon, 220, 0);
    lv_obj_remove_flag(horizon, LV_OBJ_FLAG_CLICKABLE);

    // 8. 40ms 高帧率动态律动定时器 (VU 频谱波形与光晕)
    wakeup_timer_ = lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(t));
            if (self) {
                self->UpdateWakeupVuAnimation();
            }
        },
        40, this);

    // 9. 1s 真实可视倒计时定时器 (驱动 10s 倒计时数字递减与平滑退出)
    countdown_timer_ = lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(t));
            if (self) {
                if (self->auto_hide_seconds_left_ > 0) {
                    self->auto_hide_seconds_left_--;
                    self->UpdateCountdownDisplay();
                    if (self->auto_hide_seconds_left_ <= 0) {
                        self->StopCountdown();
                        self->HideWakeupOverlay();
                    }
                }
            }
        },
        1000, this);
    lv_timer_pause(countdown_timer_);
}

void CustomLcdDisplay::ShowWakeupOverlay() {
    DisplayLockGuard lock(this);
    if (!wakeup_overlay_) {
        return;
    }
    lv_obj_remove_flag(wakeup_overlay_, LV_OBJ_FLAG_HIDDEN);
    if (wakeup_timer_) {
        lv_timer_resume(wakeup_timer_);
    }
}

void CustomLcdDisplay::HideWakeupOverlay() {
    DisplayLockGuard lock(this);
    if (!wakeup_overlay_) {
        return;
    }
    StopCountdown();
    if (wakeup_timer_) {
        lv_timer_pause(wakeup_timer_);
    }
    lv_obj_add_flag(wakeup_overlay_, LV_OBJ_FLAG_HIDDEN);
}

void CustomLcdDisplay::SetupWifiConfigOverlay() {
    if (wifi_config_overlay_)
        return;

    wifi_config_overlay_ = lv_obj_create(lv_layer_top());
    lv_obj_set_size(wifi_config_overlay_, 360, 360);
    lv_obj_align(wifi_config_overlay_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(wifi_config_overlay_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(wifi_config_overlay_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifi_config_overlay_, 0, 0);
    lv_obj_set_style_pad_all(wifi_config_overlay_, 0, 0);
    lv_obj_remove_flag(wifi_config_overlay_, LV_OBJ_FLAG_SCROLLABLE);

    // 背景科技装饰圈
    lv_obj_t* dial_outer = lv_arc_create(wifi_config_overlay_);
    lv_obj_set_size(dial_outer, 350, 350);
    lv_obj_align(dial_outer, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_angles(dial_outer, 0, 360);
    lv_obj_remove_style(dial_outer, nullptr, LV_PART_KNOB);
    lv_obj_remove_style(dial_outer, nullptr, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(dial_outer, 1, LV_PART_MAIN);
    lv_obj_set_style_arc_color(dial_outer, lv_color_hex(0x1C2028), LV_PART_MAIN);
    lv_obj_remove_flag(dial_outer, LV_OBJ_FLAG_CLICKABLE);

    // 1. 顶部图标与标题
    lv_obj_t* wifi_icon = lv_label_create(wifi_config_overlay_);
    lv_obj_set_style_text_font(wifi_icon, &font_material_symbols_30_4, 0);
    lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0x00D2FF), 0);
    lv_label_set_text(wifi_icon, MATERIAL_SYMBOLS_WIFI);
    lv_obj_align(wifi_icon, LV_ALIGN_TOP_MID, 0, 36);

    lv_obj_t* title_lbl = lv_label_create(wifi_config_overlay_);
    lv_obj_set_style_text_font(title_lbl, &font_noto_sans_basic_20_4, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0xE0F2FE), 0);
    lv_label_set_text(title_lbl, "网络配置模式");
    lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 78);

    // 2. 中间信息卡片
    lv_obj_t* card = lv_obj_create(wifi_config_overlay_);
    lv_obj_set_size(card, 280, 160);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 116);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x181C24), 0);
    lv_obj_set_style_bg_opa(card, 220, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x00D2FF), 0);
    lv_obj_set_style_border_opa(card, 120, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* tip_ssid = lv_label_create(card);
    lv_obj_set_style_text_font(tip_ssid, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(tip_ssid, lv_color_hex(0x859399), 0);
    lv_label_set_text(tip_ssid, "1. 手机连接热点");
    lv_obj_align(tip_ssid, LV_ALIGN_TOP_MID, 0, 4);

    wifi_config_ssid_val_ = lv_label_create(card);
    lv_obj_set_style_text_font(wifi_config_ssid_val_, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(wifi_config_ssid_val_, lv_color_hex(0x00D2FF), 0);
    lv_label_set_text(wifi_config_ssid_val_, "Xiaozhi-XXXX");
    lv_obj_align(wifi_config_ssid_val_, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t* tip_url = lv_label_create(card);
    lv_obj_set_style_text_font(tip_url, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(tip_url, lv_color_hex(0x859399), 0);
    lv_label_set_text(tip_url, "2. 浏览器访问后台");
    lv_obj_align(tip_url, LV_ALIGN_TOP_MID, 0, 48);

    wifi_config_url_val_ = lv_label_create(card);
    lv_obj_set_style_text_font(wifi_config_url_val_, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(wifi_config_url_val_, lv_color_hex(0x00D2FF), 0);
    lv_label_set_text(wifi_config_url_val_, "http://192.168.4.1");
    lv_obj_align(wifi_config_url_val_, LV_ALIGN_TOP_MID, 0, 68);

    // 配对码徽章容器
    wifi_config_code_box_ = lv_obj_create(card);
    lv_obj_set_size(wifi_config_code_box_, 250, 46);
    lv_obj_align(wifi_config_code_box_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(wifi_config_code_box_, lv_color_hex(0x0E2C24), 0);
    lv_obj_set_style_border_width(wifi_config_code_box_, 1, 0);
    lv_obj_set_style_border_color(wifi_config_code_box_, lv_color_hex(0x4EDEA3), 0);
    lv_obj_set_style_radius(wifi_config_code_box_, 8, 0);
    lv_obj_set_style_pad_all(wifi_config_code_box_, 0, 0);
    lv_obj_remove_flag(wifi_config_code_box_, LV_OBJ_FLAG_SCROLLABLE);

    wifi_config_code_val_ = lv_label_create(wifi_config_code_box_);
    lv_obj_set_style_text_font(wifi_config_code_val_, &font_noto_sans_basic_16_4, 0);
    lv_obj_set_style_text_color(wifi_config_code_val_, lv_color_hex(0x4EDEA3), 0);
    lv_label_set_text(wifi_config_code_val_, "配对码: ------");
    lv_obj_align(wifi_config_code_val_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(wifi_config_code_box_, LV_OBJ_FLAG_HIDDEN);  // 默认隐藏

    // 3. 底部提示
    lv_obj_t* foot_lbl = lv_label_create(wifi_config_overlay_);
    lv_obj_set_style_text_font(foot_lbl, &font_noto_sans_basic_16_4, 0);
    lv_obj_set_style_text_color(foot_lbl, lv_color_hex(0x859399), 0);
    lv_label_set_text(foot_lbl, "配置完成后将自动连接");
    lv_obj_align(foot_lbl, LV_ALIGN_TOP_MID, 0, 290);

    lv_obj_add_flag(wifi_config_overlay_, LV_OBJ_FLAG_HIDDEN);
}

void CustomLcdDisplay::ShowWifiConfigOverlay(const std::string& ssid, const std::string& url,
                                             const std::string& code) {
    DisplayLockGuard lock(this);
    if (!wifi_config_overlay_) {
        SetupWifiConfigOverlay();
    }
    HideWakeupOverlay();

    std::string final_ssid = ssid;
    if (final_ssid.empty()) {
        final_ssid = WifiManager::GetInstance().GetApSsid();
    }
    if (final_ssid.empty()) {
        final_ssid = "Xiaozhi-1.85C";
    }
    if (wifi_config_ssid_val_) {
        lv_label_set_text(wifi_config_ssid_val_, final_ssid.c_str());
    }
    if (wifi_config_url_val_) {
        std::string final_url = url.empty() ? "http://192.168.4.1" : url;
        lv_label_set_text(wifi_config_url_val_, final_url.c_str());
    }
    if (!code.empty() && wifi_config_code_val_ && wifi_config_code_box_) {
        lv_label_set_text(wifi_config_code_val_, code.c_str());
        lv_obj_remove_flag(wifi_config_code_box_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(wifi_config_overlay_, LV_OBJ_FLAG_HIDDEN);
}

void CustomLcdDisplay::HideWifiConfigOverlay() {
    DisplayLockGuard lock(this);
    if (wifi_config_overlay_) {
        lv_obj_add_flag(wifi_config_overlay_, LV_OBJ_FLAG_HIDDEN);
    }
}

void CustomLcdDisplay::UpdateWakeupVuAnimation() {
    if (!wakeup_overlay_ || lv_obj_has_flag(wakeup_overlay_, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    static float phase = 0.0f;
    static int blink_counter = 0;
    phase += 0.12f;
    blink_counter++;

    // 虚拟发光 LED 双眼微动效 (极其轻量化)
    if (led_eye_left_ && led_eye_right_) {
        bool is_speaking = (wakeup_text_container_ &&
                            !lv_obj_has_flag(wakeup_text_container_, LV_OBJ_FLAG_HIDDEN));
        if (is_speaking) {
            // 播报时：双眼随着语音节奏在 24 ~ 36px 之间自然跃动
            int eye_h = (int)(30.0f + sinf(phase * 2.2f) * 5.0f);
            lv_obj_set_height(led_eye_left_, eye_h);
            lv_obj_set_height(led_eye_right_, eye_h);
            lv_obj_set_y(led_eye_left_, 14 + (36 - eye_h) / 2);
            lv_obj_set_y(led_eye_right_, 14 + (36 - eye_h) / 2);
        } else {
            // 聆听时：双眼专注睁大，每隔约 80 帧 (约 3.2s) 自然眨眼
            int eye_h = 36;
            if (blink_counter % 80 < 4) {
                eye_h = 6;
            }
            lv_obj_set_height(led_eye_left_, eye_h);
            lv_obj_set_height(led_eye_right_, eye_h);
            lv_obj_set_y(led_eye_left_, 14 + (36 - eye_h) / 2);
            lv_obj_set_y(led_eye_right_, 14 + (36 - eye_h) / 2);
        }
    }

    // 如果当前切到了文本播报模式（vu_container 隐藏），暂停频谱计算
    if (wakeup_vu_container_ && !lv_obj_has_flag(wakeup_vu_container_, LV_OBJ_FLAG_HIDDEN)) {
        float base = sinf(phase) * 0.4f + 0.6f;
        for (int i = 0; i < 10; ++i) {
            if (!wakeup_vu_bars_[i])
                continue;
            float noise = sinf(phase * 2.0f + i * 0.8f) * 0.5f + 0.5f;
            int h = (int)(noise * base * 16.0f);
            if (h < 4)
                h = 4;
            if (h > 20)
                h = 20;

            lv_obj_set_height(wakeup_vu_bars_[i], h);
            lv_obj_set_y(wakeup_vu_bars_[i], 32 - h);

            if (h > 14) {
                lv_obj_set_style_bg_color(wakeup_vu_bars_[i], lv_color_hex(0x00D2FF), 0);
            } else if (h > 8) {
                lv_obj_set_style_bg_color(wakeup_vu_bars_[i], lv_color_hex(0x4EDEA3), 0);
            } else {
                lv_obj_set_style_bg_color(wakeup_vu_bars_[i], lv_color_hex(0x31353E), 0);
            }
        }
    }

    // 检查语音活动：待机等待期用户直接开口说话时，倒计时立即停止、HUD 不该消失。
    // 但必须等倒计时真的走起来（已经递减过至少一秒）才允许取消：
    // 对话刚结束时 EnableWakeWordDetection 会立刻启用唤醒词引擎，麦克风里还回响着
    // 助手最后那句 TTS 的尾音，VAD 随即报 speaking=true，会把刚启动的倒计时误杀，
    // 于是屏幕永远停在「回答完毕」不再自动退出。
    if (auto_hide_seconds_left_ > 0 && auto_hide_seconds_left_ < countdown_start_seconds_ &&
        !voice_input_detected_) {
        if (Application::GetInstance().GetAudioService().IsVoiceDetected()) {
            voice_input_detected_ = true;
            StopCountdown();
        }
    }
}

void CustomLcdDisplay::StartCountdown(int seconds) {
    auto_hide_seconds_left_ = seconds > 0 ? seconds : 10;
    countdown_start_seconds_ = auto_hide_seconds_left_;
    if (countdown_timer_) {
        lv_timer_reset(countdown_timer_);
        lv_timer_resume(countdown_timer_);
    }
    UpdateCountdownDisplay();
}

void CustomLcdDisplay::StopCountdown() {
    if (countdown_timer_) {
        lv_timer_pause(countdown_timer_);
    }
    auto_hide_seconds_left_ = 0;
    countdown_start_seconds_ = 0;
    UpdateCountdownDisplay();
}

void CustomLcdDisplay::UpdateCountdownDisplay() {
    if (!wakeup_title_label_) {
        return;
    }
    std::string text = current_title_base_;
    if (auto_hide_seconds_left_ > 0) {
        text += " (" + std::to_string(auto_hide_seconds_left_) + "s)";
    }
    lv_label_set_text(wakeup_title_label_, text.c_str());
    lv_obj_align(wakeup_title_label_, LV_ALIGN_TOP_MID, -12, 0);
    if (wakeup_icon_label_) {
        lv_obj_align_to(wakeup_icon_label_, wakeup_title_label_, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    }
}

void CustomLcdDisplay::SetStatus(const char* status) {
    DisplayLockGuard lock(this);
    if (status_bar_) {
        lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    if (status_label_) {
        lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    }

    if (!status || !wakeup_overlay_) {
        return;
    }

    std::string s(status);
    if (s == Lang::Strings::WIFI_CONFIG_MODE || s.find("配置") != std::string::npos ||
        s.find("Config") != std::string::npos || s == Lang::Strings::ACTIVATION) {
        ShowWifiConfigOverlay();
        return;
    }

    // 若退出配网进入正常待命状态，隐藏配网层
    if (s == Lang::Strings::STANDBY || s.find("Standby") != std::string::npos ||
        s.find("待命") != std::string::npos) {
        HideWifiConfigOverlay();
    }

    // 关键修复：仅在真正进入语音拾音或连接状态时才停止音乐，严禁将时间刷新字符串（如"21:55"）误判为活跃状态！
    bool is_voice_wake = (s == Lang::Strings::LISTENING || s == Lang::Strings::CONNECTING ||
                          s.find("正在聆听") != std::string::npos || s.find("Listening") != std::string::npos ||
                          s.find("连接中") != std::string::npos || s.find("Connecting") != std::string::npos);
    if (is_voice_wake) {
#if CONFIG_WS185C_ENABLE_NAVIDROME
        if (is_playing_) {
            ESP_LOGI(TAG, "Voice assistant awakened [%s], auto-stopping music stream", status);
            StopNavidromeStream();
        }
#endif
    }

    if (s == Lang::Strings::LISTENING || s.find("Listening") != std::string::npos ||
        s.find("正在聆听") != std::string::npos) {
        HideWifiConfigOverlay();
        ShowWakeupOverlay();
        is_new_assistant_turn_ = true;
        voice_input_detected_ = false;

        current_title_base_ = "正在聆听";
        const lv_font_t* f16 = GetMainTextFont16();
        if (wakeup_title_label_) {
            lv_obj_set_style_text_font(wakeup_title_label_, f16, 0);
        }
        if (wakeup_icon_label_) {
            lv_obj_set_style_text_color(wakeup_icon_label_, lv_color_hex(0x00D2FF), 0);
            lv_label_set_text(wakeup_icon_label_, MATERIAL_SYMBOLS_MIC);
        }
        StartCountdown(10);

        if (led_eye_left_ && led_eye_right_) {
            lv_obj_set_style_bg_color(led_eye_left_, lv_color_hex(0x00D2FF), 0);
            lv_obj_set_style_shadow_color(led_eye_left_, lv_color_hex(0x00D2FF), 0);
            lv_obj_set_style_bg_color(led_eye_right_, lv_color_hex(0x00D2FF), 0);
            lv_obj_set_style_shadow_color(led_eye_right_, lv_color_hex(0x00D2FF), 0);
        }
        if (wakeup_vu_container_) {
            lv_obj_remove_flag(wakeup_vu_container_, LV_OBJ_FLAG_HIDDEN);
        }
        if (wakeup_text_container_) {
            lv_obj_add_flag(wakeup_text_container_, LV_OBJ_FLAG_HIDDEN);
        }
        // BOOT 键是显式意图，不做起始静默倒计时；否则音频通道还没握手完
        // HUD 就消失了，用户会以为没收音到
        if (countdown_enabled_) {
            StartCountdown(10);
        } else {
            StopCountdown();
        }
    } else if (s.find("Thinking") != std::string::npos || s.find("思考") != std::string::npos) {
        ShowWakeupOverlay();
        voice_input_detected_ = true;
        StopCountdown();
        current_title_base_ = "小智思考中";
        const lv_font_t* f16 = GetMainTextFont16();
        if (wakeup_title_label_) {
            lv_obj_set_style_text_font(wakeup_title_label_, f16, 0);
        }
        if (wakeup_icon_label_) {
            lv_obj_set_style_text_color(wakeup_icon_label_, lv_color_hex(0x38BDF8), 0);
            lv_label_set_text(wakeup_icon_label_, MATERIAL_SYMBOLS_PROGRESS_ACTIVITY);
        }
        UpdateCountdownDisplay();

        if (led_eye_left_ && led_eye_right_) {
            lv_obj_set_style_bg_color(led_eye_left_, lv_color_hex(0x38BDF8), 0);
            lv_obj_set_style_shadow_color(led_eye_left_, lv_color_hex(0x38BDF8), 0);
            lv_obj_set_style_bg_color(led_eye_right_, lv_color_hex(0x38BDF8), 0);
            lv_obj_set_style_shadow_color(led_eye_right_, lv_color_hex(0x38BDF8), 0);
        }
    } else if (s == Lang::Strings::SPEAKING || s.find("Speaking") != std::string::npos ||
               s.find("正在说话") != std::string::npos || s.find("正在播报") != std::string::npos) {
        ShowWakeupOverlay();
        voice_input_detected_ = true;
        StopCountdown();
        current_title_base_ = "正在回答";
        const lv_font_t* f16 = GetMainTextFont16();
        if (wakeup_title_label_) {
            lv_obj_set_style_text_font(wakeup_title_label_, f16, 0);
        }
        if (wakeup_text_label_) {
            lv_obj_set_style_text_font(wakeup_text_label_, f16, 0);
        }
        if (wakeup_icon_label_) {
            lv_obj_set_style_text_color(wakeup_icon_label_, lv_color_hex(0x4EDEA3), 0);
            lv_label_set_text(wakeup_icon_label_, MATERIAL_SYMBOLS_VOLUME_UP);
        }
        UpdateCountdownDisplay();

        if (led_eye_left_ && led_eye_right_) {
            lv_obj_set_style_bg_color(led_eye_left_, lv_color_hex(0x4EDEA3), 0);
            lv_obj_set_style_shadow_color(led_eye_left_, lv_color_hex(0x4EDEA3), 0);
            lv_obj_set_style_bg_color(led_eye_right_, lv_color_hex(0x4EDEA3), 0);
            lv_obj_set_style_shadow_color(led_eye_right_, lv_color_hex(0x4EDEA3), 0);
        }
        if (wakeup_vu_container_) {
            lv_obj_add_flag(wakeup_vu_container_, LV_OBJ_FLAG_HIDDEN);
        }
        if (wakeup_text_container_) {
            lv_obj_remove_flag(wakeup_text_container_, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (s == Lang::Strings::STANDBY || s.find("Standby") != std::string::npos ||
               s.find("待命") != std::string::npos) {
        current_title_base_ = "回答完毕";
        const lv_font_t* f16 = GetMainTextFont16();
        if (wakeup_title_label_) {
            lv_obj_set_style_text_font(wakeup_title_label_, f16, 0);
        }
        if (wakeup_text_label_) {
            lv_obj_set_style_text_font(wakeup_text_label_, f16, 0);
        }
        if (wakeup_icon_label_) {
            lv_obj_set_style_text_color(wakeup_icon_label_, lv_color_hex(0x94A3B8), 0);
            lv_label_set_text(wakeup_icon_label_, MATERIAL_SYMBOLS_CHECK_CIRCLE);
        }
        voice_input_detected_ = false;
        // 回到待机即本轮对话结束，无论上一轮是按键还是唤醒词唤醒，
        // 都恢复唤醒词路径需要的静默倒计时
        countdown_enabled_ = true;
        // 倒计时的实际启动统一交给 ClearChatMessages()（application.cc 的 idle
        // 分支紧接着就会调它）。这里不再重复 StartCountdown(10)，否则倒计时会被
        // 重置两次，改动时容易只改到一处。

        if (led_eye_left_ && led_eye_right_) {
            lv_obj_set_style_bg_color(led_eye_left_, lv_color_hex(0x859399), 0);
            lv_obj_set_style_shadow_color(led_eye_left_, lv_color_hex(0x859399), 0);
            lv_obj_set_style_bg_color(led_eye_right_, lv_color_hex(0x859399), 0);
            lv_obj_set_style_shadow_color(led_eye_right_, lv_color_hex(0x859399), 0);
        }
    }
}

void CustomLcdDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (!content) {
        return;
    }

    std::string r = role ? role : "";
    std::string text = content;

    if (r == "system") {
        if (!text.empty()) {
            if (text.find("192.168.") != std::string::npos ||
                text.find("Xiaozhi") != std::string::npos ||
                text.find("热点") != std::string::npos || text.find("http") != std::string::npos) {
                ShowWifiConfigOverlay("", "", "");
            } else if (text.find("验证码") != std::string::npos ||
                       text.find("激活码") != std::string::npos ||
                       text.find("Code") != std::string::npos) {
                ShowWifiConfigOverlay("", "", text);
            }
        }
        return;
    }
    if (text.empty() || !wakeup_overlay_) {
        return;
    }

    ShowWakeupOverlay();

    if (r == "user") {
        voice_input_detected_ = true;
        StopCountdown();
        assistant_stream_text_.clear();
        is_new_assistant_turn_ = true;

        current_title_base_ = "我";
        const lv_font_t* f16 = GetMainTextFont16();
        if (wakeup_title_label_) {
            lv_obj_set_style_text_font(wakeup_title_label_, f16, 0);
        }
        if (wakeup_icon_label_) {
            lv_obj_set_style_text_color(wakeup_icon_label_, lv_color_hex(0x00D2FF), 0);
            lv_label_set_text(wakeup_icon_label_, MATERIAL_SYMBOLS_PERSON);
        }
        UpdateCountdownDisplay();

        if (wakeup_vu_container_) {
            lv_obj_add_flag(wakeup_vu_container_, LV_OBJ_FLAG_HIDDEN);
        }
        if (wakeup_text_container_) {
            lv_obj_remove_flag(wakeup_text_container_, LV_OBJ_FLAG_HIDDEN);
        }
        if (wakeup_text_label_) {
            lv_obj_set_style_text_font(wakeup_text_label_, f16, 0);
            std::string user_clean = SanitizeDisplayText(text);
            lv_label_set_text(wakeup_text_label_, user_clean.c_str());
            lv_obj_scroll_to_y(wakeup_text_container_, 0, LV_ANIM_OFF);
        }
    } else if (r == "assistant") {
        voice_input_detected_ = true;
        StopCountdown();
        current_title_base_ = "正在回答";
        const lv_font_t* f16 = GetMainTextFont16();
        if (wakeup_title_label_) {
            lv_obj_set_style_text_font(wakeup_title_label_, f16, 0);
        }
        if (wakeup_text_label_) {
            lv_obj_set_style_text_font(wakeup_text_label_, f16, 0);
        }
        if (wakeup_icon_label_) {
            lv_obj_set_style_text_color(wakeup_icon_label_, lv_color_hex(0x4EDEA3), 0);
            lv_label_set_text(wakeup_icon_label_, MATERIAL_SYMBOLS_VOLUME_UP);
        }
        UpdateCountdownDisplay();

        if (wakeup_vu_container_) {
            lv_obj_add_flag(wakeup_vu_container_, LV_OBJ_FLAG_HIDDEN);
        }
        if (wakeup_text_container_) {
            lv_obj_remove_flag(wakeup_text_container_, LV_OBJ_FLAG_HIDDEN);
        }

        std::string clean_new = SanitizeDisplayText(text);
        if (!clean_new.empty()) {
            if (is_new_assistant_turn_) {
                assistant_stream_text_ = clean_new;
                is_new_assistant_turn_ = false;
            } else {
                // 1. 检查是否为前缀全量累加推送（避免整句重复拼接产生大量逗号）
                if (clean_new.rfind(assistant_stream_text_, 0) == 0) {
                    assistant_stream_text_ = clean_new;
                } else if (assistant_stream_text_.find(clean_new) != std::string::npos) {
                    // 已包含，无需重复追加
                } else {
                    // 2. 增量句段合并
                    assistant_stream_text_ += clean_new;
                }
            }
            assistant_stream_text_ = SanitizeDisplayText(assistant_stream_text_);
        }

        if (wakeup_text_label_) {
            lv_label_set_text(wakeup_text_label_, assistant_stream_text_.c_str());
            // 自动向下滚动，保证最新句段呈现在视野中（使用 LV_ANIM_OFF 消除动画对音频解码与 SPI 总线的抢占）
            lv_obj_update_layout(wakeup_text_label_);
            lv_coord_t label_h = lv_obj_get_height(wakeup_text_label_);
            lv_coord_t cont_h = lv_obj_get_height(wakeup_text_container_);
            if (label_h > cont_h) {
                lv_obj_scroll_to_y(wakeup_text_container_, label_h - cont_h, LV_ANIM_OFF);
            }
        }
    }
}

void CustomLcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    is_new_assistant_turn_ = true;
    current_title_base_ = "回答完毕";
    const lv_font_t* f16 = GetMainTextFont16();
    if (wakeup_title_label_) {
        lv_obj_set_style_text_font(wakeup_title_label_, f16, 0);
    }
    if (wakeup_text_label_) {
        lv_obj_set_style_text_font(wakeup_text_label_, f16, 0);
    }
    if (wakeup_icon_label_) {
        lv_obj_set_style_text_color(wakeup_icon_label_, lv_color_hex(0x94A3B8), 0);
        lv_label_set_text(wakeup_icon_label_, MATERIAL_SYMBOLS_CHECK_CIRCLE);
    }
    voice_input_detected_ = false;
    countdown_enabled_ = true;
    StartCountdown(10);
}

void CustomLcdDisplay::SetupHomeDashboardUI() {
    auto screen = lv_screen_active();

    // 隐藏默认表情
    if (emoji_label_) {
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_image_) {
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }

    // 核心满屏看板容器：360x360，全屏布局
    home_dashboard_ = lv_obj_create(screen);
    lv_obj_set_size(home_dashboard_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(home_dashboard_, 0, 0);
    lv_obj_set_style_bg_opa(home_dashboard_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(home_dashboard_, 0, 0);
    lv_obj_set_style_pad_all(home_dashboard_, 0, 0);
    lv_obj_set_scrollbar_mode(home_dashboard_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(home_dashboard_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(home_dashboard_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(home_dashboard_, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(home_dashboard_, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // 外圈赛博发光微弧线 (Cyan Top Arc)
    lv_obj_t* top_arc = lv_arc_create(home_dashboard_);
    lv_obj_set_size(top_arc, 348, 348);
    lv_obj_align(top_arc, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_angles(top_arc, 220, 320);
    lv_arc_set_bg_angles(top_arc, 220, 320);
    lv_obj_remove_style(top_arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(top_arc, 2, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(top_arc, lv_color_hex(0x00D2FF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(top_arc, LV_OPA_70, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(top_arc, 0, LV_PART_MAIN);
    lv_obj_remove_flag(top_arc, LV_OBJ_FLAG_CLICKABLE);

    // ========================================================
    // 上半部分：极简通透大字时间 (y: 28 ~ 130)
    // ========================================================
    home_hud_box_ = lv_obj_create(home_dashboard_);
    lv_obj_set_size(home_hud_box_, 300, 100);
    lv_obj_align(home_hud_box_, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_opa(home_hud_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(home_hud_box_, 0, 0);
    lv_obj_set_style_pad_all(home_hud_box_, 0, 0);
    lv_obj_set_flex_flow(home_hud_box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(home_hud_box_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(home_hud_box_, 4, 0);
    lv_obj_set_scrollbar_mode(home_hud_box_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(home_hud_box_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(home_hud_box_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(home_hud_box_, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(home_hud_box_, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // 大号时间行：30px 饱满无衬线纯白时间 + 浅青色秒数
    lv_obj_t* time_row = lv_obj_create(home_hud_box_);
    lv_obj_set_size(time_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(time_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(time_row, 0, 0);
    lv_obj_set_style_pad_all(time_row, 0, 0);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(time_row, 6, 0);
    lv_obj_remove_flag(time_row, LV_OBJ_FLAG_CLICKABLE);

    home_time_label_ = lv_label_create(time_row);
    lv_obj_set_style_text_font(home_time_label_, &font_maison_neue_book_26, 0);
    lv_obj_set_style_text_color(home_time_label_, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(home_time_label_, "13:31");
    lv_obj_remove_flag(home_time_label_, LV_OBJ_FLAG_CLICKABLE);

    home_sec_label_ = lv_label_create(time_row);
    lv_obj_set_style_text_font(home_sec_label_, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(home_sec_label_, lv_color_hex(0x00D2FF), 0);
    lv_obj_set_style_pad_bottom(home_sec_label_, 2, 0);
    lv_label_set_text(home_sec_label_, "00s");
    lv_obj_remove_flag(home_sec_label_, LV_OBJ_FLAG_CLICKABLE);

    // 优雅日期行：10月2日 · 星期五 (FRI)
    home_date_label_ = lv_label_create(home_hud_box_);
    lv_obj_set_style_text_color(home_date_label_, lv_color_hex(0x859399), 0);
    lv_label_set_text(home_date_label_, "10月2日 · 星期五 (FRI)");
    lv_obj_remove_flag(home_date_label_, LV_OBJ_FLAG_CLICKABLE);

    // 中间微光水平分割线（宽 260px，高 1px）
    lv_obj_t* mid_div = lv_obj_create(home_dashboard_);
    lv_obj_set_size(mid_div, 260, 1);
    lv_obj_align(mid_div, LV_ALIGN_TOP_MID, 0, 134);
    lv_obj_set_style_bg_color(mid_div, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_border_width(mid_div, 0, 0);
    lv_obj_set_style_pad_all(mid_div, 0, 0);
    lv_obj_remove_flag(mid_div, LV_OBJ_FLAG_CLICKABLE);

    // ========================================================
    // 下半部分：宽幅舒展股票卡片区 (支持 联想、NVDA、QQQ、AAPL、GOOGL 自动轮播)
    // ========================================================
    stock_card_ = lv_obj_create(home_dashboard_);
    lv_obj_set_size(stock_card_, 300, 136);
    lv_obj_align(stock_card_, LV_ALIGN_TOP_MID, 0, 150);
    lv_obj_set_style_bg_opa(stock_card_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(stock_card_, 0, 0);
    lv_obj_set_style_pad_all(stock_card_, 0, 0);
    lv_obj_set_flex_flow(stock_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(stock_card_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(stock_card_, 12, 0);
    lv_obj_set_scrollbar_mode(stock_card_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(stock_card_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(stock_card_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(stock_card_, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(stock_card_, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // A. 标的代码与名称：联想集团 00992.HK (居中展示)
    lv_obj_t* title_box = lv_obj_create(stock_card_);
    lv_obj_set_size(title_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(title_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_box, 0, 0);
    lv_obj_set_style_pad_all(title_box, 0, 0);
    lv_obj_set_flex_flow(title_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(title_box, 8, 0);
    lv_obj_remove_flag(title_box, LV_OBJ_FLAG_CLICKABLE);

    stock_name_label_ = lv_label_create(title_box);
    lv_obj_set_style_text_color(stock_name_label_, lv_color_hex(0xDFE2EE), 0);
    lv_label_set_text(stock_name_label_, "联想集团");
    lv_obj_remove_flag(stock_name_label_, LV_OBJ_FLAG_CLICKABLE);

    stock_code_label_ = lv_label_create(title_box);
    lv_obj_set_style_text_font(stock_code_label_, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(stock_code_label_, lv_color_hex(0x38BDF8), 0);
    lv_label_set_text(stock_code_label_, "00992.HK");
    lv_obj_remove_flag(stock_code_label_, LV_OBJ_FLAG_CLICKABLE);

    // B. 大号现价与涨跌幅
    lv_obj_t* price_row = lv_obj_create(stock_card_);
    lv_obj_set_size(price_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(price_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(price_row, 0, 0);
    lv_obj_set_style_pad_all(price_row, 0, 0);
    lv_obj_set_flex_flow(price_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(price_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(price_row, 12, 0);
    lv_obj_remove_flag(price_row, LV_OBJ_FLAG_CLICKABLE);

    stock_price_label_ = lv_label_create(price_row);
    lv_obj_set_style_text_font(stock_price_label_, &font_maison_neue_book_26, 0);
    lv_obj_set_style_text_color(stock_price_label_, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(stock_price_label_, "HK$ 34.12");
    lv_obj_remove_flag(stock_price_label_, LV_OBJ_FLAG_CLICKABLE);

    // 涨跌幅副级字（统一 Maison Neue 14px 科技感字体，红涨绿跌）
    stock_change_badge_ = lv_obj_create(price_row);
    lv_obj_set_size(stock_change_badge_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(stock_change_badge_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(stock_change_badge_, 0, 0);
    lv_obj_set_style_pad_all(stock_change_badge_, 0, 0);
    lv_obj_remove_flag(stock_change_badge_, LV_OBJ_FLAG_CLICKABLE);

    stock_change_label_ = lv_label_create(stock_change_badge_);
    lv_obj_set_style_text_font(stock_change_label_, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(stock_change_label_, lv_color_hex(0xEF4444), 0);
    lv_label_set_text(stock_change_label_, "-1.22% (-0.42)");
    lv_obj_remove_flag(stock_change_label_, LV_OBJ_FLAG_CLICKABLE);

    // C. 区间直接展示（统一 Maison Neue 14px 字体）
    stock_range_label_ = lv_label_create(stock_card_);
    lv_obj_set_style_text_font(stock_range_label_, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(stock_range_label_, lv_color_hex(0x859399), 0);
    lv_label_set_text(stock_range_label_, "33.80 - 35.24");
    lv_obj_remove_flag(stock_range_label_, LV_OBJ_FLAG_CLICKABLE);

    // 初始化默认 5 支股票轮播列表（开箱即用）
    if (stock_list_.empty()) {
        stock_list_ = {{"00992.HK", "联想集团", "LNVGY", "HK$ ", "34.32", "-0.64% (-0.22)",
                        "(-0.22)", "33.74 - 35.24", "42.8M", false, true, true},
                       {"NVDA", "英伟达", "NVDA", "$ ", "230.86", "+1.09% (+2.48)", "(+2.48)",
                        "228.16 - 232.29", "98.5M", true, false, true},
                       {"QQQ", "纳指100", "QQQ", "$ ", "742.03", "+0.31% (+2.26)", "(+2.26)",
                        "736.25 - 744.67", "35.7M", true, false, true},
                       {"AAPL", "苹果", "AAPL", "$ ", "330.32", "-0.81% (-2.70)", "(-2.70)",
                        "325.81 - 332.48", "36.3M", false, true, true},
                       {"GOOGL", "谷歌", "GOOGL", "$ ", "338.24", "-1.70% (-5.84)", "(-5.84)",
                        "335.51 - 353.22", "33.2M", false, true, true}};
    }
    ApplyStockUI(stock_list_[0]);

    UpdateHomeClock();
}

void CustomLcdDisplay::UpdateHomeClock() {
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    bool in_config = (state == kDeviceStateWifiConfiguring);

    // 状态切换时动态适配 UI，确保配网提示（热点名、齿轮、IP）完整可见且零干扰
    if (in_config != in_config_mode_cached_) {
        in_config_mode_cached_ = in_config;
        if (in_config) {
            // 配网模式：隐藏股票看板与播放器，显露基类表情与提示，显示基类状态栏
            if (home_dashboard_)
                lv_obj_add_flag(home_dashboard_, LV_OBJ_FLAG_HIDDEN);
#if CONFIG_WS185C_ENABLE_NAVIDROME
            if (player_overlay_)
                lv_obj_add_flag(player_overlay_, LV_OBJ_FLAG_HIDDEN);
#endif
            if (weather_overlay_)
                lv_obj_add_flag(weather_overlay_, LV_OBJ_FLAG_HIDDEN);
            if (settings_overlay_)
                lv_obj_add_flag(settings_overlay_, LV_OBJ_FLAG_HIDDEN);
#if CONFIG_WS185C_ENABLE_BESZEL
            if (server_overlay_)
                lv_obj_add_flag(server_overlay_, LV_OBJ_FLAG_HIDDEN);
#endif
            if (emoji_label_)
                lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            if (status_bar_)
                lv_obj_remove_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
            if (indicator_container_)
                lv_obj_add_flag(indicator_container_, LV_OBJ_FLAG_HIDDEN);
            return;
        } else {
            // 退出配网/进入待机：恢复股票时间看板，隐藏基类状态栏与表情
            if (home_dashboard_)
                lv_obj_remove_flag(home_dashboard_, LV_OBJ_FLAG_HIDDEN);
            if (emoji_label_)
                lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            if (status_bar_)
                lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
            if (indicator_container_)
                lv_obj_remove_flag(indicator_container_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (in_config || !home_time_label_)
        return;

    bool is_busy = (state != kDeviceStateIdle);
#if CONFIG_WS185C_ENABLE_NAVIDROME
    if (is_playing_) {
        is_busy = true;
    }
#endif

    // 如果处于对话中（listening/speaking/connecting 等）或正在播放音乐：
    // 全面冻结底层的 UI 刷新、轮播和数据拉取，把全部 CPU/网络资源让渡给音频流！
    if (is_busy) {
        return;
    }

    time_t now = time(NULL);
    struct tm* tm_now = localtime(&now);

    if (tm_now && tm_now->tm_year >= (2025 - 1900)) {
        char time_buf[16];
        snprintf(time_buf, sizeof(time_buf), "%02d:%02d", tm_now->tm_hour, tm_now->tm_min);
        lv_label_set_text(home_time_label_, time_buf);

        char sec_buf[16];
        snprintf(sec_buf, sizeof(sec_buf), "%02ds", tm_now->tm_sec);
        lv_label_set_text(home_sec_label_, sec_buf);

        static const char* const weekdays_cn[] = {"星期日", "星期一", "星期二", "星期三",
                                                  "星期四", "星期五", "星期六"};
        static const char* const weekdays_en[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
        char date_buf[64];
        snprintf(date_buf, sizeof(date_buf), "%d月%d日 · %s (%s)", tm_now->tm_mon + 1,
                 tm_now->tm_mday, weekdays_cn[tm_now->tm_wday], weekdays_en[tm_now->tm_wday]);
        lv_label_set_text(home_date_label_, date_buf);
    }

    // 股票轮播：每 4 秒轮换展示下一只股票（5 支股票 20 秒循环一轮）
    if (!stock_list_.empty()) {
        if (++stock_carousel_counter_ >= 4) {
            stock_carousel_counter_ = 0;
            current_stock_idx_ = (current_stock_idx_ + 1) % stock_list_.size();
            ApplyStockUI(stock_list_[current_stock_idx_]);
        }
    }

#if CONFIG_WS185C_ENABLE_BESZEL
    // VPS 自动轮播：根据缓存的配置间隔轮播（免每秒读 NVS）
    if (!vps_nodes_.empty()) {
        int rotate_interval = beszel_rotate_interval_s_ > 0 ? beszel_rotate_interval_s_ : 5;
        if (++vps_carousel_counter_ >= (uint32_t)rotate_interval) {
            vps_carousel_counter_ = 0;
            current_vps_idx_ = (current_vps_idx_ + 1) % vps_nodes_.size();
            if (current_page_ == 3 && server_ui_created_) {
                UpdateServerUI();
            }
        }
    }

    // 定期检查并后台拉取 Beszel 最新数据
    CheckAndTriggerServerFetch();
#endif
}

void CustomLcdDisplay::CheckAndTriggerStockFetch() {
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();

    if (state != kDeviceStateIdle) {
        idle_start_sec_ = 0;
        return;
    }

#if CONFIG_WS185C_ENABLE_NAVIDROME
    if (is_playing_) {
        idle_start_sec_ = 0;
        return;
    }
#endif

    int64_t now_sec = esp_timer_get_time() / 1000000;
    if (idle_start_sec_ == 0) {
        idle_start_sec_ = now_sec;
    }

    // 避开开机握手敏感期
    if (now_sec - idle_start_sec_ < 20) {
        return;
    }

    if (stock_fetching_) {
        return;
    }

    // 每 10 分钟自动刷新一次
    if (last_stock_fetch_sec_ != 0 && (now_sec - last_stock_fetch_sec_) < 600) {
        return;
    }

    last_stock_fetch_sec_ = now_sec;
    stock_fetching_ = true;

    xTaskCreate(
        [](void* arg) {
            auto self = static_cast<CustomLcdDisplay*>(arg);
            self->FetchStockData();
            self->stock_fetching_ = false;
            vTaskDelete(NULL);
        },
        "stock_fetch", 4096, this, 1, nullptr);
}

void CustomLcdDisplay::FetchStockData() {
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if (state != kDeviceStateIdle) {
        return;
    }
#if CONFIG_WS185C_ENABLE_NAVIDROME
    if (is_playing_) {
        return;
    }
#endif

    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    if (!network) {
        return;
    }

    ESP_LOGI(TAG, "Fetching Multi-Stock data (Lenovo, NVDA, QQQ, AAPL, GOOGL)...");
    auto http = network->CreateHttp(0);
    if (!http) {
        return;
    }

    bool success = false;
    std::string body;
    if (http->Open("GET", "http://qt.gtimg.cn/q=r_hk00992,usNVDA,usQQQ,usAAPL,usGOOGL")) {
        auto status_code = http->GetStatusCode();
        if (status_code && *status_code == 200) {
            body = http->ReadAll();
            success = true;
        }
        http->Close();
    }

    if (app.GetDeviceState() != kDeviceStateIdle) {
        return;
    }
#if CONFIG_WS185C_ENABLE_NAVIDROME
    if (is_playing_) {
        return;
    }
#endif

    if (success && !body.empty()) {
        ParseAndApplyStock(body);
    }
}

void CustomLcdDisplay::ParseAndApplyStock(const std::string& body) {
    auto safe_stof = [](const std::string& str, float def_val) -> float {
        if (str.empty())
            return def_val;
        char* endptr = nullptr;
        float val = strtof(str.c_str(), &endptr);
        return (endptr == str.c_str()) ? def_val : val;
    };

    std::vector<StockData> updated_list;
    size_t line_start = 0;
    while (line_start < body.size()) {
        size_t line_end = body.find(';', line_start);
        if (line_end == std::string::npos)
            line_end = body.size();
        std::string line = body.substr(line_start, line_end - line_start);
        line_start = line_end + 1;

        size_t quote_pos = line.find('"');
        if (quote_pos == std::string::npos)
            continue;
        std::string payload = line.substr(quote_pos + 1);
        if (!payload.empty() && payload.back() == '"')
            payload.pop_back();

        std::vector<std::string> parts;
        size_t start = 0;
        while (true) {
            size_t pos = payload.find('~', start);
            if (pos == std::string::npos) {
                parts.push_back(payload.substr(start));
                break;
            }
            parts.push_back(payload.substr(start, pos - start));
            start = pos + 1;
        }

        if (parts.size() >= 35) {
            StockData data;
            std::string raw_code = parts[2];

            if (raw_code == "00992") {
                data.code = "00992.HK";
                data.name = "联想集团";
                data.currency = "HK$ ";
            } else if (raw_code.find("NVDA") != std::string::npos) {
                data.code = "NVDA";
                data.name = "英伟达";
                data.currency = "$ ";
            } else if (raw_code.find("QQQ") != std::string::npos) {
                data.code = "QQQ";
                data.name = "纳指100";
                data.currency = "$ ";
            } else if (raw_code.find("AAPL") != std::string::npos) {
                data.code = "AAPL";
                data.name = "苹果";
                data.currency = "$ ";
            } else if (raw_code.find("GOOGL") != std::string::npos) {
                data.code = "GOOGL";
                data.name = "谷歌";
                data.currency = "$ ";
            } else {
                continue;
            }

            float p = safe_stof(parts[3], 0.0f);
            float chg = safe_stof(parts[31], 0.0f);
            float pct = safe_stof(parts[32], 0.0f);
            float high = safe_stof(parts[33], 0.0f);
            float low = safe_stof(parts[34], 0.0f);

            if (p > 0.01f) {
                char buf[64];
                snprintf(buf, sizeof(buf), "%.2f", p);
                data.price = buf;

                if (chg >= 0.0f) {
                    data.is_up = true;
                    data.is_down = false;
                    snprintf(buf, sizeof(buf), "+%.2f%% (+%.2f)", pct, chg);
                } else {
                    data.is_up = false;
                    data.is_down = true;
                    snprintf(buf, sizeof(buf), "%.2f%% (%.2f)", pct, chg);
                }
                data.change_pct = buf;

                if (low > 0.01f && high > 0.01f) {
                    char range_buf[64];
                    snprintf(range_buf, sizeof(range_buf), "%.2f - %.2f", low, high);
                    data.range = range_buf;
                }
                data.loaded = true;
                updated_list.push_back(data);
            }
        }
    }

    if (!updated_list.empty()) {
        Application::GetInstance().Schedule([this, updated_list]() {
            DisplayLockGuard lock(this);
            this->stock_list_ = updated_list;
            if (this->current_stock_idx_ >= this->stock_list_.size()) {
                this->current_stock_idx_ = 0;
            }
            this->ApplyStockUI(this->stock_list_[this->current_stock_idx_]);
        });
    }
}

void CustomLcdDisplay::ApplyStockUI(const StockData& data) {
    current_stock_ = data;
    if (!stock_price_label_ || !stock_change_label_ || !stock_name_label_ || !stock_code_label_)
        return;

    lv_label_set_text(stock_name_label_, data.name.c_str());
    lv_label_set_text(stock_code_label_, data.code.c_str());

    std::string price_text = data.currency + data.price;
    lv_label_set_text(stock_price_label_, price_text.c_str());
    lv_label_set_text(stock_change_label_, data.change_pct.c_str());
    if (stock_range_label_) {
        lv_label_set_text(stock_range_label_, data.range.c_str());
    }

    // 红涨绿跌
    if (data.is_up) {
        lv_obj_set_style_text_color(stock_change_label_, lv_color_hex(0xEF4444), 0);  // 亮红涨
    } else {
        lv_obj_set_style_text_color(stock_change_label_, lv_color_hex(0x10B981), 0);  // 翠绿跌
    }
}

// ========================================================
// 懒加载：第二屏 Weather Telemetry (宽幅大气天气表盘)
// ========================================================
void CustomLcdDisplay::EnsureWeatherUI() {
    if (weather_ui_created_)
        return;
    weather_ui_created_ = true;

    // 1. 顶部位置标签（单独汉字，无背景胶囊，居中，y: 16）
    weather_loc_label_ = lv_label_create(weather_overlay_);
    lv_label_set_text(weather_loc_label_, current_weather_.city.c_str());
    lv_obj_set_style_text_color(weather_loc_label_, lv_color_hex(0xCBD5E1), 0);
    lv_obj_align(weather_loc_label_, LV_ALIGN_TOP_MID, 0, 16);

    // 2. 天气状况与 AQI 标签行 (y: 42, h: 20)
    lv_obj_t* cond_box = lv_obj_create(weather_overlay_);
    lv_obj_set_size(cond_box, 260, 20);
    lv_obj_align(cond_box, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_set_style_bg_opa(cond_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cond_box, 0, 0);
    lv_obj_set_style_pad_all(cond_box, 0, 0);
    lv_obj_set_flex_flow(cond_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cond_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cond_box, 10, 0);
    lv_obj_remove_flag(cond_box, LV_OBJ_FLAG_SCROLLABLE);

    weather_cond_label_ = lv_label_create(cond_box);
    lv_label_set_text(weather_cond_label_, current_weather_.weather.c_str());
    lv_obj_set_style_text_color(weather_cond_label_, lv_color_hex(0x94A3B8), 0);

    lv_obj_t* aqi_badge = lv_obj_create(cond_box);
    lv_obj_set_size(aqi_badge, 76, 18);
    lv_obj_set_style_radius(aqi_badge, 9, 0);
    lv_obj_set_style_bg_color(aqi_badge, lv_color_hex(0x064E3B), 0);
    lv_obj_set_style_border_width(aqi_badge, 0, 0);
    lv_obj_set_style_pad_all(aqi_badge, 0, 0);
    lv_obj_remove_flag(aqi_badge, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* aqi_lbl = lv_label_create(aqi_badge);
    lv_label_set_text(aqi_lbl, current_weather_.aqi.c_str());
    lv_obj_set_style_text_color(aqi_lbl, lv_color_hex(0x34D399), 0);
    lv_obj_center(aqi_lbl);

    // 3. 中央超大主温度与湿度整合 (y: 68, h: 44)
    lv_obj_t* temp_box = lv_obj_create(weather_overlay_);
    lv_obj_set_size(temp_box, 240, 44);
    lv_obj_align(temp_box, LV_ALIGN_TOP_MID, 0, 68);
    lv_obj_set_style_bg_opa(temp_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(temp_box, 0, 0);
    lv_obj_set_style_pad_all(temp_box, 0, 0);
    lv_obj_set_flex_flow(temp_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(temp_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(temp_box, 4, 0);
    lv_obj_remove_flag(temp_box, LV_OBJ_FLAG_SCROLLABLE);

    weather_temp_label_ = lv_label_create(temp_box);
    lv_label_set_text(weather_temp_label_, current_weather_.temp.c_str());
    lv_obj_set_style_text_font(weather_temp_label_, &font_maison_neue_book_26, 0);
    lv_obj_set_style_text_color(weather_temp_label_, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t* deg_c = lv_label_create(temp_box);
    lv_label_set_text(deg_c, "°C");
    lv_obj_set_style_text_font(deg_c, &font_noto_sans_basic_20_4, 0);
    lv_obj_set_style_text_color(deg_c, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_pad_bottom(deg_c, 4, 0);

    weather_hum_label_ = lv_label_create(temp_box);
    std::string hum_str = "湿度 " + current_weather_.humidity;
    lv_label_set_text(weather_hum_label_, hum_str.c_str());
    lv_obj_set_style_text_color(weather_hum_label_, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_pad_bottom(weather_hum_label_, 6, 0);
    lv_obj_set_style_margin_left(weather_hum_label_, 10, 0);

    // 4. 三联环境遥测卡片 (y: 122, h: 66，已移除风向、紫外线、气压汉字)
    lv_obj_t* trio_box = lv_obj_create(weather_overlay_);
    lv_obj_set_size(trio_box, 286, 66);
    lv_obj_align(trio_box, LV_ALIGN_TOP_MID, 0, 122);
    lv_obj_set_style_bg_color(trio_box, lv_color_hex(0x111827), 0);
    lv_obj_set_style_border_color(trio_box, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_border_width(trio_box, 1, 0);
    lv_obj_set_style_radius(trio_box, 10, 0);
    lv_obj_set_style_pad_all(trio_box, 4, 0);
    lv_obj_set_flex_flow(trio_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(trio_box, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(trio_box, LV_OBJ_FLAG_SCROLLABLE);

    // 指标1：2级 + 速度 (移除汉字“风向”)
    lv_obj_t* item1 = lv_obj_create(trio_box);
    lv_obj_set_size(item1, 88, 56);
    lv_obj_set_style_bg_color(item1, lv_color_hex(0x161F2E), 0);
    lv_obj_set_style_border_width(item1, 0, 0);
    lv_obj_set_style_radius(item1, 6, 0);
    lv_obj_set_style_pad_all(item1, 4, 0);
    lv_obj_set_flex_flow(item1, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(item1, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(item1, LV_OBJ_FLAG_SCROLLABLE);

    weather_wind_val_ = lv_label_create(item1);
    lv_label_set_text(weather_wind_val_, "2级");
    lv_obj_set_style_text_font(weather_wind_val_, &font_noto_sans_basic_20_4, 0);
    lv_obj_set_style_text_color(weather_wind_val_, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t* i1_s = lv_label_create(item1);
    lv_label_set_text(i1_s, current_weather_.wind_speed.c_str());
    lv_obj_set_style_text_font(i1_s, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(i1_s, lv_color_hex(0x38BDF8), 0);

    // 指标2：弱 + UV值 (移除汉字“紫外线”)
    lv_obj_t* item2 = lv_obj_create(trio_box);
    lv_obj_set_size(item2, 88, 56);
    lv_obj_set_style_bg_color(item2, lv_color_hex(0x161F2E), 0);
    lv_obj_set_style_border_width(item2, 0, 0);
    lv_obj_set_style_radius(item2, 6, 0);
    lv_obj_set_style_pad_all(item2, 4, 0);
    lv_obj_set_flex_flow(item2, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(item2, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(item2, LV_OBJ_FLAG_SCROLLABLE);

    weather_uv_val_ = lv_label_create(item2);
    lv_label_set_text(weather_uv_val_, current_weather_.uv_level.c_str());
    lv_obj_set_style_text_color(weather_uv_val_, lv_color_hex(0xFBBF24), 0);

    lv_obj_t* i2_s = lv_label_create(item2);
    lv_label_set_text(i2_s, current_weather_.uv_val.c_str());
    lv_obj_set_style_text_font(i2_s, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(i2_s, lv_color_hex(0xFDE68A), 0);

    // 指标3：气压值 + hPa (移除汉字“气压”)
    lv_obj_t* item3 = lv_obj_create(trio_box);
    lv_obj_set_size(item3, 88, 56);
    lv_obj_set_style_bg_color(item3, lv_color_hex(0x161F2E), 0);
    lv_obj_set_style_border_width(item3, 0, 0);
    lv_obj_set_style_radius(item3, 6, 0);
    lv_obj_set_style_pad_all(item3, 4, 0);
    lv_obj_set_flex_flow(item3, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(item3, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(item3, LV_OBJ_FLAG_SCROLLABLE);

    weather_pres_val_ = lv_label_create(item3);
    lv_label_set_text(weather_pres_val_, current_weather_.pressure.c_str());
    lv_obj_set_style_text_font(weather_pres_val_, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(weather_pres_val_, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t* i3_s = lv_label_create(item3);
    lv_label_set_text(i3_s, "hPa");
    lv_obj_set_style_text_font(i3_s, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(i3_s, lv_color_hex(0x94A3B8), 0);

    // 6. 底部 3 时段预报条 (y: 202, h: 92，卡片增高至 88px，时间完整暴露无遮挡！)
    lv_obj_t* fore_box = lv_obj_create(weather_overlay_);
    lv_obj_set_size(fore_box, 290, 92);
    lv_obj_align(fore_box, LV_ALIGN_TOP_MID, 0, 202);
    lv_obj_set_style_bg_opa(fore_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(fore_box, 0, 0);
    lv_obj_set_style_pad_all(fore_box, 0, 0);
    lv_obj_set_flex_flow(fore_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fore_box, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(fore_box, LV_OBJ_FLAG_SCROLLABLE);

    // 时段 1 (w: 90, h: 88，充足高度，时间标签绝无遮挡)
    lv_obj_t* f1 = lv_obj_create(fore_box);
    lv_obj_set_size(f1, 90, 88);
    lv_obj_set_style_bg_color(f1, lv_color_hex(0x131A26), 0);
    lv_obj_set_style_border_color(f1, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_width(f1, 1, 0);
    lv_obj_set_style_radius(f1, 8, 0);
    lv_obj_set_style_pad_top(f1, 6, 0);
    lv_obj_set_style_pad_top(f1, 8, 0);
    lv_obj_set_style_pad_bottom(f1, 8, 0);
    lv_obj_set_style_pad_left(f1, 4, 0);
    lv_obj_set_style_pad_right(f1, 4, 0);
    lv_obj_set_flex_flow(f1, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(f1, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(f1, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* f1_time = lv_label_create(f1);
    lv_label_set_text(f1_time, "15:00");
    lv_obj_set_style_text_font(f1_time, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(f1_time, lv_color_hex(0x94A3B8), 0);

    // 晴天：Icon 与 温度并排在同一行
    lv_obj_t* f1_row = lv_obj_create(f1);
    lv_obj_set_size(f1_row, 82, 34);
    lv_obj_set_style_bg_opa(f1_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(f1_row, 0, 0);
    lv_obj_set_style_pad_all(f1_row, 0, 0);
    lv_obj_set_flex_flow(f1_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(f1_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(f1_row, 5, 0);
    lv_obj_remove_flag(f1_row, LV_OBJ_FLAG_CLICKABLE);

    // 晴天太阳 Icon
    lv_obj_t* f1_sun = lv_obj_create(f1_row);
    lv_obj_set_size(f1_sun, 12, 12);
    lv_obj_set_style_radius(f1_sun, 6, 0);
    lv_obj_set_style_bg_color(f1_sun, lv_color_hex(0xFBBF24), 0);
    lv_obj_set_style_border_color(f1_sun, lv_color_hex(0xF59E0B), 0);
    lv_obj_set_style_border_width(f1_sun, 2, 0);
    lv_obj_set_style_pad_all(f1_sun, 0, 0);
    lv_obj_remove_flag(f1_sun, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* f1_temp = lv_label_create(f1_row);
    lv_label_set_text(f1_temp, "25");
    lv_obj_set_style_text_font(f1_temp, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(f1_temp, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t* f1_deg = lv_label_create(f1_row);
    lv_label_set_text(f1_deg, "°");
    lv_obj_set_style_text_font(f1_deg, &font_noto_sans_basic_20_4, 0);
    lv_obj_set_style_text_color(f1_deg, lv_color_hex(0x94A3B8), 0);

    // 时段 2 (多云，w: 90, h: 88)
    lv_obj_t* f2 = lv_obj_create(fore_box);
    lv_obj_set_size(f2, 90, 88);
    lv_obj_set_style_bg_color(f2, lv_color_hex(0x182234), 0);
    lv_obj_set_style_border_color(f2, lv_color_hex(0x0284C7), 0);
    lv_obj_set_style_border_width(f2, 1, 0);
    lv_obj_set_style_radius(f2, 8, 0);
    lv_obj_set_style_pad_top(f2, 8, 0);
    lv_obj_set_style_pad_bottom(f2, 8, 0);
    lv_obj_set_style_pad_left(f2, 4, 0);
    lv_obj_set_style_pad_right(f2, 4, 0);
    lv_obj_set_flex_flow(f2, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(f2, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(f2, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* f2_time = lv_label_create(f2);
    lv_label_set_text(f2_time, "18:00");
    lv_obj_set_style_text_font(f2_time, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(f2_time, lv_color_hex(0x38BDF8), 0);

    // 多云：Icon 与 温度并排在同一行
    lv_obj_t* f2_row = lv_obj_create(f2);
    lv_obj_set_size(f2_row, 82, 34);
    lv_obj_set_style_bg_opa(f2_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(f2_row, 0, 0);
    lv_obj_set_style_pad_all(f2_row, 0, 0);
    lv_obj_set_flex_flow(f2_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(f2_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(f2_row, 5, 0);
    lv_obj_remove_flag(f2_row, LV_OBJ_FLAG_CLICKABLE);

    // 多云云朵 Icon
    lv_obj_t* f2_cloud = lv_obj_create(f2_row);
    lv_obj_set_size(f2_cloud, 16, 10);
    lv_obj_set_style_radius(f2_cloud, 5, 0);
    lv_obj_set_style_bg_color(f2_cloud, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_border_width(f2_cloud, 0, 0);
    lv_obj_set_style_pad_all(f2_cloud, 0, 0);
    lv_obj_remove_flag(f2_cloud, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* f2_temp = lv_label_create(f2_row);
    lv_label_set_text(f2_temp, "22");
    lv_obj_set_style_text_font(f2_temp, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(f2_temp, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t* f2_deg = lv_label_create(f2_row);
    lv_label_set_text(f2_deg, "°");
    lv_obj_set_style_text_font(f2_deg, &font_noto_sans_basic_20_4, 0);
    lv_obj_set_style_text_color(f2_deg, lv_color_hex(0x94A3B8), 0);

    // 时段 3 (雨天/夜间，w: 90, h: 88)
    lv_obj_t* f3 = lv_obj_create(fore_box);
    lv_obj_set_size(f3, 90, 88);
    lv_obj_set_style_bg_color(f3, lv_color_hex(0x131A26), 0);
    lv_obj_set_style_border_color(f3, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_width(f3, 1, 0);
    lv_obj_set_style_radius(f3, 8, 0);
    lv_obj_set_style_pad_top(f3, 8, 0);
    lv_obj_set_style_pad_bottom(f3, 8, 0);
    lv_obj_set_style_pad_left(f3, 4, 0);
    lv_obj_set_style_pad_right(f3, 4, 0);
    lv_obj_set_flex_flow(f3, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(f3, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(f3, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* f3_time = lv_label_create(f3);
    lv_label_set_text(f3_time, "21:00");
    lv_obj_set_style_text_font(f3_time, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(f3_time, lv_color_hex(0x94A3B8), 0);

    // 雨天/夜间：Icon 与 温度并排在同一行
    lv_obj_t* f3_row = lv_obj_create(f3);
    lv_obj_set_size(f3_row, 82, 34);
    lv_obj_set_style_bg_opa(f3_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(f3_row, 0, 0);
    lv_obj_set_style_pad_all(f3_row, 0, 0);
    lv_obj_set_flex_flow(f3_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(f3_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(f3_row, 5, 0);
    lv_obj_remove_flag(f3_row, LV_OBJ_FLAG_CLICKABLE);

    // 蓝滴雨水 Icon (LV_SYMBOL_TINT)
    lv_obj_t* f3_rain = lv_label_create(f3_row);
    lv_label_set_text(f3_rain, LV_SYMBOL_TINT);
    lv_obj_set_style_text_color(f3_rain, lv_color_hex(0x60A5FA), 0);

    lv_obj_t* f3_temp = lv_label_create(f3_row);
    lv_label_set_text(f3_temp, "19");
    lv_obj_set_style_text_font(f3_temp, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(f3_temp, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t* f3_deg = lv_label_create(f3_row);
    lv_label_set_text(f3_deg, "°");
    lv_obj_set_style_text_font(f3_deg, &font_noto_sans_basic_20_4, 0);
    lv_obj_set_style_text_color(f3_deg, lv_color_hex(0x94A3B8), 0);

    fore_cards_[0] = f1;
    fore_times_[0] = f1_time;
    fore_temps_[0] = f1_temp;

    fore_cards_[1] = f2;
    fore_times_[1] = f2_time;
    fore_temps_[1] = f2_temp;

    fore_cards_[2] = f3;
    fore_times_[2] = f3_time;
    fore_temps_[2] = f3_temp;

    UpdateWeatherLabels();
    UpdateWeatherHourlyForecast();
}

void CustomLcdDisplay::UpdateWeatherHourlyForecast() {
    if (!weather_ui_created_ || !fore_cards_[0] || !fore_cards_[1] || !fore_cards_[2])
        return;

    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    int cur_h = timeinfo.tm_hour;
    if (timeinfo.tm_year < 120) {
        cur_h = 12;
    }

    int hours[3] = {cur_h, (cur_h + 3) % 24, (cur_h + 6) % 24};

    // 第 0 个时段（当前时间）为高亮选中态，其余为暗色常规态
    for (int i = 0; i < 3; ++i) {
        char time_buf[16];
        snprintf(time_buf, sizeof(time_buf), "%02d:00", hours[i]);
        if (fore_times_[i]) {
            lv_label_set_text(fore_times_[i], time_buf);
        }

        if (i == 0) {
            // 当前时段高亮选中态（科技蓝光高亮边框 + 微微发蓝背景）
            lv_obj_set_style_bg_color(fore_cards_[i], lv_color_hex(0x182234), 0);
            lv_obj_set_style_border_color(fore_cards_[i], lv_color_hex(0x0284C7), 0);
            lv_obj_set_style_border_width(fore_cards_[i], 1, 0);
            if (fore_times_[i]) {
                lv_obj_set_style_text_color(fore_times_[i], lv_color_hex(0x38BDF8), 0);
            }
        } else {
            // 未选中常规暗色态
            lv_obj_set_style_bg_color(fore_cards_[i], lv_color_hex(0x131A26), 0);
            lv_obj_set_style_border_color(fore_cards_[i], lv_color_hex(0x1E293B), 0);
            lv_obj_set_style_border_width(fore_cards_[i], 1, 0);
            if (fore_times_[i]) {
                lv_obj_set_style_text_color(fore_times_[i], lv_color_hex(0x94A3B8), 0);
            }
        }
    }

    // 根据当前主温度动态填充时段温度
    int base_temp = 25;
    if (!current_weather_.temp.empty()) {
        int parsed = atoi(current_weather_.temp.c_str());
        if (parsed > -50 && parsed < 60) {
            base_temp = parsed;
        }
    }
    if (fore_temps_[0]) {
        lv_label_set_text_fmt(fore_temps_[0], "%d", base_temp);
    }
    if (fore_temps_[1]) {
        int diff1 = (hours[1] >= 11 && hours[1] <= 15) ? 1 : -1;
        lv_label_set_text_fmt(fore_temps_[1], "%d", base_temp + diff1);
    }
    if (fore_temps_[2]) {
        int diff2 = (hours[2] >= 11 && hours[2] <= 15) ? 1 : -2;
        lv_label_set_text_fmt(fore_temps_[2], "%d", base_temp + diff2);
    }
}

void CustomLcdDisplay::UpdateWeatherLabels() {
    if (!weather_ui_created_)
        return;

    if (weather_temp_label_) {
        lv_label_set_text(weather_temp_label_, current_weather_.temp.c_str());
    }
    if (weather_hum_label_) {
        std::string hum_str = "湿度 " + current_weather_.humidity;
        lv_label_set_text(weather_hum_label_, hum_str.c_str());
    }
    if (weather_loc_label_) {
        lv_label_set_text(weather_loc_label_, current_weather_.city.c_str());
    }
    if (weather_cond_label_) {
        lv_label_set_text(weather_cond_label_, current_weather_.weather.c_str());
    }
    if (weather_wind_val_) {
        lv_label_set_text(weather_wind_val_, current_weather_.wind_level.c_str());
    }
    UpdateWeatherHourlyForecast();
}

void CustomLcdDisplay::UpdateWeatherClock() { UpdateWeatherHourlyForecast(); }

void CustomLcdDisplay::UpdateIndicator(int active_page) {
#if CONFIG_WS185C_ENABLE_NAVIDROME
    if (!dot_player_ || !dot_home_ || !dot_weather_)
        return;

    // 全部重置为 6px 暗灰色微圆点
    lv_obj_set_size(dot_player_, 6, 5);
    lv_obj_set_style_bg_color(dot_player_, lv_color_hex(0x31353E), 0);
#else
    if (!dot_home_ || !dot_weather_)
        return;
#endif
    lv_obj_set_size(dot_home_, 6, 5);
    lv_obj_set_style_bg_color(dot_home_, lv_color_hex(0x31353E), 0);
    lv_obj_set_size(dot_weather_, 6, 5);
    lv_obj_set_style_bg_color(dot_weather_, lv_color_hex(0x31353E), 0);

#if CONFIG_WS185C_ENABLE_NAVIDROME
    // 激活对应页面为 18px 亮青色胶囊
    if (active_page == -1) {
        lv_obj_set_size(dot_player_, 18, 5);
        lv_obj_set_style_bg_color(dot_player_, lv_color_hex(0x00D2FF), 0);
    } else
#endif
        if (active_page == 1) {
        lv_obj_set_size(dot_weather_, 18, 5);
        lv_obj_set_style_bg_color(dot_weather_, lv_color_hex(0x00D2FF), 0);
    } else {
        lv_obj_set_size(dot_home_, 18, 5);
        lv_obj_set_style_bg_color(dot_home_, lv_color_hex(0x00D2FF), 0);
    }
}

void CustomLcdDisplay::UpdateTomorrowWeather(const TomorrowWeather& weather) {
    DisplayLockGuard lock(this);
    current_weather_ = weather;
    UpdateWeatherLabels();
}

void CustomLcdDisplay::ShowPlayerPage() {
#if CONFIG_WS185C_ENABLE_NAVIDROME
    DisplayLockGuard lock(this);
    EnsurePlayerUI();

    current_page_ = -1;
    if (weather_overlay_) {
        lv_obj_add_flag(weather_overlay_, LV_OBJ_FLAG_HIDDEN);
    }
    if (home_dashboard_) {
        lv_obj_add_flag(home_dashboard_, LV_OBJ_FLAG_HIDDEN);
    }
    if (settings_overlay_) {
        lv_obj_add_flag(settings_overlay_, LV_OBJ_FLAG_HIDDEN);
    }
#if CONFIG_WS185C_ENABLE_BESZEL
    if (server_overlay_) {
        lv_obj_add_flag(server_overlay_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
    if (player_overlay_) {
        lv_obj_remove_flag(player_overlay_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(player_overlay_);
    }
    if (indicator_container_) {
        lv_obj_remove_flag(indicator_container_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(indicator_container_);
    }
    UpdateIndicator(-1);
    SetPlayerAnimationActive(is_playing_);
    // 进入播放器页面时自动在后台扫描可用的 DLNA 设备并缓存（非强制刷新，防抖防频繁触发）
    ScanDlnaDevices(false);
#else
    ShowHomePage();
#endif
}

void CustomLcdDisplay::ShowWeatherPage() {
    DisplayLockGuard lock(this);
    EnsureWeatherUI();
    SetPlayerAnimationActive(false);

    current_page_ = 1;
#if CONFIG_WS185C_ENABLE_NAVIDROME
    if (player_overlay_) {
        lv_obj_add_flag(player_overlay_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
    if (home_dashboard_) {
        lv_obj_add_flag(home_dashboard_, LV_OBJ_FLAG_HIDDEN);
    }
    if (settings_overlay_) {
        lv_obj_add_flag(settings_overlay_, LV_OBJ_FLAG_HIDDEN);
    }
#if CONFIG_WS185C_ENABLE_BESZEL
    if (server_overlay_) {
        lv_obj_add_flag(server_overlay_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
    if (weather_overlay_) {
        lv_obj_remove_flag(weather_overlay_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(weather_overlay_);
    }
    if (indicator_container_) {
        lv_obj_remove_flag(indicator_container_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(indicator_container_);
    }
    UpdateIndicator(1);
}

void CustomLcdDisplay::ShowHomePage() {
    DisplayLockGuard lock(this);
    SetPlayerAnimationActive(false);
    current_page_ = 0;
#if CONFIG_WS185C_ENABLE_NAVIDROME
    if (player_overlay_) {
        lv_obj_add_flag(player_overlay_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
    if (weather_overlay_) {
        lv_obj_add_flag(weather_overlay_, LV_OBJ_FLAG_HIDDEN);
    }
    if (settings_overlay_) {
        lv_obj_add_flag(settings_overlay_, LV_OBJ_FLAG_HIDDEN);
    }
#if CONFIG_WS185C_ENABLE_BESZEL
    if (server_overlay_) {
        lv_obj_add_flag(server_overlay_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
    if (home_dashboard_) {
        lv_obj_remove_flag(home_dashboard_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(home_dashboard_);
    }
    if (indicator_container_) {
        lv_obj_remove_flag(indicator_container_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(indicator_container_);
    }
    UpdateIndicator(0);
}

void CustomLcdDisplay::ShowSettingsPage() {
    DisplayLockGuard lock(this);
    EnsureSettingsUI();
    UpdateSettingsValues();
    SetPlayerAnimationActive(false);

    current_page_ = 2;
    if (weather_overlay_) {
        lv_obj_add_flag(weather_overlay_, LV_OBJ_FLAG_HIDDEN);
    }
#if CONFIG_WS185C_ENABLE_NAVIDROME
    if (player_overlay_) {
        lv_obj_add_flag(player_overlay_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
    if (home_dashboard_) {
        lv_obj_add_flag(home_dashboard_, LV_OBJ_FLAG_HIDDEN);
    }
#if CONFIG_WS185C_ENABLE_BESZEL
    if (server_overlay_) {
        lv_obj_add_flag(server_overlay_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
    if (indicator_container_) {
        lv_obj_add_flag(indicator_container_, LV_OBJ_FLAG_HIDDEN);
    }
    if (settings_overlay_) {
        lv_obj_remove_flag(settings_overlay_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(settings_overlay_);
    }
}

void CustomLcdDisplay::ShowServerPage() {
#if CONFIG_WS185C_ENABLE_BESZEL
    DisplayLockGuard lock(this);
    EnsureServerUI();
    UpdateServerUI();
    SetPlayerAnimationActive(false);

    current_page_ = 3;
    vps_carousel_counter_ = 0;

#if CONFIG_WS185C_ENABLE_NAVIDROME
    if (player_overlay_) {
        lv_obj_add_flag(player_overlay_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
    if (weather_overlay_) {
        lv_obj_add_flag(weather_overlay_, LV_OBJ_FLAG_HIDDEN);
    }
    if (settings_overlay_) {
        lv_obj_add_flag(settings_overlay_, LV_OBJ_FLAG_HIDDEN);
    }
    if (home_dashboard_) {
        lv_obj_add_flag(home_dashboard_, LV_OBJ_FLAG_HIDDEN);
    }
    if (indicator_container_) {
        lv_obj_add_flag(indicator_container_, LV_OBJ_FLAG_HIDDEN);
    }
    if (server_overlay_) {
        lv_obj_remove_flag(server_overlay_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(server_overlay_);
    }

    // 触发异步数据更新：若距离上次拉取超过 5 秒，则重置间隔允许立刻更新
    int64_t now_sec = esp_timer_get_time() / 1000000;
    if (last_server_fetch_sec_ != 0 && (now_sec - last_server_fetch_sec_) >= 5) {
        last_server_fetch_sec_ = 0;
    }
    CheckAndTriggerServerFetch();
#else
    ShowHomePage();
#endif
}

#if CONFIG_WS185C_ENABLE_BESZEL
void CustomLcdDisplay::NextVpsNode() {
    DisplayLockGuard lock(this);
    if (vps_nodes_.empty())
        return;
    current_vps_idx_ = (current_vps_idx_ + 1) % vps_nodes_.size();
    vps_carousel_counter_ = 0;
    UpdateServerUI();
}

void CustomLcdDisplay::PrevVpsNode() {
    DisplayLockGuard lock(this);
    if (vps_nodes_.empty())
        return;
    if (current_vps_idx_ == 0) {
        current_vps_idx_ = vps_nodes_.size() - 1;
    } else {
        current_vps_idx_--;
    }
    vps_carousel_counter_ = 0;
    UpdateServerUI();
}
#endif

// ========================================================
// 懒加载：第三屏 Cyber HUD 音乐播放器 (Music Player)
// ========================================================
#if CONFIG_WS185C_ENABLE_NAVIDROME
void CustomLcdDisplay::EnsurePlayerUI() {
    DisplayLockGuard lock(this);
    if (player_ui_created_)
        return;
    player_ui_created_ = true;

    if (playlist_.empty() && service_config_ && service_config_->GetNavidromeConfig().IsConfigured()) {
        FetchNavidromePlaylist();
    }

    lv_obj_t* screen = lv_display_get_screen_active(lv_display_get_default());

    // 1. 全屏覆盖层 (黑底圆角)
    player_overlay_ = lv_obj_create(screen);
    lv_obj_set_size(player_overlay_, 360, 360);
    lv_obj_align(player_overlay_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(player_overlay_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(player_overlay_, 0, 0);
    lv_obj_set_style_pad_all(player_overlay_, 0, 0);
    lv_obj_set_style_radius(player_overlay_, 180, 0);
    lv_obj_remove_flag(player_overlay_, LV_OBJ_FLAG_SCROLLABLE);

    // 绑定向左滑动手势返回主屏
    auto on_player_gesture = [](lv_event_t* e) {
        auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir == LV_DIR_LEFT) {
            self->ShowHomePage();
        }
    };
    lv_obj_add_event_cb(player_overlay_, on_player_gesture, LV_EVENT_GESTURE, this);

    // 2. 顶部微光标题 (隐藏，顶部彻底留白，不放置内容)
    player_header_label_ = lv_label_create(player_overlay_);
    lv_obj_add_flag(player_header_label_, LV_OBJ_FLAG_HIDDEN);

    // 3. 中间黑胶唱片与环形进度条区 (y: 30, w: 168, h: 168, 下边缘为 198)
    lv_obj_t* disc_box = lv_obj_create(player_overlay_);
    lv_obj_set_size(disc_box, 168, 168);
    lv_obj_align(disc_box, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_opa(disc_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(disc_box, 0, 0);
    lv_obj_set_style_pad_all(disc_box, 0, 0);
    lv_obj_remove_flag(disc_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(disc_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        disc_box,
        [](lv_event_t* e) {
            auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
            self->OnVinylClicked();
        },
        LV_EVENT_CLICKED, this);

    // 弧形进度条 (围绕唱片)
    player_arc_ = lv_arc_create(disc_box);
    lv_obj_set_size(player_arc_, 164, 164);
    lv_obj_align(player_arc_, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_rotation(player_arc_, 135);
    lv_arc_set_bg_angles(player_arc_, 0, 270);
    lv_arc_set_range(player_arc_, 0, 100);
    lv_arc_set_value(player_arc_, 0);
    lv_obj_set_style_arc_width(player_arc_, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(player_arc_, lv_color_hex(0x1F2937), LV_PART_MAIN);
    lv_obj_set_style_arc_width(player_arc_, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(player_arc_, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);
    lv_obj_set_style_opa(player_arc_, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_remove_flag(player_arc_, LV_OBJ_FLAG_CLICKABLE);

    // 黑胶唱片本体 (直径 134px，深邃黑胶质感，边缘高光)
    // 同心圆正中央增加触控交互：单击切换播放设备（本机 / 局域网各 DLNA 设备）
    lv_obj_t* vinyl = lv_obj_create(disc_box);
    lv_obj_set_size(vinyl, 134, 134);
    lv_obj_align(vinyl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(vinyl, 67, 0);
    lv_obj_set_style_bg_color(vinyl, lv_color_hex(0x0B0F19), 0);
    lv_obj_set_style_border_color(vinyl, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_width(vinyl, 2, 0);
    lv_obj_set_style_pad_all(vinyl, 0, 0);
    lv_obj_add_flag(vinyl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(
        vinyl,
        [](lv_event_t* e) {
            auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
            self->OnVinylClicked();
        },
        LV_EVENT_CLICKED, this);

    // 宽幅律动声谱 (宽 116px, 高 58px，13 根律动跳柱)
    lv_obj_t* spectrum_box = lv_obj_create(vinyl);
    lv_obj_set_size(spectrum_box, 116, 58);
    lv_obj_align(spectrum_box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(spectrum_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spectrum_box, 0, 0);
    lv_obj_set_style_pad_all(spectrum_box, 0, 0);
    lv_obj_set_flex_flow(spectrum_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(spectrum_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(spectrum_box, 3, 0);
    lv_obj_remove_flag(spectrum_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(spectrum_box, LV_OBJ_FLAG_CLICKABLE);

    // 13 柱全景音律渐变色彩体系
    static const uint32_t kEqColors[kEqBarCount] = {
        0x0284C7, 0x0EA5E9, 0x38BDF8, 0x00E5FF, 0x2DD4BF, 0x34D399, 0x6EE7B7,
        0x34D399, 0x2DD4BF, 0x00E5FF, 0x38BDF8, 0x0EA5E9, 0x0284C7};
    for (size_t i = 0; i < kEqBarCount; ++i) {
        eq_bars_[i] = lv_obj_create(spectrum_box);
        lv_obj_set_size(eq_bars_[i], 5, 4);
        lv_obj_set_style_radius(eq_bars_[i], 2, 0);
        lv_obj_set_style_bg_color(eq_bars_[i], lv_color_hex(kEqColors[i]), 0);
        lv_obj_set_style_border_width(eq_bars_[i], 0, 0);
        lv_obj_set_style_pad_all(eq_bars_[i], 0, 0);
        lv_obj_remove_flag(eq_bars_[i], LV_OBJ_FLAG_CLICKABLE);
    }

    // 4. 专属播放设备说明栏 (y: 204, 位于同心圆下方、歌曲名称上方，宽度 280 完整展示设备名)
    // 触控交互：点击设备说明栏直接切换设备，与点击黑胶唱片一致
    player_device_label_ = lv_label_create(player_overlay_);
    lv_obj_set_style_text_color(player_device_label_, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_text_font(player_device_label_, GetMainTextFont16(), 0);
    lv_obj_set_width(player_device_label_, 280);
    lv_obj_set_style_text_align(player_device_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(player_device_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(player_device_label_, "♪ 01/01 · 本机扬声器");
    lv_obj_align(player_device_label_, LV_ALIGN_TOP_MID, 0, 204);
    lv_obj_add_flag(player_device_label_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        player_device_label_,
        [](lv_event_t* e) {
            auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
            self->OnVinylClicked();
        },
        LV_EVENT_CLICKED, this);

    // 5. 曲目名称与艺术家 (y: 232 / 256, 继承系统全量中文字库)
    player_title_label_ = lv_label_create(player_overlay_);
    lv_obj_set_style_text_color(player_title_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_width(player_title_label_, 280);
    lv_obj_set_style_text_font(player_title_label_, GetMainTextFont16(), 0);
    lv_obj_set_style_text_align(player_title_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(player_title_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(player_title_label_, LV_ALIGN_TOP_MID, 0, 232);

    player_artist_label_ = lv_label_create(player_overlay_);
    lv_obj_set_style_text_color(player_artist_label_, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_width(player_artist_label_, 260);
    lv_obj_set_style_text_align(player_artist_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(player_artist_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(player_artist_label_, LV_ALIGN_TOP_MID, 0, 256);

    // 6. 底部触控控制栏 (y: 280, 高度 54, 居中排列: Prev, Play, Next)
    lv_obj_t* ctrl_row = lv_obj_create(player_overlay_);
    lv_obj_set_size(ctrl_row, 240, 54);
    lv_obj_align(ctrl_row, LV_ALIGN_TOP_MID, 0, 280);
    lv_obj_set_style_bg_opa(ctrl_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctrl_row, 0, 0);
    lv_obj_set_style_pad_all(ctrl_row, 0, 0);
    lv_obj_set_flex_flow(ctrl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctrl_row, 22, 0);
    lv_obj_remove_flag(ctrl_row, LV_OBJ_FLAG_SCROLLABLE);

    // 上一曲按钮 (44x44，深科技蓝底色，亮青边框，高辨识度)
    player_prev_btn_ = lv_btn_create(ctrl_row);
    lv_obj_set_size(player_prev_btn_, 44, 44);
    lv_obj_set_style_radius(player_prev_btn_, 22, 0);
    lv_obj_set_style_bg_color(player_prev_btn_, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_color(player_prev_btn_, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_border_width(player_prev_btn_, 2, 0);
    lv_obj_set_style_pad_all(player_prev_btn_, 0, 0);
    lv_obj_t* prev_icon = lv_label_create(player_prev_btn_);
    lv_obj_set_style_text_font(prev_icon, &font_material_symbols_16_4, 0);
    lv_label_set_text(prev_icon, MATERIAL_SYMBOLS_SKIP_PREVIOUS);
    lv_obj_set_style_text_color(prev_icon, lv_color_hex(0xF1F5F9), 0);
    lv_obj_align(prev_icon, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(
        player_prev_btn_,
        [](lv_event_t* e) {
            auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
            self->OnPlayerPrevClicked();
        },
        LV_EVENT_CLICKED, this);

    // 播放/暂停按钮 (52x52 大号核心按键，使用实心高质感矢量抗锯齿图标)
    player_play_btn_ = lv_btn_create(ctrl_row);
    lv_obj_set_size(player_play_btn_, 52, 52);
    lv_obj_set_style_radius(player_play_btn_, 26, 0);
    lv_obj_set_style_bg_color(player_play_btn_, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_border_color(player_play_btn_, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_border_width(player_play_btn_, 2, 0);
    lv_obj_set_style_pad_all(player_play_btn_, 0, 0);
    player_play_icon_ = lv_image_create(player_play_btn_);
    lv_image_set_src(player_play_icon_, &img_player_play_arrow);
    lv_obj_align(player_play_icon_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(
        player_play_btn_,
        [](lv_event_t* e) {
            auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
            self->OnPlayerPlayPauseClicked();
        },
        LV_EVENT_CLICKED, this);

    // 下一曲按钮 (44x44，深科技蓝底色，亮青边框，高辨识度)
    player_next_btn_ = lv_btn_create(ctrl_row);
    lv_obj_set_size(player_next_btn_, 44, 44);
    lv_obj_set_style_radius(player_next_btn_, 22, 0);
    lv_obj_set_style_bg_color(player_next_btn_, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_color(player_next_btn_, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_border_width(player_next_btn_, 2, 0);
    lv_obj_set_style_pad_all(player_next_btn_, 0, 0);
    lv_obj_t* next_icon = lv_label_create(player_next_btn_);
    lv_obj_set_style_text_font(next_icon, &font_material_symbols_16_4, 0);
    lv_label_set_text(next_icon, MATERIAL_SYMBOLS_SKIP_NEXT);
    lv_obj_set_style_text_color(next_icon, lv_color_hex(0xF1F5F9), 0);
    lv_obj_align(next_icon, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(
        player_next_btn_,
        [](lv_event_t* e) {
            auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
            self->OnPlayerNextClicked();
        },
        LV_EVENT_CLICKED, this);

    // 7. 动画驱动定时器 (90ms 节拍，默认休眠，仅播放且在当前页时唤醒)
    if (!player_anim_timer_) {
        player_anim_timer_ = lv_timer_create(
            [](lv_timer_t* t) {
                auto self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(t));
                self->UpdatePlayerAnimation();
            },
            90, this);
        lv_timer_pause(player_anim_timer_);
    }

    UpdatePlayerUI();
}

void CustomLcdDisplay::UpdatePlayerUI() {
    DisplayLockGuard lock(this);
    if (!player_ui_created_)
        return;

    if (playlist_.empty()) {
        if (!service_config_ || !service_config_->GetNavidromeConfig().IsConfigured()) {
            if (player_device_label_)
                lv_label_set_text(player_device_label_, "♪ --/-- · 请配置 Navidrome");
            if (player_title_label_)
                lv_label_set_text(player_title_label_, "等待服务配置");
            if (player_artist_label_)
                lv_label_set_text(player_artist_label_, "使用 MCP 或 NVS 设置");
        } else if (navidrome_status_ == ServiceStatus::kAuthError) {
            if (player_device_label_)
                lv_label_set_text(player_device_label_, "♪ --/-- · 账号或密码错误");
            if (player_title_label_)
                lv_label_set_text(player_title_label_, "请检查账号配置");
            if (player_artist_label_)
                lv_label_set_text(player_artist_label_, "请在设置中修改");
        } else if (navidrome_status_ == ServiceStatus::kNetworkError) {
            if (player_device_label_)
                lv_label_set_text(player_device_label_, "♪ --/-- · 无法连接服务器");
            if (player_title_label_)
                lv_label_set_text(player_title_label_, "网络不可达");
            if (player_artist_label_)
                lv_label_set_text(player_artist_label_, "请检查网络或地址");
        } else {
            if (player_device_label_)
                lv_label_set_text(player_device_label_, "♪ 00/00 · 等待曲目加载...");
            if (player_title_label_)
                lv_label_set_text(player_title_label_, "曲库暂无音乐");
            if (player_artist_label_)
                lv_label_set_text(player_artist_label_, "正在从服务器拉取");
        }
        if (player_time_label_)
            lv_label_set_text(player_time_label_, "--:--");
        if (player_arc_)
            lv_arc_set_value(player_arc_, 0);
        SetPlayerAnimationActive(false);
        return;
    }

    const auto& track = playlist_[current_track_idx_];

    // 1. 顶部不再放置内容，彻底隐藏
    if (player_header_label_) {
        lv_obj_add_flag(player_header_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // 2. 专属设备显示栏 (位于同心圆下方、歌曲名称上方，包含 icon 与曲目序号，去掉中括号与
    // navidrome)
    auto& dlna = waveshare185c::DlnaController::GetInstance();
    int target_idx = dlna.GetTargetIndex();
    bool is_scanning = dlna.IsScanning();

    if (player_device_label_) {
        char dev_buf[96];
        char prefix[32];
        snprintf(prefix, sizeof(prefix), "♪ %02d/%02d · ", (int)(current_track_idx_ + 1),
                 (int)playlist_.size());

        if (target_idx >= 0) {
            std::string dev_name = dlna.GetTargetName();
            snprintf(dev_buf, sizeof(dev_buf), "%s投播: %s", prefix, dev_name.c_str());
            lv_label_set_text(player_device_label_, dev_buf);
            lv_obj_set_style_text_color(player_device_label_, lv_color_hex(0x00E5FF), 0);
        } else if (is_scanning) {
            snprintf(dev_buf, sizeof(dev_buf), "%s正在搜索设备...", prefix);
            lv_label_set_text(player_device_label_, dev_buf);
            lv_obj_set_style_text_color(player_device_label_, lv_color_hex(0xFBBF24), 0);
        } else {
            snprintf(dev_buf, sizeof(dev_buf), "%s本机扬声器", prefix);
            lv_label_set_text(player_device_label_, dev_buf);
            lv_obj_set_style_text_color(player_device_label_, lv_color_hex(0x38BDF8), 0);
        }
    }

    if (player_title_label_) {
        lv_label_set_text(player_title_label_, track.title.c_str());
    }

    if (player_artist_label_) {
        lv_label_set_text(player_artist_label_, track.artist.c_str());
    }

    // 播放/暂停按钮视觉状态更新 (实心抗锯齿图标切换)
    if (player_play_btn_ && player_play_icon_) {
        lv_obj_set_style_bg_color(player_play_btn_, lv_color_hex(0x0F172A), 0);
        lv_obj_set_style_border_color(player_play_btn_, lv_color_hex(0x00E5FF), 0);
        lv_obj_set_style_border_width(player_play_btn_, 2, 0);
        if (is_playing_) {
            lv_image_set_src(player_play_icon_, &img_player_pause);
        } else {
            lv_image_set_src(player_play_icon_, &img_player_play_arrow);
        }
    }

    if (player_arc_) {
        int pct = 0;
        if (track.duration_sec > 0) {
            pct = (play_elapsed_sec_ * 100) / track.duration_sec;
            if (pct > 100)
                pct = 100;
        }
        lv_arc_set_value(player_arc_, pct);
    }

    SetPlayerAnimationActive(is_playing_);
}

void CustomLcdDisplay::SetPlayerAnimationActive(bool active) {
    DisplayLockGuard lock(this);
    if (!player_anim_timer_)
        return;
    if (active && is_playing_ && current_page_ == -1) {
        lv_timer_resume(player_anim_timer_);
    } else {
        lv_timer_pause(player_anim_timer_);
        for (size_t i = 0; i < kEqBarCount; ++i) {
            if (eq_bars_[i]) {
                lv_obj_set_height(eq_bars_[i], 4);
            }
        }
    }
}

void CustomLcdDisplay::UpdatePlayerAnimation() {
    if (!player_ui_created_ || current_page_ != -1)
        return;

    if (!is_playing_) {
        for (size_t i = 0; i < kEqBarCount; ++i) {
            if (eq_bars_[i]) {
                lv_obj_set_height(eq_bars_[i], 4);
            }
        }
        return;
    }

    anim_step_++;

    // 13 根声波柱的大开大合声学跳动包络（中央最高 44px，两翼自然收束）
    static const int kBaseMaxH[kEqBarCount] = {10, 16, 22, 28, 34, 40, 44, 40, 34, 28, 22, 16, 10};
    for (size_t i = 0; i < kEqBarCount; ++i) {
        if (!eq_bars_[i])
            continue;
        float phase = (anim_step_ * 0.38f) + (float)i * 0.55f;
        float wave1 = sinf(phase);
        float wave2 = sinf(phase * 1.7f);
        int wave = (int)(9.0f * wave1 + 4.0f * wave2);
        int noise = ((anim_step_ * 11 + i * 17) % 7) - 3;
        int h = kBaseMaxH[i] + wave + noise;
        if (h < 4)
            h = 4;
        if (h > 46)
            h = 46;
        lv_obj_set_height(eq_bars_[i], h);
    }
}

void CustomLcdDisplay::StopNavidromeStream() {
    is_playing_ = false;
    int target_idx = waveshare185c::DlnaController::GetInstance().GetTargetIndex();
    if (target_idx >= 0) {
        xTaskCreate(
            [](void* param) {
                int t_idx = waveshare185c::DlnaController::GetInstance().GetTargetIndex();
                waveshare185c::DlnaController::GetInstance().Pause(t_idx);
                vTaskDelete(NULL);
            },
            "dlna_pause", 3072, NULL, 3, NULL);
    }
    stream_stop_requested_ = true;
    current_playback_id_++;
    SetPlayerAnimationActive(false);
    Application::GetInstance().GetAudioService().ResetDecoder();
    UpdatePlayerUI();
}

void CustomLcdDisplay::StartNavidromeStream(size_t track_idx) {
    StopNavidromeStream();

    // 如果上一个流任务正在退出，等待它清理完成，避免双流并发争抢网络与内存
    int wait_cycles = 0;
    while (stream_task_handle_ != nullptr && wait_cycles < 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_cycles++;
    }

    if (!service_config_ || !service_config_->GetNavidromeConfig().IsConfigured()) {
        ESP_LOGW(TAG, "Navidrome is unconfigured, cannot stream");
        return;
    }

    if (playlist_.empty() || track_idx >= playlist_.size())
        return;

    const auto& track = playlist_[track_idx];
    if (track.id.empty()) {
        ESP_LOGW(TAG, "Track id is empty, cannot stream from Navidrome");
        return;
    }

    int target_idx = waveshare185c::DlnaController::GetInstance().GetTargetIndex();
    if (target_idx >= 0) {
        // DLNA 模式：向电视/小米音响推流原生直链
        xTaskCreate(
            [](void* param) {
                auto self = static_cast<CustomLcdDisplay*>(param);
                int t_idx = waveshare185c::DlnaController::GetInstance().GetTargetIndex();
                if (t_idx >= 0 && self->current_track_idx_ < self->playlist_.size()) {
                    const auto& t = self->playlist_[self->current_track_idx_];
                    std::string stream = t.stream_url;
                    if (stream.empty()) {
                        stream = waveshare185c::ServiceConfig::GenerateNavidromeDirectUrl(
                            self->navidrome_server_, t.id, self->navidrome_user_,
                            self->navidrome_pass_, true);
                    }
                    ESP_LOGI(TAG, "DLNA Cast to device %d: %s (url: %s)", t_idx, t.title.c_str(),
                             waveshare185c::ServiceConfig::RedactUrl(stream).c_str());
                    waveshare185c::DlnaController::GetInstance().Play(t_idx, stream, t.title,
                                                                      t.artist);
                }
                vTaskDelete(NULL);
            },
            "dlna_cast", 4096, this, 3, NULL);

        is_playing_ = true;
        SetPlayerAnimationActive(true);
        UpdatePlayerUI();
        return;
    }

    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    stream_stop_requested_ = false;
    current_playback_id_++;
    uint32_t my_playback_id = current_playback_id_;
    (void)my_playback_id;

    is_playing_ = true;
    SetPlayerAnimationActive(true);
    UpdatePlayerUI();

    xTaskCreate(
        [](void* param) {
            auto self = static_cast<CustomLcdDisplay*>(param);
            uint32_t my_playback_id = self->current_playback_id_;

            auto network = Board::GetInstance().GetNetwork();
            if (!network) {
                if (self->stream_task_handle_ == xTaskGetCurrentTaskHandle()) {
                    self->stream_task_handle_ = nullptr;
                }
                vTaskDelete(NULL);
                return;
            }

            size_t idx = self->current_track_idx_;
            if (idx >= self->playlist_.size()) {
                if (self->stream_task_handle_ == xTaskGetCurrentTaskHandle()) {
                    self->stream_task_handle_ = nullptr;
                }
                vTaskDelete(NULL);
                return;
            }

            std::string song_id = self->playlist_[idx].id;
            std::string stream_url = self->navidrome_server_ + "/rest/stream.view?id=" + song_id +
                                     "&format=opus&maxBitRate=96&u=" + self->navidrome_user_ +
                                     "&p=" + self->navidrome_pass_ + "&v=1.16.1&c=xiaozhi";

            ESP_LOGI(TAG, "Navidrome streaming start [%lu]: %s (url: %s)",
                     (unsigned long)my_playback_id, self->playlist_[idx].title.c_str(),
                     waveshare185c::ServiceConfig::RedactUrl(stream_url).c_str());

            auto http = network->CreateHttp(0);
            bool natural_finish = false;

            if (http) {
                http->SetTimeout(15000);
                http->SetHeader("Accept", "audio/ogg, application/ogg");
                http->SetHeader("Accept-Encoding", "identity");

                if (http->Open("GET", stream_url)) {
                    auto status = http->GetStatusCode();
                    if (status && *status >= 200 && *status < 300 &&
                        !self->stream_stop_requested_ &&
                        self->current_playback_id_ == my_playback_id) {
                        auto demuxer = std::make_unique<OggDemuxer>();
                        auto buffer = std::make_unique<std::array<char, 1024>>();
                        uint32_t media_position_ms = 0;
                        bool packet_error = false;

                        demuxer->OnPacket([self, my_playback_id, &media_position_ms, &packet_error](
                                              const uint8_t* data, int sample_rate,
                                              int frame_duration_ms, size_t size) {
                            if (packet_error || self->stream_stop_requested_ ||
                                self->current_playback_id_ != my_playback_id) {
                                packet_error = true;
                                return;
                            }

                            auto packet = std::make_unique<AudioStreamPacket>();
                            packet->sample_rate = sample_rate;
                            packet->frame_duration = frame_duration_ms;
                            packet->playback_id = my_playback_id;
                            packet->media_position_ms = media_position_ms;
                            packet->payload.assign(data, data + size);

                            if (!Application::GetInstance()
                                     .GetAudioService()
                                     .PushPacketToDecodeQueue(std::move(packet), true)) {
                                packet_error = true;
                                return;
                            }
                            media_position_ms += frame_duration_ms;
                        });

                        bool eof = false;
                        while (!packet_error && !self->stream_stop_requested_ &&
                               self->current_playback_id_ == my_playback_id) {
                            auto read_size = http->Read(buffer->data(), buffer->size());
                            if (!read_size) {
                                ESP_LOGW(TAG, "Navidrome HTTP read error: %s",
                                         read_size.error().ToString().c_str());
                                break;
                            }
                            if (*read_size == 0) {
                                eof = true;
                                break;
                            }
                            demuxer->Process(reinterpret_cast<const uint8_t*>(buffer->data()),
                                             *read_size);
                            if (demuxer->HasError()) {
                                ESP_LOGE(TAG, "Navidrome demuxer error during decode");
                                break;
                            }
                        }

                        if (eof && !packet_error && !self->stream_stop_requested_ &&
                            self->current_playback_id_ == my_playback_id) {
                            if (demuxer->Finish()) {
                                natural_finish = true;
                            }
                        }
                    }
                    http->Close();
                }
            }

            if (natural_finish && !self->stream_stop_requested_ &&
                self->current_playback_id_ == my_playback_id && self->is_playing_) {
                ESP_LOGI(TAG, "Navidrome track finished naturally, advance to next track");
                Application::GetInstance().Schedule([self]() { self->OnPlayerNextClicked(); });
            }

            if (self->stream_task_handle_ == xTaskGetCurrentTaskHandle()) {
                self->stream_task_handle_ = nullptr;
            }
            vTaskDelete(NULL);
        },
        "navi_stream", 4096, this, 3, &stream_task_handle_);
}

void CustomLcdDisplay::OnPlayerPlayPauseClicked() {
    DisplayLockGuard lock(this);
    if (playlist_.empty())
        return;
    is_playing_ = !is_playing_;
    if (is_playing_) {
        StartNavidromeStream(current_track_idx_);
    } else {
        StopNavidromeStream();
    }
    UpdatePlayerUI();
}

void CustomLcdDisplay::OnPlayerPrevClicked() {
    DisplayLockGuard lock(this);
    if (playlist_.empty())
        return;
    if (current_track_idx_ == 0) {
        current_track_idx_ = playlist_.size() - 1;
    } else {
        current_track_idx_--;
    }
    play_elapsed_sec_ = 0;
    if (is_playing_) {
        StartNavidromeStream(current_track_idx_);
    }
    UpdatePlayerUI();
}

void CustomLcdDisplay::OnPlayerNextClicked() {
    DisplayLockGuard lock(this);
    if (playlist_.empty())
        return;
    current_track_idx_ = (current_track_idx_ + 1) % playlist_.size();
    play_elapsed_sec_ = 0;
    if (is_playing_) {
        StartNavidromeStream(current_track_idx_);
    }
    UpdatePlayerUI();
}

void CustomLcdDisplay::OnVinylClicked() {
    DisplayLockGuard lock(this);
    auto& dlna = waveshare185c::DlnaController::GetInstance();
    auto devs = dlna.GetDevices();
    if (devs.empty()) {
        ESP_LOGI(TAG, "Vinyl clicked: no DLNA devices cached, initiating background scan");
        ScanDlnaDevices(true);
        if (player_device_label_) {
            char dev_buf[96];
            snprintf(dev_buf, sizeof(dev_buf), "♪ %02d/%02d · 正在搜索设备...",
                     playlist_.empty() ? 1 : (int)(current_track_idx_ + 1),
                     playlist_.empty() ? 1 : (int)playlist_.size());
            lv_label_set_text(player_device_label_, dev_buf);
            lv_obj_set_style_text_color(player_device_label_, lv_color_hex(0xFBBF24), 0);
        }
        return;
    }

    int next_target = dlna.GetNextTargetIndex();
    std::string target_name = (next_target >= 0 && next_target < (int)devs.size())
                                  ? devs[next_target].name
                                  : "本机扬声器";
    ESP_LOGI(TAG, "Vinyl clicked: switching target to %d (%s)", next_target, target_name.c_str());
    SwitchPlaybackTarget(next_target);
}

void CustomLcdDisplay::SwitchPlaybackTarget(int target_idx) {
    DisplayLockGuard lock(this);
    auto& dlna = waveshare185c::DlnaController::GetInstance();
    int old_target = dlna.GetTargetIndex();
    if (old_target == target_idx) {
        UpdatePlayerUI();
        return;
    }

    dlna.SetTargetIndex(target_idx);
    ESP_LOGI(TAG, "Switched playback target from %d to %d (playing=%d)", old_target, target_idx,
             (int)is_playing_);

    if (is_playing_) {
        if (target_idx >= 0) {
            // 从本地或另一个设备切换到 DLNA 设备：先停本地流，再向 DLNA 投播
            StopNavidromeStream();
            StartNavidromeStream(current_track_idx_);
        } else {
            // 切回本地扬声器播放：停止旧 DLNA 设备，启动本地流
            if (old_target >= 0) {
                waveshare185c::DlnaController::GetInstance().Stop(old_target);
            }
            StartNavidromeStream(current_track_idx_);
        }
    } else {
        // 关键修复：暂停状态下切换播放设备
        // 如果旧设备是 DLNA 设备，异步发送 Stop 彻底结束旧设备的播放会话
        if (old_target >= 0) {
            xTaskCreate(
                [](void* param) {
                    int dev = (int)(intptr_t)param;
                    waveshare185c::DlnaController::GetInstance().Stop(dev);
                    vTaskDelete(NULL);
                },
                "dlna_stop_old", 3072, (void*)(intptr_t)old_target, 3, NULL);
        }
    }

    UpdatePlayerUI();
}

std::string CustomLcdDisplay::GetCurrentTrackDirectUrl(bool for_dlna) {
    return GetTrackDirectUrl(current_track_idx_, for_dlna);
}

std::string CustomLcdDisplay::GetTrackDirectUrl(size_t track_idx, bool for_dlna) {
    if (playlist_.empty() || track_idx >= playlist_.size()) {
        return "";
    }
    const auto& track = playlist_[track_idx];
    if (for_dlna && !track.stream_url.empty()) {
        return track.stream_url;
    }
    return waveshare185c::ServiceConfig::GenerateNavidromeDirectUrl(
        navidrome_server_, track.id, navidrome_user_, navidrome_pass_, for_dlna);
}

void CustomLcdDisplay::ScanDlnaDevices(bool force) {
    waveshare185c::DlnaController::GetInstance().StartDiscovery(
        [this](const std::vector<waveshare185c::DlnaDevice>&) { this->UpdatePlayerUI(); }, force);
}

std::vector<waveshare185c::DlnaDevice> CustomLcdDisplay::GetDlnaDevices() const {
    return waveshare185c::DlnaController::GetInstance().GetDevices();
}

int CustomLcdDisplay::GetCurrentPlaybackTarget() const {
    return waveshare185c::DlnaController::GetInstance().GetTargetIndex();
}

std::string CustomLcdDisplay::GetCurrentPlaybackTargetName() const {
    return waveshare185c::DlnaController::GetInstance().GetTargetName();
}
#endif

void CustomLcdDisplay::CheckAndTriggerWeatherFetch() {
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();

    if (state != kDeviceStateIdle) {
        return;
    }

#if CONFIG_WS185C_ENABLE_NAVIDROME
    if (is_playing_) {
        return;
    }
#endif

    int64_t now_sec = esp_timer_get_time() / 1000000;
    if (idle_start_sec_ == 0 || (now_sec - idle_start_sec_ < 25)) {
        return;
    }

    if (weather_fetching_) {
        return;
    }

    // 每 30 分钟刷新一次天气（1800 秒）
    if (last_weather_fetch_sec_ != 0 && (now_sec - last_weather_fetch_sec_) < 1800) {
        return;
    }

    last_weather_fetch_sec_ = now_sec;
    weather_fetching_ = true;

    xTaskCreate(
        [](void* arg) {
            auto self = static_cast<CustomLcdDisplay*>(arg);
            self->FetchWeatherData();
            self->weather_fetching_ = false;
            vTaskDelete(NULL);
        },
        "weather_fetch", 4096, this, 1, nullptr);
}

void CustomLcdDisplay::FetchWeatherData() {
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if (state != kDeviceStateIdle) {
        return;
    }
#if CONFIG_WS185C_ENABLE_NAVIDROME
    if (is_playing_) {
        return;
    }
#endif

    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    if (!network) {
        return;
    }

    ESP_LOGI(TAG, "Auto fetching location & weather...");

    std::string detected_city = "成都市";
    // 1. 探测外网 IP 定位城市
    auto http_loc = network->CreateHttp(0);
    if (http_loc) {
        if (http_loc->Open("GET", "http://myip.ipip.net/json")) {
            auto status = http_loc->GetStatusCode();
            if (status && *status == 200) {
                std::string loc_body = http_loc->ReadAll();
                cJSON* root = cJSON_Parse(loc_body.c_str());
                if (root) {
                    cJSON* data = cJSON_GetObjectItem(root, "data");
                    if (data) {
                        cJSON* loc_arr = cJSON_GetObjectItem(data, "location");
                        if (loc_arr && cJSON_GetArraySize(loc_arr) >= 3) {
                            cJSON* city_item = cJSON_GetArrayItem(loc_arr, 2);
                            if (city_item && city_item->valuestring &&
                                strlen(city_item->valuestring) > 0) {
                                detected_city = city_item->valuestring;
                                if (detected_city.find("市") == std::string::npos) {
                                    detected_city += "市";
                                }
                            }
                        }
                    }
                    cJSON_Delete(root);
                }
            }
            http_loc->Close();
        }
    }

    if (app.GetDeviceState() != kDeviceStateIdle) {
        return;
    }
#if CONFIG_WS185C_ENABLE_NAVIDROME
    if (is_playing_) {
        return;
    }
#endif

    // 2. 根据城市获取天气（成都市默认 citykey: 101270101）
    std::string city_code = "101270101";
    std::string weather_url = "http://t.weather.sojson.com/api/weather/city/" + city_code;

    auto http_w = network->CreateHttp(0);
    if (!http_w) {
        return;
    }

    bool success = false;
    std::string w_body;
    if (http_w->Open("GET", weather_url)) {
        auto status = http_w->GetStatusCode();
        if (status && *status == 200) {
            w_body = http_w->ReadAll();
            success = true;
        }
        http_w->Close();
    }

    if (!success || w_body.empty()) {
        ESP_LOGW(TAG, "Weather fetch failed, keeping current data");
        return;
    }

    cJSON* root = cJSON_Parse(w_body.c_str());
    if (!root) {
        return;
    }

    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (data) {
        TomorrowWeather tw = current_weather_;
        tw.city = detected_city;

        cJSON* wendu = cJSON_GetObjectItem(data, "wendu");
        if (wendu && wendu->valuestring) {
            float f_wendu = atof(wendu->valuestring);
            char tbuf[16];
            snprintf(tbuf, sizeof(tbuf), "%.0f", f_wendu);
            tw.temp = tbuf;
            tw.feels_like = std::string(tbuf) + "°";
        }

        cJSON* shidu = cJSON_GetObjectItem(data, "shidu");
        if (shidu && shidu->valuestring) {
            tw.humidity = shidu->valuestring;
        }

        cJSON* quality = cJSON_GetObjectItem(data, "quality");

        cJSON* forecast = cJSON_GetObjectItem(data, "forecast");
        if (forecast && cJSON_GetArraySize(forecast) > 0) {
            cJSON* today = cJSON_GetArrayItem(forecast, 0);
            if (today) {
                cJSON* type = cJSON_GetObjectItem(today, "type");
                cJSON* fx = cJSON_GetObjectItem(today, "fx");
                cJSON* fl = cJSON_GetObjectItem(today, "fl");
                cJSON* aqi = cJSON_GetObjectItem(today, "aqi");

                std::string cond_str = type && type->valuestring ? type->valuestring : "多云";
                if (fx && fx->valuestring) {
                    tw.wind_dir = fx->valuestring;
                    cond_str += " · ";
                    cond_str += fx->valuestring;
                }
                tw.weather = cond_str;

                if (fl && fl->valuestring) {
                    tw.wind_level = fl->valuestring;
                }

                char aqi_buf[32];
                int aqi_val = aqi ? aqi->valueint : 30;
                const char* q_str = (quality && quality->valuestring) ? quality->valuestring : "优";
                snprintf(aqi_buf, sizeof(aqi_buf), "AQI %d %s", aqi_val, q_str);
                tw.aqi = aqi_buf;
            }
        }

        Application::GetInstance().Schedule([this, tw]() { this->UpdateTomorrowWeather(tw); });
    }

    cJSON_Delete(root);
}

// ========================================================
// 懒加载：第四屏 Cyber Control Center 下拉快捷控制中心
// ========================================================
void CustomLcdDisplay::EnsureSettingsUI() {
    if (settings_ui_created_)
        return;
    settings_ui_created_ = true;

    lv_obj_t* screen = lv_display_get_screen_active(lv_display_get_default());
    settings_overlay_ = lv_obj_create(screen);
    lv_obj_set_size(settings_overlay_, 360, 360);
    lv_obj_align(settings_overlay_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(settings_overlay_, lv_color_hex(0x07090E), 0);
    lv_obj_set_style_border_width(settings_overlay_, 0, 0);
    lv_obj_set_style_pad_all(settings_overlay_, 0, 0);
    lv_obj_set_style_radius(settings_overlay_, 180, 0);
    lv_obj_remove_flag(settings_overlay_, LV_OBJ_FLAG_SCROLLABLE);

    // 设置页面手势监听（从下往上划关闭返回主页）
    auto on_settings_gesture = [](lv_event_t* e) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir == LV_DIR_TOP) {
            auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
            self->ShowHomePage();
        }
    };
    lv_obj_add_event_cb(settings_overlay_, on_settings_gesture, LV_EVENT_GESTURE, this);

    // 1. 顶部标题指引 (y: 26)
    lv_obj_t* title_label = lv_label_create(settings_overlay_);
    lv_label_set_text(title_label, "▲ CONTROL CENTER ▲");
    lv_obj_set_style_text_font(title_label, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0x00E5FF), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 26);

    // 2. 亮度控制模块 (y: 58 ~ 118)
    lv_obj_t* b_row = lv_obj_create(settings_overlay_);
    lv_obj_set_size(b_row, 240, 24);
    lv_obj_align(b_row, LV_ALIGN_TOP_MID, 0, 58);
    lv_obj_set_style_bg_opa(b_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(b_row, 0, 0);
    lv_obj_set_style_pad_all(b_row, 0, 0);
    lv_obj_remove_flag(b_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* b_title = lv_label_create(b_row);
    lv_label_set_text(b_title, "BRIGHTNESS");
    lv_obj_set_style_text_font(b_title, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(b_title, lv_color_hex(0x94A3B8), 0);
    lv_obj_align(b_title, LV_ALIGN_LEFT_MID, 0, 0);

    brightness_val_label_ = lv_label_create(b_row);
    lv_label_set_text(brightness_val_label_, "75%");
    lv_obj_set_style_text_font(brightness_val_label_, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(brightness_val_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(brightness_val_label_, LV_ALIGN_RIGHT_MID, 0, 0);

    // 亮度 Slider (y: 86)
    brightness_slider_ = lv_slider_create(settings_overlay_);
    lv_obj_set_size(brightness_slider_, 240, 22);
    lv_obj_align(brightness_slider_, LV_ALIGN_TOP_MID, 0, 86);
    lv_slider_set_range(brightness_slider_, 10, 100);
    lv_slider_set_value(brightness_slider_, 75, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(brightness_slider_, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_radius(brightness_slider_, 11, LV_PART_MAIN);
    lv_obj_set_style_bg_color(brightness_slider_, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);
    lv_obj_set_style_radius(brightness_slider_, 11, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(brightness_slider_, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_radius(brightness_slider_, 11, LV_PART_KNOB);
    lv_obj_set_style_pad_all(brightness_slider_, 3, LV_PART_KNOB);

    lv_obj_add_event_cb(
        brightness_slider_,
        [](lv_event_t* e) {
            auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
            int val = lv_slider_get_value(self->brightness_slider_);
            auto backlight = Board::GetInstance().GetBacklight();
            if (backlight) {
                backlight->SetBrightness(val);
            }
            char buf[16];
            snprintf(buf, sizeof(buf), "%d%%", val);
            if (self->brightness_val_label_) {
                lv_label_set_text(self->brightness_val_label_, buf);
            }
        },
        LV_EVENT_VALUE_CHANGED, this);

    // 3. 音量控制模块 (y: 126 ~ 186)
    lv_obj_t* v_row = lv_obj_create(settings_overlay_);
    lv_obj_set_size(v_row, 240, 24);
    lv_obj_align(v_row, LV_ALIGN_TOP_MID, 0, 126);
    lv_obj_set_style_bg_opa(v_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(v_row, 0, 0);
    lv_obj_set_style_pad_all(v_row, 0, 0);
    lv_obj_remove_flag(v_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* v_title = lv_label_create(v_row);
    lv_label_set_text(v_title, "VOLUME");
    lv_obj_set_style_text_font(v_title, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(v_title, lv_color_hex(0x94A3B8), 0);
    lv_obj_align(v_title, LV_ALIGN_LEFT_MID, 0, 0);

    volume_val_label_ = lv_label_create(v_row);
    lv_label_set_text(volume_val_label_, "30%");
    lv_obj_set_style_text_font(volume_val_label_, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(volume_val_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(volume_val_label_, LV_ALIGN_RIGHT_MID, 0, 0);

    // 音量 Slider (y: 154)
    volume_slider_ = lv_slider_create(settings_overlay_);
    lv_obj_set_size(volume_slider_, 240, 22);
    lv_obj_align(volume_slider_, LV_ALIGN_TOP_MID, 0, 154);
    lv_slider_set_range(volume_slider_, 0, 100);
    lv_slider_set_value(volume_slider_, 30, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(volume_slider_, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_radius(volume_slider_, 11, LV_PART_MAIN);
    lv_obj_set_style_bg_color(volume_slider_, lv_color_hex(0xF59E0B), LV_PART_INDICATOR);
    lv_obj_set_style_radius(volume_slider_, 11, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(volume_slider_, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_radius(volume_slider_, 11, LV_PART_KNOB);
    lv_obj_set_style_pad_all(volume_slider_, 3, LV_PART_KNOB);

    lv_obj_add_event_cb(
        volume_slider_,
        [](lv_event_t* e) {
            auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
            int val = lv_slider_get_value(self->volume_slider_);
            auto codec = Board::GetInstance().GetAudioCodec();
            if (codec) {
                codec->SetOutputVolume(val);
            }
            char buf[16];
            snprintf(buf, sizeof(buf), "%d%%", val);
            if (self->volume_val_label_) {
                lv_label_set_text(self->volume_val_label_, buf);
            }
        },
        LV_EVENT_VALUE_CHANGED, this);

    lv_obj_add_event_cb(
        volume_slider_,
        [](lv_event_t* e) { Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP); },
        LV_EVENT_RELEASED, this);

    // 4. 网络与 Navidrome 服务状态 (y: 198 ~ 250)
    settings_wifi_label_ = lv_label_create(settings_overlay_);
    lv_obj_set_style_text_font(settings_wifi_label_, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(settings_wifi_label_, lv_color_hex(0x10B981), 0);
    lv_label_set_text(settings_wifi_label_, "● WiFi: Connected");
    lv_obj_align(settings_wifi_label_, LV_ALIGN_TOP_MID, 0, 198);

    settings_navidrome_label_ = lv_label_create(settings_overlay_);
    lv_obj_set_style_text_font(settings_navidrome_label_, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(settings_navidrome_label_, lv_color_hex(0x60A5FA), 0);
    lv_label_set_text(settings_navidrome_label_, "NAVI: --");
    lv_obj_align(settings_navidrome_label_, LV_ALIGN_TOP_MID, 0, 224);

    // 5. 底部收起胶囊按钮 (y: 266, w: 120, h: 36)
    lv_obj_t* close_btn = lv_btn_create(settings_overlay_);
    lv_obj_set_size(close_btn, 120, 36);
    lv_obj_align(close_btn, LV_ALIGN_TOP_MID, 0, 264);
    lv_obj_set_style_radius(close_btn, 18, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_color(close_btn, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(close_btn, 1, 0);
    lv_obj_set_style_pad_all(close_btn, 0, 0);

    lv_obj_t* close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, "▲ CLOSE");
    lv_obj_set_style_text_font(close_label, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(close_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(close_label, LV_ALIGN_CENTER, 0, 0);

    lv_obj_add_event_cb(
        close_btn,
        [](lv_event_t* e) {
            auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
            self->ShowHomePage();
        },
        LV_EVENT_CLICKED, this);
}

void CustomLcdDisplay::UpdateSettingsValues() {
    if (!settings_ui_created_)
        return;

    auto backlight = Board::GetInstance().GetBacklight();
    if (backlight && brightness_slider_ && brightness_val_label_) {
        int b = backlight->brightness();
        lv_slider_set_value(brightness_slider_, b, LV_ANIM_OFF);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", b);
        lv_label_set_text(brightness_val_label_, buf);
    }

    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec && volume_slider_ && volume_val_label_) {
        int v = codec->output_volume();
        lv_slider_set_value(volume_slider_, v, LV_ANIM_OFF);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", v);
        lv_label_set_text(volume_val_label_, buf);
    }

    auto& wifi = WifiManager::GetInstance();
    if (settings_wifi_label_) {
        if (!wifi.IsConfigMode() && !wifi.GetIpAddress().empty()) {
            std::string net_str = "● " + wifi.GetSsid() + " · " + wifi.GetIpAddress();
            lv_label_set_text(settings_wifi_label_, net_str.c_str());
        } else {
            lv_label_set_text(settings_wifi_label_, "○ WiFi: Connecting...");
        }
    }

    if (settings_navidrome_label_) {
#if CONFIG_WS185C_ENABLE_NAVIDROME
        std::string n_str;
        if (!service_config_ || !service_config_->GetNavidromeConfig().IsConfigured()) {
            n_str = "NAVI: 未配置";
        } else if (navidrome_status_ == ServiceStatus::kAuthError) {
            n_str = "NAVI: 认证失败";
        } else if (navidrome_status_ == ServiceStatus::kNetworkError) {
            n_str = "NAVI: 网络不可达";
        } else {
            n_str = "NAVI: " + navidrome_server_;
            if (n_str.find("http://") != std::string::npos) {
                n_str.erase(n_str.find("http://"), 7);
            } else if (n_str.find("https://") != std::string::npos) {
                n_str.erase(n_str.find("https://"), 8);
            }
            if (!n_str.empty() && n_str.back() == '/') {
                n_str.pop_back();
            }
        }
        lv_label_set_text(settings_navidrome_label_, n_str.c_str());
#else
        lv_label_set_text(settings_navidrome_label_, "NAVI: 已禁用");
#endif
    }
}

#if CONFIG_WS185C_ENABLE_NAVIDROME
bool CustomLcdDisplay::ConfigureNavidrome(const std::string& url, const std::string& user,
                                          const std::string& pass, std::string& err_msg) {
    if (!service_config_) {
        err_msg = "service config not initialized";
        return false;
    }
    if (!service_config_->SetNavidromeConfig(url, user, pass, err_msg)) {
        return false;
    }
    bool was_configured = false;
    {
        DisplayLockGuard lock(this);
        auto navi_cfg = service_config_->GetNavidromeConfig();
        navidrome_server_ = navi_cfg.url;
        navidrome_user_ = navi_cfg.user;
        navidrome_pass_ = navi_cfg.pass;
        navidrome_status_ =
            navi_cfg.IsConfigured() ? ServiceStatus::kOk : ServiceStatus::kUnconfigured;
        was_configured = (navidrome_status_ == ServiceStatus::kOk);

        UpdateSettingsValues();
        if (!was_configured) {
            playlist_.clear();
            current_track_idx_ = 0;
            play_elapsed_sec_ = 0;
            UpdatePlayerUI();
        }
    }
    if (was_configured) {
        FetchNavidromePlaylist();
    }
    return true;
}

void CustomLcdDisplay::FetchNavidromePlaylist() {
    if (navidrome_server_.empty() || navidrome_user_.empty() || navidrome_pass_.empty()) {
        DisplayLockGuard lock(this);
        navidrome_status_ = ServiceStatus::kUnconfigured;
        UpdatePlayerUI();
        UpdateSettingsValues();
        return;
    }

    if (navidrome_fetching_)
        return;
    navidrome_fetching_ = true;

    xTaskCreate(
        [](void* param) {
            auto self = static_cast<CustomLcdDisplay*>(param);
            auto network = Board::GetInstance().GetNetwork();
            if (!network) {
                self->navidrome_fetching_ = false;
                self->navidrome_status_ = ServiceStatus::kNetworkError;
                Application::GetInstance().Schedule([self]() {
                    self->UpdatePlayerUI();
                    self->UpdateSettingsValues();
                });
                vTaskDelete(NULL);
                return;
            }

            std::string url = self->navidrome_server_ +
                              "/rest/getRandomSongs.view?u=" + self->navidrome_user_ +
                              "&p=" + self->navidrome_pass_ + "&v=1.16.1&c=xiaozhi&f=json&size=10";

            std::string safe_url = ServiceConfig::RedactUrl(self->navidrome_server_);
            ESP_LOGI(TAG, "Fetching Navidrome songs from: %s", safe_url.c_str());

            auto http = network->CreateHttp(0);
            if (http) {
                http->SetTimeout(8000);
                if (http->Open("GET", url)) {
                    auto status = http->GetStatusCode();
                    if (status && *status == 200) {
                        std::string body = http->ReadAll();
                        cJSON* root = cJSON_Parse(body.c_str());
                        if (root) {
                            cJSON* resp = cJSON_GetObjectItem(root, "subsonic-response");
                            if (resp) {
                                cJSON* status_item = cJSON_GetObjectItem(resp, "status");
                                if (status_item && status_item->valuestring &&
                                    strcmp(status_item->valuestring, "failed") == 0) {
                                    self->navidrome_status_ = ServiceStatus::kAuthError;
                                } else {
                                    cJSON* random_songs = cJSON_GetObjectItem(resp, "randomSongs");
                                    if (random_songs) {
                                        cJSON* song_arr = cJSON_GetObjectItem(random_songs, "song");
                                        if (song_arr && cJSON_IsArray(song_arr)) {
                                            int count = cJSON_GetArraySize(song_arr);
                                            std::vector<MusicTrack> new_list;
                                            for (int i = 0; i < count; i++) {
                                                cJSON* s = cJSON_GetArrayItem(song_arr, i);
                                                if (!s)
                                                    continue;
                                                MusicTrack t;
                                                cJSON* id = cJSON_GetObjectItem(s, "id");
                                                cJSON* title = cJSON_GetObjectItem(s, "title");
                                                cJSON* artist = cJSON_GetObjectItem(s, "artist");
                                                cJSON* duration =
                                                    cJSON_GetObjectItem(s, "duration");
                                                if (id && id->valuestring)
                                                    t.id = id->valuestring;
                                                if (title && title->valuestring)
                                                    t.title = title->valuestring;
                                                if (artist && artist->valuestring)
                                                    t.artist = artist->valuestring;
                                                if (duration)
                                                    t.duration_sec = duration->valueint;
                                                t.source = "NAVIDROME";
                                                t.stream_url = waveshare185c::ServiceConfig::
                                                    GenerateNavidromeDirectUrl(
                                                        self->navidrome_server_, t.id,
                                                        self->navidrome_user_,
                                                        self->navidrome_pass_, true);
                                                new_list.push_back(std::move(t));
                                            }
                                            if (!new_list.empty()) {
                                                ESP_LOGI(TAG,
                                                         "Navidrome loaded %d songs successfully",
                                                         (int)new_list.size());
                                                self->navidrome_status_ = ServiceStatus::kOk;
                                                Application::GetInstance().Schedule(
                                                    [self,
                                                     new_list = std::move(new_list)]() mutable {
                                                        DisplayLockGuard lock(self);
                                                        self->default_playlist_ = new_list;
                                                        if (!self->is_playing_ && !self->is_search_playlist_) {
                                                            self->playlist_ = std::move(new_list);
                                                            self->current_track_idx_ = 0;
                                                            self->play_elapsed_sec_ = 0;
                                                        }
                                                        self->UpdatePlayerUI();
                                                        self->UpdateSettingsValues();
                                                    });
                                            }
                                        }
                                    }
                                }
                            }
                            cJSON_Delete(root);
                        }
                    } else if (status && (*status == 401 || *status == 403)) {
                        self->navidrome_status_ = ServiceStatus::kAuthError;
                    } else {
                        self->navidrome_status_ = ServiceStatus::kNetworkError;
                    }
                    http->Close();
                } else {
                    self->navidrome_status_ = ServiceStatus::kNetworkError;
                }
            } else {
                self->navidrome_status_ = ServiceStatus::kNetworkError;
            }

            Application::GetInstance().Schedule([self]() {
                self->UpdatePlayerUI();
                self->UpdateSettingsValues();
            });

            self->navidrome_fetching_ = false;
            vTaskDelete(NULL);
        },
        "navi_fetch", 4096, this, 3, NULL);
}

// 辅助：轻量 URL 编码
static std::string UrlEncodeQuery(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (auto c : value) {
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::uppercase << std::setw(2) << int((unsigned char)c);
        }
    }
    return escaped.str();
}

bool CustomLcdDisplay::ResolvePlaybackTargetName(const std::string& name, int& out_index,
                                                 std::string& out_name, std::string& err) const {
    auto trim = [](const std::string& s) {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) {
            return std::string();
        }
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    };
    auto lower_of = [](std::string s) {
        // 必须走 unsigned char：设备名是 UTF-8 中文，字节 >127 时 char 为负，
        // 直接传给 tolower 属于越界未定义行为
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(::tolower(c)); });
        return s;
    };

    std::string key = trim(name);
    std::string lowered = lower_of(key);

    // 本机 / 本地 / 未指定：一律走板载喇叭
    if (lowered.empty() || lowered == "local" || lowered == "speaker" || lowered == "本机" ||
        lowered == "本机喇叭" || lowered == "本地" || lowered == "本地喇叭" ||
        lowered == "这台设备" || lowered == "小智") {
        out_index = -1;
        out_name = "本机";
        return true;
    }

    auto devices = waveshare185c::DlnaController::GetInstance().GetDevices();

    // 第一轮：设备名包含用户说出的关键词（「小爱音箱」匹配「客厅的小爱音箱 Pro」）
    int matched = -1;
    for (size_t i = 0; i < devices.size(); ++i) {
        if (lower_of(devices[i].name).find(lowered) != std::string::npos) {
            matched = (int)i;
            break;
        }
    }
    // 第二轮：关键词包含设备名（用户说「客厅的小爱音箱放首歌」，设备名是「小爱音箱」）
    // 仅在设备名不短于 2 字符时启用，避免单字符设备名匹配到任意句子
    if (matched < 0) {
        for (size_t i = 0; i < devices.size(); ++i) {
            std::string dn = lower_of(devices[i].name);
            if (dn.size() >= 2 && lowered.find(dn) != std::string::npos) {
                matched = (int)i;
                break;
            }
        }
    }

    if (matched < 0) {
        std::string avail;
        for (const auto& d : devices) {
            if (!avail.empty()) {
                avail += "、";
            }
            avail += d.name;
        }
        err = "未找到名为「" + key + "」的播放设备";
        err += avail.empty() ? "，且局域网内暂未发现任何 DLNA 设备" : "，当前可用设备：" + avail;
        return false;
    }

    out_index = matched;
    out_name = devices[matched].name;
    return true;
}

void CustomLcdDisplay::ApplyPlaybackTarget(int target_index) {
    auto& dlna = waveshare185c::DlnaController::GetInstance();
    int old_target = dlna.GetTargetIndex();
    if (old_target == target_index) {
        return;
    }
    dlna.SetTargetIndex(target_index);
    ESP_LOGI(TAG, "Playback target changed: %d -> %d", old_target, target_index);

    // 离开旧 DLNA 设备时同步发送 Stop，避免它自己继续播下去
    if (old_target >= 0) {
        xTaskCreate(
            [](void* param) {
                int dev = (int)(intptr_t)param;
                waveshare185c::DlnaController::GetInstance().Stop(dev);
                vTaskDelete(NULL);
            },
            "dlna_stop_old", 3072, (void*)(intptr_t)old_target, 3, NULL);
    }
}

std::string CustomLcdDisplay::SearchAndPlayMusic(const std::string& keyword,
                                                const std::string& artist,
                                                const std::string& title,
                                                const std::string& candidates,
                                                const std::string& target) {
    if (navidrome_server_.empty() || navidrome_user_.empty() || navidrome_pass_.empty()) {
        return "{\"status\": \"error\", \"message\": \"Navidrome 服务未配置，无法在曲库中检索\"}";
    }

    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        return "{\"status\": \"error\", \"message\": \"网络不可用，无法连接 Navidrome 服务器\"}";
    }

    // 先解析播放设备再检索：设备名不存在就直接失败，不去动当前播放列表
    int target_index = -1;
    std::string target_name;
    std::string target_err;
    if (!ResolvePlaybackTargetName(target, target_index, target_name, target_err)) {
        ESP_LOGW(TAG, "SearchAndPlayMusic: %s", target_err.c_str());
        cJSON* err_json = cJSON_CreateObject();
        cJSON_AddStringToObject(err_json, "status", "device_not_found");
        cJSON_AddStringToObject(err_json, "message", target_err.c_str());
        char* err_printed = cJSON_PrintUnformatted(err_json);
        std::string err_str = err_printed ? err_printed : "{\"status\": \"device_not_found\"}";
        cJSON_free(err_printed);
        cJSON_Delete(err_json);
        return err_str;
    }

    ESP_LOGI(TAG,
             "SearchAndPlayMusic invoked: artist=[%s], title=[%s], keyword=[%s], candidates=[%s], "
             "target=[%s]",
             artist.c_str(), title.c_str(), keyword.c_str(), candidates.c_str(), target_name.c_str());

    bool is_random_mode = false;
    std::string lower_kw = keyword;
    std::transform(lower_kw.begin(), lower_kw.end(), lower_kw.begin(), ::tolower);
    if (lower_kw == "random" || lower_kw == "随机" || lower_kw == "随便" || lower_kw == "随意" ||
        lower_kw == "推荐" || lower_kw == "一点" || lower_kw == "来点音乐") {
        is_random_mode = true;
    } else if (keyword.empty() && artist.empty() && title.empty() && candidates.empty()) {
        is_random_mode = true;
    }

    std::vector<MusicTrack> matched_tracks;
    std::set<std::string> seen_ids;

    if (is_random_mode) {
        std::string rand_url = navidrome_server_ +
                               "/rest/getRandomSongs.view?u=" + navidrome_user_ +
                               "&p=" + navidrome_pass_ +
                               "&v=1.16.1&c=xiaozhi&f=json&size=12";
        ESP_LOGI(TAG, "Navidrome random mode triggered: fetching random songs");
        auto http = network->CreateHttp(0);
        if (http) {
            http->SetTimeout(2500);
            if (http->Open("GET", rand_url)) {
                auto status = http->GetStatusCode();
                if (status && *status == 200) {
                    std::string body = http->ReadAll();
                    cJSON* root = cJSON_Parse(body.c_str());
                    if (root) {
                        cJSON* resp = cJSON_GetObjectItem(root, "subsonic-response");
                        if (resp) {
                            cJSON* random_songs = cJSON_GetObjectItem(resp, "randomSongs");
                            if (random_songs) {
                                cJSON* song_arr = cJSON_GetObjectItem(random_songs, "song");
                                if (song_arr && cJSON_IsArray(song_arr)) {
                                    int count = cJSON_GetArraySize(song_arr);
                                    for (int i = 0; i < count; ++i) {
                                        cJSON* s = cJSON_GetArrayItem(song_arr, i);
                                        if (!s) continue;
                                        cJSON* c_id = cJSON_GetObjectItem(s, "id");
                                        cJSON* c_title = cJSON_GetObjectItem(s, "title");
                                        cJSON* c_artist = cJSON_GetObjectItem(s, "artist");
                                        cJSON* c_dur = cJSON_GetObjectItem(s, "duration");
                                        if (!c_id || !c_id->valuestring) continue;
                                        MusicTrack t;
                                        t.id = c_id->valuestring;
                                        t.title = (c_title && c_title->valuestring) ? c_title->valuestring : "未知曲目";
                                        t.artist = (c_artist && c_artist->valuestring) ? c_artist->valuestring : "未知艺术家";
                                        t.duration_sec = c_dur ? c_dur->valueint : 0;
                                        t.source = "NAVIDROME";
                                        t.stream_url = waveshare185c::ServiceConfig::GenerateNavidromeDirectUrl(
                                            navidrome_server_, t.id, navidrome_user_, navidrome_pass_, true);
                                        matched_tracks.push_back(std::move(t));
                                    }
                                }
                            }
                        }
                        cJSON_Delete(root);
                    }
                }
                http->Close();
            }
        }
        if (matched_tracks.empty() && !default_playlist_.empty()) {
            matched_tracks = default_playlist_;
        }
    } else {
        // 1. 构建轻量检索策略：优先单次精准，最多只执行 2 次查询，单次超时 1800ms
        std::vector<std::pair<std::string, int>> search_queries; // {query_str, requested_count}

        if (!artist.empty() && !title.empty()) {
            search_queries.push_back({artist + " " + title, 10});
            search_queries.push_back({artist, 30});
        } else if (!artist.empty()) {
            // 用户指定歌手（如陈奕迅/刘欢），一次性拉取 30 首，随后在本地做随机洗牌
            search_queries.push_back({artist, 30});
        } else if (!title.empty()) {
            search_queries.push_back({title, 10});
        } else if (!keyword.empty()) {
            search_queries.push_back({keyword, 15});
        }

        // 解析 candidates（情绪场景由大模型拓展出的经典曲目）
        if (!candidates.empty() && search_queries.empty()) {
            std::stringstream ss(candidates);
            std::string item;
            int cand_limit = 0;
            while (std::getline(ss, item, ',') && cand_limit < 3) {
                while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.erase(0, 1);
                while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.pop_back();
                if (!item.empty()) {
                    search_queries.push_back({item, 10});
                    cand_limit++;
                }
            }
        }

        if (search_queries.empty()) {
            return "{\"status\": \"error\", \"message\": \"未提供任何有效的检索关键词\"}";
        }

        auto str_contains_ic = [](const std::string& haystack, const std::string& needle) {
            if (needle.empty()) return true;
            auto it = std::search(
                haystack.begin(), haystack.end(),
                needle.begin(), needle.end(),
                [](char ch1, char ch2) { return std::tolower((unsigned char)ch1) == std::tolower((unsigned char)ch2); }
            );
            return (it != haystack.end());
        };

        for (size_t q_idx = 0; q_idx < search_queries.size(); ++q_idx) {
            const auto& q_item = search_queries[q_idx];
            std::string encoded_q = UrlEncodeQuery(q_item.first);
            std::string search_url = navidrome_server_ + "/rest/search3.view?u=" + navidrome_user_ +
                                     "&p=" + navidrome_pass_ +
                                     "&v=1.16.1&c=xiaozhi&f=json&songCount=" +
                                     std::to_string(q_item.second) + "&query=" + encoded_q;

            ESP_LOGI(TAG, "Navidrome search3 query [%d/%d]: %s (songCount=%d)",
                     (int)(q_idx + 1), (int)search_queries.size(), q_item.first.c_str(), q_item.second);

            auto http = network->CreateHttp(0);
            if (!http) continue;
            http->SetTimeout(1800); // 严格限制 1.8 秒超时，绝不阻塞看门狗
            if (http->Open("GET", search_url)) {
                auto status = http->GetStatusCode();
                if (status && *status == 200) {
                    std::string body = http->ReadAll();
                    cJSON* root = cJSON_Parse(body.c_str());
                    if (root) {
                        cJSON* resp = cJSON_GetObjectItem(root, "subsonic-response");
                        if (resp) {
                            cJSON* s_res = cJSON_GetObjectItem(resp, "searchResult3");
                            if (s_res) {
                                cJSON* song_arr = cJSON_GetObjectItem(s_res, "song");
                                if (song_arr && cJSON_IsArray(song_arr)) {
                                    int count = cJSON_GetArraySize(song_arr);
                                    for (int i = 0; i < count; ++i) {
                                        cJSON* s = cJSON_GetArrayItem(song_arr, i);
                                        if (!s) continue;
                                        cJSON* c_id = cJSON_GetObjectItem(s, "id");
                                        cJSON* c_title = cJSON_GetObjectItem(s, "title");
                                        cJSON* c_artist = cJSON_GetObjectItem(s, "artist");
                                        cJSON* c_dur = cJSON_GetObjectItem(s, "duration");
                                        if (!c_id || !c_id->valuestring) continue;

                                        std::string song_id = c_id->valuestring;
                                        if (seen_ids.count(song_id)) continue;

                                        MusicTrack t;
                                        t.id = song_id;
                                        t.title = (c_title && c_title->valuestring) ? c_title->valuestring : "未知曲目";
                                        t.artist = (c_artist && c_artist->valuestring) ? c_artist->valuestring : "未知艺术家";
                                        t.duration_sec = c_dur ? c_dur->valueint : 0;

                                        // 相关度检查：过滤无关的模糊命中
                                        bool relevant = true;
                                        if (!artist.empty()) {
                                            relevant = str_contains_ic(t.artist, artist) || str_contains_ic(t.title, artist);
                                        } else if (!title.empty()) {
                                            relevant = str_contains_ic(t.title, title);
                                        } else if (!keyword.empty()) {
                                            relevant = str_contains_ic(t.title, keyword) || str_contains_ic(t.artist, keyword);
                                        }
                                        if (!relevant) {
                                            continue;
                                        }

                                        seen_ids.insert(song_id);
                                        t.source = "NAVIDROME";
                                        t.stream_url = waveshare185c::ServiceConfig::GenerateNavidromeDirectUrl(
                                            navidrome_server_, t.id, navidrome_user_, navidrome_pass_, true);
                                        matched_tracks.push_back(std::move(t));
                                    }
                                }
                            }
                        }
                        cJSON_Delete(root);
                    }
                }
                http->Close();
            }

            // 只要当前查询命中了曲目，就提前结束，保证极速响应
            if (!matched_tracks.empty()) {
                break;
            }
        }
    }

    if (matched_tracks.empty()) {
        ESP_LOGW(TAG, "Search completed: no matching tracks found in Navidrome for artist=[%s], title=[%s], keyword=[%s]",
                 artist.c_str(), title.c_str(), keyword.c_str());
        return "{\"status\": \"not_found\", \"message\": \"在私有 Navidrome 曲库中未找到相关歌曲，已保持当前播放列表不变。\"}";
    }

    // 若命中了多首曲目（例如指定歌手陈奕迅返回了多首），进行随机洗牌（Shuffle），每次生成不一样的生动歌单
    if (matched_tracks.size() > 1) {
        std::random_device rd;
        std::default_random_engine rng(rd());
        std::shuffle(matched_tracks.begin(), matched_tracks.end(), rng);
    }

    // 截断上限为 12 首，保护内存
    if (matched_tracks.size() > 12) {
        matched_tracks.resize(12);
    }

    ESP_LOGI(TAG, "Search completed: found %d tracks (shuffled), starting playback with: %s - %s",
             (int)matched_tracks.size(), matched_tracks[0].title.c_str(), matched_tracks[0].artist.c_str());

    // 构建给大模型的详细结果 JSON，明确告知已在硬件端起播，指示大模型保持沉默无需朗读
    cJSON* res_json = cJSON_CreateObject();
    cJSON_AddStringToObject(res_json, "status", "success");
    cJSON_AddNumberToObject(res_json, "matched_count", (int)matched_tracks.size());
    cJSON_AddStringToObject(res_json, "playing_title", matched_tracks[0].title.c_str());
    cJSON_AddStringToObject(res_json, "playing_artist", matched_tracks[0].artist.c_str());
    cJSON_AddStringToObject(res_json, "playback_device", target_name.c_str());
    cJSON_AddStringToObject(res_json, "instruction", "Playback started. Remain completely silent and do NOT read any text.");
    cJSON* song_names = cJSON_AddArrayToObject(res_json, "songs");
    for (const auto& t : matched_tracks) {
        std::string item_desc = t.title + " - " + t.artist;
        cJSON_AddItemToArray(song_names, cJSON_CreateString(item_desc.c_str()));
    }
    char* printed = cJSON_PrintUnformatted(res_json);
    std::string result_str = printed ? printed : "{\"status\": \"success\"}";
    cJSON_free(printed);
    cJSON_Delete(res_json);

    // 调度到主线程：更新播放列表并立即起播、切至播放屏，自动退出对话模式
    Application::GetInstance().Schedule([this, new_list = std::move(matched_tracks),
                                         target_index]() mutable {
        DisplayLockGuard lock(this);
        // 保留原日常随机列表副本（如果之前为空）
        if (default_playlist_.empty() && !playlist_.empty()) {
            default_playlist_ = playlist_;
        }
        playlist_ = std::move(new_list);
        current_track_idx_ = 0;
        play_elapsed_sec_ = 0;
        is_search_playlist_ = true;

        // 1. 隐藏小智唤醒对话遮罩层，直接展示全屏音乐播放器
        HideWakeupOverlay();
        ShowPlayerPage();

        // 2. 退出小智对话模式，打断后续 TTS 播报，避免多余声音打扰音乐
        auto& app = Application::GetInstance();
        app.AbortSpeaking(kAbortReasonNone);
        app.GetAudioService().ResetDecoder();
        app.SetDeviceState(kDeviceStateIdle);

        // 3. 先落到本次点歌指定的播放设备（StartNavidromeStream 据此决定本地流还是 DLNA 投送）
        ApplyPlaybackTarget(target_index);

        // 4. 立即起播音乐
        StartNavidromeStream(0);
    });

    return result_str;
}
#endif

#if CONFIG_WS185C_ENABLE_BESZEL
// ========================================================
// 懒加载：第五屏 Beszel VPS 集群机能监控看板 (Server Telemetry HUD)
// ========================================================
void CustomLcdDisplay::EnsureServerUI() {
    if (server_ui_created_)
        return;
    server_ui_created_ = true;

    auto screen = lv_screen_active();
    server_overlay_ = lv_obj_create(screen);
    lv_obj_set_size(server_overlay_, 360, 360);
    lv_obj_set_pos(server_overlay_, 0, 0);
    lv_obj_set_style_bg_color(server_overlay_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(server_overlay_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(server_overlay_, 0, 0);
    lv_obj_set_style_pad_all(server_overlay_, 0, 0);
    lv_obj_set_style_radius(server_overlay_, 180, 0);
    lv_obj_remove_flag(server_overlay_, LV_OBJ_FLAG_SCROLLABLE);

    // 绑定向下滑动返回主屏，左右滑动切换 VPS
    auto on_server_gesture = [](lv_event_t* e) {
        auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir == LV_DIR_BOTTOM) {
            self->ShowHomePage();
        } else if (dir == LV_DIR_LEFT) {
            self->NextVpsNode();
        } else if (dir == LV_DIR_RIGHT) {
            self->PrevVpsNode();
        }
    };
    lv_obj_add_event_cb(server_overlay_, on_server_gesture, LV_EVENT_GESTURE, this);

    // 点击屏幕也可切换下一台 VPS
    lv_obj_add_event_cb(
        server_overlay_,
        [](lv_event_t* e) {
            auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
            self->NextVpsNode();
        },
        LV_EVENT_CLICKED, this);

    // 1. 外圈精密刻度装饰环 (Outer Precision Bezel Track, D: 346)
    lv_obj_t* outer_track = lv_obj_create(server_overlay_);
    lv_obj_set_size(outer_track, 346, 346);
    lv_obj_align(outer_track, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(outer_track, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(outer_track, lv_color_hex(0x00F3FF), 0);
    lv_obj_set_style_border_width(outer_track, 1, 0);
    lv_obj_set_style_border_opa(outer_track, 50, 0);
    lv_obj_set_style_radius(outer_track, 173, 0);
    lv_obj_remove_flag(outer_track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(outer_track, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(outer_track, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // 1.1 四向准星刻度线 (12点/6点/9点/3点)
    auto create_tick = [this](int w, int h, lv_align_t align, int x, int y) {
        lv_obj_t* t = lv_obj_create(server_overlay_);
        lv_obj_set_size(t, w, h);
        lv_obj_align(t, align, x, y);
        lv_obj_set_style_bg_color(t, lv_color_hex(0x00F3FF), 0);
        lv_obj_set_style_border_width(t, 0, 0);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(t, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(t, LV_OBJ_FLAG_GESTURE_BUBBLE);
        return t;
    };
    create_tick(2, 7, LV_ALIGN_TOP_MID, 0, 4);
    create_tick(2, 7, LV_ALIGN_BOTTOM_MID, 0, -4);
    create_tick(7, 2, LV_ALIGN_LEFT_MID, 4, 0);
    create_tick(7, 2, LV_ALIGN_RIGHT_MID, -4, 0);

    // 2. 同心三环 (Concentric Multi-Ring Engine)
    // 2.1 RING 1 (OUTER): LOAD (D: 326, R: 163, stroke: 6)
    server_arc_load_ = lv_arc_create(server_overlay_);
    lv_obj_set_size(server_arc_load_, 326, 326);
    lv_obj_align(server_arc_load_, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_rotation(server_arc_load_, 270);
    lv_arc_set_bg_angles(server_arc_load_, 0, 360);
    lv_arc_set_range(server_arc_load_, 0, 100);
    lv_arc_set_value(server_arc_load_, 0);
    lv_obj_remove_style(server_arc_load_, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(server_arc_load_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(server_arc_load_, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(server_arc_load_, lv_color_hex(0x350A15), LV_PART_MAIN);
    lv_obj_set_style_arc_width(server_arc_load_, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(server_arc_load_, lv_color_hex(0xFF2D55), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(server_arc_load_, true, LV_PART_INDICATOR);
    lv_obj_add_flag(server_arc_load_, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(server_arc_load_, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // 2.2 RING 2 (MIDDLE): RAM USAGE (D: 300, R: 150, stroke: 5)
    server_arc_ram_ = lv_arc_create(server_overlay_);
    lv_obj_set_size(server_arc_ram_, 300, 300);
    lv_obj_align(server_arc_ram_, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_rotation(server_arc_ram_, 270);
    lv_arc_set_bg_angles(server_arc_ram_, 0, 360);
    lv_arc_set_range(server_arc_ram_, 0, 100);
    lv_arc_set_value(server_arc_ram_, 0);
    lv_obj_remove_style(server_arc_ram_, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(server_arc_ram_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(server_arc_ram_, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_color(server_arc_ram_, lv_color_hex(0x05263D), LV_PART_MAIN);
    lv_obj_set_style_arc_width(server_arc_ram_, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(server_arc_ram_, lv_color_hex(0x00F3FF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(server_arc_ram_, true, LV_PART_INDICATOR);
    lv_obj_add_flag(server_arc_ram_, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(server_arc_ram_, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // 2.3 RING 3 (INNER): DISK USAGE (D: 274, R: 137, stroke: 5 - 放大的最内环)
    server_arc_disk_ = lv_arc_create(server_overlay_);
    lv_obj_set_size(server_arc_disk_, 274, 274);
    lv_obj_align(server_arc_disk_, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_rotation(server_arc_disk_, 270);
    lv_arc_set_bg_angles(server_arc_disk_, 0, 360);
    lv_arc_set_range(server_arc_disk_, 0, 100);
    lv_arc_set_value(server_arc_disk_, 0);
    lv_obj_remove_style(server_arc_disk_, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(server_arc_disk_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(server_arc_disk_, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_color(server_arc_disk_, lv_color_hex(0x28103E), LV_PART_MAIN);
    lv_obj_set_style_arc_width(server_arc_disk_, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(server_arc_disk_, lv_color_hex(0xB054FF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(server_arc_disk_, true, LV_PART_INDICATOR);
    lv_obj_add_flag(server_arc_disk_, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(server_arc_disk_, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // 内环装饰细圈 (D: 252)
    lv_obj_t* inner_deco = lv_obj_create(server_overlay_);
    lv_obj_set_size(inner_deco, 252, 252);
    lv_obj_align(inner_deco, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(inner_deco, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(inner_deco, lv_color_hex(0x00F3FF), 0);
    lv_obj_set_style_border_width(inner_deco, 1, 0);
    lv_obj_set_style_border_opa(inner_deco, 40, 0);
    lv_obj_set_style_radius(inner_deco, 126, 0);
    lv_obj_remove_flag(inner_deco, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(inner_deco, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(inner_deco, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // 3. PURE DATA MINIMAL CORE (中心内容区: w: 210, h: 210)
    lv_obj_t* core = lv_obj_create(server_overlay_);
    lv_obj_set_size(core, 210, 210);
    lv_obj_align(core, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(core, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(core, 0, 0);
    lv_obj_set_style_pad_all(core, 0, 0);
    lv_obj_remove_flag(core, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(core, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(core, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // 3.1 TOP MICRO HEADER: ● BESZEL [0/0]
    lv_obj_t* hdr_box = lv_obj_create(core);
    lv_obj_set_size(hdr_box, 200, 20);
    lv_obj_align(hdr_box, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_bg_opa(hdr_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr_box, 0, 0);
    lv_obj_set_style_pad_all(hdr_box, 0, 0);
    lv_obj_set_flex_flow(hdr_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr_box, 6, 0);
    lv_obj_remove_flag(hdr_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(hdr_box, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(hdr_box, LV_OBJ_FLAG_GESTURE_BUBBLE);

    server_status_dot_ = lv_obj_create(hdr_box);
    lv_obj_set_size(server_status_dot_, 6, 6);
    lv_obj_set_style_radius(server_status_dot_, 3, 0);
    lv_obj_set_style_bg_color(server_status_dot_, lv_color_hex(0x00F3FF), 0);
    lv_obj_set_style_border_width(server_status_dot_, 0, 0);

    server_name_label_ = lv_label_create(hdr_box);
    lv_obj_set_style_text_font(server_name_label_, &font_noto_sans_basic_16_4, 0);
    lv_obj_set_style_text_color(server_name_label_, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(server_name_label_, "BESZEL");

    server_counter_label_ = lv_label_create(hdr_box);
    lv_obj_set_style_text_font(server_counter_label_, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(server_counter_label_, lv_color_hex(0x00D2FF), 0);
    lv_label_set_text(server_counter_label_, "[0/0]");

    // 3.2 HERO LOAD METRIC (-- / SYSTEM LOAD)
    server_load_val_ = lv_label_create(core);
    lv_obj_set_style_text_font(server_load_val_, &font_maison_neue_book_26, 0);
    lv_obj_set_style_text_color(server_load_val_, lv_color_hex(0xFF2D55), 0);
    lv_label_set_text(server_load_val_, "--");
    lv_obj_align(server_load_val_, LV_ALIGN_TOP_MID, 0, 32);

    lv_obj_t* load_tag = lv_label_create(core);
    lv_obj_set_style_text_font(load_tag, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(load_tag, lv_color_hex(0xFF6B8B), 0);
    lv_label_set_text(load_tag, "SYSTEM LOAD");
    lv_obj_align(load_tag, LV_ALIGN_TOP_MID, 0, 64);

    // 3.3 THREE-LINE COMPACT TELEMETRY MATRIX
    lv_obj_t* matrix = lv_obj_create(core);
    lv_obj_set_size(matrix, 172, 60);
    lv_obj_align(matrix, LV_ALIGN_TOP_MID, 0, 88);
    lv_obj_set_style_bg_opa(matrix, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(matrix, 0, 0);
    lv_obj_set_style_pad_all(matrix, 0, 0);
    lv_obj_remove_flag(matrix, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(matrix, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(matrix, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // Line 1: RAM
    lv_obj_t* r1_dot = lv_obj_create(matrix);
    lv_obj_set_size(r1_dot, 4, 4);
    lv_obj_set_style_radius(r1_dot, 2, 0);
    lv_obj_set_style_bg_color(r1_dot, lv_color_hex(0x00F3FF), 0);
    lv_obj_set_style_border_width(r1_dot, 0, 0);
    lv_obj_align(r1_dot, LV_ALIGN_TOP_LEFT, 0, 6);

    lv_obj_t* r1_lbl = lv_label_create(matrix);
    lv_obj_set_style_text_font(r1_lbl, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(r1_lbl, lv_color_hex(0x00F3FF), 0);
    lv_label_set_text(r1_lbl, "RAM");
    lv_obj_align(r1_lbl, LV_ALIGN_TOP_LEFT, 10, 0);

    server_ram_val_ = lv_label_create(matrix);
    lv_obj_set_style_text_font(server_ram_val_, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(server_ram_val_, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(server_ram_val_, "--");
    lv_obj_align(server_ram_val_, LV_ALIGN_TOP_RIGHT, 0, 0);

    // Line 2: DISK
    lv_obj_t* r2_dot = lv_obj_create(matrix);
    lv_obj_set_size(r2_dot, 4, 4);
    lv_obj_set_style_radius(r2_dot, 2, 0);
    lv_obj_set_style_bg_color(r2_dot, lv_color_hex(0xB054FF), 0);
    lv_obj_set_style_border_width(r2_dot, 0, 0);
    lv_obj_align(r2_dot, LV_ALIGN_TOP_LEFT, 0, 24);

    lv_obj_t* r2_lbl = lv_label_create(matrix);
    lv_obj_set_style_text_font(r2_lbl, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(r2_lbl, lv_color_hex(0xB054FF), 0);
    lv_label_set_text(r2_lbl, "DISK");
    lv_obj_align(r2_lbl, LV_ALIGN_TOP_LEFT, 10, 18);

    server_disk_val_ = lv_label_create(matrix);
    lv_obj_set_style_text_font(server_disk_val_, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(server_disk_val_, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(server_disk_val_, "--");
    lv_obj_align(server_disk_val_, LV_ALIGN_TOP_RIGHT, 0, 18);

    // Line 3: NET
    lv_obj_t* r3_dot = lv_obj_create(matrix);
    lv_obj_set_size(r3_dot, 4, 4);
    lv_obj_set_style_radius(r3_dot, 2, 0);
    lv_obj_set_style_bg_color(r3_dot, lv_color_hex(0x4EDEA3), 0);
    lv_obj_set_style_border_width(r3_dot, 0, 0);
    lv_obj_align(r3_dot, LV_ALIGN_TOP_LEFT, 0, 42);

    lv_obj_t* r3_lbl = lv_label_create(matrix);
    lv_obj_set_style_text_font(r3_lbl, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(r3_lbl, lv_color_hex(0x4EDEA3), 0);
    lv_label_set_text(r3_lbl, "NET");
    lv_obj_align(r3_lbl, LV_ALIGN_TOP_LEFT, 10, 36);

    server_net_val_ = lv_label_create(matrix);
    lv_obj_set_style_text_font(server_net_val_, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(server_net_val_, lv_color_hex(0x4EDEA3), 0);
    lv_label_set_text(server_net_val_, "--");
    lv_obj_align(server_net_val_, LV_ALIGN_TOP_RIGHT, 0, 36);

    // 3.4 BOTTOM FOOTER: BESZEL HUD
    server_footer_label_ = lv_label_create(core);
    lv_obj_set_style_text_font(server_footer_label_, &font_maison_neue_book_14, 0);
    lv_obj_set_style_text_color(server_footer_label_, lv_color_hex(0x00D2FF), 0);
    lv_label_set_text(server_footer_label_, "BESZEL HUD");
    lv_obj_align(server_footer_label_, LV_ALIGN_TOP_MID, 0, 166);
}

void CustomLcdDisplay::UpdateServerUI() {
    if (!server_ui_created_)
        return;

    bool is_configured = service_config_ && service_config_->GetBeszelConfig().IsConfigured();

    if (!is_configured) {
        if (server_name_label_)
            lv_label_set_text(server_name_label_, "未配置服务");
        if (server_counter_label_)
            lv_label_set_text(server_counter_label_, "[0/0]");
        if (server_load_val_)
            lv_label_set_text(server_load_val_, "--");
        if (server_ram_val_)
            lv_label_set_text(server_ram_val_, "--");
        if (server_disk_val_)
            lv_label_set_text(server_disk_val_, "--");
        if (server_net_val_)
            lv_label_set_text(server_net_val_, "--");
        if (server_footer_label_) {
            lv_label_set_text(server_footer_label_, "UNCONFIGURED | BESZEL");
            lv_obj_set_style_text_color(server_footer_label_, lv_color_hex(0x888888), 0);
        }
        if (server_status_dot_) {
            lv_obj_set_style_bg_color(server_status_dot_, lv_color_hex(0x888888), 0);
        }
        if (server_arc_load_)
            lv_arc_set_value(server_arc_load_, 0);
        if (server_arc_ram_)
            lv_arc_set_value(server_arc_ram_, 0);
        if (server_arc_disk_)
            lv_arc_set_value(server_arc_disk_, 0);
        return;
    }

    if (beszel_status_ == ServiceStatus::kAuthError) {
        if (server_name_label_)
            lv_label_set_text(server_name_label_, "认证失败");
        if (server_counter_label_)
            lv_label_set_text(server_counter_label_, "[!/!]");
        if (server_load_val_)
            lv_label_set_text(server_load_val_, "ERR");
        if (server_ram_val_)
            lv_label_set_text(server_ram_val_, "AUTH");
        if (server_disk_val_)
            lv_label_set_text(server_disk_val_, "FAIL");
        if (server_net_val_)
            lv_label_set_text(server_net_val_, "401/403");
        if (server_footer_label_) {
            lv_label_set_text(server_footer_label_, "AUTH ERROR | BESZEL");
            lv_obj_set_style_text_color(server_footer_label_, lv_color_hex(0xFF2D55), 0);
        }
        if (server_status_dot_) {
            lv_obj_set_style_bg_color(server_status_dot_, lv_color_hex(0xFF2D55), 0);
        }
        if (server_arc_load_)
            lv_arc_set_value(server_arc_load_, 0);
        if (server_arc_ram_)
            lv_arc_set_value(server_arc_ram_, 0);
        if (server_arc_disk_)
            lv_arc_set_value(server_arc_disk_, 0);
        return;
    }

    if (beszel_status_ == ServiceStatus::kNetworkError) {
        if (server_name_label_)
            lv_label_set_text(server_name_label_, "网络不可达");
        if (server_counter_label_)
            lv_label_set_text(server_counter_label_, "[!/!]");
        if (server_load_val_)
            lv_label_set_text(server_load_val_, "ERR");
        if (server_ram_val_)
            lv_label_set_text(server_ram_val_, "NET");
        if (server_disk_val_)
            lv_label_set_text(server_disk_val_, "FAIL");
        if (server_net_val_)
            lv_label_set_text(server_net_val_, "TIMEOUT");
        if (server_footer_label_) {
            lv_label_set_text(server_footer_label_, "NET ERROR | BESZEL");
            lv_obj_set_style_text_color(server_footer_label_, lv_color_hex(0xFF2D55), 0);
        }
        if (server_status_dot_) {
            lv_obj_set_style_bg_color(server_status_dot_, lv_color_hex(0xFF2D55), 0);
        }
        if (server_arc_load_)
            lv_arc_set_value(server_arc_load_, 0);
        if (server_arc_ram_)
            lv_arc_set_value(server_arc_ram_, 0);
        if (server_arc_disk_)
            lv_arc_set_value(server_arc_disk_, 0);
        return;
    }

    if (vps_nodes_.empty()) {
        if (server_name_label_)
            lv_label_set_text(server_name_label_, "等待数据同步");
        if (server_counter_label_)
            lv_label_set_text(server_counter_label_, "[0/0]");
        if (server_load_val_)
            lv_label_set_text(server_load_val_, "--");
        if (server_ram_val_)
            lv_label_set_text(server_ram_val_, "--");
        if (server_disk_val_)
            lv_label_set_text(server_disk_val_, "--");
        if (server_net_val_)
            lv_label_set_text(server_net_val_, "--");
        if (server_footer_label_) {
            lv_label_set_text(server_footer_label_, "FETCHING | BESZEL");
            lv_obj_set_style_text_color(server_footer_label_, lv_color_hex(0x00D2FF), 0);
        }
        if (server_status_dot_) {
            lv_obj_set_style_bg_color(server_status_dot_, lv_color_hex(0x00F3FF), 0);
        }
        if (server_arc_load_)
            lv_arc_set_value(server_arc_load_, 0);
        if (server_arc_ram_)
            lv_arc_set_value(server_arc_ram_, 0);
        if (server_arc_disk_)
            lv_arc_set_value(server_arc_disk_, 0);
        return;
    }

    if (current_vps_idx_ >= vps_nodes_.size()) {
        current_vps_idx_ = 0;
    }

    const auto& node = vps_nodes_[current_vps_idx_];

    // 更新主机名
    if (server_name_label_) {
        lv_label_set_text(server_name_label_, node.name.c_str());
    }

    // 更新状态小圆点
    if (server_status_dot_) {
        if (node.status == "up") {
            lv_obj_set_style_bg_color(server_status_dot_, lv_color_hex(0x00F3FF), 0);
        } else {
            lv_obj_set_style_bg_color(server_status_dot_, lv_color_hex(0xFF2D55), 0);
        }
    }

    // 更新序号 [1/3]
    if (server_counter_label_) {
        char cnt_buf[32];
        snprintf(cnt_buf, sizeof(cnt_buf), "[%zu/%zu]", current_vps_idx_ + 1, vps_nodes_.size());
        lv_label_set_text(server_counter_label_, cnt_buf);
    }

    // 1. 更新 LOAD (数值与外圆弧)
    if (server_load_val_) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.2f", node.load);
        lv_label_set_text(server_load_val_, buf);
    }
    if (server_arc_load_) {
        int load_pct = (int)(node.load * 25.0f);
        if (load_pct > 100)
            load_pct = 100;
        if (load_pct < 0)
            load_pct = 0;
        lv_arc_set_value(server_arc_load_, load_pct);
    }

    // 2. 更新 RAM (中圆弧与数值)
    if (server_ram_val_) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f%%", node.mem);
        lv_label_set_text(server_ram_val_, buf);
    }
    if (server_arc_ram_) {
        int ram_pct = (int)node.mem;
        if (ram_pct > 100)
            ram_pct = 100;
        if (ram_pct < 0)
            ram_pct = 0;
        lv_arc_set_value(server_arc_ram_, ram_pct);
    }

    // 3. 更新 DISK (内圆弧与数值)
    if (server_disk_val_) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f%%", node.disk);
        lv_label_set_text(server_disk_val_, buf);
    }
    if (server_arc_disk_) {
        int disk_pct = (int)node.disk;
        if (disk_pct > 100)
            disk_pct = 100;
        if (disk_pct < 0)
            disk_pct = 0;
        lv_arc_set_value(server_arc_disk_, disk_pct);
    }

    // 4. 更新 NET SPEED
    if (server_net_val_) {
        char buf[24];
        if (node.net_bytes_sec >= 1048576.0f) {
            snprintf(buf, sizeof(buf), "%.1f MB/s", node.net_bytes_sec / 1048576.0f);
        } else if (node.net_bytes_sec >= 1024.0f) {
            snprintf(buf, sizeof(buf), "%.1f KB/s", node.net_bytes_sec / 1024.0f);
        } else {
            snprintf(buf, sizeof(buf), "%.0f B/s", node.net_bytes_sec);
        }
        lv_label_set_text(server_net_val_, buf);
    }

    // 5. 更新底部状态
    if (server_footer_label_) {
        if (node.status == "up") {
            lv_label_set_text(server_footer_label_, "ONLINE  |  BESZEL HUD");
            lv_obj_set_style_text_color(server_footer_label_, lv_color_hex(0x00D2FF), 0);
        } else {
            lv_label_set_text(server_footer_label_, "OFFLINE  |  BESZEL HUD");
            lv_obj_set_style_text_color(server_footer_label_, lv_color_hex(0xFF5252), 0);
        }
    }
}

void CustomLcdDisplay::CheckAndTriggerServerFetch() {
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if (state != kDeviceStateIdle) {
        return;
    }

    if (beszel_hub_url_.empty() || beszel_user_.empty() || beszel_pass_.empty()) {
        beszel_status_ = ServiceStatus::kUnconfigured;
        return;  // 未配置状态绝不产生网络请求
    }

#if CONFIG_WS185C_ENABLE_NAVIDROME
    if (is_playing_) {
        return;
    }
#endif

    int interval_s = beszel_fetch_interval_s_ > 0 ? beszel_fetch_interval_s_ : 15;
    // 当不在服务器监控页面时，拉取间隔放缓至 120 秒，避免持续占用网络 socket 与内存
    if (current_page_ != 3) {
        interval_s = 120;
    }

    int64_t now_sec = esp_timer_get_time() / 1000000;
    if (server_fetching_) {
        return;
    }

    if (last_server_fetch_sec_ != 0 && (now_sec - last_server_fetch_sec_) < interval_s) {
        return;
    }

    ESP_LOGI(TAG, "Triggering Beszel periodic fetch (interval: %ds)...", interval_s);
    last_server_fetch_sec_ = now_sec;
    server_fetching_ = true;

    xTaskCreate(
        [](void* arg) {
            auto self = static_cast<CustomLcdDisplay*>(arg);
            self->FetchBeszelData();
            self->server_fetching_ = false;
            ESP_LOGI(TAG, "Beszel fetch cycle completed.");
            vTaskDelete(NULL);
        },
        "beszel_fetch", 4096, this, 1, nullptr);
}

void CustomLcdDisplay::FetchBeszelData() {
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() != kDeviceStateIdle) {
        return;
    }
#if CONFIG_WS185C_ENABLE_NAVIDROME
    if (is_playing_) {
        return;
    }
#endif

    if (beszel_hub_url_.empty() || beszel_user_.empty() || beszel_pass_.empty()) {
        beszel_status_ = ServiceStatus::kUnconfigured;
        return;
    }

    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    if (!network) {
        beszel_status_ = ServiceStatus::kNetworkError;
        Application::GetInstance().Schedule([this]() {
            if (current_page_ == 3 && server_ui_created_) {
                UpdateServerUI();
            }
        });
        return;
    }

    std::string safe_url = ServiceConfig::RedactUrl(beszel_hub_url_);
    ESP_LOGI(TAG, "Fetching Beszel VPS Cluster data from %s...", safe_url.c_str());

    // 1. 如果没有 token，或者上次请求被 401 拒绝，则先登录鉴权
    if (beszel_token_.empty()) {
        auto http_auth = network->CreateHttp(0);
        if (http_auth) {
            cJSON* auth_json = cJSON_CreateObject();
            cJSON_AddStringToObject(auth_json, "identity", beszel_user_.c_str());
            cJSON_AddStringToObject(auth_json, "password", beszel_pass_.c_str());
            char* payload_str = cJSON_PrintUnformatted(auth_json);
            std::string payload = payload_str ? payload_str : "";
            cJSON_free(payload_str);
            cJSON_Delete(auth_json);

            http_auth->SetHeader("Content-Type", "application/json");
            http_auth->SetContent(std::move(payload));
            std::string auth_url = beszel_hub_url_ + "/api/collections/users/auth-with-password";
            if (http_auth->Open("POST", auth_url)) {
                auto status = http_auth->GetStatusCode();
                ESP_LOGI(TAG, "Beszel auth HTTP status: %d", status ? *status : -1);
                if (status && *status == 200) {
                    std::string resp = http_auth->ReadAll();
                    cJSON* root = cJSON_Parse(resp.c_str());
                    if (root) {
                        cJSON* tok = cJSON_GetObjectItem(root, "token");
                        if (tok && tok->valuestring) {
                            beszel_token_ = tok->valuestring;
                            ESP_LOGI(TAG, "Beszel auth success, token acquired.");
                        }
                        cJSON_Delete(root);
                    }
                } else if (status && (*status == 400 || *status == 401 || *status == 403)) {
                    ESP_LOGW(TAG, "Beszel auth failed (auth error), status: %d", *status);
                    beszel_status_ = ServiceStatus::kAuthError;
                } else {
                    ESP_LOGW(TAG, "Beszel auth failed, status: %d", status ? *status : -1);
                    beszel_status_ = ServiceStatus::kNetworkError;
                }
                http_auth->Close();
            } else {
                beszel_status_ = ServiceStatus::kNetworkError;
            }
        } else {
            beszel_status_ = ServiceStatus::kNetworkError;
        }
    }

    if (beszel_token_.empty() || app.GetDeviceState() != kDeviceStateIdle) {
        Application::GetInstance().Schedule([this]() {
            if (current_page_ == 3 && server_ui_created_) {
                UpdateServerUI();
            }
        });
        return;
    }
#if CONFIG_WS185C_ENABLE_NAVIDROME
    if (is_playing_) {
        return;
    }
#endif

    // 2. 使用 Token 获取节点实时数据 (PocketBase 直接传 token，不能带 Bearer 前缀)
    auto http_data = network->CreateHttp(0);
    if (http_data) {
        http_data->SetHeader("Authorization", beszel_token_);
        std::string records_url = beszel_hub_url_ + "/api/collections/systems/records";
        auto opened = http_data->Open("GET", records_url);
        if (opened) {
            auto status = http_data->GetStatusCode();
            ESP_LOGI(TAG, "Beszel records HTTP status: %d", status ? *status : -1);
            if (status && *status == 200) {
                std::string body = http_data->ReadAll();
                ESP_LOGI(TAG, "Beszel records body read (%zu bytes)", body.size());
                if (app.GetDeviceState() != kDeviceStateIdle) {
                    http_data->Close();
                    return;
                }
                beszel_status_ = ServiceStatus::kOk;
                ParseAndApplyBeszel(body);
            } else if (status && (*status == 401 || *status == 403)) {
                ESP_LOGW(TAG, "Beszel token expired or forbidden, clearing token.");
                beszel_token_.clear();
                beszel_status_ = ServiceStatus::kAuthError;
            } else {
                ESP_LOGW(TAG, "Beszel fetch records failed, status: %d", status ? *status : -1);
                beszel_status_ = ServiceStatus::kNetworkError;
            }
            http_data->Close();
        } else {
            ESP_LOGW(TAG, "Beszel Open GET records failed");
            beszel_status_ = ServiceStatus::kNetworkError;
        }
    } else {
        beszel_status_ = ServiceStatus::kNetworkError;
    }

    Application::GetInstance().Schedule([this]() {
        if (current_page_ == 3 && server_ui_created_) {
            UpdateServerUI();
        }
    });
}

void CustomLcdDisplay::ParseAndApplyBeszel(const std::string& body) {
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        ESP_LOGW(TAG, "Beszel JSON parse failed");
        return;
    }

    cJSON* items = cJSON_GetObjectItem(root, "items");
    if (items && cJSON_IsArray(items)) {
        std::vector<VpsNode> new_nodes;
        int count = cJSON_GetArraySize(items);
        for (int i = 0; i < count; i++) {
            cJSON* it = cJSON_GetArrayItem(items, i);
            if (!it)
                continue;
            VpsNode node;
            cJSON* name_item = cJSON_GetObjectItem(it, "name");
            if (name_item && name_item->valuestring) {
                node.name = name_item->valuestring;
            }
            cJSON* status_item = cJSON_GetObjectItem(it, "status");
            if (status_item && status_item->valuestring) {
                node.status = status_item->valuestring;
            }
            cJSON* host_item = cJSON_GetObjectItem(it, "host");
            if (host_item && host_item->valuestring) {
                node.host = host_item->valuestring;
            }
            cJSON* info = cJSON_GetObjectItem(it, "info");
            if (info) {
                // 1. 解析负载 la (1分钟平均负载)
                cJSON* la_item = cJSON_GetObjectItem(info, "la");
                if (la_item) {
                    if (cJSON_IsArray(la_item) && cJSON_GetArraySize(la_item) > 0) {
                        cJSON* first_la = cJSON_GetArrayItem(la_item, 0);
                        if (first_la && cJSON_IsNumber(first_la)) {
                            node.load = (float)first_la->valuedouble;
                        }
                    } else if (cJSON_IsNumber(la_item)) {
                        node.load = (float)la_item->valuedouble;
                    }
                }
                // 2. 解析实时网络带宽 bb (bytes/s)
                cJSON* bb_item = cJSON_GetObjectItem(info, "bb");
                if (bb_item && cJSON_IsNumber(bb_item)) {
                    node.net_bytes_sec = (float)bb_item->valuedouble;
                }
                // 3. 解析内存百分比 mp
                cJSON* mp_item = cJSON_GetObjectItem(info, "mp");
                if (mp_item && cJSON_IsNumber(mp_item)) {
                    node.mem = (float)mp_item->valuedouble;
                }
                // 4. 解析磁盘百分比 dp
                cJSON* dp_item = cJSON_GetObjectItem(info, "dp");
                if (dp_item && cJSON_IsNumber(dp_item)) {
                    node.disk = (float)dp_item->valuedouble;
                }
                // 保留 CPU 字段
                cJSON* cpu_item = cJSON_GetObjectItem(info, "cpu");
                if (cpu_item && cJSON_IsNumber(cpu_item)) {
                    node.cpu = (float)cpu_item->valuedouble;
                }
            }
            new_nodes.push_back(node);
        }

        if (!new_nodes.empty()) {
            ESP_LOGI(TAG, "Beszel parsed %d VPS nodes successfully", (int)new_nodes.size());
            Application::GetInstance().Schedule([this, new_nodes = std::move(new_nodes)]() mutable {
                DisplayLockGuard lock(this);
                vps_nodes_ = std::move(new_nodes);
                if (current_vps_idx_ >= vps_nodes_.size()) {
                    current_vps_idx_ = 0;
                }
                if (current_page_ == 3 && server_ui_created_) {
                    UpdateServerUI();
                }
            });
        }
    }
    cJSON_Delete(root);
}

bool CustomLcdDisplay::ConfigureBeszel(const std::string& url, const std::string& user,
                                       const std::string& pass, int32_t fetch_interval_s,
                                       int32_t rotate_interval_s, std::string& err_msg) {
    if (!service_config_) {
        err_msg = "service config not initialized";
        return false;
    }
    if (!service_config_->SetBeszelConfig(url, user, pass, fetch_interval_s, rotate_interval_s,
                                          err_msg)) {
        return false;
    }
    bool trigger_fetch = false;
    {
        DisplayLockGuard lock(this);
        auto bsz_cfg = service_config_->GetBeszelConfig();
        beszel_hub_url_ = bsz_cfg.url;
        beszel_user_ = bsz_cfg.user;
        beszel_pass_ = bsz_cfg.pass;
        beszel_fetch_interval_s_ = bsz_cfg.fetch_interval_s;
        beszel_rotate_interval_s_ = bsz_cfg.rotate_interval_s;
        beszel_token_.clear();
        beszel_status_ = bsz_cfg.IsConfigured() ? ServiceStatus::kOk : ServiceStatus::kUnconfigured;
        if (!bsz_cfg.IsConfigured()) {
            vps_nodes_.clear();
            current_vps_idx_ = 0;
        }
        if (server_ui_created_) {
            UpdateServerUI();
        }
        trigger_fetch = (beszel_status_ == ServiceStatus::kOk);
    }
    if (trigger_fetch) {
        last_server_fetch_sec_ = 0;
        CheckAndTriggerServerFetch();
    }
    return true;
}

void CustomLcdDisplay::TriggerBeszelFetch() {
    last_server_fetch_sec_ = 0;
    CheckAndTriggerServerFetch();
}
#endif

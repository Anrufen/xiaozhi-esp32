#include <unistd.h>
#include "lvgl.h"
#include "src/drivers/sdl/lv_sdl_window.h"
#include "src/drivers/sdl/lv_sdl_mouse.h"
#include "src/libs/lodepng/lodepng.h"
#include "src/libs/tiny_ttf/lv_tiny_ttf.h"
#include "material_symbols.h"
#include "player_icons.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

LV_FONT_DECLARE(font_noto_sans_basic_30_4);
LV_FONT_DECLARE(font_noto_sans_basic_20_4);
LV_FONT_DECLARE(font_noto_sans_basic_16_4);
LV_FONT_DECLARE(font_material_symbols_16_4);
LV_FONT_DECLARE(font_material_symbols_30_4);
LV_FONT_DECLARE(font_puhui_basic_20_4);
LV_FONT_DECLARE(font_maison_neue_book_26);

static lv_font_t* font_huge = NULL;    // 34px (Time / Temp)
static lv_font_t* font_large = NULL;   // 20px (Price / Value)
static lv_font_t* font_medium = NULL;  // 13px (Titles / Subtitles)
static lv_font_t* font_small = NULL;   // 10px (Pills / Micro tags)

static lv_obj_t* page_home = NULL;
static lv_obj_t* page_weather = NULL;
static lv_obj_t* page_fonts = NULL;
static lv_obj_t* page_player = NULL;
static lv_obj_t* dot_home = NULL;
static lv_obj_t* dot_weather = NULL;
static int current_page = 0;

void SwitchPage(int page_idx) {
    current_page = page_idx;
    if (page_fonts) lv_obj_add_flag(page_fonts, LV_OBJ_FLAG_HIDDEN);
    if (page_player) lv_obj_add_flag(page_player, LV_OBJ_FLAG_HIDDEN);
    if (page_idx == 0) {
        lv_obj_remove_flag(page_home, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(page_weather, LV_OBJ_FLAG_HIDDEN);
        if (dot_home) {
            lv_obj_set_style_bg_color(dot_home, lv_color_hex(0x00D2FF), 0);
            lv_obj_set_style_bg_opa(dot_home, LV_OPA_COVER, 0);
        }
        if (dot_weather) {
            lv_obj_set_style_bg_color(dot_weather, lv_color_hex(0x4A5568), 0);
            lv_obj_set_style_bg_opa(dot_weather, LV_OPA_50, 0);
        }
    } else if (page_idx == 1) {
        lv_obj_add_flag(page_home, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(page_weather, LV_OBJ_FLAG_HIDDEN);
        if (dot_home) {
            lv_obj_set_style_bg_color(dot_home, lv_color_hex(0x4A5568), 0);
            lv_obj_set_style_bg_opa(dot_home, LV_OPA_50, 0);
        }
        if (dot_weather) {
            lv_obj_set_style_bg_color(dot_weather, lv_color_hex(0x00D2FF), 0);
            lv_obj_set_style_bg_opa(dot_weather, LV_OPA_COVER, 0);
        }
    } else if (page_idx == 2) {
        if (page_home) lv_obj_add_flag(page_home, LV_OBJ_FLAG_HIDDEN);
        if (page_weather) lv_obj_add_flag(page_weather, LV_OBJ_FLAG_HIDDEN);
        if (page_fonts) lv_obj_remove_flag(page_fonts, LV_OBJ_FLAG_HIDDEN);
    } else if (page_idx == 3) {
        if (page_home) lv_obj_add_flag(page_home, LV_OBJ_FLAG_HIDDEN);
        if (page_weather) lv_obj_add_flag(page_weather, LV_OBJ_FLAG_HIDDEN);
        if (page_player) lv_obj_remove_flag(page_player, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_dot_home_click(lv_event_t* e) { (void)e; SwitchPage(0); }
static void on_dot_weather_click(lv_event_t* e) { (void)e; SwitchPage(1); }

static const lv_point_precise_t kSparklinePoints[] = {
    {0, 20}, {35, 18}, {70, 23}, {105, 14}, {140, 16}, {175, 8}, {210, 12}, {240, 4}
};

static lv_obj_t* time_lbl = NULL;
static lv_obj_t* price_lbl = NULL;

void BuildHomePage(lv_obj_t* parent) {
    page_home = lv_obj_create(parent);
    lv_obj_set_size(page_home, 360, 360);
    lv_obj_set_pos(page_home, 0, 0);
    lv_obj_set_style_bg_opa(page_home, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page_home, 0, 0);
    lv_obj_set_style_pad_all(page_home, 0, 0);
    lv_obj_set_scrollbar_mode(page_home, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(page_home, LV_OBJ_FLAG_SCROLLABLE);

    // 1. 顶部同步圆弧胶囊
    lv_obj_t* pill = lv_obj_create(page_home);
    lv_obj_set_size(pill, 180, 22);
    lv_obj_align(pill, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_set_style_radius(pill, 11, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(0x131A26), 0);
    lv_obj_set_style_border_color(pill, lv_color_hex(0x233147), 0);
    lv_obj_set_style_border_width(pill, 1, 0);
    lv_obj_set_style_pad_all(pill, 0, 0);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* pill_label = lv_label_create(pill);
    lv_label_set_text(pill_label, "#00E5FF ●# #94A3B8 10m刷新 · 刚刚同步#");
    lv_label_set_recolor(pill_label, true);
    lv_obj_set_style_text_font(pill_label, font_small, 0);
    lv_obj_center(pill_label);

    // 2. 主时间行
    lv_obj_t* time_box = lv_obj_create(page_home);
    lv_obj_set_size(time_box, 300, 44);
    lv_obj_align(time_box, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_bg_opa(time_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(time_box, 0, 0);
    lv_obj_set_style_pad_all(time_box, 0, 0);
    lv_obj_set_flex_flow(time_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(time_box, LV_OBJ_FLAG_SCROLLABLE);

    time_lbl = lv_label_create(time_box);
    lv_label_set_text(time_lbl, "13:31");
    lv_obj_set_style_text_font(time_lbl, font_huge, 0);
    lv_obj_set_style_text_color(time_lbl, lv_color_hex(0x00E5FF), 0);

    lv_obj_t* sec_lbl = lv_label_create(time_box);
    lv_label_set_text(sec_lbl, " 11s");
    lv_obj_set_style_text_font(sec_lbl, font_medium, 0);
    lv_obj_set_style_text_color(sec_lbl, lv_color_hex(0x7DD3FC), 0);
    lv_obj_set_style_pad_bottom(sec_lbl, 6, 0);

    // 3. 日期行
    lv_obj_t* date_lbl = lv_label_create(page_home);
    lv_label_set_text(date_lbl, "10月2日 星期五 (FRI)");
    lv_obj_set_style_text_font(date_lbl, font_small, 0);
    lv_obj_set_style_text_color(date_lbl, lv_color_hex(0x94A3B8), 0);
    lv_obj_align(date_lbl, LV_ALIGN_TOP_MID, 0, 92);

    // 4. 股票中心卡片（宽幅 286px，充分利用屏幕宽度，圆角 12px）
    lv_obj_t* card = lv_obj_create(page_home);
    lv_obj_set_size(card, 286, 172);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 116);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // 卡片第一行：股票名称 + 股票代码 (已去掉港股汉字)
    lv_obj_t* row1 = lv_obj_create(card);
    lv_obj_set_size(row1, 266, 20);
    lv_obj_set_style_bg_opa(row1, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row1, 0, 0);
    lv_obj_set_style_pad_all(row1, 0, 0);
    lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row1, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row1, 8, 0);
    lv_obj_remove_flag(row1, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* name_lbl = lv_label_create(row1);
    lv_label_set_text(name_lbl, "#DFE2EE 联想集团#");
    lv_label_set_recolor(name_lbl, true);
    lv_obj_set_style_text_font(name_lbl, font_medium, 0);

    lv_obj_t* code_lbl = lv_label_create(row1);
    lv_label_set_text(code_lbl, "#38BDF8 00992.HK#");
    lv_label_set_recolor(code_lbl, true);
    lv_obj_set_style_text_font(code_lbl, font_medium, 0);

    // 卡片第二行：超大价格 + 涨跌幅胶囊（居中对齐，间距舒适，绝无重叠）
    lv_obj_t* row2 = lv_obj_create(card);
    lv_obj_set_size(row2, 266, 32);
    lv_obj_set_style_bg_opa(row2, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row2, 0, 0);
    lv_obj_set_style_pad_all(row2, 0, 0);
    lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row2, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row2, 12, 0);
    lv_obj_align(row2, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_remove_flag(row2, LV_OBJ_FLAG_SCROLLABLE);

    price_lbl = lv_label_create(row2);
    lv_label_set_text(price_lbl, "HK$ 34.12");
    lv_obj_set_style_text_font(price_lbl, font_large, 0);
    lv_obj_set_style_text_color(price_lbl, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t* chg_badge = lv_obj_create(row2);
    lv_obj_set_size(chg_badge, 96, 24);
    lv_obj_set_style_radius(chg_badge, 6, 0);
    lv_obj_set_style_bg_color(chg_badge, lv_color_hex(0x7F1D1D), 0);
    lv_obj_set_style_border_width(chg_badge, 0, 0);
    lv_obj_set_style_pad_all(chg_badge, 0, 0);
    lv_obj_remove_flag(chg_badge, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* chg_txt = lv_label_create(chg_badge);
    lv_label_set_text(chg_txt, "-1.22% (-0.42)");
    lv_obj_set_style_text_font(chg_txt, font_small, 0);
    lv_obj_set_style_text_color(chg_txt, lv_color_hex(0xFCA5A5), 0);
    lv_obj_center(chg_txt);

    // 卡片第三行：区间直接展示（直接展示数值区间，无冗余前缀）
    lv_obj_t* row4 = lv_obj_create(card);
    lv_obj_set_size(row4, 266, 20);
    lv_obj_set_style_bg_opa(row4, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row4, 0, 0);
    lv_obj_set_style_pad_all(row4, 0, 0);
    lv_obj_set_flex_flow(row4, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row4, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(row4, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_remove_flag(row4, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* range_lbl = lv_label_create(row4);
    lv_label_set_text(range_lbl, "33.74 - 35.24");
    lv_obj_set_style_text_font(range_lbl, font_small, 0);
    lv_obj_set_style_text_color(range_lbl, lv_color_hex(0x94A3B8), 0);

    // 5. 底部下次刷新指示器
    lv_obj_t* bottom_bar = lv_obj_create(page_home);
    lv_obj_set_size(bottom_bar, 200, 20);
    lv_obj_align(bottom_bar, LV_ALIGN_TOP_MID, 0, 298);
    lv_obj_set_style_bg_opa(bottom_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bottom_bar, 0, 0);
    lv_obj_set_style_pad_all(bottom_bar, 0, 0);
    lv_obj_set_flex_flow(bottom_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bottom_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bottom_bar, 8, 0);
    lv_obj_remove_flag(bottom_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* next_lbl = lv_label_create(bottom_bar);
    lv_label_set_text(next_lbl, "下次更新");
    lv_obj_set_style_text_font(next_lbl, font_small, 0);
    lv_obj_set_style_text_color(next_lbl, lv_color_hex(0x64748B), 0);

    lv_obj_t* prog = lv_bar_create(bottom_bar);
    lv_obj_set_size(prog, 50, 4);
    lv_bar_set_value(prog, 45, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(prog, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_bg_color(prog, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);

    lv_obj_t* cd_lbl = lv_label_create(bottom_bar);
    lv_label_set_text(cd_lbl, "08:49");
    lv_obj_set_style_text_font(cd_lbl, font_small, 0);
    lv_obj_set_style_text_color(cd_lbl, lv_color_hex(0x00E5FF), 0);
}

void BuildWeatherPage(lv_obj_t* parent) {
    page_weather = lv_obj_create(parent);
    lv_obj_set_size(page_weather, 360, 360);
    lv_obj_set_pos(page_weather, 0, 0);
    lv_obj_set_style_bg_opa(page_weather, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page_weather, 0, 0);
    lv_obj_set_style_pad_all(page_weather, 0, 0);
    lv_obj_set_scrollbar_mode(page_weather, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(page_weather, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(page_weather, LV_OBJ_FLAG_HIDDEN);

    // 1. 顶部位置标签（单独汉字，无背景胶囊，居中）
    lv_obj_t* loc_lbl = lv_label_create(page_weather);
    lv_label_set_text(loc_lbl, "成都市 · 武侯区");
    lv_obj_set_style_text_font(loc_lbl, font_medium, 0);
    lv_obj_set_style_text_color(loc_lbl, lv_color_hex(0xCBD5E1), 0);
    lv_obj_align(loc_lbl, LV_ALIGN_TOP_MID, 0, 16);

    // 2. 天气状况与 AQI 标签行 (y: 42, h: 20)
    lv_obj_t* cond_box = lv_obj_create(page_weather);
    lv_obj_set_size(cond_box, 260, 20);
    lv_obj_align(cond_box, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_set_style_bg_opa(cond_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cond_box, 0, 0);
    lv_obj_set_style_pad_all(cond_box, 0, 0);
    lv_obj_set_flex_flow(cond_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cond_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cond_box, 10, 0);
    lv_obj_remove_flag(cond_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* cond_lbl = lv_label_create(cond_box);
    lv_label_set_text(cond_lbl, "多云转晴 · 微风");
    lv_obj_set_style_text_font(cond_lbl, font_medium, 0);
    lv_obj_set_style_text_color(cond_lbl, lv_color_hex(0x94A3B8), 0);

    lv_obj_t* aqi_badge = lv_obj_create(cond_box);
    lv_obj_set_size(aqi_badge, 76, 18);
    lv_obj_set_style_radius(aqi_badge, 9, 0);
    lv_obj_set_style_bg_color(aqi_badge, lv_color_hex(0x064E3B), 0);
    lv_obj_set_style_border_width(aqi_badge, 0, 0);
    lv_obj_set_style_pad_all(aqi_badge, 0, 0);
    lv_obj_remove_flag(aqi_badge, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* aqi_lbl = lv_label_create(aqi_badge);
    lv_label_set_text(aqi_lbl, "AQI 32 优");
    lv_obj_set_style_text_font(aqi_lbl, font_small, 0);
    lv_obj_set_style_text_color(aqi_lbl, lv_color_hex(0x34D399), 0);
    lv_obj_center(aqi_lbl);

    // 3. 中央超大主温度与湿度整合 (y: 68, h: 44)
    lv_obj_t* temp_box = lv_obj_create(page_weather);
    lv_obj_set_size(temp_box, 240, 44);
    lv_obj_align(temp_box, LV_ALIGN_TOP_MID, 0, 68);
    lv_obj_set_style_bg_opa(temp_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(temp_box, 0, 0);
    lv_obj_set_style_pad_all(temp_box, 0, 0);
    lv_obj_set_flex_flow(temp_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(temp_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(temp_box, 4, 0);
    lv_obj_remove_flag(temp_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* temp_lbl = lv_label_create(temp_box);
    lv_label_set_text(temp_lbl, "23");
    lv_obj_set_style_text_font(temp_lbl, font_huge, 0);
    lv_obj_set_style_text_color(temp_lbl, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t* unit_lbl = lv_label_create(temp_box);
    lv_label_set_text(unit_lbl, "°C");
    lv_obj_set_style_text_font(unit_lbl, font_large, 0);
    lv_obj_set_style_text_color(unit_lbl, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_pad_bottom(unit_lbl, 4, 0);

    lv_obj_t* hum_lbl = lv_label_create(temp_box);
    lv_label_set_text(hum_lbl, "湿度 72%");
    lv_obj_set_style_text_font(hum_lbl, font_medium, 0);
    lv_obj_set_style_text_color(hum_lbl, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_pad_bottom(hum_lbl, 6, 0);
    lv_obj_set_style_margin_left(hum_lbl, 10, 0);

    // 4. 三联环境遥测卡片 (y: 122, h: 66，已移除风向、紫外线、气压汉字)
    lv_obj_t* trio_box = lv_obj_create(page_weather);
    lv_obj_set_size(trio_box, 286, 66);
    lv_obj_align(trio_box, LV_ALIGN_TOP_MID, 0, 122);
    lv_obj_set_style_bg_color(trio_box, lv_color_hex(0x111827), 0);
    lv_obj_set_style_border_color(trio_box, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_border_width(trio_box, 1, 0);
    lv_obj_set_style_radius(trio_box, 10, 0);
    lv_obj_set_style_pad_all(trio_box, 4, 0);
    lv_obj_set_flex_flow(trio_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(trio_box, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(trio_box, LV_OBJ_FLAG_SCROLLABLE);

    // 指标1：2级 + 12 km/h
    lv_obj_t* item1 = lv_obj_create(trio_box);
    lv_obj_set_size(item1, 88, 56);
    lv_obj_set_style_bg_color(item1, lv_color_hex(0x161F2E), 0);
    lv_obj_set_style_border_width(item1, 0, 0);
    lv_obj_set_style_radius(item1, 6, 0);
    lv_obj_set_style_pad_all(item1, 4, 0);
    lv_obj_set_flex_flow(item1, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(item1, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(item1, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* i1_v = lv_label_create(item1);
    lv_label_set_text(i1_v, "2级");
    lv_obj_set_style_text_font(i1_v, font_large, 0);
    lv_obj_set_style_text_color(i1_v, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t* i1_s = lv_label_create(item1);
    lv_label_set_text(i1_s, "12 km/h");
    lv_obj_set_style_text_font(i1_s, font_small, 0);
    lv_obj_set_style_text_color(i1_s, lv_color_hex(0x38BDF8), 0);

    // 指标2：弱 + UV 2
    lv_obj_t* item2 = lv_obj_create(trio_box);
    lv_obj_set_size(item2, 88, 56);
    lv_obj_set_style_bg_color(item2, lv_color_hex(0x161F2E), 0);
    lv_obj_set_style_border_width(item2, 0, 0);
    lv_obj_set_style_radius(item2, 6, 0);
    lv_obj_set_style_pad_all(item2, 4, 0);
    lv_obj_set_flex_flow(item2, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(item2, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(item2, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* i2_v = lv_label_create(item2);
    lv_label_set_text(i2_v, "弱");
    lv_obj_set_style_text_font(i2_v, font_large, 0);
    lv_obj_set_style_text_color(i2_v, lv_color_hex(0xFBBF24), 0);

    lv_obj_t* i2_s = lv_label_create(item2);
    lv_label_set_text(i2_s, "UV 2");
    lv_obj_set_style_text_font(i2_s, font_small, 0);
    lv_obj_set_style_text_color(i2_s, lv_color_hex(0xFDE68A), 0);

    // 指标3：1016 + hPa
    lv_obj_t* item3 = lv_obj_create(trio_box);
    lv_obj_set_size(item3, 88, 56);
    lv_obj_set_style_bg_color(item3, lv_color_hex(0x161F2E), 0);
    lv_obj_set_style_border_width(item3, 0, 0);
    lv_obj_set_style_radius(item3, 6, 0);
    lv_obj_set_flex_flow(item3, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(item3, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(item3, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* i3_v = lv_label_create(item3);
    lv_label_set_text(i3_v, "1016");
    lv_obj_set_style_text_font(i3_v, font_large, 0);
    lv_obj_set_style_text_color(i3_v, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t* i3_s = lv_label_create(item3);
    lv_label_set_text(i3_s, "hPa");
    lv_obj_set_style_text_font(i3_s, font_small, 0);
    lv_obj_set_style_text_color(i3_s, lv_color_hex(0x94A3B8), 0);

    // 6. 底部 3 时段预报条 (y: 202, h: 88)
    lv_obj_t* fore_box = lv_obj_create(page_weather);
    lv_obj_set_size(fore_box, 290, 88);
    lv_obj_align(fore_box, LV_ALIGN_TOP_MID, 0, 204);
    lv_obj_set_style_bg_opa(fore_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(fore_box, 0, 0);
    lv_obj_set_style_pad_all(fore_box, 0, 0);
    lv_obj_set_flex_flow(fore_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fore_box, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(fore_box, LV_OBJ_FLAG_SCROLLABLE);

    // 时段 1 (w: 90, h: 84)
    lv_obj_t* f1 = lv_obj_create(fore_box);
    lv_obj_set_size(f1, 90, 84);
    lv_obj_set_style_bg_color(f1, lv_color_hex(0x131A26), 0);
    lv_obj_set_style_border_color(f1, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_width(f1, 1, 0);
    lv_obj_set_style_radius(f1, 8, 0);
    lv_obj_set_style_pad_top(f1, 8, 0);
    lv_obj_set_style_pad_bottom(f1, 8, 0);
    lv_obj_set_style_pad_left(f1, 4, 0);
    lv_obj_set_style_pad_right(f1, 4, 0);
    lv_obj_set_flex_flow(f1, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(f1, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(f1, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* f1_time = lv_label_create(f1);
    lv_label_set_text(f1_time, "15:00");
    lv_obj_set_style_text_font(f1_time, font_small, 0);
    lv_obj_set_style_text_color(f1_time, lv_color_hex(0x94A3B8), 0);

    // 晴天：Icon 与 温度并排在同一行
    lv_obj_t* f1_row = lv_obj_create(f1);
    lv_obj_set_size(f1_row, 82, 36);
    lv_obj_set_style_bg_opa(f1_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(f1_row, 0, 0);
    lv_obj_set_style_pad_all(f1_row, 0, 0);
    lv_obj_set_flex_flow(f1_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(f1_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(f1_row, 4, 0);
    lv_obj_remove_flag(f1_row, LV_OBJ_FLAG_SCROLLABLE);

    // 晴天太阳 Icon（金黄圆球 + 优雅光圈）
    lv_obj_t* f1_sun = lv_obj_create(f1_row);
    lv_obj_set_size(f1_sun, 14, 14);
    lv_obj_set_style_radius(f1_sun, 7, 0);
    lv_obj_set_style_bg_color(f1_sun, lv_color_hex(0xFBBF24), 0);
    lv_obj_set_style_border_color(f1_sun, lv_color_hex(0xF59E0B), 0);
    lv_obj_set_style_border_width(f1_sun, 2, 0);
    lv_obj_set_style_pad_all(f1_sun, 0, 0);
    lv_obj_remove_flag(f1_sun, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* f1_temp = lv_label_create(f1_row);
    lv_label_set_text(f1_temp, "24°");
    lv_obj_set_style_text_font(f1_temp, &font_maison_neue_book_26, 0);
    lv_obj_set_style_text_color(f1_temp, lv_color_hex(0xFFFFFF), 0);

    // 时段 2 (多云，w: 90, h: 84)
    lv_obj_t* f2 = lv_obj_create(fore_box);
    lv_obj_set_size(f2, 90, 84);
    lv_obj_set_style_bg_color(f2, lv_color_hex(0x182234), 0);
    lv_obj_set_style_border_color(f2, lv_color_hex(0x0284C7), 0);
    lv_obj_set_style_border_width(f1, 1, 0);
    lv_obj_set_style_radius(f2, 8, 0);
    lv_obj_set_style_pad_top(f2, 8, 0);
    lv_obj_set_style_pad_bottom(f2, 8, 0);
    lv_obj_set_style_pad_left(f2, 4, 0);
    lv_obj_set_style_pad_right(f2, 4, 0);
    lv_obj_set_flex_flow(f2, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(f2, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(f2, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* f2_time = lv_label_create(f2);
    lv_label_set_text(f2_time, "18:00");
    lv_obj_set_style_text_font(f2_time, font_small, 0);
    lv_obj_set_style_text_color(f2_time, lv_color_hex(0x38BDF8), 0);

    // 多云：Icon 与 温度并排在同一行
    lv_obj_t* f2_row = lv_obj_create(f2);
    lv_obj_set_size(f2_row, 82, 36);
    lv_obj_set_style_bg_opa(f2_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(f2_row, 0, 0);
    lv_obj_set_style_pad_all(f2_row, 0, 0);
    lv_obj_set_flex_flow(f2_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(f2_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(f2_row, 4, 0);
    lv_obj_remove_flag(f2_row, LV_OBJ_FLAG_SCROLLABLE);

    // 多云云朵 Icon（胶囊云朵形）
    lv_obj_t* f2_cloud = lv_obj_create(f2_row);
    lv_obj_set_size(f2_cloud, 18, 11);
    lv_obj_set_style_radius(f2_cloud, 5, 0);
    lv_obj_set_style_bg_color(f2_cloud, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_border_width(f2_cloud, 0, 0);
    lv_obj_set_style_pad_all(f2_cloud, 0, 0);
    lv_obj_remove_flag(f2_cloud, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* f2_temp = lv_label_create(f2_row);
    lv_label_set_text(f2_temp, "21°");
    lv_obj_set_style_text_font(f2_temp, &font_maison_neue_book_26, 0);
    lv_obj_set_style_text_color(f2_temp, lv_color_hex(0xFFFFFF), 0);

    // 时段 3 (雨天/夜间，w: 90, h: 84)
    lv_obj_t* f3 = lv_obj_create(fore_box);
    lv_obj_set_size(f3, 90, 84);
    lv_obj_set_style_bg_color(f3, lv_color_hex(0x131A26), 0);
    lv_obj_set_style_border_color(f3, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_width(f3, 1, 0);
    lv_obj_set_style_radius(f3, 8, 0);
    lv_obj_set_style_pad_top(f3, 8, 0);
    lv_obj_set_style_pad_bottom(f3, 8, 0);
    lv_obj_set_style_pad_left(f3, 2, 0);
    lv_obj_set_style_pad_right(f3, 2, 0);
    lv_obj_set_flex_flow(f3, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(f3, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(f3, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* f3_time = lv_label_create(f3);
    lv_label_set_text(f3_time, "21:00");
    lv_obj_set_style_text_font(f3_time, font_small, 0);
    lv_obj_set_style_text_color(f3_time, lv_color_hex(0x94A3B8), 0);

    // 雨天/夜间：Icon 与 温度并排在同一行
    lv_obj_t* f3_row = lv_obj_create(f3);
    lv_obj_set_size(f3_row, 82, 36);
    lv_obj_set_style_bg_opa(f3_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(f3_row, 0, 0);
    lv_obj_set_style_pad_all(f3_row, 0, 0);
    lv_obj_set_flex_flow(f3_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(f3_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(f3_row, 4, 0);
    lv_obj_remove_flag(f3_row, LV_OBJ_FLAG_SCROLLABLE);

    // 雨水 Icon（蓝滴）或 LV_SYMBOL_TINT
    lv_obj_t* f3_rain = lv_label_create(f3_row);
    lv_label_set_text(f3_rain, LV_SYMBOL_TINT);
    lv_obj_set_style_text_font(f3_rain, (lv_font_t*)&lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(f3_rain, lv_color_hex(0x60A5FA), 0);

    lv_obj_t* f3_temp = lv_label_create(f3_row);
    lv_label_set_text(f3_temp, "18°");
    lv_obj_set_style_text_font(f3_temp, &font_maison_neue_book_26, 0);
    lv_obj_set_style_text_color(f3_temp, lv_color_hex(0xFFFFFF), 0);
}


#include "src/draw/snapshot/lv_snapshot.h"

void SaveScreen(const char* filename) {
    lv_draw_buf_t* buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_ARGB8888);
    if (!buf) {
        printf("[Simulator] lv_snapshot_take failed for %s\n", filename);
        return;
    }
    int w = buf->header.w;
    int h = buf->header.h;
    uint8_t* rgba = (uint8_t*)malloc(w * h * 4);
    const uint8_t* src = (const uint8_t*)buf->data;
    // LVGL ARGB8888 on Little Endian is BGRA, swap B and R to get RGBA
    for (int i = 0; i < w * h; i++) {
        rgba[i * 4 + 0] = src[i * 4 + 2]; // R
        rgba[i * 4 + 1] = src[i * 4 + 1]; // G
        rgba[i * 4 + 2] = src[i * 4 + 0]; // B
        rgba[i * 4 + 3] = src[i * 4 + 3]; // A
    }

    unsigned char* png_buf = NULL;
    size_t png_size = 0;
    unsigned err = lodepng_encode32(&png_buf, &png_size, rgba, w, h);
    if (err) {
        printf("[Simulator] lodepng encoding error %u: %s\n", err, lodepng_error_text(err));
    } else {
        FILE* f = fopen(filename, "wb");
        if (f) {
            fwrite(png_buf, 1, png_size, f);
            fclose(f);
            printf("[Simulator] Successfully saved snapshot (%ux%u, %zu bytes) to %s\n", 
                   w, h, png_size, filename);
        } else {
            perror("[Simulator] fopen error");
        }
        free(png_buf);
    }
    free(rgba);
    lv_draw_buf_destroy(buf);
}

static uint8_t* font_buffer = NULL;

void BuildFontShowcasePage(lv_obj_t* parent) {
    page_fonts = lv_obj_create(parent);
    lv_obj_set_size(page_fonts, 360, 360);
    lv_obj_set_pos(page_fonts, 0, 0);
    lv_obj_set_style_bg_color(page_fonts, lv_color_hex(0x0A0E16), 0);
    lv_obj_set_style_bg_opa(page_fonts, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page_fonts, 0, 0);
    lv_obj_set_style_pad_all(page_fonts, 8, 0);
    lv_obj_set_scrollbar_mode(page_fonts, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(page_fonts, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(page_fonts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page_fonts, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(page_fonts, 8, 0);

    // 标题
    lv_obj_t* title = lv_label_create(page_fonts);
    lv_label_set_text(title, "ESP32 Built-in Fonts Showcase");
    lv_obj_set_style_text_font(title, (lv_font_t*)&lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_pad_top(title, 8, 0);

    // 卡片1: Maison Neue 26 (包豪斯现代腕表几何体)
    lv_obj_t* c1 = lv_obj_create(page_fonts);
    lv_obj_set_size(c1, 310, 68);
    lv_obj_set_style_bg_color(c1, lv_color_hex(0x111827), 0);
    lv_obj_set_style_border_color(c1, lv_color_hex(0x00D2FF), 0);
    lv_obj_set_style_border_width(c1, 1, 0);
    lv_obj_set_style_radius(c1, 8, 0);
    lv_obj_set_style_pad_all(c1, 6, 0);
    lv_obj_remove_flag(c1, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* c1_tag = lv_label_create(c1);
    lv_label_set_text(c1_tag, "1. Maison Neue 26px (Swiss / Watch Style)");
    lv_obj_set_style_text_font(c1_tag, (lv_font_t*)&lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(c1_tag, lv_color_hex(0x38BDF8), 0);
    lv_obj_align(c1_tag, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t* c1_val = lv_label_create(c1);
    lv_label_set_text(c1_val, "13:31  HK$ 34.12  24C");
    lv_obj_set_style_text_font(c1_val, &font_maison_neue_book_26, 0);
    lv_obj_set_style_text_color(c1_val, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(c1_val, LV_ALIGN_BOTTOM_LEFT, 0, -2);

    // 卡片2: Montserrat 30/20 (LVGL 经典饱满现代几何)
    lv_obj_t* c2 = lv_obj_create(page_fonts);
    lv_obj_set_size(c2, 310, 68);
    lv_obj_set_style_bg_color(c2, lv_color_hex(0x111827), 0);
    lv_obj_set_style_border_color(c2, lv_color_hex(0x374151), 0);
    lv_obj_set_style_border_width(c2, 1, 0);
    lv_obj_set_style_radius(c2, 8, 0);
    lv_obj_set_style_pad_all(c2, 6, 0);
    lv_obj_remove_flag(c2, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* c2_tag = lv_label_create(c2);
    lv_label_set_text(c2_tag, "2. Montserrat 20/30px (Modern Bold)");
    lv_obj_set_style_text_font(c2_tag, (lv_font_t*)&lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(c2_tag, lv_color_hex(0x94A3B8), 0);
    lv_obj_align(c2_tag, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t* c2_val = lv_label_create(c2);
    lv_label_set_text(c2_val, "13:31  HK$ 34.12  24C");
    lv_obj_set_style_text_font(c2_val, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(c2_val, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(c2_val, LV_ALIGN_BOTTOM_LEFT, 0, -2);

    // 卡片3: Noto Sans Basic 30/20 (当前默认思源黑体)
    lv_obj_t* c3 = lv_obj_create(page_fonts);
    lv_obj_set_size(c3, 310, 68);
    lv_obj_set_style_bg_color(c3, lv_color_hex(0x111827), 0);
    lv_obj_set_style_border_color(c3, lv_color_hex(0x374151), 0);
    lv_obj_set_style_border_width(c3, 1, 0);
    lv_obj_set_style_radius(c3, 8, 0);
    lv_obj_set_style_pad_all(c3, 6, 0);
    lv_obj_remove_flag(c3, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* c3_tag = lv_label_create(c3);
    lv_label_set_text(c3_tag, "3. Noto Sans Basic 20px (Default Sans)");
    lv_obj_set_style_text_font(c3_tag, (lv_font_t*)&lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(c3_tag, lv_color_hex(0x94A3B8), 0);
    lv_obj_align(c3_tag, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t* c3_val = lv_label_create(c3);
    lv_label_set_text(c3_val, "13:31  HK$ 34.12  24C");
    lv_obj_set_style_text_font(c3_val, &font_noto_sans_basic_20_4, 0);
    lv_obj_set_style_text_color(c3_val, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(c3_val, LV_ALIGN_BOTTOM_LEFT, 0, -2);

    // 卡片4: Unscii 16 (赛博朋克像素点阵数码管)
    lv_obj_t* c4 = lv_obj_create(page_fonts);
    lv_obj_set_size(c4, 310, 68);
    lv_obj_set_style_bg_color(c4, lv_color_hex(0x111827), 0);
    lv_obj_set_style_border_color(c4, lv_color_hex(0x10B981), 0);
    lv_obj_set_style_border_width(c4, 1, 0);
    lv_obj_set_style_radius(c4, 8, 0);
    lv_obj_set_style_pad_all(c4, 6, 0);
    lv_obj_remove_flag(c4, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* c4_tag = lv_label_create(c4);
    lv_label_set_text(c4_tag, "4. Unscii 16px (Cyber Retro Pixel)");
    lv_obj_set_style_text_font(c4_tag, (lv_font_t*)&lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(c4_tag, lv_color_hex(0x34D399), 0);
    lv_obj_align(c4_tag, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t* c4_val = lv_label_create(c4);
    lv_label_set_text(c4_val, "13:31  HK$ 34.12  24C");
    lv_obj_set_style_text_font(c4_val, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(c4_val, lv_color_hex(0x34D399), 0);
    lv_obj_align(c4_val, LV_ALIGN_BOTTOM_LEFT, 0, -4);
}

static lv_obj_t* player_play_btn = NULL;
static lv_obj_t* player_play_icon = NULL;

void SetPlayerState(bool is_playing) {
    if (player_play_btn && player_play_icon) {
        lv_obj_set_style_bg_color(player_play_btn, lv_color_hex(0x0F172A), 0);
        lv_obj_set_style_border_color(player_play_btn, lv_color_hex(0x00E5FF), 0);
        lv_obj_set_style_border_width(player_play_btn, 2, 0);
        if (is_playing) {
            lv_image_set_src(player_play_icon, &img_player_pause);
        } else {
            lv_image_set_src(player_play_icon, &img_player_play_arrow);
        }
    }
}

void BuildPlayerPage(lv_obj_t* parent) {
    page_player = lv_obj_create(parent);
    lv_obj_set_size(page_player, 360, 360);
    lv_obj_set_pos(page_player, 0, 0);
    lv_obj_set_style_bg_color(page_player, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(page_player, 0, 0);
    lv_obj_set_style_pad_all(page_player, 0, 0);
    lv_obj_set_style_radius(page_player, 180, 0);
    lv_obj_remove_flag(page_player, LV_OBJ_FLAG_SCROLLABLE);

    // 顶部微光标题
    lv_obj_t* header = lv_label_create(page_player);
    lv_label_set_text(header, "01/05 · NAVIDROME");
    lv_obj_set_style_text_font(header, (lv_font_t*)&font_noto_sans_basic_16_4, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(0x38BDF8), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 20);

    // 中间黑胶唱片与环形进度条区
    lv_obj_t* disc_box = lv_obj_create(page_player);
    lv_obj_set_size(disc_box, 172, 172);
    lv_obj_align(disc_box, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_bg_opa(disc_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(disc_box, 0, 0);
    lv_obj_set_style_pad_all(disc_box, 0, 0);
    lv_obj_remove_flag(disc_box, LV_OBJ_FLAG_SCROLLABLE);

    // 弧形进度条 (围绕唱片)
    lv_obj_t* arc = lv_arc_create(disc_box);
    lv_obj_set_size(arc, 168, 168);
    lv_obj_align(arc, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 42);
    lv_obj_set_style_arc_width(arc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x1F2937), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);
    lv_obj_set_style_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);

    // 黑胶唱片本体 (直径 134px，深邃黑胶质感，边缘高光)
    lv_obj_t* vinyl = lv_obj_create(disc_box);
    lv_obj_set_size(vinyl, 134, 134);
    lv_obj_align(vinyl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(vinyl, 67, 0);
    lv_obj_set_style_bg_color(vinyl, lv_color_hex(0x0B0F19), 0);
    lv_obj_set_style_border_color(vinyl, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_width(vinyl, 2, 0);
    lv_obj_set_style_pad_all(vinyl, 0, 0);

    // 移掉原先正中央的圆环限制，直接在唱片核心舒展大尺寸宽幅律动声谱
    lv_obj_t* spectrum_box = lv_obj_create(vinyl);
    lv_obj_set_size(spectrum_box, 116, 58);
    lv_obj_align(spectrum_box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(spectrum_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spectrum_box, 0, 0);
    lv_obj_set_style_pad_all(spectrum_box, 0, 0);
    lv_obj_set_flex_flow(spectrum_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(spectrum_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(spectrum_box, 3, 0);
    lv_obj_remove_flag(spectrum_box, LV_OBJ_FLAG_SCROLLABLE);

    static const uint32_t kColors[13] = {
        0x0284C7, 0x0EA5E9, 0x38BDF8, 0x00E5FF, 0x2DD4BF, 0x34D399, 0x6EE7B7,
        0x34D399, 0x2DD4BF, 0x00E5FF, 0x38BDF8, 0x0EA5E9, 0x0284C7
    };
    static const int kHeights[13] = {12, 18, 26, 32, 38, 44, 48, 44, 38, 32, 26, 18, 12};
    for (int i = 0; i < 13; i++) {
        lv_obj_t* bar = lv_obj_create(spectrum_box);
        lv_obj_set_size(bar, 5, kHeights[i]);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(kColors[i]), 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
    }

    // 4. 曲目名称与艺术家 (支持中文字体)
    lv_obj_t* title = lv_label_create(page_player);
    lv_label_set_text(title, "威廉古堡");
    lv_obj_set_style_text_font(title, font_large, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_width(title, 280);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 222);

    lv_obj_t* artist = lv_label_create(page_player);
    lv_label_set_text(artist, "Jay Chou · 范特西");
    lv_obj_set_style_text_font(artist, font_medium, 0);
    lv_obj_set_style_text_color(artist, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_width(artist, 260);
    lv_obj_set_style_text_align(artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(artist, LV_ALIGN_TOP_MID, 0, 250);

    // 5. 底部触控控制栏
    lv_obj_t* ctrl_row = lv_obj_create(page_player);
    lv_obj_set_size(ctrl_row, 240, 56);
    lv_obj_align(ctrl_row, LV_ALIGN_TOP_MID, 0, 280);
    lv_obj_set_style_bg_opa(ctrl_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctrl_row, 0, 0);
    lv_obj_set_style_pad_all(ctrl_row, 0, 0);
    lv_obj_set_flex_flow(ctrl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctrl_row, 22, 0);
    lv_obj_remove_flag(ctrl_row, LV_OBJ_FLAG_SCROLLABLE);

    // 上一曲按钮 (44x44，深科技蓝底色，亮青边框，高辨识度)
    lv_obj_t* prev_btn = lv_btn_create(ctrl_row);
    lv_obj_set_size(prev_btn, 44, 44);
    lv_obj_set_style_radius(prev_btn, 22, 0);
    lv_obj_set_style_bg_color(prev_btn, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_color(prev_btn, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_border_width(prev_btn, 2, 0);
    lv_obj_set_style_pad_all(prev_btn, 0, 0);
    lv_obj_t* prev_icon = lv_label_create(prev_btn);
    lv_obj_set_style_text_font(prev_icon, (lv_font_t*)&font_material_symbols_16_4, 0);
    lv_label_set_text(prev_icon, MATERIAL_SYMBOLS_SKIP_PREVIOUS);
    lv_obj_set_style_text_color(prev_icon, lv_color_hex(0xF1F5F9), 0);
    lv_obj_align(prev_icon, LV_ALIGN_CENTER, 0, 0);

    // 播放/暂停按钮 (52x52 大号核心按键，使用实心高质感矢量抗锯齿图标)
    player_play_btn = lv_btn_create(ctrl_row);
    lv_obj_set_size(player_play_btn, 52, 52);
    lv_obj_set_style_radius(player_play_btn, 26, 0);
    lv_obj_set_style_bg_color(player_play_btn, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_border_color(player_play_btn, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_border_width(player_play_btn, 2, 0);
    lv_obj_set_style_pad_all(player_play_btn, 0, 0);
    player_play_icon = lv_image_create(player_play_btn);
    lv_image_set_src(player_play_icon, &img_player_play_arrow);
    lv_obj_align(player_play_icon, LV_ALIGN_CENTER, 0, 0);

    // 下一曲按钮 (44x44，深科技蓝底色，亮青边框，高辨识度)
    lv_obj_t* next_btn = lv_btn_create(ctrl_row);
    lv_obj_set_size(next_btn, 44, 44);
    lv_obj_set_style_radius(next_btn, 22, 0);
    lv_obj_set_style_bg_color(next_btn, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_color(next_btn, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_border_width(next_btn, 2, 0);
    lv_obj_set_style_pad_all(next_btn, 0, 0);
    lv_obj_t* next_icon = lv_label_create(next_btn);
    lv_obj_set_style_text_font(next_icon, (lv_font_t*)&font_material_symbols_16_4, 0);
    lv_label_set_text(next_icon, MATERIAL_SYMBOLS_SKIP_NEXT);
    lv_obj_set_style_text_color(next_icon, lv_color_hex(0xF1F5F9), 0);
    lv_obj_align(next_icon, LV_ALIGN_CENTER, 0, 0);

    SetPlayerState(true);
}

static void LoadChineseFonts() {
    const char* path = "/System/Library/Fonts/Supplemental/Arial Unicode.ttf";
    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("Failed to open %s\n", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    font_buffer = (uint8_t*)malloc(size);
    fread(font_buffer, 1, size, f);
    fclose(f);

    font_large = lv_tiny_ttf_create_data(font_buffer, size, 22);
    font_medium = lv_tiny_ttf_create_data(font_buffer, size, 16);
    font_small = lv_tiny_ttf_create_data(font_buffer, size, 12);
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    lv_init();

    lv_display_t* disp = lv_sdl_window_create(360, 360);
    lv_sdl_window_set_title(disp, "XiaoZhi 1.85C Round Screen Simulator (360x360)");

    lv_indev_t* mouse = lv_sdl_mouse_create();
    lv_indev_set_display(mouse, disp);

    // 默认大字体使用 ESP32 内置字体，中文字体加载全字符集模拟真机小智字库
    font_huge = (lv_font_t*)&font_noto_sans_basic_30_4;
    font_large = (lv_font_t*)&font_noto_sans_basic_20_4;
    font_medium = (lv_font_t*)&font_noto_sans_basic_16_4;
    font_small = (lv_font_t*)&font_noto_sans_basic_16_4;

    LoadChineseFonts();

    lv_obj_t* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0A0E16), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    BuildHomePage(screen);
    BuildWeatherPage(screen);
    BuildFontShowcasePage(screen);
    BuildPlayerPage(screen);

    // Indicator
    lv_obj_t* ind_box = lv_obj_create(screen);
    lv_obj_set_size(ind_box, 100, 30);
    lv_obj_align(ind_box, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_opa(ind_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ind_box, 0, 0);
    lv_obj_set_style_pad_all(ind_box, 0, 0);
    lv_obj_set_flex_flow(ind_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ind_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ind_box, 12, 0);
    lv_obj_remove_flag(ind_box, LV_OBJ_FLAG_SCROLLABLE);

    dot_home = lv_obj_create(ind_box);
    lv_obj_set_size(dot_home, 16, 5);
    lv_obj_set_style_radius(dot_home, 3, 0);
    lv_obj_set_style_bg_color(dot_home, lv_color_hex(0x00D2FF), 0);
    lv_obj_set_style_border_width(dot_home, 0, 0);
    lv_obj_add_flag(dot_home, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(dot_home, on_dot_home_click, LV_EVENT_CLICKED, NULL);

    dot_weather = lv_obj_create(ind_box);
    lv_obj_set_size(dot_weather, 6, 5);
    lv_obj_set_style_radius(dot_weather, 3, 0);
    lv_obj_set_style_bg_color(dot_weather, lv_color_hex(0x4A5568), 0);
    lv_obj_set_style_bg_opa(dot_weather, LV_OPA_50, 0);
    lv_obj_set_style_border_width(dot_weather, 0, 0);
    lv_obj_add_flag(dot_weather, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(dot_weather, on_dot_weather_click, LV_EVENT_CLICKED, NULL);

    // 1. 渲染并保存字体对比图 (preview_fonts.png)
    SwitchPage(2);
    lv_obj_invalidate(screen);
    for (int i = 0; i < 20; i++) {
        lv_timer_handler();
        SDL_Delay(10);
    }
    lv_refr_now(disp);
    SaveScreen("preview_fonts.png");

    // 方案 A: Noto Sans (当前默认)
    SwitchPage(0);
    lv_obj_set_style_text_font(time_lbl, (lv_font_t*)&font_noto_sans_basic_30_4, 0);
    lv_obj_set_style_text_font(price_lbl, (lv_font_t*)&font_noto_sans_basic_20_4, 0);
    lv_obj_invalidate(screen);
    for (int i = 0; i < 20; i++) { lv_timer_handler(); SDL_Delay(10); }
    lv_refr_now(disp);
    SaveScreen("preview_style_noto.png");

    // 方案 B: Maison Neue (包豪斯高级腕表风，强烈推荐数字钟表与财经)
    lv_obj_set_style_text_font(time_lbl, (lv_font_t*)&font_maison_neue_book_26, 0);
    lv_obj_set_style_text_font(price_lbl, (lv_font_t*)&font_maison_neue_book_26, 0);
    lv_obj_invalidate(screen);
    for (int i = 0; i < 20; i++) { lv_timer_handler(); SDL_Delay(10); }
    lv_refr_now(disp);
    SaveScreen("preview_style_maison.png");

    // 方案 C: Montserrat 30/20 (现代饱满大几何)
    lv_obj_set_style_text_font(time_lbl, (lv_font_t*)&lv_font_montserrat_30, 0);
    lv_obj_set_style_text_font(price_lbl, (lv_font_t*)&lv_font_montserrat_20, 0);
    lv_obj_invalidate(screen);
    for (int i = 0; i < 20; i++) { lv_timer_handler(); SDL_Delay(10); }
    lv_refr_now(disp);
    SaveScreen("preview_style_montserrat.png");

    // 方案 D: Unscii 16 (赛博极客/数码管像素点阵)
    lv_obj_set_style_text_font(time_lbl, (lv_font_t*)&lv_font_unscii_16, 0);
    lv_obj_set_style_text_font(price_lbl, (lv_font_t*)&lv_font_unscii_16, 0);
    lv_obj_invalidate(screen);
    for (int i = 0; i < 20; i++) { lv_timer_handler(); SDL_Delay(10); }
    lv_refr_now(disp);
    SaveScreen("preview_style_unscii.png");

    // 天气页
    SwitchPage(1);
    lv_obj_invalidate(screen);
    for (int i = 0; i < 20; i++) { lv_timer_handler(); SDL_Delay(10); }
    lv_refr_now(disp);
    SaveScreen("preview_weather.png");

    // 播放器页 - 播放中状态
    SwitchPage(3);
    SetPlayerState(true);
    lv_obj_invalidate(screen);
    for (int i = 0; i < 20; i++) { lv_timer_handler(); SDL_Delay(10); }
    lv_refr_now(disp);
    SaveScreen("preview_player_playing.png");

    // 播放器页 - 暂停中状态
    SetPlayerState(false);
    lv_obj_invalidate(screen);
    for (int i = 0; i < 20; i++) { lv_timer_handler(); SDL_Delay(10); }
    lv_refr_now(disp);
    SaveScreen("preview_player_paused.png");

    SwitchPage(0);
    lv_obj_invalidate(screen);
    for (int i = 0; i < 10; i++) {
        lv_timer_handler();
        SDL_Delay(10);
    }
    lv_refr_now(disp);

    // 如果指定了 --headless，则直接退出
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) {
            printf("[Simulator] Headless mode: screenshots generated, exiting.\n");
            return 0;
        }
    }

    bool running = true;
    while (running) {
        lv_timer_handler();

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) {
                    running = false;
                } else if (event.key.keysym.sym == SDLK_SPACE || event.key.keysym.sym == SDLK_TAB) {
                    SwitchPage(1 - current_page);
                } else if (event.key.keysym.sym == SDLK_s) {
                    SaveScreen(current_page == 0 ? "snap_home.png" : "snap_weather.png");
                }
            }
        }
        SDL_Delay(10);
    }

    return 0;
}

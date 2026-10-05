#include "service_config.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <vector>

#ifdef ESP_PLATFORM
#include <esp_random.h>
#include "settings.h"
#endif

namespace waveshare185c {

namespace {

inline std::string TrimWhitespace(const std::string& str) {
    size_t first = 0;
    while (first < str.size() && std::isspace(static_cast<unsigned char>(str[first]))) {
        first++;
    }
    if (first == str.size()) {
        return "";
    }
    size_t last = str.size() - 1;
    while (last > first && std::isspace(static_cast<unsigned char>(str[last]))) {
        last--;
    }
    return str.substr(first, last - first + 1);
}

// 自包含极轻量 RFC 1321 MD5 实现（零外部依赖，跨平台与全 IDF 版本兼容）
std::string CalculateMd5(const std::string& input) {
    uint32_t s[] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
                    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t K[] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    size_t orig_len = input.length();
    size_t new_len = ((((orig_len + 8) / 64) + 1) * 64);
    std::vector<uint8_t> msg(new_len, 0);
    memcpy(msg.data(), input.data(), orig_len);
    msg[orig_len] = 0x80;
    uint64_t bits_len = 8 * (uint64_t)orig_len;
    memcpy(msg.data() + new_len - 8, &bits_len, 8);

    for (size_t offset = 0; offset < new_len; offset += 64) {
        uint32_t* M = (uint32_t*)(msg.data() + offset);
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (uint32_t i = 0; i < 64; i++) {
            uint32_t F, g;
            if (i < 16) {
                F = (B & C) | ((~B) & D);
                g = i;
            } else if (i < 32) {
                F = (D & B) | ((~D) & C);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                F = B ^ C ^ D;
                g = (3 * i + 5) % 16;
            } else {
                F = C ^ (B | (~D));
                g = (7 * i) % 16;
            }
            uint32_t dTemp = D;
            D = C;
            C = B;
            uint32_t x = A + F + K[i] + M[g];
            B = B + ((x << s[i]) | (x >> (32 - s[i])));
            A = dTemp;
        }
        a0 += A;
        b0 += B;
        c0 += C;
        d0 += D;
    }

    uint8_t digest[16];
    memcpy(digest, &a0, 4);
    memcpy(digest + 4, &b0, 4);
    memcpy(digest + 8, &c0, 4);
    memcpy(digest + 12, &d0, 4);

    char res[33];
    for (int i = 0; i < 16; ++i) {
        snprintf(&res[i * 2], 3, "%02x", digest[i]);
    }
    return std::string(res);
}

}  // namespace

ServiceConfig::ServiceConfig(std::shared_ptr<KeyValueStore> store) : store_(std::move(store)) {
    // 默认回退初值（全空，间隔为出厂默认）
    kconfig_navi_defaults_ = NavidromeConfig{"", "", ""};
    kconfig_bsz_defaults_ =
        BeszelConfig{"", "", "", kDefaultBszFetchInterval, kDefaultBszRotateInterval};
    kconfig_notify_defaults_ = NotifyConfig{};
}

bool ServiceConfig::NormalizeAndValidateUrl(const std::string& input, std::string& output,
                                            bool allow_empty) {
    std::string trimmed = TrimWhitespace(input);
    if (trimmed.empty()) {
        output = "";
        return allow_empty;
    }

    bool is_http = (trimmed.rfind("http://", 0) == 0);
    bool is_https = (trimmed.rfind("https://", 0) == 0);

    if (!is_http && !is_https) {
        return false;
    }

    size_t prefix_len = is_http ? 7 : 8;
    if (trimmed.size() <= prefix_len) {
        // 仅有协议头，无主机名
        return false;
    }

    // 去除末尾所有 '/'
    while (trimmed.size() > prefix_len && trimmed.back() == '/') {
        trimmed.pop_back();
    }

    if (trimmed.size() <= prefix_len) {
        return false;
    }

    output = trimmed;
    return true;
}

std::string ServiceConfig::RedactUrl(const std::string& url) {
    std::string res = url;
    for (const char* param : {"p", "token", "password"}) {
        std::string amp = "&" + std::string(param) + "=";
        std::string q = "?" + std::string(param) + "=";
        for (const std::string& marker : {amp, q}) {
            size_t pos = res.find(marker);
            if (pos == std::string::npos) {
                continue;
            }
            size_t start = pos + marker.size();
            size_t end = res.find('&', start);
            if (end == std::string::npos) {
                res.replace(start, res.size() - start, "***");
            } else {
                res.replace(start, end - start, "***");
            }
        }
    }
    return res;
}

std::string ServiceConfig::GenerateNavidromeDirectUrl(const std::string& base_url,
                                                      const std::string& song_id,
                                                      const std::string& username,
                                                      const std::string& password,
                                                      bool for_dlna) {
    if (base_url.empty() || song_id.empty() || username.empty()) {
        return "";
    }

    std::ostringstream oss;
    oss << base_url << "/rest/stream?id=" << song_id
        << "&u=" << username;

    if (!password.empty()) {
        char salt[7];
#ifdef ESP_PLATFORM
        uint32_t rnd = esp_random();
#else
        uint32_t rnd = 0x123456;
#endif
        snprintf(salt, sizeof(salt), "%06x", (unsigned int)(rnd & 0xFFFFFF));

        std::string secret = password + salt;
        std::string token = CalculateMd5(secret);
        oss << "&t=" << token << "&s=" << salt;
    }

    oss << "&v=1.16.1&c=xiaozhi";

    if (for_dlna) {
        // 第三方局域网设备（电视/音箱）原生硬解，无损且不占服务端转码 CPU
        oss << "&format=raw";
    } else {
        // ESP32 本机轻量播放
        oss << "&format=opus&maxBitRate=96";
    }

    return oss.str();
}

void ServiceConfig::SetKconfigDefaults(const NavidromeConfig& navi, const BeszelConfig& bsz,
                                       const NotifyConfig& notify) {
    kconfig_navi_defaults_ = navi;
    kconfig_bsz_defaults_ = bsz;
    kconfig_notify_defaults_ = notify;

    std::string norm_url;
    if (NormalizeAndValidateUrl(navi.url, norm_url, true)) {
        kconfig_navi_defaults_.url = norm_url;
    }
    if (NormalizeAndValidateUrl(bsz.url, norm_url, true)) {
        kconfig_bsz_defaults_.url = norm_url;
    }
    if (NormalizeAndValidateUrl(notify.tts_base_url, norm_url, true)) {
        kconfig_notify_defaults_.tts_base_url = norm_url;
    }

    if (kconfig_bsz_defaults_.fetch_interval_s < kMinInterval ||
        kconfig_bsz_defaults_.fetch_interval_s > kMaxInterval) {
        kconfig_bsz_defaults_.fetch_interval_s = kDefaultBszFetchInterval;
    }
    if (kconfig_bsz_defaults_.rotate_interval_s < kMinInterval ||
        kconfig_bsz_defaults_.rotate_interval_s > kMaxInterval) {
        kconfig_bsz_defaults_.rotate_interval_s = kDefaultBszRotateInterval;
    }
    if (kconfig_notify_defaults_.mqtt_port <= 0 || kconfig_notify_defaults_.mqtt_port > 65535) {
        kconfig_notify_defaults_.mqtt_port = kDefaultNotifyMqttPort;
    }
    if (kconfig_notify_defaults_.tts_format.empty()) {
        kconfig_notify_defaults_.tts_format = kDefaultNotifyTtsFormat;
    }
}

NavidromeConfig ServiceConfig::GetNavidromeConfig() const {
    NavidromeConfig cfg;
    std::string nvs_url = store_ ? store_->GetString(kKeyNaviUrl, "") : "";
    std::string nvs_user = store_ ? store_->GetString(kKeyNaviUser, "") : "";
    std::string nvs_pass = store_ ? store_->GetString(kKeyNaviPass, "") : "";

    cfg.url = !nvs_url.empty() ? nvs_url : kconfig_navi_defaults_.url;
    cfg.user = !nvs_user.empty() ? nvs_user : kconfig_navi_defaults_.user;
    cfg.pass = !nvs_pass.empty() ? nvs_pass : kconfig_navi_defaults_.pass;
    return cfg;
}

bool ServiceConfig::SetNavidromeConfig(const std::string& url, const std::string& user,
                                       const std::string& pass, std::string& err_msg) {
    std::string norm_url;
    if (!NormalizeAndValidateUrl(url, norm_url, /*allow_empty=*/false)) {
        err_msg = "Invalid URL: must start with http:// or https:// and include a host";
        return false;
    }

    if (!store_) {
        err_msg = "Internal error: storage unavailable";
        return false;
    }

    auto cur_cfg = GetNavidromeConfig();
    std::string final_user = !user.empty() ? user : cur_cfg.user;
    std::string final_pass = !pass.empty() ? pass : cur_cfg.pass;

    store_->SetString(kKeyNaviUrl, norm_url);
    store_->SetString(kKeyNaviUser, final_user);
    store_->SetString(kKeyNaviPass, final_pass);
    return true;
}

BeszelConfig ServiceConfig::GetBeszelConfig() const {
    BeszelConfig cfg;
    std::string nvs_url = store_ ? store_->GetString(kKeyBszUrl, "") : "";
    std::string nvs_user = store_ ? store_->GetString(kKeyBszUser, "") : "";
    std::string nvs_pass = store_ ? store_->GetString(kKeyBszPass, "") : "";

    cfg.url = !nvs_url.empty() ? nvs_url : kconfig_bsz_defaults_.url;
    cfg.user = !nvs_user.empty() ? nvs_user : kconfig_bsz_defaults_.user;
    cfg.pass = !nvs_pass.empty() ? nvs_pass : kconfig_bsz_defaults_.pass;

    int32_t nvs_fetch = store_ ? store_->GetInt(kKeyBszFetch, 0) : 0;
    if (nvs_fetch >= kMinInterval && nvs_fetch <= kMaxInterval) {
        cfg.fetch_interval_s = nvs_fetch;
    } else {
        cfg.fetch_interval_s = kconfig_bsz_defaults_.fetch_interval_s;
    }

    int32_t nvs_rotate = store_ ? store_->GetInt(kKeyBszRotate, 0) : 0;
    if (nvs_rotate >= kMinInterval && nvs_rotate <= kMaxInterval) {
        cfg.rotate_interval_s = nvs_rotate;
    } else {
        cfg.rotate_interval_s = kconfig_bsz_defaults_.rotate_interval_s;
    }

    return cfg;
}

bool ServiceConfig::SetBeszelConfig(const std::string& url, const std::string& user,
                                    const std::string& pass, int32_t fetch_interval_s,
                                    int32_t rotate_interval_s, std::string& err_msg) {
    std::string norm_url;
    if (!NormalizeAndValidateUrl(url, norm_url, /*allow_empty=*/false)) {
        err_msg = "Invalid Beszel URL: must start with http:// or https:// and include a host";
        return false;
    }

    if (!store_) {
        err_msg = "Internal error: storage unavailable";
        return false;
    }

    auto cur_cfg = GetBeszelConfig();
    std::string final_user = !user.empty() ? user : cur_cfg.user;
    std::string final_pass = !pass.empty() ? pass : cur_cfg.pass;

    int32_t final_fetch = cur_cfg.fetch_interval_s;
    if (fetch_interval_s > 0) {
        if (fetch_interval_s < kMinInterval || fetch_interval_s > kMaxInterval) {
            err_msg = "Invalid fetch interval: must be between 1 and 86400 seconds";
            return false;
        }
        final_fetch = fetch_interval_s;
    }

    int32_t final_rotate = cur_cfg.rotate_interval_s;
    if (rotate_interval_s > 0) {
        if (rotate_interval_s < kMinInterval || rotate_interval_s > kMaxInterval) {
            err_msg = "Invalid rotate interval: must be between 1 and 86400 seconds";
            return false;
        }
        final_rotate = rotate_interval_s;
    }

    store_->SetString(kKeyBszUrl, norm_url);
    store_->SetString(kKeyBszUser, final_user);
    store_->SetString(kKeyBszPass, final_pass);
    store_->SetInt(kKeyBszFetch, final_fetch);
    store_->SetInt(kKeyBszRotate, final_rotate);
    return true;
}

NotifyConfig ServiceConfig::GetNotifyConfig() const {
    NotifyConfig cfg;
    if (!store_) {
        return kconfig_notify_defaults_;
    }

    auto pick = [this](const char* key, const std::string& fallback) {
        std::string v = store_->GetString(key, "");
        return v.empty() ? fallback : v;
    };

    cfg.mqtt_host = pick(kKeyNotifyMqttHost, kconfig_notify_defaults_.mqtt_host);
    cfg.mqtt_user = pick(kKeyNotifyMqttUser, kconfig_notify_defaults_.mqtt_user);
    cfg.mqtt_pass = pick(kKeyNotifyMqttPass, kconfig_notify_defaults_.mqtt_pass);
    cfg.mqtt_topic = pick(kKeyNotifyMqttTopic, kconfig_notify_defaults_.mqtt_topic);
    cfg.tts_base_url = pick(kKeyNotifyTtsUrl, kconfig_notify_defaults_.tts_base_url);
    cfg.tts_token = pick(kKeyNotifyTtsToken, kconfig_notify_defaults_.tts_token);
    cfg.tts_voice = pick(kKeyNotifyTtsVoice, kconfig_notify_defaults_.tts_voice);
    cfg.tts_format = pick(kKeyNotifyTtsFormat, kconfig_notify_defaults_.tts_format);

    int32_t nvs_port = store_->GetInt(kKeyNotifyMqttPort, 0);
    cfg.mqtt_port = (nvs_port > 0 && nvs_port <= 65535) ? nvs_port
                                                       : kconfig_notify_defaults_.mqtt_port;
    if (cfg.mqtt_port <= 0) {
        cfg.mqtt_port = kDefaultNotifyMqttPort;
    }
    return cfg;
}

bool ServiceConfig::SetNotifyConfig(const std::string& mqtt_host, int32_t mqtt_port,
                                    const std::string& mqtt_user, const std::string& mqtt_pass,
                                    const std::string& mqtt_topic, const std::string& tts_base_url,
                                    const std::string& tts_token, const std::string& tts_voice,
                                    const std::string& tts_format, std::string& err_msg) {
    if (!store_) {
        err_msg = "Internal error: storage unavailable";
        return false;
    }

    NotifyConfig cur = GetNotifyConfig();

    std::string final_host = mqtt_host.empty() ? cur.mqtt_host : TrimWhitespace(mqtt_host);
    if (final_host.empty()) {
        err_msg = "Invalid MQTT host: must not be empty";
        return false;
    }

    int32_t final_port = (mqtt_port > 0) ? mqtt_port : cur.mqtt_port;
    if (final_port <= 0 || final_port > 65535) {
        err_msg = "Invalid MQTT port: must be between 1 and 65535";
        return false;
    }

    std::string final_topic = mqtt_topic.empty() ? cur.mqtt_topic : TrimWhitespace(mqtt_topic);
    if (final_topic.empty()) {
        err_msg = "Invalid MQTT topic: must not be empty";
        return false;
    }

    std::string norm_url;
    if (!NormalizeAndValidateUrl(tts_base_url.empty() ? cur.tts_base_url : tts_base_url,
                                 norm_url, /*allow_empty=*/false)) {
        err_msg = "Invalid TTS URL: must start with http:// or https:// and include a host";
        return false;
    }

    std::string final_format = tts_format.empty() ? cur.tts_format : TrimWhitespace(tts_format);
    if (final_format.empty()) {
        err_msg = "Invalid TTS format: must not be empty";
        return false;
    }

    store_->SetString(kKeyNotifyMqttHost, final_host);
    store_->SetInt(kKeyNotifyMqttPort, final_port);
    store_->SetString(kKeyNotifyMqttUser, mqtt_user.empty() ? cur.mqtt_user : mqtt_user);
    store_->SetString(kKeyNotifyMqttPass, mqtt_pass.empty() ? cur.mqtt_pass : mqtt_pass);
    store_->SetString(kKeyNotifyMqttTopic, final_topic);
    store_->SetString(kKeyNotifyTtsUrl, norm_url);
    store_->SetString(kKeyNotifyTtsToken, tts_token.empty() ? cur.tts_token : tts_token);
    store_->SetString(kKeyNotifyTtsVoice, tts_voice.empty() ? cur.tts_voice : tts_voice);
    store_->SetString(kKeyNotifyTtsFormat, final_format);
    return true;
}

#ifdef ESP_PLATFORM
class SettingsKeyValueStore : public KeyValueStore {
public:
    SettingsKeyValueStore() : settings_(ServiceConfig::kNvsNamespace, true) {}

    std::string GetString(const std::string& key, const std::string& default_val = "") override {
        return settings_.GetString(key, default_val);
    }

    void SetString(const std::string& key, const std::string& value) override {
        settings_.SetString(key, value);
    }

    int32_t GetInt(const std::string& key, int32_t default_val = 0) override {
        return settings_.GetInt(key, default_val);
    }

    void SetInt(const std::string& key, int32_t value) override { settings_.SetInt(key, value); }

private:
    Settings settings_;
};

std::shared_ptr<ServiceConfig> CreateDefaultServiceConfig() {
    auto store = std::make_shared<SettingsKeyValueStore>();
    auto config = std::make_shared<ServiceConfig>(store);

    // 从 Kconfig 宏装载编译期默认值
    NavidromeConfig navi_kconfig;
#ifdef CONFIG_WS185C_NAVIDROME_URL
    navi_kconfig.url = CONFIG_WS185C_NAVIDROME_URL;
#endif
#ifdef CONFIG_WS185C_NAVIDROME_USER
    navi_kconfig.user = CONFIG_WS185C_NAVIDROME_USER;
#endif
#ifdef CONFIG_WS185C_NAVIDROME_PASS
    navi_kconfig.pass = CONFIG_WS185C_NAVIDROME_PASS;
#endif

    BeszelConfig bsz_kconfig;
#ifdef CONFIG_WS185C_BESZEL_URL
    bsz_kconfig.url = CONFIG_WS185C_BESZEL_URL;
#endif
#ifdef CONFIG_WS185C_BESZEL_USER
    bsz_kconfig.user = CONFIG_WS185C_BESZEL_USER;
#endif
#ifdef CONFIG_WS185C_BESZEL_PASS
    bsz_kconfig.pass = CONFIG_WS185C_BESZEL_PASS;
#endif
#ifdef CONFIG_WS185C_BESZEL_FETCH_INTERVAL
    bsz_kconfig.fetch_interval_s = CONFIG_WS185C_BESZEL_FETCH_INTERVAL;
#endif
#ifdef CONFIG_WS185C_BESZEL_CAROUSEL_INTERVAL
    bsz_kconfig.rotate_interval_s = CONFIG_WS185C_BESZEL_CAROUSEL_INTERVAL;
#endif

    NotifyConfig notify_kconfig;
#ifdef CONFIG_WS185C_NOTIFY_MQTT_HOST
    notify_kconfig.mqtt_host = CONFIG_WS185C_NOTIFY_MQTT_HOST;
#endif
#ifdef CONFIG_WS185C_NOTIFY_MQTT_PORT
    notify_kconfig.mqtt_port = CONFIG_WS185C_NOTIFY_MQTT_PORT;
#endif
#ifdef CONFIG_WS185C_NOTIFY_MQTT_USER
    notify_kconfig.mqtt_user = CONFIG_WS185C_NOTIFY_MQTT_USER;
#endif
#ifdef CONFIG_WS185C_NOTIFY_MQTT_PASS
    notify_kconfig.mqtt_pass = CONFIG_WS185C_NOTIFY_MQTT_PASS;
#endif
#ifdef CONFIG_WS185C_NOTIFY_MQTT_TOPIC
    notify_kconfig.mqtt_topic = CONFIG_WS185C_NOTIFY_MQTT_TOPIC;
#endif
#ifdef CONFIG_WS185C_NOTIFY_TTS_URL
    notify_kconfig.tts_base_url = CONFIG_WS185C_NOTIFY_TTS_URL;
#endif
#ifdef CONFIG_WS185C_NOTIFY_TTS_TOKEN
    notify_kconfig.tts_token = CONFIG_WS185C_NOTIFY_TTS_TOKEN;
#endif
#ifdef CONFIG_WS185C_NOTIFY_TTS_VOICE
    notify_kconfig.tts_voice = CONFIG_WS185C_NOTIFY_TTS_VOICE;
#endif
#ifdef CONFIG_WS185C_NOTIFY_TTS_FORMAT
    notify_kconfig.tts_format = CONFIG_WS185C_NOTIFY_TTS_FORMAT;
#endif

    config->SetKconfigDefaults(navi_kconfig, bsz_kconfig, notify_kconfig);
    return config;
}
#endif

}  // namespace waveshare185c

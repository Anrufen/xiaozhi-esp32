#include "service_config.h"

#include <algorithm>
#include <cctype>

#ifdef ESP_PLATFORM
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

}  // namespace

ServiceConfig::ServiceConfig(std::shared_ptr<KeyValueStore> store) : store_(std::move(store)) {
    // 默认回退初值（全空，间隔为出厂默认）
    kconfig_navi_defaults_ = NavidromeConfig{"", "", ""};
    kconfig_bsz_defaults_ =
        BeszelConfig{"", "", "", kDefaultBszFetchInterval, kDefaultBszRotateInterval};
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
    for (const char* prefix : {"&p=", "?p="}) {
        size_t pos = res.find(prefix);
        if (pos != std::string::npos) {
            size_t start = pos + 3;
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

void ServiceConfig::SetKconfigDefaults(const NavidromeConfig& navi, const BeszelConfig& bsz) {
    kconfig_navi_defaults_ = navi;
    kconfig_bsz_defaults_ = bsz;

    std::string norm_url;
    if (NormalizeAndValidateUrl(navi.url, norm_url, true)) {
        kconfig_navi_defaults_.url = norm_url;
    }
    if (NormalizeAndValidateUrl(bsz.url, norm_url, true)) {
        kconfig_bsz_defaults_.url = norm_url;
    }

    if (kconfig_bsz_defaults_.fetch_interval_s < kMinInterval ||
        kconfig_bsz_defaults_.fetch_interval_s > kMaxInterval) {
        kconfig_bsz_defaults_.fetch_interval_s = kDefaultBszFetchInterval;
    }
    if (kconfig_bsz_defaults_.rotate_interval_s < kMinInterval ||
        kconfig_bsz_defaults_.rotate_interval_s > kMaxInterval) {
        kconfig_bsz_defaults_.rotate_interval_s = kDefaultBszRotateInterval;
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

    config->SetKconfigDefaults(navi_kconfig, bsz_kconfig);
    return config;
}
#endif

}  // namespace waveshare185c

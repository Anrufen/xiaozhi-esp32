#ifndef BOARD_WAVESHARE_185C_SERVICE_CONFIG_H_
#define BOARD_WAVESHARE_185C_SERVICE_CONFIG_H_

#include <cstdint>
#include <memory>
#include <string>

namespace waveshare185c {

class KeyValueStore {
public:
    virtual ~KeyValueStore() = default;
    virtual std::string GetString(const std::string& key, const std::string& default_val = "") = 0;
    virtual void SetString(const std::string& key, const std::string& value) = 0;
    virtual int32_t GetInt(const std::string& key, int32_t default_val = 0) = 0;
    virtual void SetInt(const std::string& key, int32_t value) = 0;
};

struct NavidromeConfig {
    std::string url;
    std::string user;
    std::string pass;

    bool IsConfigured() const { return !url.empty() && !user.empty() && !pass.empty(); }
};

struct BeszelConfig {
    std::string url;
    std::string user;
    std::string pass;
    int32_t fetch_interval_s = 15;
    int32_t rotate_interval_s = 5;

    bool IsConfigured() const { return !url.empty() && !user.empty() && !pass.empty(); }
};

class ServiceConfig {
public:
    // NVS 命名空间与键名定义 (持久化 API, <=15 字符)
    static constexpr const char* kNvsNamespace = "waveshare185c";
    static constexpr const char* kKeyNaviUrl = "navi_url";
    static constexpr const char* kKeyNaviUser = "navi_user";
    static constexpr const char* kKeyNaviPass = "navi_pass";
    static constexpr const char* kKeyBszUrl = "bsz_url";
    static constexpr const char* kKeyBszUser = "bsz_user";
    static constexpr const char* kKeyBszPass = "bsz_pass";
    static constexpr const char* kKeyBszFetch = "bsz_fetch_s";
    static constexpr const char* kKeyBszRotate = "bsz_rotate_s";

    static constexpr int32_t kDefaultBszFetchInterval = 15;
    static constexpr int32_t kDefaultBszRotateInterval = 5;
    static constexpr int32_t kMinInterval = 1;
    static constexpr int32_t kMaxInterval = 86400;

    explicit ServiceConfig(std::shared_ptr<KeyValueStore> store);

    // 地址规范化与校验：
    // 1. 去除首尾空白
    // 2. 去除末尾所有 '/'
    // 3. 要求以 http:// 或 https:// 开头
    // 4. allow_empty 为 true 时空值合法并输出空串；为 false 时拒绝空值返回 false
    // 5. 非空且不满足协议头时返回 false
    static bool NormalizeAndValidateUrl(const std::string& input, std::string& output,
                                        bool allow_empty = false);

    // 脱敏 URL（用于日志安全输出，隐藏 &p= 或 ?p= 后的密码）
    static std::string RedactUrl(const std::string& url);

    // 设置编译期默认值（回退底色）
    void SetKconfigDefaults(const NavidromeConfig& navi, const BeszelConfig& bsz);

    // Navidrome 配置存取
    NavidromeConfig GetNavidromeConfig() const;
    bool SetNavidromeConfig(const std::string& url, const std::string& user,
                            const std::string& pass, std::string& err_msg);

    // Beszel 配置存取
    BeszelConfig GetBeszelConfig() const;
    bool SetBeszelConfig(const std::string& url, const std::string& user, const std::string& pass,
                         int32_t fetch_interval_s, int32_t rotate_interval_s, std::string& err_msg);

    // 辅助：获取底层存储
    std::shared_ptr<KeyValueStore> GetStore() const { return store_; }

private:
    std::shared_ptr<KeyValueStore> store_;
    NavidromeConfig kconfig_navi_defaults_;
    BeszelConfig kconfig_bsz_defaults_;
};

#ifdef ESP_PLATFORM
// 生产环境便捷工厂函数：创建对接全局 Settings 的 Store 和 ServiceConfig
std::shared_ptr<ServiceConfig> CreateDefaultServiceConfig();
#endif

}  // namespace waveshare185c

#endif  // BOARD_WAVESHARE_185C_SERVICE_CONFIG_H_

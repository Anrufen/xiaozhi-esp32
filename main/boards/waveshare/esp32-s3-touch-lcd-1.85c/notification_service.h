#ifndef NOTIFICATION_SERVICE_H_
#define NOTIFICATION_SERVICE_H_

#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <esp_timer.h>
#include <mqtt_client.h>
#include <cJSON.h>

#include "service_config.h"

namespace waveshare185c {

struct PendingNotification {
    std::string text;
    std::string audio_url;
    int priority = 1;
    int64_t created_at_ms = 0;
};

class NotificationService {
public:
    static NotificationService& GetInstance() {
        static NotificationService instance;
        return instance;
    }

    void Initialize();
    void Start();
    void Stop();

    // 收到外部消息时推入通知队列或立即播报
    void EnqueueNotification(const std::string& text, const std::string& audio_url = "", int priority = 1);

    // MCP 工具查询与配置接口
    std::string GetStatusJson();
    bool TestNotify(const std::string& text);
    bool SetConfig(const NotifyConfig& config);

    bool IsConnected() const { return is_connected_; }

private:
    NotificationService();
    ~NotificationService();

    void LoadConfig();
    void ProcessQueue();
    std::string BuildTtsUrl(const std::string& text);
    static std::string UrlEncode(const std::string& value);

    static void MqttEventHandler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data);
    void HandleMqttConnected();
    void HandleMqttDisconnected();
    void HandleMqttData(const char* topic, int topic_len, const char* data, int data_len);

    mutable std::recursive_mutex mutex_;
    std::shared_ptr<ServiceConfig> service_config_;
    esp_mqtt_client_handle_t mqtt_client_ = nullptr;
    esp_timer_handle_t check_timer_ = nullptr;

    bool is_connected_ = false;
    bool is_started_ = false;

    NotifyConfig config_;

    // 通知排队队列（最大 5 条）
    static constexpr size_t kMaxQueueSize = 5;
    std::vector<PendingNotification> queue_;
};

} // namespace waveshare185c

#endif // NOTIFICATION_SERVICE_H_

# 02 — 运行时配置内存缓存与 UI 线程安全互斥

**What to build:** 消除 UI 1Hz 时钟与轮播定时器中高频读取 NVS 的开销，并在 MCP 异步任务调用 UI 刷新时增加严格的锁保护，清理未引用的冗余接口。

**Blocked by:** 01 — 安全输入校验、增量配置更新与默认值规范化

**Status:** ready-for-agent

- [x] 在 `CustomLcdDisplay` 内部缓存当前生效的 `NavidromeConfig` 与 `BeszelConfig`，初始化及配置更新时同步刷新
- [x] 将 1Hz 定时器中的轮播间隔检查与后台拉取检查改为直接读取内存缓存，杜绝每秒 5~10 次 NVS 读操作
- [x] 确保 `ConfigureNavidrome` 和 `ConfigureBeszel` 在 MCP 异步回调线程中调用时，所有 LVGL 控件更新（`UpdateSettingsValues`、`UpdateServerUI`、`UpdatePlayerUI`）均处于 `DisplayLockGuard` 互斥锁保护下
- [x] 移除全工程未使用的 `ServiceConfig::SetNavidromeServer` 与 `CustomLcdDisplay::SetNavidromeServer` 冗余接口

# 03 — MCP 参数命名统一与使用者文档全面对齐

**What to build:** 保证 MCP 工具接口属性与对外使用文档、开发指南 100% 对齐。修正文档中错误的参数名拼写、Kconfig 宏名前缀及菜单路径，使用户照着文档操作即可正确配置。

**Blocked by:** 01 — 安全输入校验、增量配置更新与默认值规范化

**Status:** ready-for-agent

- [x] 确认 MCP 工具参数统一为：`url`、`user`、`password`、`fetch_interval`、`carousel_interval`
- [x] 更正 `docs/waveshare_1.85c_architecture_and_config.md` 中所有参数名拼写，移除 `username`、`fetch_interval_sec`、`rotate_interval_sec` 等历史不符名称
- [x] 更正架构文档中 Kconfig 宏命名前缀为 `CONFIG_WS185C_*`
- [x] 更正文档中关于 menuconfig 的实际菜单路径与配置项有效范围（1~86400 秒）
- [x] 同步更正 `dist/firmware_package/README.md` 中的扩展服务配置说明

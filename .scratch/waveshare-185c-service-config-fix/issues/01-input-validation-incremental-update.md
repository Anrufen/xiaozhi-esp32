# 01 — 安全输入校验、增量配置更新与默认值规范化

**What to build:** 增强服务配置模块的输入校验与更新安全性。空 URL 在配置时被明确拒绝并返回错误提示；调用配置工具时若只修改部分字段，未传或为空的字段自动保留已有配置（如仅修改 URL 时不擦除已有密码和账号，不重置自定义轮播间隔）；Kconfig 默认 URL 经过规范化去除尾部斜杠；在宿主机单元测试中完备覆盖这些新规则。

**Blocked by:** None — can start immediately

**Status:** ready-for-agent

- [x] `NormalizeAndValidateUrl` 支持非空校验，当作为配置输入时空 URL 返回 false 并给出明确错误信息
- [x] `SetNavidromeConfig` 实现增量更新语义：当传入的 user 或 pass 为空时，若 NVS 中已有值则保留既有值
- [x] `SetBeszelConfig` 实现增量更新语义：当传入 user 或 pass 为空时保留既有凭据；当 interval 传入 <=0 时保留既有有效间隔
- [x] `SetKconfigDefaults` 对 Kconfig 中的 navi 和 bsz 默认 URL 执行去除末尾 `/` 的规范化
- [x] 在 `scripts/tests/test_service_config.py` 中增加对空 URL 拒绝、增量配置保留与默认值规范化的测试用例

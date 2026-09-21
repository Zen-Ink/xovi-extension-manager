# 管理器错误分类与报告审计

审计日期：2026-09-21。范围：extension-manager、manager-ui、qt-resource-rebuilder 的 C/Rust 集成反馈，以及 XOVI 加载状态。不是所有第三方插件、Qt 和系统日志的全集。

**本文件保留修改前的审计快照和整改建议。运行时代码现已更新，当前接口与已知限制见 [diagnostics.md](diagnostics.md)。** 下表“级别/处理/建议报告”是建议；审计时实现只有 issues/warnings、操作 ok/error、注入 state/code 和 UI 状态，尚无统一严重级别、可恢复性或动作注册表。

## 修改前确认的误判

1. `inventory.cpp::requiresRestart()` 将 active 且非 INITIALIZED 的 native 插件一律标记重启，因此 DLOPEN_FAILED 等确定失败也被包括。已有 pending 标记还会优先返回 true。
2. `Manager.qml` 列表优先显示 Needs repair → Restart → Loaded/Enabled，加载失败会被 Restart 掩盖；详情也无条件追加 Restart required。
3. UI 状态映射使用 `load-failed`，而后端实际返回 `dlopen-failed`；shouldload/condition/dependency 失败也没有完整翻译映射。
4. `SettingsHost::recoveryHint()` 对所有页面错误都返回 `Update the plugin, then restart xochitl.`，没有区分缺资源、上下文销毁、调用时机和语法错误。
5. `hasBlockingEnableIssue()` 不包含本插件 runtime-dlopen/link 等失败。启动统计复用它，因此不能当作“所有错误数”。
6. QRR 的 `qrr_diagnostic()` 统一记为 failed，包括 shadowed-by-qrr 等可能只是覆盖关系的诊断。status::commit 又会将 prepared 的 no-matching-block 覆盖为 resource-registered，不能据此断言命中了补丁或页面可用。
7. missing-manifest 在清单里是 warning，但对需要 manifest 的 install/repair 操作是 error；missing-entry 对可推断 native 入口是 warning，对 QMD/QML 清单是 error。不能只按错误码分级。

## 分级与重启规则

- I 信息：预期禁用/跳过/处理中，无须修复。
- W 警告：信息不全、状态竞争、保护性拒绝或潜在冲突；不表示插件失效。
- E 错误：本次操作、插件或页面明确失败；提供针对性动作。
- F 严重：已确认影响主程序启动/全局资源；不能仅凭某个错误码推断。
- I/E、W/E、E/F 表示必须根据上下文判定，不能机械地映射为固定级别。

重启是应用已完成变更的动作，不是错误类别。应分开报告 `failure`、`pendingChange` 和 `recoveryAction`。未修复根因时不能把“重启”作为主建议；替换正确文件后可显示“已更换，重启以加载”。

## wrong ELF class 的结论

XOVI 在 dynamiclinker.c 将 dlerror 原文存入 loadError；manager 将该加载阶段映射为 runtime-dlopen-failed。目前未细分根因。wrong ELF class 表示加载链中的 ELF 位数与宿主进程不匹配，可能来自插件本体，也可能来自其依赖；不能只根据设备 CPU 是 64 位就判断包正确。

建议主文案：**插件或依赖的 ELF 位数与当前进程不兼容，请安装匹配的构建。** 详情保留原始 loadError、文件路径、宿主位数和实际 ELF 信息（后二者当前未完整采集）。更换正确构建前，单独重启不会解决。

同一 dlopen 阶段还应细分：缺共享库→安装兼容依赖；undefined symbol/符号版本不匹配→匹配 ABI 或重新编译；invalid ELF header/file too short→重新获取完整构建；Permission denied→检查权限/挂载策略。尚未识别的原文显示“加载失败，查看详情”，不能自动建议重启。

## 修改前的报告方式

- inventory：errors/issues 主要原样显示错误码；loadError 显示动态加载器原文；warnings 加 Warnings 标题后原样列出。
- API：多数只有 ok:false/error；inventory 操作部分附英文 message 或 errno 原文。没有逐码中文恢复提示。
- 设置页：按字符串匹配显示不兼容/缺组件/不可用/无法打开四类摘要，全部共享同一个更新后重启提示。
- 注入：显示 owner/injectionId、state、code、message；无统一严重等级。
- 启用禁用：成功显示 Enabled/Disabled，可追加 Restart xochitl to apply；失败显示 Unable to save。
- 修复入口：成功显示 Activation repaired，可追加重启；失败统一 Unable to repair activation. Open diagnostics for details.

## 错误码逐项清单（174 个诊断码/阶段状态，含动态依赖后缀）

以冒号结尾的 code 表示后面还会追加依赖 ID；信息阶段状态也列入表中，不能将 174 项理解为 174 种故障。每行给出当前错误码的源码位置；当前原文以 code 及该处 message 为准（有些 message 来自 errno、Qt 或 Rust，无法有限枚举）。建议报告列不是已经上线的翻译。

| 当前 code | 建议级别 | 处理方式 | 建议报告语 | 源码位置 |
|---|---|---|---|---|
| `action-already-pending` | I | 等待原请求处理 | 该操作已在等待处理 | [notifications.cpp:302](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/notifications.cpp#L302) |
| `action-not-available` | W | 刷新并确认对象存在/已启用；不默认重启 | 目标已不存在、未启用或当前不可用 | [notifications.cpp:297](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/notifications.cpp#L297) |
| `action-queue-full` | E | 释放过期对象/消费队列并检查泄漏；重启不是根治 | 已达到处理上限，请稍后重试或检查插件 | [notifications.cpp:304](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/notifications.cpp#L304) |
| `activation-cleanup-failed` | E | 禁用配置已保存但入口清理不完整；检查冲突文件并安全移除 | 已禁用，但残留入口未清理完成 | [inventory.cpp:2438](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2438) |
| `active-directory-failed` | E | 依据 errno/message 检查权限、只读文件系统、空间或文件完整性；修复后重试 | 文件或服务操作失败；详见原始原因 | [inventory.cpp:2493](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2493) |
| `active-entry-conflict` | E | 保留原文件，检查路径、类型或权限；禁止盲目覆盖 | 入口或路径不符合预期，操作已停止 | [inventory.cpp:2495](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2495) |
| `active-entry-dangling` | E | 修复入口；仅有运行态变更时再提示重启 | 插件入口与启用设置不一致 | [inventory.cpp:287](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L287) |
| `active-entry-remove-failed` | E | 依据 errno/message 检查权限、只读文件系统、空间或文件完整性；修复后重试 | 文件或服务操作失败；详见原始原因 | [inventory.cpp:1905](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1905) |
| `active-entry-repair-failed` | E | 依据 errno/message 检查权限、只读文件系统、空间或文件完整性；修复后重试 | 文件或服务操作失败；详见原始原因 | [inventory.cpp:2499](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2499) |
| `active-entry-unreadable` | E | 保留原文件，检查路径、类型或权限；禁止盲目覆盖 | 入口或路径不符合预期，操作已停止 | [inventory.cpp:282](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L282) |
| `adopt-enable-blocked` | E | 接管可能已完成；核对返回结果，先解决启用阻断项 | 插件已接管但启用失败 | [inventory.cpp:2663](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2663) |
| `allocation-failed` | E/F | 检查内存、资源包、压缩格式或 rebuilder 兼容性；按影响范围升级严重程度 | 资源处理失败，相关注入不可用 | `qt-resource-rebuilder/src/main.c:159`（工作区） |
| `already-managed` | W/E | 刷新状态并选正确操作或明确替换目标 | 当前对象状态不允许此操作 | [inventory.cpp:2566](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2566) |
| `apply-failed` | E | 按原始 message 修正 QMD/编码/注册时机；修复后重新应用 | 注入未完成，请检查补丁与固件兼容性 | `qt-resource-rebuilder/qmldiff/src/lib.rs:575`（工作区） |
| `architecture-mismatch` | E | 安装与宿主进程架构匹配的构建；然后重启加载 | 插件架构不兼容，请更换安装包 | [inventory.cpp:1141](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1141) |
| `busy` | W | 稍后重试，避免紧密轮询 | 配置正在被其他操作使用，请稍后重试 | [settings.cpp:200](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/settings.cpp#L200) |
| `component-limit` | E | 释放过期对象/消费队列并检查泄漏；重启不是根治 | 已达到处理上限，请稍后重试或检查插件 | [bridge.cpp:120](https://github.com/Zen-Ink/xovi-extension-manager-ui/blob/4612724/src/bridge.cpp#L120) |
| `copy-failed` | E | 依据 errno/message 检查权限、只读文件系统、空间或文件完整性；修复后重试 | 文件或服务操作失败；详见原始原因 | [inventory.cpp:2021](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2021) |
| `decompression-failed` | E/F | 检查内存、资源包、压缩格式或 rebuilder 兼容性；按影响范围升级严重程度 | 资源处理失败，相关注入不可用 | `qt-resource-rebuilder/src/main.c:162`（工作区） |
| `dependency-version-incompatible:` | E | 补齐/启用/升级兼容依赖，修复后重启加载 | 插件依赖不可用或版本不兼容 | [inventory.cpp:1104](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1104) |
| `dependency-version-unknown:` | W | 不自动修复；按需补元数据或检查真实冲突 | 插件信息不完整或存在潜在冲突；不等于运行失败 | [inventory.cpp:1202](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1202) |
| `disable-legacy-failed` | E | 依据 errno/message 检查权限、只读文件系统、空间或文件完整性；修复后重试 | 文件或服务操作失败；详见原始原因 | [inventory.cpp:2675](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2675) |
| `disabled` | I/W | 检查是否预期跳过/目标尚未加载；不称为设置页成功 | 注入阶段状态；并非 QML 页面渲染结果 | `qt-resource-rebuilder/qmldiff/src/lib.rs:90`（工作区） |
| `disabled-dependency:` | E | 补齐/启用/升级兼容依赖，修复后重启加载 | 插件依赖不可用或版本不兼容 | [inventory.cpp:1100](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1100) |
| `disabled-dependency:qt-resource-rebuilder` | E | 先检查并恢复资源重建器，随后应用变更 | 资源重建器未启用或加载失败 | [inventory.cpp:1155](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1155) |
| `disabled-qmd-dependency:` | E | 补依赖、改顺序或消除循环；修复后重启应用 | QMD 依赖关系不满足 | [inventory.cpp:1118](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1118) |
| `duplicate-id` | E | 使用唯一注入标识，避免重复注册 | 注入标识已被注册 | `qt-resource-rebuilder/qmldiff/src/lib.rs:202`（工作区） |
| `effective-disabled` | E | 修复入口；仅有运行态变更时再提示重启 | 插件入口与启用设置不一致 | [inventory.cpp:1080](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1080) |
| `effective-enabled-while-disabled` | E | 修复入口；仅有运行态变更时再提示重启 | 插件入口与启用设置不一致 | [inventory.cpp:1081](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1081) |
| `enable-after-install-blocked` | W | 保留已安装结果，处理具体阻断项再启用 | 安装完成，但尚未启用 | [inventory.cpp:1883](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1883) |
| `enable-blocked` | E | 展示具体 issues，处理阻断条件 | 存在阻断问题，操作未完成 | [inventory.cpp:2401](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2401) |
| `entry-conflict` | W/E | 刷新状态并选正确操作或明确替换目标 | 当前对象状态不允许此操作 | [inventory.cpp:2636](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2636) |
| `entry-missing` | E | 恢复缺失文件或重新安装正确包 | 插件文件缺失 | [inventory.cpp:2069](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2069) |
| `extract-failed` | E | 依据 errno/message 检查权限、只读文件系统、空间或文件完整性；修复后重试 | 文件或服务操作失败；详见原始原因 | [inventory.cpp:2050](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2050) |
| `hashtab-mode` | I/W | 检查是否预期跳过/目标尚未加载；不称为设置页成功 | 注入阶段状态；并非 QML 页面渲染结果 | `qt-resource-rebuilder/qmldiff/src/lib.rs:1`（工作区） |
| `id-normalized-from-filename` | W | 不自动修复；按需补元数据或检查真实冲突 | 插件信息不完整或存在潜在冲突；不等于运行失败 | [inventory.cpp:1409](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1409) |
| `installed-conflict` | W/E | 刷新状态并选正确操作或明确替换目标 | 当前对象状态不允许此操作 | [inventory.cpp:2616](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2616) |
| `installed-legacy-conflict` | W/E | 先接管或明确处理现有旧式插件 | 已有旧式插件占用该标识 | [inventory.cpp:1818](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1818) |
| `installed-type-conflict` | W/E | 刷新状态并选正确操作或明确替换目标 | 当前对象状态不允许此操作 | [inventory.cpp:2617](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2617) |
| `invalid-action-id` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-action-id | [notifications.cpp:284](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/notifications.cpp#L284) |
| `invalid-actions` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-actions | [notifications.cpp:91](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/notifications.cpp#L91) |
| `invalid-chrome` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-chrome | [settings.cpp:293](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/settings.cpp#L293) |
| `invalid-command` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-command | [notifications.cpp:368](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/notifications.cpp#L368) |
| `invalid-component` | E | 修正线程、对象生命周期、注册 ID 和所有权 | 页面注册或对象生命周期不正确 | [bridge.cpp:116](https://github.com/Zen-Ink/xovi-extension-manager-ui/blob/4612724/src/bridge.cpp#L116) |
| `invalid-component-context` | E | 修正线程、对象生命周期、注册 ID 和所有权 | 页面注册或对象生命周期不正确 | [bridge.cpp:115](https://github.com/Zen-Ink/xovi-extension-manager-ui/blob/4612724/src/bridge.cpp#L115) |
| `invalid-compressed-size` | E/F | 检查内存、资源包、压缩格式或 rebuilder 兼容性；按影响范围升级严重程度 | 资源处理失败，相关注入不可用 | `qt-resource-rebuilder/src/main.c:156`（工作区） |
| `invalid-entry` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-entry | [inventory.cpp:1992](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1992) |
| `invalid-id` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-id | [inventory.cpp:1991](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1991) |
| `invalid-json` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-json | [settings.cpp:409](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/settings.cpp#L409) |
| `invalid-json-object` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-json-object | [inventory.cpp:844](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L844) |
| `invalid-launcher-file` | E | 备份并修复对应配置；不静默清空 | 配置文件损坏或格式不受支持 | [settings.cpp:203](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/settings.cpp#L203) |
| `invalid-level` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-level | [notifications.cpp:129](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/notifications.cpp#L129) |
| `invalid-location` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-location | [settings.cpp:303](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/settings.cpp#L303) |
| `invalid-manifest` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-manifest | [inventory.cpp:1990](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1990) |
| `invalid-message` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-message | [notifications.cpp:125](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/notifications.cpp#L125) |
| `invalid-notification-id` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-notification-id | [notifications.cpp:61](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/notifications.cpp#L61) |
| `invalid-order` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-order | [inventory.cpp:2601](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2601) |
| `invalid-output` | E | 按原始 message 修正 QMD/编码/注册时机；修复后重新应用 | 注入未完成，请检查补丁与固件兼容性 | `qt-resource-rebuilder/qmldiff/src/lib.rs:543`（工作区） |
| `invalid-owner` | E | 修正线程、对象生命周期、注册 ID 和所有权 | 页面注册或对象生命周期不正确 | [bridge.cpp:151](https://github.com/Zen-Ink/xovi-extension-manager-ui/blob/4612724/src/bridge.cpp#L151) |
| `invalid-owner-id` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-owner-id | [notifications.cpp:60](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/notifications.cpp#L60) |
| `invalid-page` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-page | [settings.cpp:287](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/settings.cpp#L287) |
| `invalid-page-id` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-page-id | [notifications.cpp:128](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/notifications.cpp#L128) |
| `invalid-page-kind` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-page-kind | [settings.cpp:288](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/settings.cpp#L288) |
| `invalid-page-url` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-page-url | [settings.cpp:291](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/settings.cpp#L291) |
| `invalid-policy-file` | E | 备份并修复对应配置；不静默清空 | 配置文件损坏或格式不受支持 | [settings.cpp:382](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/settings.cpp#L382) |
| `invalid-progress` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-progress | [notifications.cpp:75](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/notifications.cpp#L75) |
| `invalid-registration` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-registration | [settings.cpp:280](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/settings.cpp#L280) |
| `invalid-request` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-request | [settings.cpp:304](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/settings.cpp#L304) |
| `invalid-revision` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-revision | [notifications.cpp:248](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/notifications.cpp#L248) |
| `invalid-sequence` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-sequence | [notifications.cpp:290](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/notifications.cpp#L290) |
| `invalid-settings-file` | E | 备份并修复对应配置；不静默清空 | 配置文件损坏或格式不受支持 | [settings.cpp:357](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/settings.cpp#L357) |
| `invalid-state` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-state | [notifications.cpp:130](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/notifications.cpp#L130) |
| `invalid-title` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-title | [notifications.cpp:124](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/notifications.cpp#L124) |
| `invalid-type` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：invalid-type | [inventory.cpp:2577](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2577) |
| `invalid-utf8` | E | 按原始 message 修正 QMD/编码/注册时机；修复后重新应用 | 注入未完成，请检查补丁与固件兼容性 | `qt-resource-rebuilder/qmldiff/src/lib.rs:195`（工作区） |
| `last-manager-entry` | W | 先处理依赖/保留另一入口；不自动重复操作 | 此操作会破坏依赖或失去管理入口，已阻止 | [settings.cpp:329](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/settings.cpp#L329) |
| `launcher-entry-not-found` | W | 刷新并确认对象存在/已启用；不默认重启 | 目标已不存在、未启用或当前不可用 | [settings.cpp:318](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/settings.cpp#L318) |
| `legacy-active-move-failed` | E | 依据 errno/message 检查权限、只读文件系统、空间或文件完整性；修复后重试 | 文件或服务操作失败；详见原始原因 | [inventory.cpp:2773](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2773) |
| `legacy-directory-move-failed` | E | 依据 errno/message 检查权限、只读文件系统、空间或文件完整性；修复后重试 | 文件或服务操作失败；详见原始原因 | [inventory.cpp:2788](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2788) |
| `manager-unavailable` | W/E | 等待初始化，或检查组件是否启用/加载失败 | 所需服务尚未就绪或不可用 | [bridge.cpp:39](https://github.com/Zen-Ink/xovi-extension-manager-ui/blob/4612724/src/bridge.cpp#L39) |
| `manifest-entry-mismatch` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：manifest-entry-mismatch | [inventory.cpp:2610](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2610) |
| `manifest-id-mismatch` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：manifest-id-mismatch | [inventory.cpp:2608](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2608) |
| `manifest-invalid` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：manifest-invalid | [inventory.cpp:1075](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1075) |
| `manifest-not-found` | E | 恢复缺失文件或重新安装正确包 | 插件文件缺失 | [inventory.cpp:2587](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2587) |
| `manifest-order-mismatch` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：manifest-order-mismatch | [inventory.cpp:2611](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2611) |
| `manifest-type-mismatch` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：manifest-type-mismatch | [inventory.cpp:2590](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2590) |
| `missing-dependency:` | E | 补齐/启用/升级兼容依赖，修复后重启加载 | 插件依赖不可用或版本不兼容 | [inventory.cpp:1097](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1097) |
| `missing-enabled` | W | 不自动修复；按需补元数据或检查真实冲突 | 插件信息不完整或存在潜在冲突；不等于运行失败 | [inventory.cpp:1](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1) |
| `missing-entry` | W/E | native 可推断入口时仅警告；QMD/QML 缺少入口应修正清单 | 插件未声明入口；是否阻断取决于包类型 | [inventory.cpp:885](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L885) |
| `missing-id` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：missing-id | [inventory.cpp:852](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L852) |
| `missing-manifest` | W/E | 清单显示仅警告；需要 manifest 的安装/修复操作应补清单或改用支持的接入方式 | 插件未提供清单；当前操作是否需要它取决于接口 | [inventory.cpp:2056](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2056) |
| `missing-manifestVersion` | W | 不自动修复；按需补元数据或检查真实冲突 | 插件信息不完整或存在潜在冲突；不等于运行失败 | [inventory.cpp:848](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L848) |
| `missing-name` | W | 不自动修复；按需补元数据或检查真实冲突 | 插件信息不完整或存在潜在冲突；不等于运行失败 | [inventory.cpp:1](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1) |
| `missing-order` | W | 不自动修复；按需补元数据或检查真实冲突 | 插件信息不完整或存在潜在冲突；不等于运行失败 | [inventory.cpp:902](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L902) |
| `missing-path` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：missing-path | [inventory.cpp:2545](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2545) |
| `missing-qmd-dependency:` | E | 补依赖、改顺序或消除循环；修复后重启应用 | QMD 依赖关系不满足 | [inventory.cpp:1113](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1113) |
| `missing-requires` | W | 不自动修复；按需补元数据或检查真实冲突 | 插件信息不完整或存在潜在冲突；不等于运行失败 | [inventory.cpp:911](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L911) |
| `missing-requires.xovi` | W | 不自动修复；按需补元数据或检查真实冲突 | 插件信息不完整或存在潜在冲突；不等于运行失败 | [inventory.cpp:919](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L919) |
| `missing-runtime-dependency:qt-resource-rebuilder` | E | 补齐/启用/升级兼容依赖，修复后重启加载 | 插件依赖不可用或版本不兼容 | [inventory.cpp:1154](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1154) |
| `missing-target` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：missing-target | [inventory.cpp:2559](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2559) |
| `missing-title` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：missing-title | [bridge.cpp:107](https://github.com/Zen-Ink/xovi-extension-manager-ui/blob/4612724/src/bridge.cpp#L107) |
| `missing-type` | W | 不自动修复；按需补元数据或检查真实冲突 | 插件信息不完整或存在潜在冲突；不等于运行失败 | [inventory.cpp:849](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L849) |
| `missing-version` | W | 不自动修复；按需补元数据或检查真实冲突 | 插件信息不完整或存在潜在冲突；不等于运行失败 | [inventory.cpp:1](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1) |
| `mkdir-failed` | E | 依据 errno/message 检查权限、只读文件系统、空间或文件完整性；修复后重试 | 文件或服务操作失败；详见原始原因 | [inventory.cpp:2016](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2016) |
| `navigation-failed` | E | 检查原生导航回调及具体异常；不默认重启 | 无法打开目标系统设置 | [bridge.cpp:237](https://github.com/Zen-Ink/xovi-extension-manager-ui/blob/4612724/src/bridge.cpp#L237) |
| `navigation-unavailable` | W/E | 等待初始化，或检查组件是否启用/加载失败 | 所需服务尚未就绪或不可用 | [bridge.cpp:235](https://github.com/Zen-Ink/xovi-extension-manager-ui/blob/4612724/src/bridge.cpp#L235) |
| `no-applicable-changes` | I/W | 检查是否预期跳过/目标尚未加载；不称为设置页成功 | 注入阶段状态；并非 QML 页面渲染结果 | `qt-resource-rebuilder/qmldiff/src/lib.rs:107`（工作区） |
| `no-matching-block` | I/W | 检查是否预期跳过/目标尚未加载；不称为设置页成功 | 注入阶段状态；并非 QML 页面渲染结果 | `qt-resource-rebuilder/qmldiff/src/lib.rs:1`（工作区） |
| `not-active-legacy` | W/E | 刷新状态并选正确操作或明确替换目标 | 当前对象状态不允许此操作 | [inventory.cpp:2572](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2572) |
| `not-found` | W | 刷新并确认对象存在/已启用；不默认重启 | 目标已不存在、未启用或当前不可用 | [inventory.cpp:1965](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1965) |
| `not-legacy` | W/E | 刷新状态并选正确操作或明确替换目标 | 当前对象状态不允许此操作 | [inventory.cpp:2720](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2720) |
| `page-already-registered` | E | 修正线程、对象生命周期、注册 ID 和所有权 | 页面注册或对象生命周期不正确 | [settings.cpp:282](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/settings.cpp#L282) |
| `page-not-found` | W | 刷新并确认对象存在/已启用；不默认重启 | 目标已不存在、未启用或当前不可用 | [bridge.cpp:48](https://github.com/Zen-Ink/xovi-extension-manager-ui/blob/4612724/src/bridge.cpp#L48) |
| `page-unavailable` | W | 刷新并确认对象存在/已启用；不默认重启 | 目标已不存在、未启用或当前不可用 | [bridge.cpp:44](https://github.com/Zen-Ink/xovi-extension-manager-ui/blob/4612724/src/bridge.cpp#L44) |
| `parse-failed` | E | 按原始 message 修正 QMD/编码/注册时机；修复后重新应用 | 注入未完成，请检查补丁与固件兼容性 | `qt-resource-rebuilder/qmldiff/src/lib.rs:426`（工作区） |
| `parse-panic` | E | 按原始 message 修正 QMD/编码/注册时机；修复后重新应用 | 注入未完成，请检查补丁与固件兼容性 | `qt-resource-rebuilder/qmldiff/src/lib.rs:427`（工作区） |
| `policy-busy` | W | 稍后重试，避免紧密轮询 | 配置正在被其他操作使用，请稍后重试 | [settings.cpp:380](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/settings.cpp#L380) |
| `policy-invalid` | E | 按原始 message 修正 QMD/编码/注册时机；修复后重新应用 | 注入未完成，请检查补丁与固件兼容性 | `qt-resource-rebuilder/qmldiff/src/lib.rs:399`（工作区） |
| `processed` | I/W | 检查是否预期跳过/目标尚未加载；不称为设置页成功 | 注入阶段状态；并非 QML 页面渲染结果 | `qt-resource-rebuilder/qmldiff/src/lib.rs:1`（工作区） |
| `qmd-dependencies-invalid` | E | 处理返回 issues 中的依赖、顺序或启用消费者 | QMD 依赖阻止本次操作 | [inventory.cpp:1760](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1760) |
| `qmd-dependency-cycle:` | E | 补依赖、改顺序或消除循环；修复后重启应用 | QMD 依赖关系不满足 | [inventory.cpp:1128](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1128) |
| `qmd-dependency-order-invalid:` | E | 补依赖、改顺序或消除循环；修复后重启应用 | QMD 依赖关系不满足 | [inventory.cpp:1124](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1124) |
| `qmd-dependency-version-incompatible:` | E | 补依赖、改顺序或消除循环；修复后重启应用 | QMD 依赖关系不满足 | [inventory.cpp:1121](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1121) |
| `qmd-order-conflict:` | W | 不自动修复；按需补元数据或检查真实冲突 | 插件信息不完整或存在潜在冲突；不等于运行失败 | [inventory.cpp:1193](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1193) |
| `qmd-required-by` | W | 先处理依赖/保留另一入口；不自动重复操作 | 此操作会破坏依赖或失去管理入口，已阻止 | [inventory.cpp:1795](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1795) |
| `qmd-required-by:` | E | 处理返回 issues 中的依赖、顺序或启用消费者 | QMD 依赖阻止本次操作 | [inventory.cpp:1067](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1067) |
| `rcc-registration-failed` | E/F | 检查内存、资源包、压缩格式或 rebuilder 兼容性；按影响范围升级严重程度 | 资源处理失败，相关注入不可用 | `qt-resource-rebuilder/src/rccload.cpp:6`（工作区） |
| `rebuilder-disabled` | E/F | 检查内存、资源包、压缩格式或 rebuilder 兼容性；按影响范围升级严重程度 | 资源处理失败，相关注入不可用 | `qt-resource-rebuilder/qmldiff/src/lib.rs:268`（工作区） |
| `rebuilder-unavailable` | W/E | 等待初始化，或检查组件是否启用/加载失败 | 所需服务尚未就绪或不可用 | [settings.cpp:337](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/settings.cpp#L337) |
| `registered` | I/W | 检查是否预期跳过/目标尚未加载；不称为设置页成功 | 注入阶段状态；并非 QML 页面渲染结果 | `qt-resource-rebuilder/qmldiff/src/lib.rs:126`（工作区） |
| `registration-closed` | E | 按原始 message 修正 QMD/编码/注册时机；修复后重新应用 | 注入未完成，请检查补丁与固件兼容性 | `qt-resource-rebuilder/qmldiff/src/lib.rs:390`（工作区） |
| `registration-failed` | E | 按原始 message 修正 QMD/编码/注册时机；修复后重新应用 | 注入未完成，请检查补丁与固件兼容性 | `qt-resource-rebuilder/qmldiff/src/lib.rs:163`（工作区） |
| `registration-limit` | E | 释放过期对象/消费队列并检查泄漏；重启不是根治 | 已达到处理上限，请稍后重试或检查插件 | [settings.cpp:284](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/settings.cpp#L284) |
| `registration-not-owned` | E | 修正线程、对象生命周期、注册 ID 和所有权 | 页面注册或对象生命周期不正确 | [bridge.cpp:176](https://github.com/Zen-Ink/xovi-extension-manager-ui/blob/4612724/src/bridge.cpp#L176) |
| `remove-failed` | E | 依据 errno/message 检查权限、只读文件系统、空间或文件完整性；修复后重试 | 文件或服务操作失败；详见原始原因 | [inventory.cpp:2842](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2842) |
| `repair-blocked` | E | 展示具体 issues，处理阻断条件 | 存在阻断问题，操作未完成 | [inventory.cpp:2490](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2490) |
| `replacement-conflict` | W/E | 确认覆盖是否有意；检查实际生效资源，不自动判全部失败 | 同一资源存在覆盖或替换冲突 | `qt-resource-rebuilder/src/indexfile.c:44`（工作区） |
| `request-too-large` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：request-too-large | [notifications.cpp:369](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/notifications.cpp#L369) |
| `resource-registered` | I/W | 检查是否预期跳过/目标尚未加载；不称为设置页成功 | 注入阶段状态；并非 QML 页面渲染结果 | `qt-resource-rebuilder/qmldiff/src/status.rs:1`（工作区） |
| `resource-registration-failed` | E/F | 检查内存、资源包、压缩格式或 rebuilder 兼容性；按影响范围升级严重程度 | 资源处理失败，相关注入不可用 | `qt-resource-rebuilder/qmldiff/src/status.rs:1`（工作区） |
| `revision-conflict` | W | 刷新后重新确认操作；不覆盖旧版本 | 内容已经更新，请刷新后重试 | [settings.cpp:363](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/settings.cpp#L363) |
| `revision-exhausted` | E | 备份并迁移配置版本计数；不能无限重试 | 设置版本计数已达上限 | [settings.cpp:364](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/settings.cpp#L364) |
| `runtime-condition-failed` | I/E | 区分插件主动跳过与异常；核对适用设备/进程/条件 | 插件未满足加载条件；不默认建议重启 | [inventory.cpp:1168](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1168) |
| `runtime-dependency-failed` | E | 补齐/启用/升级兼容依赖，修复后重启加载 | 插件依赖不可用或版本不兼容 | [inventory.cpp:1171](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1171) |
| `runtime-dependency-failed:` | E | 补齐/启用/升级兼容依赖，修复后重启加载 | 插件依赖不可用或版本不兼容 | [inventory.cpp:1101](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1101) |
| `runtime-dependency-failed:qt-resource-rebuilder` | E | 先检查并恢复资源重建器，随后应用变更 | 资源重建器未启用或加载失败 | [inventory.cpp:1156](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1156) |
| `runtime-dlopen-failed` | E | 检查 loadError/ELF/依赖；修复后重新加载，重启本身无效 | 插件加载失败；请查看具体原因并安装兼容构建 | [inventory.cpp:1162](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1162) |
| `runtime-link-failed` | E | 补齐/启用/升级兼容依赖，修复后重启加载 | 插件依赖不可用或版本不兼容 | [inventory.cpp:1174](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1174) |
| `runtime-shouldload-failed` | I/E | 区分插件主动跳过与异常；核对适用设备/进程/条件 | 插件未满足加载条件；不默认建议重启 | [inventory.cpp:1165](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1165) |
| `self-disable-blocked` | W | 先处理依赖/保留另一入口；不自动重复操作 | 此操作会破坏依赖或失去管理入口，已阻止 | [inventory.cpp:2386](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2386) |
| `self-remove-blocked` | W | 先处理依赖/保留另一入口；不自动重复操作 | 此操作会破坏依赖或失去管理入口，已阻止 | [inventory.cpp:2756](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2756) |
| `settings-busy` | W | 稍后重试，避免紧密轮询 | 配置正在被其他操作使用，请稍后重试 | [settings.cpp:355](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/settings.cpp#L355) |
| `shadowed-by-qrr` | W/E | 确认覆盖是否有意；检查实际生效资源，不自动判全部失败 | 同一资源存在覆盖或替换冲突 | `qt-resource-rebuilder/src/main.c:225`（工作区） |
| `snapshot-unavailable` | W/E | 等待初始化，或检查组件是否启用/加载失败 | 所需服务尚未就绪或不可用 | [settings.cpp:339](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/settings.cpp#L339) |
| `source-entry-missing` | E | 恢复缺失文件或重新安装正确包 | 插件文件缺失 | [inventory.cpp:2492](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2492) |
| `stale-notification` | W | 刷新后重新确认操作；不覆盖旧版本 | 内容已经更新，请刷新后重试 | [notifications.cpp:291](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/notifications.cpp#L291) |
| `status-limit` | E | 释放过期对象/消费队列并检查泄漏；重启不是根治 | 已达到处理上限，请稍后重试或检查插件 | [settings.cpp:392](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/settings.cpp#L392) |
| `target-not-seen` | I/W | 检查是否预期跳过/目标尚未加载；不称为设置页成功 | 注入阶段状态；并非 QML 页面渲染结果 | `qt-resource-rebuilder/qmldiff/src/lib.rs:118`（工作区） |
| `tempdir-failed` | E | 依据 errno/message 检查权限、只读文件系统、空间或文件完整性；修复后重试 | 文件或服务操作失败；详见原始原因 | [inventory.cpp:2010](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2010) |
| `type-mismatch` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：type-mismatch | [inventory.cpp:2578](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2578) |
| `ui-not-ready` | W/E | 等待初始化，或检查组件是否启用/加载失败 | 所需服务尚未就绪或不可用 | [bridge.cpp:33](https://github.com/Zen-Ink/xovi-extension-manager-ui/blob/4612724/src/bridge.cpp#L33) |
| `unknown-command` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：unknown-command | [settings.cpp:399](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/settings.cpp#L399) |
| `unmanaged` | W | 不自动修复；按需补元数据或检查真实冲突 | 插件信息不完整或存在潜在冲突；不等于运行失败 | [inventory.cpp:938](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L938) |
| `unmanaged-package` | W/E | 刷新状态并选正确操作或明确替换目标 | 当前对象状态不允许此操作 | [inventory.cpp:2754](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2754) |
| `unsafe-active-entry` | E | 保留原文件，检查路径、类型或权限；禁止盲目覆盖 | 入口或路径不符合预期，操作已停止 | [inventory.cpp:2778](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2778) |
| `unsafe-package-path` | E | 保留原文件，检查路径、类型或权限；禁止盲目覆盖 | 入口或路径不符合预期，操作已停止 | [inventory.cpp:2815](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2815) |
| `unsupported-compression` | E/F | 检查内存、资源包、压缩格式或 rebuilder 兼容性；按影响范围升级严重程度 | 资源处理失败，相关注入不可用 | `qt-resource-rebuilder/src/main.c:173`（工作区） |
| `unsupported-file` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：unsupported-file | [inventory.cpp:1968](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1968) |
| `unsupported-native-location` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：unsupported-native-location | [settings.cpp:314](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/settings.cpp#L314) |
| `unsupported-package` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：unsupported-package | [inventory.cpp:2554](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2554) |
| `unsupported-target` | E | 修正请求字段、清单或包格式；重启无效 | 请求或插件声明无效：unsupported-target | [bridge.cpp:234](https://github.com/Zen-Ink/xovi-extension-manager-ui/blob/4612724/src/bridge.cpp#L234) |
| `write-failed` | E | 依据 errno/message 检查权限、只读文件系统、空间或文件完整性；修复后重试 | 文件或服务操作失败；详见原始原因 | [inventory.cpp:2026](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L2026) |
| `xochitl-version-incompatible` | E | 换用兼容插件/适配固件；不要只重启 | 插件不支持当前运行环境 | [inventory.cpp:1146](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1146) |
| `xovi-version-incompatible` | E | 换用兼容插件/适配固件；不要只重启 | 插件不支持当前运行环境 | [inventory.cpp:1084](https://github.com/Zen-Ink/xovi-extension-manager/blob/cdae6f8/src/inventory.cpp#L1084) |

## 无固定错误码的反馈盲区

| 原始报告/状态 | 级别与处理 | 建议报告 |
|---|---|---|
| Non-existent attached object / Cannot override FINAL property | E；检查 import、Qt/固件版本和属性声明 | 页面与当前运行环境不兼容，请更新插件 |
| Type unavailable / module is not installed / No such file | E；先看内层错误，检查模块、RCC 和资源路径 | 页面所需组件无法加载，查看缺失项 |
| Cannot create a component in an invalid context | E；修复创建上下文/owner 生命周期 | 页面上下文已失效，请重新打开；重复发生需修复插件 |
| Page component owner was destroyed / context is no longer valid | E 或正常退出产生的 I；重新注册、保持 owner 存活 | 页面已失效，请重新打开 |
| Settings root must be an Item with a settingsContext property | E；必须为 Item，只有 usesSettingsContext=true 才要求该属性；当前文案过宽 | 页面根对象或声明的宿主接口不符合要求 |
| QML engine unavailable / Settings page unavailable | W/E；区分页面关闭/引擎未就绪/服务失败 | 页面当前不可用 |
| TypeError / ReferenceError / Binding loop / recursive rearrange | W/E；按实际功能影响处理，不因组件创建成功忽略 | 页面发生运行错误，部分功能可能不可用 |
| target-not-seen | I，资源按需加载时正常；确认应出现而未出现再升 W/E | 等待目标资源加载 |
| registered / prepared / applied / ready | 分别只证明接收/预处理/资源注册/对象创建；不证明长期运行无错误 | 保留阶段描述，不统一称“插件正常” |
| hashtab/规则读取失败、固件版本未知 | 当前部分仅 eprintln 日志，未全部进入结构化结果；检查 lib.rs::load_hashtab/load_rules | 哈希补丁缺少当前固件的映射或规则 |
| 崩溃、死锁、无限同步循环、进程被终止 | F 需日志/dmp/看门狗证据；同进程 API 无法保证完整上报 | 主程序异常，需定位并隔离故障插件 |

## 建议统一报告字段

`code`（稳定码）、`category`（参数/文件/依赖/兼容/入口/加载/注入/页面/并发/保护）、`severity`、`scope`、`summaryKey`、`detail`、`actions`、`retryable`，以及独立的 `pendingChange` / `restartToApply`。

同一码按 operation、stage、owner 和原始 cause 分级。保留错误链：依赖不可用 → 依赖 dlopen 失败 → wrong ELF class。不要将整条链压成“需要重启”。

修复按钮只用于当前有实现、能安全验证的入口协调。错误包、ABI 不兼容、页面代码错误、损坏配置不能冒充“一键修复”。未知错误默认 E + 原始详情 + 检查建议，不默认重启。

## 修改前的 UI 中文报告原文

这些是现有翻译，不是上面的整改建议。

| 英文源文 | 当前简体中文 |
|---|---|
| Manager unavailable | 管理器不可用 |
| Load failed | 加载失败 |
| Activation needs repair | 启用状态需要修复 |
| Restart required | 需要重启 |
| Page unavailable | 页面不可用 |
| Activation repaired. Restart xochitl to apply. | 启用状态已修复，重启 xochitl 后生效。 |
| Activation repaired | 启用状态已修复 |
| Unable to repair activation. Open diagnostics for details. | 无法修复启用状态，请查看诊断详情。 |
| Unable to save | 无法保存 |
| Needs repair | 需要修复 |
| Restart | 重启 |
| Repair | 修复 |
| Restart xochitl to apply | 重启 xochitl 后生效 |
| Repair activation | 修复启用状态 |
| Unable to open page | 无法打开页面 |
| Action unavailable | 操作不可用 |
| This plugin's settings page is not compatible with this device. | 此插件的设置页面与当前设备不兼容。 |
| A component required by this page is missing. | 此页面缺少必需的组件。 |
| This settings page is currently unavailable. | 此设置页面暂不可用。 |
| This plugin's settings page could not be opened. | 无法打开此插件的设置页面。 |
| Update the plugin, then restart xochitl. | 请更新插件，然后重启 xochitl。 |

## 审计时的 API 固定英文 message

下列为源码中直接给出的固定原文；errno/Qt/Rust 的动态原文以现场返回为准。只有 error 而无 message 的接口已在逐码表中列出。

| code | 当前 message |
|---|---|
| `activation-cleanup-failed` | Disabled, but an active entry could not be removed; regular files are preserved |
| `active-directory-failed` | cannot create active entry directory |
| `active-entry-conflict` | active entry is not a symlink and was left in place |
| `active-entry-conflict` | active entry points somewhere else |
| `active-entry-repair-failed` |  |
| `already-managed` | package is already managed |
| `enable-blocked` | package has blocking issues |
| `entry-conflict` | target entry already exists and is not the legacy source |
| `entry-missing` | archive manifest entry file was not found |
| `entry-missing` | legacy source entry was not found |
| `installed-conflict` | a managed package with this id already exists |
| `installed-conflict` | target manifest already exists |
| `installed-type-conflict` | a legacy package with this id already exists with a different type |
| `invalid-entry` | entry is invalid for package type |
| `invalid-id` | package id is invalid |
| `invalid-manifest` | package manifest must be fixed before changing enabled state |
| `invalid-manifest` | package manifest must be fixed before repair |
| `invalid-order` | QMD order must be between 0 and 999 |
| `invalid-type` | package type could not be inferred |
| `manifest-entry-mismatch` | provided manifest entry does not match requested entry |
| `manifest-id-mismatch` | provided manifest id does not match requested id |
| `manifest-not-found` | provided manifestPath was not found |
| `manifest-order-mismatch` | provided manifest order does not match requested order |
| `manifest-type-mismatch` | provided manifest type does not match legacy package type |
| `manifest-type-mismatch` | provided manifest type does not match requested type |
| `missing-manifest` | archive does not contain manifest.json |
| `missing-manifest` | package has no manifest.json |
| `missing-manifest` | repair requires a managed package manifest |
| `missing-path` | install request requires a path |
| `missing-target` | adopt requires an id or path |
| `missing-target` | disableLegacy requires an id or path |
| `missing-target` | remove requires an id or path |
| `mkdir-failed` | legacy-disabled directory could not be created |
| `mkdir-failed` | target entry directory could not be created |
| `mkdir-failed` | target package directory could not be created |
| `mkdir-failed` | temporary package directory could not be created |
| `not-active-legacy` | adopt path must be an active .so or .qmd entry |
| `not-active-legacy` | package is not a legacy active file |
| `not-found` | install path was not found |
| `not-found` | legacy package was not found |
| `not-found` | legacy path was not found |
| `not-found` | package disappeared during repair |
| `not-found` | package was not found |
| `not-legacy` | disableLegacy only applies to unmanaged legacy packages |
| `qmd-dependencies-invalid` | proposed QMD package has blocking QMD dependencies |
| `qmd-required-by` | QMD package is required by enabled consumers |
| `qmd-required-by` | proposed QMD package would invalidate enabled consumers |
| `repair-blocked` | package has blocking issues |
| `self-disable-blocked` | xovi-extension-manager cannot disable itself |
| `self-remove-blocked` | xovi-extension-manager cannot remove itself |
| `source-entry-missing` | package source entry is missing |
| `type-mismatch` | requested type does not match legacy package type |
| `unmanaged-package` | remove only applies to managed or legacy packages |
| `unsafe-active-entry` | legacy active entry is not a regular file or symlink |
| `unsafe-package-path` | package directory is outside the manager-owned layout |
| `unsupported-file` | adopt supports .so and .qmd files |
| `unsupported-file` | single-file install supports .so, .qmd and .qml |
| `unsupported-package` | install supports .so, .qmd, .qml, .tar.gz, .tgz, and .zip |

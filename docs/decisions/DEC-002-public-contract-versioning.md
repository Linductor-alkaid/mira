# DEC-002：公共契约、结果和版本化边界

> 状态：Accepted  
> 日期：2026-08-30  
> 负责人：Mira Maintainers  
> 冻结里程碑：M1  
> 替代/被替代：无

## 背景与问题

Mira Core、Host、Provider、Adapter、持久化和 Replay 会独立演进。若公共 C++ API 暴露 Executor、
平台 SDK、异常或未版本化 variant，升级时无法区分兼容扩展与破坏性变化。

## 决策

- Core 公共边界使用 C++20 强类型值对象和 `Result<T>`；第三方异常、JNI 类型、Executor handle、
  ONNX/OpenCV 类型不得穿过公共接口。
- 命令调用返回 `CommandReceipt`，仅说明 accepted/rejected；实际效果以带 `CommandId` 的
  `CommandOutcome`、Task snapshot 和事件为准。
- wire/event/persistence schema 使用显式 `major.minor`：新增可忽略字段提升 minor，删除字段、改变
  含义或默认安全行为提升 major。
- 枚举保留 `Unknown`，reader 对未知 action、权限和副作用类型默认拒绝；纯诊断字段可保留并转发。
- v1 不承诺稳定 C++ ABI。稳定集成面是版本化语义和未来的窄 C Host ABI；公共 C++ 源兼容按发布
  说明管理。
- reader 至少支持当前和上一受支持 major 的显式迁移；不能解析时进入受限诊断模式，不清空数据。

## 备选方案

- 首版即冻结跨编译器 C++ ABI：成本过高且会妨碍核心契约收敛，不采用。
- 只依赖 JSON 的宽松兼容：无法保证安全默认值和类型约束，不采用。
- 用异常作为公共错误协议：跨 ABI 和语言边界不稳定，不采用。

## 影响与风险

所有公开类型需要 golden/schema tests。未来 Android JNI 只调用 Host C ABI 或稳定 facade，不直接
映射复杂 STL 对象。为避免未知字段造成权限提升，安全相关 decoder 必须 fail closed。

## 验证方式

- 公共头独立包含、最小 consumer、异常边界和 current/previous schema fixtures。
- 未知 action、permission、risk 和 schema major 的负向测试。

## 关联文档和工作项

- [M1 核心契约](../plans/m1-core-contracts.md)：`M1-01`、`M1-02`、`M1-17`、`M1-18`
- [核心公共契约与状态机设计](../design/core_contracts_and_state_machine.md)


# Messages兼容性验收（2026-10-06）

> 状态：Completed（Linux本地范围）；M3-21 / DEC-050
> 负责人：Mira维护者

固定基线0a099ba，保持Executor2ae4fc8。新增显式Messages方言与SecretRef ApiKey模式，不改调度/生命周期，不回退到OpenAI。原协议默认Bearer兼容，明确新增enum/API成员与链接单元。

Linux GCC Debug在/tmp/mira-stream-build实际构建并运行mira_m3_anthropic_test/dialect/sse/transport/gateway/tool_loop/conversation_loop共7/7；新增fixture覆盖全部双片切点（包括UTF-8中间）、逐字节工具JSON、缺终态、错误/预算、缓存计数与未知usage元数据、SecretRef真实socket认证及header/Key注入拒绝。架构/文档/格式/SBOM/平台边界通过。不要将夹具等同于所有厂商服务。

MiniMax官方文档确认M3可接受图片，M2系列不支持；2026-10-06使用维护者授权的本机模型配置/Key，Mira公开Messages Provider + SocketHttpTransport/OpenSSL + Executor发送两张随机验证码/图形PNG。图片4580/5304字节，返回验证码7G3PDH/0X5JJ9、蓝色圆形/绿色三角形均匹配，4/6个预览，message_stop终态与input_tokens385。问题中没有答案，不使用用户桌面图或敏感资料。首次失败是额外usage对象字段被错误当作计数，已修复并有fixture覆盖。初次失败不计成功。

同一本机MiniMax-M3通过Mirage真实ModelLayer/ConversationLoop执行wait1毫秒，结果ok=1 steps=2 tools=1，回复“工具往返成功”；从空配置选择MiniMax预设只输入Key保存/刷新后真实会话也成功（138次预览，1063字节最终回复）。相关外部驱动/图片在Mirage M6-22证据中保存，Key不进入Git、输出或CLI参数。

参考：[Messages规格](https://platform.claude.com/docs/en/api/messages/create)、[SSE](https://platform.claude.com/docs/en/build-with-claude/streaming)、[MiniMax兼容规格](https://platform.minimax.io/docs/api-reference/text-anthropic-api)。

首阶段不支持extended thinking签名回填、服务端工具、非Text模式和非Auto图片细节；未知/不可表达内容明确失败，不删掉thinking后继续执行工具。Claude真实服务与其他厂商未执行（无已授权Key），维护者在对应授权环境复跑；Messages fixture及确定性逐字节变异种子已链接Mirage的ASAN/UBSAN/TSAN依赖库实际运行通过；TSAN用setarch x86_64 -R。Windows/Android编译与完整跨平台矩阵由本PR CI继续验证，不由Linux结果外推。

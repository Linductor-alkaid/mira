# HTTP 流式连接策略维护

> 状态：Completed
> 负责人：Mira Maintainers
> 更新日期：2026-11-07
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)
> 决策：[DEC-091](../decisions/DEC-091-http-stream-connection-policy.md)
> 设计：[LLM API 协议设计](../design/llm-api-protocol-design.md) §9.4

## 目标与非目标

修复 SocketHttpTransport 对敏感 SSE 端点无法表达 keep-alive 意图的问题。提供
默认 false 的请求级字段，模型 stream 自动接线；不增加连接复用、TCP keepalive、
重连、永久事件订阅 owner 或新的 Executor 并发路径。不修改第三方代码与 SSE parser。

## 工作项与退出条件

- [x] `MNT-202611-SC-01`：请求字段、唯一 Connection 头、模型流式接线与公开契约同步。
- [x] SC-G1：默认 close 与显式 keep-alive wire、普通头拒绝、敏感端点分片 SSE 和
  Exchange 客户端释放通过真实 socket fixture 验证。
- [x] SC-G2：keep-alive 下取消、idle 超时、shutdown、截断、大小上限和重定向的结果
  及回收语义明确且测试通过，HTTP 代理/TLS/CONNECT 路径受同一策略控制。
- [x] SC-G3：三模型方言流式启用、非流式默认 false 的 provider 接线测试通过。
- [x] SC-G4：适用回归、严格构建、sanitizer 与 format/docs/architecture/platform-boundary
  门禁通过，记录准确命令和证据边界。

## 风险与补跑条件

本地 fixture 模拟宿主报告的 header 敏感行为；不能推导所有 Node/Express 都拒绝
close 请求。真实 NapCat 服务当前未部署，负责人为部署宿主维护者；补跑需固定
NapCat/Node/宿主版本、端点配置和字节捕获，对照两种连接策略及取消/重连。
Windows/Android 目标本轮本机未配置，不能以 Linux 结果宣称跨平台互操作。

## 验证记录

2026-11-07：从干净 `master` `effae6e` 建立 `fix/http-stream-connection-policy`。
定位固定 Connection 头及现有保留头拒绝；按 DEC-091 冻结显式头语义、默认兼容性、
模型 stream 接线与单 Exchange 所有权。测试异步工作沿用公开 `submit_auto()`，
保留并消费 future，socket 等待有界；外部 owner 先 shutdown transport，再由非
worker 关闭 Executor。待补实际验证命令与结果。

2026-11-07 首轮结果：GCC Debug 新专项首次失败在 shutdown 用例：错误预期为
ModelCancelled，实际为 ModelDeadlineExceeded（`exchange stage deadline exceeded`，
2.33 秒）；加入诊断复跑确认同一原因。查明既有 shutdown 只停止队列并等待活动交换
按请求 context/deadline 收敛，不是新的取消源。测试改为 owner 先取消活动 context，
再 shutdown，符合仓库规定；生产 shutdown 路径未改。同期 OpenSSL/Mbed TLS 测试
2/2 通过，四项静态门禁通过。最终验证待后续记录。

2026-11-07 第二轮：专项调整 owner 取消顺序后 Debug 1/1 通过（0.39 秒），
ASAN+UBSAN 专项通过，TSAN 专项 1/1 通过（0.44 秒）。本地 clang-tidy 对两处生产
实现检查通过，但新测试 main 被 `bugprone-exception-escape` 拒绝；增加显式异常
失败边界（打印诊断并返回非零），继续消费所有受控 future，不忽略执行异常。
待重建最终测试并完成宽回归。

2026-11-07 Node 参考对照：本机 Node v22.23.2 的普通 HTTP SSE 服务对 close 与
keep-alive 均返回两个事件（异步分开发送），请求策略通过回显头核对；不复现“所有
Node 都饿死”的结论。参考脚本最初使用全局 agent 时第二请求返回 400，使回显自检
失败；改为 `agent:false` 控制每请求新连接后 2/2 通过，不将该客户端现象归因于
Mira。这个独立参考实验只用于限定根因推论，不是 Mira/NapCat 互操作证据，也不
代替 socket fixture。准确复现：

```bash
node - <<'JS'
const http = require('node:http');
const assert = require('node:assert/strict');
const server = http.createServer((req, res) => {
  res.writeHead(200, {'Content-Type': 'text/event-stream', 'Connection': 'keep-alive',
                      'X-Seen-Connection': req.headers.connection});
  res.flushHeaders();
  res.write('data: first\n\n');
  setImmediate(() => res.end('data: second\n\n'));
});
server.listen(0, '127.0.0.1', async () => {
  try {
    for (const policy of ['close', 'keep-alive']) {
      await new Promise((resolve, reject) => {
        const req = http.get({host: '127.0.0.1', port: server.address().port,
          path: '/_events', agent: false, headers: {Connection: policy, Accept: 'text/event-stream'}}, res => {
          let body = '';
          res.on('data', data => body += data);
          res.on('error', reject);
          res.on('end', () => {
            try {
              assert.equal(res.headers['x-seen-connection'], policy);
              assert.equal(body, 'data: first\n\ndata: second\n\n');
              console.log(process.version, policy, 'two SSE frames received');
              resolve();
            } catch (error) { reject(error); }
          });
        });
        req.setTimeout(2000, () => req.destroy(new Error('reference timeout')));
        req.on('error', reject);
      });
    }
  } catch (error) { console.error(error); process.exitCode = 1; }
  finally { server.closeAllConnections(); server.close(); }
});
JS
```

## 最终本地验收

2026-11-07：SC-G1–G4 在 Linux x86_64 / GCC 13.3.0 / Debug /
warnings-as-errors 范围完成。新增专项覆盖唯一头/默认值、敏感服务端的负对照、
分片 chunked SSE 增量回调与结束后主动关闭、取消/idle/shutdown/大小上限/截断退出、
普通 Connection 头大小写变体拒绝、每跳重定向、HTTP 正向代理，以及三方言 provider
流式/非流式字段。OpenSSL 与 Mbed TLS 的直接 TLS/CONNECT 请求均验证启用后的头。

最终命令（宽回归的目标选择脚本与实际构建相同；修正新测试后还增量重建该目标）：

```bash
cmake --preset debug
python3 - <<'PY'
import json, subprocess
suite = json.loads(subprocess.check_output(
    ['ctest', '--test-dir', 'build/debug', '--show-only=json-v1'], text=True))
targets = [t['name'] for t in suite['tests'] if t['name'].startswith('mira_m3_')]
targets += [
    'mira_unit_test', 'mira_contract_test', 'mira_agent_harness_test', 'mira_android_adapter',
    'mira_minimal_consumer', 'mira_stateful_consumer', 'mira_workflow_runtime_host_consumer',
    'mira_context_assembly_host_consumer', 'mira_recovery_planner_host_consumer',
    'mira_runtime_scheduling_host_consumer', 'mira_model_price_host_consumer',
    'mira_working_context_host_consumer', 'mira_verified_fact_host_consumer',
    'mira_journaled_action_consumer']
subprocess.run(['cmake', '--build', 'build/debug', '--target', *targets, '-j', '4'], check=True)
PY
cmake --build build/debug --target mira_m3_connection_policy_test -j 4
ctest --test-dir build/debug -R '^(mira_m3_.*|mira_.*consumer.*|mira_unit_test|mira_contract_test|mira_agent_harness_test)$' --output-on-failure -j 2
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_BUILD_TESTS=ON -DMIRA_WARNINGS_AS_ERRORS=ON -DMIRA_ENABLE_ASAN=ON -DMIRA_ENABLE_UBSAN=ON
cmake --build build/asan --target mira_m3_connection_policy_test mira_m3_tls_test mira_m3_mbedtls_test -j 4
ctest --test-dir build/asan -R '^mira_m3_(connection_policy|tls|mbedtls)_test$' --output-on-failure -j 2
cmake --preset tsan
cmake --build build/tsan --target mira_m3_connection_policy_test -j 4
setarch x86_64 -R ctest --test-dir build/tsan -R '^mira_m3_connection_policy_test$' --output-on-failure
clang-tidy adapters/net/socket_transport.cpp src/model/model_provider.cpp tests/m3/m3_connection_policy_test.cpp -p build/debug
# 上述初查两生产文件通过，新测试入口失败记录见第二轮；入口修正后的最终复查：
clang-tidy tests/m3/m3_connection_policy_test.cpp -p build/debug
cmake --build build/debug --target format-check docs-check architecture-check platform-boundary-check
git diff --check
```

结果：受影响的 41 个构建目标成功；Debug 宽回归 **41/41**（39.91 秒，含 installed
consumer），ASAN+UBSAN **3/3**（1.80 秒），TSAN **1/1**（0.43 秒）；两生产文件
与最终新测试的 clang-tidy 检查通过。四项静态门禁和 diff 检查通过，架构存量 baseline
仍为 16 项，未刷新。初始 shutdown 假设及测试异常边界的失败均保留在前述记录。

公开头文件、provider 选项注释、API 配置示例、设计 §9.4、兼容性矩阵、DEC-091 与
总计划同步。无新 Executor 生命周期或第三方源码修改。此处只关闭本地离线交付范围；
PR CI 的额外平台证据随后在 PR 记录，真实 NapCat/Express 与 Android 真机运行仍需
按风险小节的负责人和条件补跑。
